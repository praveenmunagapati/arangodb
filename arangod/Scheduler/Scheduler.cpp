////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
/// @author Achim Brandt
////////////////////////////////////////////////////////////////////////////////

#include "Scheduler.h"

#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>

#include <thread>

#include "Basics/MutexLocker.h"
#include "Basics/StringUtils.h"
#include "Basics/Thread.h"
#include "GeneralServer/RestHandler.h"
#include "Logger/Logger.h"
#include "Random/RandomGenerator.h"
#include "Rest/GeneralResponse.h"
#include "Scheduler/Acceptor.h"
#include "Scheduler/JobGuard.h"
#include "Scheduler/Task.h"
#include "Statistics/RequestStatistics.h"

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

namespace {
constexpr double MIN_SECONDS = 30.0;
}

// -----------------------------------------------------------------------------
// --SECTION--                                            SchedulerManagerThread
// -----------------------------------------------------------------------------

namespace {
class SchedulerManagerThread final : public Thread {
 public:
  SchedulerManagerThread(Scheduler* scheduler, asio_ns::io_context* service)
      : Thread("SchedulerManager", true),
        _scheduler(scheduler),
        _service(service) {}

  ~SchedulerManagerThread() { shutdown(); }

 public:
  void run() override {
    while (!_scheduler->isStopping()) {
      try {
        _service->run_one();
      } catch (...) {
        LOG_TOPIC(ERR, Logger::THREADS)
            << "manager loop caught an error, restarting";
      }
    }
  }

 private:
  Scheduler* _scheduler;
  asio_ns::io_context* _service;
};
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   SchedulerThread
// -----------------------------------------------------------------------------

namespace {
class SchedulerThread : public Thread {
 public:
  SchedulerThread(Scheduler* scheduler, asio_ns::io_context* service)
      : Thread("Scheduler", true), _scheduler(scheduler), _service(service) {}

  ~SchedulerThread() { shutdown(); }

 public:
  void run() override {
    constexpr size_t EVERY_LOOP = size_t(MIN_SECONDS);

    // when we enter this method,
    // _nrRunning has already been increased for this thread
    LOG_TOPIC(DEBUG, Logger::THREADS) << "started thread ("
                                      << _scheduler->infoStatus() << ")";

    // some random delay value to avoid all initial threads checking for
    // their deletion at the very same time
    double const randomWait = static_cast<double>(RandomGenerator::interval(
        int64_t(0), static_cast<int64_t>(MIN_SECONDS * 0.5)));

    double start = TRI_microtime() + randomWait;
    size_t counter = 0;
    bool doDecrement = true;

    while (!_scheduler->isStopping()) {
      try {
        _service->run_one();
      } catch (std::exception const& ex) {
        LOG_TOPIC(ERR, Logger::THREADS) << "scheduler loop caught exception: "
                                        << ex.what();
      } catch (...) {
        LOG_TOPIC(ERR, Logger::THREADS)
            << "scheduler loop caught unknown exception";
      }

      try {
        _scheduler->drain();
      } catch (...) {
      }

      if (++counter > EVERY_LOOP) {
        counter = 0;

        double const now = TRI_microtime();

        if (now - start > MIN_SECONDS) {
          // test if we should stop this thread
          // if this returns true, nrRunning will have been
          // decremented by one already
          if (_scheduler->stopThreadIfTooMany(now)) {
            // nrRunning was decremented already. now exit thread
            // main loop
            doDecrement = false;
            break;
          }

          // use new start time
          start = now;
        }
      }
    }

    LOG_TOPIC(DEBUG, Logger::THREADS) << "stopped (" << _scheduler->infoStatus()
                                      << ")";

    if (doDecrement) {
      // only decrement here if this wasn't already done above
      _scheduler->stopThread();
    }
  }

 private:
  Scheduler* _scheduler;
  asio_ns::io_context* _service;
};
}

// -----------------------------------------------------------------------------
// --SECTION--                                                         Scheduler
// -----------------------------------------------------------------------------

Scheduler::Scheduler(uint64_t nrMinimum, uint64_t /*nrDesired*/,
                     uint64_t nrMaximum, uint64_t maxQueueSize)
    : _maxQueueSize(maxQueueSize),
      _maxFifoSize{16 * 4096, 4096},
      _fifo1(_maxFifoSize[0]),
      _fifo2(_maxFifoSize[1]),
      _fifos{&_fifo1, &_fifo2},
      _nrMinimum(nrMinimum),
      _nrMaximum(nrMaximum),
      _counters(0),
      _nrQueued(0),
      _lastAllBusyStamp(0.0) {
  _fifoSize[0] = 0;
  _fifoSize[1] = 0;

  // setup signal handlers
  initializeSignalHandlers();
}

Scheduler::~Scheduler() {
  stopRebalancer();

  _managerGuard.reset();
  _managerService.reset();

  _serviceGuard.reset();
  _ioContext.reset();
}

void Scheduler::post(std::function<void()> callback) {
  ++_nrQueued;

  try {
    // capture without self, ioContext will not live longer than scheduler
    _ioContext.get()->post([this, callback]() {
      JobGuard guard(this);
      guard.work();

      --_nrQueued;

      callback();
    });
  } catch (...) {
    --_nrQueued;
    throw;
  }
}

void Scheduler::post(asio_ns::io_context::strand& strand,
                     std::function<void()> callback) {
  ++_nrQueued;

  try {
    // capture without self, ioContext will not live longer than scheduler
    strand.post([this, callback]() {
      --_nrQueued;

      JobGuard guard(this);
      guard.work();

      callback();
    });
  } catch (...) {
    --_nrQueued;
    throw;
  }
}

bool Scheduler::queue(size_t fifo, std::function<void()> callback) {
  bool ok = true;

  if (fifo == 1) {
    if (0 < _fifoSize[0]) {
      ok = pushToFifo(fifo, callback);
    } else if (canPostDirectly()) {
      post(callback);
    } else {
      ok = pushToFifo(fifo, callback);
    }
  } else if (fifo == 2) {
    if (0 < _fifoSize[0]) {
      ok = pushToFifo(fifo, callback);
    } else if (0 < _fifoSize[1]) {
      ok = pushToFifo(fifo, callback);
    } else if (canPostDirectly()) {
      post(callback);
    } else {
      pushToFifo(fifo, callback);
    }
  } else {
    TRI_ASSERT(1 <= fifo && fifo <= 2);
  }

  return ok;
}

void Scheduler::drain() {
  while (canPostDirectly()) {
    bool found = popFifo(1);

    if (!found) {
      found = popFifo(2);

      if (!found) {
        break;
      }
    }
  }
}

void Scheduler::addQueueStatistics(velocypack::Builder& b) const {
  auto counters = getCounters();

  b.add("scheduler-threads",
        VPackValue(static_cast<int32_t>(numRunning(counters))));
  b.add("in-progress", VPackValue(static_cast<int32_t>(numWorking(counters))));
  b.add("blocked", VPackValue(static_cast<int32_t>(numBlocked(counters))));
  b.add("queue-size", VPackValue(static_cast<int32_t>(numQueued())));
  b.add("max-queue-size", VPackValue(static_cast<int32_t>(_maxQueueSize)));
  b.add("fifo1-size", VPackValue(static_cast<int32_t>(_fifoSize[0])));
  b.add("max-fifo1-size", VPackValue(static_cast<int32_t>(_maxFifoSize[0])));
  b.add("fifo2-size", VPackValue(static_cast<int32_t>(_fifoSize[1])));
  b.add("max-fifo2-size", VPackValue(static_cast<int32_t>(_maxFifoSize[1])));
}

Scheduler::QueueStatistics Scheduler::queueStatistics() const {
  auto counters = getCounters();

  return QueueStatistics{numRunning(counters), numWorking(counters),
                         numBlocked(counters), numQueued()};
}

bool Scheduler::canPostDirectly() const noexcept {
  auto counters = getCounters();
  auto nrWorking = numWorking(counters);
  auto nrQueued = numQueued();

  return nrWorking + nrQueued <= _maxQueueSize;
}

bool Scheduler::pushToFifo(size_t fifo, std::function<void()> callback) {
  size_t p = fifo - 1;
  TRI_ASSERT(0 < fifo && p <= 1);

  std::unique_ptr<FifoJob> job(new FifoJob(callback));

  try {
    if (0 < _maxFifoSize[p] && (int64_t)_maxFifoSize[p] <= _fifoSize[p]) {
      return false;
    }

    if (!_fifos[p]->push(job.get())) {
      return false;
    }

    job.release();
    ++_fifoSize[p];

    // then check, otherwise we might miss to wake up a thread
    auto counters = getCounters();
    auto nrWorking = numRunning(counters);
    auto nrQueued = numQueued();

    if (0 == nrWorking + nrQueued) {
      post([] { /*wakeup call for scheduler thread*/ });
    }
  } catch (...) {
    return false;
  }

  return true;
}

bool Scheduler::popFifo(size_t fifo) {
  int64_t p = fifo - 1;
  TRI_ASSERT(0 <= p && p <= 1);

  FifoJob* job = nullptr;
  bool ok = _fifos[p]->pop(job) && job != nullptr;

  if (ok) {
    post(job->_callback);
    delete job;

    --_fifoSize[p];
  }

  return ok;
}

bool Scheduler::start() {
  // start the I/O
  startIoService();

  TRI_ASSERT(0 < _nrMinimum);
  TRI_ASSERT(_nrMinimum <= _nrMaximum);

  for (uint64_t i = 0; i < _nrMinimum; ++i) {
    {
      MUTEX_LOCKER(locker, _threadCreateLock);
      incRunning();
    }
    try {
      startNewThread();
    } catch (...) {
      MUTEX_LOCKER(locker, _threadCreateLock);
      decRunning();
      throw;
    }
  }

  startManagerThread();
  startRebalancer();

  LOG_TOPIC(TRACE, arangodb::Logger::FIXME)
      << "all scheduler threads are up and running";

  return true;
}

void Scheduler::startIoService() {
  _ioContext.reset(new asio_ns::io_context());
  _serviceGuard.reset(new asio_ns::io_context::work(*_ioContext));

  _managerService.reset(new asio_ns::io_context());
  _managerGuard.reset(new asio_ns::io_context::work(*_managerService));
}

void Scheduler::startRebalancer() {
  std::chrono::milliseconds interval(100);
  _threadManager.reset(new asio_ns::steady_timer(*_managerService));

  _threadHandler = [this, interval](const asio_ns::error_code& error) {
    if (error || isStopping()) {
      return;
    }

    try {
      rebalanceThreads();
    } catch (...) {
      // continue if this fails.
      // we can try rebalancing again in the next round
    }

    if (_threadManager != nullptr) {
      _threadManager->expires_from_now(interval);
      _threadManager->async_wait(_threadHandler);
    }
  };

  _threadManager->expires_from_now(interval);
  _threadManager->async_wait(_threadHandler);
}

void Scheduler::stopRebalancer() noexcept {
  if (_threadManager != nullptr) {
    try {
      _threadManager->cancel();
    } catch (...) {
    }
  }
}

void Scheduler::startManagerThread() {
  auto thread = new SchedulerManagerThread(this, _managerService.get());
  if (!thread->start()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FAILED,
                                   "unable to start rebalancer thread");
  }
}

void Scheduler::startNewThread() {
  auto thread = new SchedulerThread(this, _ioContext.get());
  if (!thread->start()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FAILED,
                                   "unable to start scheduler thread");
  }
}

void Scheduler::stopThread() {
  MUTEX_LOCKER(locker, _threadCreateLock);
  decRunning();
}

// check if the current thread should be stopped
// returns true if yes, otherwise false. when the function returns
// true, it has already decremented the nrRunning counter!
bool Scheduler::stopThreadIfTooMany(double now) {
  // make sure no extra threads are created while we check the timestamp
  // and while we modify nrRunning

  uint64_t const queueCap = std::max(uint64_t(1), uint64_t(_nrMaximum / 4));
  uint64_t const nrQueued = std::min(_nrQueued.load(), queueCap);

  MUTEX_LOCKER(locker, _threadCreateLock);

  // fetch all counters in one atomic operation
  uint64_t counters = _counters.load();
  uint64_t const nrRunning = numRunning(counters);
  uint64_t const nrBlocked = numBlocked(counters);
  uint64_t const nrWorking = numWorking(counters);

  if (nrRunning <= _nrMinimum + nrBlocked) {
    // don't stop a thread if we already reached the minimum
    // number of threads
    _lastAllBusyStamp = now;
    return false;
  }

  if (nrRunning <= nrWorking + nrQueued) {
    return false;
  }

  if (_lastAllBusyStamp + 1.25 * MIN_SECONDS >= now) {
    // last time all threads were busy is less than x seconds ago
    return false;
  }

  // set the all busy stamp. this avoids that we shut down all threads
  // at the same time
  if (_lastAllBusyStamp < now - MIN_SECONDS / 2.0) {
    _lastAllBusyStamp = now - MIN_SECONDS / 2.0;
  }

  // decrement nrRunning by one already in here while holding the lock
  decRunning();
  return true;
}

std::string Scheduler::infoStatus() {
  uint64_t const counters = _counters.load();

  return "scheduler threads " + std::to_string(numRunning(counters)) +
         " in-progress " + std::to_string(numWorking(counters)) + " queued " +
         std::to_string(_nrQueued) + " blocked " +
         std::to_string(numBlocked(counters)) + " fifo1 " +
         std::to_string(_fifoSize[0]) + " fifo2 " +
         std::to_string(_fifoSize[1]);
}

void Scheduler::rebalanceThreads() {
  static uint64_t count = 0;

  ++count;

  if (count % 50 == 0) {
    LOG_TOPIC(DEBUG, Logger::THREADS) << "rebalancing threads: "
                                      << infoStatus();
  } else if (count % 5 == 0) {
    LOG_TOPIC(TRACE, Logger::THREADS) << "rebalancing threads: "
                                      << infoStatus();
  }

  uint64_t const queueCap = std::max(uint64_t(1), uint64_t(_nrMaximum / 4));

  while (true) {
    {
      double const now = TRI_microtime();

      uint64_t const nrQueued = std::min(_nrQueued.load(), queueCap);

      MUTEX_LOCKER(locker, _threadCreateLock);

      uint64_t const counters = _counters.load();
      uint64_t const nrRunning = numRunning(counters);
      uint64_t const nrWorking = numWorking(counters);
      uint64_t const nrBlocked = numBlocked(counters);

      if (nrRunning >=
          std::max(_nrMinimum, nrWorking + nrBlocked + nrQueued + 1)) {
        // all threads are working, and none are blocked. so there is no
        // need to start a new thread now
        if (nrWorking == nrRunning) {
          // still note that all threads are maxed out
          _lastAllBusyStamp = now;
        }
        break;
      }

      if (nrRunning >= _nrMaximum + nrBlocked) {
        // reached the maximum now
        break;
      }

      if (isStopping(counters)) {
        // do not start any new threads in case we are already shutting down
        break;
      }

      // LOG_TOPIC(ERR, Logger::THREADS) << "starting new thread. " <<
      // this->infoStatus();

      // all threads are maxed out
      _lastAllBusyStamp = now;

      // increase nrRunning by one here already, while holding the lock
      incRunning();
    }

    // create thread and sleep without holding the mutex
    try {
      // actually start the new thread
      startNewThread();
    } catch (...) {
      // cannot create new thread or start new thread
      // if this happens, we have to rollback the increase of nrRunning again
      {
        MUTEX_LOCKER(locker, _threadCreateLock);
        decRunning();
      }
      // add an extra sleep so the system has a chance to recover and provide
      // the needed resources
      std::this_thread::sleep_for(std::chrono::microseconds(20000));
    }

    std::this_thread::sleep_for(std::chrono::microseconds(5000));
  }
}

void Scheduler::beginShutdown() {
  if (isStopping()) {
    return;
  }

  stopRebalancer();
  _threadManager.reset();

  _managerGuard.reset();
  _managerService->stop();

  _serviceGuard.reset();
  _ioContext->stop();

  // set the flag AFTER stopping the threads
  setStopping();
}

void Scheduler::shutdown() {
  while (true) {
    uint64_t const counters = _counters.load();

    if (numRunning(counters) == 0 && numWorking(counters) == 0) {
      break;
    }

    std::this_thread::yield();
    // we can be quite generous here with waiting...
    // as we are in the shutdown already, we do not care if we need to wait for
    // a
    // bit longer
    std::this_thread::sleep_for(std::chrono::microseconds(20000));
  }

  _managerService.reset();
  _ioContext.reset();
}

void Scheduler::initializeSignalHandlers() {
#ifdef _WIN32
// Windows does not support POSIX signal handling
#else
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  sigfillset(&action.sa_mask);

  // ignore broken pipes
  action.sa_handler = SIG_IGN;

  int res = sigaction(SIGPIPE, &action, nullptr);

  if (res < 0) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME)
        << "cannot initialize signal handlers for pipe";
  }
#endif
}

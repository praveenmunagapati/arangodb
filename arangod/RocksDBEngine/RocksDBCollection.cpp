////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBCollection.h"
#include "Aql/PlanCache.h"
#include "Basics/ReadLocker.h"
#include "Basics/Result.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Cache/CacheManagerFeature.h"
#include "Cache/Common.h"
#include "Cache/Manager.h"
#include "Cache/TransactionalCache.h"
#include "Cluster/ClusterMethods.h"
#include "Indexes/Index.h"
#include "Indexes/IndexIterator.h"
#include "RestServer/DatabaseFeature.h"
#include "RocksDBEngine/RocksDBPrimaryIndex.h"
#include "RocksDBEngine/RocksDBCommon.h"
#include "RocksDBEngine/RocksDBComparator.h"
#include "RocksDBEngine/RocksDBEngine.h"
#include "RocksDBEngine/RocksDBIterators.h"
#include "RocksDBEngine/RocksDBKey.h"
#include "RocksDBEngine/RocksDBLogValue.h"
#include "RocksDBEngine/RocksDBMethods.h"
#include "RocksDBEngine/RocksDBPrimaryIndex.h"
#include "RocksDBEngine/RocksDBSettingsManager.h"
#include "RocksDBEngine/RocksDBTransactionCollection.h"
#include "RocksDBEngine/RocksDBTransactionState.h"
#include "RocksDBEngine/RocksDBValue.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Helpers.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/Events.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LocalDocumentId.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"
#include "VocBase/voc-types.h"

#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/utilities/write_batch_with_index.h>
#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;

// helper class that optionally disables indexing inside the
// RocksDB transaction if possible, and that will turn indexing
// back on later in its dtor 
// this is just a performance optimization for small transactions
struct IndexingDisabler {
  IndexingDisabler(RocksDBMethods* mthd, bool disableIndexing) 
      : mthd(mthd), disableIndexing(disableIndexing) {
    if (disableIndexing) {
      disableIndexing = mthd->DisableIndexing();
    }
  }

  ~IndexingDisabler() {
    if (disableIndexing) {
      mthd->EnableIndexing();
    }
  }

  RocksDBMethods* mthd;
  bool disableIndexing;
};

RocksDBCollection::RocksDBCollection(
    LogicalCollection& collection,
    arangodb::velocypack::Slice const& info
)
    : PhysicalCollection(collection, info),
      _objectId(basics::VelocyPackHelper::stringUInt64(info, "objectId")),
      _numberDocuments(0),
      _revisionId(0),
      _primaryIndex(nullptr),
      _cache(nullptr),
      _cachePresent(false),
      _cacheEnabled(!collection.system() &&
                    basics::VelocyPackHelper::readBooleanValue(
                        info, "cacheEnabled", false)) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  VPackSlice s = info.get("isVolatile");
  if (s.isBoolean() && s.getBoolean()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "volatile collections are unsupported in the RocksDB engine");
  }
  
  rocksutils::globalRocksEngine()->addCollectionMapping(
    _objectId, _logicalCollection.vocbase().id(), _logicalCollection.id()
  );

  if (_cacheEnabled) {
    createCache();
  }
}

RocksDBCollection::RocksDBCollection(
    LogicalCollection& collection,
    PhysicalCollection const* physical
)
    : PhysicalCollection(collection, VPackSlice::emptyObjectSlice()),
      _objectId(static_cast<RocksDBCollection const*>(physical)->_objectId),
      _numberDocuments(0),
      _revisionId(0),
      _primaryIndex(nullptr),
      _cache(nullptr),
      _cachePresent(false),
      _cacheEnabled(
          static_cast<RocksDBCollection const*>(physical)->_cacheEnabled) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  rocksutils::globalRocksEngine()->addCollectionMapping(
    _objectId, _logicalCollection.vocbase().id(), _logicalCollection.id()
  );

  if (_cacheEnabled) {
    createCache();
  }
}

RocksDBCollection::~RocksDBCollection() {
  if (useCache()) {
    try {
      destroyCache();
    } catch (...) {
    }
  }
}

std::string const& RocksDBCollection::path() const {
  return StaticStrings::Empty;  // we do not have any path
}

void RocksDBCollection::setPath(std::string const&) {
  // we do not have any path
}

Result RocksDBCollection::updateProperties(VPackSlice const& slice,
                                           bool doSync) {
  auto isSys = _logicalCollection.system();

  _cacheEnabled = !isSys && basics::VelocyPackHelper::readBooleanValue(
                                slice, "cacheEnabled", _cacheEnabled);
  primaryIndex()->setCacheEnabled(_cacheEnabled);

  if (_cacheEnabled) {
    createCache();
    primaryIndex()->createCache();
  } else {
    // will do nothing if cache is not present
    destroyCache();
    primaryIndex()->destroyCache();
    TRI_ASSERT(_cache.get() == nullptr);
  }

  // nothing else to do
  return TRI_ERROR_NO_ERROR;
}

arangodb::Result RocksDBCollection::persistProperties() {
  // only code path calling this causes these properties to be
  // already written in RocksDBEngine::changeCollection()
  return Result();
}

PhysicalCollection* RocksDBCollection::clone(LogicalCollection& logical) const {
  return new RocksDBCollection(logical, this);
}

/// @brief export properties
void RocksDBCollection::getPropertiesVPack(velocypack::Builder& result) const {
  TRI_ASSERT(result.isOpenObject());
  result.add("objectId", VPackValue(std::to_string(_objectId)));
  result.add("cacheEnabled", VPackValue(_cacheEnabled));
  TRI_ASSERT(result.isOpenObject());
}

/// @brief closes an open collection
int RocksDBCollection::close() {
  READ_LOCKER(guard, _indexesLock);
  for (auto it : _indexes) {
    it->unload();
  }
  return TRI_ERROR_NO_ERROR;
}

void RocksDBCollection::load() {
  if (_cacheEnabled) {
    createCache();
    if (_cachePresent) {
      uint64_t numDocs = numberDocuments();
      if (numDocs > 0) {
        _cache->sizeHint(static_cast<uint64_t>(0.3 * numDocs));
      }
    }
  }
  READ_LOCKER(guard, _indexesLock);
  for (auto it : _indexes) {
    it->load();
  }
}

void RocksDBCollection::unload() {
  if (useCache()) {
    destroyCache();
    TRI_ASSERT(!_cachePresent);
  }
  READ_LOCKER(guard, _indexesLock);
  for (auto it : _indexes) {
    it->unload();
  }
}

TRI_voc_rid_t RocksDBCollection::revision() const { return _revisionId; }

TRI_voc_rid_t RocksDBCollection::revision(transaction::Methods* trx) const {
  auto state = RocksDBTransactionState::toState(trx);
  auto trxCollection = static_cast<RocksDBTransactionCollection*>(
    state->findCollection(_logicalCollection.id())
  );

  TRI_ASSERT(trxCollection != nullptr);

  return trxCollection->revision();
}

uint64_t RocksDBCollection::numberDocuments() const { return _numberDocuments; }

uint64_t RocksDBCollection::numberDocuments(transaction::Methods* trx) const {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  auto state = RocksDBTransactionState::toState(trx);
  auto trxCollection = static_cast<RocksDBTransactionCollection*>(
    state->findCollection(_logicalCollection.id())
  );

  TRI_ASSERT(trxCollection != nullptr);

  return trxCollection->numberDocuments();
}

/// @brief report extra memory used by indexes etc.
size_t RocksDBCollection::memory() const { return 0; }

void RocksDBCollection::open(bool ignoreErrors) {
  TRI_ASSERT(_objectId != 0);

  // set the initial number of documents
  RocksDBEngine* engine =
      static_cast<RocksDBEngine*>(EngineSelectorFeature::ENGINE);
  auto counterValue = engine->settingsManager()->loadCounter(this->objectId());
  _numberDocuments = counterValue.added() - counterValue.removed();
  _revisionId = counterValue.revisionId();
}

void RocksDBCollection::prepareIndexes(
    arangodb::velocypack::Slice indexesSlice) {
  WRITE_LOCKER(guard, _indexesLock);
  TRI_ASSERT(indexesSlice.isArray());

  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  std::vector<std::shared_ptr<Index>> indexes;

  if (indexesSlice.length() == 0 && _indexes.empty()) {
    engine->indexFactory().fillSystemIndexes(_logicalCollection, indexes);
  } else {
    engine->indexFactory().prepareIndexes(
      _logicalCollection, indexesSlice, indexes
    );
  }

  for (std::shared_ptr<Index>& idx : indexes) {
    addIndex(std::move(idx));
  }

  if (_indexes[0]->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX ||
      (TRI_COL_TYPE_EDGE == _logicalCollection.type() &&
       (_indexes[1]->type() != Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX ||
        _indexes[2]->type() != Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX))) {
         std::string msg = "got invalid indexes for collection '"
           + _logicalCollection.name() + "'";
         LOG_TOPIC(ERR, arangodb::Logger::FIXME) << msg;

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
      for (auto it : _indexes) {
        LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "- " << it.get();
      }
#endif

      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, msg);
    }

  TRI_ASSERT(!_indexes.empty());
}

static std::shared_ptr<Index> findIndex(
    velocypack::Slice const& info,
    std::vector<std::shared_ptr<Index>> const& indexes) {
  TRI_ASSERT(info.isObject());

  auto value = info.get(arangodb::StaticStrings::IndexType); // extract type

  if (!value.isString()) {
    // Compatibility with old v8-vocindex.
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "invalid index type definition");
  }

  std::string tmp = value.copyString();
  arangodb::Index::IndexType const type = arangodb::Index::type(tmp.c_str());

  for (auto const& idx : indexes) {
    if (idx->type() == type) {
      // Only check relevant indexes
      if (idx->matchesDefinition(info)) {
        // We found an index for this definition.
        return idx;
      }
    }
  }
  return nullptr;
}

/// @brief Find index by definition
std::shared_ptr<Index> RocksDBCollection::lookupIndex(
    velocypack::Slice const& info) const {
  READ_LOCKER(guard, _indexesLock);
  return findIndex(info, _indexes);
}

std::shared_ptr<Index> RocksDBCollection::createIndex(
    transaction::Methods* trx, arangodb::velocypack::Slice const& info,
    bool& created) {
  // prevent concurrent dropping
  bool isLocked =
    trx->isLocked(&_logicalCollection, AccessMode::Type::EXCLUSIVE);
  CONDITIONAL_WRITE_LOCKER(guard, _exclusiveLock, !isLocked);
  std::shared_ptr<Index> idx;

  {
    WRITE_LOCKER(guard, _indexesLock);

    idx = findIndex(info, _indexes);

    if (idx) {
      created = false;

      // We already have this index.
      return idx;
    }
  }

  StorageEngine* engine = EngineSelectorFeature::ENGINE;

  // We are sure that we do not have an index of this type.
  // We also hold the lock.
  // Create it

  idx = engine->indexFactory().prepareIndexFromSlice(
    info, true, _logicalCollection, false
  );
  TRI_ASSERT(idx != nullptr);

  int res = saveIndex(trx, idx);

  if (res != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION(res);
  }

#if USE_PLAN_CACHE
  arangodb::aql::PlanCache::instance()->invalidate(
      _logicalCollection->vocbase());
#endif
  // Until here no harm is done if something fails. The shared_ptr will
  // clean up, if left before
  {
    WRITE_LOCKER(guard, _indexesLock);
    addIndex(idx);
  }
  auto builder = _logicalCollection.toVelocyPackIgnore(
      {"path", "statusString"}, true, /*forPersistence*/ true);
  VPackBuilder indexInfo;

  idx->toVelocyPack(indexInfo, false, true);
  res = static_cast<RocksDBEngine*>(engine)->writeCreateCollectionMarker(
    _logicalCollection.vocbase().id(),
    _logicalCollection.id(),
    builder.slice(),
    RocksDBLogValue::IndexCreate(
      _logicalCollection.vocbase().id(),
      _logicalCollection.id(),
      indexInfo.slice()
    )
  );

  if (res != TRI_ERROR_NO_ERROR) {
    // We could not persist the index creation. Better abort
    // Remove the Index in the local list again.
    size_t i = 0;
    WRITE_LOCKER(guard, _indexesLock);
    for (auto index : _indexes) {
      if (index == idx) {
        _indexes.erase(_indexes.begin() + i);
        break;
      }
      ++i;
    }
    THROW_ARANGO_EXCEPTION(res);
  }
  created = true;
  return idx;
}

/// @brief Restores an index from VelocyPack.
int RocksDBCollection::restoreIndex(transaction::Methods* trx,
                                    velocypack::Slice const& info,
                                    std::shared_ptr<Index>& idx) {
  // The coordinator can never get into this state!
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  idx.reset();  // Clear it to make sure.

  if (!info.isObject()) {
    return TRI_ERROR_INTERNAL;
  }

  // We create a new Index object to make sure that the index
  // is not handed out except for a successful case.
  std::shared_ptr<Index> newIdx;

  try {
    StorageEngine* engine = EngineSelectorFeature::ENGINE;

    newIdx = engine->indexFactory().prepareIndexFromSlice(
      info, false, _logicalCollection, false
    );
  } catch (arangodb::basics::Exception const& e) {
    // Something with index creation went wrong.
    // Just report.
    return e.code();
  }
  if (!newIdx) {
    return TRI_ERROR_ARANGO_INDEX_NOT_FOUND;
  }
  
  TRI_ASSERT(newIdx != nullptr);
  auto const id = newIdx->id();

  TRI_UpdateTickServer(id);

  for (auto& it : _indexes) {
    if (it->id() == id) {
      // index already exists
      idx = it;
      return TRI_ERROR_NO_ERROR;
    }
  }

  TRI_ASSERT(newIdx.get()->type() !=
             Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);

  Result res = fillIndexes(trx, newIdx);

  if (!res.ok()) {
    return res.errorNumber();
  }

  addIndex(newIdx);
  {
    auto builder = _logicalCollection.toVelocyPackIgnore(
        {"path", "statusString"}, true, /*forPersistence*/ true);
    VPackBuilder indexInfo;

    newIdx->toVelocyPack(indexInfo, false, true);

    RocksDBEngine* engine =
        static_cast<RocksDBEngine*>(EngineSelectorFeature::ENGINE);
    TRI_ASSERT(engine != nullptr);
    int res = engine->writeCreateCollectionMarker(
      _logicalCollection.vocbase().id(),
      _logicalCollection.id(),
      builder.slice(),
      RocksDBLogValue::IndexCreate(
        _logicalCollection.vocbase().id(),
        _logicalCollection.id(),
        indexInfo.slice()
      )
    );

    if (res != TRI_ERROR_NO_ERROR) {
      // We could not persist the index creation. Better abort
      // Remove the Index in the local list again.
      size_t i = 0;
      WRITE_LOCKER(guard, _indexesLock);
      for (auto index : _indexes) {
        if (index == newIdx) {
          _indexes.erase(_indexes.begin() + i);
          break;
        }
        ++i;
      }
      return res;
    }
  }

  idx = newIdx;
  // We need to write the IndexMarker

  return TRI_ERROR_NO_ERROR;
}

/// @brief Drop an index with the given iid.
bool RocksDBCollection::dropIndex(TRI_idx_iid_t iid) {
  // usually always called when _exclusiveLock is held
  if (iid == 0) {
    // invalid index id or primary index
    return true;
  }

  size_t i = 0;
  WRITE_LOCKER(guard, _indexesLock);
  for (std::shared_ptr<Index> index : _indexes) {
    RocksDBIndex* cindex = static_cast<RocksDBIndex*>(index.get());
    TRI_ASSERT(cindex != nullptr);

    if (iid == cindex->id()) {
      int rv = cindex->drop();

      if (rv == TRI_ERROR_NO_ERROR) {
        // trigger compaction before deleting the object
        cindex->cleanup();

        _indexes.erase(_indexes.begin() + i);
        events::DropIndex("", std::to_string(iid), TRI_ERROR_NO_ERROR);
        // toVelocyPackIgnore will take a read lock and we don't need the
        // lock anymore, this branch always returns
        guard.unlock();

        auto engine = static_cast<RocksDBEngine*>(EngineSelectorFeature::ENGINE);
        engine->removeIndexMapping(cindex->objectId());

        auto builder = _logicalCollection.toVelocyPackIgnore(
            {"path", "statusString"}, true, true);

        // log this event in the WAL and in the collection meta-data
        int res = engine->writeCreateCollectionMarker(
          _logicalCollection.vocbase().id(),
          _logicalCollection.id(),
          builder.slice(),
          RocksDBLogValue::IndexDrop(
            _logicalCollection.vocbase().id(), _logicalCollection.id(), iid
          )
        );

        return res == TRI_ERROR_NO_ERROR;
      }

      break;
    }
    ++i;
  }

  // We tried to remove an index that does not exist
  events::DropIndex("", std::to_string(iid), TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
  return false;
}

std::unique_ptr<IndexIterator> RocksDBCollection::getAllIterator(transaction::Methods* trx) const {
  return std::unique_ptr<IndexIterator>(
    new RocksDBAllIndexIterator(&_logicalCollection, trx, primaryIndex())
  );
}

std::unique_ptr<IndexIterator> RocksDBCollection::getAnyIterator(
    transaction::Methods* trx) const {
  return std::unique_ptr<IndexIterator>(
    new RocksDBAnyIndexIterator(&_logicalCollection, trx, primaryIndex())
  );
}

std::unique_ptr<IndexIterator> RocksDBCollection::getSortedAllIterator(
    transaction::Methods* trx) const {
  return std::unique_ptr<RocksDBSortedAllIterator>(
    new RocksDBSortedAllIterator(&_logicalCollection, trx, primaryIndex())
  );
}

void RocksDBCollection::invokeOnAllElements(
    transaction::Methods* trx,
    std::function<bool(LocalDocumentId const&)> callback) {
  std::unique_ptr<IndexIterator> cursor(this->getAllIterator(trx));
  bool cnt = true;
  auto cb = [&](LocalDocumentId token) {
    if (cnt) {
      cnt = callback(token);
    }
  };

  while (cursor->next(cb, 1000) && cnt) {
  }
}

////////////////////////////////////
// -- SECTION DML Operations --
///////////////////////////////////

void RocksDBCollection::truncate(transaction::Methods* trx,
                                 OperationOptions& options) {
  TRI_ASSERT(_objectId != 0);
  auto state = RocksDBTransactionState::toState(trx);
  RocksDBMethods* mthd = state->rocksdbMethods();
  // delete documents
  RocksDBKeyBounds documentBounds =
      RocksDBKeyBounds::CollectionDocuments(this->objectId());
  rocksdb::Comparator const* cmp =
      RocksDBColumnFamily::documents()->GetComparator();
  rocksdb::ReadOptions ro = mthd->readOptions();
  rocksdb::Slice const end = documentBounds.end();
  ro.iterate_upper_bound = &end;

  // avoid OOM error for truncate by committing earlier
  uint64_t const prvICC = state->options().intermediateCommitCount;
  state->options().intermediateCommitCount = std::min<uint64_t>(prvICC, 10000);

  std::unique_ptr<rocksdb::Iterator> iter =
      mthd->NewIterator(ro, documentBounds.columnFamily());
  iter->Seek(documentBounds.start());

  uint64_t found = 0;
  while (iter->Valid() && cmp->Compare(iter->key(), end) < 0) {
    ++found;
    TRI_ASSERT(_objectId == RocksDBKey::objectId(iter->key()));
    VPackSlice doc = VPackSlice(iter->value().data());
    TRI_ASSERT(doc.isObject());

    // To print the WAL we need key and RID
    VPackSlice key;
    TRI_voc_rid_t rid = 0;
    transaction::helpers::extractKeyAndRevFromDocument(doc, key, rid);
    TRI_ASSERT(key.isString());
    TRI_ASSERT(rid != 0);

    state->prepareOperation(
      _logicalCollection.id(),
      rid, // actual revision ID!!
      TRI_VOC_DOCUMENT_OPERATION_REMOVE
    );

    LocalDocumentId const docId = RocksDBKey::documentId(iter->key());
    auto res = removeDocument(trx, docId, doc, options);

    if (res.fail()) {
      // Failed to remove document in truncate. Throw
      THROW_ARANGO_EXCEPTION(res);
    }

    res = state->addOperation(
      _logicalCollection.id(), docId.id(), TRI_VOC_DOCUMENT_OPERATION_REMOVE
    );

    // transaction size limit reached
    if (res.fail()) {
      // This should never happen...
      THROW_ARANGO_EXCEPTION(res);
    }

    trackWaitForSync(trx, options);
    iter->Next();
  }

  // reset to previous value after truncate is finished
  state->options().intermediateCommitCount = prvICC;

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (state->numCommits() == 0) {
    // check if documents have been deleted
    if (mthd->countInBounds(documentBounds, true)) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "deletion check in collection truncate "
                                     "failed - not all documents have been "
                                     "deleted");
    }
  }
#endif

  TRI_IF_FAILURE("FailAfterAllCommits") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }
  TRI_IF_FAILURE("SegfaultAfterAllCommits") {
    TRI_SegfaultDebugging("SegfaultAfterAllCommits");
  }

  if (found > 64 * 1024) {
    // also compact the ranges in order to speed up all further accesses
    // to the collection
    compact();
  }
}

LocalDocumentId RocksDBCollection::lookupKey(transaction::Methods* trx,
                                             VPackSlice const& key) const {
  TRI_ASSERT(key.isString());
  return primaryIndex()->lookupKey(trx, StringRef(key));
}

Result RocksDBCollection::read(transaction::Methods* trx,
                               arangodb::StringRef const& key,
                               ManagedDocumentResult& result, bool) {
  LocalDocumentId const documentId = primaryIndex()->lookupKey(trx, key);
  if (documentId.isSet()) {
    return lookupDocumentVPack(documentId, trx, result, true);
  }
  // not found
  return Result(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND);
}

// read using a token!
bool RocksDBCollection::readDocument(transaction::Methods* trx,
                                     LocalDocumentId const& documentId,
                                     ManagedDocumentResult& result) const {
  if (documentId.isSet()) {
    auto res = lookupDocumentVPack(documentId, trx, result, true);
    return res.ok();
  }
  return false;
}

// read using a token!
bool RocksDBCollection::readDocumentWithCallback(
    transaction::Methods* trx, LocalDocumentId const& documentId,
    IndexIterator::DocumentCallback const& cb) const {
  if (documentId.isSet()) {
    auto res = lookupDocumentVPack(documentId, trx, cb, true);
    return res.ok();
  }
  return false;
}

Result RocksDBCollection::insert(arangodb::transaction::Methods* trx,
                                 arangodb::velocypack::Slice const slice,
                                 arangodb::ManagedDocumentResult& mdr,
                                 OperationOptions& options,
                                 TRI_voc_tick_t& resultMarkerTick,
                                 bool /*lock*/, TRI_voc_rid_t& revisionId) {
  // store the tick that was used for writing the document
  // note that we don't need it for this engine
  resultMarkerTick = 0;

  LocalDocumentId const documentId = LocalDocumentId::create();
  VPackSlice fromSlice;
  VPackSlice toSlice;
  auto isEdgeCollection = (TRI_COL_TYPE_EDGE == _logicalCollection.type());
  transaction::BuilderLeaser builder(trx);
  Result res(newObjectForInsert(trx, slice, isEdgeCollection,
                                *builder.get(), options.isRestore, revisionId));

  if (res.fail()) {
    return res;
  }

  VPackSlice newSlice = builder->slice();

  auto state = RocksDBTransactionState::toState(trx);
  auto mthds = RocksDBTransactionState::toMethods(trx);
  RocksDBSavePoint guard(mthds, trx->isSingleOperationTransaction());

  state->prepareOperation(
    _logicalCollection.id(), revisionId, TRI_VOC_DOCUMENT_OPERATION_INSERT
  );

  // disable indexing in this transaction if we are allowed to
  IndexingDisabler disabler(mthds, trx->isSingleOperationTransaction());

  res = insertDocument(trx, documentId, newSlice, options);

  if (res.ok()) {
    trackWaitForSync(trx, options);
    if (options.silent) {
      mdr.reset();
    } else { 
      mdr.setManaged(newSlice.begin(), documentId);
      TRI_ASSERT(!mdr.empty());
    }

    auto result = state->addOperation(
      _logicalCollection.id(), revisionId, TRI_VOC_DOCUMENT_OPERATION_INSERT
    );

    // transaction size limit reached -- fail
    if (result.fail()) {
      THROW_ARANGO_EXCEPTION(result);
    }

    guard.commit();
  }

  return res;
}

Result RocksDBCollection::update(arangodb::transaction::Methods* trx,
                                 arangodb::velocypack::Slice const newSlice,
                                 arangodb::ManagedDocumentResult& mdr,
                                 OperationOptions& options,
                                 TRI_voc_tick_t& resultMarkerTick,
                                 bool /*lock*/, TRI_voc_rid_t& prevRev,
                                 ManagedDocumentResult& previous,
                                 arangodb::velocypack::Slice const key) {
  resultMarkerTick = 0;

  LocalDocumentId const documentId = LocalDocumentId::create();
  auto isEdgeCollection = (TRI_COL_TYPE_EDGE == _logicalCollection.type());
  Result res = this->read(trx, key, previous, /*lock*/false);

  if (res.fail()) {
    return res;
  }

  TRI_ASSERT(!previous.empty());

  LocalDocumentId const oldDocumentId = previous.localDocumentId();
  VPackSlice oldDoc(previous.vpack());
  TRI_voc_rid_t const oldRevisionId =
      transaction::helpers::extractRevFromDocument(oldDoc);

  prevRev = oldRevisionId;

  // Check old revision:
  if (!options.ignoreRevs) {
    TRI_voc_rid_t expectedRev = 0;

    if (newSlice.isObject()) {
      expectedRev = TRI_ExtractRevisionId(newSlice);
    }

    int result = checkRevision(trx, expectedRev, prevRev);

    if (result != TRI_ERROR_NO_ERROR) {
      return Result(result);
    }
  }

  if (newSlice.length() <= 1) {
    // shortcut. no need to do anything
    previous.clone(mdr);

    TRI_ASSERT(!mdr.empty());

    trackWaitForSync(trx, options);
    return Result();
  }

  // merge old and new values
  TRI_voc_rid_t revisionId;
  transaction::BuilderLeaser builder(trx);
  res = mergeObjectsForUpdate(trx, oldDoc, newSlice, isEdgeCollection,
                              options.mergeObjects, options.keepNull, *builder.get(),
                              options.isRestore, revisionId);

  if (res.fail()) {
    return res;
  }

  if (_isDBServer) {
    // Need to check that no sharding keys have changed:
    if (arangodb::shardKeysChanged(
          _logicalCollection,
          oldDoc,
          builder->slice(),
          false
       )) {
      return Result(TRI_ERROR_CLUSTER_MUST_NOT_CHANGE_SHARDING_ATTRIBUTES);
    }
  }

  VPackSlice const newDoc(builder->slice());

  auto state = RocksDBTransactionState::toState(trx);
  RocksDBSavePoint guard(RocksDBTransactionState::toMethods(trx),
                         trx->isSingleOperationTransaction());

  // add possible log statement under guard
  state->prepareOperation(
    _logicalCollection.id(), revisionId, TRI_VOC_DOCUMENT_OPERATION_UPDATE
  );
  res = updateDocument(trx, oldDocumentId, oldDoc, documentId, newDoc, options);

  if (res.ok()) {
    trackWaitForSync(trx, options);

    if (options.silent) {
      mdr.reset();
    } else {
      mdr.setManaged(newDoc.begin(), documentId);
      TRI_ASSERT(!mdr.empty());
    }

    auto result = state->addOperation(
      _logicalCollection.id(), revisionId, TRI_VOC_DOCUMENT_OPERATION_UPDATE
    );

    // transaction size limit reached -- fail hard
    if (result.fail()) {
      THROW_ARANGO_EXCEPTION(result);
    }

    guard.commit();
  }

  return res;
}

Result RocksDBCollection::replace(transaction::Methods* trx,
                                  arangodb::velocypack::Slice const newSlice,
                                  ManagedDocumentResult& mdr,
                                  OperationOptions& options,
                                  TRI_voc_tick_t& resultMarkerTick,
                                  bool /*lock*/, TRI_voc_rid_t& prevRev,
                                  ManagedDocumentResult& previous) {
  resultMarkerTick = 0;

  LocalDocumentId const documentId = LocalDocumentId::create();
  auto isEdgeCollection = (TRI_COL_TYPE_EDGE == _logicalCollection.type());

  // get the previous revision
  VPackSlice key = newSlice.get(StaticStrings::KeyString);

  if (key.isNone()) {
    return Result(TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD);
  }

  // get the previous revision
  Result res = this->read(trx, key, previous, /*lock*/false);

  if (res.fail()) {
    return res;
  }

  TRI_ASSERT(!previous.empty());
  LocalDocumentId const oldDocumentId = previous.localDocumentId();

  VPackSlice oldDoc(previous.vpack());
  TRI_voc_rid_t oldRevisionId =
      transaction::helpers::extractRevFromDocument(oldDoc);
  prevRev = oldRevisionId;

  // Check old revision:
  if (!options.ignoreRevs) {
    TRI_voc_rid_t expectedRev = 0;
    if (newSlice.isObject()) {
      expectedRev = TRI_ExtractRevisionId(newSlice);
    }
    int res = checkRevision(trx, expectedRev, prevRev);

    if (res != TRI_ERROR_NO_ERROR) {
      return Result(res);
    }
  }

  // merge old and new values
  TRI_voc_rid_t revisionId;
  transaction::BuilderLeaser builder(trx);
  res = newObjectForReplace(trx, oldDoc, newSlice, 
                            isEdgeCollection, *builder.get(), options.isRestore,
                            revisionId);

  if (res.fail()) {
    return res;
  }

  if (_isDBServer) {
    // Need to check that no sharding keys have changed:
    if (arangodb::shardKeysChanged(
          _logicalCollection,
          oldDoc,
          builder->slice(),
          false
       )) {
      return Result(TRI_ERROR_CLUSTER_MUST_NOT_CHANGE_SHARDING_ATTRIBUTES);
    }
  }

  VPackSlice const newDoc(builder->slice());

  auto state = RocksDBTransactionState::toState(trx);
  RocksDBSavePoint guard(RocksDBTransactionState::toMethods(trx),
                         trx->isSingleOperationTransaction());

  // add possible log statement under guard
  state->prepareOperation(
    _logicalCollection.id(), revisionId, TRI_VOC_DOCUMENT_OPERATION_REPLACE
  );

  Result opResult = updateDocument(trx, oldDocumentId, oldDoc, documentId, newDoc, options);

  if (opResult.ok()) {
    trackWaitForSync(trx, options);

    if (options.silent) {
      mdr.reset();
    } else {
      mdr.setManaged(newDoc.begin(), documentId);
      TRI_ASSERT(!mdr.empty());
    }

    auto result = state->addOperation(
      _logicalCollection.id(), revisionId, TRI_VOC_DOCUMENT_OPERATION_REPLACE
    );

    // transaction size limit reached -- fail
    if (result.fail()) {
      THROW_ARANGO_EXCEPTION(result);
    }

    guard.commit();
  }

  return opResult;
}

Result RocksDBCollection::remove(arangodb::transaction::Methods* trx,
                                 arangodb::velocypack::Slice const slice,
                                 arangodb::ManagedDocumentResult& previous,
                                 OperationOptions& options,
                                 TRI_voc_tick_t& resultMarkerTick,
                                 bool /*lock*/, TRI_voc_rid_t& prevRev,
                                 TRI_voc_rid_t& revisionId) {
  // store the tick that was used for writing the document
  // note that we don't need it for this engine
  resultMarkerTick = 0;
  prevRev = 0;
  revisionId = newRevisionId();

  VPackSlice key;
  if (slice.isString()) {
    key = slice;
  } else {
    key = slice.get(StaticStrings::KeyString);
  }
  TRI_ASSERT(!key.isNone());

  // get the previous revision
  Result res = this->read(trx, key, previous, /*lock*/false);
  if (res.fail()) {
    return res;
  }

  TRI_ASSERT(!previous.empty());
  LocalDocumentId const oldDocumentId = previous.localDocumentId();

  VPackSlice oldDoc(previous.vpack());
  TRI_voc_rid_t oldRevisionId =
      arangodb::transaction::helpers::extractRevFromDocument(oldDoc);
  prevRev = oldRevisionId;

  // Check old revision:
  if (!options.ignoreRevs && slice.isObject()) {
    TRI_voc_rid_t expectedRevisionId = TRI_ExtractRevisionId(slice);
    int res = checkRevision(trx, expectedRevisionId, oldRevisionId);

    if (res != TRI_ERROR_NO_ERROR) {
      return Result(res);
    }
  }

  auto state = RocksDBTransactionState::toState(trx);
  RocksDBSavePoint guard(RocksDBTransactionState::toMethods(trx),
                         trx->isSingleOperationTransaction());

  // add possible log statement under guard
  state->prepareOperation(
    _logicalCollection.id(), oldRevisionId, TRI_VOC_DOCUMENT_OPERATION_REMOVE
  );
  res = removeDocument(trx, oldDocumentId, oldDoc, options);

  if (res.ok()) {
    trackWaitForSync(trx, options);

    // report key size
    res = state->addOperation(
      _logicalCollection.id(), revisionId, TRI_VOC_DOCUMENT_OPERATION_REMOVE
    );

    // transaction size limit reached -- fail
    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }

    guard.commit();
  }

  return res;
}

void RocksDBCollection::deferDropCollection(
    std::function<bool(LogicalCollection&)> const& /*callback*/
) {
  // nothing to do here
}

/// @brief return engine-specific figures
void RocksDBCollection::figuresSpecific(
    std::shared_ptr<arangodb::velocypack::Builder>& builder) {
  rocksdb::TransactionDB* db = rocksutils::globalRocksDB();
  RocksDBKeyBounds bounds = RocksDBKeyBounds::CollectionDocuments(_objectId);
  rocksdb::Range r(bounds.start(), bounds.end());

  uint64_t out = 0;
  db->GetApproximateSizes(
      RocksDBColumnFamily::documents(), &r, 1, &out,
      static_cast<uint8_t>(
          rocksdb::DB::SizeApproximationFlags::INCLUDE_MEMTABLES |
          rocksdb::DB::SizeApproximationFlags::INCLUDE_FILES));

  builder->add("documentsSize", VPackValue(out));
}

void RocksDBCollection::addIndex(std::shared_ptr<arangodb::Index> idx) {
  // LOCKED from the outside
  // primary index must be added at position 0
  TRI_ASSERT(ServerState::instance()->isRunningInCluster() ||
             idx->type() != arangodb::Index::TRI_IDX_TYPE_PRIMARY_INDEX ||
             _indexes.empty());

  auto const id = idx->id();
  for (auto const& it : _indexes) {
    if (it->id() == id) {
      // already have this particular index. do not add it again
      return;
    }
  }

  TRI_UpdateTickServer(static_cast<TRI_voc_tick_t>(id));
  _indexes.emplace_back(idx);
  if (idx->type() == Index::TRI_IDX_TYPE_PRIMARY_INDEX) {
    TRI_ASSERT(idx->id() == 0);
    _primaryIndex = static_cast<RocksDBPrimaryIndex*>(idx.get());
  }
}

int RocksDBCollection::saveIndex(transaction::Methods* trx,
                                 std::shared_ptr<arangodb::Index> idx) {
  // LOCKED from the outside
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  // we cannot persist primary or edge indexes
  TRI_ASSERT(idx->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);
  TRI_ASSERT(idx->type() != Index::IndexType::TRI_IDX_TYPE_EDGE_INDEX);

  Result res = fillIndexes(trx, idx);
  if (!res.ok()) {
    return res.errorNumber();
  }

  return TRI_ERROR_NO_ERROR;
}

/// non-transactional: fill index with existing documents
/// from this collection
arangodb::Result RocksDBCollection::fillIndexes(
    transaction::Methods* trx, std::shared_ptr<arangodb::Index> added) {
  // FIXME: assert for an exclusive lock on this collection
  TRI_ASSERT(trx->state()->collection(
    _logicalCollection.id(), AccessMode::Type::EXCLUSIVE
  ));

  RocksDBIndex* ridx = static_cast<RocksDBIndex*>(added.get());
  auto state = RocksDBTransactionState::toState(trx);
  std::unique_ptr<IndexIterator> it(new RocksDBAllIndexIterator(
    &_logicalCollection, trx, primaryIndex()
  ));

  // fillindex can be non transactional, we just need to clean up
  rocksdb::DB* db = rocksutils::globalRocksDB()->GetBaseDB();
  TRI_ASSERT(db != nullptr);

  uint64_t numDocsWritten = 0;
  // write batch will be reset every x documents
  rocksdb::WriteBatchWithIndex batch(ridx->columnFamily()->GetComparator(),
                                     32 * 1024 * 1024);
  RocksDBBatchedMethods batched(state, &batch);

  arangodb::Result res;
  auto cb = [&](LocalDocumentId const& documentId, VPackSlice slice) {
    if (res.ok()) {
      res = ridx->insertInternal(trx, &batched, documentId, slice,
                                 Index::OperationMode::normal);
      if (res.ok()) {
        numDocsWritten++;
      }
    }
  };

  rocksdb::WriteOptions writeOpts;
  bool hasMore = true;

  while (hasMore && res.ok()) {
    hasMore = it->nextDocument(cb, 250);

    if (TRI_VOC_COL_STATUS_DELETED == _logicalCollection.status()
        || _logicalCollection.deleted()) {
      res = TRI_ERROR_INTERNAL;
    }

    if (res.ok()) {
      rocksdb::Status s = db->Write(writeOpts, batch.GetWriteBatch());

      if (!s.ok()) {
        res = rocksutils::convertStatus(s, rocksutils::StatusHint::index);
        break;
      }
    }
    batch.Clear();
  }

  // we will need to remove index elements created before an error
  // occured, this needs to happen since we are non transactional
  if (!res.ok()) {
    it->reset();
    batch.Clear();

    ManagedDocumentResult mmdr;

    arangodb::Result res2;  // do not overwrite original error
    auto removeCb = [&](LocalDocumentId token) {
      if (res2.ok() && numDocsWritten > 0 &&
          this->readDocument(trx, token, mmdr)) {
        // we need to remove already inserted documents up to numDocsWritten
        res2 = ridx->removeInternal(trx, &batched, mmdr.localDocumentId(),
                                    VPackSlice(mmdr.vpack()),
                                    Index::OperationMode::rollback);
        if (res2.ok()) {
          numDocsWritten--;
        }
      }
    };

    hasMore = true;
    while (hasMore && numDocsWritten > 0) {
      hasMore = it->next(removeCb, 500);
    }
    rocksdb::WriteOptions writeOpts;
    db->Write(writeOpts, batch.GetWriteBatch());
  }

  return res;
}

Result RocksDBCollection::insertDocument(
    arangodb::transaction::Methods* trx, LocalDocumentId const& documentId,
    VPackSlice const& doc, OperationOptions& options) const {
  // Coordinator doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(trx->state()->isRunning());

  RocksDBKeyLeaser key(trx);
  key->constructDocument(_objectId, documentId);

  blackListKey(key->string().data(), static_cast<uint32_t>(key->string().size()));

  RocksDBMethods* mthd = RocksDBTransactionState::toMethods(trx);
  Result res = mthd->Put(RocksDBColumnFamily::documents(), key.ref(),
                         rocksdb::Slice(reinterpret_cast<char const*>(doc.begin()),
                                        static_cast<size_t>(doc.byteSize())));
  if (!res.ok()) {
    return res;
  }

  READ_LOCKER(guard, _indexesLock);
  for (std::shared_ptr<Index> const& idx : _indexes) {
    RocksDBIndex* rIdx = static_cast<RocksDBIndex*>(idx.get());
    Result tmpres = rIdx->insertInternal(trx, mthd, documentId, doc,
                                         options.indexOperationMode);
    if (!tmpres.ok()) {
      if (tmpres.is(TRI_ERROR_OUT_OF_MEMORY)) {
        // in case of OOM return immediately
        return tmpres;
      } else if (tmpres.is(TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED) ||
                 res.ok()) {
        // "prefer" unique constraint violated over other errors
        res.reset(tmpres);
      }
    }
  }

  return res;
}

Result RocksDBCollection::removeDocument(
    arangodb::transaction::Methods* trx, LocalDocumentId const& documentId,
    VPackSlice const& doc, OperationOptions& options) const {
  // Coordinator doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(_objectId != 0);

  RocksDBKeyLeaser key(trx);
  key->constructDocument(_objectId, documentId);

  blackListKey(key->string().data(), static_cast<uint32_t>(key->string().size()));

  RocksDBMethods* mthd = RocksDBTransactionState::toMethods(trx);

  // disable indexing in this transaction if we are allowed to
  IndexingDisabler disabler(mthd, trx->isSingleOperationTransaction());

  Result res = mthd->Delete(RocksDBColumnFamily::documents(), key.ref());
  if (!res.ok()) {
    return res;
  }

  /*LOG_TOPIC(ERR, Logger::FIXME)
      << "Delete rev: " << revisionId << " trx: " << trx->state()->id()
      << " seq: " << mthd->readOptions().snapshot->GetSequenceNumber()
      << " objectID " << _objectId << " name: " << _logicalCollection->name();*/

  Result resInner;
  READ_LOCKER(guard, _indexesLock);
  for (std::shared_ptr<Index> const& idx : _indexes) {
    Result tmpres = idx->remove(trx, documentId, doc, options.indexOperationMode);
    if (!tmpres.ok()) {
      if (tmpres.is(TRI_ERROR_OUT_OF_MEMORY)) {
        // in case of OOM return immediately
        return tmpres;
      }
      // for other errors, set result
      res.reset(tmpres);
    }
  }

  return res;
}

Result RocksDBCollection::updateDocument(
    transaction::Methods* trx, LocalDocumentId const& oldDocumentId,
    VPackSlice const& oldDoc, LocalDocumentId const& newDocumentId,
    VPackSlice const& newDoc, OperationOptions& options) const {
  // keysize in return value is set by insertDocument

  // Coordinator doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(_objectId != 0);

  RocksDBMethods* mthd = RocksDBTransactionState::toMethods(trx);

  // We NEED to do the PUT first, otherwise WAL tailing breaks
  RocksDBKeyLeaser newKey(trx);
  newKey->constructDocument(_objectId, newDocumentId);
  // TODO: given that this should have a unique revision ID, do
  // we really need to blacklist the new key?
  blackListKey(newKey->string().data(),
               static_cast<uint32_t>(newKey->string().size()));
  rocksdb::Slice docSlice(reinterpret_cast<char const*>(newDoc.begin()),
                          static_cast<size_t>(newDoc.byteSize()));

  // disable indexing in this transaction if we are allowed to
  IndexingDisabler disabler(mthd, trx->isSingleOperationTransaction());

  Result res = mthd->Put(RocksDBColumnFamily::documents(), newKey.ref(), docSlice);
  if (!res.ok()) {
    return res;
  }

  RocksDBKeyLeaser oldKey(trx);
  oldKey->constructDocument(_objectId, oldDocumentId);
  blackListKey(oldKey->string().data(),
               static_cast<uint32_t>(oldKey->string().size()));

  res = mthd->Delete(RocksDBColumnFamily::documents(), oldKey.ref());
  if (!res.ok()) {
    return res;
  }

  READ_LOCKER(guard, _indexesLock);
  for (std::shared_ptr<Index> const& idx : _indexes) {
    RocksDBIndex* rIdx = static_cast<RocksDBIndex*>(idx.get());
    Result tmpres = rIdx->updateInternal(trx, mthd, oldDocumentId, oldDoc, newDocumentId,
                                         newDoc, options.indexOperationMode);
    if (!tmpres.ok()) {
      if (tmpres.is(TRI_ERROR_OUT_OF_MEMORY)) {
        // in case of OOM return immediately
        return tmpres;
      }
      res.reset(tmpres);
    }
  }

  return res;
}

arangodb::Result RocksDBCollection::lookupDocumentVPack(
    LocalDocumentId const& documentId, transaction::Methods* trx,
    arangodb::ManagedDocumentResult& mdr, bool withCache) const {
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(_objectId != 0);

  RocksDBKeyLeaser key(trx);
  key->constructDocument(_objectId, documentId);

  bool lockTimeout = false;
  if (withCache && useCache()) {
    TRI_ASSERT(_cache != nullptr);
    // check cache first for fast path
    auto f = _cache->find(key->string().data(),
                          static_cast<uint32_t>(key->string().size()));
    if (f.found()) {
      std::string* value = mdr.prepareStringUsage();
      value->append(reinterpret_cast<char const*>(f.value()->value()),
                    f.value()->valueSize());
      mdr.setManagedAfterStringUsage(documentId);
      return TRI_ERROR_NO_ERROR;
    } else if (f.result().errorNumber() == TRI_ERROR_LOCK_TIMEOUT) {
      // assuming someone is currently holding a write lock, which
      // is why we cannot access the TransactionalBucket.
      lockTimeout = true;  // we skip the insert in this case
    }
  }

  RocksDBMethods* mthd = RocksDBTransactionState::toMethods(trx);
  std::string* value = mdr.prepareStringUsage();
  Result res = mthd->Get(RocksDBColumnFamily::documents(), key.ref(), value);

  if (res.ok()) {
    if (withCache && useCache() && !lockTimeout) {
      TRI_ASSERT(_cache != nullptr);
      // write entry back to cache
      auto entry = cache::CachedValue::construct(
          key->string().data(), static_cast<uint32_t>(key->string().size()),
          value->data(), static_cast<uint64_t>(value->size()));

      if (entry) {
        Result status = _cache->insert(entry);

        if (status.errorNumber() == TRI_ERROR_LOCK_TIMEOUT) {
          // the writeLock uses cpu_relax internally, so we can try yield
          std::this_thread::yield();
          status = _cache->insert(entry);
        }

        if (status.fail()) {
          delete entry;
        }
      }
    }

    mdr.setManagedAfterStringUsage(documentId);
  } else {
    LOG_TOPIC(DEBUG, Logger::FIXME)
        << "NOT FOUND rev: " << documentId.id() << " trx: " << trx->state()->id()
        << " seq: " << mthd->readOptions().snapshot->GetSequenceNumber()
        << " objectID " << _objectId << " name: " << _logicalCollection.name();
    mdr.reset();
  }

  return res;
}

arangodb::Result RocksDBCollection::lookupDocumentVPack(
    LocalDocumentId const& documentId, transaction::Methods* trx,
    IndexIterator::DocumentCallback const& cb, bool withCache) const {
  TRI_ASSERT(trx->state()->isRunning());
  TRI_ASSERT(_objectId != 0);

  RocksDBKeyLeaser key(trx);
  key->constructDocument(_objectId, documentId);

  bool lockTimeout = false;
  if (withCache && useCache()) {
    TRI_ASSERT(_cache != nullptr);
    // check cache first for fast path
    auto f = _cache->find(key->string().data(),
                          static_cast<uint32_t>(key->string().size()));
    if (f.found()) {
      cb(documentId,
         VPackSlice(reinterpret_cast<char const*>(f.value()->value())));
      return TRI_ERROR_NO_ERROR;
    } else if (f.result().errorNumber() == TRI_ERROR_LOCK_TIMEOUT) {
      // assuming someone is currently holding a write lock, which
      // is why we cannot access the TransactionalBucket.
      lockTimeout = true;  // we skip the insert in this case
    }
  }

  std::string value;
  auto state = RocksDBTransactionState::toState(trx);
  RocksDBMethods* mthd = state->rocksdbMethods();
  Result res = mthd->Get(RocksDBColumnFamily::documents(), key.ref(), &value);
  TRI_ASSERT(value.data());
  if (res.ok()) {
    if (withCache && useCache() && !lockTimeout) {
      TRI_ASSERT(_cache != nullptr);
      // write entry back to cache
      auto entry = cache::CachedValue::construct(
          key->string().data(), static_cast<uint32_t>(key->string().size()),
          value.data(), static_cast<uint64_t>(value.size()));
      if (entry) {
        auto status = _cache->insert(entry);
        if (status.errorNumber() == TRI_ERROR_LOCK_TIMEOUT) {
          // the writeLock uses cpu_relax internally, so we can try yield
          std::this_thread::yield();
          status = _cache->insert(entry);
        }
        if (status.fail()) {
          delete entry;
        }
      }
    }

    cb(documentId, VPackSlice(value.data()));
  } else {
    LOG_TOPIC(DEBUG, Logger::FIXME)
        << "NOT FOUND rev: " << documentId.id() << " trx: " << trx->state()->id()
        << " seq: " << mthd->readOptions().snapshot->GetSequenceNumber()
        << " objectID " << _objectId << " name: " << _logicalCollection.name();
  }
  return res;
}

void RocksDBCollection::setRevision(TRI_voc_rid_t revisionId) {
  _revisionId = revisionId;
}

void RocksDBCollection::adjustNumberDocuments(int64_t adjustment) {
  if (adjustment < 0) {
    _numberDocuments -= static_cast<uint64_t>(-adjustment);
  } else if (adjustment > 0) {
    _numberDocuments += static_cast<uint64_t>(adjustment);
  }
}

/// @brief write locks a collection, with a timeout
int RocksDBCollection::lockWrite(double timeout) {
  uint64_t waitTime = 0;  // indicates that time is uninitialized
  double startTime = 0.0;

  while (true) {
    TRY_WRITE_LOCKER(locker, _exclusiveLock);

    if (locker.isLocked()) {
      // keep lock and exit loop
      locker.steal();
      return TRI_ERROR_NO_ERROR;
    }

    double now = TRI_microtime();

    if (waitTime == 0) {  // initialize times
      // set end time for lock waiting
      if (timeout <= 0.0) {
        timeout = defaultLockTimeout;
      }

      startTime = now;
      waitTime = 1;
    }

    if (now > startTime + timeout) {
      LOG_TOPIC(TRACE, arangodb::Logger::FIXME)
          << "timed out after " << timeout
          << " s waiting for write-lock on collection '"
          << _logicalCollection.name() << "'";

      return TRI_ERROR_LOCK_TIMEOUT;
    }

    if (now - startTime < 0.001) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(waitTime));
      if (waitTime < 32) {
        waitTime *= 2;
      }
    }
  }
}

/// @brief write unlocks a collection
int RocksDBCollection::unlockWrite() {
  _exclusiveLock.unlockWrite();

  return TRI_ERROR_NO_ERROR;
}

/// @brief read locks a collection, with a timeout
int RocksDBCollection::lockRead(double timeout) {
  uint64_t waitTime = 0;  // indicates that time is uninitialized
  double startTime = 0.0;

  while (true) {
    TRY_READ_LOCKER(locker, _exclusiveLock);

    if (locker.isLocked()) {
      // keep lock and exit loop
      locker.steal();
      return TRI_ERROR_NO_ERROR;
    }

    double now = TRI_microtime();

    if (waitTime == 0) {  // initialize times
      // set end time for lock waiting
      if (timeout <= 0.0) {
        timeout = defaultLockTimeout;
      }

      startTime = now;
      waitTime = 1;
    }

    if (now > startTime + timeout) {
      LOG_TOPIC(TRACE, arangodb::Logger::FIXME)
          << "timed out after " << timeout
          << " s waiting for read-lock on collection '"
          << _logicalCollection.name() << "'";

      return TRI_ERROR_LOCK_TIMEOUT;
    }

    if (now - startTime < 0.001) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(waitTime));

      if (waitTime < 32) {
        waitTime *= 2;
      }
    }
  }
}

/// @brief read unlocks a collection
int RocksDBCollection::unlockRead() {
  _exclusiveLock.unlockRead();
  return TRI_ERROR_NO_ERROR;
}

// rescans the collection to update document count
uint64_t RocksDBCollection::recalculateCounts() {
  // start transaction to get a collection lock
  auto ctx =
    transaction::StandaloneContext::Create(_logicalCollection.vocbase());
  SingleCollectionTransaction trx(
    ctx, _logicalCollection, AccessMode::Type::EXCLUSIVE
  );
  auto res = trx.begin();

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  RocksDBEngine* engine = rocksutils::globalRocksEngine();
  // count documents
  auto documentBounds = RocksDBKeyBounds::CollectionDocuments(_objectId);
  _numberDocuments =
      rocksutils::countKeyRange(engine->db(), documentBounds, true);

  // update counter manager value
  res = engine->settingsManager()->setAbsoluteCounter(_objectId,
                                                      _numberDocuments);
  if (res.ok()) {
    // in case of fail the counter has never been written and hence does not
    // need correction. The value is not changed and does not need to be synced
    engine->settingsManager()->sync(true);
  }
  trx.commit();

  return _numberDocuments;
}

void RocksDBCollection::compact() {
  rocksdb::TransactionDB* db = rocksutils::globalRocksDB();
  rocksdb::CompactRangeOptions opts;
  RocksDBKeyBounds bounds = RocksDBKeyBounds::CollectionDocuments(_objectId);
  rocksdb::Slice b = bounds.start(), e = bounds.end();
  db->CompactRange(opts, bounds.columnFamily(), &b, &e);

  READ_LOCKER(guard, _indexesLock);
  for (std::shared_ptr<Index> i : _indexes) {
    RocksDBIndex* index = static_cast<RocksDBIndex*>(i.get());
    index->cleanup();
  }
}

void RocksDBCollection::estimateSize(velocypack::Builder& builder) {
  TRI_ASSERT(!builder.isOpenObject() && !builder.isOpenArray());

  rocksdb::TransactionDB* db = rocksutils::globalRocksDB();
  RocksDBKeyBounds bounds = RocksDBKeyBounds::CollectionDocuments(_objectId);
  rocksdb::Range r(bounds.start(), bounds.end());
  uint64_t out = 0, total = 0;
  db->GetApproximateSizes(
      RocksDBColumnFamily::documents(), &r, 1, &out,
      static_cast<uint8_t>(
          rocksdb::DB::SizeApproximationFlags::INCLUDE_MEMTABLES |
          rocksdb::DB::SizeApproximationFlags::INCLUDE_FILES));
  total += out;

  builder.openObject();
  builder.add("documents", VPackValue(out));
  builder.add("indexes", VPackValue(VPackValueType::Object));

  READ_LOCKER(guard, _indexesLock);
  for (std::shared_ptr<Index> i : _indexes) {
    RocksDBIndex* index = static_cast<RocksDBIndex*>(i.get());
    out = index->memory();
    builder.add(std::to_string(index->id()), VPackValue(out));
    total += out;
  }
  builder.close();
  builder.add("total", VPackValue(total));
  builder.close();
}

std::pair<arangodb::Result, rocksdb::SequenceNumber>
RocksDBCollection::serializeIndexEstimates(
    rocksdb::Transaction* rtrx, rocksdb::SequenceNumber inputSeq) const {
  auto outputSeq = inputSeq;
  std::string output;
  for (auto index : getIndexes()) {
    output.clear();
    RocksDBIndex* cindex = static_cast<RocksDBIndex*>(index.get());
    TRI_ASSERT(cindex != nullptr);
    if (cindex->needToPersistEstimate()) {
      LOG_TOPIC(TRACE, Logger::ENGINES)
        << "beginning estimate serialization for index '"
        << cindex->objectId() << "'";
      auto committedSeq = cindex->serializeEstimate(output, inputSeq);
      outputSeq = std::min(outputSeq, committedSeq);
      LOG_TOPIC(TRACE, Logger::ENGINES)
        << "serialized estimate for index '" << cindex->objectId()
        << "' valid through seq " << outputSeq;
      if (output.size() > sizeof(uint64_t)) {
        RocksDBKey key;
        key.constructIndexEstimateValue(cindex->objectId());
        rocksdb::Slice value(output);
        rocksdb::Status s =
            rtrx->Put(RocksDBColumnFamily::definitions(), key.string(), value);

        if (!s.ok()) {
          LOG_TOPIC(WARN, Logger::ENGINES) << "writing index estimates failed";
          rtrx->Rollback();
          return std::make_pair(rocksutils::convertStatus(s), outputSeq);
        }
      }
    }
  }
  return std::make_pair(Result(), outputSeq);
}

void RocksDBCollection::deserializeIndexEstimates(RocksDBSettingsManager* mgr) {
  std::vector<std::shared_ptr<Index>> toRecalculate;
  for (auto const& it : getIndexes()) {
    auto idx = static_cast<RocksDBIndex*>(it.get());
    if (!idx->deserializeEstimate(mgr)) {
      toRecalculate.push_back(it);
    }
  }
  if (!toRecalculate.empty()) {
    recalculateIndexEstimates(toRecalculate);
  }
}

void RocksDBCollection::recalculateIndexEstimates() {
  auto idxs = getIndexes();
  recalculateIndexEstimates(idxs);
}

void RocksDBCollection::recalculateIndexEstimates(
    std::vector<std::shared_ptr<Index>> const& indexes) {
  // IMPORTANT if this method is called outside of startup/recovery, we may have
  // issues with estimate integrity; please do not expose via a user-facing
  // method or endpoint unless the implementation changes

  // start transaction to get a collection lock
  auto ctx =
    transaction::StandaloneContext::Create(_logicalCollection.vocbase());
  arangodb::SingleCollectionTransaction trx(
    ctx, _logicalCollection, AccessMode::Type::EXCLUSIVE
  );
  auto res = trx.begin();

  if (res.fail()) {
    THROW_ARANGO_EXCEPTION(res);
  }

  for (auto const& it : indexes) {
    auto idx = static_cast<RocksDBIndex*>(it.get());

    TRI_ASSERT(idx != nullptr);
    idx->recalculateEstimates();
  }

  trx.commit();
}

arangodb::Result RocksDBCollection::serializeKeyGenerator(
    rocksdb::Transaction* rtrx) const {
  VPackBuilder builder;

  builder.openObject();
  _logicalCollection.keyGenerator()->toVelocyPack(builder);
  builder.close();

  RocksDBKey key;

  key.constructKeyGeneratorValue(_objectId);

  RocksDBValue value = RocksDBValue::KeyGeneratorValue(builder.slice());
  rocksdb::Status s = rtrx->Put(RocksDBColumnFamily::definitions(),
                                key.string(), value.string());

  if (!s.ok()) {
    LOG_TOPIC(WARN, Logger::ENGINES) << "writing key generator data failed";
    rtrx->Rollback();
    return rocksutils::convertStatus(s);
  }

  return Result();
}

void RocksDBCollection::deserializeKeyGenerator(RocksDBSettingsManager* mgr) {
  uint64_t value = mgr->stealKeyGenerator(_objectId);

  if (value > 0) {
    std::string k(basics::StringUtils::itoa(value));

    _logicalCollection.keyGenerator()->track(k.data(), k.size());
  }
}

void RocksDBCollection::createCache() const {
  if (!_cacheEnabled || _cachePresent || _logicalCollection.isAStub() ||
      ServerState::instance()->isCoordinator()) {
    // we leave this if we do not need the cache
    // or if cache already created
    return;
  }

  TRI_ASSERT(_cacheEnabled);
  TRI_ASSERT(_cache.get() == nullptr);
  TRI_ASSERT(CacheManagerFeature::MANAGER != nullptr);
  LOG_TOPIC(DEBUG, Logger::CACHE) << "Creating document cache";
  _cache = CacheManagerFeature::MANAGER->createCache(
      cache::CacheType::Transactional);
  _cachePresent = (_cache.get() != nullptr);
  TRI_ASSERT(_cacheEnabled);
}

void RocksDBCollection::destroyCache() const {
  if (!_cachePresent) {
    return;
  }
  TRI_ASSERT(CacheManagerFeature::MANAGER != nullptr);
  // must have a cache...
  TRI_ASSERT(_cache.get() != nullptr);
  LOG_TOPIC(DEBUG, Logger::CACHE) << "Destroying document cache";
  CacheManagerFeature::MANAGER->destroyCache(_cache);
  _cache.reset();
  _cachePresent = false;
}

// blacklist given key from transactional cache
void RocksDBCollection::blackListKey(char const* data, std::size_t len) const {
  if (useCache()) {
    TRI_ASSERT(_cache != nullptr);
    bool blacklisted = false;
    while (!blacklisted) {
      auto status = _cache->blacklist(data, static_cast<uint32_t>(len));
      if (status.ok()) {
        blacklisted = true;
      } else if (status.errorNumber() == TRI_ERROR_SHUTTING_DOWN) {
        destroyCache();
        break;
      }
    }
  }
}

void RocksDBCollection::trackWaitForSync(arangodb::transaction::Methods* trx,
                                         OperationOptions& options) {
  if (_logicalCollection.waitForSync() && !options.isRestore) {
    options.waitForSync = true;
  }

  if (options.waitForSync) {
    trx->state()->waitForSync(true);
  }
}

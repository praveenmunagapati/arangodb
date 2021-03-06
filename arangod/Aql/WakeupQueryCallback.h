////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018-2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_WAKEUP_QUERY_CALLBACK_H
#define ARANGOD_AQL_WAKEUP_QUERY_CALLBACK_H 1

#include "Basics/Common.h"
#include "Cluster/ClusterComm.h"

namespace arangodb {
namespace aql {

class ExecutionBlock;
class Query;

struct WakeupQueryCallback : public ClusterCommCallback {
  WakeupQueryCallback(ExecutionBlock* initiator, Query* query);
  ~WakeupQueryCallback() {};

  bool operator()(ClusterCommResult*) override;

  private:
    ExecutionBlock* _initiator;
    Query* _query;
};

} // aql
} // arangodb

#endif

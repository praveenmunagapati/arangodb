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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_CLUSTER_APPLICATION_CLUSTER_H
#define ARANGOD_CLUSTER_APPLICATION_CLUSTER_H 1

#include "Basics/Common.h"
#include "ApplicationServer/ApplicationFeature.h"

struct TRI_server_t;

namespace arangodb {
namespace rest {
class ApplicationDispatcher;
}

class ApplicationV8;
class HeartbeatThread;

////////////////////////////////////////////////////////////////////////////////
/// @brief sharding feature configuration
////////////////////////////////////////////////////////////////////////////////

class ApplicationCluster : public rest::ApplicationFeature {
 private:
  ApplicationCluster(ApplicationCluster const&) = delete;
  ApplicationCluster& operator=(ApplicationCluster const&) = delete;

 public:
  ApplicationCluster(TRI_server_t*, arangodb::rest::ApplicationDispatcher*,
                     arangodb::ApplicationV8*);

  ~ApplicationCluster();

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief disable the heartbeat (used for testing)
  //////////////////////////////////////////////////////////////////////////////

  void disableHeartbeat() { _disableHeartbeat = true; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the cluster is enabled
  //////////////////////////////////////////////////////////////////////////////

  inline bool enabled() const { return _enableCluster; }

 public:
  void setupOptions(std::map<std::string, basics::ProgramOptionsDescription>&) override final;

  bool prepare() override final;

  bool open() override final;

  bool start() override final;

  void close() override final;

  void stop() override final;

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief server
  //////////////////////////////////////////////////////////////////////////////

  TRI_server_t* _server;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief dispatcher
  //////////////////////////////////////////////////////////////////////////////

  arangodb::rest::ApplicationDispatcher* _dispatcher;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief v8 dispatcher
  //////////////////////////////////////////////////////////////////////////////

  arangodb::ApplicationV8* _applicationV8;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief thread for heartbeat
  //////////////////////////////////////////////////////////////////////////////

  HeartbeatThread* _heartbeat;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief heartbeat interval (in milliseconds)
  //////////////////////////////////////////////////////////////////////////////

  uint64_t _heartbeatInterval;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief was docuBlock clusterAgencyEndpoint
  ////////////////////////////////////////////////////////////////////////////////

  std::vector<std::string> _agencyEndpoints;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief was docuBlock clusterAgencyPrefix
  ////////////////////////////////////////////////////////////////////////////////

  std::string _agencyPrefix;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief was docuBlock clusterMyLocalInfo
  ////////////////////////////////////////////////////////////////////////////////

  std::string _myLocalInfo;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief was docuBlock clusterMyId
  ////////////////////////////////////////////////////////////////////////////////

  std::string _myId;
  
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief was docuBlock clusterMyRole
  ////////////////////////////////////////////////////////////////////////////////

  std::string _myRole;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief was docuBlock clusterMyAddress
  ////////////////////////////////////////////////////////////////////////////////

  std::string _myAddress;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief was docuBlock clusterUsername
  ////////////////////////////////////////////////////////////////////////////////

  std::string _username;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief was docuBlock clusterPassword
  ////////////////////////////////////////////////////////////////////////////////

  std::string _password;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief data path for the cluster
  ///
  /// @CMDOPT{\--cluster.data-path @CA{path}}
  ///
  /// The default directory where the databases for the cluster processes are
  /// stored.
  //////////////////////////////////////////////////////////////////////////////

  std::string _dataPath;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief log path for the cluster
  ///
  /// @CMDOPT{\--cluster.log-path @CA{path}}
  ///
  /// The default directory where the log files for the cluster processes are
  /// stored.
  //////////////////////////////////////////////////////////////////////////////

  std::string _logPath;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief arangod path for the cluster
  ///
  /// @CMDOPT{\--cluster.arangod-path @CA{path}}
  ///
  /// The path to arangod executable.
  //////////////////////////////////////////////////////////////////////////////

  std::string _arangodPath;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief DBserver config for the cluster
  ///
  /// @CMDOPT{\--cluster.dbserver-config @CA{path}}
  ///
  /// The configuration file for the DBserver.
  //////////////////////////////////////////////////////////////////////////////

  std::string _dbserverConfig;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief coordinator config for the cluster
  ///
  /// @CMDOPT{\--cluster.coordinator-config @CA{path}}
  ///
  /// The configuration file for the coordinator.
  //////////////////////////////////////////////////////////////////////////////

  std::string _coordinatorConfig;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the cluster feature is enabled
  //////////////////////////////////////////////////////////////////////////////

  bool _enableCluster;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief flag for turning off heartbeat (used for testing)
  //////////////////////////////////////////////////////////////////////////////

  bool _disableHeartbeat;
};
}

#endif

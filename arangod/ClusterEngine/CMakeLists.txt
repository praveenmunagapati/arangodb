# -*- mode: CMAKE; -*-



# add sources for cluster engine
set(CLUSTER_ENGINE_SOURCES
  ClusterEngine/ClusterCollection.cpp
  ClusterEngine/ClusterEngine.cpp
  ClusterEngine/ClusterIndex.cpp
  ClusterEngine/ClusterIndexFactory.cpp
  ClusterEngine/ClusterRestExportHandler.cpp
  ClusterEngine/ClusterRestHandlers.cpp
  ClusterEngine/ClusterRestReplicationHandler.cpp
  ClusterEngine/ClusterRestWalHandler.cpp
  ClusterEngine/ClusterTransactionCollection.cpp
  ClusterEngine/ClusterTransactionContextData.h
  ClusterEngine/ClusterTransactionState.cpp
  ClusterEngine/ClusterV8Functions.cpp
)
set(CLUSTER_ENGINE_SOURCES ${CLUSTER_ENGINE_SOURCES} PARENT_SCOPE)

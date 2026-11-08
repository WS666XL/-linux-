#pragma once

#include <grpcpp/grpcpp.h>

#include "monitor.grpc.pb.h"
#include "storage/mysql_storage.h"

namespace manager {

// gRPC 查询服务：最新快照 / 历史区间 / 窗口趋势，数据源 MysqlStorage
class QueryService final : public monitor::QueryService::Service {
 public:
  explicit QueryService(MysqlStorage* storage) : storage_(storage) {}

  grpc::Status QueryLatest(grpc::ServerContext* context,
                           const monitor::QueryNodeRequest* request,
                           monitor::MetricsSample* response) override;

  grpc::Status QueryHistory(grpc::ServerContext* context,
                            const monitor::QueryHistoryRequest* request,
                            monitor::QueryHistoryResponse* response) override;

  grpc::Status QueryTrend(grpc::ServerContext* context,
                          const monitor::QueryTrendRequest* request,
                          monitor::QueryTrendResponse* response) override;

  grpc::Status QueryHost(grpc::ServerContext* context,
                         const monitor::QueryNodeRequest* request,
                         monitor::HostInfo* response) override;

 private:
  MysqlStorage* storage_;
};

}  // namespace manager

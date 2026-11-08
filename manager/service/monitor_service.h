#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include "alert/alert_engine.h"
#include "monitor.grpc.pb.h"
#include "scheduler/weighted_scheduler.h"
#include "storage/mysql_storage.h"

namespace manager {

// gRPC 服务实现：
//   UploadMetrics  客户端流式接收 -> 告警检查 -> 持久化 -> 更新调度缓存
//   QueryBestNode  按场景加权打分返回节点排名
class MonitorService final : public monitor::MonitorService::Service {
 public:
  MonitorService(MysqlStorage* storage, AlertEngine* alert,
                 WeightedScheduler* scheduler)
      : storage_(storage), alert_(alert), scheduler_(scheduler) {}

  grpc::Status UploadMetrics(
      grpc::ServerContext* context,
      grpc::ServerReader<monitor::UploadRequest>* reader,
      monitor::UploadResponse* response) override;

  grpc::Status QueryBestNode(
      grpc::ServerContext* context,
      const monitor::QueryBestNodeRequest* request,
      monitor::QueryBestNodeResponse* response) override;

 private:
  MysqlStorage* storage_;
  AlertEngine* alert_;
  WeightedScheduler* scheduler_;
};

}  // namespace manager

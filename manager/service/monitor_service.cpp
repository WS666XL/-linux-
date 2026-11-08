#include "monitor_service.h"

#include "common/logger.h"

namespace manager {

grpc::Status MonitorService::UploadMetrics(
    grpc::ServerContext* context,
    grpc::ServerReader<monitor::UploadRequest>* reader,
    monitor::UploadResponse* response) {
  monitor::UploadRequest req;
  uint64_t received = 0;
  while (reader->Read(&req)) {
    const monitor::MetricsSample& s = req.sample();
    if (req.has_host()) {
      LOG_INFO("service: host node=%s hostname=%s ip=%s kernel=%s os=%s",
               s.node_id().c_str(), req.host().hostname().c_str(),
               req.host().ip().c_str(), req.host().kernel().c_str(),
               req.host().os().c_str());
      if (!storage_->upsert_host(s.node_id(), req.host())) {
        LOG_WARN("storage: persist host info of %s failed",
                 s.node_id().c_str());
      }
    }
    scheduler_->update(s);  // 先更新调度缓存，保证打分用的是最新数据
    alert_->check(s);
    if (!storage_->insert(s)) {
      LOG_WARN("storage: persist sample of %s failed", s.node_id().c_str());
    }
    ++received;
    req.Clear();
  }
  response->set_ok(true);
  response->set_received(received);
  LOG_INFO("service: stream from %s ended, received %llu samples",
           context->peer().c_str(), static_cast<unsigned long long>(received));
  return grpc::Status::OK;
}

grpc::Status MonitorService::QueryBestNode(
    grpc::ServerContext* context, const monitor::QueryBestNodeRequest* request,
    monitor::QueryBestNodeResponse* response) {
  auto ranked = scheduler_->rank(request->scenario());
  for (const auto& n : ranked) {
    auto* bn = response->add_ranked();
    bn->set_node_id(n.node_id);
    bn->set_score(n.score);
    *bn->mutable_latest() = n.latest;
  }
  return grpc::Status::OK;
}

}  // namespace manager

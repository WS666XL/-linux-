#include "service/query_service.h"

#include "common/logger.h"

namespace manager {

grpc::Status QueryService::QueryLatest(
    grpc::ServerContext* context, const monitor::QueryNodeRequest* request,
    monitor::MetricsSample* response) {
  if (request->node_id().empty())
    return grpc::Status(grpc::INVALID_ARGUMENT, "node_id is required");
  if (!storage_->query_latest(request->node_id(), response))
    return grpc::Status(grpc::NOT_FOUND, "no sample for node");
  return grpc::Status::OK;
}

grpc::Status QueryService::QueryHistory(
    grpc::ServerContext* context, const monitor::QueryHistoryRequest* request,
    monitor::QueryHistoryResponse* response) {
  if (request->node_id().empty())
    return grpc::Status(grpc::INVALID_ARGUMENT, "node_id is required");
  int64_t end = request->end_ms();
  if (end <= 0) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    end = now.count();
  }
  int64_t start = request->start_ms();
  if (start <= 0) start = end - 300 * 1000;  // 默认最近 5 分钟

  std::vector<monitor::MetricsSample> rows;
  int n = storage_->query_history(request->node_id(), start, end,
                                  request->limit(), &rows);
  if (n < 0)
    return grpc::Status(grpc::UNAVAILABLE, "storage unavailable");
  for (auto& s : rows) response->mutable_samples()->Add(std::move(s));
  return grpc::Status::OK;
}

grpc::Status QueryService::QueryTrend(
    grpc::ServerContext* context, const monitor::QueryTrendRequest* request,
    monitor::QueryTrendResponse* response) {
  if (request->node_id().empty())
    return grpc::Status(grpc::INVALID_ARGUMENT, "node_id is required");
  manager::TrendResult t;
  if (!storage_->query_trend(request->node_id(), request->window_sec(), &t))
    return grpc::Status(grpc::UNAVAILABLE, "storage unavailable");
  response->set_cpu_avg(t.cpu_avg);
  response->set_mem_avg(t.mem_avg);
  response->set_softirq_avg(t.softirq_avg);
  response->set_net_total_avg_bps(t.net_total_avg_bps);
  response->set_sample_count(t.sample_count);
  return grpc::Status::OK;
}

grpc::Status QueryService::QueryHost(
    grpc::ServerContext* context, const monitor::QueryNodeRequest* request,
    monitor::HostInfo* response) {
  if (request->node_id().empty())
    return grpc::Status(grpc::INVALID_ARGUMENT, "node_id is required");
  monitor::HostInfo info;
  if (!storage_->query_host(request->node_id(), &info))
    return grpc::Status(grpc::NOT_FOUND, "no host info for node");
  *response = info;
  return grpc::Status::OK;
}

}  // namespace manager

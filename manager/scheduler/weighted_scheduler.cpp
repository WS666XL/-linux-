#include "weighted_scheduler.h"

#include <algorithm>

namespace manager {

namespace {
constexpr double kMaxIops = 5000.0;                  // 打分满载参考值
constexpr double kMaxNetBps = 200.0 * 1024 * 1024;   // 200MB/s
}  // namespace

void WeightedScheduler::update(const monitor::MetricsSample& s) {
  std::lock_guard<std::mutex> lk(mu_);
  latest_[s.node_id()] = s;
}

WeightedScheduler::Weights WeightedScheduler::weights_for(
    const std::string& scenario) const {
  // 场景预设：CPU 密集 / IO 密集 / 网络密集，其余用默认权重
  if (scenario == "cpu_bound") return {0.7, 0.2, 0.05, 0.05};
  if (scenario == "io_bound") return {0.2, 0.1, 0.1, 0.6};
  if (scenario == "net_bound") return {0.2, 0.1, 0.6, 0.1};
  return default_w_;
}

std::vector<WeightedScheduler::ScoredNode> WeightedScheduler::rank(
    const std::string& scenario) const {
  std::lock_guard<std::mutex> lk(mu_);
  const Weights w = weights_for(scenario);
  std::vector<ScoredNode> out;
  out.reserve(latest_.size());
  for (const auto& [id, s] : latest_) {
    double cpu_cap = std::max(0.0, 100.0 - s.cpu_usage());
    double mem_cap = std::max(0.0, 100.0 - s.mem_usage());
    double disk_cap =
        100.0 * (1.0 - std::min(s.disk_iops() / kMaxIops, 1.0));
    double net_cap =
        100.0 * (1.0 - std::min((s.net_recv_bps() + s.net_send_bps()) /
                                       kMaxNetBps, 1.0));
    ScoredNode n;
    n.node_id = id;
    n.latest = s;
    n.score = w.cpu * cpu_cap + w.mem * mem_cap + w.disk * disk_cap +
              w.net * net_cap;
    out.push_back(std::move(n));
  }
  std::sort(out.begin(), out.end(),
            [](const ScoredNode& a, const ScoredNode& b) {
              return a.score > b.score;
            });
  return out;
}

}  // namespace manager

#include "alert_engine.h"

#include <cmath>

#include "common/logger.h"

namespace manager {

void AlertEngine::check(const monitor::MetricsSample& s) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = last_.find(s.node_id());
  if (it != last_.end()) {
    const auto& prev = it->second;
    if (s.cpu_usage() > opt_.cpu_max) {
      LOG_WARN("[ALERT] %s cpu_usage=%.1f%% exceeds %.1f%%", s.node_id().c_str(),
               s.cpu_usage(), opt_.cpu_max);
      if (on_alert)
        on_alert({s.node_id(), "cpu_max", s.cpu_usage()});
    }
    if (s.mem_usage() > opt_.mem_max) {
      LOG_WARN("[ALERT] %s mem_usage=%.1f%% exceeds %.1f%%", s.node_id().c_str(),
               s.mem_usage(), opt_.mem_max);
      if (on_alert)
        on_alert({s.node_id(), "mem_max", s.mem_usage()});
    }
    double jump = std::fabs(s.cpu_usage() - prev.cpu_usage());
    if (jump > opt_.cpu_jump) {
      LOG_WARN("[ALERT] %s cpu_usage jumped %.1f%% ( %.1f -> %.1f )",
               s.node_id().c_str(), jump, prev.cpu_usage(), s.cpu_usage());
      if (on_alert)
        on_alert({s.node_id(), "cpu_jump", jump});
    }
  }
  last_[s.node_id()] = s;
}

}  // namespace manager

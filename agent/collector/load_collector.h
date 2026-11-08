#pragma once

#include "icollector.h"

namespace agent {

// 系统负载采集：读 /proc/loadavg 前三个数（1/5/15 分钟平均负载）
class LoadCollector : public ICollector {
 public:
  std::string name() const override { return "load"; }
  void collect(monitor::MetricsSample* out) override;
};

}  // namespace agent

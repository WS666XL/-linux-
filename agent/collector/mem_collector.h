#pragma once

#include "icollector.h"

namespace agent {

// 内存采集：读 /proc/meminfo，用 MemTotal/MemAvailable 计算使用率
class MemCollector : public ICollector {
 public:
  std::string name() const override { return "mem"; }
  void collect(monitor::MetricsSample* out) override;
};

}  // namespace agent

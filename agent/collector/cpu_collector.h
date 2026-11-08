#pragma once

#include <cstdint>

#include "icollector.h"

namespace agent {

// CPU 采集：读 /proc/stat，用两次采样差值计算使用率、iowait、softirq 占比
class CpuCollector : public ICollector {
public:
  std::string name() const override { return "cpu"; }
  void collect(monitor::MetricsSample *out) override; // 采集 CPU 指标

private:
  struct Snapshot { // CPU 采快照
    uint64_t idle = 0, iowait = 0, softirq = 0,
             total = 0; // 空闲、IO 等待、软中断、总时间
  };
  bool read_snapshot(Snapshot *s) const; // 读取快照

  Snapshot last_;         // 上次采样快照
  bool has_last_ = false; // 是否有上次采样快照
};

} // namespace agent

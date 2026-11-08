#pragma once

#include <chrono>
#include <cstdint>

#include "icollector.h"

namespace agent {

// 磁盘 IO 采集：读 /proc/diskstats，用两次采样差值计算读写字节速率与 IOPS
// 只统计物理整盘（/sys/block/<name>/device 存在），自动排除分区与虚拟设备
class DiskCollector : public ICollector {
public:
  std::string name() const override { return "disk"; }
  void collect(monitor::MetricsSample *out) override;

private:
  struct Snap { // 磁盘 IO 采快照
    uint64_t rsect = 0, wsect = 0, ios = 0;
  };

  Snap last_;                                       // 上次采样快照
  std::chrono::steady_clock::time_point last_ts_{}; // 上次采样时间
  bool has_last_ = false; // 是否有上次采样快照
};

} // namespace agent

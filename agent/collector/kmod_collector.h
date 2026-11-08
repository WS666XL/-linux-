#pragma once

#include <cstdint>

#include "icollector.h"

namespace agent {

// 内核模块版 CPU 采集：读 /dev/cpu_stat_monitor（kcpustat，单位 ns）
// 差值算法与 /proc/stat 版一致（比值与单位无关）
class KmodCpuCollector : public ICollector {
 public:
  std::string name() const override { return "cpu-kmod"; }
  void collect(monitor::MetricsSample* out) override;

 private:
  struct Snapshot {
    uint64_t idle = 0, iowait = 0, softirq = 0, total = 0;
  };
  bool read_snapshot(Snapshot* s) const;

  Snapshot last_;
  bool has_last_ = false;
};

// 软中断速率采集：读 /dev/softirq_monitor（kstat 累计计数），差值算 次/秒
class SoftirqCollector : public ICollector {
 public:
  std::string name() const override { return "softirq-kmod"; }
  void collect(monitor::MetricsSample* out) override;

 private:
  bool read_total(uint64_t* total) const;
  uint64_t last_ = 0;
  bool has_last_ = false;
};

}  // namespace agent

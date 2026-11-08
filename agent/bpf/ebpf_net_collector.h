#pragma once

#include <chrono>
#include <cstdint>

#include "ebpf_net_loader.h"
#include "../collector/icollector.h"

namespace agent {

// 基于 eBPF 的网络采集：kprobe 统计协议栈收发字节，用户态做差值算速率
// init() 失败（非 root / 内核不支持）时，调用方应回退到 /proc/net/dev 采集
class EbpfNetCollector : public ICollector {
 public:
  bool init(const std::string& bpf_object_path);

  std::string name() const override { return "net-ebpf"; }
  void collect(monitor::MetricsSample* out) override;

 private:
  EbpfNetLoader loader_;
  uint64_t last_rx_ = 0, last_tx_ = 0;
  std::chrono::steady_clock::time_point last_ts_{};
  bool has_last_ = false;
};

}  // namespace agent

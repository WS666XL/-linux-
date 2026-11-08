#pragma once

#include <chrono>
#include <cstdint>

#include "icollector.h"

namespace agent {

// 网络采集骨架：读 /proc/net/dev 统计网卡收发字节差值
// TODO(第二阶段)：替换为 eBPF 挂载内核协议栈采集，降低对 proc 文本的解析开销
class NetCollector : public ICollector {
 public:
  std::string name() const override { return "net"; }
  void collect(monitor::MetricsSample* out) override;

 private:
  uint64_t last_recv_ = 0, last_send_ = 0;
  std::chrono::steady_clock::time_point last_ts_{};
  bool has_last_ = false;
};

}  // namespace agent

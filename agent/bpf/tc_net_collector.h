#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <bpf/libbpf.h>

#include "../collector/icollector.h"

namespace agent {

// TC(clsact) 网络采集：ingress/egress 各挂一个 SCHED_CLS 程序，按网卡统计字节
// 需要 root 与内核 qdisc 支持；初始化失败由 main 回退到 eBPF kprobe 或 /proc
class TcNetCollector : public ICollector {
public:
  ~TcNetCollector() override;
  std::string name() const override { return "net-tc"; }

  // bpf_object 为空、网卡不存在、加载/挂载失败均返回 false
  bool init(const std::string &bpf_object, const std::string &iface);

  void collect(monitor::MetricsSample *out) override;

private:
  bool attach_prog(bpf_program *prog, bpf_tc_hook *hook, uint32_t handle);
  void detach_prog(bpf_tc_hook *hook, uint32_t handle, bool attached);
  std::pair<uint64_t, uint64_t> read_bytes();

  bpf_object *obj_ = nullptr;
  int map_fd_ = -1;
  int ifindex_ = 0;
  std::string iface_;

  bpf_tc_hook ing_hook_{};
  bpf_tc_hook eg_hook_{};
  uint32_t ing_handle_ = 1;
  uint32_t eg_handle_ = 2;
  bool ing_attached_ = false;
  bool eg_attached_ = false;
  bool created_hook_ = false;

  uint64_t last_rx_ = 0, last_tx_ = 0;
  std::chrono::steady_clock::time_point last_ts_{};
  bool has_last_ = false;
};

} // namespace agent

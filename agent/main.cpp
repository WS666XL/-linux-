#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <thread>

#include "bpf/ebpf_net_collector.h"
#include "bpf/tc_net_collector.h"
#include "collector/cpu_collector.h"
#include "collector/disk_collector.h"
#include "collector/kmod_collector.h"
#include "collector/load_collector.h"
#include "collector/mem_collector.h"
#include "collector/net_collector.h"
#include "common/config.h"
#include "common/logger.h"
#include "host_info.h"
#include "reporter/grpc_reporter.h"

static std::atomic<bool> g_running = true;
static void on_signal(int) { g_running = false; }

int main(int argc, char** argv) {
  // 支持两种写法：-c <path> 或直接 <path>
  std::string conf_path = "configs/agent.conf";
  if (argc > 1) {
    if (std::string(argv[1]) == "-c" && argc > 2)
      conf_path = argv[2];
    else if (std::string(argv[1]) != "-c")
      conf_path = argv[1];
  }

  common::Config cfg;
  if (!cfg.load(conf_path)) return 1;

  const std::string node_id = getenv("NODE_ID")
                                  ? getenv("NODE_ID")
                                  : cfg.get("agent", "node_id", "node-1");
  const std::string manager_addr =
      getenv("MANAGER_ADDR") ? getenv("MANAGER_ADDR")
                             : cfg.get("agent", "manager_addr",
                                       "127.0.0.1:50051");
  const int interval_ms = cfg.get_int("agent", "interval_ms", 1000);

  agent::GrpcReporter reporter(manager_addr);
  // 主机静态标识：随每条上报流的首条消息发送，重连后自动重发
  const monitor::HostInfo host = agent::collect_host_info();
  reporter.set_host_info(host);
  LOG_INFO("host: %s ip=%s machine_id=%s kernel=%s os=%s",
           host.hostname().c_str(), host.ip().c_str(),
           host.machine_id().c_str(), host.kernel().c_str(),
           host.os().c_str());
  reporter.start();

  agent::CpuCollector cpu_proc;
  agent::MemCollector mem;
  agent::LoadCollector load;
  agent::DiskCollector disk;
  // CPU 采集优先内核模块字符设备（/dev/cpu_stat_monitor），无设备回退 /proc
  agent::KmodCpuCollector cpu_kmod;
  agent::ICollector* cpu_collector = &cpu_proc;
  if (std::ifstream("/dev/cpu_stat_monitor").good()) cpu_collector = &cpu_kmod;
  LOG_INFO("cpu collector: %s", cpu_collector->name().c_str());
  // 软中断速率：仅内核模块加载后可采
  agent::SoftirqCollector softirq;
  const bool has_softirq = std::ifstream("/dev/softirq_monitor").good();
  if (has_softirq) LOG_INFO("softirq collector: %s", softirq.name().c_str());
  // 网络采集链：TC(clsact) → eBPF kprobe → /proc/net/dev，按配置依次尝试
  agent::NetCollector net_proc;
  agent::EbpfNetCollector net_ebpf;
  agent::TcNetCollector net_tc;
  agent::ICollector* net_collector = &net_proc;
  const std::string net_mode = cfg.get("agent", "net_mode", "auto");
  if (net_mode == "auto" || net_mode == "tc") {
    const std::string tc_obj =
        cfg.get("agent", "tc_object", "build/bpf/net_tc.bpf.o");
    if (net_tc.init(tc_obj, cfg.get("agent", "tc_iface", ""))) {
      net_collector = &net_tc;
    }
  }
  if (net_collector == &net_proc &&
      (net_mode == "auto" || net_mode == "ebpf")) {
    if (net_ebpf.init(cfg.get("agent", "bpf_object", ""))) {
      net_collector = &net_ebpf;
    }
  }
  LOG_INFO("net collector: %s", net_collector->name().c_str());

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  LOG_INFO("agent %s started, report to %s every %dms", node_id.c_str(),
           manager_addr.c_str(), interval_ms);

  while (g_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    monitor::MetricsSample sample;
    sample.set_node_id(node_id);
    sample.set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    cpu_collector->collect(&sample);
    mem.collect(&sample);
    load.collect(&sample);
    disk.collect(&sample);
    if (has_softirq) softirq.collect(&sample);
    net_collector->collect(&sample);
    reporter.submit(std::move(sample));
  }

  reporter.stop();
  LOG_INFO("agent %s exited", node_id.c_str());
  return 0;
}

#include "tc_net_collector.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <bpf/bpf.h>
#include <net/if.h>

#include "common/logger.h"

namespace agent {

namespace {

// /proc/net/route 里 Flags 含 RTF_GATEWAY(0x2) 且 Destination=0 的网卡
std::string default_iface() {
  std::ifstream in("/proc/net/route");
  std::string line;
  std::getline(in, line); // 跳过表头
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::string iface, dest, gateway, flags;
    if (!(ss >> iface >> dest >> gateway >> flags)) continue;
    if (dest == "00000000" && (std::strtoul(flags.c_str(), nullptr, 16) & 0x2))
      return iface;
  }
  return "";
}

} // namespace

TcNetCollector::~TcNetCollector() {
  detach_prog(&ing_hook_, ing_handle_, ing_attached_);
  detach_prog(&eg_hook_, eg_handle_, eg_attached_);
  if (created_hook_) bpf_tc_hook_destroy(&ing_hook_);
  if (obj_) bpf_object__close(obj_);
}

bool TcNetCollector::attach_prog(bpf_program *prog, bpf_tc_hook *hook,
                                 uint32_t handle) {
  bpf_tc_opts opts{};
  opts.sz = sizeof(opts);
  opts.prog_fd = bpf_program__fd(prog);
  opts.handle = handle;
  opts.priority = 1;
  int err = bpf_tc_attach(hook, &opts);
  if (err) {
    LOG_WARN("tc: attach %s on %s failed: %s", bpf_program__name(prog),
             iface_.c_str(), strerror(-err));
    return false;
  }
  return true;
}

void TcNetCollector::detach_prog(bpf_tc_hook *hook, uint32_t handle,
                                 bool attached) {
  if (!attached) return;
  bpf_tc_opts opts{};
  opts.sz = sizeof(opts);
  opts.handle = handle;
  opts.priority = 1;
  bpf_tc_detach(hook, &opts);
}

bool TcNetCollector::init(const std::string &bpf_object,
                          const std::string &iface) {
  if (bpf_object.empty()) return false;

  iface_ = iface.empty() ? default_iface() : iface;
  if (iface_.empty()) {
    LOG_WARN("tc: no default-route interface found, set tc_iface in config");
    return false;
  }
  ifindex_ = static_cast<int>(if_nametoindex(iface_.c_str()));
  if (ifindex_ == 0) {
    LOG_WARN("tc: interface %s not found", iface_.c_str());
    return false;
  }

  obj_ = bpf_object__open_file(bpf_object.c_str(), nullptr);
  if (!obj_) {
    LOG_WARN("tc: open %s failed: %s", bpf_object.c_str(), strerror(errno));
    return false;
  }
  if (bpf_object__load(obj_)) {
    // 常见原因：非 root、内核未开启 clsact、对象与内核不兼容
    LOG_WARN("tc: load %s failed: %s (need root? clsact support?)",
             bpf_object.c_str(), strerror(errno));
    bpf_object__close(obj_);
    obj_ = nullptr;
    return false;
  }

  map_fd_ = bpf_object__find_map_fd_by_name(obj_, "tc_bytes");
  if (map_fd_ < 0) {
    LOG_ERROR("tc: map tc_bytes not found");
    bpf_object__close(obj_);
    obj_ = nullptr;
    return false;
  }

  bpf_program *rx = bpf_object__find_program_by_name(obj_, "tc_rx");
  bpf_program *tx = bpf_object__find_program_by_name(obj_, "tc_tx");
  if (!rx || !tx) {
    LOG_ERROR("tc: program tc_rx/tc_tx not found in %s", bpf_object.c_str());
    bpf_object__close(obj_);
    obj_ = nullptr;
    return false;
  }

  ing_hook_.sz = sizeof(ing_hook_);
  ing_hook_.ifindex = ifindex_;
  ing_hook_.attach_point = BPF_TC_INGRESS;
  eg_hook_.sz = sizeof(eg_hook_);
  eg_hook_.ifindex = ifindex_;
  eg_hook_.attach_point = BPF_TC_EGRESS;

  // ingress/egress 共用同一个 clsact qdisc，创建一次即可
  int err = bpf_tc_hook_create(&ing_hook_);
  if (err && err != -EEXIST) {
    LOG_WARN("tc: create clsact on %s failed: %s", iface_.c_str(),
             strerror(-err));
    bpf_object__close(obj_);
    obj_ = nullptr;
    return false;
  }
  created_hook_ = (err == 0);

  if (!attach_prog(rx, &ing_hook_, ing_handle_)) {
    if (created_hook_) bpf_tc_hook_destroy(&ing_hook_);
    bpf_object__close(obj_);
    obj_ = nullptr;
    return false;
  }
  ing_attached_ = true;
  if (!attach_prog(tx, &eg_hook_, eg_handle_)) {
    detach_prog(&ing_hook_, ing_handle_, ing_attached_);
    ing_attached_ = false;
    if (created_hook_) bpf_tc_hook_destroy(&ing_hook_);
    bpf_object__close(obj_);
    obj_ = nullptr;
    return false;
  }
  eg_attached_ = true;

  LOG_INFO("tc: net monitor attached on %s (ingress+egress)", iface_.c_str());
  return true;
}

std::pair<uint64_t, uint64_t> TcNetCollector::read_bytes() {
  std::pair<uint64_t, uint64_t> out{0, 0};
  if (map_fd_ < 0) return out;

  int cpus = libbpf_num_possible_cpus();
  if (cpus <= 0) return out;
  std::vector<uint64_t> buf(static_cast<size_t>(cpus));

  uint32_t key = 0;
  if (bpf_map_lookup_elem(map_fd_, &key, buf.data()) == 0)
    for (int i = 0; i < cpus; ++i) out.first += buf[i];
  key = 1;
  if (bpf_map_lookup_elem(map_fd_, &key, buf.data()) == 0)
    for (int i = 0; i < cpus; ++i) out.second += buf[i];
  return out;
}

void TcNetCollector::collect(monitor::MetricsSample *out) {
  auto [rx, tx] = read_bytes();
  auto now = std::chrono::steady_clock::now();
  if (has_last_) {
    if (rx < last_rx_ || tx < last_tx_) {
      // 计数器归零（程序重挂/网卡重建）：重建基线
      last_rx_ = rx;
      last_tx_ = tx;
      last_ts_ = now;
      return;
    }
    double dt = std::chrono::duration<double>(now - last_ts_).count();
    if (dt > 0) {
      out->set_net_recv_bps((rx - last_rx_) / dt);
      out->set_net_send_bps((tx - last_tx_) / dt);
    }
  }
  last_rx_ = rx;
  last_tx_ = tx;
  last_ts_ = now;
  has_last_ = true;
}

} // namespace agent

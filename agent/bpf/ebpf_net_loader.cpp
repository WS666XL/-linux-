#include "ebpf_net_loader.h"

#include <cerrno>
#include <cstring>
#include <vector>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "common/logger.h"

namespace agent {

EbpfNetLoader::~EbpfNetLoader() {
  if (rx_link_) bpf_link__destroy(rx_link_);
  if (tx_link_) bpf_link__destroy(tx_link_);
  if (obj_) bpf_object__close(obj_);
}

bool EbpfNetLoader::load(const std::string& path) {
  libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

  obj_ = bpf_object__open_file(path.c_str(), nullptr);
  if (!obj_) {
    LOG_ERROR("ebpf: open %s failed: %s", path.c_str(), strerror(errno));
    return false;
  }
  if (bpf_object__load(obj_)) {
    // 常见原因：非 root 运行、WSL2 内核未开启 kprobe、内核版本不兼容
    LOG_WARN("ebpf: load %s failed: %s (need root? kernel support?)",
             path.c_str(), strerror(errno));
    bpf_object__close(obj_);
    obj_ = nullptr;
    return false;
  }

  map_fd_ = bpf_object__find_map_fd_by_name(obj_, "net_bytes");
  if (map_fd_ < 0) {
    LOG_ERROR("ebpf: map net_bytes not found");
    bpf_object__close(obj_);
    obj_ = nullptr;
    return false;
  }

  struct bpf_program *tx = bpf_object__find_program_by_name(obj_, "k_tx");
  struct bpf_program *rx = bpf_object__find_program_by_name(obj_, "k_rx");
  if (!tx || !rx) {
    LOG_ERROR("ebpf: program not found in %s", path.c_str());
    return false;
  }
  tx_link_ = bpf_program__attach_kprobe(tx, false /* attach, not retprobe */,
                                        "__dev_queue_xmit");
  rx_link_ = bpf_program__attach_kprobe(rx, false, "__netif_receive_skb");
  if (!tx_link_ || !rx_link_) {
    LOG_WARN("ebpf: attach kprobe failed: %s (WSL2 kernel may lack kprobes?)",
             strerror(errno));
    return false;
  }
  LOG_INFO("ebpf: net monitor loaded, kprobe on __dev_queue_xmit/__netif_receive_skb");
  return true;
}

std::pair<uint64_t, uint64_t> EbpfNetLoader::read_bytes() {
  std::pair<uint64_t, uint64_t> out{0, 0};
  if (map_fd_ < 0) return out;
  // percpu 读取：每个 CPU 一个槽位，汇总所有 CPU
  int cpus = libbpf_num_possible_cpus();
  if (cpus <= 0) return out;
  std::vector<uint64_t> buf(cpus);
  uint32_t key = 0;
  if (bpf_map_lookup_elem(map_fd_, &key, buf.data()) == 0) {
    for (int i = 0; i < cpus; ++i) out.first += buf[i];
  }
  key = 1;
  if (bpf_map_lookup_elem(map_fd_, &key, buf.data()) == 0) {
    for (int i = 0; i < cpus; ++i) out.second += buf[i];
  }
  return out;
}

}  // namespace agent

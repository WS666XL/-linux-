#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include <bpf/libbpf.h>

namespace agent {

// eBPF 网络采集的用户态接口
// load() 需要root（CAP_BPF/CAP_SYS_ADMIN）；失败时调用方应回退 /proc 采集
class EbpfNetLoader {
 public:
  EbpfNetLoader() = default;
  ~EbpfNetLoader();

  EbpfNetLoader(const EbpfNetLoader&) = delete;
  EbpfNetLoader& operator=(const EbpfNetLoader&) = delete;

  // 加载并挂载 kprobe，path 为编译好的 .bpf.o 对象文件
  bool load(const std::string& path);

  // 读取自挂载以来的累计字节数（内核态持续累加，用户态做差值算速率）
  // 返回 {rx_bytes, tx_bytes}，未加载时返回 {0,0}
  std::pair<uint64_t, uint64_t> read_bytes();

 private:
  bpf_object* obj_ = nullptr;
  bpf_link* rx_link_ = nullptr;
  bpf_link* tx_link_ = nullptr;
  int map_fd_ = -1;
};

}  // namespace agent

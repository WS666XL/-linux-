#include "mem_collector.h"

#include <cstdint>
#include <fstream>
#include <string>

#include "common/logger.h"

namespace agent {

void MemCollector::collect(monitor::MetricsSample *out) {
  std::ifstream in("/proc/meminfo");
  if (!in.is_open()) {
    LOG_ERROR("mem_collector: open /proc/meminfo failed");
    return;
  }
  std::string key, unit;                  // 键值对键、单位
  uint64_t val = 0, total = 0, avail = 0; // 数值、总内存、可用内存
  while (in >> key) {
    in >> val;
    in >> unit; // 数值后通常带 kB 单位，必须消费掉
    if (key == "MemTotal:") {
      total = val;
    } else if (key == "MemAvailable:") {
      avail = val;
      break;
    }
  }
  if (total > 0 && total >= avail) {
    out->set_mem_usage(100.0 * (total - avail) / total); // 内存使用率
  }
}

} // namespace agent

#include "net_collector.h"

#include <fstream>
#include <sstream>
#include <string>

#include "common/logger.h"

namespace agent {

namespace {

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

}  // namespace

void NetCollector::collect(monitor::MetricsSample* out) {
  std::ifstream in("/proc/net/dev");
  if (!in.is_open()) {
    LOG_ERROR("net_collector: open /proc/net/dev failed");
    return;
  }

  uint64_t recv = 0, send = 0;
  std::string line;
  std::getline(in, line);  // 跳过两行表头
  std::getline(in, line);
  while (std::getline(in, line)) {
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string ifname = trim(line.substr(0, colon));
    if (ifname == "lo") continue;  // 排除回环
    std::istringstream ss(line.substr(colon + 1));
    uint64_t rbytes = 0, sbytes = 0;
    ss >> rbytes;  // 第 1 列 receive bytes
    uint64_t tmp = 0;
    for (int i = 0; i < 7; ++i) ss >> tmp;  // 跳过 7 列
    ss >> sbytes;  // 第 9 列 transmit bytes
    recv += rbytes;
    send += sbytes;
  }

  auto now = std::chrono::steady_clock::now();
  if (has_last_) {
    double dt = std::chrono::duration<double>(now - last_ts_).count();
    if (dt > 0) {
      out->set_net_recv_bps((recv - last_recv_) / dt);
      out->set_net_send_bps((send - last_send_) / dt);
    }
  }
  last_recv_ = recv;
  last_send_ = send;
  last_ts_ = now;
  has_last_ = true;
}

}  // namespace agent

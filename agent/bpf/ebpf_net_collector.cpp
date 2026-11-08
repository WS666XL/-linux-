#include "ebpf_net_collector.h"

namespace agent {

bool EbpfNetCollector::init(const std::string& bpf_object_path) {
  if (bpf_object_path.empty()) return false;
  return loader_.load(bpf_object_path);
}

void EbpfNetCollector::collect(monitor::MetricsSample* out) {
  auto [rx, tx] = loader_.read_bytes();
  auto now = std::chrono::steady_clock::now();
  if (has_last_) {
    double dt = std::chrono::duration<double>(now - last_ts_).count();
    if (dt > 0 && rx >= last_rx_ && tx >= last_tx_) {
      out->set_net_recv_bps((rx - last_rx_) / dt);
      out->set_net_send_bps((tx - last_tx_) / dt);
    }
  }
  last_rx_ = rx;
  last_tx_ = tx;
  last_ts_ = now;
  has_last_ = true;
}

}  // namespace agent

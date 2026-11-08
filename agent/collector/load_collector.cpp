#include "load_collector.h"

#include <fstream>

#include "common/logger.h"

namespace agent {

void LoadCollector::collect(monitor::MetricsSample* out) {
  std::ifstream in("/proc/loadavg");
  if (!in.is_open()) {
    LOG_ERROR("load_collector: open /proc/loadavg failed");
    return;
  }
  double l1 = 0, l5 = 0, l15 = 0;
  if (!(in >> l1 >> l5 >> l15)) {
    LOG_ERROR("load_collector: parse /proc/loadavg failed");
    return;
  }
  out->set_load_avg1(l1);
  out->set_load_avg5(l5);
  out->set_load_avg15(l15);
}

}  // namespace agent

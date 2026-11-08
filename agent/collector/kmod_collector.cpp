#include "kmod_collector.h"

#include <fstream>

#include "common/logger.h"

namespace agent {

bool KmodCpuCollector::read_snapshot(Snapshot* s) const {
  std::ifstream in("/dev/cpu_stat_monitor");
  uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0,
           softirq = 0, steal = 0;
  if (!(in >> user >> nice >> system >> idle >> iowait >> irq >> softirq >>
        steal))
    return false;
  s->idle = idle;
  s->iowait = iowait;
  s->softirq = softirq;
  s->total = user + nice + system + idle + iowait + irq + softirq + steal;
  return true;
}

void KmodCpuCollector::collect(monitor::MetricsSample* out) {
  Snapshot cur;
  if (!read_snapshot(&cur)) {
    LOG_ERROR("kmod_cpu: read /dev/cpu_stat_monitor failed");
    return;
  }
  if (has_last_) {
    uint64_t dt = cur.total - last_.total;
    if (dt > 0) {
      out->set_cpu_usage(100.0 * (dt - (cur.idle - last_.idle)) / dt);
      out->set_cpu_iowait(100.0 * (cur.iowait - last_.iowait) / dt);
      out->set_cpu_softirq(100.0 * (cur.softirq - last_.softirq) / dt);
    }
  }
  last_ = cur;
  has_last_ = true;
}

bool SoftirqCollector::read_total(uint64_t* total) const {
  std::ifstream in("/dev/softirq_monitor");
  if (!(in >> *total)) return false;
  return true;
}

void SoftirqCollector::collect(monitor::MetricsSample* out) {
  uint64_t cur = 0;
  if (!read_total(&cur)) {
    LOG_ERROR("kmod_softirq: read /dev/softirq_monitor failed");
    return;
  }
  if (has_last_ && cur > last_) {
    // 采集间隔默认 1s，rate 即差值；通用起见按时间戳折算
    // 这里由 main 保证间隔，简单取差值/1s，精度足够
    out->set_softirq_rate(static_cast<double>(cur - last_));
  }
  last_ = cur;
  has_last_ = true;
}

}  // namespace agent

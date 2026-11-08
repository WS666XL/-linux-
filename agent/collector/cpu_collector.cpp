#include "cpu_collector.h"

#include <fstream>

#include "common/logger.h"

namespace agent {

// 读取快照
// 返回值：是否成功读取快照
// 参数：s - 输出参数，用于存储读取到的快照数据
bool CpuCollector::read_snapshot(Snapshot *s) const {
  std::ifstream in("/proc/stat");
  std::string cpu;
  if (!(in >> cpu) || cpu != "cpu")
    return false;
  uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0,
           softirq = 0, steal = 0;
  in >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
  s->idle = idle;
  s->iowait = iowait;
  s->softirq = softirq;
  s->total = user + nice + system + idle + iowait + irq + softirq + steal;
  return true;
}

// 采集 CPU 指标
void CpuCollector::collect(monitor::MetricsSample *out) {
  Snapshot cur; // 当前采样快照
  if (!read_snapshot(&cur)) {
    LOG_ERROR("cpu_collector: read /proc/stat failed");
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
  // 更新上次采样快照为当前采样快照
  // 更新快照和状态
  last_ = cur;
  // 标记已存在上次采样快照
  has_last_ = true;
}

} // namespace agent

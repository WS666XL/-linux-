#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "monitor.pb.h"

namespace manager {

// 异常检测：阈值告警 + 相邻采样突变告警
// 默认打日志；on_alert 钩子可注入（测试/后续通知渠道）
class AlertEngine {
 public:
  struct Options {
    double cpu_max = 90.0;   // CPU 使用率阈值 %
    double mem_max = 90.0;   // 内存使用率阈值 %
    double cpu_jump = 30.0;  // 相邻两次采样 CPU 突变阈值 %
  };

  struct Event {
    std::string node_id;
    std::string kind;  // "cpu_max" / "mem_max" / "cpu_jump"
    double value = 0;  // 当前值；cpu_jump 时为突变量
  };

  explicit AlertEngine(Options opt) : opt_(opt) {}

  void check(const monitor::MetricsSample& s);

  std::function<void(const Event&)> on_alert;  // 测试/通知注入点

 private:
  Options opt_;
  std::map<std::string, monitor::MetricsSample> last_;  // node_id -> 上次采样
  std::mutex mu_;
};

}  // namespace manager

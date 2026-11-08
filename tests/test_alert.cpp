#include <string>
#include <vector>

#include "alert/alert_engine.h"
#include "test_framework.h"

namespace {

monitor::MetricsSample sample(const std::string& node, double cpu,
                              double mem = 50.0) {
  monitor::MetricsSample s;
  s.set_node_id(node);
  s.set_cpu_usage(cpu);
  s.set_mem_usage(mem);
  return s;
}

std::vector<manager::AlertEngine::Event> capture(
    manager::AlertEngine& engine) {
  std::vector<manager::AlertEngine::Event> events;
  engine.on_alert = [&events](const manager::AlertEngine::Event& e) {
    events.push_back(e);
  };
  return events;
}

}  // namespace

TEST(alert_first_sample_no_alert) {
  // 无基线：第一次采样不应触发突变告警
  manager::AlertEngine e({90.0, 90.0, 30.0});
  auto ev = capture(e);
  e.check(sample("n1", 100.0));  // 即使 CPU 打满
  CHECK(ev.empty());
}

TEST(alert_cpu_threshold) {
  manager::AlertEngine e({90.0, 90.0, 30.0});
  auto ev = capture(e);
  e.check(sample("n1", 50.0));   // 建立基线
  CHECK(ev.empty());
  e.check(sample("n1", 95.0));   // 超阈值 + 突变 45
  CHECK(ev.size() == 2);         // cpu_max + cpu_jump 各一条
  CHECK(ev[0].kind == "cpu_max");
  CHECK_NEAR(ev[0].value, 95.0, 1e-9);
  CHECK(ev[1].kind == "cpu_jump");
  CHECK_NEAR(ev[1].value, 45.0, 1e-9);
}

TEST(alert_threshold_boundary_strict) {
  // 等于阈值不算超（严格大于）
  manager::AlertEngine e({90.0, 90.0, 30.0});
  auto ev = capture(e);
  e.check(sample("n1", 60.0));
  e.check(sample("n1", 90.0));  // == 阈值，且突变 30 == jump 阈值
  CHECK(ev.empty());
}

TEST(alert_mem_threshold) {
  manager::AlertEngine e({90.0, 80.0, 100.0});  // jump 阈值放大避免混入
  auto ev = capture(e);
  e.check(sample("n1", 50.0, 85.0));
  CHECK(ev.empty());
  e.check(sample("n1", 50.0, 85.0));  // 与基线相同，mem 超 80
  CHECK(ev.size() == 1);
  CHECK(ev[0].kind == "mem_max");
  CHECK_NEAR(ev[0].value, 85.0, 1e-9);
}

TEST(alert_per_node_independent) {
  manager::AlertEngine e({90.0, 90.0, 30.0});
  auto ev = capture(e);
  e.check(sample("a", 80.0));  // a 基线 80
  e.check(sample("b", 10.0));  // b 基线 10
  e.check(sample("b", 20.0));  // b 突变 10 < 30，不告警
  CHECK(ev.empty());
  e.check(sample("a", 85.0));  // a 突变 5，不告警
  CHECK(ev.empty());
  CHECK(true);
}

TEST(alert_negative_jump_counts) {
  // 突变取绝对值：CPU 骤降同样告警
  manager::AlertEngine e({90.0, 90.0, 30.0});
  auto ev = capture(e);
  e.check(sample("n1", 80.0));
  e.check(sample("n1", 40.0));  // 骤降 40
  CHECK(ev.size() == 1);
  CHECK(ev[0].kind == "cpu_jump");
  CHECK_NEAR(ev[0].value, 40.0, 1e-9);
}

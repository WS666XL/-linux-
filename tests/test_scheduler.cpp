#include <string>
#include <vector>

#include "scheduler/weighted_scheduler.h"
#include "test_framework.h"

namespace {

monitor::MetricsSample sample(const std::string& node, double cpu, double mem,
                              double iops, double net_total_bps) {
  monitor::MetricsSample s;
  s.set_node_id(node);
  s.set_cpu_usage(cpu);
  s.set_mem_usage(mem);
  s.set_disk_iops(iops);
  s.set_net_recv_bps(net_total_bps / 2);
  s.set_net_send_bps(net_total_bps / 2);
  return s;
}

}  // namespace

TEST(scheduler_empty_rank) {
  manager::WeightedScheduler sch({});
  CHECK(sch.rank("cpu_bound").empty());
  CHECK(sch.rank("unknown").empty());
}

TEST(scheduler_score_range) {
  manager::WeightedScheduler sch({});
  sch.update(sample("n1", 100.0, 100.0, 9999.0, 400e6));  // 全打满
  auto r = sch.rank("default");
  CHECK(r.size() == 1);
  CHECK(r[0].score >= 0.0 && r[0].score <= 100.0);
}

TEST(scheduler_cpu_bound_prefers_idle_cpu) {
  manager::WeightedScheduler sch({});
  sch.update(sample("busy", 95.0, 50.0, 100.0, 1e6));
  sch.update(sample("idle", 5.0, 50.0, 100.0, 1e6));
  auto r = sch.rank("cpu_bound");
  CHECK(r.size() == 2);
  CHECK(r[0].node_id == "idle");
  CHECK(r[0].score > r[1].score);
}

TEST(scheduler_net_bound_prefers_low_traffic) {
  manager::WeightedScheduler sch({});
  sch.update(sample("heavy", 50.0, 50.0, 100.0, 190e6));  // 接近 200MB/s 打满
  sch.update(sample("light", 50.0, 50.0, 100.0, 10e6));
  auto r = sch.rank("net_bound");
  CHECK(r[0].node_id == "light");
}

TEST(scheduler_io_bound_prefers_low_iops) {
  manager::WeightedScheduler sch({});
  sch.update(sample("ioheavy", 50.0, 50.0, 4500.0, 1e6));
  sch.update(sample("ioidle", 50.0, 50.0, 100.0, 1e6));
  auto r = sch.rank("io_bound");
  CHECK(r[0].node_id == "ioidle");
}

TEST(scheduler_unknown_scenario_uses_default_weights) {
  // 默认权重 cpu=0.5: CPU 空闲差异主导排序结果
  manager::WeightedScheduler sch({});
  sch.update(sample("busy", 90.0, 50.0, 100.0, 1e6));
  sch.update(sample("idle", 10.0, 50.0, 100.0, 1e6));
  auto r = sch.rank("whatever");
  CHECK(r[0].node_id == "idle");
  // 手算期望分: idle = 0.5*90 + 0.3*50 + 0.1*98 + 0.1*~99.5 = 79.75
  CHECK_NEAR(r[0].score, 79.75, 0.1);
}

TEST(scheduler_update_overwrites_latest) {
  manager::WeightedScheduler sch({});
  sch.update(sample("n1", 95.0, 50.0, 100.0, 1e6));
  sch.update(sample("n1", 5.0, 50.0, 100.0, 1e6));  // 新采样覆盖旧采样
  auto r = sch.rank("cpu_bound");
  CHECK(r.size() == 1);
  CHECK_NEAR(r[0].latest.cpu_usage(), 5.0, 1e-9);
}

TEST(scheduler_rank_descending_order) {
  manager::WeightedScheduler sch({});
  for (int i = 0; i < 5; ++i) {
    sch.update(sample("node" + std::to_string(i), 20.0 * i, 50.0, 100.0, 1e6));
  }
  auto r = sch.rank("cpu_bound");
  CHECK(r.size() == 5);
  for (size_t i = 1; i < r.size(); ++i) {
    CHECK(r[i - 1].score >= r[i].score);
  }
  CHECK(r[0].node_id == "node0");  // cpu=0 最空闲
}

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <mysql.h>

#include "monitor.pb.h"

namespace manager {

// 趋势统计结果
struct TrendResult {
  double cpu_avg = 0;
  double mem_avg = 0;
  double softirq_avg = 0;
  double net_total_avg_bps = 0;
  int sample_count = 0;
};

// 指标持久化：MySQL 预处理语句批量写单条采样
// enable=false 时所有接口直接返回 true，方便无库环境跑通链路
// 掉线自动重连（5s 节流 + 3s 连接超时），manager 启动不依赖 MySQL 就绪
class MysqlStorage {
 public:
  struct Options {
    bool enable = false;
    std::string host = "127.0.0.1";
    int port = 3306;
    std::string user;
    std::string password;
    std::string db;
  };

  explicit MysqlStorage(Options opt);
  ~MysqlStorage();

  bool connect();
  bool insert(const monitor::MetricsSample& s);

  // 主机标识：每节点一行（node_id 主键），上报流首条消息触发写入
  bool upsert_host(const std::string& node_id, const monitor::HostInfo& h);
  bool query_host(const std::string& node_id, monitor::HostInfo* out);

  // ---- QueryService 数据源（只读，与写入共用连接与锁）----
  bool query_latest(const std::string& node_id, monitor::MetricsSample* out);
  // 返回条数；连接/查询失败返回 -1（QueryService 据此返回 UNAVAILABLE）
  int query_history(const std::string& node_id, int64_t start_ms,
                    int64_t end_ms, int limit,
                    std::vector<monitor::MetricsSample>* out);
  bool query_trend(const std::string& node_id, int window_sec,
                   TrendResult* out);

 private:
  // 持锁调用：检查连接活性（ping），掉线则重连
  bool ensure_connected();

  Options opt_;
  MYSQL* conn_ = nullptr;
  std::mutex mu_;  // 当前只有上报线程写；后续多流并发时保证线程安全
  std::chrono::steady_clock::time_point last_retry_{};
  bool retried_ = false;
};

}  // namespace manager

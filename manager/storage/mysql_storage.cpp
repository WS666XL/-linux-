#include "mysql_storage.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/logger.h"

namespace manager {

namespace {

const char* kInsertSql =
    "INSERT INTO metrics(node_id, ts, cpu_usage, cpu_iowait, cpu_softirq, "
    "mem_usage, disk_read_bps, disk_write_bps, disk_iops, net_recv_bps, "
    "net_send_bps, softirq_rate, load_avg1, load_avg5, load_avg15) "
    "VALUES(?, FROM_UNIXTIME(?/1000), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

// SQL 字符串转义（调用方需持锁，conn_ 非线程安全）
std::string escape(MYSQL* conn, const std::string& in) {
  if (in.empty()) return "";
  std::string out(in.size() * 2 + 1, '\0');
  unsigned long n = mysql_real_escape_string(
      conn, out.data(), in.c_str(), static_cast<unsigned long>(in.size()));
  out.resize(n);
  return out;
}

}  // namespace

MysqlStorage::MysqlStorage(Options opt) : opt_(std::move(opt)) {}

MysqlStorage::~MysqlStorage() {
  if (conn_) mysql_close(conn_);
}

bool MysqlStorage::connect() {
  if (!opt_.enable) return true;
  conn_ = mysql_init(nullptr);
  if (!conn_) {
    LOG_ERROR("storage: mysql_init failed");
    return false;
  }
  const unsigned timeout = 3;  // 连接/读写超时，避免阻塞上报线程
  mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(conn_, MYSQL_OPT_READ_TIMEOUT, &timeout);
  mysql_options(conn_, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
  if (!mysql_real_connect(conn_, opt_.host.c_str(), opt_.user.c_str(),
                          opt_.password.c_str(), opt_.db.c_str(), opt_.port,
                          nullptr, 0)) {
    LOG_ERROR("storage: connect %s:%d db=%s failed: %s", opt_.host.c_str(),
              opt_.port, opt_.db.c_str(), mysql_error(conn_));
    mysql_close(conn_);
    conn_ = nullptr;
    return false;
  }
  LOG_INFO("storage: connected to %s:%d db=%s", opt_.host.c_str(), opt_.port,
           opt_.db.c_str());
  return true;
}

bool MysqlStorage::ensure_connected() {
  if (!opt_.enable) return true;
  if (conn_ && mysql_ping(conn_) == 0) return true;
  // 重连节流：5s 内最多尝试一次，避免每秒采样刷爆日志
  auto now = std::chrono::steady_clock::now();
  if (retried_ && now - last_retry_ < std::chrono::seconds(5)) return false;
  last_retry_ = now;
  retried_ = true;
  if (conn_) {
    mysql_close(conn_);
    conn_ = nullptr;
  }
  return connect();
}

bool MysqlStorage::insert(const monitor::MetricsSample& s) {
  if (!opt_.enable) return true;

  // 全程持锁：MYSQL 句柄非线程安全，多节点并发上报时
  // ensure_connected 与 stmt 操作必须串行，否则连接损坏 + SSL 结构双重释放
  std::lock_guard<std::mutex> lk(mu_);
  if (!ensure_connected()) return false;

  // node_id 列是 VARCHAR(64)，留 1 字节余量给终止符，避免插入被 MySQL 截断报错
  if (s.node_id().size() > 63) {
    LOG_ERROR("storage: node_id too long (%zu > 63)", s.node_id().size());
    return false;
  }

  MYSQL_STMT* st = mysql_stmt_init(conn_);
  if (!st) return false;
  bool ok = false;
  do {
    if (mysql_stmt_prepare(st, kInsertSql, strlen(kInsertSql))) break;

    char node_buf[128] = {};
    const size_t node_len =
        s.node_id().copy(node_buf, sizeof(node_buf) - 1);
    const int64_t ts_ms = s.timestamp_ms();
    const double cpu = s.cpu_usage(), iowait = s.cpu_iowait(),
                 softirq = s.cpu_softirq(), mem = s.mem_usage(),
                 dr = s.disk_read_bps(), dw = s.disk_write_bps(),
                 iops = s.disk_iops(), nr = s.net_recv_bps(),
                 ns = s.net_send_bps(), sir = s.softirq_rate(),
                 l1 = s.load_avg1(), l5 = s.load_avg5(), l15 = s.load_avg15();

    MYSQL_BIND bind[15] = {};
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = node_buf;
    bind[0].buffer_length = node_len;
    unsigned long len = node_len;
    bind[0].length = &len;
    bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[1].buffer = (void*)&ts_ms;
    double vals[] = {cpu, iowait, softirq, mem,       dr, dw, iops,
                     nr,  ns,      sir,     l1,       l5, l15};
    for (int i = 0; i < 13; ++i) {
      bind[2 + i].buffer_type = MYSQL_TYPE_DOUBLE;
      bind[2 + i].buffer = &vals[i];
    }

    if (mysql_stmt_bind_param(st, bind)) break;
    if (mysql_stmt_execute(st)) break;
    ok = true;
  } while (false);

  if (!ok) {
    LOG_ERROR("storage: insert failed: %s", mysql_stmt_error(st));
  }
  mysql_stmt_close(st);
  return ok;
}

bool MysqlStorage::upsert_host(const std::string& node_id,
                               const monitor::HostInfo& h) {
  if (!opt_.enable) return true;
  std::lock_guard<std::mutex> lk(mu_);
  if (!ensure_connected()) return false;

  const std::string e_node = escape(conn_, node_id);
  const std::string e_host = escape(conn_, h.hostname());
  const std::string e_ip = escape(conn_, h.ip());
  const std::string e_mid = escape(conn_, h.machine_id());
  const std::string e_kernel = escape(conn_, h.kernel());
  const std::string e_os = escape(conn_, h.os());

  std::string sql =
      "INSERT INTO hosts(node_id,hostname,ip,machine_id,kernel,os,"
      "boot_time_ms,updated_at) VALUES('" +
      e_node + "','" + e_host + "','" + e_ip + "','" + e_mid + "','" +
      e_kernel + "','" + e_os + "'," + std::to_string(h.boot_time_ms()) +
      ",NOW(3)) ON DUPLICATE KEY UPDATE hostname=VALUES(hostname),"
      "ip=VALUES(ip),machine_id=VALUES(machine_id),kernel=VALUES(kernel),"
      "os=VALUES(os),boot_time_ms=VALUES(boot_time_ms),updated_at=NOW(3)";
  if (mysql_query(conn_, sql.c_str())) {
    LOG_WARN("storage: upsert host failed: %s", mysql_error(conn_));
    return false;
  }
  return true;
}

bool MysqlStorage::query_host(const std::string& node_id,
                              monitor::HostInfo* out) {
  if (!opt_.enable) return false;
  std::lock_guard<std::mutex> lk(mu_);
  if (!ensure_connected()) return false;

  const std::string e_node = escape(conn_, node_id);
  const std::string sql =
      "SELECT hostname,ip,machine_id,kernel,os,boot_time_ms FROM hosts "
      "WHERE node_id='" + e_node + "' LIMIT 1";
  if (mysql_query(conn_, sql.c_str())) return false;
  MYSQL_RES* res = mysql_store_result(conn_);
  if (!res) return false;
  MYSQL_ROW r = mysql_fetch_row(res);
  bool ok = r != nullptr;
  if (ok) {
    out->set_hostname(r[0] ? r[0] : "");
    out->set_ip(r[1] ? r[1] : "");
    out->set_machine_id(r[2] ? r[2] : "");
    out->set_kernel(r[3] ? r[3] : "");
    out->set_os(r[4] ? r[4] : "");
    out->set_boot_time_ms(r[5] ? std::strtoll(r[5], nullptr, 10) : 0);
  }
  mysql_free_result(res);
  return ok;
}

namespace {

// 把 metrics 查询结果的一行填充进 MetricsSample（列序见 kSelectCols）
void fill_sample(MYSQL_ROW r, monitor::MetricsSample* s) {
  s->set_node_id(r[0] ? r[0] : "");
  s->set_timestamp_ms(r[1] ? std::strtoll(r[1], nullptr, 10) : 0);
  s->set_cpu_usage(r[2] ? std::strtod(r[2], nullptr) : 0);
  s->set_cpu_iowait(r[3] ? std::strtod(r[3], nullptr) : 0);
  s->set_cpu_softirq(r[4] ? std::strtod(r[4], nullptr) : 0);
  s->set_mem_usage(r[5] ? std::strtod(r[5], nullptr) : 0);
  s->set_disk_read_bps(r[6] ? std::strtod(r[6], nullptr) : 0);
  s->set_disk_write_bps(r[7] ? std::strtod(r[7], nullptr) : 0);
  s->set_disk_iops(r[8] ? std::strtod(r[8], nullptr) : 0);
  s->set_net_recv_bps(r[9] ? std::strtod(r[9], nullptr) : 0);
  s->set_net_send_bps(r[10] ? std::strtod(r[10], nullptr) : 0);
  s->set_softirq_rate(r[11] ? std::strtod(r[11], nullptr) : 0);
  s->set_load_avg1(r[12] ? std::strtod(r[12], nullptr) : 0);
  s->set_load_avg5(r[13] ? std::strtod(r[13], nullptr) : 0);
  s->set_load_avg15(r[14] ? std::strtod(r[14], nullptr) : 0);
}

const char* kSelectCols =
    "node_id, UNIX_TIMESTAMP(ts)*1000, cpu_usage, cpu_iowait, cpu_softirq, "
    "mem_usage, disk_read_bps, disk_write_bps, disk_iops, net_recv_bps, "
    "net_send_bps, softirq_rate, load_avg1, load_avg5, load_avg15 FROM metrics";

}  // namespace

bool MysqlStorage::query_latest(const std::string& node_id,
                                monitor::MetricsSample* out) {
  if (!opt_.enable) return false;
  std::lock_guard<std::mutex> lk(mu_);
  if (!ensure_connected()) return false;

  const std::string e_node = escape(conn_, node_id);
  const std::string sql = "SELECT " + std::string(kSelectCols) +
                          " WHERE node_id='" + e_node +
                          "' ORDER BY ts DESC LIMIT 1";
  if (mysql_query(conn_, sql.c_str())) return false;
  MYSQL_RES* res = mysql_store_result(conn_);
  if (!res) return false;
  MYSQL_ROW r = mysql_fetch_row(res);
  bool ok = r != nullptr;
  if (ok) fill_sample(r, out);
  mysql_free_result(res);
  return ok;
}

int MysqlStorage::query_history(const std::string& node_id, int64_t start_ms,
                                int64_t end_ms, int limit,
                                std::vector<monitor::MetricsSample>* out) {
  out->clear();
  if (!opt_.enable) return -1;
  std::lock_guard<std::mutex> lk(mu_);
  if (!ensure_connected()) return -1;
  if (limit <= 0) limit = 600;
  if (limit > 10000) limit = 10000;

  const std::string e_node = escape(conn_, node_id);
  const std::string sql =
      "SELECT " + std::string(kSelectCols) + " WHERE node_id='" + e_node +
      "' AND ts BETWEEN FROM_UNIXTIME(" + std::to_string(start_ms / 1000.0) +
      ") AND FROM_UNIXTIME(" + std::to_string(end_ms / 1000.0) +
      ") ORDER BY ts ASC LIMIT " + std::to_string(limit);
  if (mysql_query(conn_, sql.c_str())) return -1;
  MYSQL_RES* res = mysql_store_result(conn_);
  if (!res) return -1;
  int n = 0;
  while (MYSQL_ROW r = mysql_fetch_row(res)) {
    monitor::MetricsSample s;
    fill_sample(r, &s);
    out->push_back(std::move(s));
    ++n;
  }
  mysql_free_result(res);
  return n;
}

bool MysqlStorage::query_trend(const std::string& node_id, int window_sec,
                               TrendResult* out) {
  if (!opt_.enable) return false;
  std::lock_guard<std::mutex> lk(mu_);
  if (!ensure_connected()) return false;
  if (window_sec <= 0) window_sec = 300;
  if (window_sec > 86400) window_sec = 86400;

  const std::string e_node = escape(conn_, node_id);
  const std::string sql =
      "SELECT AVG(cpu_usage), AVG(mem_usage), AVG(softirq_rate), "
      "AVG(net_recv_bps + net_send_bps), COUNT(*) FROM metrics "
      "WHERE node_id='" + e_node + "' AND ts >= NOW() - INTERVAL " +
      std::to_string(window_sec) + " SECOND";
  if (mysql_query(conn_, sql.c_str())) return false;
  MYSQL_RES* res = mysql_store_result(conn_);
  if (!res) return false;
  MYSQL_ROW r = mysql_fetch_row(res);
  bool ok = r != nullptr;
  if (ok) {
    out->cpu_avg = r[0] ? std::strtod(r[0], nullptr) : 0;
    out->mem_avg = r[1] ? std::strtod(r[1], nullptr) : 0;
    out->softirq_avg = r[2] ? std::strtod(r[2], nullptr) : 0;
    out->net_total_avg_bps = r[3] ? std::strtod(r[3], nullptr) : 0;
    out->sample_count = r[4] ? std::atoi(r[4]) : 0;
  }
  mysql_free_result(res);
  return ok;
}

}  // namespace manager

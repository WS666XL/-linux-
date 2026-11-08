-- 分布式监控系统数据库初始化
CREATE DATABASE IF NOT EXISTS monitor_db DEFAULT CHARSET utf8mb4;
USE monitor_db;

CREATE TABLE IF NOT EXISTS metrics (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  node_id VARCHAR(64) NOT NULL,
  ts DATETIME(3) NOT NULL,
  cpu_usage DOUBLE,
  cpu_iowait DOUBLE,
  cpu_softirq DOUBLE,
  mem_usage DOUBLE,
  disk_read_bps DOUBLE,
  disk_write_bps DOUBLE,
  disk_iops DOUBLE,
  net_recv_bps DOUBLE,
  net_send_bps DOUBLE,
  softirq_rate DOUBLE,  -- 每秒软中断次数（内核模块采集）
  load_avg1 DOUBLE,     -- 1 分钟平均负载（/proc/loadavg）
  load_avg5 DOUBLE,     -- 5 分钟平均负载
  load_avg15 DOUBLE,    -- 15 分钟平均负载
  INDEX idx_node_ts (node_id, ts)
) ENGINE=InnoDB;

-- 主机标识表：每个 agent 一行（node_id 主键），随上报流首条消息写入
CREATE TABLE IF NOT EXISTS hosts (
  node_id VARCHAR(64) PRIMARY KEY,
  hostname VARCHAR(255) NOT NULL DEFAULT '',
  ip VARCHAR(64) NOT NULL DEFAULT '',
  machine_id VARCHAR(64) NOT NULL DEFAULT '',
  kernel VARCHAR(128) NOT NULL DEFAULT '',
  os VARCHAR(255) NOT NULL DEFAULT '',
  boot_time_ms BIGINT NOT NULL DEFAULT 0,
  updated_at DATETIME(3) NOT NULL
) ENGINE=InnoDB;

-- TODO(第二阶段)：按时间分区或定期归档，避免时序数据无限增长

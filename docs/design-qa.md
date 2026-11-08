# DistMonitor 设计问答

> 本文记录 DistMonitor 的关键设计取舍、指标含义与踩坑过程，用于答辩/面试时讲清"为什么这样做"，而不是只背功能列表。

## 1. 一句话架构

agent 每秒从内核模块 / eBPF / `/proc` 采集指标 → gRPC client-streaming 上报 → manager 收流后告警、加权调度、落 MySQL → QueryService 查询、monitor_tui 终端展示。

## 2. 需要掌握的基础知识

| 概念 | 在本项目中的用途 |
|------|------------------|
| `/proc` 文件系统 | CPU、内存、磁盘、网络等指标的通用采集源，无需权限、无需依赖 |
| 内核模块 | 通过 kcpustat / kstat 直接读内核累计计数，暴露为 `/dev/*_monitor` 字符设备 |
| 字符设备 | 内核模块与用户态的边界：`misc_register` 注册设备，用户态 `read()` 取一行文本 |
| eBPF + CO-RE | 内核函数挂 kprobe、TC 分类器挂载点，用 `vmlinux.h` 做跨内核版本的类型重定位 |
| gRPC / Protobuf | agent 与 manager 的通信协议；流式上报降低连接开销 |
| mmap（延伸） | 字符设备传递大数据时的零拷贝方案；本项目数据量小，用 `read()` 已足够 |

## 3. 项目背景与目标

- 目标：多节点 Linux 服务器的 CPU / 内存 / 磁盘 / 网络指标采集、异常发现与负载调度
- 约束：纯 C++ 实现，不引入 Prometheus/Agent 这类现成组件；能在 WSL2 开发和云服务器部署
- 结果：单机 agent 常驻内存约 4MB、CPU < 0.5%；manager 单进程同时提供 gRPC 与 HTTP

## 4. 架构主逻辑

```text
agent（每节点）                           manager（单进程）
  ├ 采集器（kmod / eBPF / TC / proc）
  ├ 每 interval_ms 组装 MetricsSample
  └ gRPC client-streaming ──────────────► MonitorService
                                           ├ WeightedScheduler.update
                                           ├ AlertEngine.check
                                           └ MysqlStorage.insert
                                         QueryService（快照/历史/趋势/主机信息）
  monitor_tui / query_client ──────────► 终端展示 / 接口验证
```

**为什么 agent/manager 分离**：采集要贴着被监控机（需要 root、内核权限），分析与存储要集中（告警阈值、调度排名需要全局视图）。两者只通过 gRPC 协议耦合，agent 不认识 MySQL，manager 不关心采集方式。

## 5. 监控指标与采集方式

| 指标 | 首选来源 | 回退来源 | 单位 | 统计意义 |
|------|----------|----------|------|----------|
| cpu_usage | `/dev/cpu_stat_monitor` | `/proc/stat` | % | 非空闲时间占比，整体负载 |
| cpu_iowait | 同上 | 同上 | % | CPU 等 IO 的时间，高说明磁盘/网络是瓶颈 |
| cpu_softirq | 同上 | 同上 | % | 软中断处理占用，高说明网络包处理压力大 |
| mem_usage | `/proc/meminfo` | — | % | `(MemTotal-MemAvailable)/MemTotal` |
| load_avg1/5/15 | `/proc/loadavg` | — | — | 1/5/15 分钟平均负载，反映排队压力 |
| disk_read/write_bps | `/proc/diskstats` | — | B/s | 物理盘读写吞吐 |
| disk_iops | 同上 | — | 次/s | 随机/顺序 IO 压力 |
| net_recv/send_bps | TC(clsact) | eBPF kprobe → `/proc/net/dev` | B/s | 网卡收发带宽 |
| softirq_rate | `/dev/softirq_monitor` | 不采集（0） | 次/s | 每秒软中断次数，网络/定时器压力 |
| 主机标识 | `host_info`（hostname/IP/machine-id/kernel/os） | — | — | 多节点身份识别与运维定位 |

## 6. 为什么用 `/proc`

优点：任何 Linux 都有、无需 root、格式稳定、调试直观；缺点是轮询、精度受采样间隔限制、看不到协议栈内部细节。

因此 `/proc` 在本项目里是**兜底层**：内核模块或 eBPF 不可用时保证系统仍能工作，而不是唯一方案。

## 7. 为什么做内核模块

- `/proc/stat` 统计的是全局汇总；内核模块可以直接读 `kcpustat` / `kstat`，拿到同样的累计计数，但链路更短、可扩展（例如未来加 per-CPU、per-软中断类型）
- 通过 misc 字符设备暴露，用户态只做 `read()`，实现简单
- 代价：需要匹配内核版本的内核树和 `Module.symvers`；WSL2 还要自己全量编译微软内核源码

**为什么不用 mmap**：本项目每次只传一行文本（几十字节），`read()` 的拷贝成本可以忽略；mmap 更适合高频大缓冲（如 ring buffer）。

## 8. 为什么用 eBPF，以及 kprobe / TC / XDP 怎么选

| 方案 | 挂载点 | 能看到什么 | 优点 | 代价 |
|------|--------|-----------|------|------|
| kprobe | 内核函数 `__dev_queue_xmit` / `__netif_receive_skb` | 协议栈收发函数每次调用 | 无驱动依赖、实现简单、能覆盖所有网卡 | 依赖内核符号名；统计的是函数调用而不是"网卡流量" |
| TC(clsact) | qdisc ingress / egress | 指定网卡进出方向 | 收发方向天然分离、贴近网卡、支持 veth/tunnel | 需要 root、内核 qdisc 支持、按网卡配置 |
| XDP | 驱动收包最早阶段 | 入方向原始包 | 性能最高、可在驱动层丢弃/重定向 | 依赖网卡/驱动、主要只能处理 ingress |

本项目的选择：

1. 默认 `net_mode=auto`：**TC → kprobe → `/proc`** 依次尝试
2. TC 只统计默认路由网卡（或配置 `tc_iface`），避免 loopback 噪声
3. kprobe 统计全协议栈收发，覆盖所有网卡，包括 loopback
4. 任一失败都保留 `/proc/net/dev`，agent 启动日志打印 `net collector: net-*`，口径可核对

TC 与 kprobe 都用 **PERCPU_ARRAY** 按 CPU 累计字节，用户态读取时用 `libbpf_num_possible_cpus()` 汇总，避免跨 CPU 原子操作和锁竞争。

## 9. CPU 100% 了怎么排查

用本项目的指标按顺序判断：

1. 看 `cpu_usage`：是整体打满还是某个节点异常
2. 看 `load_avg1/5/15`：高说明等 CPU 的任务在排队；CPU% 不高但 load 高，通常是 IO/锁等待
3. 看 `cpu_iowait`：高 → 磁盘 IO 或网络存储慢，CPU 在等 IO
4. 看 `cpu_softirq` / `softirq_rate`：高 → 网络收包/定时器压力，常见于大流量或小包风暴
5. 看 `net_recv_bps/net_send_bps`：是否与流量峰值对应（iperf3/压测）
6. 看 `disk_iops/disk_bps`：是否磁盘打满
7. 看告警：`cpu_jump` 突变告警能在曲线突变的第一时间提示
8. 对比多节点：`QueryBestNode` 排名可快速定位哪台机器最闲/最忙

## 10. gRPC 为什么，为什么用 client-streaming

- HTTP/2 多路复用 + 长连接，避免每秒一次短连接握手
- Protobuf 二进制体积小、有 schema、字段编号保证向后兼容
- 与 REST/JSON 相比：网络开销更低、类型更强；与 Thrift 相比：生态和 C++ 支持更成熟
- 上报用 **client-streaming**：agent 一直发、服务端 `while (reader->Read())` 边收边处理，流结束才回 `{ok, received}`；比一元 RPC 少大量建连/收尾开销
- 查询用一元 RPC：请求-响应语义天然匹配

## 11. Protobuf 对比 XML / JSON

| 维度 | Protobuf | JSON | XML |
|------|----------|------|-----|
| 体积 | 小（二进制、无字段名） | 中 | 大 |
| 解析速度 | 快 | 中 | 慢 |
| 类型/校验 | 强类型 + 编译期生成 | 弱类型 | 弱类型（可 XSD） |
| 兼容性 | 字段编号可向后兼容 | 靠约定 | 靠约定 |
| 可读性 | 需要工具解码 | 人可读 | 人可读 |

监控数据每秒都在传，体积与解析开销优先，所以选 Protobuf。

## 12. 主机标识为什么重要

最初只有配置里的 `node_id`，多节点部署容易重复或写错。现在 agent 每条上报流的首条消息携带 `HostInfo`：

- `machine_id`：操作系统唯一标识，换 `node_id` 也能认回同一台机器
- `hostname` / `ip`：运维定位与查询结果直接可读
- `kernel` / `os`：排查环境差异（例如 eBPF 挂不上是内核问题还是权限问题）
- `boot_time_ms`：判断是否重启过（配合计数器归零判断）

manager 把主机信息写入 `hosts` 表（`node_id` 主键），并提供 `QueryHost` RPC。

## 13. 关键设计与踩坑

### 13.1 MySQL 句柄非线程安全

多 agent 并发上报时，多个线程同时使用同一个 `MYSQL*` 句柄，曾触发 libmysqlclient 内部 SSL 结构 double-free（ASan 定位）。修复：

- `insert` 全程持锁，`ensure_connected` 与 prepared statement 操作串行
- 连接/读写超时 3s，避免阻塞上报线程
- 掉线后 5s 节流重连，避免每秒刷日志

可优化点：当前每条采样都 `mysql_stmt_init/prepare/close` 一次，后续可复用 statement。

### 13.2 gRPC ClientContext 不可复用

一次 RPC 一个 `ClientContext`；复用一个 context 会触发 gRPC 断言崩溃。上报线程每次重连都新建 context。

### 13.3 计数器回绕

热插拔/重启/虚拟盘重建会让 `/proc/diskstats`、eBPF map 的累计值归零；`uint64_t` 相减会回绕成天文数字。磁盘与 TC 采集器都加了"当前值小于上次值 → 重建基线、本周期跳过"的保护。

### 13.4 磁盘口径：分区双算与漏盘

最初用前缀匹配 `sd/nvme/vd/hd`：

- `sda1`、`nvme0n1p1` 也匹配 → 整盘 + 分区重复累加，约 2 倍
- `xvda`（AWS Xen）不以 `vd` 开头、`mmcblk`（eMMC）不在白名单 → 恒为 0

改为 `/sys/block/<name>/device` 判定：只有真实整盘才有 `device` 软链；分区不在 `/sys/block` 下，loop/ram/zram/dm/md 没有 `device`。启动日志会打印实际统计的盘列表，便于在云上核对。

### 13.5 WSL2 的口径问题

WSL2 里的 `/proc/diskstats` 是虚拟机虚拟盘（sda/sdb/sdc/sdd），不是 Windows 主机物理盘；`/mnt/c` 是 9p 挂载，没有块设备。所以 agent 在 WSL2 里只能监控 WSL 自身；要监控宿主机，agent 必须跑在 Windows 侧或真实 Linux 主机上。

### 13.6 MySQL 存时序指标是权宜之计

生产级时序数据建议 InfluxDB/Prometheus/ClickHouse；本项目的查询层已把存储实现隔离在 `MysqlStorage`，替换存储不影响 QueryService 与上层。`init_db.sql` 留了按时间分区/归档的 TODO。

## 14. 常见面试追问

**Q1：和 Prometheus 有什么区别？**
Prometheus 是拉模式 + 成熟生态；本项目是推模式 + 自研采集（内核模块/eBPF），重点在"采集原理与全链路实现"，不是替代 Prometheus。

**Q2：为什么不用 OpenTelemetry？**
项目目标是理解监控系统每一层（采集、传输、存储、查询、告警、调度），引入现成 SDK 会掩盖这些细节；生产环境当然可以对接 OTel。

**Q3：1 秒采集一次够吗？**
对服务器级监控足够；高频场景（毫秒级）需要环形缓冲/采样聚合，当前 gRPC 逐条 Write 也会成为瓶颈（代码里预留了攒批演进点）。

**Q4：数据量怎么估算？**
单节点 1 条/s，一条约 100 字节量级；1 节点 1 天 86400 行。节点数上千时 MySQL 单表会吃紧，需要分区/归档或换时序库。

**Q5：为什么把告警放 manager？**
阈值与突变需要跨节点统一视角和集中配置；放 agent 侧会重复配置且难以统一调整。

**Q6：调度打分的局限？**
只用最新一条采样、权重是启发式、参考上限（5000 IOPS、200MB/s）是硬编码；没有反馈闭环（不知道调度后实际效果）。

**Q7：流断了会丢数据吗？**
队列中的采样会保留，正在 `Write` 失败的那一条会丢失；没有本地磁盘缓冲，这是监控场景可接受的取舍。

**Q8：多节点时钟不同步怎么办？**
当前信任 agent 的系统时间；生产可改用 manager 接收时间或要求 NTP，并在存储层加 `received_at` 字段。

**Q9：为什么用终端面板而不是 Web 前端？**
部署目标是 Linux 服务器，运维直接 SSH 登录；终端面板零依赖（无浏览器、无 CDN、无前端构建），`--once` 还能输出到日志。生产环境需要图表时再对接 Grafana，不需要自己维护 Web 前端。

**Q10：如果重做会怎么改？**
存储换时序库并加分区；上报攒批 + 背压；采集配置热更新；告警规则可配置化；eBPF 增加 TC/XDP 的自动探测与多网卡支持。

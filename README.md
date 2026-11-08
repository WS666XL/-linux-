# DistMonitor — 分布式系统性能监控分析平台

> 基于 C++20 从零自研的分布式服务器监控系统：三级采集源（内核模块 / eBPF（TC、kprobe）/ /proc 自动降级）、gRPC 流式上报、阈值与突变告警、按业务场景加权调度、MySQL 持久化、gRPC 查询服务、终端监控面板、Docker 一键部署。
>
> 设计取舍与面试问答见 [docs/design-qa.md](docs/design-qa.md)。
>
> **快速开始**：`cmake -B build && cmake --build build -j$(nproc)` → 启动 manager / agent → `./build/monitor_tui 127.0.0.1:50051`。完整步骤见 [9. 编译与部署](#9-编译与部署)。

## 1. 项目概述

### 技术栈

| 层级 | 技术 |
|------|------|
| 语言 | C++20（agent / manager / common 全部手写） |
| RPC | gRPC + Protobuf（client-streaming 上报，双 service 同端口） |
| 展示 | 自研 ANSI 终端面板（monitor_tui，无第三方 TUI 库/前端） |
| 采集 | 内核模块（kcpustat / kstat → misc 字符设备）+ eBPF（TC clsact / kprobe，CO-RE）+ /proc 解析 |
| 并发模型 | std::thread / mutex / atomic + 自研 BlockingQueue（生产者-消费者） |
| 存储 | MySQL 8.0（libmysqlclient，预处理语句写入） |
| 构建 | CMake 3.16+ / g++（C++20）/ clang（编 eBPF 目标文件） |
| 部署 | Docker Compose（MySQL + manager + 2 个 agent 一键编排） |
| 测试 | 自研极简测试框架，23 个单元用例（ctest 一键运行） |

### 核心功能

- **三级采集源，自动降级**：CPU 优先内核模块字符设备（`/dev/cpu_stat_monitor`）；网络按 `net_mode=auto` 依次尝试 TC(clsact，方向明确) → eBPF kprobe（`__dev_queue_xmit` / `__netif_receive_skb`，percpu map 无锁累计）→ `/proc/net/dev`，任一不可用自动回退，agent 启动日志标明当前采集器
- **主机标识上报**：agent 随每条流的首条消息发送 hostname / 默认路由网卡 IP / machine-id / 内核版本 / 发行版 / 启动时间，manager 落 `hosts` 表并提供 `QueryHost` 查询
- **系统负载采集**：读取 `/proc/loadavg` 的 1/5/15 分钟平均负载，用于区分"CPU 忙"与"系统排队严重"
- **gRPC client-streaming 上报**：agent 按 `interval_ms` 采样，流式逐条推送，Protobuf 序列化，断流 3s 自动重连
- **异常检测**：CPU/内存阈值告警 + 相邻采样 CPU 突变告警（取绝对值），告警回调可注入（为可测性与后续通知渠道预留）
- **加权调度**：按 cpu_bound / io_bound / net_bound 业务场景加权打分（剩余能力百分比模型），返回最优节点排名
- **查询服务**：gRPC `QueryService` 提供最新快照 / 历史区间 / 窗口趋势 / 主机标识四个查询接口
- **终端监控面板**：`monitor_tui` 通过 gRPC 轮询 `QueryBestNode` / `QueryHost`，在终端整屏刷新 CPU、内存、磁盘、网络与调度分数，支持 `--once` 单帧输出
- **测试与压测**：自研极简测试框架 23 个用例（配置 / 告警 / 调度 / 阻塞队列），并有 stress、iperf3 实测数据

---

## 2. 系统架构

### 2.1 整体架构图

```text
┌────────────────── agent（每节点一个，无监听端口） ──────────────────┐
│  采集层（每个采集器只填 MetricsSample 中属于自己的字段）             │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │ CpuCollector      /proc/stat                  ← 回退          │  │
│  │ KmodCpuCollector  /dev/cpu_stat_monitor       ← 优先          │  │
│  │ SoftirqCollector  /dev/softirq_monitor（仅模块加载后启用）     │  │
│  │ MemCollector      /proc/meminfo                              │  │
│  │ LoadCollector     /proc/loadavg（1/5/15 分钟）                │  │
│  │ DiskCollector     /proc/diskstats                            │  │
│  │ NetCollector      /proc/net/dev                ← 兜底         │  │
│  │ EbpfNetCollector  kprobe + CO-RE（percpu map）← 次选         │  │
│  │ TcNetCollector    clsact ingress/egress       ← 首选         │  │
│  └───────────────────────────┬──────────────────────────────────┘  │
│              每 interval_ms 采一次 → 组装 MetricsSample             │
│                              ↓                                      │
│              GrpcReporter（BlockingQueue + 独立上报线程）           │
└──────────────────────────────┼──────────────────────────────────────┘
                               │ gRPC client-streaming
                               ▼
┌──────────────── manager（单进程，双 service 同端口） ───────────────┐
│  MonitorService :50051                                              │
│    UploadMetrics ──► WeightedScheduler.update（最新采样缓存）       │
│                  ──► AlertEngine.check（阈值 / 突变告警）           │
│                  ──► MysqlStorage.insert（预处理语句落库）          │
│    QueryBestNode ──► WeightedScheduler.rank（按场景加权排名）       │
│  QueryService :50051                                                │
│    QueryLatest / History / Trend / Host ──► MysqlStorage           │
└──────────────────────────────┬──────────────────────────────────────┘
                               ▼
                     MySQL monitor_db.metrics + hosts
```

### 2.2 请求处理流程

```text
agent 主循环（agent/main.cpp）
  ① sleep(interval_ms)
  ② 填 node_id / timestamp_ms
  ③ 依次采集：cpu（kmod→/proc）→ mem → disk → softirq（可选）→ net（TC→eBPF→/proc）
  ④ reporter.submit(sample) 入阻塞队列

上报线程（GrpcReporter::run）
  ⑤ 建流 UploadMetrics（一次调用一个 ClientContext，不复用）
  ⑥ 阻塞 pop 队列 → writer->Write(req) 逐条写
  ⑦ 流断开 → Finish 判错 → 打印原因 → 3s 后重建流（进程内自动重连）

manager 接收（MonitorService::UploadMetrics）
  ⑧ reader->Read 循环，每条采样依次：scheduler.update → alert.check → storage.insert
  ⑨ 流结束回 UploadResponse{ok, received}
```

**架构分层逻辑**：采集与上报在 agent 侧闭环，manager 只做"收、判、存、查"；调度与告警依赖内存中的最新采样（毫秒级生效），历史查询依赖 MySQL（持久化）；MySQL 关闭时整条链路仍可运行，只是不落库、历史查询无数据。

---

## 3. 项目目录结构

```text
DistMonitor/
├── README.md                   # 本文档
├── docs/design-qa.md           # 设计取舍 / 面试问答
├── CMakeLists.txt              # 单工程构建（common/agent/manager/tools/tests）
├── agent/                      # 采集端（每节点一个进程）
│   ├── main.cpp                #   入口：采集器选择 + 采样循环 + 信号处理
│   ├── host_info.*             #   主机标识采集（hostname/IP/machine-id/kernel/os）
│   ├── collector/
│   │   ├── icollector.h        #   采集器接口：name() / collect(MetricsSample*)
│   │   ├── cpu_collector.*     #   /proc/stat 差值计算 CPU 使用率
│   │   ├── kmod_collector.*    #   /dev/cpu_stat_monitor 与 /dev/softirq_monitor
│   │   ├── mem_collector.*     #   /proc/meminfo（MemTotal/MemAvailable）
│   │   ├── load_collector.*    #   /proc/loadavg（1/5/15 分钟负载）
│   │   ├── disk_collector.*    #   /proc/diskstats（物理盘扇区/IO 计数）
│   │   └── net_collector.*     #   /proc/net/dev（排除 lo）
│   ├── bpf/
│   │   ├── net_monitor.bpf.c   #   eBPF 程序：kprobe + percpu array 按字节累计
│   │   ├── net_tc.bpf.c        #   eBPF 程序：TC clsact ingress/egress 分类器
│   │   ├── vmlinux.h           #   CO-RE 类型定义（免内核头文件依赖）
│   │   ├── ebpf_net_loader.*   #   libbpf 加载、挂 kprobe、汇总各 CPU 计数
│   │   ├── ebpf_net_collector.*#   适配 ICollector：差值折算 B/s
│   │   └── tc_net_collector.*  #   TC 采集器：clsact 挂载 + 按网卡统计
│   ├── kmod/
│   │   ├── cpu_stat_monitor.c  #   内核模块：kcpustat 聚合 → misc 字符设备
│   │   ├── softirq_monitor.c   #   内核模块：kstat 软中断计数 → misc 字符设备
│   │   └── Makefile            #   外部模块构建（KDIR 指向内核树）
│   └── reporter/
│       └── grpc_reporter.*     #   gRPC 流式上报线程 + 阻塞队列
├── manager/                    # 管理端（单进程：gRPC 双 service）
│   ├── main.cpp                #   入口：配置装配、服务注册
│   ├── service/
│   │   ├── monitor_service.*   #   MonitorService：上报流 / 主机注册 / 最优节点
│   │   └── query_service.*     #   QueryService：快照 / 历史 / 趋势 / 主机信息
│   ├── alert/
│   │   └── alert_engine.*      #   阈值 + 突变告警，on_alert 回调注入
│   ├── scheduler/
│   │   └── weighted_scheduler.*#   场景加权打分与排名
│   └── storage/
│       └── mysql_storage.*     #   metrics/hosts 落库 + 查询
├── common/
│   ├── include/common/
│   │   ├── config.h            #   INI 配置解析（section/key，类型转换）
│   │   ├── logger.h            #   时间戳分级日志（stderr）
│   │   └── blocking_queue.h    #   线程安全阻塞队列（agent 采集→上报）
│   └── src/config.cpp
├── proto/monitor.proto         #   MetricsSample + 双 service 定义
├── configs/
│   ├── agent.conf              #   node_id / manager_addr / interval_ms / bpf_object
│   └── manager.conf            #   监听地址 / MySQL / 告警阈值 / 调度权重
├── docker/
│   ├── Dockerfile.agent        #   Ubuntu 24.04 全量依赖构建（关 eBPF 编译）
│   ├── Dockerfile.manager
│   └── docker-compose.yml      #   mysql + manager + agent1/agent2
├── scripts/init_db.sql         #   建库建表 + (node_id, ts) 索引
├── tests/                      #   自研框架单元测试（4 文件 23 用例）
└── tools/
    ├── query_client.cpp         #   QueryService 四个接口验证工具
    └── monitor_tui.cpp          #   终端监控面板（ANSI 整屏刷新）
```

---

## 4. 服务与模块详解

### 4.1 端口分配

| 端口 | 服务 | 说明 |
|------|------|------|
| 50051 | manager gRPC | MonitorService + QueryService 共用监听 |
| 3306 | MySQL | 本地默认端口（配置可改） |
| 3307 | MySQL（Docker） | compose 将容器 3306 映射到宿主机 3307 |
| — | agent | 只作为 gRPC 客户端，不监听端口 |

### 4.2 agent 模块（agent/）

| 文件 | 职责 | 关键设计 |
|------|------|----------|
| main.cpp | 采集器选择 + 采样循环 | 启动时探测设备/加载 eBPF/TC 决定实现；SIGINT/SIGTERM 优雅退出 |
| host_info.* | 主机静态标识 | hostname / 默认路由网卡 IP / machine-id / 内核版本 / 发行版 / 启动时间 |
| collector/icollector.h | 采集器抽象 | 每个采集器只填自己的字段，互不覆盖 |
| collector/cpu_collector.* | CPU 使用率 | `/proc/stat` 两次快照差值：`100*(Δtotal-Δidle)/Δtotal` |
| collector/kmod_collector.* | 内核模块采集 | 逻辑与 `/proc` 版一致，源换成 `/dev/cpu_stat_monitor` |
| collector/mem_collector.* | 内存使用率 | `100*(MemTotal-MemAvailable)/MemTotal` |
| collector/load_collector.* | 系统负载 | `/proc/loadavg` 前三个数：1/5/15 分钟平均负载 |
| collector/disk_collector.* | 磁盘吞吐/IOPS | 只统计整盘（`/sys/block/<name>/device` 判定，排除分区与 loop/ram/dm/md）；扇区按 512B 折算，steady_clock 算 dt，计数器归零自动重建基线 |
| collector/net_collector.* | 网络收发 | `/proc/net/dev` 收字节第 1 列、发字节第 9 列，跳过 `lo` |
| bpf/ebpf_net_collector.* | eBPF 网络采集 | 加载/挂载失败打印告警并回退；按 CPU 汇总 percpu 计数后折算 B/s |
| bpf/tc_net_collector.* | TC 网络采集 | clsact ingress/egress + SCHED_CLS 程序；只统计指定网卡，退出时 detach |
| reporter/grpc_reporter.* | 上报线程 | BlockingQueue 解耦采样与网络 IO；断流 3s 重连；每条新流首条携带 HostInfo |

### 4.3 manager 模块（manager/）

| 文件 | 职责 | 关键设计 |
|------|------|----------|
| main.cpp | 进程装配 | 配置读取 → 存储/告警/调度构造 → 双 service 注册 |
| service/monitor_service.* | 上报流 + 调度 RPC | 首条消息注册主机标识；每条采样先更新调度缓存再告警、落库；流结束返回累计条数 |
| service/query_service.* | 四个查询 RPC | 参数校验（空 node_id → INVALID_ARGUMENT），带默认区间/窗口；主机信息无记录返回 NOT_FOUND |
| alert/alert_engine.* | 告警判定 | 每节点独立维护上次采样；阈值严格大于；突变取绝对值 |
| scheduler/weighted_scheduler.* | 加权打分 | 剩余能力百分比模型；场景预设权重；分数降序 |
| storage/mysql_storage.* | 持久化与查询 | metrics 预处理语句写入 + hosts 主机标识 upsert/查询；全程持锁；ping 探活 + 5s 节流重连；3s 超时 |

### 4.4 指标上报流程

```text
① agent 启动：探测 /dev/cpu_stat_monitor → 有则 KmodCpuCollector，否则 CpuCollector
              探测 /dev/softirq_monitor → 有则启用 SoftirqCollector
              网络按 net_mode 依次尝试 TcNetCollector → EbpfNetCollector → NetCollector
② 每个 interval_ms：填 node_id / timestamp_ms → 依次 collect → reporter.submit
③ 上报线程建流 UploadMetrics，首条消息附带 HostInfo（重连后重发），之后 queue.pop() 阻塞取采样，逐条 writer->Write
④ 服务端 reader->Read 收一条处理一条：调度缓存 → 告警 → MySQL
⑤ 流结束：WritesDone + Finish；失败则 3s 后重建流（断流期间采样继续入队等待发送）
```

### 4.5 加权调度流程

```text
QueryBestNode(scenario)
  → 遍历内存中每节点最新采样
  → 按场景取权重（cpu_bound / io_bound / net_bound / 默认）
  → 计算剩余能力分（0~100）：score = Σ w_i × capacity_i
  → 按 score 降序返回 {node_id, score, latest}
```

### 4.6 查询服务流程

| RPC | 数据来源 | 参数约定 | 返回/错误 |
|------|----------|----------|-----------|
| QueryLatest | `metrics` 按 ts 倒序 LIMIT 1 | node_id 必填 | 无数据 → NOT_FOUND |
| QueryHistory | 时间区间升序 | `end<=0` 取当前；`start<=0` 取 end-300s；`limit<=0` 取 600（上限 10000） | 空区间 → 空列表 OK |
| QueryTrend | 最近 N 秒聚合 | `window_sec<=0` 取 300（上限 86400） | 存储不可用 → UNAVAILABLE |
| QueryHost | `hosts` 按 node_id 查询 | node_id 必填 | 无记录 → NOT_FOUND |

### 4.7 终端监控面板（tools/monitor_tui）

| 参数 | 说明 |
|------|------|
| `manager_addr` | manager 地址，默认 `127.0.0.1:50051` |
| `--interval=N` | 刷新间隔（秒），默认 1，范围 1~60 |
| `--once` | 只输出一帧后退出（非 TTY 环境自动启用） |
| `--no-color` | 关闭 ANSI 颜色 |

- 数据来源：`QueryBestNode`（全部节点最新采样 + 调度分数）+ `QueryHost`（主机标识，查询失败每 30s 重试）
- 渲染方式：备用屏幕 + 隐藏光标，`Ctrl+C` 退出时自动恢复；MySQL 风格边框，CPU/内存/分数按 60%/90% 阈值变色
- 表格列：`NODE / CPU% / MEM% / LOAD1 / LOAD5 / LOAD15 / DISK-R / DISK-W / NET-RX / NET-TX / SCORE / TIME`
- 表格下方补充：`host=` 主机标识、`iowait`、`softirq`、`iops`
- 不依赖 Web 前端、浏览器、CDN，SSH 登录服务器即可查看

示例输出：

```text
DistMonitor  |  manager 127.0.0.1:50051  |  2026-11-08 18:13:47  |  1 node(s)  |  avg CPU 1.5%  |  best node-1 (86.9)
+------------+------+------+--------+--------+--------+-----------+-----------+-----------+-----------+-------+--------+
|NODE        |  CPU%|  MEM%|   LOAD1|   LOAD5|  LOAD15|     DISK-R|     DISK-W|     NET-RX|     NET-TX|  SCORE|    TIME|
+------------+------+------+--------+--------+--------+-----------+-----------+-----------+-----------+-------+--------+
|node-1      |   1.5|  41.1|    0.71|    0.38|    0.27|      0 B/s|      0 B/s|     66 B/s|     66 B/s|   86.9|18:13:46|
+------------+------+------+--------+--------+--------+-----------+-----------+-----------+-----------+-------+--------+
  node-1       host=-  iowait=0.0%  softirq=0.5%  iops=0
```

---

## 5. 三级采集实现

### 5.1 采集源选择矩阵

| 指标 | 优先来源 | 回退来源 | 触发条件 |
|------|----------|----------|----------|
| CPU（user/system/idle/iowait/softirq…） | `/dev/cpu_stat_monitor` 内核模块 | `/proc/stat` | 设备文件可打开 |
| 软中断速率 | `/dev/softirq_monitor` 内核模块 | 不采集（该字段为 0） | 设备文件可打开 |
| 内存 | `/proc/meminfo` | — | 始终可用 |
| 系统负载（1/5/15 分钟） | `/proc/loadavg` | — | 始终可用 |
| 磁盘吞吐 / IOPS | `/proc/diskstats` | — | 始终可用 |
| 网络收发 | TC(clsact，percpu map) | eBPF kprobe → `/proc/net/dev` | TC 挂载成功（需 root）；失败按 `net_mode` 依次回退 |

### 5.2 内核模块

| 模块 | 设备 | 数据来源 | 输出格式 |
|------|------|----------|----------|
| cpu_stat_monitor | `/dev/cpu_stat_monitor`（0444） | `kcpustat_cpu_fetch` 遍历在线 CPU | 一行 8 个累计值：user nice system idle iowait irq softirq steal |
| softirq_monitor | `/dev/softirq_monitor`（0444） | `kstat_cpu(cpu).softirqs[]` 遍历在线 CPU | 一行 11 个累计值：total + HI/TIMER/NET_TX/NET_RX/BLOCK/IRQ_POLL/TASKLET/SCHED/HRTIMER/RCU |

均为单次读语义（`*off > 0` 直接返回 0）；agent 用两次读数的差值折算，CPU 按总增量归一化，软中断按采样间隔（默认 1s）取差值。

### 5.3 eBPF（CO-RE）

```text
net_monitor.bpf.c
  map: BPF_MAP_TYPE_PERCPU_ARRAY, key 0=RX / 1=TX, value u64 累计字节
  kprobe __dev_queue_xmit   → 发方向累加 BPF_CORE_READ(skb, len)
  kprobe __netif_receive_skb → 收方向累加（所有收包必经路径）
加载：libbpf strict mode → open/load → 找 map/program → attach kprobe
读取：libbpf_num_possible_cpus() 逐 CPU 汇总，避免跨 CPU 锁竞争
```

`vmlinux.h` 提供内核类型定义，程序与具体内核版本解耦（CO-RE 重定位），无需按内核树编译。

### 5.4 TC（clsact）

```text
net_tc.bpf.c（SEC("classifier")，BPF_PROG_TYPE_SCHED_CLS）
  ingress 程序 tc_rx → map key 0 累加 skb->len
  egress  程序 tc_tx → map key 1 累加 skb->len
挂载：libbpf 加载对象 → bpf_tc_hook_create(ifindex, clsact)
      → bpf_tc_attach(ingress/egress，固定 handle/priority)
读取：percpu 计数按 CPU 汇总 → 两次读数差值折算 B/s
```

- 只统计 `tc_iface`（留空取默认路由网卡），避免 loopback 与无关网卡噪声
- 与 kprobe 的分工：TC 在 qdisc 层、方向明确；kprobe 在协议栈函数、覆盖所有网卡
- 退出时 detach 过滤器；若 clsact qdisc 是本次创建的，则一并销毁
- 计数器归零（重挂/网卡重建）时重建基线，避免无符号相减回绕

### 5.5 /proc 回退实现要点

- CPU：`/proc/stat` 首行 `cpu` 8 个累计 jiffies，差值算使用率（total 含 iowait/irq/softirq/steal）
- 内存：`MemTotal` 与 `MemAvailable`（必须消费掉数值后的 `kB` 单位再继续读）
- 磁盘：`/proc/diskstats` 只取整盘（`/sys/block/<name>/device` 存在即真实整盘，自动排除分区与 loop/ram/dm/md 虚拟设备），扇区 ×512B 折算字节，dt 用 `steady_clock`；计数器归零（热插拔/重启）时重建基线、本周期跳过
- 网络：`/proc/net/dev` 跳过 `lo`，收字节取第 1 列、发字节取第 9 列，多网卡累加

---

## 6. 数据库设计

数据库名：`monitor_db`。

### 6.1 表结构

#### `metrics` — 指标采样表（manager 只写、查询服务只读）

| 字段 | 类型 | 说明 |
|------|------|------|
| id | BIGINT UNSIGNED AUTO_INCREMENT PK | 行 ID |
| node_id | VARCHAR(64) | 节点标识（agent 配置或 `NODE_ID` 环境变量） |
| ts | DATETIME(3) | 采样时间（由毫秒时间戳 `FROM_UNIXTIME(?/1000)` 转换，毫秒精度） |
| cpu_usage | DOUBLE | CPU 使用率 % |
| cpu_iowait | DOUBLE | iowait 占比 % |
| cpu_softirq | DOUBLE | softirq 占比 % |
| mem_usage | DOUBLE | 内存使用率 % |
| disk_read_bps | DOUBLE | 读速率 B/s |
| disk_write_bps | DOUBLE | 写速率 B/s |
| disk_iops | DOUBLE | 每秒 IO 次数 |
| net_recv_bps | DOUBLE | 收速率 B/s |
| net_send_bps | DOUBLE | 发速率 B/s |
| softirq_rate | DOUBLE | 每秒软中断触发次数（仅内核模块源非 0） |
| load_avg1 | DOUBLE | 1 分钟平均负载（`/proc/loadavg`） |
| load_avg5 | DOUBLE | 5 分钟平均负载 |
| load_avg15 | DOUBLE | 15 分钟平均负载 |

索引：`INDEX idx_node_ts (node_id, ts)`，支撑"单节点最新一条 / 区间历史 / 窗口聚合"三类查询。

#### `hosts` — 主机标识表（node_id 主键，每个 agent 一行）

| 字段 | 类型 | 说明 |
|------|------|------|
| node_id | VARCHAR(64) PK | 节点标识，与 metrics.node_id 对应 |
| hostname | VARCHAR(255) | 主机名 |
| ip | VARCHAR(64) | 默认路由网卡 IPv4 |
| machine_id | VARCHAR(64) | `/etc/machine-id` |
| kernel | VARCHAR(128) | 内核版本（uname -r） |
| os | VARCHAR(255) | 发行版名称（PRETTY_NAME） |
| boot_time_ms | BIGINT | 系统启动时间（毫秒时间戳） |
| updated_at | DATETIME(3) | 最近一次上报时间 |

### 6.2 表关系与内存结构

```text
metrics（时序事实表）        hosts（主机标识表）
node_id ──1:N──→ 采样行      node_id ──1:1──→ 主机信息行
        └─────── 逻辑关联（无数据库外键）───────┘

manager 进程内缓存（非持久化，重启即丢，可由后续上报重建）：
  WeightedScheduler.latest_ : node_id → 最新 MetricsSample   （调度打分）
  AlertEngine.last_         : node_id → 上次 MetricsSample   （突变判定基线）
```

- **为什么缓存最新采样**：调度与告警要求毫秒级响应，不能每次回查 MySQL
- **为什么历史进 MySQL**：QueryHistory/Trend 需要跨进程、跨重启的时序数据
- 下一步规划：按时间分区或定期归档（`scripts/init_db.sql` 已留 TODO）

---

## 7. 告警与调度

### 7.1 告警规则

| 规则 | 条件 | 事件 kind | 说明 |
|------|------|-----------|------|
| CPU 超阈值 | `cpu_usage > cpu_max` | `cpu_max` | 严格大于，等于阈值不告警 |
| 内存超阈值 | `mem_usage > mem_max` | `mem_max` | 默认 90% |
| CPU 突变 | `abs(cur.cpu - prev.cpu) > cpu_jump` | `cpu_jump` | 取绝对值，骤降同样告警；默认 30% |

- **必须有基线**：同一 node 的第二条采样起才判定（首条只建立基线，即使 CPU 100% 也不告警）
- **节点隔离**：`last_` 按 node_id 分别维护，互不干扰
- **通知方式**：默认仅打印 WARN 日志；`on_alert` 回调可注入测试捕获或后续接短信/邮件/IM

### 7.2 调度打分模型

每个维度先折算"剩余能力"百分比（0~100），再加权求和：

```text
cpu_cap  = max(0, 100 - cpu_usage)
mem_cap  = max(0, 100 - mem_usage)
disk_cap = 100 × (1 - min(disk_iops / 5000, 1))          # 5000 IOPS 视为打满
net_cap  = 100 × (1 - min((net_recv_bps + net_send_bps) / 200MB/s, 1))

score = w_cpu×cpu_cap + w_mem×mem_cap + w_disk×disk_cap + w_net×net_cap
```

| 场景 | w_cpu | w_mem | w_net | w_disk | 适用 |
|------|-------|-------|-------|--------|------|
| 默认（配置） | 0.5 | 0.3 | 0.1 | 0.1 | 未指定 scenario |
| cpu_bound | 0.7 | 0.2 | 0.05 | 0.05 | 计算密集任务 |
| io_bound | 0.2 | 0.1 | 0.1 | 0.6 | 磁盘密集任务 |
| net_bound | 0.2 | 0.1 | 0.6 | 0.1 | 网络密集任务 |

返回按 score 降序的节点排名，仅包含"有上报记录"的节点（空缓存返回空列表）。

### 7.3 可测性设计

- 告警引擎通过 `std::function<void(const Event&)> on_alert` 注入回调，测试直接捕获事件而不解析日志
- 调度器构造时注入默认权重，单元测试可构造多节点采样验证排序
- MySQL 支持 `enable=false`，所有写接口直接返回 true，无库环境也能跑通整条链路

---

## 8. 测试与压测

### 8.1 单元测试（23 个用例）

| 文件 | 用例数 | 覆盖点 |
|------|--------|--------|
| tests/test_config.cpp | 5 | 文件缺失、注释/空白/等号两侧空格、类型转换、默认值、bool 变体、非法行忽略 |
| tests/test_alert.cpp | 6 | 首采样无基线不告警、CPU/内存阈值、边界严格大于、突变取绝对值、节点隔离 |
| tests/test_scheduler.cpp | 8 | 空集、分值范围、cpu/io/net 三种场景偏好、默认权重手算校验、最新采样覆盖、降序与稳定性 |
| tests/test_blocking_queue.cpp | 4 | FIFO、close 后排空再返回 nullopt、多线程 1000 条交接、pop 阻塞唤醒 |

框架零第三方依赖：`TEST(name)` 静态注册 + `CHECK` / `CHECK_NEAR` 断言，`ctest` 一键执行。

### 8.2 压测验证数据

| 场景 | 理论值 | 系统采集值 |
|------|--------|-----------|
| stress 4 核压满（6 核机） | 66.7% | 68~71% |
| iperf3 60Gbps 回环（CPU） | ~100% | 95% |
| iperf3 网络带宽（eBPF 采集） | 60 Gbps | 7.0~7.5 GB/s 双向对称，无丢数 |
| agent 自身开销 | — | 常驻内存 ~4MB，CPU < 0.5% |

---

## 9. 编译与部署

### 9.1 环境要求

- Linux（WSL2 可用）、MySQL 8.0
- CMake 3.16+、g++（C++20）、protobuf + gRPC（含 `grpc_cpp_plugin`）
- libmysqlclient（`mysql.h` + `libmysqlclient`）
- libbpf（eBPF 加载）、clang（编译 `net_monitor.bpf.o`）
- 内核模块编译需匹配运行内核的构建树（含 `Module.symvers`）
- Docker / Docker Compose（可选，一键部署）

### 9.2 本地构建

```bash
# 配置 + 编译（-DENABLE_EBPF_OBJ=OFF 可跳过 eBPF 目标文件，运行时自动回退 /proc）
LD_LIBRARY_PATH= cmake -B build
LD_LIBRARY_PATH= cmake --build build -j$(nproc)

# manager（configs/manager.conf 已开启 MySQL；缺省值为关闭，不落库也能跑）
LD_LIBRARY_PATH= ./build/manager configs/manager.conf

# agent（eBPF/内核模块需 root；-c 指定配置可同机起多节点）
sudo env LD_LIBRARY_PATH= ./build/agent -c configs/agent.conf
```

### 9.3 加载内核模块（可选，WSL2）

```bash
# 需要匹配运行内核的构建树（~/wsl-kernel，含 Module.symvers）
cd agent/kmod && make
sudo insmod cpu_stat_monitor.ko && sudo insmod softirq_monitor.ko
cat /dev/cpu_stat_monitor   # user nice system idle iowait irq softirq steal
cat /dev/softirq_monitor    # total + 10 类软中断计数
```

WSL2 内核头文件构建：克隆 `microsoft/WSL2-Linux-Kernel` 对应版本 tag → `zcat /proc/config.gz > .config` → 关闭 `CONFIG_IKHEADERS` → 全量 `make` 生成 `Module.symvers`。

### 9.4 Docker Compose 一键部署

```bash
git clone https://github.com/<you>/DistMonitor.git
cd DistMonitor
sudo docker compose -f docker/docker-compose.yml up -d
# mysql(宿主 3307) + manager(:50051 gRPC) + agent1/agent2
```

容器内无特权，eBPF/TC 与内核模块自动降级为 `/proc` 采集（agent 读的是容器自身 `/proc`，上生产需 host 网络或挂载宿主机 `/proc`）。

在容器里快速看一眼终端面板：

```bash
sudo docker exec -it $(sudo docker compose -f docker/docker-compose.yml ps -q manager) \
  /app/build/monitor_tui 127.0.0.1:50051 --once --no-color
```

### 9.5 验证命令

```bash
# 终端监控面板（实时刷新，Ctrl+C 退出）
./build/monitor_tui 127.0.0.1:50051

# 单帧快照（适合日志/脚本）
./build/monitor_tui 127.0.0.1:50051 --once --no-color

# QueryService 四个接口实测
./build/query_client 127.0.0.1:50051 node-2

# 落库数据（Docker 映射端口 3307）
mysql -h 127.0.0.1 -P 3307 -u monitor -pmonitor123 monitor_db \
  -e "SELECT node_id, ROUND(cpu_usage,1) cpu, ROUND(mem_usage,1) mem,
      ROUND(net_send_bps/1024,1) tx_KBps FROM metrics ORDER BY ts DESC LIMIT 10;"

# 单元测试
cd build && LD_LIBRARY_PATH= ctest
```

### 9.6 配置项

`configs/agent.conf`

| section | key | 默认 | 说明 |
|---------|-----|------|------|
| agent | node_id | node-1 | 节点标识（可被 `NODE_ID` 环境变量覆盖） |
| agent | manager_addr | 127.0.0.1:50051 | manager 地址（可被 `MANAGER_ADDR` 覆盖） |
| agent | interval_ms | 1000 | 采样间隔（毫秒） |
| agent | net_mode | auto | 网络采集链：auto(TC→eBPF→/proc) / tc / ebpf / proc |
| agent | bpf_object | build/bpf/net_monitor.bpf.o | eBPF kprobe 对象路径；空或加载失败回退 |
| agent | tc_object | build/bpf/net_tc.bpf.o | TC(clsact) 对象路径；需 root + 内核 qdisc 支持 |
| agent | tc_iface | 空 | TC 统计的网卡；空 = 默认路由网卡 |

`configs/manager.conf`

| section | key | 默认 | 说明 |
|---------|-----|------|------|
| manager | listen_addr | 0.0.0.0:50051 | gRPC 监听地址 |
| mysql | enable | false | 关闭时不落库，链路仍可跑通 |
| mysql | host / port / user / password / db | 127.0.0.1 / 3306 / root / 空 / monitor_db | `MYSQL_HOST` 环境变量可覆盖 host |
| alert | cpu_max / mem_max / cpu_jump | 90 / 90 / 30 | 告警阈值（%），比较为严格大于 |
| scheduler | w_cpu / w_mem / w_net / w_disk | 0.5 / 0.3 / 0.1 / 0.1 | 默认场景权重 |

### 9.7 监控云服务器：SSH 反向隧道（本机 manager + 云端 agent）

**适用场景**：manager 跑在本地 WSL / 内网机器，被监控的云服务器只有 NAT 公网地址，
且不想在安全组开放 50051。此时由本机主动 SSH 登录云服务器并建立反向隧道，
云端 agent 只需连接云服务器本机的回环地址。

```text
云服务器 agent
  → 127.0.0.1:50051（云服务器回环）
  → sshd 反向端口转发（走本机发起的 SSH 连接）
  → 本机 manager 127.0.0.1:50051
```

**为什么不用直连**：云服务器的公网 IP 是云厂商 NAT/EIP 映射，不在 `eth0` 上；
本机 WSL 也在 NAT 后面，云服务器无法主动连入。反向隧道由本机主动连出，
天然穿透两边 NAT，也不需要开放任何公网入站端口。

**前置条件**

- 本机 manager 已启动，监听 `0.0.0.0:50051`；
- 本机能 SSH 登录云服务器；
- 云服务器已按 9.1 / 9.2 装好依赖并编译出 `build/agent`。

**1. 本机启动 manager**

```bash
./scripts/start_all.sh --no-tui
```

**2. 本机建立反向隧道**

```bash
ssh -N -o ServerAliveInterval=30 -o ServerAliveCountMax=3 -o ExitOnForwardFailure=yes \
  -R 127.0.0.1:50051:127.0.0.1:50051 user@<云服务器公网IP>
```

- 终端保持打开，关闭即断；
- SSH 非 22 端口时加 `-p <port>`；
- sshd 默认 `GatewayPorts no`，云端只监听 `127.0.0.1:50051`，安全组无需放行 50051。

**3. 云服务器验证隧道**

```bash
ss -lntp | grep 50051
# LISTEN 0 128 127.0.0.1:50051 0.0.0.0:* users:(("sshd",pid=...,fd=...))
```

**4. 云服务器准备代码并编译**

在本机项目根目录打包上传：

```bash
tar -czf /tmp/distmonitor.tar.gz --exclude=build --exclude=.cache --exclude=.git .
scp /tmp/distmonitor.tar.gz user@<云服务器公网IP>:~/
```

在云服务器上解压、装依赖并编译：

```bash
mkdir -p ~/DistMonitor && tar -xzf ~/distmonitor.tar.gz -C ~/DistMonitor
cd ~/DistMonitor
sudo apt update
sudo apt install -y build-essential cmake pkg-config clang \
  libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
  libgrpc++-dev libmysqlclient-dev libbpf-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**5. 云服务器启动 agent**

```bash
cd ~/DistMonitor
sudo env NODE_ID=node-cloud MANAGER_ADDR=127.0.0.1:50051 LD_LIBRARY_PATH= \
  ./build/agent -c configs/agent.conf
```

- `NODE_ID` 必须与本机现有节点不同，否则调度缓存和 `hosts` 表会被覆盖；
- `MANAGER_ADDR` 填 `127.0.0.1:50051`，走反向隧道；
- root 运行可启用 TC / eBPF，失败会自动回退 `/proc`；
- 需要后台常驻时：

```bash
sudo setsid nohup env NODE_ID=node-cloud MANAGER_ADDR=127.0.0.1:50051 \
  ./build/agent -c configs/agent.conf > /tmp/agent-cloud.log 2>&1 &
```

正常启动日志（Ubuntu 22.04.5 云主机实测）：

```text
host: iZbp... ip=172.21.56.42 machine_id=... kernel=5.15.0-191-generic os=Ubuntu 22.04.5 LTS
tc: net monitor attached on eth0 (ingress+egress)
net collector: net-tc
agent node-cloud started, report to 127.0.0.1:50051 every 1000ms
disk collector: 1 physical disk(s): vda
```

**6. 本机验证**

```bash
grep "node-cloud" /tmp/distmonitor/manager.log | tail -1
./build/monitor_tui 127.0.0.1:50051
```

manager 日志应出现云端真实主机信息：

```text
service: host node=node-cloud hostname=iZbp... ip=172.21.56.42 kernel=5.15.0-191-generic os=Ubuntu 22.04.5 LTS
```

面板会同时显示本机与云端两个节点。在云服务器上做 CPU / 网络压测时，
只有 `node-cloud` 的指标变化，可用来确认监控对象不是本机。

**注意事项**

- 云端显示的 IP 是私网 IP（如 `172.21.56.42`）：公网 IP 是 NAT/EIP 映射，
  不在网卡上，agent 通过 `getifaddrs()` 读不到；用 hostname / machine-id / kernel 区分机器；
- `monitor_tui` 的 host 信息是进程内缓存，查到一次后不再刷新；
  如果节点先用旧主机信息上报过，重启面板即可显示新值；
- 隧道断开后 agent 不退出，gRPC 每 3 秒重连；重新执行第 2 步即可；
- 需要清掉同名节点的旧测试数据时：

```bash
mysql -h127.0.0.1 -umonitor -pmonitor123 monitor_db -e \
"DELETE FROM metrics WHERE node_id='node-cloud'; DELETE FROM hosts WHERE node_id='node-cloud';"
```

**排障**

| 现象 | 原因 | 处理 |
|------|------|------|
| 云端 `ss` 看不到 50051 | 隧道未建立或 sshd 禁止转发 | 检查第 2 步命令与 `AllowTcpForwarding` |
| manager 日志没有新 host | agent 未启动或 `MANAGER_ADDR` 写错 | 检查云端 agent 日志与端口 |
| 面板指标不变 | 测的是本机 agent，或 node_id 冲突 | 确认云端 `NODE_ID` 唯一，压测在云服务器执行 |
| host 仍显示 localhost | 面板缓存了旧值 | 重启 `monitor_tui` |

---

## 10. gRPC 接口汇总

### 10.1 MonitorService（manager :50051）

| RPC | 类型 | 说明 |
|-----|------|------|
| UploadMetrics | client-streaming | agent 持续推送 `MetricsSample`，每条即时告警+落库；流结束返回 `{ok, received}` |
| QueryBestNode | unary | 入参 `scenario`（cpu_bound/io_bound/net_bound/空），返回按分数降序的 `{node_id, score, latest}` |

### 10.2 QueryService（manager :50051）

| RPC | 类型 | 说明 | 错误码 |
|-----|------|------|--------|
| QueryLatest | unary | 单节点最新快照 | node_id 空 → INVALID_ARGUMENT；无数据 → NOT_FOUND |
| QueryHistory | unary | 时间区间历史（升序） | node_id 空 → INVALID_ARGUMENT |
| QueryTrend | unary | 窗口均值（cpu/mem/softirq/net 合计 + 样本数） | node_id 空 → INVALID_ARGUMENT；存储不可用 → UNAVAILABLE |
| QueryHost | unary | 节点主机标识（hostname/IP/machine-id/内核/发行版/启动时间） | node_id 空 → INVALID_ARGUMENT；无记录 → NOT_FOUND |

### 10.3 MetricsSample 字段

| 字段 | 类型 | 单位 | 来源 |
|------|------|------|------|
| node_id | string | — | agent 配置 / 环境变量 |
| timestamp_ms | int64 | 毫秒 | 采样时刻系统时间 |
| cpu_usage | double | % | kmod 或 `/proc/stat` |
| cpu_iowait | double | % | 同上 |
| cpu_softirq | double | % | 同上 |
| mem_usage | double | % | `/proc/meminfo` |
| disk_read_bps | double | B/s | `/proc/diskstats` |
| disk_write_bps | double | B/s | `/proc/diskstats` |
| disk_iops | double | 次/s | `/proc/diskstats` |
| net_recv_bps | double | B/s | TC / eBPF / `/proc/net/dev` |
| net_send_bps | double | B/s | TC / eBPF / `/proc/net/dev` |
| softirq_rate | double | 次/s | `/dev/softirq_monitor`（无模块时为 0） |
| load_avg1 | double | — | `/proc/loadavg` 1 分钟平均负载 |
| load_avg5 | double | — | `/proc/loadavg` 5 分钟平均负载 |
| load_avg15 | double | — | `/proc/loadavg` 15 分钟平均负载 |

### 10.4 HostInfo 字段（上报流首条携带）

| 字段 | 类型 | 说明 |
|------|------|------|
| hostname | string | `gethostname()` |
| ip | string | 默认路由网卡 IPv4（无默认路由时取第一个非回环地址） |
| machine_id | string | `/etc/machine-id`，缺失时读 `/var/lib/dbus/machine-id` |
| kernel | string | `uname -r` |
| os | string | `/etc/os-release` 的 `PRETTY_NAME` |
| boot_time_ms | int64 | `/proc/stat` 的 `btime` 转毫秒 |

---

## 11. 数据持久化

| 数据 | 存储位置 | 生命周期 | 说明 |
|------|----------|----------|------|
| 指标采样 | MySQL `monitor_db.metrics` | 持久 | 权威历史数据，`(node_id, ts)` 索引 |
| 主机标识 | MySQL `monitor_db.hosts` | 持久 | `node_id` 主键，随上报流首条消息 upsert |
| 调度最新采样 | manager 进程内存 `latest_` | 进程内 | 重启后由下一条上报重建 |
| 告警基线 | manager 进程内存 `last_` | 进程内 | 每节点一条，用于突变判定 |
| 配置 | `configs/*.conf` | 持久 | INI 格式，支持环境变量覆盖关键项 |
| 日志 | stderr（容器由 docker 收集） | 运行期 | 带毫秒时间戳与级别 |

---

## 12. 第三方依赖

| 库 / 工具 | 用途 |
|-----------|------|
| gRPC + Protobuf | RPC 通信与消息序列化（`protoc` + `grpc_cpp_plugin` 生成代码） |
| libmysqlclient | MySQL 连接、预处理语句、结果集查询 |
| libbpf | 加载 eBPF 对象、挂载 kprobe / TC、读取 percpu map |
| clang | 编译 `net_monitor.bpf.c` / `net_tc.bpf.c` 为 BPF 目标文件（`-target bpf`） |
| pthread | 采集/上报/HTTP 线程 |
| CMake | 构建与 ctest |
| Docker / Docker Compose | 镜像构建与一键编排（可选） |
| Linux 内核头文件 | 仅编译内核模块时需要（外部模块构建） |

> INI 配置解析、日志、阻塞队列、单元测试框架均为自研，无第三方日志/测试依赖。

---

## 13. 已知问题与设计取舍

- **MYSQL 连接非线程安全**：多 agent 并发上报曾触发 libmysqlclient SSL double-free（ASan 定位），现 insert 全程持锁 + 3s 连接/读写超时 + 掉线 5s 节流重连
- **gRPC ClientContext 不可复用**：一次调用一个 context，复用触发断言崩溃
- **上报逐条 Write**：骨架版未攒批，样本量大时系统调用偏多（代码中预留了"每 N 条或每 100ms flush"的演进点）
- **MySQL 存时序指标是权宜之计**：生产建议 InfluxDB/Prometheus；查询接口层已把存储实现隔离，替换存储不影响上层
- **时序表无限增长**：`init_db.sql` 留有按时间分区 / 定期归档的 TODO
- **QueryHistory 在存储不可用时返回 UNAVAILABLE**（底层返回 -1），与 QueryTrend 行为一致
- **WSL2 无内核头文件**：需自行全量构建微软内核源码生成 `Module.symvers`（MODVERSIONS 校验）
- **容器内采集的是容器自身 `/proc`**：compose 演示用，生产需 host 网络或挂载宿主机 `/proc`，eBPF/内核模块需特权
- **TC 只统计 `tc_iface`**：默认取默认路由网卡，多网卡/容器 veth 场景需显式配置；与 kprobe（全协议栈、全网卡）口径不同，切换时看启动日志 `net collector: net-tc|net-ebpf|net`
- **已有数据库升级**：`init_db.sql` 的 `CREATE TABLE IF NOT EXISTS` 不会修改已存在的 `metrics` 表，旧库需手动 `ALTER TABLE` 补 `softirq_rate`、`load_avg1/5/15` 列并新建 `hosts` 表

---

## 14. 简历与面试使用建议

### 14.1 简历一句话（可按岗位裁剪）

> C++20 实现的分布式服务器监控系统：内核模块 / eBPF（TC、kprobe）/ `/proc` 三级采集，gRPC client-streaming 上报与查询，MySQL 持久化；实现阈值/突变告警、按场景加权调度和终端监控面板；23 个单元测试 + Docker Compose 一键部署。

- 偏后端：强调 gRPC 流式通信、MySQL 线程安全、并发队列、告警/调度
- 偏基础架构：强调 eBPF、TC/kprobe、内核模块、字符设备、三级降级

### 14.2 可量化亮点

| 亮点 | 证据 |
|------|------|
| 采集准确性 | stress 4 核压满理论 66.7%，实测 68~71% |
| 网络采集 | iperf3 60Gbps 回环，eBPF 采集 7.0~7.5 GB/s，双向对称无丢数 |
| agent 开销 | 常驻内存约 4MB，CPU < 0.5% |
| 测试 | 23 个单元用例，`ctest` 一键运行通过 |
| 工程化 | Docker Compose 编排 MySQL + manager + 2 个 agent |

### 14.3 面试时最值得讲深的三件事

1. **三级采集与降级**：为什么 `/proc` 作为兜底、内核模块和 eBPF 各自适合什么、TC 与 kprobe 的统计口径差异（第 5 章、[design-qa](docs/design-qa.md) 第 8 节）
2. **流式上报与线程安全**：client-streaming 为什么比一元 RPC 更适合持续采样；MySQL 句柄非线程安全导致的 SSL double-free 如何定位、如何用持锁 + 超时 + 重连节流修复（第 13 章）
3. **指标如何变成结论**：`load_avg` / `iowait` / `softirq` 怎么组合判断 CPU 100%；加权调度模型的假设与局限（第 7 章、design-qa 第 9 节）

### 14.4 面试追问索引

- 设计取舍、原理与 10 个常见追问：[docs/design-qa.md](docs/design-qa.md)
- 接口与指标字段：第 10 章
- 踩坑与已知问题：第 13 章

### 14.5 写法提醒

- 不要写“仿照某课程项目”；写“自己实现 + 遇到的问题 + 怎么解决”，面试官更关心后者
- 指标口径、压测数字、踩坑记录比功能列表更有说服力

## License

MIT

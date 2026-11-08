# DistMonitor 技术文档

> 本文按「基础知识 → 项目背景 → 项目架构 → 监控指标 → 基本功能 → 相关知识点」的顺序整理，
> 对应项目 [README.md](../README.md) 的实现，适合答辩讲稿、面试复习和后续维护。
>
> 一句话概括：agent 每秒从内核模块 / eBPF / `/proc` 采集指标，gRPC client-streaming 上报，
> manager 收流后告警、加权调度、落 MySQL，QueryService 查询，monitor_tui 终端展示。

## 本文结构

- [1. 需要掌握的基础知识](#1-需要掌握的基础知识)
- [2. 项目背景](#2-项目背景)
- [3. 项目架构](#3-项目架构)
  - 架构主逻辑 / 一次采样链路 / 为什么分离 / 并发模型
- [4. 项目监控指标](#4-项目监控指标)
- [5. 项目的基本功能](#5-项目的基本功能)
  - 5.1 使用到的中间件（gRPC / Protobuf）
  - 5.2 监控数据定时采集（采集方式 / TC 与 XDP / 具体实现）
  - 5.3 相关知识点（proc / 内核模块 / 字符设备 / mmap）
- [6. 数据处理与存储](#6-数据处理与存储)
  - 告警 / 加权调度 / MySQL 表设计 / 查询接口
- [7. 部署与运行](#7-部署与运行)
- [8. 项目结构](#8-项目结构)
- [9. 延伸阅读](#9-延伸阅读)

---

## 1. 需要掌握的基础知识

| 概念 | 在本项目中的用途 |
|------|------------------|
| `/proc` 文件系统 | CPU、内存、磁盘、网络、负载的通用采集源，无需 root、无需额外依赖 |
| 内核模块 | 读取 `kcpustat` / `kstat` 累计计数，通过 misc 字符设备暴露给用户态 |
| 字符设备 | 内核与用户态的边界：模块注册 `/dev/*_monitor`，agent 用 `read()` 取一行文本 |
| eBPF + CO-RE | 在网络收发路径挂 kprobe / TC 分类器，`vmlinux.h` 做跨内核版本类型重定位 |
| gRPC / Protobuf | agent 与 manager 的通信协议；流式上报降低建连开销 |
| mmap（延伸） | 大缓冲零拷贝方案；本项目单次只传一行文本，`read()` 已足够 |

这些概念在后文「5.3 相关知识点」中会逐个展开。

---

## 2. 项目背景

### 2.1 要解决的问题

多台 Linux 服务器需要统一观测：

- 每台机器的 CPU、内存、磁盘、网络是否吃紧；
- 负载升高时，是 CPU 密集、IO 密集还是网络密集；
- 出现异常时，能否第一时间告警并定位到具体节点；
- 有多台机器可选时，任务应该调度到哪一台。

市面上有 Prometheus + node_exporter + Grafana 的成熟方案，但本项目刻意不引入现成组件，
目的是完整走一遍「内核采集 → 网络传输 → 服务端处理 → 存储查询 → 终端展示」的链路。

### 2.2 设计目标与约束

| 维度 | 约束 |
|------|------|
| 语言 | C++20，agent / manager / common 全部手写 |
| 采集 | 不依赖第三方采集器，优先内核模块，其次 eBPF，最后 `/proc` 兜底 |
| 传输 | 自研 gRPC 协议，Protobuf 序列化 |
| 存储 | MySQL 8.0，不用时序数据库 |
| 部署 | 能在 WSL2 开发、在 Ubuntu 云服务器部署，支持 Docker Compose |
| 展示 | 不依赖 Web 前端，SSH 登录即可看（终端面板） |

### 2.3 最终效果

- 单节点 agent 按 1 秒间隔采样，实测常驻内存约 4 MB、CPU 占用低于 0.5%；
- manager 单进程、单端口同时提供「上报流 + 调度」和「查询服务」两组 gRPC 接口；
- 23 个单元测试覆盖配置解析、告警、调度、阻塞队列；
- Docker Compose 可一键拉起 MySQL + manager + 两个 agent。

---

## 3. 项目架构

### 3.1 架构主逻辑

```text
┌────────────────── agent（每节点一个，不监听端口） ──────────────────┐
│  采集层：每个采集器只填 MetricsSample 中属于自己的字段               │
│                                                                     │
│  CPU      KmodCpuCollector  /dev/cpu_stat_monitor   ← 优先          │
│           CpuCollector      /proc/stat              ← 回退          │
│  软中断   SoftirqCollector  /dev/softirq_monitor    ← 模块加载后    │
│  内存     MemCollector      /proc/meminfo                           │
│  负载     LoadCollector     /proc/loadavg                           │
│  磁盘     DiskCollector     /proc/diskstats                         │
│  网络     TcNetCollector    clsact ingress/egress   ← 首选          │
│           EbpfNetCollector  kprobe + CO-RE          ← 次选          │
│           NetCollector      /proc/net/dev           ← 兜底          │
│                                                                     │
│  每 interval_ms 采一次 → 组装 MetricsSample                          │
│  → GrpcReporter（BlockingQueue + 独立上报线程）                     │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ gRPC client-streaming
                               ▼
┌──────────────── manager（单进程，双 service 同端口） ───────────────┐
│  MonitorService :50051                                              │
│    UploadMetrics ──► WeightedScheduler.update（内存最新采样）       │
│                  ──► AlertEngine.check（阈值 / 突变告警）           │
│                  ──► MysqlStorage.insert（预处理语句落库）          │
│    QueryBestNode ──► WeightedScheduler.rank（按场景加权排名）       │
│  QueryService :50051                                                │
│    QueryLatest / History / Trend / Host ──► MysqlStorage            │
└──────────────────────────────┬──────────────────────────────────────┘
                               ▼
                    MySQL monitor_db：metrics + hosts
                               ▲
                               │ gRPC 查询
                     monitor_tui / query_client
```

**分层原则**：采集与上报在 agent 侧闭环；manager 只做「收、判、存、查」。
调度和告警依赖内存中的最新采样，毫秒级生效；历史查询依赖 MySQL。
MySQL 不可用时整条链路仍能运行，只是不落库、历史查询无数据。

### 3.2 一次采样经过的完整链路

```text
① agent 主循环 sleep(interval_ms)
② 填 node_id / timestamp_ms
③ 依次采集：CPU（kmod→/proc）→ 内存 → 磁盘 → 软中断（可选）→ 网络（TC→eBPF→/proc）
④ reporter.submit(sample) 写入阻塞队列（采集线程不等待网络）
⑤ 上报线程建流 UploadMetrics，首条消息附带 HostInfo（重连后重发）
⑥ 阻塞 pop 队列，逐条 writer->Write
⑦ manager reader->Read 收一条处理一条：调度缓存 → 告警 → MySQL
⑧ 流结束 WritesDone + Finish；失败则 3 秒后重建流，期间采样继续入队
```

### 3.3 为什么 agent / manager 分离

- 采集要贴着被监控机：需要 root、需要内核模块和 eBPF 权限，容器化程度低；
- 分析与存储要集中：告警阈值、节点排名需要全局视图；
- 两者只通过 gRPC 协议耦合：agent 不认识 MySQL，manager 不关心采集方式；
- 新增节点只要部署一个 agent 并填 `manager_addr`，服务端不用改配置。

### 3.4 线程与并发模型

| 位置 | 机制 | 解决的问题 |
|------|------|-----------|
| agent 采集循环 | 主线程 `sleep` + 顺序采集 | 实现简单，避免多采集器争抢字段 |
| 采集 → 上报 | 自研 `BlockingQueue` | 网络抖动不阻塞采样，队列起到削峰作用 |
| 上报线程 | 独立 `std::thread` | 建流、写流、重连与采集解耦 |
| manager 调度缓存 | `std::mutex` + 最新采样 map | 多节点并发上报时保证排名一致性 |
| manager 存储 | 单连接 + 全程互斥锁 | libmysqlclient 句柄非线程安全 |
| 计数器 | `std::atomic`（eBPF percpu / TC） | 避免跨 CPU 锁竞争 |

---

## 4. 项目监控指标

| 指标 | 首选来源 | 回退来源 | 单位 | 统计意义 |
|------|----------|----------|------|----------|
| `cpu_usage` | `/dev/cpu_stat_monitor` | `/proc/stat` | % | 非空闲时间占比，整体 CPU 压力 |
| `cpu_iowait` | 同上 | 同上 | % | CPU 等待 IO 的时间，高说明磁盘/网络存储是瓶颈 |
| `cpu_softirq` | 同上 | 同上 | % | 软中断处理占用，高说明收包/定时器压力大 |
| `mem_usage` | `/proc/meminfo` | — | % | `(MemTotal - MemAvailable) / MemTotal` |
| `load_avg1/5/15` | `/proc/loadavg` | — | — | 1/5/15 分钟平均负载，反映排队压力 |
| `disk_read_bps` | `/proc/diskstats` | — | B/s | 物理盘读吞吐 |
| `disk_write_bps` | 同上 | — | B/s | 物理盘写吞吐 |
| `disk_iops` | 同上 | — | 次/s | 每秒读写完成次数，反映 IO 压力 |
| `net_recv_bps` | TC(clsact) | eBPF kprobe → `/proc/net/dev` | B/s | 网卡接收带宽 |
| `net_send_bps` | 同上 | 同上 | B/s | 网卡发送带宽 |
| `softirq_rate` | `/dev/softirq_monitor` | 不采集（0） | 次/s | 每秒软中断次数，网络/定时器压力 |
| 主机标识 | `host_info` | — | — | hostname / IP / machine-id / 内核 / 发行版 / 启动时间 |

**怎么把这些指标连起来看**：

- `cpu_usage` 高但 `load_avg` 不高 → 短时计算峰值，未必是持续压力；
- `cpu_usage` 不高但 `load_avg` 高 → 任务在等 IO 或锁，重点看 `cpu_iowait`、`disk_iops`；
- `cpu_softirq` / `softirq_rate` 高 → 网络收包或定时器压力，对照 `net_recv_bps` 找峰值；
- `mem_usage` 高要看 `MemAvailable`，Linux 会把空闲内存拿去做缓存，使用率高不一定有问题。

---

## 5. 项目的基本功能

### 5.1 使用到的中间件

#### 5.1.1 gRPC

##### gRPC 的优势

###### HTTP/2 协议

gRPC 的底层是 HTTP/2，带来四个直接收益：

| 特性 | 说明 | 在本项目中的作用 |
|------|------|------------------|
| 多路复用 | 一条 TCP 连接上并发多个流，互不阻塞 | 上报流和查询请求可以共用一条连接 |
| 长连接 | 连接建立一次，多次请求复用 | agent 每秒上报，不用反复握手 |
| 二进制分帧 + 头部压缩 | 帧头小、HPACK 压缩字段 | 高频小包场景开销更低 |
| 双向流 | 客户端和服务端都可连续收发 | 支撑 client-streaming 上报 |

###### 对比其他框架（重点）

| 方案 | 传输/编码 | 流式能力 | C++ 生态 | 适用场景 |
|------|-----------|----------|----------|----------|
| gRPC | HTTP/2 + Protobuf | 一元、客户端流、服务端流、双向流 | 官方支持完善 | 跨语言、长连接、流式上报 |
| REST + JSON | HTTP/1.1 + JSON | 基本只有请求-响应 | 需自己选 HTTP 库 | 简单接口、人可读 |
| Thrift | 自定义二进制协议 | 流式需自行封装 | 可用但生态较小 | 内部 RPC、强 IDL |
| Dubbo | 自定义协议 | 以 Java 生态为主 | C++ 支持弱 | Java 微服务 |

本项目的选择理由：

1. 上报是**持续不断的小包**，client-streaming 比每秒一次一元 RPC 少大量建连和收尾；
2. 查询是请求-响应，用一元 RPC 语义刚好；
3. Protobuf 有 schema、字段编号，后续加指标不会破坏旧客户端；
4. C++ 官方库成熟，`grpc_cpp_plugin` 直接生成代码，不需要额外框架。

###### 项目中的使用

- `MonitorService.UploadMetrics`：agent 建一条长流持续写 `UploadRequest`，manager `while (reader->Read())` 边收边处理，流结束才回 `{ok, received}`；
- `MonitorService.QueryBestNode`：一元 RPC，按场景返回节点排名；
- `QueryService` 四个查询接口：一元 RPC；
- 两个 service 注册到同一个 `ServerBuilder`，共用 50051 端口；
- 断流后 3 秒重建；每条新流的首条消息携带 `HostInfo`，重连后自动重发。

#### 5.1.2 Protobuf

##### 对比 XML / JSON / Protobuf

| 维度 | Protobuf | JSON | XML |
|------|----------|------|-----|
| 体积 | 小（二进制、不传字段名） | 中（重复字段名） | 大（标签成对出现） |
| 解析速度 | 快（按编号直接取值） | 中（字符串解析） | 慢（DOM/SAX 解析） |
| 类型校验 | 强类型，编译期生成代码 | 弱类型，运行期判断 | 弱类型，可配 XSD |
| 兼容性 | 字段编号 + 默认值，向后兼容 | 靠约定 | 靠约定 |
| 可读性 | 需要工具解码 | 人可读 | 人可读 |
| 适用 | 服务间高频通信 | 配置、调试接口 | 文档、遗留系统 |

监控数据每秒都在传，体积和解析开销优先，所以选 Protobuf。

##### 项目中的 proto 设计

| 消息 | 作用 | 关键点 |
|------|------|--------|
| `MetricsSample` | 一次采样的全部指标 | 15 个字段，CPU / 内存 / 磁盘 / 网络 / 软中断 / 负载 |
| `HostInfo` | 主机静态标识 | 只在流首条发送，重连后重发 |
| `UploadRequest` | 上报消息 | `sample` + 可选 `host` |
| `UploadResponse` | 流结束确认 | `ok` + 本次流累计接收条数 |
| `BestNode` / `QueryBestNodeResponse` | 调度结果 | 节点 ID、分数、最新采样 |
| `QueryHistory/Trend` 系列 | 历史与趋势 | 区间、窗口、limit 都有默认值 |

---

### 5.2 监控数据定时采集

#### 5.2.1 采集方式

本项目是**推模式**：agent 主动按 `interval_ms` 采样并推给 manager。

| 模式 | 代表 | 优点 | 缺点 |
|------|------|------|------|
| 推 | 本项目 agent | 服务端不用维护节点地址；NAT/防火墙友好；采集频率由 agent 决定 | 服务端要处理并发流 |
| 拉 | Prometheus | 服务端统一控制抓取节奏；易做服务发现 | 需要能访问每个节点；要额外维护目标列表 |

采集流程的两个细节：

1. CPU、磁盘、网络都是**累计计数**，第一帧没有前值，只能建立基线，从第二帧开始才有速率；
2. 采集与上报通过阻塞队列解耦：队列本身不设上限，`push` 不会阻塞采集；
   上报变慢时采样会在队列中堆积（用内存换吞吐，极端情况下存在积压风险）。

#### 5.2.2 TC 与 XDP

| 方案 | 挂载位置 | 方向 | 优点 | 代价 |
|------|----------|------|------|------|
| XDP | 网卡驱动收包最早阶段 | 主要是 ingress | 性能最高，可在驱动层丢包/重定向 | 依赖网卡和驱动；主要只能处理入方向 |
| TC(clsact) | qdisc 的 ingress / egress | 收发双向 | 方向天然分离、贴近网卡、支持 veth/tunnel | 需要 root 和 qdisc 支持 |
| kprobe | 内核函数 `__dev_queue_xmit` / `__netif_receive_skb` | 收发双向 | 无驱动依赖，覆盖所有网卡 | 依赖内核符号名；统计的是函数调用而非网卡流量 |

本项目的选择：

1. 默认 `net_mode=auto`，顺序为 **TC → eBPF kprobe → `/proc/net/dev`**；
2. TC 只统计默认路由网卡（或配置 `tc_iface`），避免 loopback 噪声；
3. kprobe 统计全协议栈收发，覆盖所有网卡，包括 loopback；
4. 任一方案失败都保留 `/proc` 兜底，启动日志打印 `net collector: net-tc|net-ebpf|net`，方便核对口径。

**为什么不用 XDP**：项目只需要统计字节数，不需要在驱动层丢包或重定向；
XDP 主要覆盖 ingress，且对网卡/驱动有要求，收益与复杂度不匹配。

#### 5.2.3 具体实现

##### CPU 负载信息的采集（/proc/stat）

`/proc/stat` 首行 `cpu` 是 8 个累计 jiffies：

```text
cpu  user nice system idle iowait irq softirq steal
```

采样时读取两次快照，用差值计算：

```text
total = user + nice + system + idle + iowait + irq + softirq + steal
cpu_usage   = 100 * (Δtotal - Δidle) / Δtotal
cpu_iowait  = 100 * Δiowait / Δtotal
cpu_softirq = 100 * Δsoftirq / Δtotal
```

注意 `cpu_usage` 只把 `idle` 当空闲，`iowait`、`softirq` 都算作忙，
这样 `cpu_usage` 反映"CPU 没有完全空闲"的整体比例；具体瓶颈再看拆分字段。

##### 选择 /proc 方式采集的原因

- 任何 Linux 都有，不需要 root，不需要装依赖；
- 纯文本、字段稳定，调试时 `cat /proc/stat` 就能核对；
- 缺点是轮询、精度受采样间隔限制、看不到协议栈内部细节。

因此 `/proc` 在本项目里是**兜底层**：内核模块或 eBPF 不可用时保证系统仍能工作，
而不是唯一方案。

##### CPU 软中断信息的采集（/dev/softirq_monitor）

- 内核模块遍历在线 CPU 的 `kstat_cpu(cpu).softirqs[]`，累加后输出一行 11 个值：
  `total + HI / TIMER / NET_TX / NET_RX / BLOCK / IRQ_POLL / TASKLET / SCHED / HRTIMER / RCU`；
- agent 只取 total，按采样间隔做差得到 `softirq_rate`（次/秒）；
- 没有加载模块时该字段保持 0，但 `cpu_softirq` 百分比仍可从 `/proc/stat` 得到。

##### CPU 状态信息的采集（内核模块）

- 内核模块用 `kcpustat_cpu_fetch` 遍历在线 CPU，聚合 user/nice/system/idle/iowait/irq/softirq/steal；
- 通过 `misc_register` 注册 `/dev/cpu_stat_monitor`（0444 只读）；
- 用户态 `read()` 得到一行文本，agent 打开、读取、关闭，逻辑与 `/proc` 版完全一致，只是换数据源；
- 模块是**单次读语义**（`*off > 0` 直接返回 0），每次采样重新打开文件；
- 优势：链路更短，未来可扩展 per-CPU、per-软中断类型；
- 代价：需要匹配运行内核的构建树和 `Module.symvers`，WSL2 还要自己编译微软内核源码。

##### CPU 为什么满了？如何排查？

按顺序看指标，能把"CPU 忙"拆成具体原因：

1. `cpu_usage`：整体打满，还是单节点异常；
2. `load_avg1/5/15`：高说明等 CPU 的任务在排队；CPU% 不高但 load 高，通常是 IO/锁等待；
3. `cpu_iowait`：高 → 磁盘或网络存储慢，CPU 在等 IO；
4. `cpu_softirq` / `softirq_rate`：高 → 收包/定时器压力，常见于大流量或小包风暴；
5. `net_recv_bps / net_send_bps`：是否与流量峰值对应；
6. `disk_iops / disk_bps`：是否磁盘打满；
7. 告警：`cpu_jump` 突变告警能在曲线突变时第一时间提示；
8. 多节点对比：`QueryBestNode` 排名快速定位最忙/最闲的机器。

##### 内存信息的采集（/proc/meminfo）

```text
mem_usage = 100 * (MemTotal - MemAvailable) / MemTotal
```

为什么用 `MemAvailable` 而不是 `MemFree`：Linux 会把空闲内存拿去做页缓存，
`MemFree` 很小是正常现象；`MemAvailable` 估算的是"还能给新进程用多少"，更接近真实压力。

##### 指标统计的意义：定位内存问题

- 内存使用率持续上升 + `load_avg` 上升 → 可能存在换页/内存回收压力；
- 内存使用率高但 `load_avg` 低 → 多半是页缓存占用，属于正常现象；
- 当前版本没有采集 swap 明细，判断时需要结合 `MemAvailable` 和系统日志；
- 与 CPU、IO 指标一起看，才能区分"真缺内存"和"缓存占内存"。

##### 网络信息的采集（TC / eBPF / /proc）

三套实现，输出同一组字段 `net_recv_bps / net_send_bps`：

1. **TC(clsact)**：`net_tc.bpf.c` 的 ingress/egress 分类器分别累加 `skb->len`；
   用 `bpf_tc_hook_create` 创建 clsact，再 `bpf_tc_attach` 挂载固定 handle/priority；
   只统计 `tc_iface`（留空取默认路由网卡），退出时 detach。
2. **eBPF kprobe**：`net_monitor.bpf.c` 在 `__dev_queue_xmit`（发）和
   `__netif_receive_skb`（收）累加字节；`PERCPU_ARRAY` 按 CPU 累计，
   用户态用 `libbpf_num_possible_cpus()` 汇总，避免跨 CPU 锁竞争。
3. **`/proc/net/dev`**：跳过 `lo`，收字节取第 1 列、发字节取第 9 列，多网卡累加。

三者的计数都是累计值，采样时做差再除以 `dt` 得到 B/s；
计数器归零（模块重挂、网卡重建）时重建基线，避免无符号相减回绕成巨大值。

##### 选择 eBPF 的原因

- 在收发包路径上直接累加，不用轮询解析大段文本；
- TC 能明确区分收发方向，kprobe 能覆盖所有网卡；
- CO-RE + `vmlinux.h` 让程序与具体内核版本解耦，不需要按内核树编译；
- 代价是需要 root 和内核支持，因此保留 `/proc` 兜底。

##### 网络指标统计的意义

- `net_recv_bps / net_send_bps` 判断带宽是否打满、是否存在单向异常；
- 与 `cpu_softirq`、`softirq_rate` 对照，可区分"流量大"还是"小包多导致软中断高"；
- TC 口径是单网卡，kprobe 口径是全协议栈全网卡，切换采集器时要以启动日志为准。

##### 磁盘 IO 信息的采集（/proc/diskstats）

- 每行一个块设备，取读扇区数、写扇区数、读完成次数、写完成次数；
- 扇区固定按 512B 折算，`dt` 用 `steady_clock`，输出 B/s 和 IOPS；
- 只统计**物理整盘**：`/sys/block/<name>/device` 存在才是真实整盘，
  自动排除分区、loop/ram/zram、dm/md 等虚拟设备；
- 首次采样打印参与统计的整盘列表，便于核对口径；
- 计数器变小（热插拔/重启）时重建基线，本周期的速率跳过。

##### 磁盘指标统计的意义

- `disk_read_bps / disk_write_bps` 看吞吐是否打满；
- `disk_iops` 看随机小 IO 压力，IOPS 高但吞吐低通常是随机读写；
- 与 `cpu_iowait` 强相关：iowait 高、磁盘 IOPS 高，说明瓶颈在磁盘；
- 与 `load_avg` 结合，可解释"CPU 不忙但系统很卡"的情况。

##### 主机信息标识采集

| 字段 | 来源 | 说明 |
|------|------|------|
| hostname | `gethostname()` | 主机名 |
| ip | `/proc/net/route` + `getifaddrs()` | 默认路由网卡 IPv4；无默认路由时取第一个非回环地址 |
| machine_id | `/etc/machine-id` | 取不到时退回 `/var/lib/dbus/machine-id` |
| kernel | `uname()` | 内核 release |
| os | `/etc/os-release` 的 `PRETTY_NAME` | 发行版名称 |
| boot_time_ms | `/proc/stat` 的 `btime` | 系统启动时间，转毫秒 |

发送时机：每条上报流的**第一条消息**携带，重连后自动重发；
manager 用 `INSERT ... ON DUPLICATE KEY UPDATE` 写入 `hosts` 表，并提供 `QueryHost` 查询。

为什么需要它：`node_id` 是配置值，可能重复或写错；主机标识是机器真实身份，
多节点排障时能直接定位到"哪台机器、什么系统、什么时候启动"。

---

### 5.3 相关知识点

#### proc 文件系统

- 内核在内存中生成的虚拟文件系统，读取时由内核实时计算，不占磁盘；
- 项目用到的文件：`/proc/stat`、`/proc/meminfo`、`/proc/loadavg`、
  `/proc/diskstats`、`/proc/net/dev`、`/proc/net/route`；
- 优点：无需权限、无需依赖、格式稳定；缺点：轮询、精度受采样间隔限制。

#### 内核模块

- `.ko` 文件，通过 `insmod` / `modprobe` 加载进内核，运行在内核态；
- 本项目两个模块：
  - `cpu_stat_monitor.ko`：`kcpustat_cpu_fetch` → `/dev/cpu_stat_monitor`；
  - `softirq_monitor.ko`：`kstat_cpu().softirqs[]` → `/dev/softirq_monitor`；
- 编译需要匹配运行内核的构建树；WSL2 需要克隆对应版本的微软内核源码并全量 `make`
  生成 `Module.symvers`。

#### 字符设备

- Linux 设备分字符设备、块设备、网络设备；本项目用字符设备传递文本；
- `misc_register` 注册杂项设备，自动分配次设备号，权限设为 0444；
- 用户态通过 `open()` / `read()` / `close()` 操作，不需要 ioctl；
- 模块的 `read` 是单次读语义：第二次读同一文件返回 0。

#### mmap 内存映射

- 把设备内存或文件映射到进程地址空间，读写不经过 `read()` 拷贝，适合大缓冲；
- 本项目每次只传一行几十字节文本，`read()` 的拷贝成本可忽略，因此没有用 mmap；
- 如果未来改成 ring buffer 高频传输（类似 eBPF 的 `BPF_MAP_TYPE_RINGBUF`），
  mmap 才是合适方案。

---

## 6. 数据处理与存储

### 6.1 告警

| 规则 | 默认阈值 | 判定方式 |
|------|----------|----------|
| CPU 超限 | `cpu_max = 90%` | 严格大于 |
| 内存超限 | `mem_max = 90%` | 严格大于 |
| CPU 突变 | `cpu_jump = 30%` | 相邻采样差值的绝对值 |

- 每个节点独立维护上一条采样，避免多节点互相干扰；
- 告警回调通过 `std::function` 注入，当前打印日志，后续可接钉钉/邮件/Webhook；
- 突变告警取绝对值，既覆盖突增也覆盖突降。

### 6.2 加权调度

调度分数采用「剩余能力百分比」模型：

```text
cpu_cap  = max(0, 100 - cpu_usage)
mem_cap  = max(0, 100 - mem_usage)
disk_cap = 100 * (1 - min(disk_iops / 5000, 1))
net_cap  = 100 * (1 - min((net_recv_bps + net_send_bps) / 200MB, 1))

score = w_cpu * cpu_cap + w_mem * mem_cap + w_disk * disk_cap + w_net * net_cap
```

场景权重：

| 场景 | cpu | mem | net | disk |
|------|-----|-----|-----|------|
| `cpu_bound` | 0.7 | 0.2 | 0.05 | 0.05 |
| `io_bound` | 0.2 | 0.1 | 0.1 | 0.6 |
| `net_bound` | 0.2 | 0.1 | 0.6 | 0.1 |
| 默认 | 0.5 | 0.3 | 0.1 | 0.1 |

调度依赖内存中的最新采样，因此是毫秒级生效；历史排名不落库，避免调度策略变化后历史分数失真。

### 6.3 MySQL 表设计

数据库 `monitor_db`，两张表：

| 表 | 作用 | 关键字段 |
|----|------|----------|
| `metrics` | 指标采样（只写 + 查询） | `node_id`、`ts`、CPU/内存/磁盘/网络/软中断/负载字段、`INDEX idx_node_ts(node_id, ts)` |
| `hosts` | 主机标识（upsert + 查询） | `node_id` 主键、hostname、ip、machine_id、kernel、os、boot_time_ms、updated_at |

写入要点：

- `metrics` 用**预处理语句**，15 个参数绑定，避免拼接 SQL 和注入；
- `hosts` 用 `INSERT ... ON DUPLICATE KEY UPDATE` 做 upsert；
- 单条 MySQL 连接 + 互斥锁，`ping` 探活，断线后 5 秒节流重连，连接/读写超时 3 秒；
- 账号至少需要 `SELECT, INSERT, UPDATE` 权限（upsert 需要 UPDATE）。

```sql
-- 建表与初始化见 scripts/init_db.sql
-- 旧库升级补充字段：
ALTER TABLE metrics
  ADD COLUMN softirq_rate DOUBLE,
  ADD COLUMN load_avg1 DOUBLE,
  ADD COLUMN load_avg5 DOUBLE,
  ADD COLUMN load_avg15 DOUBLE;

GRANT SELECT, INSERT, UPDATE ON monitor_db.* TO 'monitor'@'localhost';
```

### 6.4 查询接口

| RPC | 数据来源 | 参数约定 | 错误语义 |
|-----|----------|----------|----------|
| `QueryLatest` | `metrics` 按 ts 倒序 LIMIT 1 | `node_id` 必填 | 无数据 → NOT_FOUND |
| `QueryHistory` | 时间区间升序 | `end<=0` 取当前；`start<=0` 取 end-300s；`limit<=0` 取 600（上限 10000） | 空区间 → 空列表 OK |
| `QueryTrend` | 最近 N 秒聚合 | `window_sec<=0` 取 300（上限 86400） | 存储不可用 → UNAVAILABLE |
| `QueryHost` | `hosts` 按 node_id 查询 | `node_id` 必填 | 无记录 → NOT_FOUND |

终端面板 `monitor_tui` 通过 `QueryBestNode` 取全部节点最新采样与分数，
通过 `QueryHost` 补主机信息（失败后每 30 秒重试），用 ANSI 备用屏幕整屏刷新。

---

## 7. 部署与运行

### 7.1 编译

```bash
cd DistMonitor
cmake -B build
cmake --build build -j$(nproc)
```

`-DENABLE_EBPF_OBJ=OFF` 可跳过 eBPF 目标文件编译，运行时自动回退 `/proc`。
eBPF 程序需要 clang；内核模块需要匹配内核树（见 5.3）。

### 7.2 启动

一键脚本：

```bash
./scripts/start_all.sh          # 启动 manager + agent，并进入实时面板
./scripts/start_all.sh --no-tui # 只启动后台服务
./scripts/start_all.sh status   # 查看 PID、端口、最近日志
./scripts/start_all.sh stop     # 停止
```

手动方式（三个终端）：

```bash
# 终端 1：manager
./build/manager -c configs/manager.conf

# 终端 2：agent（root 才能启用 TC/eBPF，普通用户自动回退 /proc）
sudo env LD_LIBRARY_PATH= ./build/agent -c configs/agent.conf

# 终端 3：面板
./build/monitor_tui
```

### 7.3 Docker

```bash
sudo docker compose -f docker/docker-compose.yml up -d
# mysql 映射宿主机 3307，manager 50051，agent1/agent2 各一个
```

容器内无特权，eBPF/TC 与内核模块自动降级 `/proc`。

### 7.4 验证

```bash
./build/monitor_tui --once --no-color   # 面板单帧
./build/query_client                    # 四个查询接口
mysql -h127.0.0.1 -umonitor -pmonitor123 monitor_db \
  -e "SELECT node_id,cpu_usage,mem_usage,load_avg1,ts FROM metrics ORDER BY ts DESC LIMIT 5;"
```

### 7.5 常见问题

| 现象 | 原因 | 处理 |
|------|------|------|
| `GLIBCXX_... not found` | Trae 终端注入旧版 libstdc++ | 用 `env -u LD_LIBRARY_PATH <命令>` 或 `LD_LIBRARY_PATH=` |
| `storage: insert failed` | 缺 `softirq_rate` / `load_avg` 字段或权限不足 | 执行 6.3 的 ALTER + GRANT |
| `Connection refused` | manager 未启动或被终端回收 | 先起 manager；后台启动用 `setsid nohup` |
| `manager: listen on 0.0.0.0:50051 failed` | 50051 被占用 | `ss -lntp | grep 50051` 找到并停止旧进程 |
| `softirq_rate = 0` | agent 非 root，模块/eBPF 未加载 | 用 sudo 启动，或接受 `/proc` 降级 |
| `host=` 显示为空 | `hosts` 表不存在或 UPDATE 权限缺失 | 建表并授权 |
| eBPF 加载失败 | WSL2 内核限制 / RLIMIT_MEMLOCK / 非 root | 属预期降级，看启动日志确认回退到 `/proc` |

---

## 8. 项目结构

```text
DistMonitor/
├── agent/                  # 采集端（部署在被监控机器）
│   ├── collector/          # 各类采集器实现（CPU / 内存 / 磁盘 / 网络 / 负载）
│   ├── bpf/                # eBPF 程序源码（TC 分类器、kprobe）与 libbpf 加载器
│   ├── kmod/               # 内核模块源码（CPU 统计、软中断计数 → 字符设备）
│   ├── reporter/           # gRPC 流式上报（阻塞队列解耦 + 断流重连）
│   ├── host_info.*         # 主机静态标识（hostname / IP / machine-id / 内核 / OS）
│   └── main.cpp            # 采集器装配 + 采样循环
├── manager/                # 管理端（部署在管理机）
│   ├── service/            # MonitorService（上报 / 调度）、QueryService（查询）
│   ├── storage/            # MySQL 持久化与查询（metrics / hosts）
│   ├── scheduler/          # 场景加权打分与节点排名
│   ├── alert/              # 阈值 + 突增告警
│   └── main.cpp            # 配置装配 + 双 service 注册
├── common/                 # 公共基础库：配置解析、分级日志、阻塞队列
├── proto/                  # Protobuf 指标结构 + gRPC 接口定义
├── tools/                  # monitor_tui 终端面板、query_client 验证工具
├── scripts/                # init_db.sql 建表、start_all.sh 一键启停
├── tests/                  # 单元测试（23 用例）
├── docker/                 # Dockerfile + docker-compose
├── configs/                # agent.conf / manager.conf
└── CMakeLists.txt          # 构建配置
```

---

## 9. 延伸阅读

- [README.md](../README.md)：功能清单、模块详解、数据库字段、压测数据；
- [design-qa.md](design-qa.md)：关键设计取舍与面试问答；
- [init_db.sql](../scripts/init_db.sql)：建库建表语句。

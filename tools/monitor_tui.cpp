// monitor_tui：终端监控面板（无 Web 前端、无第三方 TUI 库）
// 通过 gRPC 向 manager 轮询节点最新指标，用 ANSI 转义序列在终端整屏刷新。
//
// 用法:
//   ./build/monitor_tui [manager_addr] [--interval=N] [--once] [--no-color]
//   例: ./build/monitor_tui 127.0.0.1:50051 --interval=1
//
// 数据来源:
//   MonitorService.QueryBestNode  -> 全部节点最新采样 + 调度分数
//   QueryService.QueryHost        -> 节点主机标识（MySQL 未就绪时显示 "-"）
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "monitor.grpc.pb.h"

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }

constexpr const char *kReset = "\033[0m";
constexpr const char *kBold = "\033[1m";
constexpr const char *kDim = "\033[2m";
constexpr const char *kGreen = "\033[32m";
constexpr const char *kYellow = "\033[33m";
constexpr const char *kRed = "\033[31m";

struct NodeView {
  std::string node_id;
  monitor::MetricsSample latest;
  double score = 0;
  std::string hostname;
  std::string ip;
};

struct HostEntry {
  std::string hostname;
  std::string ip;
};

std::string now_string() {
  std::time_t t = std::time(nullptr);
  char buf[64] = {};
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
  return buf;
}

std::string cut(const std::string &s, size_t n) {
  return s.size() <= n ? s : s.substr(0, n);
}

std::string fmt_time(int64_t ms) {
  if (ms <= 0) return "-";
  std::time_t t = static_cast<std::time_t>(ms / 1000);
  char buf[16] = {};
  std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
  return buf;
}

// MySQL 风格的表格边框，列宽与表头/数据行严格对应
std::string border_line() {
  static const int kWidths[] = {12, 6, 6, 8, 8, 8, 11, 11, 11, 11, 7, 8};
  std::string s = "+";
  for (int w : kWidths) s += std::string(w, '-') + "+";
  return s;
}

const char *load_color(double pct) {
  if (pct >= 90) return kRed;
  if (pct >= 60) return kYellow;
  return kGreen;
}

// 分数越高越好，配色方向与负载相反
const char *score_color(double score) {
  if (score >= 70) return kGreen;
  if (score >= 40) return kYellow;
  return kRed;
}

std::string fmt_bps(double bps) {
  char buf[64];
  if (bps >= 1024.0 * 1024.0 * 1024.0) {
    std::snprintf(buf, sizeof(buf), "%.2f GB/s", bps / (1024.0 * 1024.0 * 1024.0));
  } else if (bps >= 1024.0 * 1024.0) {
    std::snprintf(buf, sizeof(buf), "%.1f MB/s", bps / (1024.0 * 1024.0));
  } else if (bps >= 1024.0) {
    std::snprintf(buf, sizeof(buf), "%.1f KB/s", bps / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f B/s", bps);
  }
  return buf;
}

bool fetch_frame(const std::string &addr, monitor::MonitorService::Stub *mon,
                 monitor::QueryService::Stub *query,
                 std::map<std::string, HostEntry> *hosts,
                 std::map<std::string, std::chrono::steady_clock::time_point>
                     *host_attempt,
                 std::vector<NodeView> *out, std::string *error) {
  out->clear();
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() +
                   std::chrono::seconds(2));
  monitor::QueryBestNodeRequest req;  // 空 scenario = 默认权重
  monitor::QueryBestNodeResponse resp;
  grpc::Status st = mon->QueryBestNode(&ctx, req, &resp);
  if (!st.ok()) {
    *error = st.error_message();
    return false;
  }
  *error = "";

  for (int i = 0; i < resp.ranked_size(); ++i) {
    const auto &bn = resp.ranked(i);
    NodeView nv;
    nv.node_id = bn.node_id();
    nv.latest = bn.latest();
    nv.score = bn.score();

    auto it = hosts->find(nv.node_id);
    if (it != hosts->end()) {
      nv.hostname = it->second.hostname;
      nv.ip = it->second.ip;
    } else {
      // MySQL 还没就绪时查询会失败；每 30s 重试一次，数据库修好后无需重启面板
      auto now = std::chrono::steady_clock::now();
      auto at = host_attempt->find(nv.node_id);
      if (at == host_attempt->end() ||
          now - at->second >= std::chrono::seconds(30)) {
        (*host_attempt)[nv.node_id] = now;
        grpc::ClientContext hctx;
        hctx.set_deadline(std::chrono::system_clock::now() +
                          std::chrono::seconds(1));
        monitor::QueryNodeRequest hreq;
        hreq.set_node_id(nv.node_id);
        monitor::HostInfo info;
        if (query->QueryHost(&hctx, hreq, &info).ok()) {
          HostEntry e{info.hostname(), info.ip()};
          (*hosts)[nv.node_id] = e;
          nv.hostname = e.hostname;
          nv.ip = e.ip;
        }
      }
    }
    out->push_back(std::move(nv));
  }
  return true;
}

void draw(const std::string &addr, const std::vector<NodeView> &nodes,
          const std::string &error, bool once, bool color) {
  const char *reset = color ? kReset : "";
  const char *bold = color ? kBold : "";
  const char *dim = color ? kDim : "";
  const char *red = color ? kRed : "";

  if (!once) std::printf("\033[H\033[2J");  // 光标归位 + 清屏

  double avg_cpu = 0;
  for (const auto &n : nodes) avg_cpu += n.latest.cpu_usage();
  if (!nodes.empty()) avg_cpu /= static_cast<double>(nodes.size());

  std::printf("%sDistMonitor%s  |  manager %s  |  %s  |  %zu node(s)",
              bold, reset, addr.c_str(), now_string().c_str(), nodes.size());
  if (!nodes.empty()) {
    std::printf("  |  avg CPU %.1f%%  |  best %s (%.1f)", avg_cpu,
                cut(nodes.front().node_id, 20).c_str(), nodes.front().score);
  }
  std::printf("\n");

  if (!error.empty()) {
    std::printf("%s无法连接 manager: %s%s\n", red, error.c_str(), reset);
    std::printf("%s检查 manager 是否已启动、地址是否正确%s\n", dim, reset);
    if (!once) std::printf("\n%sCtrl+C 退出%s\n", dim, reset);
    std::fflush(stdout);
    return;
  }
  if (nodes.empty()) {
    std::printf("%s暂无数据：等待 agent 上报（QueryBestNode 缓存为空）%s\n", dim,
                reset);
    if (!once) std::printf("\n%sCtrl+C 退出%s\n", dim, reset);
    std::fflush(stdout);
    return;
  }

  // MySQL 风格表格：NODE/CPU/MEM/LOAD1/5/15/DISK/NET/SCORE/TIME
  const std::string line = border_line();
  std::printf("%s%s%s\n", dim, line.c_str(), reset);
  std::printf(
      "%s|%-12s|%6s|%6s|%8s|%8s|%8s|%11s|%11s|%11s|%11s|%7s|%8s|%s\n", bold,
      "NODE", "CPU%", "MEM%", "LOAD1", "LOAD5", "LOAD15", "DISK-R", "DISK-W",
      "NET-RX", "NET-TX", "SCORE", "TIME", reset);
  std::printf("%s%s%s\n", dim, line.c_str(), reset);

  for (const auto &n : nodes) {
    const auto &s = n.latest;
    std::printf(
        "|%-12s|%s%6.1f%s|%s%6.1f%s|%8.2f|%8.2f|%8.2f|%11s|%11s|%11s|%11s|"
        "%s%7.1f%s|%8s|\n",
        cut(n.node_id, 12).c_str(), color ? load_color(s.cpu_usage()) : "",
        s.cpu_usage(), reset, color ? load_color(s.mem_usage()) : "",
        s.mem_usage(), reset, s.load_avg1(), s.load_avg5(), s.load_avg15(),
        fmt_bps(s.disk_read_bps()).c_str(),
        fmt_bps(s.disk_write_bps()).c_str(),
        fmt_bps(s.net_recv_bps()).c_str(),
        fmt_bps(s.net_send_bps()).c_str(),
        color ? score_color(n.score) : "", n.score, reset,
        fmt_time(s.timestamp_ms()).c_str());
  }
  std::printf("%s%s%s\n", dim, line.c_str(), reset);

  // 表格放不下的细节放到底部：主机标识 / iowait / softirq / iops
  for (const auto &n : nodes) {
    const auto &s = n.latest;
    std::string host = "-";
    if (!n.hostname.empty() || !n.ip.empty()) {
      host = n.hostname;
      if (!n.ip.empty()) host += (host.empty() ? "" : " · ") + n.ip;
    }
    std::printf(
        "%s  %-12s host=%s  iowait=%.1f%%  softirq=%.1f%%  iops=%.0f%s\n", dim,
        cut(n.node_id, 12).c_str(), host.c_str(), s.cpu_iowait(),
        s.cpu_softirq(), s.disk_iops(), reset);
  }

  if (!once) std::printf("\n%sCtrl+C 退出%s\n", dim, reset);
  std::fflush(stdout);
}

void usage(const char *prog) {
  std::printf(
      "用法: %s [manager_addr] [--interval=N] [--once] [--no-color]\n"
      "  manager_addr   默认 127.0.0.1:50051\n"
      "  --interval=N   刷新间隔秒，默认 1，范围 1~60\n"
      "  --once         只打印一帧后退出（非 TTY 环境自动启用）\n"
      "  --no-color     关闭 ANSI 颜色\n",
      prog);
}

}  // namespace

int main(int argc, char **argv) {
  std::string addr = "127.0.0.1:50051";
  int interval = 1;
  bool once = false;
  bool color = true;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--once") {
      once = true;
    } else if (a == "--no-color") {
      color = false;
    } else if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return 0;
    } else if (a.rfind("--interval=", 0) == 0) {
      interval = std::atoi(a.c_str() + std::strlen("--interval="));
    } else if (a == "--interval" && i + 1 < argc) {
      interval = std::atoi(argv[++i]);
    } else {
      addr = a;
    }
  }
  if (interval < 1) interval = 1;
  if (interval > 60) interval = 60;

  // 非 TTY（重定向/管道）只输出一帧，避免 ANSI 控制字符污染日志
  if (!isatty(STDOUT_FILENO)) {
    once = true;
    color = false;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
  auto mon = monitor::MonitorService::NewStub(channel);
  auto query = monitor::QueryService::NewStub(channel);

  std::map<std::string, HostEntry> hosts;
  std::map<std::string, std::chrono::steady_clock::time_point> host_attempt;
  std::vector<NodeView> nodes;
  std::string error;

  if (!once) std::printf("\033[?1049h\033[?25l");  // 备用屏幕 + 隐藏光标

  while (g_running) {
    auto start = std::chrono::steady_clock::now();
    fetch_frame(addr, mon.get(), query.get(), &hosts, &host_attempt, &nodes,
                &error);
    draw(addr, nodes, error, once, color);
    if (once) break;

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto rest = std::chrono::seconds(interval) - elapsed;
    if (rest > std::chrono::seconds::zero())
      std::this_thread::sleep_for(rest);
  }

  if (!once) std::printf("\033[?25h\033[?1049l");  // 恢复光标 + 主屏幕
  return 0;
}

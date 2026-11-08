// QueryService 验证客户端：QueryLatest / QueryHistory / QueryTrend
// 用法: ./query_client <endpoint> <node_id>   例: ./query_client 127.0.0.1:50051 node-1
#include <cstdio>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "monitor.grpc.pb.h"

int main(int argc, char** argv) {
  std::string endpoint = argc > 1 ? argv[1] : "127.0.0.1:50051";
  std::string node = argc > 2 ? argv[2] : "node-1";

  auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
  auto stub = monitor::QueryService::NewStub(channel);

  // 1) 最新快照
  {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    monitor::QueryNodeRequest req;
    req.set_node_id(node);
    monitor::MetricsSample snap;
    grpc::Status st = stub->QueryLatest(&ctx, req, &snap);
    if (!st.ok()) {
      std::printf("[QueryLatest] FAILED: %s\n", st.error_message().c_str());
    } else {
      std::printf("[QueryLatest] %s cpu=%.1f%% mem=%.1f%% iops=%.0f "
                  "net=%.1f/%.1f KB/s softirq=%.0f/s load=%.2f/%.2f/%.2f "
                  "t=%lld\n",
                  snap.node_id().c_str(), snap.cpu_usage(), snap.mem_usage(),
                  snap.disk_iops(), snap.net_recv_bps() / 1024,
                  snap.net_send_bps() / 1024, snap.softirq_rate(),
                  snap.load_avg1(), snap.load_avg5(), snap.load_avg15(),
                  (long long)snap.timestamp_ms());
    }
  }

  // 2) 历史区间：最近 60 秒
  {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    monitor::QueryHistoryRequest req;
    req.set_node_id(node);
    req.set_limit(100);
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    req.set_end_ms(now.count());
    req.set_start_ms(now.count() - 60 * 1000);
    monitor::QueryHistoryResponse resp;
    grpc::Status st = stub->QueryHistory(&ctx, req, &resp);
    if (!st.ok()) {
      std::printf("[QueryHistory] FAILED: %s\n", st.error_message().c_str());
    } else {
      std::printf("[QueryHistory] %d 条（最近60s，limit=100）\n",
                  resp.samples_size());
      if (resp.samples_size() > 0) {
        const auto& first = resp.samples(0);
        std::printf("  首条: %s cpu=%.1f%% t=%lld\n", first.node_id().c_str(),
                    first.cpu_usage(), (long long)first.timestamp_ms());
      }
    }
  }

  // 3) 窗口趋势：最近 300 秒均值
  {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    monitor::QueryTrendRequest req;
    req.set_node_id(node);
    req.set_window_sec(300);
    monitor::QueryTrendResponse resp;
    grpc::Status st = stub->QueryTrend(&ctx, req, &resp);
    if (!st.ok()) {
      std::printf("[QueryTrend] FAILED: %s\n", st.error_message().c_str());
    } else {
      std::printf("[QueryTrend] n=%d cpu_avg=%.1f%% mem_avg=%.1f%% "
                  "softirq_avg=%.0f/s net_avg=%.1f KB/s\n",
                  resp.sample_count(), resp.cpu_avg(), resp.mem_avg(),
                  resp.softirq_avg(), resp.net_total_avg_bps() / 1024);
    }
  }
  // 4) 主机标识
  {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    monitor::QueryNodeRequest req;
    req.set_node_id(node);
    monitor::HostInfo host;
    grpc::Status st = stub->QueryHost(&ctx, req, &host);
    if (!st.ok()) {
      std::printf("[QueryHost] FAILED: %s\n", st.error_message().c_str());
    } else {
      std::printf("[QueryHost] %s hostname=%s ip=%s kernel=%s os=%s "
                  "machine_id=%s boot=%lld\n",
                  node.c_str(), host.hostname().c_str(), host.ip().c_str(),
                  host.kernel().c_str(), host.os().c_str(),
                  host.machine_id().c_str(), (long long)host.boot_time_ms());
    }
  }
  return 0;
}

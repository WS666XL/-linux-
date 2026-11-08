#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "common/blocking_queue.h"
#include "monitor.grpc.pb.h"

namespace agent {

// 指标上报器：后台线程与 manager 建立客户端流式连接持续推送，
// 连接断开自动重连。采集线程通过 submit() 线程安全投递。
class GrpcReporter {
 public:
  explicit GrpcReporter(std::string server_addr);
  ~GrpcReporter();

  void start();
  void stop();
  void submit(monitor::MetricsSample sample);

  // 主机标识：每条新建的流只随首条消息发送一次
  void set_host_info(const monitor::HostInfo &info);

 private:
  void run();

  std::string server_addr_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<monitor::MonitorService::Stub> stub_;
  common::BlockingQueue<monitor::MetricsSample> queue_;
  std::thread worker_;
  std::atomic<bool> running_{false};
  monitor::HostInfo host_;
  bool has_host_ = false;
};

}  // namespace agent

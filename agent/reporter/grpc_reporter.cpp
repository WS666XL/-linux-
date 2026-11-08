#include "grpc_reporter.h"

#include <chrono>

#include "common/logger.h"

using namespace std::chrono_literals;

namespace agent {

GrpcReporter::GrpcReporter(std::string server_addr)
    : server_addr_(std::move(server_addr)) {
  channel_ = grpc::CreateChannel(server_addr_, grpc::InsecureChannelCredentials());
  stub_ = monitor::MonitorService::NewStub(channel_);
}

GrpcReporter::~GrpcReporter() { stop(); }

void GrpcReporter::start() {
  running_ = true;
  worker_ = std::thread([this] { run(); });
}

void GrpcReporter::stop() {
  if (!running_.exchange(false)) return;
  queue_.close();  // 唤醒阻塞的 pop，让 run() 退出
  if (worker_.joinable()) worker_.join();
}

void GrpcReporter::set_host_info(const monitor::HostInfo &info) {
  host_ = info;
  has_host_ = true;
}

void GrpcReporter::submit(monitor::MetricsSample sample) {
  queue_.push(std::move(sample));
}

void GrpcReporter::run() {
  while (running_) {
    grpc::ClientContext ctx;
    monitor::UploadResponse resp;
    auto writer = stub_->UploadMetrics(&ctx, &resp);
    bool first = true; // 主机标识只随本条流的首条消息发送
    // 骨架版逐条写流；后续可攒批（如每 10 条或每 100ms flush 一次）减少系统调用
    while (running_) {
      auto sample = queue_.pop();
      if (!sample) break;  // 队列已关闭
      monitor::UploadRequest req;
      req.mutable_sample()->Swap(&*sample);
      if (first) {
        if (has_host_) req.mutable_host()->CopyFrom(host_);
        first = false;
      }
      if (!writer->Write(req)) break;  // 流断了，走重连
    }
    writer->WritesDone();
    grpc::Status st = writer->Finish();
    if (!st.ok()) {
      LOG_WARN("reporter: stream to %s broken: %s", server_addr_.c_str(),
               st.error_message().c_str());
    } else {
      LOG_INFO("reporter: stream finished, server ack received=%llu",
               static_cast<unsigned long long>(resp.received()));
    }
    if (running_) {
      LOG_WARN("reporter: reconnect to %s in 3s", server_addr_.c_str());
      std::this_thread::sleep_for(3s);
    }
  }
}

}  // namespace agent

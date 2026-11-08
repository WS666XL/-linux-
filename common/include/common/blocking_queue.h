#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace common {

// 线程安全阻塞队列：agent 采集线程 -> 上报线程
template <typename T>
class BlockingQueue {
 public:
  void push(T v) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      q_.emplace_back(std::move(v));
    }
    cv_.notify_one();
  }

  // 阻塞取一个；队列关闭且取空后返回 nullopt
  std::optional<T> pop() {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { return closed_ || !q_.empty(); });
    if (q_.empty()) return std::nullopt;
    T v = std::move(q_.front());
    q_.pop_front();
    return v;
  }

  void close() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      closed_ = true;
    }
    cv_.notify_all();
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<T> q_;
  bool closed_ = false;
};

}  // namespace common

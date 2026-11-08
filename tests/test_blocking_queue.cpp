#include <thread>
#include <vector>

#include "common/blocking_queue.h"
#include "test_framework.h"

TEST(bq_fifo_order) {
  common::BlockingQueue<int> q;
  q.push(1);
  q.push(2);
  q.push(3);
  CHECK(q.pop().value_or(-1) == 1);
  CHECK(q.pop().value_or(-1) == 2);
  CHECK(q.pop().value_or(-1) == 3);
}

TEST(bq_close_drains_then_nullopt) {
  common::BlockingQueue<int> q;
  q.push(7);
  q.close();
  CHECK(q.pop().value_or(-1) == 7);   // 关闭后仍能取完剩余元素
  CHECK(q.pop() == std::nullopt);     // 取空后立即返回 nullopt，不阻塞
  CHECK(q.pop() == std::nullopt);
}

TEST(bq_multi_thread_handoff) {
  common::BlockingQueue<int> q;
  constexpr int kN = 1000;
  std::thread producer([&q] {
    for (int i = 0; i < kN; ++i) q.push(i);
    q.close();
  });
  std::vector<int> got;
  while (auto v = q.pop()) got.push_back(*v);
  producer.join();
  CHECK(got.size() == static_cast<size_t>(kN));
  for (int i = 0; i < kN; ++i) CHECK(got[i] == i);
}

TEST(bq_pop_blocks_until_push) {
  common::BlockingQueue<int> q;
  std::thread late([&q] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.push(42);
  });
  CHECK(q.pop().value_or(-1) == 42);  // pop 阻塞等待直到 push 唤醒
  late.join();
  q.close();
}

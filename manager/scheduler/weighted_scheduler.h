#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "monitor.pb.h"

namespace manager {

// 负载调度：维护每个节点最新采样，按业务场景加权打分选出最优节点
// 打分思路：每个维度算"剩余能力"百分比（0~100），加权求和，分高者优先
class WeightedScheduler {
 public:
  struct Weights {
    double cpu = 0.5, mem = 0.3, net = 0.1, disk = 0.1;
  };

  struct ScoredNode {
    std::string node_id;
    double score = 0;  // 0~100，越高越优
    monitor::MetricsSample latest;
  };

  explicit WeightedScheduler(Weights default_w) : default_w_(default_w) {}

  void update(const monitor::MetricsSample& s);
  std::vector<ScoredNode> rank(const std::string& scenario) const;  // 按分数降序

 private:
  Weights weights_for(const std::string& scenario) const;

  Weights default_w_;
  std::map<std::string, monitor::MetricsSample> latest_;  // node_id -> 最新采样
  mutable std::mutex mu_;
};

}  // namespace manager

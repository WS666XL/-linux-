#pragma once

#include <string>

#include "monitor.pb.h"

namespace agent {

// 采集器接口：每个采集器只负责填充 MetricsSample 中属于自己的字段
class ICollector {
 public:
  virtual ~ICollector() = default;
  virtual std::string name() const = 0;
  virtual void collect(monitor::MetricsSample* out) = 0;
};

}  // namespace agent

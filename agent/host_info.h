#pragma once

#include "monitor.pb.h"

namespace agent {

// 采集本机静态标识：主机名 / 主网卡 IP / machine-id / 内核版本 / 发行版 / 启动时间
monitor::HostInfo collect_host_info();

} // namespace agent

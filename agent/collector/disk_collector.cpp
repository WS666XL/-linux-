#include "disk_collector.h"

#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>

#include "common/logger.h"

namespace agent {

namespace {

// 是否为需要统计的物理整盘：
// 只有整盘才有 /sys/block/<name>/device；分区不在 /sys/block 下，
// loop/ram/zram/dm/md 等虚拟设备也没有 device 软链。
bool is_physical_disk(const std::string &name) {
  return ::access(("/sys/block/" + name + "/device").c_str(), F_OK) == 0;
}

} // namespace

void DiskCollector::collect(monitor::MetricsSample *out) {
  std::ifstream in("/proc/diskstats");
  if (!in.is_open()) {
    LOG_ERROR("disk_collector: open /proc/diskstats failed");
    return;
  }

  Snap cur;         // 当前采样快照
  std::string line; // 当前行
  std::string used; // 参与统计的整盘列表（首次打印，便于核对口径）
  int device_count = 0;

  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::string major, minor, name;
    if (!(ss >> major >> minor >> name))
      continue;
    if (!is_physical_disk(name))
      continue;
    uint64_t rd = 0, rm = 0, rs = 0, rt = 0, wr = 0, wm = 0, ws = 0, wt = 0,
             iof = 0, iot = 0;
    ss >> rd >> rm >> rs >> rt >> wr >> wm >> ws >> wt >> iof >> iot;
    cur.rsect += rs;    // 读扇区数
    cur.wsect += ws;    // 写扇区数
    cur.ios += rd + wr; // 读完成次数 + 写完成次数
    used += (device_count++ ? ", " : "");
    used += name;
  }

  if (!has_last_) {
    LOG_INFO("disk collector: %d physical disk(s): %s", device_count,
             used.c_str());
  }

  auto now = std::chrono::steady_clock::now();
  if (has_last_) {
    // 热插拔/重启后计数器会归零，无符号相减会回绕成巨大值，需重建基线
    if (cur.rsect < last_.rsect || cur.wsect < last_.wsect ||
        cur.ios < last_.ios) {
      LOG_WARN("disk collector: counters reset, rebaseline");
      last_ = cur;
      last_ts_ = now;
      return;
    }
    double dt = std::chrono::duration<double>(now - last_ts_).count();
    if (dt > 0) {
      // 扇区大小固定 512B
      out->set_disk_read_bps((cur.rsect - last_.rsect) * 512.0 / dt);
      out->set_disk_write_bps((cur.wsect - last_.wsect) * 512.0 / dt);
      out->set_disk_iops((cur.ios - last_.ios) / dt);
    }
  }
  last_ = cur;
  last_ts_ = now;
  has_last_ = true;
}

} // namespace agent

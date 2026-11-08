#include "host_info.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace agent {

namespace {

// 读取文件首行（machine-id 等）
std::string first_line(const char *path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}

// /etc/os-release 的 PRETTY_NAME，去掉两侧引号
std::string os_name() {
  std::ifstream in("/etc/os-release");
  std::string line;
  const std::string key = "PRETTY_NAME=";
  while (std::getline(in, line)) {
    if (line.rfind(key, 0) != 0) continue;
    std::string value = line.substr(key.size());
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    return value;
  }
  return "";
}

// /proc/net/route 里 Flags 含 RTF_GATEWAY(0x2) 且 Destination=0 的网卡
std::string default_iface() {
  std::ifstream in("/proc/net/route");
  std::string line;
  std::getline(in, line); // 跳过表头
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::string iface, dest, gateway, flags;
    if (!(ss >> iface >> dest >> gateway >> flags)) continue;
    if (dest == "00000000" && (std::strtoul(flags.c_str(), nullptr, 16) & 0x2))
      return iface;
  }
  return "";
}

// 默认路由网卡的 IPv4；没有默认路由时退回第一个非回环地址
std::string primary_ip() {
  const std::string want = default_iface();
  struct ifaddrs *addrs = nullptr;
  if (getifaddrs(&addrs) != 0) return "";

  std::string fallback;
  std::string result;
  for (auto *a = addrs; a != nullptr; a = a->ifa_next) {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
    if (!(a->ifa_flags & IFF_UP)) continue;
    char buf[INET_ADDRSTRLEN] = {};
    auto *sin = reinterpret_cast<sockaddr_in *>(a->ifa_addr);
    if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) == nullptr)
      continue;
    std::string ip = buf;
    if (ip == "127.0.0.1") continue;
    if (!want.empty() && std::string(a->ifa_name) == want) {
      result = ip; // 默认路由网卡优先
      break;
    }
    if (fallback.empty()) fallback = ip;
  }
  freeifaddrs(addrs);
  return result.empty() ? fallback : result;
}

// /proc/stat 的 btime（秒）转毫秒时间戳
int64_t boot_time_ms() {
  std::ifstream in("/proc/stat");
  std::string line;
  const std::string key = "btime ";
  while (std::getline(in, line)) {
    if (line.rfind(key, 0) != 0) continue;
    return std::strtoll(line.c_str() + key.size(), nullptr, 10) * 1000;
  }
  return 0;
}

} // namespace

monitor::HostInfo collect_host_info() {
  monitor::HostInfo h;
  char hostname[256] = {};
  if (gethostname(hostname, sizeof(hostname)) == 0) h.set_hostname(hostname);

  h.set_machine_id(first_line("/etc/machine-id"));
  if (h.machine_id().empty()) h.set_machine_id(first_line("/var/lib/dbus/machine-id"));

  utsname u{};
  if (uname(&u) == 0) h.set_kernel(u.release);

  h.set_os(os_name());
  h.set_ip(primary_ip());
  h.set_boot_time_ms(boot_time_ms());
  return h;
}

} // namespace agent

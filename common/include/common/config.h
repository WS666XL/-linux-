#pragma once

#include <map>
#include <string>

namespace common {

// 极简 INI 配置解析：[section] + key=value，'#'/';' 注释，忽略行尾空白
class Config {
 public:
  bool load(const std::string& path);

  std::string get(const std::string& section, const std::string& key,
                  const std::string& def = "") const;
  int get_int(const std::string& section, const std::string& key, int def) const;
  double get_double(const std::string& section, const std::string& key,
                    double def) const;
  bool get_bool(const std::string& section, const std::string& key, bool def) const;

 private:
  std::map<std::string, std::map<std::string, std::string>> sections_;
};

}  // namespace common

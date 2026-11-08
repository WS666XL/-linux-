#include "common/config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "common/logger.h"

namespace common {

namespace {

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

}  // namespace

bool Config::load(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    LOG_ERROR("config: cannot open %s", path.c_str());
    return false;
  }
  std::string line, cur_section;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      cur_section = trim(line.substr(1, line.size() - 2));
      continue;
    }
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    sections_[cur_section][trim(line.substr(0, eq))] =
        trim(line.substr(eq + 1));
  }
  return true;
}

std::string Config::get(const std::string& section, const std::string& key,
                        const std::string& def) const {
  auto it = sections_.find(section);
  if (it == sections_.end()) return def;
  auto kt = it->second.find(key);
  return kt == it->second.end() ? def : kt->second;
}

int Config::get_int(const std::string& section, const std::string& key,
                    int def) const {
  std::string v = get(section, key);
  return v.empty() ? def : std::atoi(v.c_str());
}

double Config::get_double(const std::string& section, const std::string& key,
                          double def) const {
  std::string v = get(section, key);
  return v.empty() ? def : std::atof(v.c_str());
}

bool Config::get_bool(const std::string& section, const std::string& key,
                      bool def) const {
  std::string v = get(section, key);
  return v.empty() ? def : (v == "true" || v == "1" || v == "yes" || v == "on");
}

}  // namespace common

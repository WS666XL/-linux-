#include <cstdio>
#include <fstream>

#include "common/config.h"
#include "test_framework.h"

namespace {

// 写临时 INI 文件供解析测试
std::string write_tmp_ini(const std::string& content) {
  std::string path = "/tmp/distmon_test_" + std::to_string(rand()) + ".ini";
  std::ofstream out(path);
  out << content;
  return path;
}

}  // namespace

TEST(config_missing_file) {
  common::Config cfg;
  CHECK(!cfg.load("/tmp/no_such_file_deadbeef.ini"));
}

TEST(config_parse_basic) {
  std::string path = write_tmp_ini(
      "# comment line\n"
      "; another comment\n"
      "[agent]\n"
      "node_id = node-1   \n"
      "interval_ms=2000\n"
      "\n"
      "[mysql]\n"
      "enable = true\n"
      "port = 3306\n"
      "ratio = 0.75\n"
      "off_key = off\n");
  common::Config cfg;
  CHECK(cfg.load(path));

  CHECK(cfg.get("agent", "node_id") == "node-1");     // 行尾空白已 trim
  CHECK(cfg.get_int("agent", "interval_ms", 0) == 2000); // 等号两侧空格
  CHECK(cfg.get_bool("mysql", "enable", false) == true);
  CHECK(cfg.get_int("mysql", "port", 0) == 3306);
  CHECK_NEAR(cfg.get_double("mysql", "ratio", 0.0), 0.75, 1e-9);
  CHECK(cfg.get_bool("mysql", "off_key", true) == false); // off 不算 true
}

TEST(config_defaults) {
  common::Config cfg;
  CHECK(cfg.load("/dev/null"));  // 空文件合法
  CHECK(cfg.get("no_such", "key", "fallback") == "fallback");
  CHECK(cfg.get_int("no_such", "key", 42) == 42);
  CHECK(cfg.get_bool("no_such", "key", true) == true);
  CHECK_NEAR(cfg.get_double("no_such", "key", 3.14), 3.14, 1e-9);
}

TEST(config_bool_variants) {
  std::string path = write_tmp_ini(
      "[s]\n"
      "k1 = yes\n"
      "k2 = on\n"
      "k3 = 1\n"
      "k4 = false\n"
      "k5 = 0\n"
      "k6 = random\n");
  common::Config cfg;
  CHECK(cfg.load(path));
  CHECK(cfg.get_bool("s", "k1", false) == true);
  CHECK(cfg.get_bool("s", "k2", false) == true);
  CHECK(cfg.get_bool("s", "k3", false) == true);
  CHECK(cfg.get_bool("s", "k4", true) == false);
  CHECK(cfg.get_bool("s", "k5", true) == false);
  CHECK(cfg.get_bool("s", "k6", true) == false);  // 非法串不算 true
}

TEST(config_keyless_line_ignored) {
  std::string path = write_tmp_ini("[s]\nnot_a_kv_line\nk = v\n");
  common::Config cfg;
  CHECK(cfg.load(path));
  CHECK(cfg.get("s", "k") == "v");
}

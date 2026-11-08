#pragma once

// 极简单元测试框架：无第三方依赖，静态注册 + CHECK 断言
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

inline std::vector<std::pair<std::string, std::function<void()>>>&
test_registry() {
  static std::vector<std::pair<std::string, std::function<void()>>> t;
  return t;
}

inline int& test_failures() {
  static int f = 0;
  return f;
}

#define TEST(name)                                                        \
  static void name();                                                     \
  static const bool reg_##name = (test_registry().push_back(              \
                                      {#name, name}),                     \
                                  true);                                  \
  static void name()

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      ++test_failures();                                                \
      std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                   \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                          \
  do {                                                                 \
    if ((a) < (b) - (eps) || (a) > (b) + (eps)) {                      \
      ++test_failures();                                               \
      std::printf("    FAIL %s:%d  %s=%f ~ %s(±%f)\n", __FILE__,       \
                  __LINE__, #a, static_cast<double>(a), #b, eps);      \
    }                                                                  \
  } while (0)

inline int test_run_all() {
  auto& ts = test_registry();
  std::printf("running %zu tests\n", ts.size());
  for (const auto& [name, fn] : ts) {
    int before = test_failures();
    fn();
    std::printf("[ %s ] %s\n",
                test_failures() == before ? "PASS" : "FAIL", name.c_str());
  }
  std::printf("%d failure(s)\n", test_failures());
  return test_failures() == 0 ? 0 : 1;
}

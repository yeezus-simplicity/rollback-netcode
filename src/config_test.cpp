// config_test.cpp — 优雅退出与配置加载的验证
//
// 用法: ./config_test

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "net/shutdown.h"

using namespace synq;

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "\xE2\x9C\x94" : "\xE2\x9C\x98", what);
  if (!ok) ++g_fail;
}
}

static void test_config_file() {
  std::printf("\n[1] 配置文件加载\n");
  const char* path = "/tmp/synq_test.conf";
  std::ofstream f(path);
  f << "# 端口\n"
       "port = 8899\n"
       "\n"
       "tick_rate=30\n"
       "workers = 4   # 行尾注释\n"
       "enable_metrics = true\n"
       "loss_rate = 0.01\n";
  f.close();

  Config c;
  check(c.load_file(path), "加载配置文件成功");
  check(c.get_int("port", 0) == 8899, "port=8899（空格容错）");
  check(c.get_int("tick_rate", 0) == 30, "tick_rate=30（无空格）");
  check(c.get_int("workers", 0) == 4, "workers=4（行尾注释剥离）");
  check(c.get_bool("enable_metrics", false), "enable_metrics=true");
  std::printf("      loss_rate 解析为 %.3f\n", c.get_double("loss_rate", 0));
  check(c.get_double("loss_rate", 0) > 0.009 &&
        c.get_double("loss_rate", 0) < 0.011, "loss_rate=0.01");
  check(c.get_int("not_exist", 42) == 42, "缺失键返回默认值");
  check(c.get_int("port", 0) != 42, "存在的键不被默认值覆盖");
  std::remove(path);
}

static void test_env_override() {
  std::printf("\n[2] 环境变量覆盖配置文件\n");
  Config c;
  c.set("port", "1234");
  check(c.get_int("port", 0) == 1234, "先取配置文件值");
#if defined(_WIN32)
  _putenv_s("port", "5678");
#else
  ::setenv("port", "5678", 1);
#endif
  check(c.get_int("port", 0) == 5678, "环境变量优先级更高");
}

static void test_argv_override() {
  std::printf("\n[3] 命令行覆盖环境变量\n");
  Config c;
  c.set("port", "1234");
#if defined(_WIN32)
  _putenv_s("port", "5678");
#else
  ::setenv("port", "5678", 1);
#endif
  char arg0[] = "prog", a1[] = "--port=9012", junk[] = "junk";
  char* argv[] = {arg0, a1, junk, nullptr};
  c.apply_arg(3, argv);
  check(c.get_int("port", 0) == 9012, "命令行优先级最高");
  check(c.get_int("junk", 7) == 7, "非 -- 前缀参数被忽略");
}

static void test_bad_values() {
  std::printf("\n[4] 非法值降级\n");
  // 【测试隔离】前几个测试用_putenv_s 写过 port 环境变量，
  // 而 get_int 的优先级是「环境变量 > 文件层」，所以这里必须先清掉，
  // 否则读到的还是上一步残留的 "9012"，测试结果依赖执行顺序（脆弱）。
#if defined(_WIN32)
  _putenv_s("port", "");
#else
  ::unsetenv("port");
#endif
  Config c;
  c.set("port", "not_a_number");
  check(c.get_int("port", 8080) == 8080, "非数字 -> 返回默认值");
  c.set("flag", "yes");
  check(c.get_bool("flag", false), "yes 解析为 true");
  c.set("flag2", "0");
  check(!c.get_bool("flag2", true), "0 解析为 false");
  c.set("flag3", "ON");
  check(c.get_bool("flag3", false), "ON 解析为 true（大小写）");
}

static void test_shutdown_flag() {
  std::printf("\n[5] 退出标志\n");
  auto& s = ShutdownSignal::instance();
  s.reset();
  check(!s.requested(), "初始为未请求退出");
  s.request();
  check(s.requested(), "request() 后为已请求");
  // wait_for 在已置位时应立即返回 true
  check(s.wait_for(std::chrono::milliseconds(50)), "已请求时 wait_for 立即返回");
  s.reset();
  check(!s.requested(), "reset() 可复位");
}

static void test_shutdown_timeout() {
  std::printf("\n[6] 退出等待超时（防卡死）\n");
  auto& s = ShutdownSignal::instance();
  s.reset();
  const auto t0 = std::chrono::steady_clock::now();
  const bool got = s.wait_for(std::chrono::milliseconds(120));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
  std::printf("      等待 %lld ms 后返回 %s\n", (long long)ms, got ? "true" : "false");
  check(!got, "未请求时返回 false");
  check(ms >= 100 && ms < 400, "实际等待时间接近超时值（说明真的等了）");
  // 后台置位，验证能被唤醒
  std::thread th([&s] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    s.request();
  });
  const bool got2 = s.wait_for(std::chrono::milliseconds(1000));
  th.join();
  check(got2, "后台置位后 wait_for 被唤醒返回 true");
  s.reset();
}

int main() {
  std::printf("=== 优雅退出与配置 验证 ===\n");
  test_config_file();
  test_env_override();
  test_argv_override();
  test_bad_values();
  test_shutdown_flag();
  test_shutdown_timeout();
  std::printf("\n========================================\n");
  if (g_fail == 0) { std::printf("全部通过\n"); return 0; }
  std::printf("失败 %d 项\n", g_fail);
  return 1;
}

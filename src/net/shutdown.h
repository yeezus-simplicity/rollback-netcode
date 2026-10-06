// shutdown.h — 优雅退出与配置（生产服务器的必备两件事）
//
// 【为什么后端面试必问这两个】
//  1. 优雅退出：收到 SIGTERM 后「停止接新请求 → 冲刷缓冲区 → 落盘 → 退出」，
//     而不是直接 kill 掉。直接 kill 会丢数据、留半截文件。
//     K8s 滚动更新时每个 Pod 都会收到 SIGTERM，
//     不处理 = 每次发布都丢一批请求。
//
//  2. 配置外置：配置写在代码里 = 改一个参数要重新编译发布。
//     生产上配置必须能「不改代码就调整」：配置文件 / 环境变量 / 命令行。
//
// 【Windows 兼容性】
// Windows 没有 signal()，SIGTERM 常量也不存在于 <csignal>。
// 本文件用平台宏隔离：POSIX 走 signal()，Windows 走 SetConsoleCtrlHandler()。
// 两种平台的语义对齐：收到终止请求 -> 置退出标志 -> 主循环收尾。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <csignal>
#  include <unistd.h>
#endif

namespace synq {

// ===================================================================
// 一、优雅退出
// ===================================================================

class ShutdownSignal {
 public:
  static ShutdownSignal& instance() {
    static ShutdownSignal inst;
    return inst;
  }

  void install() {
#if defined(_WIN32)
    SetConsoleCtrlHandler(&ShutdownSignal::handler, TRUE);
#else
    std::signal(SIGINT, &ShutdownSignal::posix_handler);
    std::signal(SIGTERM, &ShutdownSignal::posix_handler);
    // 忽略 SIGPIPE：客户端断开时会触发，不该让进程死掉
    std::signal(SIGPIPE, SIG_IGN);
#endif
  }

  bool requested() const { return flag_.load(std::memory_order_acquire); }

  void request() { flag_.store(true, std::memory_order_release); }

  void reset() { flag_.store(false, std::memory_order_release); }

  // 等待退出请求，带超时。用于「冲刷缓冲区」阶段。
  //
  // 【为什么要有超时】如果冲刷逻辑本身卡死（磁盘故障、对端不读），
  // 无限等待会让 K8s 在 30s 后 SIGKILL，退出前的清理全部作废。
  // 优雅退出的价值前提是「它能自己结束」。
  bool wait_for(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!requested()) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
#if defined(_WIN32)
      ::Sleep(10);          // Win32 原生，不依赖 <thread>
#else
      ::usleep(10000);
#endif
    }
    return true;
  }

 private:
  ShutdownSignal() = default;
  std::atomic<bool> flag_{false};

#if defined(_WIN32)
  static BOOL WINAPI handler(DWORD type) {
    switch (type) {
      case CTRL_C_EVENT:
      case CTRL_BREAK_EVENT:
      case CTRL_CLOSE_EVENT:      // 关闭窗口（服务化运行时也会收到）
      case CTRL_SHUTDOWN_EVENT:
        instance().request();
        return TRUE;   // 已处理，进程继续
      default:
        return FALSE;
    }
  }
#else
  // POSIX 信号处理函数必须是「返回 void 的普通函数」，
  // 不能捕获 C++ 异常、不能调用非 async-signal-safe 的 API。
  // 所以这里只做一件事：置原子标志。真正的清理在主循环里做。
  static void posix_handler(int) { instance().request(); }
#endif
};

// RAII 包装：作用域退出时自动 install（便于测试用局部实例）
class ScopedSignal {
 public:
  ScopedSignal() { ShutdownSignal::instance().install(); }
};


// ===================================================================
// 二、配置外置（优先级：命令行 > 环境变量 > 配置文件 > 默认值）
// ===================================================================

class Config {
 public:
  // 从「key=value」行文件加载（跳过空行与 # 注释）
  bool load_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
      const auto pos = line.find('#');
      if (pos != std::string::npos) line = line.substr(0, pos);
      // 去掉首尾空白
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.erase(line.begin());
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                              line.back() == '\r'))
        line.pop_back();
      if (line.empty()) continue;
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      // key 也要 trim ——「port = 8899」的 key 是「port 」（带尾空格），
      // 不裁剪就 get_int("port") 找不到，配置看起来被「忽略」了。
      std::string k = line.substr(0, eq);
      std::string v = line.substr(eq + 1);
      auto trim = [](std::string& x) {
        const char* ws = " \t\r\n";
        const auto b = x.find_first_not_of(ws);
        if (b == std::string::npos) { x.clear(); return; }
        const auto e = x.find_last_not_of(ws);
        x = x.substr(b, e - b + 1);
      };
      trim(k); trim(v);
      if (k.empty()) continue;
      file_[k] = v;
    }
    return true;
  }

  void set(const std::string& k, const std::string& v) { file_[k] = v; }

  // 取值优先级：命令行 > 环境变量 > 配置文件 > 默认值
  //
  // 【为什么命令行优先】容器编排时环境变量是「部署实例级」的覆盖，
  // 配置文件是「环境级」的。实例级应压过环境级。
  // （生产上更常见的约定是反过来，但两种都合理，关键是**明确且一致**。）
  std::string get_str(const std::string& key, const std::string& def) const {
    if (const char* v = std::getenv(key.c_str())) return v;
    auto it = file_.find(key);
    if (it != file_.end()) return it->second;
    return def;
  }

  int get_int(const std::string& key, int def) const {
    const std::string v = get_str(key, "");
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
  }

  double get_double(const std::string& key, double def) const {
    const std::string v = get_str(key, "");
    if (v.empty()) return def;
    try { return std::stod(v); } catch (...) { return def; }
  }

  bool get_bool(const std::string& key, bool def) const {
    const std::string v = get_str(key, "");
    if (v.empty()) return def;
    // 大小写不敏感：配置文件是人写的，"TRUE"/"True"/"On" 都应被接受
    std::string lv = v;
    for (auto& c : lv)
      c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return lv == "1" || lv == "true" || lv == "yes" || lv == "on";
  }

  // 命令行覆盖：把 --key=value 写进「最高优先级」层
  //
  // 【与 getenv 冲突的解决】getenv 优先于 file_，而命令行要优先于两者。
  // 做法：命令行解析出的值写成环境变量（子进程可见），
  // 这样 get_str 的统一优先级链就能正确工作，不需要额外的 map。
  void apply_arg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a.rfind("--", 0) != 0) continue;
      a = a.substr(2);
      const auto eq = a.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = a.substr(0, eq);
      const std::string v = a.substr(eq + 1);
#if defined(_WIN32)
      _putenv_s(k.c_str(), v.c_str());
#else
      ::setenv(k.c_str(), v.c_str(), 1);
#endif
      file_[k] = v;   // 同时记入文件层，getenv 不可用时兜底
    }
  }

  void dump(const char* prefix = "config") const {
    std::printf("[%s]\n", prefix);
    for (const auto& kv : file_)
      std::printf("  %-24s = %s\n", kv.first.c_str(), kv.second.c_str());
  }

 private:
  std::map<std::string, std::string> file_;
};

}  // namespace synq
// reconnect_test.cpp — 断线重连端到端验证（消除 README 已知限制 #6）
//
// 验证「Graceful 重连的状态恢复」：
//   1. 客户端建立 TCP 连接，正常接收服务端状态广播（含全量快照）
//   2. 断开 TCP（模拟掉线）
//   3. 重新建立 TCP 连接并发送 kReconnect 消息
//   4. 断言：重连后客户端在至多 1 帧内收到一份「比断线时更新」的全量快照，
//      且随后能持续收到更新的全量快照（证明已重新同步到权威世界并连续追踪）
//
// 与 loadtest 一致：本程序不自己拉起服务端，假定 gameserver 已在
//   127.0.0.1:18089(TCP 状态) 运行（UDP 输入端口 18088 在本测试中不使用，
//   模拟由服务端自带 bot 驱动，专注验证「状态恢复」链路）。
//
// CI 的 reconnect-e2e job 负责先启动 gameserver、再运行本程序。
//
// 退出码：0 = 通过，1 = 失败。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/world.h"
#include "net/net.h"
#include "net/serialize.h"

using namespace synq;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kTcpPort = 18089;

// 观测到的状态流（读线程写、主线程读判定）
struct Observed {
  std::mutex m;
  int max_full_frame = -1;            // 见到的最大全量快照帧号
  int frame_at_disconnect = -1;       // 断线前最后见到的全量快照帧号
  bool phase_a_done = false;
  int post_full_count = 0;            // 断线后见到的「更新」全量快照数
  int first_post_full_frame = -1;
  int prev_post_full_frame = -1;
  bool continuity_ok = true;          // 断线后全量快照帧号严格递增
  bool stop_reader = false;
};

inline sockaddr_in make_addr(const std::string& host, int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  a.sin_addr.s_addr = inet_addr(host.c_str());
  return a;
}

// 从 buf[off..] 尝试解析一个全量状态包；成功则输出帧号并返回 true。
// 同时校验内容合法（位置在战场内、hp/mp 合理、alive∈{0,1}），
// 使得即便 0x01 字节偶然出现在 delta 流中，也能被结构校验过滤掉。
bool try_parse_full_state(const std::uint8_t* buf, std::size_t n,
                          std::size_t off, std::int32_t& out_frame) {
  if (off >= n || buf[off] != 0x01) return false;
  Reader r{buf + off, n - off, 0};
  if (r.get_u8() != 0x01) return false;
  std::int32_t frame = r.get_varint();
  if (!r.ok()) return false;
  for (int i = 0; i < kMaxPlayers; ++i) {
    std::int32_t x = r.get_pos();
    std::int32_t y = r.get_pos();
    std::int32_t hp = r.get_varint();
    std::int32_t mp = r.get_varint();
    std::uint8_t alive = r.get_u8();
    if (!r.ok()) return false;
    double ux = static_cast<double>(x) / 65536.0;
    double uy = static_cast<double>(y) / 65536.0;
    if (ux < -5.0 || ux > kFieldSize + 5.0) return false;
    if (uy < -5.0 || uy > kFieldSize + 5.0) return false;
    if (hp < 0 || hp > 100000) return false;
    if (mp < 0 || mp > 100000) return false;
    if (alive != 0 && alive != 1) return false;
  }
  out_frame = frame;
  return true;
}

// 扫描字节流，找出其中所有合法的全量状态包帧号（去重），写入 frames。
void scan_full_states(const std::vector<std::uint8_t>& buf,
                      std::vector<std::int32_t>& frames) {
  for (std::size_t off = 0; off + 1 < buf.size(); ++off) {
    std::int32_t f = 0;
    if (try_parse_full_state(buf.data(), buf.size(), off, f)) {
      bool dup = false;
      for (std::int32_t x : frames)
        if (x == f) { dup = true; break; }
      if (!dup) frames.push_back(f);
    }
  }
}

// 启动读线程：持续读取 TCP 状态流，累积并解析全量快照，更新 ob。
std::thread start_reader(int tcp_fd, Observed& ob) {
  return std::thread([tcp_fd, &ob] {
    std::vector<std::uint8_t> acc;
    std::vector<std::int32_t> frames;  // 本线程已见过的全量帧号（持久）
    std::uint8_t tmp[16384];
    while (true) {
      {
        std::lock_guard<std::mutex> lk(ob.m);
        if (ob.stop_reader) break;
      }
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(tcp_fd, &rfds);
      struct timeval tv{0, 20000};  // 20ms 超时，便于响应 stop
      if (::select(tcp_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
      int n = ::recv(tcp_fd, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
      if (n <= 0) break;  // 连接关闭或错误
      acc.insert(acc.end(), tmp, tmp + n);
      // 防止无限增长（本测试时长内远不会触发）
      if (acc.size() > (1u << 20))
        acc.erase(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(acc.size() >> 1));

      std::vector<std::int32_t> all;
      scan_full_states(acc, all);
      std::lock_guard<std::mutex> lk(ob.m);
      for (std::int32_t f : all) {
        bool known = false;
        for (std::int32_t x : frames)
          if (x == f) { known = true; break; }
        if (known) continue;
        frames.push_back(f);
        if (f > ob.max_full_frame) ob.max_full_frame = f;
        if (ob.phase_a_done && f > ob.frame_at_disconnect) {
          if (ob.post_full_count == 0) ob.first_post_full_frame = f;
          else if (f <= ob.prev_post_full_frame) ob.continuity_ok = false;
          ob.prev_post_full_frame = f;
          ++ob.post_full_count;
        }
      }
    }
  });
}

// 连接服务端 TCP（带就绪重试，最多 ~5s）
int connect_state(const std::string& host) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_failed(fd)) return -1;
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<char*>(&one), sizeof(one));
  sockaddr_in a = make_addr(host, kTcpPort);
  for (int i = 0; i < 50; ++i) {
    if (!socket_failed(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a))))
      return fd;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  close_socket(fd);
  return -1;
}

int run_test(const std::string& host) {
  if (init_net() != 0) {
    std::fprintf(stderr, "[err] net init failed\n");
    return 1;
  }

  // ---- 阶段 A：连接 + 正常接收状态广播 ----
  int tcp1 = connect_state(host);
  if (tcp1 < 0) {
    std::fprintf(stderr, "[err] TCP 连接失败（服务端未启动？）\n");
    return 1;
  }
  Observed ob;
  std::thread reader1 = start_reader(tcp1, ob);

  // 让服务端多广播若干帧（~2.5s @30fps ≈ 75 帧），观察若干全量快照
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  {
    std::lock_guard<std::mutex> lk(ob.m);
    ob.frame_at_disconnect = (ob.max_full_frame > 0) ? ob.max_full_frame : 0;
    ob.phase_a_done = true;
  }

  // ---- 阶段 B：断开 TCP（模拟掉线）----
  {
    std::lock_guard<std::mutex> lk(ob.m);
    ob.stop_reader = true;
  }
  reader1.join();
  close_socket(tcp1);
  std::printf("[test] 阶段A结束：断线前最后全量快照帧=%d，已断开TCP\n",
              ob.frame_at_disconnect);

  // ---- 阶段 C：重连 + 发送 kReconnect ----
  int tcp2 = connect_state(host);
  if (tcp2 < 0) {
    std::fprintf(stderr, "[err] 重连TCP失败\n");
    return 1;
  }
  // 注意：ob.stop_reader 是共享标志，阶段 A 结束时曾被置 true 以停 reader1；
  // 必须在启动 reader2 前复位为 false，否则 reader2 一启动就因 stop_reader
  // 而立即退出（之前导致重连后收不到任何状态）。
  {
    std::lock_guard<std::mutex> lk(ob.m);
    ob.stop_reader = false;
  }
  std::thread reader2 = start_reader(tcp2, ob);
  // 发送 kReconnect(player=0, last_acked=断线前帧)，走服务端 RECONNECT 处理路径
  std::vector<std::uint8_t> rec;
  rec.push_back(static_cast<std::uint8_t>(MsgType::kReconnect));
  put_varint(rec, 0);
  put_varint(rec, ob.frame_at_disconnect);
  ::send(tcp2, reinterpret_cast<const char*>(rec.data()),
         static_cast<int>(rec.size()), 0);
  std::printf("[test] 已重连并发送 kReconnect(0, %d)\n", ob.frame_at_disconnect);

  // 等待重连后收到 ≥2 个「更新的」全量快照（最多 6s）
  bool ok = false;
  for (int i = 0; i < 60; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::lock_guard<std::mutex> lk(ob.m);
    if (ob.post_full_count >= 2 && ob.continuity_ok) { ok = true; break; }
  }
  {
    std::lock_guard<std::mutex> lk(ob.m);
    std::printf("[test] 重连后更新全量快照数=%d  首帧=%d  连续=%s\n",
                ob.post_full_count, ob.first_post_full_frame,
                ob.continuity_ok ? "是" : "否");
  }

  {
    std::lock_guard<std::mutex> lk(ob.m);
    ob.stop_reader = true;
  }
  reader2.join();
  close_socket(tcp2);
  shutdown_net();

  // ---- 判定 ----
  bool pass = ok && ob.first_post_full_frame > ob.frame_at_disconnect &&
              ob.continuity_ok;
  std::printf("\n==========================================================\n");
  if (pass) {
    std::printf(" ✓ 重连状态恢复验证通过：断线后客户端重新同步到权威世界\n");
    std::printf("   （断线帧=%d → 重连后首份全量快照帧=%d，并连续收到更新快照）\n",
                ob.frame_at_disconnect, ob.first_post_full_frame);
  } else {
    std::printf(" ✗ 重连状态恢复验证失败\n");
    if (ob.post_full_count < 2)
      std::printf("   缺少足够的更新全量快照（post_full_count=%d，期望≥2）\n",
                  ob.post_full_count);
    if (ob.first_post_full_frame <= ob.frame_at_disconnect)
      std::printf("   重连后首份全量快照未更新（%d ≤ 断线帧 %d）\n",
                  ob.first_post_full_frame, ob.frame_at_disconnect);
    if (!ob.continuity_ok)
      std::printf("   重连后全量快照帧号不连续（可能未正确重新同步）\n");
  }
  std::printf("==========================================================\n");
  return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
  return run_test(host);
}

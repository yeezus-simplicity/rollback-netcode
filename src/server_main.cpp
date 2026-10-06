// server_main.cpp — 游戏服务器主程序
//
// 架构（贴近真实游戏服）：
//   UDP  :18088  接收玩家输入包（高频、可丢）
//   TCP  :18089  广播状态 + 握手/重连（可靠、有序）
//   Tick 30Hz   固定步进推进模拟
//
// 线程模型：
//   main thread  : 30Hz tick 循环（模拟推进 + 状态广播）
//   udp thread   : 阻塞 recvfrom，处理输入包
//   tcp thread   : accept + 每连接独立发送（状态广播）
//   client sim   : 内置机器人客户端，模拟真实玩家（含延迟/丢包）
//
// 用法: ./gameserver [player_count] [latency_ms] [jitter_ms] [loss_pct] [frames]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/rollback.h"
#include "core/world.h"
#include "net/net.h"
#include "net/serialize.h"

using namespace synq;
using Clock = std::chrono::steady_clock;

namespace {
constexpr int kUdpPort = 18088;
constexpr int kTcpPort = 18089;
int one_ = 1;

int make_udp_listener(int port) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  auto* so = reinterpret_cast<char*>(&one_);
  ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_REUSEADDR, so,
               sizeof(one_));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close_socket(fd);
    return -1;
  }
  return fd;
}

int make_tcp_listener(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  auto* so = reinterpret_cast<char*>(&one_);
  ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_REUSEADDR, so,
               sizeof(one_));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(fd, SOMAXCONN) < 0) {
    close_socket(fd);
    return -1;
  }
  return fd;
}

Command gen_command(std::uint64_t seed, int tick, int player) {
  Rng r;
  r.reset(seed ^ (static_cast<std::uint64_t>(tick) << 32) ^
          static_cast<std::uint64_t>(player) * 0x9E3779B97F4A7C15ULL);
  Command c;
  std::int32_t mv = r.range(0, 3);
  c.move_x = (mv == 0) ? -1 : (mv == 1 ? 1 : 0);
  c.move_y = (r.range(0, 1) == 0) ? -1 : 0;
  if (r.range(0, 3) == 0) {
    c.attack_target = r.range(0, kMaxPlayers - 1);
    if (c.attack_target == player) c.attack_target = (player + 1) % kMaxPlayers;
  }
  if (r.range(0, 7) == 0) c.cast_spell = 1;
  else if (r.range(0, 11) == 0) c.cast_spell = 2;
  return c;
}

// 客户端模拟：产生输入包，按配置的延迟/抖动/丢包发送
struct ClientBot {
  int player_id = 0;
  int udp_fd = -1;
  sockaddr_in server_addr{};
  int latency_ms = 0;
  int jitter_ms = 0;
  int loss_pct = 0;
  std::uint64_t seed = 0;
  std::atomic<bool> running{true};
  std::atomic<std::int64_t> pkts_sent{0};
  std::atomic<std::int64_t> pkts_lost{0};

  // 在途包队列（模拟网络延迟）
  struct Inflight {
    std::int64_t deliver_at_ms;
    std::vector<std::uint8_t> data;
  };
  std::vector<Inflight> inflight;

  void start() {
    udp_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(static_cast<std::uint16_t>(kUdpPort));
    running = true;
  }

  void send_frame(int frame) {
    Command c = gen_command(seed, frame, player_id);
    // 丢包模拟
    if (loss_pct > 0) {
      Rng r;
      r.reset(seed + static_cast<std::uint64_t>(frame) * 31 + 7);
      if (r.range(0, 99) < loss_pct) {
        pkts_lost.fetch_add(1);
        return;
      }
    }
    auto pkt = serialize_input(frame, player_id, c);
    // 计算到达时间（含延迟 + 抖动）
    Rng jr;
    jr.reset(seed + static_cast<std::uint64_t>(frame) * 17 + 3);
    int lat = latency_ms + (jitter_ms > 0 ? jr.range(-jitter_ms, jitter_ms) : 0);
    if (lat < 0) lat = 0;
    Inflight f;
    f.data = std::move(pkt);
    f.deliver_at_ms = now_ms() + lat;
    inflight.push_back(std::move(f));
  }

  // 每 tick 调用：投递到期的包
  void flush() {
    std::int64_t now = now_ms();
    std::size_t i = 0;
    while (i < inflight.size()) {
      if (inflight[i].deliver_at_ms <= now) {
        ::sendto(udp_fd, reinterpret_cast<const char*>(inflight[i].data.data()),
                 static_cast<int>(inflight[i].data.size()), 0,
                 reinterpret_cast<sockaddr*>(&server_addr),
                 sizeof(server_addr));
        pkts_sent.fetch_add(1);
        inflight.erase(inflight.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
  }

  static std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (init_net() != 0) {
    std::fprintf(stderr, "net init failed\n");
    return 1;
  }

  int nplayers = argc > 1 ? std::atoi(argv[1]) : 4;
  int latency = argc > 2 ? std::atoi(argv[2]) : 60;
  int jitter = argc > 3 ? std::atoi(argv[3]) : 15;
  int loss = argc > 4 ? std::atoi(argv[4]) : 1;
  int frames = argc > 5 ? std::atoi(argv[5]) : 1800;

  if (nplayers < 1) nplayers = 1;
  if (nplayers > kMaxPlayers) nplayers = kMaxPlayers;

  printf("==========================================================\n");
  printf(" synq 帧同步游戏服务器\n");
  printf("==========================================================\n");
  printf(" 配置: 玩家=%d  延迟=%dms(±%d)  丢包=%d%%  时长=%d 帧(%.1fs @30fps)\n",
         nplayers, latency, jitter, loss, frames,
         frames / static_cast<double>(kTickRate));
  printf(" 端点: UDP :%d (输入)   TCP :%d (状态)\n\n", kUdpPort, kTcpPort);

  BattleRoom room(static_cast<std::uint64_t>(12345));

  int udp_fd = make_udp_listener(kUdpPort);
  if (udp_fd < 0) {
    std::fprintf(stderr, "UDP listen failed (err=%d)\n", SOCK_ERROR);
    return 1;
  }
  int tcp_fd = make_tcp_listener(kTcpPort);
  if (tcp_fd < 0) {
    std::fprintf(stderr, "TCP listen failed\n");
    return 1;
  }

  // ---- TCP 状态广播线程 ----
  std::atomic<bool> running{true};
  std::vector<int> tcp_clients;
  std::mutex tcp_mutex;
  std::thread tcp_thread([&] {
    struct timeval tv{0, 50000};
    ::setsockopt(static_cast<SOCKET>(tcp_fd), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<char*>(&tv), sizeof(tv));
    while (running.load(std::memory_order_relaxed)) {
      sockaddr_in cli{};
      socklen_t cl = sizeof(cli);
      int cfd = ::accept(tcp_fd, reinterpret_cast<sockaddr*>(&cli), &cl);
      if (cfd < 0) continue;
      auto* so = reinterpret_cast<char*>(&one_);
      ::setsockopt(static_cast<SOCKET>(cfd), IPPROTO_TCP, TCP_NODELAY, so,
                   sizeof(one_));
      {
        std::lock_guard<std::mutex> lk(tcp_mutex);
        tcp_clients.push_back(cfd);
      }
      // 立即下发全量状态（等价于握手/重连后的快照）
      auto full = room.build_state_packet(true);
      ::send(cfd, reinterpret_cast<const char*>(full.data()),
             static_cast<int>(full.size()), 0);
      std::printf("[TCP] 客户端接入，全量状态 %zu 字节\n", full.size());
      std::fflush(stdout);
    }
  });

  // ---- UDP 输入线程 ----
  std::thread udp_thread([&] {
    std::uint8_t buf[2048];
    while (running.load(std::memory_order_relaxed)) {
      struct timeval tv{0, 10000};
      ::setsockopt(static_cast<SOCKET>(udp_fd), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<char*>(&tv), sizeof(tv));
      sockaddr_in from{};
      socklen_t fl = sizeof(from);
      ssize_t n = ::recvfrom(udp_fd, reinterpret_cast<char*>(buf), sizeof(buf),
                             0, reinterpret_cast<sockaddr*>(&from), &fl);
      if (n <= 0) continue;
      room.on_input_packet(buf, static_cast<std::size_t>(n));
    }
  });

  // ---- 客户端机器人 ----
  std::vector<ClientBot> bots(static_cast<std::size_t>(nplayers));
  for (int p = 0; p < nplayers; ++p) {
    bots[static_cast<std::size_t>(p)].player_id = p;
    bots[static_cast<std::size_t>(p)].latency_ms = latency;
    bots[static_cast<std::size_t>(p)].jitter_ms = jitter;
    bots[static_cast<std::size_t>(p)].loss_pct = loss;
    // 让不同玩家延迟不同，模拟真实分布
    bots[static_cast<std::size_t>(p)].latency_ms += p * 20;
    bots[static_cast<std::size_t>(p)].seed =
        0xABCDEF00ULL + static_cast<std::uint64_t>(p) * 7919;
    bots[static_cast<std::size_t>(p)].start();
  }

  // ---- 30Hz tick 循环 ----
  printf("--- 开战 ---\n");
  auto tick_interval = std::chrono::microseconds(1000000 / kTickRate);
  auto next_tick = Clock::now();
  std::vector<std::int64_t> roll_hist(frames, 0);
  std::int64_t total_rolls = 0;

  for (int f = 0; f < frames; ++f) {
    next_tick += tick_interval;

    // 1. 客户端产生并发送本帧输入
    for (auto& b : bots) b.send_frame(f);
    // 2. 投递已到达的包（UDP 线程可能还没处理完，这里等一下）
    for (auto& b : bots) b.flush();

    // 3. 推进模拟
    std::int32_t rolls = room.tick();
    roll_hist[static_cast<std::size_t>(f)] = rolls;
    total_rolls += rolls;

    // 4. 广播状态（增量）
    if (f % kTickRate == 0) {
      auto full = room.build_state_packet(true);
      std::lock_guard<std::mutex> lk(tcp_mutex);
      for (int c : tcp_clients)
        ::send(c, reinterpret_cast<const char*>(full.data()),
               static_cast<int>(full.size()), 0);
    } else {
      auto d = room.build_state_packet(false);
      std::lock_guard<std::mutex> lk(tcp_mutex);
      for (int c : tcp_clients)
        ::send(c, reinterpret_cast<const char*>(d.data()),
               static_cast<int>(d.size()), 0);
    }

    // 5. 模拟真实 tick 时钟
    std::this_thread::sleep_until(next_tick);
  }

  running = false;
  close_socket(udp_fd);
  close_socket(tcp_fd);
  udp_thread.detach();
  tcp_thread.detach();
  for (auto& b : bots) b.running = false;
  if (bots[0].udp_fd >= 0) close_socket(bots[0].udp_fd);

  // ---- 报告 ----
  const auto& ns = room.net_stats();
  std::int64_t sent = 0, lost = 0;
  for (auto& b : bots) {
    sent += b.pkts_sent.load();
    lost += b.pkts_lost.load();
  }
  std::int64_t rolls_n = 0;
  std::int32_t max_roll = 0;
  for (auto v : roll_hist) {
    if (v > 0) ++rolls_n;
    if (v > max_roll) max_roll = v;
  }

  const World& w = room.world();
  std::printf("\n==========================================================\n");
  std::printf(" 对局结束  (tick=%d)\n", room.tick_count());
  std::printf("==========================================================\n");
  std::printf("【网络统计】\n");
  std::printf("  输入包接收 : %llu 个 / %llu 字节 (均值 %.1f B/包)\n",
              (unsigned long long)ns.input_pkts.load(),
              (unsigned long long)ns.input_bytes.load(),
              ns.input_pkts.load()
                  ? static_cast<double>(ns.input_bytes.load()) /
                        static_cast<double>(ns.input_pkts.load())
                  : 0.0);
  std::printf("  客户端发出 : %lld 个, 模拟丢失 %lld 个 (%.1f%%)\n",
              static_cast<long long>(sent), static_cast<long long>(lost),
              sent + lost > 0
                  ? static_cast<double>(lost) * 100.0 / (sent + lost)
                  : 0.0);
  std::printf("  状态广播   : %llu 包 / %llu 字节 (均值 %.1f B/包)\n",
              (unsigned long long)ns.state_pkts.load(),
              (unsigned long long)ns.state_bytes.load(),
              ns.state_pkts.load()
                  ? static_cast<double>(ns.state_bytes.load()) /
                        static_cast<double>(ns.state_pkts.load())
                  : 0.0);
  std::printf("  解析丢弃   : %llu\n", (unsigned long long)ns.drops.load());
  std::printf("\n【回滚统计】\n");
  std::printf("  发生回滚帧数 : %lld / %d\n", static_cast<long long>(rolls_n),
              frames);
  std::printf("  累计重算帧数 : %lld\n", static_cast<long long>(total_rolls));
  std::printf("  最坏单帧重算 : %d 帧\n", max_roll);
  std::printf("  预测输入数   : %lld\n",
              static_cast<long long>(room.session().total_predicted()));
  std::printf("\n【最终战果】\n");
  for (int p = 0; p < kMaxPlayers; ++p) {
    const auto& pl = w.players[p];
    std::printf("  P%d hp=%-5d dmg=%-6d kills=%d %s\n", p, pl.hp,
                pl.damage_dealt, pl.kills, pl.alive ? "" : "(死亡)");
  }

  shutdown_net();
  return 0;
}

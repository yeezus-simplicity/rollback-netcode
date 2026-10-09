// multiroom_net_test.cpp — 真实 socket × 多房间压测
//
// ============================================================================
// 【补的是哪一格】
//   项目此前有两个互补的压测，但中间缺一格：
//     · multi_room_test.cpp  —— 进程内多房间「模拟」吞吐（一个字节都不走 socket）
//     · loadtest_main.cpp    —— 单房间「真实 socket」压测（受 kMaxPlayers=4 限制）
//   缺的正是「真实 socket × 多房间」—— 已知限制 #5 的缺口就在这里。
//   本程序在**单进程内**同时托管 N 个房间，每个房间：
//     · 一个真实 UDP socket 收输入   （端口 kUdpBasePort + i）
//     · 一个真实 TCP listener 广播状态（端口 kTcpBasePort + i）
//     · 每个玩家一个真实客户端 socket（UDP 发输入 + TCP 收状态）
//   所有字节都经 loopback 协议栈，全部房间由**同一个模拟线程**按 30Hz 推进。
//
// ============================================================================
// 【为什么是「单进程多房间」而不是「多进程」】
//   真实游戏服就是单进程多房间：房间共享线程池、内存分配器与 fd 表；
//   多进程会把进程切换、端口重复绑定、内存重复统计混进指标里。
//   单进程多房间才是服务端真实的承载模型，也才能看出「模拟线程的每 tick 预算」。
//
// ============================================================================
// 【与 TSan 的关系】
//   本程序保持「**每个房间只被一条模拟线程读写**」这条不变式：
//   所有会改房间状态的操作（收包 / tick / 读写快照）都发生在该房间所属的那条
//   分片线程内，客户端线程只碰自己的 socket。因此不存在跨线程访问同一房间的数据竞争
//   （这正是此前 world 竞态修复树立的规则）。
//
// ============================================================================
// 【模拟线程分片（对应已知限制 #2 的轻量版）】
//   实测单条模拟线程在 64 房间时 tick p99 已占 33ms 预算的 55%，128 房间时
//   p99 82ms 直接打爆预算（背压丢弃 + 掉帧）——瓶颈在**模拟 CPU**，不在网络栈。
//   故支持把房间按 room_index % shards 均摊给多条模拟线程：房间之间无共享状态，
//   分片只改变「哪个房间归哪条线程」，不需要任何锁（详见 net/room_shard.h）。
//   验收标准：**最差分片的 tick p99 落在单 tick 预算内**（见输出「容量判定」）。
//
// ============================================================================
// 用法: ./multiroomnet [rooms] [duration_s] [players_per_room] [shards]
//   默认: 32 房间 / 10 秒 / 每房间 4 玩家 / 1 条模拟线程
// 退出码: 0 = 全部房间正常且最差分片 tick 在预算内；1 = 否则（CI 门禁）
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/rng.h"
#include "core/world.h"
#include "net/latency_histogram.h"
#include "net/net.h"
#include "net/room_shard.h"
#include "net/serialize.h"

using namespace synq;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kUdpBasePort = 19100;  // 房间 i 的输入端口 = 19100 + i
constexpr int kTcpBasePort = 20100;  // 房间 i 的状态端口 = 20100 + i

int g_one = 1;

// ---- 监听 socket（非阻塞：轮询式收包，不依赖 select 的 FD_SETSIZE 上限）----
int make_udp_listener(int port) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_failed(fd)) return -1;
  set_nonblock(fd);
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char*>(&g_one), sizeof(g_one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (socket_failed(::bind(fd, reinterpret_cast<sockaddr*>(&addr),
                           sizeof(addr)))) {
    close_socket(fd);
    return -1;
  }
  return fd;
}

int make_tcp_listener(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_failed(fd)) return -1;
  set_nonblock(fd);
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char*>(&g_one), sizeof(g_one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (socket_failed(::bind(fd, reinterpret_cast<sockaddr*>(&addr),
                           sizeof(addr))) ||
      socket_failed(::listen(fd, SOMAXCONN))) {
    close_socket(fd);
    return -1;
  }
  return fd;
}

inline sockaddr_in make_addr(int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  a.sin_addr.s_addr = inet_addr("127.0.0.1");
  return a;
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
    if (c.attack_target == player)
      c.attack_target = (player + 1) % kMaxPlayers;
  }
  if (r.range(0, 9) == 0) c.cast_spell = 1;
  return c;
}

// 一个「房间 + 它的真实 socket」——服务端侧
struct RoomNet {
  explicit RoomNet(std::uint64_t seed) : room(seed) {}
  BattleRoom room;
  int udp_fd = -1;
  int tcp_listen = -1;
  std::vector<int> conns;       // 已 accept 的 TCP 连接（非阻塞）

  // 服务端统计
  std::uint64_t state_sent = 0;
  std::uint64_t state_bytes = 0;
  std::uint64_t backpressure = 0;

  // 客户端统计（本房间的客户端线程写）
  std::uint64_t cli_inputs_sent = 0;
  std::uint64_t cli_state_recv = 0;
  std::uint64_t cli_state_bytes = 0;
  std::uint64_t cli_send_fail = 0;
  bool cli_connected = false;
};

// 向一个 conn 非阻塞发送；缓冲满即计背压丢弃（慢客户端不阻塞 tick）
void try_send(RoomNet& R, int fd, const std::uint8_t* data, std::size_t len) {
  const int n = ::send(fd, reinterpret_cast<const char*>(data),
                       static_cast<int>(len), 0);
  if (socket_failed(n) || static_cast<std::size_t>(n) != len) {
    ++R.backpressure;
    return;
  }
  ++R.state_sent;
  R.state_bytes += len;
}

// ---- 客户端线程：每个房间一个线程，持有该房间玩家的真实 socket ----
void run_room_clients(RoomNet* R, int room_id, int players, double duration_s,
                      Clock::time_point t_start, std::atomic<bool>* go) {
  if (init_net() != 0) return;

  std::vector<int> udp(static_cast<std::size_t>(players), -1);
  std::vector<int> tcp(static_cast<std::size_t>(players), -1);

  sockaddr_in udp_srv = make_addr(kUdpBasePort + room_id);
  sockaddr_in tcp_srv = make_addr(kTcpBasePort + room_id);

  for (int p = 0; p < players; ++p) {
    udp[static_cast<std::size_t>(p)] = ::socket(AF_INET, SOCK_DGRAM, 0);
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_failed(udp[static_cast<std::size_t>(p)]) || socket_failed(fd)) {
      R->cli_connected = false;
      goto cleanup;
    }
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<char*>(&g_one), sizeof(g_one));
    // 与服务端对称地加大接收缓冲，避免接收侧成为瓶颈
    int rcvbuf = 256 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                 reinterpret_cast<char*>(&rcvbuf), sizeof(rcvbuf));
    if (socket_failed(::connect(fd, reinterpret_cast<sockaddr*>(&tcp_srv),
                               sizeof(tcp_srv)))) {
      close_socket(fd);
      R->cli_connected = false;
      goto cleanup;
    }
    set_nonblock(fd);
    tcp[static_cast<std::size_t>(p)] = fd;
  }
  R->cli_connected = true;

  {
    // 等服务端把房间建好、时钟对齐
    while (!go->load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // 客户端比服务端晚 5ms 起步：保证 frame 永远不超前于服务端 tick
    //（on_input 只接受 frame <= tick + max_prediction_ahead(=2)）
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const double frame_ms = 1000.0 / kTickRate;
    const auto deadline =
        t_start + std::chrono::milliseconds(static_cast<int>(duration_s * 1000));
    auto next = Clock::now();
    int frame = 0;
    std::vector<std::uint8_t> rbuf(4096);

    while (Clock::now() < deadline) {
      for (int p = 0; p < players; ++p) {
        Command c = gen_command(0x1234 + room_id, frame, p);
        auto pkt = serialize_input(frame, p, c);
        const int n = ::sendto(udp[static_cast<std::size_t>(p)],
                               reinterpret_cast<const char*>(pkt.data()),
                               static_cast<int>(pkt.size()), 0,
                               reinterpret_cast<sockaddr*>(&udp_srv),
                               sizeof(udp_srv));
        if (socket_failed(n)) ++R->cli_send_fail;
        else ++R->cli_inputs_sent;
      }
      // 收状态广播（非阻塞 drain）
      for (int p = 0; p < players; ++p) {
        for (;;) {
          const int n = ::recv(tcp[static_cast<std::size_t>(p)],
                               reinterpret_cast<char*>(rbuf.data()),
                               static_cast<int>(rbuf.size()), 0);
          if (n <= 0) break;
          ++R->cli_state_recv;
          R->cli_state_bytes += static_cast<std::uint64_t>(n);
        }
      }
      ++frame;
      next += std::chrono::microseconds(static_cast<std::int64_t>(frame_ms * 1000));
      std::this_thread::sleep_until(next);
    }
  }

cleanup:
  for (int fd : udp)
    if (!socket_failed(fd)) close_socket(fd);
  for (int fd : tcp)
    if (!socket_failed(fd)) close_socket(fd);
  shutdown_net();
}

// ---- 服务一个房间的一个 tick ----
// 收输入 → 接纳连接 → 读 ACK/重连 → 推进模拟 → #4 重同步 → 广播增量。
//
// ★ 分片安全的前提：本函数**只碰传入房间自己的** socket 与状态。
//   房间之间无共享状态，所以多条线程各跑各的房间无需任何锁（见 room_shard.h）。
//   缓冲区是函数内局部量（每房间每 tick 一份，栈上 2KB），绝不跨线程共享。
void service_room(RoomNet& R, int players) {
  std::uint8_t rbuf[2048]{};

  // 1) 收输入（非阻塞 drain）
  for (;;) {
    const int n = ::recvfrom(R.udp_fd, reinterpret_cast<char*>(rbuf),
                             static_cast<int>(sizeof(rbuf)), 0, nullptr, nullptr);
    if (n <= 0) break;
    R.room.on_input_packet(rbuf, static_cast<std::size_t>(n));
  }

  // 2) 接纳新连接
  if (R.conns.size() < static_cast<std::size_t>(players)) {
    for (;;) {
      int c = ::accept(R.tcp_listen, nullptr, nullptr);
      if (socket_failed(c)) break;
      set_nonblock(c);
      // 【测量正确性】关 Nagle：状态广播是高频小包，Nagle 会把多个 tick 的包
      // 合并成一个段，导致接收端一次 recv 拿到多包 —— 「recv 次数」会严重
      // 低估真实包数。关掉后 recv 次数才近似等于包数。
      ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<char*>(&g_one), sizeof(g_one));
      // 【测量卫生】加大发送缓冲：多条分片并行发送时，个别连接会在一个 tick 内
      // 瞬时打满默认缓冲（Windows 默认约 64KB），触发非阻塞 send 的 EWOULDBLOCK
      // → 被 try_send 记成一次「背压丢弃」。128 房间档实测 328282 个状态包里偶发
      // 个位数丢弃 —— 这是测量假象，不是服务端故障。与其放松门禁（"允许少量丢弃"）
      // 不如消除它：给连接更大缓冲，让 tback==0 这条严格判据继续成立。
      int sndbuf = 256 * 1024;
      ::setsockopt(c, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<char*>(&sndbuf), sizeof(sndbuf));
      R.conns.push_back(c);
      // 握手：立即下发全量状态（与 server_main.cpp 语义一致）
      auto full = R.room.build_state_packet(true);
      try_send(R, c, full.data(), full.size());
    }
  }

  // 3) 读连接上的 ACK / reconnect
  for (int c : R.conns) {
    for (;;) {
      const int n = ::recv(c, reinterpret_cast<char*>(rbuf),
                           static_cast<int>(sizeof(rbuf)), 0);
      if (n <= 0) break;
      const auto type = static_cast<MsgType>(rbuf[0]);
      Reader r{rbuf, static_cast<std::size_t>(n), 1};
      if (type == MsgType::kAck) {
        R.room.on_ack(r.get_varint(), r.get_varint());
      } else if (type == MsgType::kReconnect) {
        R.room.on_reconnect(r.get_varint(), r.get_varint());
      }
    }
  }

  // 4) 推进模拟（本房间只被本分片线程读写，确定性不被破坏）
  R.room.tick();

  // 5) #4：超界输入触发重同步 —— 补发全量权威快照
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (R.room.needs_resync(p)) {
      auto full = R.room.build_state_packet(true);
      for (int c : R.conns) try_send(R, c, full.data(), full.size());
      R.room.clear_resync(p);
    }
  }

  // 6) 广播增量状态
  if (!R.conns.empty()) {
    auto pkt = R.room.build_state_packet(false);
    for (int c : R.conns) try_send(R, c, pkt.data(), pkt.size());
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int rooms = argc > 1 ? std::atoi(argv[1]) : 32;
  const int duration_s = argc > 2 ? std::atoi(argv[2]) : 10;
  int players = argc > 3 ? std::atoi(argv[3]) : kMaxPlayers;
  int shards = argc > 4 ? std::atoi(argv[4]) : 1;  // 模拟线程分片数
  if (players > kMaxPlayers) players = kMaxPlayers;  // BattleRoom 硬限制
  if (shards < 1) shards = 1;
  if (shards > rooms) shards = rooms;
  if (rooms <= 0 || players <= 0) {
    std::fprintf(stderr, "参数非法: rooms=%d players=%d shards=%d\n", rooms,
                 players, shards);
    return 2;
  }

  if (init_net() != 0) {
    std::fprintf(stderr, "网络初始化失败\n");
    return 2;
  }

  std::printf("==========================================================\n");
  std::printf(" 真实 socket × 多房间压测\n");
  std::printf("==========================================================\n");
  std::printf(" 房间数    : %d\n", rooms);
  std::printf(" 每房间玩家: %d (BattleRoom 上限 %d)\n", players, kMaxPlayers);
  std::printf(" 模拟分片  : %d 条线程 (每条约 %d 房间, round-robin)\n", shards,
              (rooms + shards - 1) / shards);
  std::printf(" 时长      : %d 秒 @ %d Hz\n", duration_s, kTickRate);
  std::printf(" 端口      : UDP %d..%d  TCP %d..%d\n", kUdpBasePort,
              kUdpBasePort + rooms - 1, kTcpBasePort, kTcpBasePort + rooms - 1);
  std::printf(" 客户端 socket 总数: %d (每房间 %d UDP + %d TCP)\n",
              rooms * players * 2, players, players);
  std::printf(" 机器      : %u 逻辑核\n\n", std::thread::hardware_concurrency());

  // ---- 建房间 + 绑 socket ----
  // 用 unique_ptr 持有：BattleRoom 内含 std::atomic（NetStats），既不可拷贝也不可
  // 移动，vector<RoomNet> 在扩容时会编译失败；且客户端线程持有 RoomNet*，
  // 必须保证**地址稳定**（扩容搬迁会导致悬垂指针）。
  std::vector<std::unique_ptr<RoomNet>> nets;
  nets.reserve(static_cast<std::size_t>(rooms));
  for (int i = 0; i < rooms; ++i)
    nets.push_back(std::make_unique<RoomNet>(static_cast<std::uint64_t>(7000 + i)));

  int bind_fail = 0;
  for (int i = 0; i < rooms; ++i) {
    RoomNet& R = *nets[static_cast<std::size_t>(i)];
    R.udp_fd = make_udp_listener(kUdpBasePort + i);
    R.tcp_listen = make_tcp_listener(kTcpBasePort + i);
    if (R.udp_fd < 0 || R.tcp_listen < 0) ++bind_fail;
  }
  if (bind_fail > 0) {
    std::fprintf(stderr,
                 "!! %d/%d 个房间端口绑定失败（端口被占用？）\n", bind_fail,
                 rooms);
  }

  // ---- 起客户端线程 ----
  std::atomic<bool> go{false};
  const auto t_start = Clock::now();
  std::vector<std::thread> clients;
  clients.reserve(static_cast<std::size_t>(rooms));
  for (int i = 0; i < rooms; ++i) {
    clients.emplace_back([&, i] {
      run_room_clients(nets[static_cast<std::size_t>(i)].get(), i, players,
                       static_cast<double>(duration_s), t_start, &go);
    });
  }

  // ---- 模拟线程分片：每条线程推进自己那批房间 ----
  //
  // 【为什么】实测单条模拟线程在 128 房间时 tick p99 82ms，打爆 33ms 预算
  //（瓶颈在模拟 CPU，不在网络栈）→ 按 room_index % shards 把房间均摊给多条线程。
  // 【为什么安全】房间之间无共享状态；分片只改变「哪个房间归哪条线程」，
  //   不改变「每个房间只被一条线程读写」——无需锁，无数据竞争（TSan CI 守护）。
  const int total_ticks = duration_s * kTickRate;
  const double frame_us = 1000.0 * 1000.0 / kTickRate;  // 单 tick 预算（us）

  auto process_room = [&](int r) {
    service_room(*nets[static_cast<std::size_t>(r)], players);
  };
  using Shard = RoomShard<decltype(process_room)>;

  std::vector<std::unique_ptr<Shard>> shard_objs;
  shard_objs.reserve(static_cast<std::size_t>(shards));
  for (int s = 0; s < shards; ++s)
    shard_objs.push_back(
        std::make_unique<Shard>(s, shards, rooms, process_room));

  go.store(true, std::memory_order_release);
  const auto loop_t0 = Clock::now();
  {
    std::vector<std::thread> shard_threads;
    shard_threads.reserve(static_cast<std::size_t>(shards));
    for (int s = 0; s < shards; ++s) {
      shard_threads.emplace_back([&, s] {
        shard_objs[static_cast<std::size_t>(s)]->run(total_ticks, kTickRate);
      });
    }
    for (auto& th : shard_threads) th.join();
  }
  const auto loop_t1 = Clock::now();   // 分片全部结束即取时刻：不含客户端 join 等待

  for (auto& c : clients) c.join();

  // ---- 汇总各分片：取最差的那条（木桶效应，它决定整体是否掉帧）----
  double achieved_hz = shard_objs.empty() ? 0.0 : 1e30;
  int ticks_done = total_ticks;
  LatencyHistogram::Summary worst{};
  {
    bool first = true;
    for (auto& sp : shard_objs) {
      const auto ss = sp->tick_cost().summary();
      if (sp->achieved_hz() < achieved_hz) achieved_hz = sp->achieved_hz();
      if (sp->ticks_done() < ticks_done) ticks_done = sp->ticks_done();
      if (first || ss.p99 > worst.p99) {
        worst = ss;
        first = false;
      }
    }
  }
  // 分片线程全部结束的墙钟耗时（仅用于参考输出）
  const double loop_s = std::chrono::duration<double>(loop_t1 - loop_t0).count();
  (void)loop_s;

  // ---- 关 socket ----
  for (auto& up : nets) {
    RoomNet& R = *up;
    for (int c : R.conns) close_socket(c);
    if (!socket_failed(R.tcp_listen)) close_socket(R.tcp_listen);
    if (!socket_failed(R.udp_fd)) close_socket(R.udp_fd);
  }

  // ---- 汇总 ----
  std::uint64_t tok_in = 0, tok_state = 0, tok_state_bytes = 0, tback = 0;
  std::uint64_t tcli_inputs = 0, tcli_state = 0, tcli_bytes = 0, tcli_fail = 0;
  int rooms_ok = 0, rooms_nostate = 0, rooms_noconn = 0;
  for (auto& up : nets) {
    RoomNet& R = *up;
    tok_in += R.room.net_stats().input_pkts.load();
    tok_state += R.state_sent;
    tok_state_bytes += R.state_bytes;
    tback += R.backpressure;
    tcli_inputs += R.cli_inputs_sent;
    tcli_state += R.cli_state_recv;
    tcli_bytes += R.cli_state_bytes;
    tcli_fail += R.cli_send_fail;
    if (R.cli_state_recv > 0 && R.room.tick_count() >= total_ticks - 2) ++rooms_ok;
    else if (R.cli_state_recv == 0) ++rooms_nostate;
    if (!R.cli_connected) ++rooms_noconn;
  }

  const auto& s = worst;   // 最差分片的 tick 耗时（木桶效应）
  // 【容量判据】最差分片的 tick p99 必须落在单 tick 预算内，否则就是掉帧。
  // 这正是「分片前 128 房间 FAIL、分片后 PASS」的验收标准。
  const bool tick_in_budget = static_cast<double>(s.p99) <= frame_us;
  const double secs = static_cast<double>(duration_s);
  const std::size_t per_room_mem = sizeof(BattleRoom);

  std::printf("----------------------------------------------------------\n");
  std::printf(" 结果\n");
  std::printf("----------------------------------------------------------\n");
  std::printf(" 正常房间          : %d / %d\n", rooms_ok, rooms);
  std::printf(" 未收到状态包房间  : %d\n", rooms_nostate);
  std::printf(" 客户端连接失败房间: %d\n", rooms_noconn);
  std::printf(" 模拟分片          : %d 条线程 × 约 %d 房间 (最差分片决定整体)\n",
              shards, (rooms + shards - 1) / shards);
  std::printf(" 服务端 tick 完成  : %d / %d (最少的那条分片)\n", ticks_done,
              total_ticks);
  std::printf(" 实际 tick 频率    : %.1f Hz (目标 %d Hz)  %s\n", achieved_hz,
              kTickRate,
              achieved_hz >= kTickRate * 0.98 ? "[未饱和]" : "[已饱和/掉帧]");
  std::printf(" 输入包(服务端收到): %llu  (%.0f 包/秒)\n",
              (unsigned long long)tok_in, tok_in / secs);
  std::printf(" 输入包(客户端发出): %llu  (失败 %llu)\n",
              (unsigned long long)tcli_inputs,
              (unsigned long long)tcli_fail);
  std::printf(" 状态包(服务端发出): %llu  (%.0f 包/秒, 背压丢弃 %llu)\n",
              (unsigned long long)tok_state, tok_state / secs,
              (unsigned long long)tback);
  std::printf(" 状态包(客户端 recv): %llu  (下界；TCP 是字节流，合并段会更少)\n",
              (unsigned long long)tcli_state);
  // 【可靠投递校验】TCP 保证不丢，故「服务端发出的字节」应等于「客户端收到的字节」
  //（差异只应来自收尾时仍在途的少量数据）。这比包计数更能证明链路正确。
  const double sent_kb = tok_state_bytes / 1024.0;
  const double recv_kb = tcli_bytes / 1024.0;
  const double bytes_diff_pct =
      tok_state_bytes > 0
          ? 100.0 * static_cast<double>(tok_state_bytes - tcli_bytes) /
                static_cast<double>(tok_state_bytes)
          : 0.0;
  std::printf(" 状态字节 发出/收到: %.1f KB / %.1f KB  (差 %.2f%%, 应≈0)\n",
              sent_kb, recv_kb, bytes_diff_pct);
  std::printf(" 状态广播带宽      : %.1f KB/s\n",
              tok_state_bytes / secs / 1024.0);
  std::printf(" 每条分片 tick     : 处理约 %d 房间, mean %.0f us / p99 %llu us / max %llu us\n",
              (rooms + shards - 1) / shards, static_cast<double>(s.mean),
              (unsigned long long)s.p99, (unsigned long long)s.max);
  std::printf(" tick 预算占用     : %.2f%% (预算 %.0f us, 取最差分片)\n",
              100.0 * static_cast<double>(s.p99) / frame_us, frame_us);
  std::printf(" 容量判定          : %s (最差分片 p99 %.0f us vs 预算 %.0f us)\n",
              tick_in_budget ? "[在预算内]" : "[超预算/掉帧]",
              static_cast<double>(s.p99), frame_us);
  std::printf(" 房间内存          : %.1f KB/房间, 合计 %.1f MB\n",
              per_room_mem / 1024.0,
              static_cast<double>(per_room_mem) * rooms / 1024.0 / 1024.0);

  std::printf("\n----------------------------------------------------------\n");
  std::printf(" Markdown 片段\n");
  std::printf("----------------------------------------------------------\n");
  std::printf("| 指标 | 数值 |\n|---|---|\n");
  std::printf("| 真实 socket × 房间数 | %d 房间 × %d 玩家 |\n", rooms, players);
  std::printf("| 模拟线程分片 | %d 条 (每条约 %d 房间) |\n", shards,
              (rooms + shards - 1) / shards);
  std::printf("| 客户端 socket 总数 | %d |\n", rooms * players * 2);
  std::printf("| 输入 QPS(服务端实收) | %.0f |\n", tok_in / secs);
  std::printf("| 状态广播包/秒 | %.0f |\n", tok_state / secs);
  std::printf("| 状态广播带宽 | %.1f KB/s |\n", tok_state_bytes / secs / 1024.0);
  std::printf("| 每条分片 tick | mean %.0f us / p99 %llu us |\n",
              static_cast<double>(s.mean), (unsigned long long)s.p99);
  std::printf("| 实际 tick 频率 | %.1f Hz (目标 %d) |\n", achieved_hz, kTickRate);
  std::printf("| 状态字节 发出/收到 | %.1f / %.1f KB (差 %.2f%%) |\n", sent_kb,
              recv_kb, bytes_diff_pct);
  std::printf("| 背压丢弃 | %llu |\n", (unsigned long long)tback);
  std::printf("| 房间常驻内存 | %.1f KB |\n", per_room_mem / 1024.0);

  // 通过条件：端口全绑上、每个房间都收到状态且推进到位、零背压、TCP 字节数对上、
  // **最差分片的 tick p99 落在预算内**、频率未明显掉帧。
  // bytes_diff_pct 允许 2% 容差 —— 收尾时客户端停止 drain，最后几帧仍在 TCP 缓冲里。
  const bool pass = (bind_fail == 0 && rooms_ok == rooms && rooms_noconn == 0 &&
                     tback == 0 && bytes_diff_pct < 2.0 && tick_in_budget &&
                     achieved_hz >= kTickRate * 0.95);
  std::printf("\n==========================================================\n");
  if (pass) {
    std::printf("MULTIROOM_OK rooms=%d players=%d shards=%d qps=%.0f tick_p99_us=%llu\n",
                rooms, players, shards, tok_in / secs, (unsigned long long)s.p99);
  } else {
    std::printf("MULTIROOM_FAIL rooms_ok=%d/%d shards=%d bind_fail=%d noconn=%d backpressure=%llu bytes_diff=%.2f%% tick_p99_us=%llu (budget %.0f)\n",
                rooms_ok, rooms, shards, bind_fail, rooms_noconn,
                (unsigned long long)tback, bytes_diff_pct,
                (unsigned long long)s.p99, frame_us);
  }
  std::printf("==========================================================\n");
  return pass ? 0 : 1;
}

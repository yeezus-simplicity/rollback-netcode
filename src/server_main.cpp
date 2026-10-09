// server_main.cpp — 游戏服务器主程序
//
// 架构（贴近真实游戏服）：
//   UDP  :18088  接收玩家输入包（高频、可丢）
//   TCP  :18089  广播状态 + 握手/重连（可靠、有序）
//   Tick 30Hz   固定步进推进模拟
//
// 并发模型（v1.1 重构 —— 对应已知限制 #3）：
//   网络 I/O 与确定性模拟解耦。模拟保持单线程确定性（room.tick 必须按固定顺序推进），
//   网络层用 worker 线程池 + 无锁队列承接，不破坏"相同输入必得相同状态"。
//     - UDP recv worker 池（kUdpRecvWorkers）: select 包裹的 recvfrom → 入 MPSC 输入队列
//     - TCP accept worker: accept → set_nonblock → 分配 sender worker + 启动每连接专属
//                           recv 线程（解析 ACK/reconnect）→ 入同一个 MPSC 输入队列
//     - sender worker 池（kSenderWorkers）: 每连接一个无锁 SPSC 发送环，drain 后 send
//     - 模拟线程（main 30Hz tick）: drain 输入队列 → room.tick() → 广播包写入各连接发送环
//       发送环满即背压触发，丢弃最旧状态包并计数（慢客户端不阻塞快客户端/tick）
//   所有修改 room 状态的操作（input/ack/reconnect）都经 MPSC 队列，由模拟线程单线程处理，
//   彻底消除旧版 udp_thread 直改 room 的数据竞争隐患。
//   无锁原语（MpscQueue / SpmcRing）经 ThreadSanitizer CI（ci.yml tsan job）验证无数据竞争。
//
// 优雅退出：所有阻塞 recv/accept 都用 select(timeout) 包裹，running=false 后无需依赖
// close() 唤醒，各 worker 必定可 join。发送 socket 设为非阻塞，慢客户端只会触发背压
// （EAGAIN → 计数丢弃），绝不会让 sender worker 卡在 send 上导致退出死锁。
//
// 用法: ./gameserver [player_count] [latency_ms] [jitter_ms] [loss_pct] [frames] [slow_tcp]
//   slow_tcp: 0/1 —— 置 1 时最后一个客户端"慢读"以触发服务端背压（验证用）

#include <algorithm>
#include <array>
#include <atomic>
#ifdef _WIN32
// Windows: select 由 winsock2.h 提供（net.h 已包含）
#else
#include <sys/select.h>
#endif
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
#include "net/concurrent.h"
#include "net/net.h"
#include "net/serialize.h"

using namespace synq;
using Clock = std::chrono::steady_clock;

namespace {
constexpr int kUdpPort = 18088;
constexpr int kTcpPort = 18089;
constexpr int kUdpRecvWorkers = 2;       // UDP 接收 worker 数
constexpr int kSenderWorkers = 2;        // 发送 worker 数
constexpr std::size_t kOutCap = 64;      // 每连接发送环容量（背压阈值）
int one_ = 1;

int make_udp_listener(int port) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  set_nonblock(fd);  // 非阻塞：worker 退出时（running=false）不依赖 close() 打断 recvfrom
  auto* so = reinterpret_cast<char*>(&one_);
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, so, sizeof(one_));
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
  set_nonblock(fd);  // 非阻塞：accept worker 退出时不依赖 close() 打断 accept
  auto* so = reinterpret_cast<char*>(&one_);
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, so, sizeof(one_));
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

// 客户端模拟：产生输入包，按配置的延迟/抖动/丢包发送；同时（默认）建立 TCP 连接，
// 接收状态广播并定期回 ACK —— 这样服务端整条并发链路都被真实跑起来，而非死代码。
struct ClientBot {
  int player_id = 0;
  int udp_fd = -1;
  int tcp_fd = -1;
  bool use_tcp = false;
  bool slow_tcp = false;                 // 慢读客户端（触发服务端背压）
  sockaddr_in udp_server_addr{};
  sockaddr_in tcp_server_addr{};
  int latency_ms = 0;
  int jitter_ms = 0;
  int loss_pct = 0;
  std::uint64_t seed = 0;
  std::atomic<bool>* stop_flag = nullptr;  // 指向全局 running（main 设置）
  std::atomic<std::int64_t> pkts_sent{0};
  std::atomic<std::int64_t> pkts_lost{0};
  std::atomic<std::int32_t> ack_frame{0};  // 已发出的最后一个 ACK 帧号
  std::thread tcp_thread;

  struct Inflight {
    std::int64_t deliver_at_ms;
    std::vector<std::uint8_t> data;
  };
  std::vector<Inflight> inflight;

  void start() {
    udp_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    udp_server_addr.sin_family = AF_INET;
    udp_server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    udp_server_addr.sin_port = htons(static_cast<std::uint16_t>(kUdpPort));
    if (use_tcp) {
      tcp_fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (tcp_fd >= 0) {
        tcp_server_addr.sin_family = AF_INET;
        tcp_server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        tcp_server_addr.sin_port = htons(static_cast<std::uint16_t>(kTcpPort));
        // 连接失败不致命：UDP 输入链路仍可用
        if (::connect(tcp_fd, reinterpret_cast<sockaddr*>(&tcp_server_addr),
                      sizeof(tcp_server_addr)) == 0) {
          tcp_thread = std::thread([this]() { reader_loop(); });
        } else {
          close_socket(tcp_fd);
          tcp_fd = -1;
        }
      }
    }
  }

  void send_frame(int frame) {
    Command c = gen_command(seed, frame, player_id);
    if (loss_pct > 0) {
      Rng r;
      r.reset(seed + static_cast<std::uint64_t>(frame) * 31 + 7);
      if (r.range(0, 99) < loss_pct) {
        pkts_lost.fetch_add(1);
        return;
      }
    }
    auto pkt = serialize_input(frame, player_id, c);
    Rng jr;
    jr.reset(seed + static_cast<std::uint64_t>(frame) * 17 + 3);
    int lat = latency_ms + (jitter_ms > 0 ? jr.range(-jitter_ms, jitter_ms) : 0);
    if (lat < 0) lat = 0;
    Inflight f;
    f.data = std::move(pkt);
    f.deliver_at_ms = now_ms() + lat;
    inflight.push_back(std::move(f));
  }

  void flush() {
    std::int64_t now = now_ms();
    std::size_t i = 0;
    while (i < inflight.size()) {
      if (inflight[i].deliver_at_ms <= now) {
        ::sendto(udp_fd, reinterpret_cast<const char*>(inflight[i].data.data()),
                 static_cast<int>(inflight[i].data.size()), 0,
                 reinterpret_cast<sockaddr*>(&udp_server_addr),
                 sizeof(udp_server_addr));
        pkts_sent.fetch_add(1);
        inflight.erase(inflight.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
  }

  // TCP 读线程：收状态广播（驱动服务端 sender 路径），周期性回 ACK（驱动 recv 线程 → MPSC → sim）
  void reader_loop() {
    std::uint8_t rb[4096];
    fd_set rfds;
    struct timeval tv;
    int ack_counter = 0;
    // 慢读验证模式：客户端从不消费状态包，使服务端内核发送缓冲与发送环堆积 → 触发背压。
    // 用以验证"慢客户端不会卡住 sender（非阻塞 send + 计数丢弃）"。
    if (slow_tcp) {
      while (stop_flag && stop_flag->load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      return;
    }
    while (stop_flag && stop_flag->load(std::memory_order_relaxed)) {
      FD_ZERO(&rfds);
      FD_SET(tcp_fd, &rfds);
      tv.tv_sec = 0;
      tv.tv_usec = 10000;  // 10ms：running=false 后无需依赖 close() 唤醒
      if (::select(tcp_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
      ssize_t n = ::recv(tcp_fd, reinterpret_cast<char*>(rb), sizeof(rb), 0);
      if (n <= 0) break;  // 服务端关闭连接 → 退出
      if ((++ack_counter % 20) == 0) {
        std::vector<std::uint8_t> ack;
        ack.push_back(static_cast<std::uint8_t>(MsgType::kAck));
        put_varint(ack, player_id);
        put_varint(ack, ack_frame.fetch_add(1) + 1);
        ::send(tcp_fd, reinterpret_cast<const char*>(ack.data()),
               static_cast<int>(ack.size()), 0);
      }
    }
  }

  void stop() {
    if (tcp_thread.joinable()) tcp_thread.join();
    if (udp_fd >= 0) { close_socket(udp_fd); udp_fd = -1; }
    if (tcp_fd >= 0) { close_socket(tcp_fd); tcp_fd = -1; }
  }

  static std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
  }
};

// ---- 网络事件（跨线程传给模拟线程，单消费者处理）----
struct NetEvent {
  enum Kind { INPUT, ACK, RECONNECT } kind;
  int player = 0;
  std::int32_t frame = 0;
  std::vector<std::uint8_t> bytes;  // INPUT 用
};
using InputQueue = synq::net::MpscQueue<NetEvent>;

// 每连接发送环（单生产者单消费者）
struct ConnOut {
  int fd = -1;  // 仅由 accept worker 写入一次；运行期只读；退出时由主线程统一 close
  std::atomic<bool> alive{true};
  synq::net::SpmcRing<std::vector<std::uint8_t>, kOutCap> ring;
};

// sender worker 池
struct SenderPool {
  std::vector<std::thread> workers;
  std::array<std::mutex, kSenderWorkers> mtx;                  // 每 worker 一把（定长）
  std::array<std::vector<ConnOut*>, kSenderWorkers> owned;     // 每 worker 负责的连接
  std::atomic<std::uint64_t>* backpressure = nullptr;          // 背压计数（指向 room.net_stats）
  std::atomic<std::size_t> rr{0};                              // round-robin 分配计数

  void assign(ConnOut* c) {
    std::size_t id = rr.fetch_add(1) % kSenderWorkers;
    std::lock_guard<std::mutex> lk(mtx[id]);
    owned[id].push_back(c);
  }

  void run(std::size_t wid, std::atomic<bool>& keep_running) {
    while (keep_running.load(std::memory_order_relaxed)) {
      std::vector<ConnOut*> my;
      {
        std::lock_guard<std::mutex> lk(mtx[wid]);
        my = owned[wid];
      }
      bool did = false;
      for (ConnOut* c : my) {
        if (!c->alive.load(std::memory_order_relaxed)) continue;
        std::vector<std::uint8_t> pkt;
        while (c->ring.try_pop(pkt)) {
          ssize_t n = ::send(c->fd, reinterpret_cast<const char*>(pkt.data()),
                             static_cast<int>(pkt.size()), 0);
          if (socket_failed(n)) {
            int err = last_error_code();
            if (err == ERR_WOULDBLOCK) {
              // 内核发送缓冲满（慢客户端）：本包丢弃计为背压，停止本轮，等下次再试
              if (backpressure)
                backpressure->fetch_add(1, std::memory_order_relaxed);
              break;
            }
            // 真实错误（连接已断）：标记死亡并从本 worker 的连接列表移除
            c->alive.store(false, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(mtx[wid]);
            std::vector<ConnOut*> f;
            for (ConnOut* x : owned[wid])
              if (x != c) f.push_back(x);
            owned[wid] = std::move(f);
            break;
          }
          did = true;
        }
      }
      if (!did) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
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
  int slow_tcp = argc > 6 ? std::atoi(argv[6]) : 0;

  if (nplayers < 1) nplayers = 1;
  if (nplayers > kMaxPlayers) nplayers = kMaxPlayers;

  printf("==========================================================\n");
  printf(" synq 帧同步游戏服务器（并发 v1.1: worker 池 + 无锁队列 + 背压）\n");
  printf("==========================================================\n");
  printf(" 配置: 玩家=%d  延迟=%dms(±%d)  丢包=%d%%  时长=%d 帧(%.1fs @30fps)\n",
         nplayers, latency, jitter, loss, frames,
         frames / static_cast<double>(kTickRate));
  printf(" 端点: UDP :%d (输入)   TCP :%d (状态)%s\n", kUdpPort, kTcpPort,
         slow_tcp ? "  [慢读客户端=触发背压]" : "");
  printf(" 线程: %d UDP recv + 1 accept + 每连接 1 recv + %d sender\n\n",
         kUdpRecvWorkers, kSenderWorkers);

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

  std::atomic<bool> running{true};
  InputQueue input_q;
  synq::net::MpscQueue<ConnOut*> conn_q;        // accept → 主线程
  std::vector<ConnOut*> conns;                  // 活跃连接（主线程独占，仅主线程增删）
  std::vector<ConnOut*> all_conns;             // 生命周期归主线程，退出时释放
  SenderPool senders;
  senders.backpressure = &room.net_stats().backpressure_drops;

  // ---- UDP 接收 worker 池 ----
  // select 10ms 超时包裹 recvfrom，退出时（running=false）无需依赖 close() 唤醒阻塞的
  // recvfrom，join 必定成功。
  auto udp_recv = [&](int /*wid*/) {
    std::uint8_t buf[2048];
    fd_set rfds;
    struct timeval tv;
    while (running.load(std::memory_order_relaxed)) {
      FD_ZERO(&rfds);
      FD_SET(udp_fd, &rfds);
      tv.tv_sec = 0;
      tv.tv_usec = 10000;  // 10ms
      if (::select(udp_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
      sockaddr_in from{};
      socklen_t fl = sizeof(from);
      ssize_t n = ::recvfrom(udp_fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&from), &fl);
      if (n <= 0) continue;
      NetEvent ev;
      ev.kind = NetEvent::INPUT;
      ev.bytes.assign(buf, buf + n);
      input_q.enqueue(std::move(ev));
    }
  };
  std::vector<std::thread> udp_workers;
  for (int i = 0; i < kUdpRecvWorkers; ++i)
    udp_workers.emplace_back(udp_recv, i);

  // ---- TCP accept worker + 每连接专属 recv 线程 ----
  std::vector<std::thread> recv_threads;
  std::thread accept_thread([&] {
    fd_set rfds;
    struct timeval tv;
    while (running.load(std::memory_order_relaxed)) {
      FD_ZERO(&rfds);
      FD_SET(tcp_fd, &rfds);
      tv.tv_sec = 0;
      tv.tv_usec = 500000;  // 500ms 超时，退出时无需依赖 close() 唤醒 accept
      if (::select(tcp_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
      if (!FD_ISSET(tcp_fd, &rfds)) continue;
      sockaddr_in cli{};
      socklen_t cl = sizeof(cli);
      int cfd = ::accept(tcp_fd, reinterpret_cast<sockaddr*>(&cli), &cl);
      if (cfd < 0) continue;
      auto* so = reinterpret_cast<char*>(&one_);
      ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, so, sizeof(one_));
      set_nonblock(cfd);  // 非阻塞：慢客户端只触发背压，绝不卡住 sender

      ConnOut* c = new ConnOut();
      c->fd = cfd;
      // 握手：立即下发全量状态（写入发送环，由 sender 发送）
      auto full = room.build_state_packet(true);
      c->ring.try_push(std::move(full));
      senders.assign(c);
      conn_q.enqueue(c);  // 通知主线程接管广播

      std::printf("[TCP] 客户端接入 (fd=%d)\n", cfd);
      std::fflush(stdout);

      // 专属 recv 线程：select 10ms 超时包裹 recv，退出时（running=false）必定返回
      recv_threads.emplace_back([&, cfd, c]() {
        std::uint8_t rbuf[256];
        fd_set rrfds;
        struct timeval rtv;
        while (running.load(std::memory_order_relaxed)) {
          FD_ZERO(&rrfds);
          FD_SET(cfd, &rrfds);
          rtv.tv_sec = 0;
          rtv.tv_usec = 10000;
          if (::select(cfd + 1, &rrfds, nullptr, nullptr, &rtv) <= 0) continue;
          ssize_t rn = ::recv(cfd, reinterpret_cast<char*>(rbuf), sizeof(rbuf), 0);
          if (rn <= 0) {
            c->alive.store(false, std::memory_order_relaxed);
            break;
          }
          Reader r{rbuf, static_cast<std::size_t>(rn), 0};
          auto type = static_cast<MsgType>(r.get_u8());
          if (type == MsgType::kAck) {
            int player = r.get_varint();
            std::int32_t frame = r.get_varint();
            NetEvent ev;
            ev.kind = NetEvent::ACK;
            ev.player = player;
            ev.frame = frame;
            input_q.enqueue(std::move(ev));
          } else if (type == MsgType::kReconnect) {
            int player = r.get_varint();
            std::int32_t last_acked = r.get_varint();
            NetEvent ev;
            ev.kind = NetEvent::RECONNECT;
            ev.player = player;
            ev.frame = last_acked;
            input_q.enqueue(std::move(ev));
          }
        }
      });
    }
  });

  // ---- sender worker 池 ----
  for (int i = 0; i < kSenderWorkers; ++i)
    senders.workers.emplace_back(
        [&, i]() { senders.run(static_cast<std::size_t>(i), running); });

  // ---- 客户端机器人（含 TCP 客户端）----
  std::vector<ClientBot> bots(static_cast<std::size_t>(nplayers));
  for (int p = 0; p < nplayers; ++p) {
    bots[static_cast<std::size_t>(p)].player_id = p;
    bots[static_cast<std::size_t>(p)].latency_ms = latency;
    bots[static_cast<std::size_t>(p)].jitter_ms = jitter;
    bots[static_cast<std::size_t>(p)].loss_pct = loss;
    bots[static_cast<std::size_t>(p)].latency_ms += p * 20;
    bots[static_cast<std::size_t>(p)].seed =
        0xABCDEF00ULL + static_cast<std::uint64_t>(p) * 7919;
    bots[static_cast<std::size_t>(p)].use_tcp = true;
    bots[static_cast<std::size_t>(p)].slow_tcp =
        (slow_tcp != 0 && p == nplayers - 1);
    bots[static_cast<std::size_t>(p)].stop_flag = &running;
    bots[static_cast<std::size_t>(p)].start();
  }

  // ---- 30Hz tick 循环（模拟线程，确定性核心）----
  printf("--- 开战 ---\n");
  auto tick_interval = std::chrono::microseconds(1000000 / kTickRate);
  auto next_tick = Clock::now();
  std::vector<std::int64_t> roll_hist(frames, 0);
  std::int64_t total_rolls = 0;

  for (int f = 0; f < frames; ++f) {
    next_tick += tick_interval;

    // 0. 接管新接入的连接
    ConnOut* nc;
    while (conn_q.dequeue(nc)) {
      all_conns.push_back(nc);
      conns.push_back(nc);
    }

    // 1. 客户端产生并发送本帧输入
    for (auto& b : bots) b.send_frame(f);
    for (auto& b : bots) b.flush();

    // 2. 消费网络事件（单线程，无竞争地修改 room）
    NetEvent ev;
    while (input_q.dequeue(ev)) {
      switch (ev.kind) {
        case NetEvent::INPUT:
          room.on_input_packet(ev.bytes.data(), ev.bytes.size());
          break;
        case NetEvent::ACK:
          room.on_ack(ev.player, ev.frame);
          break;
        case NetEvent::RECONNECT: {
          World w = room.world();
          room.on_reconnect(ev.player, ev.frame, w);
          break;
        }
      }
    }

    // 3. 推进模拟
    std::int32_t rolls = room.tick();
    roll_hist[static_cast<std::size_t>(f)] = rolls;
    total_rolls += rolls;

    // 4. 广播状态（增量或全量），写入每连接发送环；满则背压丢弃
    auto pkt = room.build_state_packet(f % kTickRate == 0);
    for (auto it = conns.begin(); it != conns.end();) {
      ConnOut* c = *it;
      if (!c->alive.load(std::memory_order_relaxed)) {
        // 连接已断：仅从广播列表移除，fd 由主线程在退出阶段统一关闭（避免与 sender 竞态）
        it = conns.erase(it);
        continue;
      }
      if (!c->ring.try_push(pkt)) {
        room.net_stats().backpressure_drops.fetch_add(1, std::memory_order_relaxed);
      }
      ++it;
    }

    // 5. 模拟真实 tick 时钟
    std::this_thread::sleep_until(next_tick);
  }

  // ---- 优雅退出 ----
  running = false;

  // 1) 先 join 全部网络线程。各 worker/线程在 running=false 后最多等待一个
  //    select 超时（UDP recv 10ms / accept 500ms / 连接 recv 10ms / sender 200us）
  //    即自行退出；join 完成意味着没有任何线程再对下列 fd 做 I/O，从而消除
  //    「主线程 close ↔ 工作线程 recvfrom/accept/recv」的竞态
  //    （ThreadSanitizer 曾在此报 data race：main close(udp_fd) vs worker recvfrom）。
  for (auto& t : udp_workers) t.join();
  accept_thread.join();
  for (auto& t : senders.workers) t.join();
  for (auto& t : recv_threads) t.join();

  // 2) 所有网络线程已退出，此刻关闭任何 fd 都安全，无并发 I/O。
  close_socket(udp_fd);
  close_socket(tcp_fd);
  for (auto* c : all_conns) close_socket(c->fd);

  // 3) 通知客户端（bot）读线程退出并回收连接对象。
  //    bot 读线程在服务器侧关闭 c->fd 后 recv 返回 0 自然退出；
  //    stop() 内会 join 其 tcp_thread，确保 bot 对象析构前线程已结束。
  for (auto& b : bots) b.stop();
  for (auto* c : all_conns) delete c;

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
  std::printf(" 对局结束  (tick=%d)  接入连接=%zu\n", room.tick_count(),
              all_conns.size());
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
  std::printf("  背压丢弃   : %llu\n",
              (unsigned long long)ns.backpressure_drops.load());
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

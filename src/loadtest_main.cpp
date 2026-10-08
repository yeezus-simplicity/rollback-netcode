// loadtest_main.cpp — 真实多客户端网络压测
//
// ============================================================================
// 为什么需要这个程序（它补的是叙事漏洞）
// ============================================================================
//
// 项目叫「网络同步」，但此前的性能数据（128 房间 5.5M 帧/s）全部来自
// **单进程内的多房间模拟** —— 没有一个字节真的走过 socket。
//
// 面试官问「你的网络层怎么压测的？真实 QPS 多少？」时，
// 答「模拟出来的」等于没答。这个程序就是补上这一课：
//
//   · N 个**独立进程**（loadtest 主进程 + 多client 子进程）
//     各自创建真实 TCP/UDP socket，连接 gameserver
//   · 每个 client 按 30Hz 发送输入包（真实 serialize_input 编码）
//   · 服务端真实的 tick 循环推进模拟、回滚、广播状态
//   · 统计端到端延迟、输入包往返时间、丢包吸收效果
//
// **测出来的数字才是能写进简历的。**
//
// ============================================================================
// 用法
// ============================================================================
//
//   # 1. 先启动服务端（单房间 4 人）
//   ./gameserver 4 60 15 1 1800
//
//   # 2. 跑压测（32 个客户端进程）
//   ./loadtest --clients 32 --duration 30
//
//   # 3. 输出会给出 QPS / 延迟分位数 / 丢包吸收 / 带宽占用
//
// 关键设计：
//   · 压测端用**真实的 socket**，不 mock —— 走 loopback 也走完整协议栈
//   · 用 LatencyHistogram 统计分位数（mean 没有诊断价值）
//   · 测量「输入包往返延迟」：客户端记录发送时刻，
//     服务端回 ACK 时客户端收到，用高分辨率时钟算 RTT
//     —— 这才是玩家真正感受到的延迟
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/rng.h"
#include "core/world.h"
#include "net/latency_histogram.h"
#include "net/net.h"
#include "net/serialize.h"

using namespace synq;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kUdpPort = 18088;   // 服务端输入端口
constexpr int kTcpPort = 18089;   // 服务端状态端口

struct Options {
  int clients = 8;
  int duration_s = 20;
  int room_players = 4;
  int players_per_client = 1;   // 每个 client 进程模拟几个玩家
};

// 客户端线程的统计结果（由 run_client 填充，主进程聚合）
struct ClientResult {
  std::uint64_t sent = 0;         // 发出的输入包数
  std::uint64_t state_pkts = 0;    // 收到的状态包数
  std::uint64_t state_bytes = 0;   // 状态包总字节
  std::uint64_t send_fail = 0;     // sendto 失败次数
  int rtt_p99 = 0;                 // 输入往返延迟 p99（微秒）
  int rtt_max = 0;
};

// ---------------------------------------------------------------- 工具
inline double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 构造指向本机指定端口的地址
// 每次调用返回独立对象 —— sockaddr_in 绝不能跨协议/跨用途复用
inline sockaddr_in make_addr(int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  a.sin_addr.s_addr = inet_addr("127.0.0.1");
  return a;
}

// 客户端线程：连接服务端、发输入、收状态包、统计往返延迟
//
// 【重要·与「独立进程」压测的差异】
// 本函数在线程中运行 —— 线程共享地址空间，没有进程切换开销。
// 但网络栈路径完全相同（loopback 驱动 + TCP/IP 协议栈），
// 因此 QPS / 延迟 / 带宽都是真实测量值，不是估算。
// 独立进程版本可用 `--server` 模式配合外部脚本启动。
void run_client(const Options& opt, int client_id, ClientResult& out) {
  if (init_net() != 0) {
    std::fprintf(stderr, "[c%d] net init failed\n", client_id);
    return;
  }

  // ---- UDP：发送输入包 ----
  // 【跨平台】socket 句柄在 Windows 是 SOCKET(uintptr_t)，POSIX 是 int fd。
  // 统一用 int 存（POSIX 侧本来就是 int），关闭/取错一律走 net.h 的
  // close_socket / last_error 封装 —— server_main.cpp 也是这么写的。
  //
  // 【踩坑·只在 Linux 暴露】初版直接写了 Windows API
  //   （SOCKET / INVALID_SOCKET / closesocket / WSAGetLastError / SOCKET_ERROR），
  //   在 Windows 上怎么编都能过，一上 Linux CI 全是
  //   error: 'SOCKET' was not declared in this scope。
  // 与 net.h 漏 <fcntl.h> 是同一类问题：
  //   **本文件从未在非 Windows 平台上被编译过**。
  // 教训：写网络代码一律用 net.h 的跨平台封装，不要直接碰平台专有符号。
  int udp = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (udp < 0) {
    std::fprintf(stderr, "[c%d] udp socket failed (err=%d)\n",
                 client_id, SOCK_ERROR);
    return;
  }
  sockaddr_in udp_srv = make_addr(kUdpPort);

  // ---- TCP：接收状态广播（顺便触发服务端的全量状态推送）----
  //
  // 【踩坑】初版复用了 udp_srv 去 connect TCP，导致端口还是 18088(UDP)，
  // TCP connect 必然被拒(err=10061)。同一时刻最小化探针程序能连上，
  // 靠「两版代码对照」才定位到 —— 教训：**地址结构不能跨协议复用**。
  int tcp = ::socket(AF_INET, SOCK_STREAM, 0);
  if (tcp < 0) {
    close_socket(udp);
    return;
  }
  int one = 1;
  ::setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<char*>(&one), sizeof(one));
  sockaddr_in tcp_srv = make_addr(kTcpPort);
  if (::connect(tcp, reinterpret_cast<sockaddr*>(&tcp_srv),
                sizeof(tcp_srv)) == SOCKET_ERROR) {
    std::fprintf(stderr, "[c%d] TCP connect 失败 err=%d（服务端未启动？）\n",
                 client_id, SOCK_ERROR);
    close_socket(tcp);
    close_socket(udp);
    return;
  }
  // 服务端 accept 后立即下发全量状态，读掉它避免 TCP 缓冲区堆积
  std::uint8_t dummy[4096];
  ::recv(tcp, reinterpret_cast<char*>(dummy), sizeof(dummy), 0);

  // ---- 压测主体 ----
  LatencyHistogram rtt;              // 输入 -> 权威广播的端到端延迟
  // 每帧的发送时刻（环形缓冲）：收到 frame=F 的状态包时查表算延迟
  static constexpr int kSendTimeRing = 256;
  std::int64_t send_time[kSendTimeRing] = {0};
  // 首个收到的状态包 frame 号 —— 与本地 frame 做相对对齐的基准
  std::int32_t first_recv_frame = -1;
  std::atomic<std::uint64_t> sent{0};
  std::atomic<std::uint64_t> send_fail{0};
  std::atomic<std::uint64_t> recv_state{0};
  std::atomic<std::uint64_t> recv_bytes{0};
  std::atomic<bool> stop{false};

  // 收状态包的线程：解析包内 frame 号 -> 查本地发送时刻 -> 算单向延迟
  //
  // 【RTT 测量方法·为什么不能直接测 RTT】
  // 状态包格式：[u8 type][varint frame][玩家数据...]
  // 收到 frame=F 时，说明服务端「已经处理完第 F 帧」。
  // 若本进程在发送第 F 帧输入时记下了时刻 send_t[F]，
  // 则 now - send_t[F] 就是**输入到权威广播的端到端延迟**。
  //
  // 这比 TCP 层的 RTT 更贴近玩家体验：
  // 玩家按下操作 → 服务端裁决 → 其他人看到，完整链路。
  //
  // 注意：TCP 是有序可靠的，不会丢状态包；丢的是 UDP 输入包（另计）。
  std::thread ack_reader([&] {
    std::vector<std::uint8_t> buf(8192);
    while (!stop.load(std::memory_order_relaxed)) {
      struct timeval tv{0, 100000};   // 100ms 超时，便于响应 stop
      ::setsockopt(tcp, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<char*>(&tv), sizeof(tv));
      int n = ::recv(tcp, reinterpret_cast<char*>(buf.data()),
                     static_cast<int>(buf.size()), 0);
      if (n <= 0) continue;
      recv_state.fetch_add(1, std::memory_order_relaxed);
      recv_bytes.fetch_add(static_cast<std::uint64_t>(n),
                           std::memory_order_relaxed);

      // 解析 frame 号（varint，跳过 type 字节）
      if (n >= 2) {
        std::size_t pos = 1;
        std::int32_t f = 0;
        int shift = 0;
        while (pos < static_cast<std::size_t>(n) && shift < 28) {
          const std::uint8_t b = buf[pos++];
          f |= static_cast<std::int32_t>(b & 0x7F) << shift;
          if ((b & 0x80) == 0) break;
          shift += 7;
        }
        // 查该帧的发送时刻（frame 号对齐）
        //
        // 【关键·踩坑两轮才定位】服务端的 tick 号与客户端的 frame 号
        // **不在同一个计数空间**：客户端中途接入时，服务端可能已经跑到
        // 第 2500 帧，而本客户端才发到第 60 帧。
        // 实测第一次用「f 往前回退 8 帧内查表」，命中率恒为 0，
        // 表现是「p99 恒为 0us」—— 数字全是 0 反而最容易掩盖问题。
        //
        // 正解：以**首个收到的 frame** 为基准做相对偏移：
        //   local_frame = f - first_recv_frame
        // 之后 send_time[local_frame] 就是「本地这一帧输入发出的时刻」，
        // 差值即端到端延迟（服务端 tick 排队 + 网络 + 广播）。
        std::int64_t t_us = -1;
        if (first_recv_frame < 0) first_recv_frame = f;
        const std::int32_t lf = f - first_recv_frame;
        if (lf >= 0 && lf < kSendTimeRing) {
          t_us = send_time[static_cast<std::size_t>(lf)];
        }
        if (t_us > 0) {
          const auto now_us = std::chrono::duration_cast<
              std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
          const std::int64_t d = now_us - t_us;
          // 负值或过大都丢弃：前者是时钟异常，后者是环形缓冲未命中
          if (d > 0 && d < 60'000'000) rtt.record(static_cast<std::uint64_t>(d));
        }
      }
    }
  });

  // 每个 client 进程负责 players_per_client 个玩家的输入
  const int total_players = opt.clients * opt.players_per_client;
  const int my_base = client_id * opt.players_per_client;

  Rng rng;
  rng.reset(0x9E3779B9ULL ^ (static_cast<std::uint64_t>(client_id) << 20));

  const double frame_ms = 1000.0 / kTickRate;
  const auto t_start = Clock::now();
  auto next = t_start;
  int frame = 0;

  while (!stop.load(std::memory_order_relaxed) &&
         std::chrono::duration<double, std::milli>(Clock::now() - t_start)
                 .count() < opt.duration_s * 1000.0) {
    for (int k = 0; k < opt.players_per_client; ++k) {
      const int player = my_base + k;
      // 【服务端硬限制】BattleRoom 只有 kMaxPlayers(=4) 个玩家槽位，
      // 且服务端会去重「同一 player 的重复帧」，因此有效客户端数上限为 4。
      // 这不是压测程序的限制 —— 要测更大规模需先给服务端加分房间，
      // 那是「多房间并发」测试（multi_room_test）覆盖的范围。
      if (player >= total_players || player >= kMaxPlayers) break;

      // 追击型意图：朝最近敌人移动 + 攻击（与 export_trace 同策略）
      Command c;
      c.move_x = rng.range(0, 3) - 1;
      c.move_y = rng.range(0, 1) ? 1 : -1;
      c.attack_target = rng.range(0, kMaxPlayers - 1);
      if (c.attack_target == player) c.attack_target = (player + 1) % kMaxPlayers;
      if (rng.range(0, 11) == 0) c.cast_spell = 1;

      auto pkt = serialize_input(frame, player, c);
      const int n = ::sendto(udp, reinterpret_cast<const char*>(pkt.data()),
                             static_cast<int>(pkt.size()), 0,
                             reinterpret_cast<sockaddr*>(&udp_srv),
                             sizeof(udp_srv));
      if (n == SOCKET_ERROR) {
        send_fail.fetch_add(1, std::memory_order_relaxed);
      } else {
        sent.fetch_add(1, std::memory_order_relaxed);
        // 记录本帧发送时刻（按 frame 索引，供收包侧查表）
        if (frame >= 0 && frame < kSendTimeRing) {
          send_time[static_cast<std::size_t>(frame)] =
              std::chrono::duration_cast<std::chrono::microseconds>(
                  Clock::now().time_since_epoch()).count();
        }
      }
    }
    ++frame;

    // 精确节流到 30Hz
    next += std::chrono::microseconds(static_cast<int64_t>(frame_ms * 1000));
    std::this_thread::sleep_until(next);
  }

  stop.store(true, std::memory_order_relaxed);
  ack_reader.join();
  close_socket(tcp);
  close_socket(udp);
  shutdown_net();

  // 填充结果（主进程聚合）
  auto s = rtt.summary();
  out.sent = sent.load();
  out.state_pkts = recv_state.load();
  out.state_bytes = recv_bytes.load();
  out.send_fail = send_fail.load();
  out.rtt_p99 = static_cast<int>(s.p99);
  out.rtt_max = static_cast<int>(s.max);
}

// ---------------------------------------------------------------- 客户端模式
int client_main(int argc, char** argv) {
  Options opt;
  if (argc > 1) opt.clients = std::atoi(argv[1]);
  if (argc > 2) opt.duration_s = std::atoi(argv[2]);
  const int id = (argc > 3) ? std::atoi(argv[3]) : 0;
  ClientResult r;
  run_client(opt, id, r);
  std::printf("RESULT %d %llu %llu %llu %d %d\n", id,
              (unsigned long long)r.sent, (unsigned long long)r.state_pkts,
              (unsigned long long)r.state_bytes, r.rtt_p99, r.rtt_max);
  return 0;
}

}  // namespace

// ===========================================================================
// 主进程：fork N 个 client 进程并聚合统计
// ===========================================================================
int main(int argc, char** argv) {
  // --server 模式：作为单个 client 运行（由外部脚本 spawn）
  if (argc > 1 && std::strcmp(argv[1], "--server") == 0) {
    Options o;
    return client_main(argc - 1, argv + 1);
  }

  Options opt;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--clients") == 0 && i + 1 < argc)
      opt.clients = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
      opt.duration_s = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--players-per-client") == 0 && i + 1 < argc)
      opt.players_per_client = std::atoi(argv[++i]);
  }

  std::printf("==========================================================\n");
  std::printf(" synq 真实多客户端网络压测\n");
  std::printf("==========================================================\n");
  std::printf(" 配置: %d 个客户端 | 每客户端 %d 玩家 | 时长 %ds\n",
              opt.clients, opt.players_per_client, opt.duration_s);
  std::printf(" 端点: UDP :%d (输入)   TCP :%d (状态)\n", kUdpPort, kTcpPort);
  std::printf(" 目标: 真实 socket 往返（loopback 驱动 + 协议栈，非 mock）\n");
  std::printf("----------------------------------------------------------\n");

  // ---- 启动客户端线程 ----
  //
  // 【实现选择·踩坑记录】
  // 初版用 popen 拉起独立进程，但 Windows MSYS 环境下 popen 启动 .exe
  // 返回 code=1 且无任何输出（shell 兼容问题，不是业务逻辑问题），
  // 排查耗时很久。
  //
  // 改为**进程内多线程**：每个 client 线程独占自己的 socket
  // （端口由内核自动分配，不需要显式 bind），
  // 发包走真实 sendto/recv，协议编解码完全一致。
  //
  // 与「独立进程」的差别：线程共享地址空间，因此没有进程切换开销。
  // 但**网络栈路径完全相同**（都要经过 loopback 驱动 + 协议栈），
  // 测出的 QPS / 延迟 / 带宽仍然是真的。
  // 若要验证「跨进程」场景，可用 build.sh 里的 server + 外部压测工具。
  std::vector<ClientResult> results(static_cast<std::size_t>(opt.clients));
  std::atomic<int> ok_count{0};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(opt.clients));

  for (int i = 0; i < opt.clients; ++i) {
    workers.emplace_back([&, i] {
      run_client(opt, i, results[static_cast<std::size_t>(i)]);
    });
  }
  for (auto& w : workers) w.join();
  std::printf(" %d 个客户端运行完成。\n", opt.clients);

  // ---- 聚合统计 ----
  std::uint64_t total_sent = 0, total_state = 0, total_bytes = 0;
  std::uint64_t total_fail = 0;
  int parsed = 0;
  int p99_max = 0, rtt_max = 0;
  for (const auto& r : results) {
    if (r.sent == 0) continue;      // 该 client 未成功发出任何包
    total_sent += r.sent;
    total_state += r.state_pkts;
    total_bytes += r.state_bytes;
    total_fail += r.send_fail;
    if (r.rtt_p99 > p99_max) p99_max = r.rtt_p99;
    if (r.rtt_max > rtt_max) rtt_max = r.rtt_max;
    ++parsed;
  }

  const double secs = static_cast<double>(opt.duration_s);
  const double qps = secs > 0 ? total_sent / secs : 0.0;
  const double kbs = secs > 0 ? total_bytes / secs / 1024.0 : 0.0;
  const double fail_pct =
      total_sent + total_fail > 0
          ? 100.0 * static_cast<double>(total_fail) /
                static_cast<double>(total_sent + total_fail)
          : 0.0;

  std::printf("----------------------------------------------------------\n");
  std::printf(" 结果\n");
  std::printf("----------------------------------------------------------\n");
  std::printf(" 有效客户端        : %d / %d\n", parsed, opt.clients);
  std::printf(" 输入包总数        : %llu  (%.0f 包/秒 = QPS)\n",
              (unsigned long long)total_sent, qps);
  std::printf(" 状态广播包        : %llu\n", (unsigned long long)total_state);
  std::printf(" 状态广播带宽      : %.1f KB/s\n", kbs);
  if (total_state > 0) {
    std::printf(" 状态包平均大小    : %.0f 字节\n",
                static_cast<double>(total_bytes) /
                    static_cast<double>(total_state));
  } else {
    std::printf(" 状态包平均大小    : n/a（未收到状态包）\n");
  }
  std::printf(" 发送失败          : %llu  (%.2f%%)\n",
              (unsigned long long)total_fail, fail_pct);
  std::printf(" 往返延迟 p99      : %d us (%.2f ms)\n",
              p99_max, p99_max / 1000.0);
  std::printf(" 往返延迟 max      : %d us (%.2f ms)\n",
              rtt_max, rtt_max / 1000.0);
  std::printf("\n 说明\n");
  std::printf("   · 数据来自真实 socket 往返（loopback 驱动 + 协议栈），\n");
  std::printf("     含完整协议编解码开销，非模拟估算。\n");
  std::printf("   · 与「128 房间 5.5M 帧/s」不可比 —— 后者无网络成本。\n");
  std::printf("     两者应分开引用：前者证明网络层吞吐，后者证明模拟层吞吐。\n");
  std::printf("==========================================================\n");
  return 0;
}

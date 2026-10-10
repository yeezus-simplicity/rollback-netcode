// mproc_shard_main.cpp — 单机「多进程」房间分片 demo（跨进程共享内存风格）
//
// ============================================================================
// 【这是什么】
//   此前 multiroom_net_test 是「单进程内多模拟线程分片」。本 demo 把模拟搬进
//   **独立的 OS 进程**（shard），由单个 router 进程持有全部客户端 socket，
//   客户端输入经「跨进程共享内存 SPSC 环」送给对应 shard，shard 跑完模拟后
//   把状态经另一条环送回 router，再由 router 转发给客户端。
//
//   拓扑（经典 front-proxy + 分片后端）：
//     客户端 --UDP(输入)--> router --[shm 环]--> shard(进程A, 房间 0,2,4..)
//     客户端 <--TCP(状态)-- router <--[shm 环]-- shard(进程B, 房间 1,3,5..)
//     客户端 --UDP(输入)--> router --[shm 环]--> shard(进程B, ...)
//
// ============================================================================
// 【为什么值得做（相对多线程分片）】
//   进程隔离是比线程隔离更强的不变量：shard 之间**物理上不共享地址空间**，
//   任何「共享状态导致竞态」在编译/运行层面都不可能发生——这正是迈向
//   「跨机分片」的第一步（下一步才是网络转发 + 一致性哈希，本 demo 不做）。
//   通道用无锁 SPSC 环放在共享内存里，router 生产 / shard 消费（每条环唯一
//   生产者与消费者），跨进程原子量落在同一物理页，无需任何锁。
//
// ============================================================================
// 【与多线程分片的一致性】
//   房间之间无共享状态 → 分片只改变「哪个房间归哪条进程/线程」。本 demo 复用
//   与多线程分片完全相同的 round-robin（room_index % shards）分配与「最差分片
//   tick p99 决定整体是否在预算内」的验收标准。差别仅在于：worker 是进程而非线程，
//   进程间通道是共享内存环而非函数内直接调用。
//
// 用法（通常不直接手敲，由 router 自动 fork/exec 出 shard）：
//   router 角色: ./mprocshard router <rooms> <shards> <players> <duration_s>
//   shard  角色: ./mprocshard shard  <shard_id> <shards> <rooms> <duration_s> <shm_base>
// 退出码: 0 = 多进程分片下最差分片 tick 在预算内 + 零背压 + TCP 字节对上；否则 1
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "core/rng.h"
#include "core/world.h"
#include "net/concurrent.h"
#include "net/latency_histogram.h"
#include "net/net.h"
#include "net/room_shard.h"
#include "net/serialize.h"
#include "net/shm.h"

#ifndef _WIN32
#  include <sys/wait.h>
#endif

using namespace synq;
using namespace synq::net;
using Clock = std::chrono::steady_clock;

namespace {

// router 用于 spawn 子进程的同一二进制路径（在 main 里赋值；前置声明以便 run_router 使用）
std::string argv0_global;

constexpr int kUdpBasePort = 19100;  // 房间 i 的输入端口 = 19100 + i（与 multiroom 一致）
constexpr int kTcpBasePort = 20100;  // 房间 i 的状态端口 = 20100 + i
constexpr std::size_t kRingCap = 2048;  // 每条共享内存环容量（2 的幂）

int g_one = 1;

// ---- 跨进程通道上搬运的包 ----
enum PktKind : std::uint8_t { kInput = 0, kTcpMsg = 1, kConnect = 2, kState = 3 };

struct Packet {
  std::uint32_t room_id;
  std::uint8_t kind;
  std::uint8_t player;
  std::uint16_t len;
  std::uint8_t data[384];  // 状态包（4 玩家全量约 70B，增量更小）；超限则 shard 丢弃
};
static_assert(std::is_trivially_copyable_v<Packet>, "Packet 必须可平凡拷贝");

// ---- 一个 shard 的双向共享内存通道（放在命名共享内存段里）----
template <std::size_t N>
struct ShardChannel {
  SpmcRing<Packet, N> to_shard;    // router 生产 / shard 消费（输入 / ack / reconnect / connect）
  SpmcRing<Packet, N> from_shard;  // shard 生产 / router 消费（状态广播）
  std::atomic<std::uint64_t> bp_in{0};          // router 推 to_shard 满时丢弃计数
  std::atomic<std::uint64_t> bp_out{0};         // shard 推 from_shard 满时丢弃计数
  std::atomic<bool> stop{false};
  // 结果（shard 结束时写，router 退出前读）
  std::atomic<std::uint64_t> ticks_done{0};
  std::atomic<std::uint64_t> input_pkts{0};
  std::atomic<std::uint64_t> state_pkts{0};
  std::atomic<std::uint64_t> state_bytes{0};
  std::atomic<std::uint64_t> tick_mean_us{0};
  std::atomic<std::uint64_t> tick_p99_us{0};
  std::atomic<std::uint64_t> tick_max_us{0};
  std::atomic<std::uint64_t> shard_backpressure{0};
  std::atomic<std::uint64_t> achieved_hz_x100{0};
};

// ===========================================================================
// 跨平台：fork/exec（POSIX）或 CreateProcess（Windows）spawn 出 shard 子进程
// ===========================================================================
struct Child { int pid;
#ifdef _WIN32
  HANDLE h = nullptr;
#endif
};

int my_pid() {
#ifdef _WIN32
  return static_cast<int>(::GetCurrentProcessId());
#else
  return static_cast<int>(::getpid());
#endif
}

bool spawn_shard(const std::string& exe, const std::vector<std::string>& args,
                 Child* out) {
#ifdef _WIN32
  // 命令行必须包含 exe 作为 argv[0]，使子进程的 argv[1] == "shard"（与 POSIX 路径一致：
  // POSIX 用 cargs=[exe, "shard", ...] 调 execv）。否则 Windows 下 argv[0] 会变成 "shard"，
  // 导致 main 读到的 role=argv[1] 变成 "0" 而非 "shard"。
  std::string cmd = "\"";
  cmd += exe;
  cmd += "\"";
  for (auto& a : args) { cmd += " "; cmd += a; }
  std::vector<char> buf(cmd.begin(), cmd.end());
  buf.push_back('\0');
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!::CreateProcessA(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi))
    return false;
  out->pid = static_cast<int>(pi.dwProcessId);
  out->h = pi.hProcess;  // 仅保留进程句柄用于等待；线程句柄关闭
  ::CloseHandle(pi.hThread);
  return true;
#else
  pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    std::vector<char*> cargs;
    cargs.push_back(const_cast<char*>(exe.c_str()));
    for (auto& a : args) cargs.push_back(const_cast<char*>(a.c_str()));
    cargs.push_back(nullptr);
    // execvp（而非 execv）：当以「无斜杠」方式调用（如 PATH 里直接敲 mprocshard）时，
    // execv 因不搜 PATH 会失败、子进程退 127、分片根本没起来。execvp 搜 PATH，行为更稳。
    ::execvp(exe.c_str(), cargs.data());
    ::_exit(127);
  }
  out->pid = pid;
  return true;
#endif
}

// ===========================================================================
// socket 封装（与 multiroom_net_test 一致；router 持有全部客户端 socket）
// ===========================================================================
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
  if (socket_failed(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))) {
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
  if (socket_failed(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) ||
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
    if (c.attack_target == player) c.attack_target = (player + 1) % kMaxPlayers;
  }
  if (r.range(0, 9) == 0) c.cast_spell = 1;
  return c;
}

// ---- router 侧的「房间 + 它的真实 socket」（不含 BattleRoom，BattleRoom 在 shard 里）
struct RouterRoom {
  int udp_fd = -1;
  int tcp_listen = -1;
  std::vector<int> conns;
  // router 统计
  std::uint64_t state_sent = 0;
  std::uint64_t state_bytes = 0;
  std::uint64_t backpressure = 0;  // router 转发状态给客户端时缓冲满丢弃
  // 客户端侧统计（客户端线程写，同进程）
  std::uint64_t cli_inputs_sent = 0;
  std::uint64_t cli_state_recv = 0;
  std::uint64_t cli_state_bytes = 0;
  std::uint64_t cli_send_fail = 0;
  bool cli_connected = false;
};

void try_send(RouterRoom& R, int fd, const std::uint8_t* data, std::size_t len) {
  const int n = ::send(fd, reinterpret_cast<const char*>(data),
                       static_cast<int>(len), 0);
  if (socket_failed(n) || static_cast<std::size_t>(n) != len) {
    ++R.backpressure;
    return;
  }
  ++R.state_sent;
  R.state_bytes += len;
}

// ---- 客户端线程：与 multiroom 完全一致（连 router 的真实 socket）----
void run_room_clients(RouterRoom* R, int room_id, int players, double duration_s,
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
    while (!go->load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

// ===========================================================================
// shard 角色：在独立进程里跑分配给自己的房间
// ===========================================================================
int run_shard(int shard_id, int shards, int rooms, int duration_s,
              const std::string& shm_base) {
  const std::string ch_name = shm_base + "_" + std::to_string(shard_id);
  Shm shm;
  if (!shm.open(ch_name, sizeof(ShardChannel<kRingCap>))) {
    std::fprintf(stderr, "[shard %d] 无法打开共享内存 %s\n", shard_id,
                ch_name.c_str());
    return 2;
  }
  auto* ch = reinterpret_cast<ShardChannel<kRingCap>*>(shm.ptr());

  // 收集本 shard 拥有的房间，并建立 room_id -> 本地下标 的 O(1) 映射
  std::vector<int> owned;
  for (int r = shard_id; r < rooms; r += shards) owned.push_back(r);
  std::vector<int> room_to_local(rooms, -1);
  // BattleRoom 内含 std::atomic（NetStats），既不可拷贝也不可移动 → 用 unique_ptr。
  std::vector<std::unique_ptr<BattleRoom>> room_objs;
  room_objs.reserve(owned.size());
  for (int i = 0; i < static_cast<int>(owned.size()); ++i) {
    room_objs.push_back(std::make_unique<BattleRoom>(
        static_cast<std::uint64_t>(7000 + owned[static_cast<std::size_t>(i)])));
    room_to_local[static_cast<std::size_t>(owned[static_cast<std::size_t>(i)])] = i;
  }
  std::vector<char> pending_full(owned.size(), 0);  // 某房间有客户端连入 → 下次发全量

  LatencyHistogram tick_cost;
  const int total_ticks = duration_s * kTickRate;
  const double frame_ms = 1000.0 / static_cast<double>(kTickRate);
  auto next = Clock::now();
  const auto t_begin = next;
  int ticks_done = 0;
  std::uint64_t shard_bp = 0;

  for (int t = 0; t < total_ticks; ++t) {
    if (ch->stop.load(std::memory_order_relaxed)) break;
    const auto t0 = Clock::now();

    // 1) 收 router 送来的输入/ack/reconnect/connect
    Packet pkt;
    while (ch->to_shard.try_pop(pkt)) {
      if (pkt.room_id >= static_cast<std::uint32_t>(rooms)) continue;
      const int li = room_to_local[static_cast<std::size_t>(pkt.room_id)];
      if (li < 0) continue;
      BattleRoom& room = *room_objs[static_cast<std::size_t>(li)];
      if (pkt.kind == kInput) {
        room.on_input_packet(pkt.data, pkt.len);
      } else if (pkt.kind == kTcpMsg) {
        const auto type = static_cast<MsgType>(pkt.data[0]);
        Reader r{pkt.data, pkt.len, 1};
        if (type == MsgType::kAck) {
          room.on_ack(r.get_varint(), r.get_varint());
        } else if (type == MsgType::kReconnect) {
          room.on_reconnect(r.get_varint(), r.get_varint());
        }
      } else if (pkt.kind == kConnect) {
        pending_full[static_cast<std::size_t>(li)] = 1;  // 握手：下个 tick 发全量
      }
    }

    // 2) 推进本 shard 拥有的每个房间
    for (int i = 0; i < static_cast<int>(owned.size()); ++i) {
      BattleRoom& room = *room_objs[static_cast<std::size_t>(i)];
      room.tick();
      const int rid = owned[static_cast<std::size_t>(i)];

      // 3) #4 超界输入重同步 → 全量
      bool sent_full = false;
      for (int p = 0; p < kMaxPlayers; ++p) {
        if (room.needs_resync(p)) {
          auto full = room.build_state_packet(true);
          Packet op{};
          op.room_id = static_cast<std::uint32_t>(rid);
          op.kind = kState;
          op.len = static_cast<std::uint16_t>(full.size());
          if (full.size() <= sizeof(op.data)) {
            std::memcpy(op.data, full.data(), full.size());
            if (!ch->from_shard.try_push(op)) ++shard_bp;
          }
          room.clear_resync(p);
          sent_full = true;
        }
      }
      // 4) 握手全量 或 增量
      if (pending_full[static_cast<std::size_t>(i)] && !sent_full) {
        auto full = room.build_state_packet(true);
        Packet op{};
        op.room_id = static_cast<std::uint32_t>(rid);
        op.kind = kState;
        op.len = static_cast<std::uint16_t>(full.size());
        if (full.size() <= sizeof(op.data)) {
          std::memcpy(op.data, full.data(), full.size());
          if (!ch->from_shard.try_push(op)) ++shard_bp;
        }
        pending_full[static_cast<std::size_t>(i)] = 0;
      } else if (!sent_full) {
        auto delta = room.build_state_packet(false);
        Packet op{};
        op.room_id = static_cast<std::uint32_t>(rid);
        op.kind = kState;
        op.len = static_cast<std::uint16_t>(delta.size());
        if (delta.size() <= sizeof(op.data)) {
          std::memcpy(op.data, delta.data(), delta.size());
          if (!ch->from_shard.try_push(op)) ++shard_bp;
        }
      }
    }

    ++ticks_done;
    tick_cost.record(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0)
            .count()));
    next += std::chrono::microseconds(static_cast<std::int64_t>(frame_ms * 1000));
    std::this_thread::sleep_until(next);
  }

  // 写结果（router 退出前读）
  const double secs =
      std::chrono::duration<double>(Clock::now() - t_begin).count();
  std::uint64_t in_pk = 0, st_pk = 0, st_by = 0;
  for (auto& up : room_objs) {
    BattleRoom& room = *up;
    in_pk += room.net_stats().input_pkts.load();
    st_pk += room.net_stats().state_pkts.load();
    st_by += room.net_stats().state_bytes.load();
  }
  auto ss = tick_cost.summary();
  ch->ticks_done.store(ticks_done, std::memory_order_release);
  ch->input_pkts.store(in_pk, std::memory_order_release);
  ch->state_pkts.store(st_pk, std::memory_order_release);
  ch->state_bytes.store(st_by, std::memory_order_release);
  ch->tick_mean_us.store(static_cast<std::uint64_t>(ss.mean), std::memory_order_release);
  ch->tick_p99_us.store(static_cast<std::uint64_t>(ss.p99), std::memory_order_release);
  ch->tick_max_us.store(static_cast<std::uint64_t>(ss.max), std::memory_order_release);
  ch->shard_backpressure.store(shard_bp, std::memory_order_release);
  ch->achieved_hz_x100.store(
      secs > 0 ? static_cast<std::uint64_t>(ticks_done / secs * 100.0) : 0,
      std::memory_order_release);
  return 0;
}

// ===========================================================================
// router 角色：持有客户端 socket，经共享内存环转发到 shard
// ===========================================================================
int run_router(int rooms, int shards, int players, int duration_s) {
  if (shards < 1) shards = 1;
  if (shards > rooms) shards = rooms;
  if (init_net() != 0) {
    std::fprintf(stderr, "网络初始化失败\n");
    return 2;
  }

  // 1) 创建共享内存通道（每 shard 一条）
  const std::string shm_base = "/synq_mp_" + std::to_string(my_pid());
  std::vector<Shm> shms;
  std::vector<ShardChannel<kRingCap>*> chs;
  shms.reserve(static_cast<std::size_t>(shards));
  chs.reserve(static_cast<std::size_t>(shards));
  for (int s = 0; s < shards; ++s) {
    // 先试着 unlink：清掉上一次崩溃遗留的同名段（忽略失败）
    Shm().unlink(shm_base + "_" + std::to_string(s));
    Shm shm;
    if (!shm.create(shm_base + "_" + std::to_string(s),
                    sizeof(ShardChannel<kRingCap>))) {
      std::fprintf(stderr, "[router] 共享内存创建失败（shard %d）\n", s);
      return 2;
    }
    auto* ch = reinterpret_cast<ShardChannel<kRingCap>*>(shm.ptr());
    new (ch) ShardChannel<kRingCap>();  // 显式构造：清零原子量/环
    shms.push_back(std::move(shm));
    chs.push_back(ch);
  }

  // 2) fork/exec 出 shard 子进程
  std::vector<Child> children;
  children.reserve(static_cast<std::size_t>(shards));
  for (int s = 0; s < shards; ++s) {
    std::vector<std::string> args = {
        "shard", std::to_string(s), std::to_string(shards),
        std::to_string(rooms), std::to_string(duration_s), shm_base};
    Child c{};
    if (!spawn_shard(std::string(argv0_global), args, &c)) {
      std::fprintf(stderr, "[router] 启动 shard %d 失败\n", s);
      return 2;
    }
    children.push_back(c);
  }

  // 3) 绑每房间真实 UDP/TCP socket + 起客户端线程
  std::vector<std::unique_ptr<RouterRoom>> nets;
  nets.reserve(static_cast<std::size_t>(rooms));
  for (int i = 0; i < rooms; ++i)
    nets.push_back(std::make_unique<RouterRoom>());
  int bind_fail = 0;
  for (int i = 0; i < rooms; ++i) {
    RouterRoom& R = *nets[static_cast<std::size_t>(i)];
    R.udp_fd = make_udp_listener(kUdpBasePort + i);
    R.tcp_listen = make_tcp_listener(kTcpBasePort + i);
    if (R.udp_fd < 0 || R.tcp_listen < 0) ++bind_fail;
  }

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
  go.store(true, std::memory_order_release);

  std::printf("==========================================================\n");
  std::printf(" 单机多进程房间分片（共享内存 SPSC 环通道）\n");
  std::printf("==========================================================\n");
  std::printf(" 房间数        : %d\n", rooms);
  std::printf(" 分片进程数    : %d (每进程约 %d 房间, round-robin)\n", shards,
              (rooms + shards - 1) / shards);
  std::printf(" 每房间玩家    : %d\n", players);
  std::printf(" 时长          : %d 秒 @ %d Hz\n", duration_s, kTickRate);
  std::printf(" 共享内存通道  : %d 条 (各含 2×%zu 环, 容量 %zu)\n", shards,
              kRingCap, kRingCap);
  std::printf(" 机器          : %u 逻辑核\n\n", std::thread::hardware_concurrency());

  // 4) 转发主循环：单线程既生产 to_shard（按房间）又消费 from_shard（按分片路由）
  //    —— 关键不变式：每条 SPSC 环只有唯一生产者与唯一消费者。from_shard 的 router
  //       侧消费必须**按分片一次性 drain 完再按 room_id 路由**，绝不在「每个房间」的
  //       循环里各自 drain 整条环，否则同分片其它房间的包会被弹出后丢弃（早期实现的 bug）。
  const double loop_s = static_cast<double>(duration_s);
  const auto cli_deadline =
      t_start + std::chrono::milliseconds(static_cast<int>(duration_s * 1000));
  const auto relay_end = Clock::now() + std::chrono::milliseconds(
                              static_cast<int>(duration_s * 1000) + 1500);
  std::uint8_t rbuf[2048]{};
  while (Clock::now() < relay_end) {
    // 4a) 每个房间：收 UDP 输入 / accept / 收 TCP ack-reconnect → 推对应分片入口环
    for (int i = 0; i < rooms; ++i) {
      RouterRoom& R = *nets[static_cast<std::size_t>(i)];
      const int s = shard_of(i, shards);
      ShardChannel<kRingCap>* ch = chs[static_cast<std::size_t>(s)];

      // UDP 输入 → 送对应 shard
      for (;;) {
        const int n = ::recvfrom(R.udp_fd, reinterpret_cast<char*>(rbuf),
                                 static_cast<int>(sizeof(rbuf)), 0, nullptr, nullptr);
        if (n <= 0) break;
        Packet pkt{};
        pkt.room_id = static_cast<std::uint32_t>(i);
        pkt.kind = kInput;
        pkt.len = static_cast<std::uint16_t>(n);
        std::memcpy(pkt.data, rbuf, static_cast<std::size_t>(n));
        if (!ch->to_shard.try_push(pkt)) ++ch->bp_in;  // 环满 → 丢（计入背压）
      }

      // 接纳新连接 → 通知 shard 发握手全量
      if (R.conns.size() < static_cast<std::size_t>(players)) {
        for (;;) {
          int c = ::accept(R.tcp_listen, nullptr, nullptr);
          if (socket_failed(c)) break;
          set_nonblock(c);
          ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<char*>(&g_one), sizeof(g_one));
          int sndbuf = 256 * 1024;
          ::setsockopt(c, SOL_SOCKET, SO_SNDBUF,
                       reinterpret_cast<char*>(&sndbuf), sizeof(sndbuf));
          R.conns.push_back(c);
          Packet pkt{};
          pkt.room_id = static_cast<std::uint32_t>(i);
          pkt.kind = kConnect;
          pkt.len = 0;
          if (!ch->to_shard.try_push(pkt)) ++ch->bp_in;
        }
      }

      // TCP 收 ack/reconnect → 送对应 shard（本 demo 客户端不发，路径保留）
      for (int c : R.conns) {
        for (;;) {
          const int n = ::recv(c, reinterpret_cast<char*>(rbuf),
                               static_cast<int>(sizeof(rbuf)), 0);
          if (n <= 0) break;
          Packet pkt{};
          pkt.room_id = static_cast<std::uint32_t>(i);
          pkt.kind = kTcpMsg;
          pkt.len = static_cast<std::uint16_t>(n);
          std::memcpy(pkt.data, rbuf, static_cast<std::size_t>(n));
          if (!ch->to_shard.try_push(pkt)) ++ch->bp_in;
        }
      }
    }

    // 4b) 按分片 drain 出口环，按 room_id 路由给对应房间的客户端连接。
    //     fwd=false（已过客户端 deadline）时只 drain 不转发——保持环空让 shard 不堆积
    //     背压，但不在客户端停止 recv 之后继续发（否则发送字节 > 接收字节，差分会超 2%）。
    const bool fwd = Clock::now() < cli_deadline;
    for (int s = 0; s < shards; ++s) {
      Packet out;
      while (chs[static_cast<std::size_t>(s)]->from_shard.try_pop(out)) {
        if (!fwd) continue;
        if (out.room_id >= static_cast<std::uint32_t>(rooms)) continue;
        RouterRoom& R = *nets[static_cast<std::size_t>(out.room_id)];
        for (int c : R.conns) try_send(R, c, out.data, out.len);
      }
    }

    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }

  // 5) 通知 shard 停止并等待子进程退出（shard 已按 duration 跑完）
  for (auto* ch : chs) ch->stop.store(true, std::memory_order_relaxed);
  for (auto& c : children) {
#ifdef _WIN32
    if (c.h) {
      ::WaitForSingleObject(c.h, INFINITE);
      ::CloseHandle(c.h);
    }
#else
    int st = 0;
    ::waitpid(c.pid, &st, 0);
#endif
  }

  // 6) 收尾 drain（此时已超过客户端 deadline，仅 drain 不转发；让 shard 出口环保持可写，
  //     使各 shard 在收到 stop 前不会因环满而积压背压）
  for (int s = 0; s < shards; ++s) {
    Packet out;
    while (chs[static_cast<std::size_t>(s)]->from_shard.try_pop(out)) {
      (void)out;
    }
  }

  for (auto& th : clients) th.join();

  // 7) 关 socket
  for (auto& up : nets) {
    RouterRoom& R = *up;
    for (int c : R.conns) close_socket(c);
    if (!socket_failed(R.tcp_listen)) close_socket(R.tcp_listen);
    if (!socket_failed(R.udp_fd)) close_socket(R.udp_fd);
  }
  shutdown_net();

  // 8) 汇总（与 multiroom_net_test 同口径，但跨进程）
  std::uint64_t tok_state = 0, tok_state_bytes = 0, tback = 0;
  std::uint64_t tcli_inputs = 0, tcli_bytes = 0, tcli_fail = 0;
  int rooms_ok = 0, rooms_noconn = 0;
  for (auto& up : nets) {
    RouterRoom& R = *up;
    tok_state += R.state_sent;
    tok_state_bytes += R.state_bytes;
    tback += R.backpressure;
    tcli_inputs += R.cli_inputs_sent;
    tcli_bytes += R.cli_state_bytes;
    tcli_fail += R.cli_send_fail;
    if (R.cli_state_recv > 0) ++rooms_ok;
    if (!R.cli_connected) ++rooms_noconn;
  }
  // shard 侧统计 + 跨进程背压
  double achieved_hz = 1e30;
  std::uint64_t worst_p99 = 0, worst_max = 0;
  std::uint64_t shard_in = 0, shard_bp = 0;
  int min_ticks = duration_s * kTickRate;
  const int expected_ticks = duration_s * kTickRate;
  for (auto* ch : chs) {
    shard_in += ch->input_pkts.load();
    shard_bp += ch->shard_backpressure.load() + ch->bp_in.load() + ch->bp_out.load();
    const std::uint64_t p99 = ch->tick_p99_us.load();
    if (p99 > worst_p99) worst_p99 = p99;
    if (ch->tick_max_us.load() > worst_max) worst_max = ch->tick_max_us.load();
    const double hz = ch->achieved_hz_x100.load() / 100.0;
    if (hz < achieved_hz) achieved_hz = hz;
    const int td = static_cast<int>(ch->ticks_done.load());
    if (td < min_ticks) min_ticks = td;
  }
  tback += shard_bp;  // 总背压 = router 转发丢弃 + shard 推送丢弃

  const double frame_us = 1000.0 * 1000.0 / kTickRate;
  const bool tick_in_budget = static_cast<double>(worst_p99) <= frame_us;
  const double sent_kb = tok_state_bytes / 1024.0;
  const double recv_kb = tcli_bytes / 1024.0;
  const double bytes_diff_pct =
      tok_state_bytes > 0
          ? 100.0 * static_cast<double>(tok_state_bytes - tcli_bytes) /
                static_cast<double>(tok_state_bytes)
          : 0.0;

  std::printf("----------------------------------------------------------\n");
  std::printf(" 结果\n");
  std::printf("----------------------------------------------------------\n");
  std::printf(" 正常房间(收到状态): %d / %d\n", rooms_ok, rooms);
  std::printf(" 客户端连接失败房间: %d\n", rooms_noconn);
  std::printf(" 分片进程          : %d 个 (每进程约 %d 房间)\n", shards,
              (rooms + shards - 1) / shards);
  std::printf(" shard tick 完成  : %d / %d (最少的那条, 满额阈值 %d)\n", min_ticks,
              duration_s * kTickRate, static_cast<int>(expected_ticks * 0.95));
  std::printf(" 实际 tick 频率    : %.1f Hz (目标 %d) %s\n", achieved_hz, kTickRate,
              achieved_hz >= kTickRate * 0.95 ? "[未饱和]" : "[已饱和/掉帧]");
  std::printf(" 输入包(服务端实收): %llu  (%.0f 包/秒)\n", (unsigned long long)shard_in,
              shard_in / loop_s);
  std::printf(" 输入包(客户端发出): %llu  (失败 %llu)\n",
              (unsigned long long)tcli_inputs, (unsigned long long)tcli_fail);
  std::printf(" 状态包(服务端发出): %llu  (%.0f 包/秒, 背压丢弃 %llu)\n",
              (unsigned long long)tok_state, tok_state / loop_s,
              (unsigned long long)tback);
  std::printf(" 状态字节 发出/收到: %.1f KB / %.1f KB (差 %.2f%%, 应≈0)\n", sent_kb,
              recv_kb, bytes_diff_pct);
  std::printf(" 每条分片 tick     : p99 %llu us / max %llu us (跨进程共享内存环通道)\n",
              (unsigned long long)worst_p99, (unsigned long long)worst_max);
  std::printf(" tick 预算占用     : %.2f%% (预算 %.0f us, 取最差分片)\n",
              100.0 * static_cast<double>(worst_p99) / frame_us, frame_us);
  std::printf(" 容量判定          : %s (最差分片 p99 %llu us vs 预算 %.0f us)\n",
              tick_in_budget ? "[在预算内]" : "[超预算/掉帧]",
              (unsigned long long)worst_p99, frame_us);

  // 每个分片都跑满 tick 预算（允许 5% 时序抖动）：容量 demo 的核心不变量——
  // 若某分片只跑了半数 tick（被饿死/卡住），不该判容量 OK。
  const bool ticks_full =
      min_ticks >= static_cast<int>(static_cast<double>(expected_ticks) * 0.95);
  const bool pass = (bind_fail == 0 && rooms_ok == rooms && rooms_noconn == 0 &&
                     tback == 0 && bytes_diff_pct < 2.0 && tick_in_budget &&
                     achieved_hz >= kTickRate * 0.95 && ticks_full);
  std::printf("\n==========================================================\n");
  if (pass) {
    std::printf("MPROC_OK rooms=%d players=%d shards=%d qps=%.0f tick_p99_us=%llu\n",
                rooms, players, shards, shard_in / loop_s,
                (unsigned long long)worst_p99);
  } else {
    std::printf("MPROC_FAIL rooms_ok=%d/%d shards=%d noconn=%d backpressure=%llu bytes_diff=%.2f%% tick_p99_us=%llu (budget %.0f)\n",
                rooms_ok, rooms, shards, rooms_noconn, (unsigned long long)tback,
                bytes_diff_pct, (unsigned long long)worst_p99, frame_us);
  }
  std::printf("==========================================================\n");

  // 清理共享内存
  for (int s = 0; s < shards; ++s) {
    shms[static_cast<std::size_t>(s)].unlink(shm_base + "_" +
                                            std::to_string(s));
    shms[static_cast<std::size_t>(s)].close();
  }
  return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "用法: %s router <rooms> <shards> <players> <duration_s>\n"
                 "      %s shard <shard_id> <shards> <rooms> <duration_s> <shm_base>\n",
                 argv[0], argv[0]);
    return 2;
  }
  argv0_global = argv[0];
  const std::string role = argv[1];

  if (role == "shard") {
    if (argc < 7) return 2;
    const int shard_id = std::atoi(argv[2]);
    const int shards = std::atoi(argv[3]);
    const int rooms = std::atoi(argv[4]);
    const int duration_s = std::atoi(argv[5]);
    const std::string shm_base = argv[6];
    return run_shard(shard_id, shards, rooms, duration_s, shm_base);
  }
  if (role == "router") {
    const int rooms = argc > 2 ? std::atoi(argv[2]) : 32;
    const int shards = argc > 3 ? std::atoi(argv[3]) : 2;
    int players = argc > 4 ? std::atoi(argv[4]) : kMaxPlayers;
    const int duration_s = argc > 5 ? std::atoi(argv[5]) : 10;
    if (players > kMaxPlayers) players = kMaxPlayers;
    if (rooms <= 0 || players <= 0 || duration_s <= 0) return 2;
    return run_router(rooms, shards, players, duration_s);
  }
  std::fprintf(stderr, "未知角色: %s\n", role.c_str());
  return 2;
}

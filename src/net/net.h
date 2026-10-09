// net.h — 游戏服网络层（UDP 输入 + TCP 状态广播 + 断线重连）
//
// 【为什么游戏服用 UDP 收发玩家输入】
//   输入包特点：极小（10~20 字节）、高频（每秒 30 次）、可丢不可卡。
//   TCP 的三次握手 + 队头阻塞(HOL)在 100ms RTT 下会直接毁掉手感：
//   一个包丢了，后面所有包都要等重传 —— 而输入包丢了就该丢（下一帧会补）。
//   所以：输入走 UDP，状态广播走 TCP（可靠、有序、便于断线重连接管）。
//
// 【这正是真实游戏服的架构】
//   客户端 --UDP(输入)--> 游戏服
//   客户端 <--TCP(状态)-- 游戏服
//   重连时：客户端带 last_ack_seq 请求全量状态快照
//
// 【断线重连的核心问题】
//   重连后服务端必须让客户端「追平」到当前帧。两种方案：
//   A. 客户端快进模拟：把错过的输入全跑一遍（需要客户端保存输入历史）
//   B. 服务端下发全量快照：从当前帧的快照环取最近一个，客户端直接跳到该帧
//   本项目用 B + 混合：下发最近快照，再补跑剩余帧（快进窗口小）

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/rollback.h"
#include "core/world.h"
#include "net/serialize.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <cerrno>
using socklen_t = int;
#  define close_socket closesocket
#  define last_error() WSAGetLastError()
#  define ERR_WOULDBLOCK WSAEWOULDBLOCK
inline int init_net() {
  WSADATA w;
  return WSAStartup(MAKEWORD(2, 2), &w);
}
inline void shutdown_net() { WSACleanup(); }
inline int set_nonblock(int fd) {
  u_long on = 1;
  return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &on);
}
#  define SOCK_ERROR WSAGetLastError()
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <cerrno>
#  // set_nonblock() 需要 fcntl / F_GETFL / F_SETFL / O_NONBLOCK，
#  // 这些符号由 <fcntl.h> 提供，**不能依赖其他头文件间接引入**。
#  //
#  // 【踩坑·只在 Linux 暴露的编译错误】
#  // 最初漏了这一行，于是本文件从来没在 Linux 上被编译过：
#  //     src/net/net.h:70: error: '::fcntl' has not been declared
#  //     src/net/net.h:70: error: 'F_GETFL' was not declared in this scope
#  // 原因很隐蔽 —— Windows 的 <winsock2.h> 会间接引入 <fcntl.h>，
#  // 所以本地（Windows/MSYS2）怎么编都能过，一上Linux CI 就挂。
#  // **「本地能编译」不等于「跨平台能编译」**，头文件依赖必须显式声明。
#  include <fcntl.h>
#  define close_socket close
#  define last_error() errno
#  define ERR_WOULDBLOCK EWOULDBLOCK
#  define SOCK_ERROR errno
inline int init_net() { return 0; }
inline void shutdown_net() {}
inline int set_nonblock(int fd) {
  int f = ::fcntl(fd, F_GETFL, 0);
  return ::fcntl(fd, F_SETFL, f | O_NONBLOCK);
}
#endif

// ===================================================================
// socket 调用失败判据 —— 收口所有「是否出错」的判断
// ===================================================================
//
// 【为什么需要这个宏】
// Windows 的 socket 函数失败返回 SOCKET_ERROR（值 -1），
// POSIX 返回 -1，两者数值其实一致，**但名字不同**：
// Windows 只有 winsock2.h 里的 SOCKET_ERROR，Linux 上根本没有这个名字。
//
// 于是产生了这四轮反复出现的错误：
//   业务代码直接写 SOCKET_ERROR → Windows 编得过，Linux 报
//   'SOCKET_ERROR' was not declared in this scope
//
// 业务层只需记住一个 socket_failed()，平台差异全部关在本文件内。
// **新增网络代码时不要直接写 -1 / SOCKET_ERROR / WSAGetLastError，统一用下面两个。**
#define socket_failed(rc) ((rc) < 0)
#define last_error_code() SOCK_ERROR

namespace synq {

// 每个玩家的连接状态
struct PlayerSession {
  int player_id = -1;
  int udp_fd = -1;        // 该玩家的 UDP 客户端地址（用 addr 标识）
  struct sockaddr_in addr{};
  bool connected = false;
  std::int32_t last_input_frame = -1;
  std::int32_t acked_frame = -1;   // 客户端已确认收到的最新帧
  std::int32_t disconnect_tick = -1;  // 断线时刻，超过宽限期则清理
  std::int64_t bytes_sent = 0;
  std::int64_t bytes_recv = 0;
  std::int32_t reconnect_count = 0;
};

// 网络统计（用于压测报告）
struct NetStats {
  std::atomic<std::uint64_t> input_pkts{0};
  std::atomic<std::uint64_t> input_bytes{0};
  std::atomic<std::uint64_t> state_pkts{0};
  std::atomic<std::uint64_t> state_bytes{0};
  std::atomic<std::uint64_t> drops{0};        // 队列满/解析失败丢弃
  std::atomic<std::uint64_t> reconnects{0};   // 重连次数
  std::atomic<std::uint64_t> timeouts{0};          // 输入超时判定
  std::atomic<std::uint64_t> backpressure_drops{0};  // 发送队列满，背压丢弃的状态包
};

// 游戏服：一个房间 = 一个 BattleRoom
class BattleRoom {
 public:
  explicit BattleRoom(std::uint64_t seed)
      : session_(seed), prev_world_(session_.world()) {}

  // UDP 输入包到达
  // 【性能】不能在这里构造 std::vector（每包一次堆分配）—— 输入包是
  // 热路径（每秒 30 包 × N 人），堆分配会成为瓶颈。直接用 Reader 原地解析。
  void on_input_packet(const std::uint8_t* data, std::size_t len) {
    net_stats_.input_pkts.fetch_add(1, std::memory_order_relaxed);
    net_stats_.input_bytes.fetch_add(len, std::memory_order_relaxed);

    Reader r{data, len, 0};
    auto type = static_cast<MsgType>(r.get_u8());
    if (type != MsgType::kInput || !r.ok()) {
      net_stats_.drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::int32_t frame = r.get_varint();
    int player = r.get_varint();
    if (player < 0 || player >= kMaxPlayers || !r.ok()) {
      net_stats_.drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    Command cmd = get_command(r);
    if (!r.ok()) {
      net_stats_.drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // 幂等：UDP 会重复投递，重复包直接丢弃
    if (frame <= last_seen_frame_[player]) return;
    last_seen_frame_[player] = frame;
    if (frame > last_input_frame_[player]) last_input_frame_[player] = frame;
    session_.on_input(player, frame, cmd);
  }

  // 客户端 ACK（已收到第 N 帧）
  void on_ack(int player, std::int32_t frame) {
    if (player >= 0 && player < kMaxPlayers) {
      if (frame > sessions_[player].acked_frame)
        sessions_[player].acked_frame = frame;
    }
  }

  // 断线重连：仅恢复会话状态（置 connected、计 reconnects）。
  // 【注意】恢复用的全量快照并不在此下发——服务端在 accept 新连接时
  // 已立即下发一份全量快照（见 server_main.cpp），因此重连客户端会在
  // 至多 1 帧内拿到最新权威状态；本方法只负责把会话重新标记为在线。
  bool on_reconnect(int player, std::int32_t last_acked) {
    if (player < 0 || player >= kMaxPlayers) return false;
    sessions_[player].connected = true;
    sessions_[player].reconnect_count++;
    sessions_[player].disconnect_tick = -1;
    net_stats_.reconnects.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  // 推进一帧，返回本帧重算帧数
  std::int32_t tick() {
    prev_world_ = session_.world();
    std::int32_t rolls = session_.advance();
    // 检测输入超时（连续 timeout_ticks 帧无输入 -> 判定掉线）
    for (int p = 0; p < kMaxPlayers; ++p) {
      std::int32_t cur_input = last_input_frame_[p];
      if (cur_input < session_.tick() - kInputTimeoutFrames) {
        if (sessions_[p].connected && session_.tick() - cur_input > kInputTimeoutFrames) {
          sessions_[p].connected = false;
          sessions_[p].disconnect_tick = session_.tick();
          net_stats_.timeouts.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    return rolls;
  }

  const World& world() const { return session_.world(); }
  const World& prev_world() const { return prev_world_; }
  std::int32_t tick_count() const { return session_.tick(); }
  RollbackSession& session() { return session_; }
  NetStats& net_stats() { return net_stats_; }
  std::int32_t last_input_frame(int p) const { return last_input_frame_[p]; }
  void set_connected(int p, bool c) { sessions_[p].connected = c; }

  // 生成广播包（增量 or 全量）
  // 【设计】非 const：内部要累加网络统计（atomic::fetch_add 不是 const 方法）。
  std::vector<std::uint8_t> build_state_packet(bool full) {
    std::vector<std::uint8_t> pkt;
    if (full) {
      pkt = serialize_full_state(session_.tick(), session_.world());
    } else {
      pkt = serialize_delta(session_.tick(), session_.world(), prev_world_);
    }
    net_stats_.state_pkts.fetch_add(1, std::memory_order_relaxed);
    net_stats_.state_bytes.fetch_add(pkt.size(), std::memory_order_relaxed);
    return pkt;
  }

  static constexpr std::int32_t kInputTimeoutFrames = 90;  // 3 秒无输入判定掉线

 private:
  RollbackSession session_;
  World prev_world_;
  PlayerSession sessions_[kMaxPlayers];
  std::int32_t last_input_frame_[kMaxPlayers] = {0, 0, 0, 0};
  std::int32_t last_seen_frame_[kMaxPlayers] = {-1, -1, -1, -1};
  NetStats net_stats_;
};

}  // namespace synq

// room_shard.h — 房间分片（多模拟线程）
//
// ============================================================================
// 【为什么必须分片：先用实测数据说话】
//   multiroom_net_test 在单条模拟线程下实测（真实 socket × N 房间）：
//     32 房间: tick p99  9.2 ms（预算 33ms 的 27.6%）
//     64 房间: tick p99 18.4 ms（55.3%）
//    128 房间: tick p99 81.9 ms（245%）→ 打爆预算 → 背压丢弃 47837 + 掉帧
//   瓶颈是**模拟 CPU（单条线程）**，不是网络栈（网络层当时远未饱和）。
//   → 结论：单机承载上限由「一条模拟线程能跑多少房间」决定，必须横向拆成多条。
//
// ============================================================================
// 【为什么分片是安全的：核心不变式】
//   房间之间**没有任何共享状态** —— 各自独立的世界状态、快照环、输入表、socket。
//   分片只改变「哪个房间归哪条模拟线程推进」，**不改变**
//     「每个房间只被一条线程读写」
//   这条不变式。因此：
//     · 不需要任何锁（加锁反而会在大房间数下变成瓶颈）
//     · 不引入数据竞争 —— 由 ThreadSanitizer CI（tsan job）守护
//     · 不破坏确定性 —— 每个房间的 tick 顺序与频率不变，只是执行它的线程不同
//   ★ 反过来说：如果将来房间之间真要共享状态（跨房间广播、全局排行榜），
//     就必须显式设计同步，不能靠"分片了所以安全"这个理由糊过去。
//
// ============================================================================
// 【分片策略】round-robin（room_index % shard_count）
//   均摊负载；且与房间创建顺序无关地保持稳定（同一个房间永远归同一条线程）。
//
// 用法：
//   RoomShard shard(shard_id, shard_count, room_count,
//                   [&](int room_index) { /* 收包 + tick + 广播 */ });
//   shard.run(total_ticks, kTickRate);
// ============================================================================

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "core/world.h"  // kTickRate
#include "net/latency_histogram.h"

namespace synq {

// 房间 -> 分片编号（round-robin）。独立成函数，便于测试与复用。
inline int shard_of(int room_index, int shard_count) {
  const int s = std::max(1, shard_count);
  return ((room_index % s) + s) % s;  // 负索引也安全
}

template <typename ProcessRoom>
class RoomShard {
 public:
  // process_room: void(int room_index) —— 推进该房间一个 tick 的全部工作
  //               （收包 / tick / 广播）。**只能碰本分片自己的房间。**
  RoomShard(int shard_id, int shard_count, int room_count,
            ProcessRoom process_room)
      : shard_id_(shard_id),
        shard_count_(std::max(1, shard_count)),
        process_room_(std::move(process_room)) {
    for (int i = shard_id_; i < room_count; i += shard_count_)
      rooms_.push_back(i);
  }

  // 在当前线程推进 total_ticks 个 tick，目标频率 tick_hz。
  // 返回**实际达到的频率**（< 目标说明这条分片已饱和）。
  double run(int total_ticks, double tick_hz,
             const std::atomic<bool>* stop = nullptr) {
    using Clock = std::chrono::steady_clock;
    const double frame_ms = 1000.0 / static_cast<double>(tick_hz);
    auto next = Clock::now();
    const auto t_begin = next;

    ticks_done_ = 0;
    for (int t = 0; t < total_ticks; ++t) {
      if (stop != nullptr && stop->load(std::memory_order_relaxed)) break;
      const auto t0 = Clock::now();
      for (int r : rooms_) process_room_(r);
      tick_cost_.record(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                                t0)
              .count()));
      ++ticks_done_;
      next += std::chrono::microseconds(static_cast<std::int64_t>(frame_ms * 1000.0));
      std::this_thread::sleep_until(next);
    }

    const double secs =
        std::chrono::duration<double>(Clock::now() - t_begin).count();
    achieved_hz_ = secs > 0 ? ticks_done_ / secs : 0.0;
    return achieved_hz_;
  }

  int shard_id() const { return shard_id_; }
  int room_count() const { return static_cast<int>(rooms_.size()); }
  int ticks_done() const { return ticks_done_; }
  double achieved_hz() const { return achieved_hz_; }
  const LatencyHistogram& tick_cost() const { return tick_cost_; }

 private:
  int shard_id_;
  int shard_count_;
  ProcessRoom process_room_;
  std::vector<int> rooms_;
  LatencyHistogram tick_cost_;  // 内部用 atomic 计数，run() 结束后读取安全
  int ticks_done_ = 0;
  double achieved_hz_ = 0.0;
};

}  // namespace synq

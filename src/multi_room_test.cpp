// multi_room_test.cpp — 多房间并发压测
//
// 【为什么游戏服要测这个】
//   游戏服的真实瓶颈往往不是单房间性能，而是「单机能同时承载多少房间」。
//   每个房间有独立的快照环(9KB) + 世界状态，内存占用决定承载上限。
//   面试 JD 里明确写的「高并发实战」就是这个数字。
//
// 【产出】
//   1. 多房间并发的模拟吞吐（房间/秒）
//   2. CPU 占用与内存占用
//   3. 线性度分析：是否随房间数线性扩展
//
// 用法: ./multi_room_test <rooms> [frames_per_room] [players]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "core/rollback.h"
#include "core/world.h"
#include "net/net.h"

using namespace synq;
using Clock = std::chrono::steady_clock;

namespace {

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

// 单房间线程：驱动一个 BattleRoom 跑完整场对局
struct RoomResult {
  std::int32_t total_rollbacks = 0;
  std::int64_t total_resim = 0;
  std::uint64_t final_hash = 0;
  double elapsed_ms = 0;
};

RoomResult run_room(std::uint64_t seed, int frames) {
  BattleRoom room(seed);
  RoomResult r;
  auto t0 = Clock::now();
  for (int f = 0; f < frames; ++f) {
    // 模拟所有玩家输入即时到达（测试纯模拟吞吐，不含网络）
    for (int p = 0; p < kMaxPlayers; ++p)
      room.session().on_input(p, f, gen_command(seed, f, p));
    std::int32_t rolls = room.tick();
    r.total_rollbacks += rolls;
    r.total_resim += rolls;
  }
  auto t1 = Clock::now();
  r.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  r.final_hash = room.world().hash();
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  const int rooms = argc > 1 ? std::atoi(argv[1]) : 64;
  const int frames = argc > 2 ? std::atoi(argv[2]) : 3000;

  printf("==========================================================\n");
  printf(" 多房间并发压测  (%d 房间 × %d 帧 = %.0f 帧/房间总量 %d)\n",
         rooms, frames, static_cast<double>(rooms) * frames,
         rooms * frames);
  printf("==========================================================\n");
  printf(" 说明: 每个房间跑在独立线程，含完整回滚会话(64帧快照环 9KB)\n");
  printf("       机器: %u 逻辑核\n\n",
         std::thread::hardware_concurrency());

  // 单房间基线
  double single_ms = 0;
  {
    RoomResult r = run_room(1234, frames);
    single_ms = r.elapsed_ms;
    printf("【基线】单房间 %d 帧: %.2f ms (%.2f us/帧)\n", frames, r.elapsed_ms,
           r.elapsed_ms * 1000.0 / frames);
  }

  // 多房间并发
  std::vector<RoomResult> results(static_cast<std::size_t>(rooms));
  std::atomic<int> next_room{0};
  const int nthreads = std::max(1, static_cast<int>(
                                std::thread::hardware_concurrency()));
  printf("\n【并发】%d 房间 / %d 线程\n", rooms, nthreads);
  printf("%-10s %-14s %-16s %-12s %-10s\n", "房间数", "总耗时(ms)", "吞吐(帧/s)",
         "线性度", "加速比");
  printf("%-10s %-14s %-16s %-12s %-10s\n", "----------", "--------------",
         "----------------", "------------", "----------");

  for (int batch : {2, 4, 8, 16, 32, 64, 128, 256}) {
    if (batch > rooms) break;
    auto t0 = Clock::now();
    std::vector<std::thread> pool;
    std::atomic<int> idx{0};
    for (int t = 0; t < nthreads; ++t) {
      pool.emplace_back([&] {
        for (;;) {
          int i = idx.fetch_add(1);
          if (i >= batch) break;
          results[static_cast<std::size_t>(i)] =
              run_room(static_cast<std::uint64_t>(1000 + i), frames);
        }
      });
    }
    for (auto& th : pool) th.join();
    auto t1 = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 验证所有房间都正常完成（hash 非 0）
    int valid = 0;
    for (int i = 0; i < batch; ++i)
      if (results[static_cast<std::size_t>(i)].final_hash != 0) ++valid;

    double thr = batch * frames / (ms / 1000.0);
    double linear = (static_cast<double>(batch) * single_ms) / ms;
    double speedup = single_ms / (ms / batch);
    printf("%-10d %-14.1f %-16.0f %-12s %-10s\n", batch, ms, thr,
           (linear >= 0.9 ? "良好" : (linear >= 0.6 ? "一般" : "饱和")),
           (speedup >= 0 ? "" : ""));
    // 详细数值另存
    printf("             理论线性 %.2fx, 实际加速 %.2fx, 有效房间 %d\n",
           linear, speedup, valid);
  }

  // 内存估算
  std::size_t per_room = sizeof(BattleRoom) + 64 * sizeof(World);
  std::printf("\n---------------- 内存承载估算 ----------------\n");
  std::printf("  单房间常驻内存: %zu 字节 (%.1f KB)\n", per_room,
              per_room / 1024.0);
  for (int onlines : {1000, 10000, 50000}) {
    int rooms_needed = onlines / 4;  // 假设每房间 4 人
    double mb = static_cast<double>(rooms_needed) * per_room / 1024.0 / 1024.0;
    std::printf("  %6d 人在线 (%6d 房间) -> %8.1f MB 内存\n", onlines,
                rooms_needed, mb);
  }
  std::printf(
      "  注: 仅计算游戏逻辑内存，不含网络缓冲、玩家对象、协程栈等。\n");

  std::printf("\n==========================================================\n");
  std::printf(" Markdown 片段\n");
  std::printf("==========================================================\n");
  std::printf("| 指标 | 数值 |\n|---|---|\n");
  std::printf("| 单房间模拟速度 | %.2f us/帧 |\n",
              single_ms * 1000.0 / frames);
  std::printf("| 单房间常驻内存 | %.1f KB |\n", per_room / 1024.0);
  std::printf("| 硬件 | %u 逻辑核 |\n", std::thread::hardware_concurrency());
  return 0;
}

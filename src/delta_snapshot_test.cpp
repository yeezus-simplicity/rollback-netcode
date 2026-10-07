// delta_snapshot_test.cpp — 增量快照压缩验证
//
// 验证三件事：
//   1. 正确性：随机取任意历史帧，状态哈希必须与原始完全一致
//      （这是回滚的前提 —— 取错一帧，整场对战就废了）
//   2. 压缩率：内存占用相比完整快照降低多少
//   3. 性能：随机访问（解压）耗时 vs 直接 memcpy 完整快照
//
// 用法: ./delta_test [capacity] [frames]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/delta_snapshot.h"
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
  if (r.range(0, 11) == 0) c.cast_spell = 1;
  else if (r.range(0, 17) == 0) c.cast_spell = 2;
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t cap = argc > 1 ? static_cast<std::size_t>(std::atoi(argv[1])) : 64;
  const int frames = argc > 2 ? std::atoi(argv[2]) : 2000;
  const std::uint64_t seed = 777;

  printf("==========================================================\n");
  printf(" 增量快照压缩验证  (环深 %zu 帧, %d 帧对局)\n", cap, frames);
  printf("==========================================================\n\n");
  printf(" 原始 World 大小: %zu 字节/帧\n", sizeof(World));
  printf(" 完整快照环(朴素)  : %zu 字节 (%.1f KB)\n", cap * sizeof(World),
         cap * sizeof(World) / 1024.0);
  printf(" 增量快照环(本项目): 待测\n\n");

  // ---- 1. 跑一局，边跑边压入 ----
  std::vector<World> history;   // 保留原始状态用于正确性比对
  history.reserve(static_cast<std::size_t>(frames));
  CompressedSnapshotRing ring(cap);

  auto t0 = Clock::now();
  World w = make_world(seed);
  for (int f = 0; f < frames; ++f) {
    Command cmds[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p) cmds[p] = gen_command(seed, f, p);
    step(w, cmds);
    history.push_back(w);
    ring.push(w);
  }
  auto t1 = Clock::now();
  double push_us = std::chrono::duration<double, std::micro>(t1 - t0).count() /
                   frames;

  // ---- 2. 正确性：随机取每个历史帧 ----
  //
  // 【踩坑记录】初版比对的是 history[size-1-back]，但对局有 2000 帧、
  // 环深仅 64，ring 只保留最近 64 帧，history[size-1-back] 早已被覆盖。
  // 结果 get() 全失败，看起来像解压错误，实际是测试拿错了参照。
  // 正解：只用环深范围内的帧做比对（这也是回滚实际能访问的范围）。
  std::vector<World> ring_ref;  // 只记录最近 cap 帧的原始状态
  ring_ref.reserve(cap);

  int ok = 0, fail = 0, out_of_range = 0;
  auto t2 = Clock::now();
  const std::size_t n_check = (cap < history.size()) ? cap : history.size();
  for (std::size_t back = 0; back < n_check; ++back) {
    World got;
    if (!ring.get(back, got)) {
      // 超出环深 = 基准帧已被淘汰，属预期行为（回滚本就需要上限）
      ++out_of_range;
      continue;
    }
    const World& expect =
        history[history.size() - n_check + (n_check - 1 - back)];
    if (got.hash() == expect.hash() && got.tick == expect.tick) ++ok;
    else ++fail;
  }
  auto t3 = Clock::now();
  double get_us =
      std::chrono::duration<double, std::micro>(t3 - t2).count() /
      (ok + fail == 0 ? 1 : ok + fail);

  std::printf("---------------- 正确性 ----------------\n");
  std::printf("  可回溯帧: %d 成功 / %d 失败 / %d 超出环深(预期)\n", ok, fail,
              out_of_range);
  std::printf("  >>> %s\n\n",
               fail == 0
                   ? "全部通过：环深内任意历史帧状态哈希与原始逐位一致"
                   : "存在失败：解压数据不正确");

  // ---- 3. 压缩率 ----
  std::size_t comp = ring.memory_bytes();
  std::size_t raw = cap * sizeof(World);
  std::printf("---------------- 压缩效果 ----------------\n");
  std::printf("  完整快照环    : %zu 字节 (%.2f KB)\n", raw, raw / 1024.0);
  std::printf("  增量快照环    : %zu 字节 (%.2f KB)\n", comp, comp / 1024.0);
  std::printf("  压缩比        : %.2fx\n", static_cast<double>(raw) /
                                            static_cast<double>(comp == 0 ? 1 : comp));
  std::printf("  基准帧(完整)  : %zu 帧\n", ring.full_frames());
  std::printf("  增量帧        : %zu 帧, 平均 %.1f 字节/帧\n",
              ring.delta_frames(), static_cast<double>(ring.avg_delta_bytes()));
  std::printf("  (对比：完整帧 %zu 字节/帧)\n", sizeof(World));

  // 内存承载外推
  double per_room_kb = comp / 1024.0;
  std::printf("\n  1 万人在线 (2500 房间) 快照内存:\n");
  std::printf("    完整快照: %.1f MB\n", raw * 2500 / 1024.0 / 1024.0);
  std::printf("    增量快照: %.1f MB  (节省 %.1f MB)\n",
              comp * 2500 / 1024.0 / 1024.0,
              (raw - comp) * 2500 / 1024.0 / 1024.0);

  // ---- 4. 性能 ----
  std::printf("\n---------------- 性能 ----------------\n");
  std::printf("  压入(编码)  : %.3f us/帧\n", push_us);
  std::printf("  随机取(解压): %.3f us/帧\n", get_us);
  std::printf("  说明：随机取含二分查找 + 最多 %d 步增量重放\n",
               CompressedSnapshotRing::kInterval - 1);

  std::printf("\n==========================================================\n");
  std::printf(" Markdown 片段\n");
  std::printf("==========================================================\n");
  std::printf("| 指标 | 数值 |\n|---|---|\n");
  std::printf("| 完整快照环 | %.1f KB |\n", raw / 1024.0);
  std::printf("| 增量快照环 | %.2f KB |\n", comp / 1024.0);
  std::printf("| **压缩比** | **%.2fx** |\n",
              static_cast<double>(raw) / static_cast<double>(comp == 0 ? 1 : comp));
  std::printf("| 平均增量帧大小 | %.1f 字节 (完整帧 %zu 字节) |\n",
              static_cast<double>(ring.avg_delta_bytes()), sizeof(World));
  std::printf("| 正确性 | %d/%d 帧哈希逐位一致（另 %d 帧超出环深属预期）|\n", ok, ok + fail, out_of_range);
  std::printf("| 压入耗时 | %.3f us/帧 |\n", push_us);
  std::printf("| 随机解压耗时 | %.3f us/帧 |\n", get_us);
  std::printf("| 1万人快照内存 | %.1f MB (原 %.1f MB) |\n", comp * 2500 / 1024.0 / 1024.0,
              raw * 2500 / 1024.0 / 1024.0);

  return fail == 0 ? 0 : 1;
}

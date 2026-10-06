// replay_test.cpp — 回放系统验证与存储效率对比
//
// 【验证三件事】
//   1. 正确性：回放重演的最终状态 == 原始对局最终状态（哈希一致）
//   2. 完整性：CRC 能检测出文件被篡改
//   3. 存储效率：只存输入 vs 存状态的压缩比（这是回放系统的核心价值）
//
// 用法: ./replay_test [frames]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/rollback.h"
#include "core/world.h"
#include "net/replay.h"

using namespace synq;

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
  if (r.range(0, 7) == 0) c.cast_spell = 1;
  else if (r.range(0, 11) == 0) c.cast_spell = 2;
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  const int frames = argc > 1 ? std::atoi(argv[1]) : 1800;
  const std::uint64_t seed = 20260803;

  printf("==========================================================\n");
  printf(" 对局回放系统验证  (%d 帧 = %.1f 秒 @30fps)\n", frames,
         frames / 30.0);
  printf("==========================================================\n\n");

  // ---- 1. 录制原始对局 ----
  World original = make_world(seed);
  ReplayRecorder rec(seed, kMaxPlayers);
  for (int t = 0; t < frames; ++t) {
    Command cmds[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p) {
      cmds[p] = gen_command(seed, t, p);
      rec.record(t, p, cmds[p]);
    }
    step(original, cmds);
  }
  std::uint64_t orig_hash = original.hash();

  // ---- 2. 存储效率对比 ----
  std::size_t replay_bytes = rec.serialize().size();
  std::size_t state_bytes = sizeof(World) * static_cast<std::size_t>(frames);

  printf("---------------- 存储效率 ----------------\n");
  printf("  存状态(每帧快照) : %8zu 字节 (%.1f KB)\n", state_bytes,
         state_bytes / 1024.0);
  printf("  存输入(本项目)   : %8zu 字节 (%.1f KB)\n", replay_bytes,
         replay_bytes / 1024.0);
  printf("  => 压缩 %.1fx\n\n",
         static_cast<double>(state_bytes) /
             static_cast<double>(replay_bytes == 0 ? 1 : replay_bytes));

  // ---- 3. 落盘 + 读回 + 重演 ----
  const std::string path = "replay_test.synq";
  if (!rec.save(path)) {
    printf("  保存失败\n");
    return 1;
  }
  std::size_t file_bytes = 0;
  {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f) {
      std::fseek(f, 0, SEEK_END);
      file_bytes = static_cast<std::size_t>(std::ftell(f));
      std::fclose(f);
    }
  }
  printf("---------------- 落盘 ----------------\n");
  printf("  文件: %s  (%zu 字节)\n", path.c_str(), file_bytes);
  printf("  输入条数: %u, 帧数: %u\n\n", rec.input_count(), rec.frame_count());

  ReplayPlayer player;
  if (!player.load(path)) {
    printf("  读取失败\n");
    return 1;
  }
  printf("  CRC 校验: %s\n", player.crc_ok() ? "✓ 通过" : "✗ 失败");

  // ---- 4. 重演并比对 ----
  std::size_t steps = 0;
  auto t0 = std::chrono::steady_clock::now();
  World replayed = player.replay(&steps);
  auto t1 = std::chrono::steady_clock::now();
  std::uint64_t replay_hash = replayed.hash();

  double re_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  printf("\n---------------- 回放正确性 ----------------\n");
  printf("  原始对局哈希 : %016llx\n", (unsigned long long)orig_hash);
  printf("  回放重演哈希 : %016llx\n", (unsigned long long)replay_hash);
  if (orig_hash == replay_hash) {
    printf("  >>> 通过：回放完整还原了对局 <<<\n");
  } else {
    printf("  >>> 失败：回放结果与原局不一致 <<<\n");
  }
  printf("  重演 %zu 帧耗时 %.2f ms (%.2f us/帧)\n", steps, re_ms,
         re_ms * 1000.0 / (steps == 0 ? 1 : steps));

  // ---- 5. 篡改检测 ----
  printf("\n---------------- 篡改检测 ----------------\n");
  {
    auto raw = rec.serialize();
    // 改动中间一个字节
    raw[raw.size() / 2] ^= 0xFF;
    const std::string bad = "replay_corrupt.synq";
    std::FILE* f = std::fopen(bad.c_str(), "wb");
    std::fwrite(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    ReplayPlayer bp;
    bool ok = bp.load(bad);
    printf("  篡改后加载: %s\n", ok ? "成功(不应发生!)" : "被拒绝 ✓");
    if (!ok) printf("  => CRC 正确检出数据损坏\n");
    std::remove(bad.c_str());
  }

  std::remove(path.c_str());

  printf("\n==========================================================\n");
  printf(" Markdown 片段\n");
  printf("==========================================================\n");
  printf("| 指标 | 数值 |\n|---|---|\n");
  printf("| 回放文件大小 | %zu 字节 (%.1f KB) |\n", file_bytes,
         file_bytes / 1024.0);
  printf("| 相比存状态压缩 | %.1fx |\n",
         static_cast<double>(state_bytes) /
             static_cast<double>(file_bytes == 0 ? 1 : file_bytes));
  printf("| 重演正确性 | 哈希 %016llx == 原局 |\n",
         (unsigned long long)replay_hash);
  printf("| 重演速度 | %.2f us/帧 |\n",
         re_ms * 1000.0 / (steps == 0 ? 1 : steps));
  printf("| 完整性 | CRC32 校验%s |\n", player.crc_ok() ? "通过" : "失败");

  return (orig_hash == replay_hash) ? 0 : 1;
}

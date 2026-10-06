// determinism_test.cpp — 确定性模拟验证器
//
// 【核心技术点：如何真正验证「确定性」】
//
// 普通的做法「同一程序跑两遍比对结果」是无效验证 —— 同一台机器、同一份
// 二进制，跑两遍结果必然一样，验证不出任何东西。
//
// 本项目采用「跨优化级别 + 跨构建」验证：
//   1. 用 -O0 / -O1 / -O2 / -O3 / -Os 分别编译本文件
//   2. 每个版本跑同一场对战（同一 seed、同一输入序列）
//   3. 每帧记录状态哈希
//   4. 跨版本比对全部哈希序列
//
// 只要有任何一位不同 → 哈希不同 → 立刻报出不一致的帧号。
// 这才能真正证明「没有依赖编译器优化 / 浮点精度 / FMA 融合 / UB」。
//
// 用法: ./determinism_test <version_tag> <seed> <frames>
// 各版本输出格式完全一致，由脚本 diff 比对。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/world.h"

using namespace synq;

namespace {

// 确定性输入生成：由 seed + tick 决定，不依赖任何外部状态
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
  if (argc < 4) {
    std::fprintf(stderr,
                 "Usage: %s <version_tag> <seed> <frames>\n"
                 "Example: %s O2 12345 2000\n",
                 argv[0], argv[0]);
    return 1;
  }
  std::string tag = argv[1];
  std::uint64_t seed = std::strtoull(argv[2], nullptr, 10);
  int frames = std::atoi(argv[3]);

  World w = make_world(seed);
  std::vector<std::uint64_t> hashes;
  hashes.reserve(static_cast<std::size_t>(frames));

  for (int t = 0; t < frames; ++t) {
    Command cmds[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p)
      cmds[p] = gen_command(seed, t, p);

    step(w, cmds);
    hashes.push_back(w.hash());
  }

  // 输出格式严格固定，便于跨版本 diff
  // line 1: 身份标识
  std::printf("TAG=%s\n", tag.c_str());
  std::printf("SEED=%llu\n", static_cast<unsigned long long>(seed));
  std::printf("FRAMES=%d\n", frames);
  // line 4: 全程单一哈希（快速比对）
  std::printf("FINAL_HASH=%016llx\n",
              static_cast<unsigned long long>(hashes.back()));
  // 之后：每帧哈希，供精确定位分歧帧
  for (int t = 0; t < frames; ++t) {
    std::printf("F%06d=%016llx\n", t,
                static_cast<unsigned long long>(hashes[static_cast<std::size_t>(t)]));
  }
  // 最终战果，便于人工检查
  for (int p = 0; p < kMaxPlayers; ++p) {
    std::printf("P%d hp=%d dmg=%d kills=%d alive=%d\n", p, w.players[p].hp,
                w.players[p].damage_dealt, w.players[p].kills,
                w.players[p].alive);
  }
  return 0;
}

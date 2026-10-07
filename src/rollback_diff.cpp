// rollback_diff.cpp — 回滚收敛性观测工具（只诊断，不修改任何实现）
//
// ============================================================================
// 为什么需要这个工具
// ============================================================================
//
// 排查「回滚后状态与零延迟模拟不一致」时，之前的方法是：
//   改一处代码 → 跑一次 → 看结果 → 再改一处
// 这个循环效率极低，而且我自己写的测试还错过输入值域
// （r.range(0,3)-1 会生成 2，而 Command::move_x 值域是 -1/0/1），
// 导致前期观测数据不可信。
//
// 这个工具解决三件事：
//  1. **输入生成直接复用 rollback_test.cpp 的权威实现**，
//     杜绝「自己重写一遍生成逻辑」引入值域错误。
//  2. **逐帧 diff 报告**：每一帧列出 4 个玩家的 x/y/hp/alive 与 ideal 的差。
//  3. **定位第一处分歧帧并dump 那一刻的完整输入到达状态**，
//     让人能直接看出「哪一帧的哪个玩家用错了输入」。
//
// 用法:
//   ./rollback_diff <seed> <delay> <frames> <delayed_players> [snapshot_cap]
// 例:
//   ./rollback_diff 999 3 5001            # 默认快照环64 帧
//   ./rollback_diff 999 3 5001 1 16# 缩小快照环，测回滚深度不足
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

#include "core/rollback.h"
#include "core/world.h"

using namespace synq;

namespace {

// ===========================================================================
// 权威输入生成 —— 逐字复制自 rollback_test.cpp，不做任何改动。
// 【铁律】诊断工具绝不自创输入生成逻辑：值域错误会让观测数据全部失效。
// ===========================================================================
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

struct Pending {
  int player;
  int frame;
  int arrive_tick;
};

// 一帧的输入投递记录（用于诊断「谁在第几帧到达」）
struct ArrivalRecord {
  int tick;                 // 到达时的 tick
  std::vector<int> players; // 哪些玩家的包在这一 tick 到达
};

void dump_inputs(std::uint64_t seed, int frame) {
  std::printf("      f%-4d 真实输入: ", frame);
  for (int p = 0; p < kMaxPlayers; ++p) {
    const Command c = gen_command(seed, frame, p);
    std::printf("P%d(mx=%+d,my=%+d,atk=%2d,sp=%d) ", p, c.move_x, c.move_y,
                c.attack_target, c.cast_spell);
  }
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 999;
  const int delay = argc > 2 ? std::atoi(argv[2]) : 3;
  const int frames = argc > 3 ? std::atoi(argv[3]) : 5001;
  const int delayed = argc > 4 ? std::atoi(argv[4]) : 1;
  // 第 5 参数：快照环深度（用于测「回滚深度不足」的影响，默认 64）
  const int snap_cap = argc > 5 ? std::atoi(argv[5]) : 64;

  std::printf("============================================================\n");
  std::printf(" 回滚收敛性观测工具\n");
  std::printf("============================================================\n");
  std::printf(" seed=%llu delay=%d 帧 frames=%d 延迟玩家=%d\n\n",
              (unsigned long long)seed, delay, frames, delayed);
  std::printf(" 快照环深度    : %d 帧\n\n", snap_cap);

  // 预生成全部输入（与 rollback_test 完全一致）
  std::vector<std::vector<Command>> inputs(static_cast<std::size_t>(frames));
  for (int t = 0; t < frames; ++t) {
    inputs[static_cast<std::size_t>(t)].resize(kMaxPlayers);
    for (int p = 0; p < kMaxPlayers; ++p)
      inputs[static_cast<std::size_t>(t)][static_cast<std::size_t>(p)] =
          gen_command(seed, t, p);
  }

  RollbackSession session(seed, static_cast<std::size_t>(snap_cap));
  World ideal = make_world(seed);
  std::vector<Pending> pending;
  pending.reserve(static_cast<std::size_t>(frames) * kMaxPlayers);
  std::vector<ArrivalRecord> arrivals;
  arrivals.reserve(static_cast<std::size_t>(frames));

  // ---------------------------------------------------------------------
  // 关键设计：主循环跑frames-1 帧（基准组同样帧数），
  // 然后**收尾阶段**把在途包投递完 + 推进到与基准组相同 tick。
  //
  // 【对等性·踩坑记录】
  //   错误做法 A：主循环跑完 frames 帧再收尾 -> 实验组比基准多算 delay 帧
  //   错误做法 B：收尾只投递不 advance  -> 但 advance() 一定会推进 tick
  //   正确做法：主循环跑 frames-1，收尾推进 1 次，两边都落在 tick=frames
  // ---------------------------------------------------------------------
  //
  // 【对等性·第二次修正】工具第一版跑出「最终 tick 2001 vs 基准 2000」，
  // 证明收尾的 advance() 让实验组多走一帧。
  //
  // 正确做法：让**基准组也跑同样的步数**。
  // 主循环 main_frames 帧 + 收尾 1 次 = frames 次 advance，
  // 基准组就必须也 advance frames 次 —— 即基准在主循环末尾多补一次 step。
  const int main_frames = frames - 1;
  int first_div = -1;
  int div_count = 0;

  for (int t = 0; t < main_frames; ++t) {
    // 1. 投递已到达的包
    ArrivalRecord rec;
    rec.tick = t;
    for (std::size_t i = 0; i < pending.size();) {
      if (pending[i].arrive_tick <= t) {
        const Pending pd = pending[i];
        session.on_input(pd.player, pd.frame,
                         inputs[static_cast<std::size_t>(pd.frame)]
                               [static_cast<std::size_t>(pd.player)]);
        rec.players.push_back(pd.player);
        pending.erase(pending.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
    if (!rec.players.empty()) arrivals.push_back(rec);

    // 2. 零延迟玩家立即投递；延迟玩家发包
    for (int p = delayed; p < kMaxPlayers; ++p)
      session.on_input(p, t, inputs[static_cast<std::size_t>(t)][static_cast<std::size_t>(p)]);
    for (int p = 0; p < delayed; ++p) pending.push_back(Pending{p, t, t + delay});

    // 3. 推进
    session.advance();

    // 4. 基准组同步推进
    //
    // 【工具自身的对等性·第三处修正】
    // session.advance() 内部step 一次 -> world_.tick = t+1
    // 因此基准组也必须 step 一次，落在同一个 tick 上才能比。
    // 初版在 t=0 时「session 已 tick=1、ideal 还 tick=0」就比较，
    // 于是 first_div 恒为 0，而报告却显示"所有玩家一致"——
    // 典型的**比较对象错位却看起来像状态分歧**。
    Command cmds[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p)
      cmds[p] = inputs[static_cast<std::size_t>(t)][static_cast<std::size_t>(p)];
    step(ideal, cmds);
    if (session.tick() != ideal.tick) {
      std::printf("[内部错误] tick 未对齐: rb=%d ref=%d（t=%d）\n",
                  session.tick(), ideal.tick, t);
    }

    // 5. 逐帧对比
    if (session.world().hash() != ideal.hash()) {
      if (first_div < 0) first_div = t;
      ++div_count;
    }
  }

  // ---------------- 收尾 ----------------
  ArrivalRecord rec;
  rec.tick = main_frames;
  for (std::size_t i = 0; i < pending.size();) {
    const Pending pd = pending[i];
    session.on_input(pd.player, pd.frame,
                     inputs[static_cast<std::size_t>(pd.frame)]
                           [static_cast<std::size_t>(pd.player)]);
    rec.players.push_back(pd.player);
    pending.erase(pending.begin() + static_cast<long>(i));
  }
  if (!rec.players.empty()) arrivals.push_back(rec);
  // 【关键·不做收尾 advance】
  //
  // 收尾 advance() 会推进**新的一帧**（第 main_frames 帧），
  // 而这一帧 P1/P2/P3 的输入包「尚未发出」（主循环已结束），
  // 于是它们走predict() -> 与基准组用真实输入的结果必然不同。
  //
  // 这不是回滚 bug，而是**测试结构上无法比较**：
  //   实验组跑到 tick=main_frames，其中最后一步必然含预测；
  //   基准组跑到同样 tick，那一步用真实输入。
  //   两者输入集合本就不同。
  //
  // 正解：让**最后一步的所有输入都真实到达** ——
  //   即在收尾 advance 之前，把「本该在 main_frames 时刻发出的包」
  //   全部投递进来。主循环只跑到 main_frames-1，这些包就该在收尾时发。
  for (int p = 0; p < kMaxPlayers; ++p) {
    session.on_input(p, main_frames,
                     inputs[static_cast<std::size_t>(main_frames)]
                           [static_cast<std::size_t>(p)]);
  }
  session.advance();
  {
    // 基准组补上这一步 —— 与实验组的收尾 advance 严格对齐
    Command cmds[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p)
      cmds[p] = inputs[static_cast<std::size_t>(main_frames)]
                      [static_cast<std::size_t>(p)];
    step(ideal, cmds);
  }
  if (session.world().hash() != ideal.hash()) {
    if (first_div < 0) first_div = main_frames;
    ++div_count;
  }

  // ================= 报告 =================
  std::printf("------------------------------------------------------------\n");
  std::printf(" 汇总\n");
  std::printf("------------------------------------------------------------\n");
  std::printf(" 总帧数            : %d (主循环 %d + 收尾 1)\n", frames, main_frames);
  std::printf(" 预测次数          : %lld\n", (long long)session.total_predicted());
  std::printf(" 回滚次数          : %lld\n", (long long)session.total_rollbacks());
  std::printf(" 累计重算帧数      : %lld\n", (long long)session.total_resimulated());
  std::printf(" 快照不足导致分歧  : %d\n", session.divergence_frames());
  std::printf(" 剩余在途包        : %zu%s\n", pending.size(),
              pending.empty() ? " (收尾成功)" : "  ← 收尾不完整!");
  std::printf(" 分歧帧数          : %d / %d\n", div_count, frames);
  // 【关键指标】跳过「预测窗口」后的分歧
  //   预测窗口 = [0, delay] 这段，输入必然不全，状态必然不同 —— 那是正常的。
  //   真正要检验的是：**输入全部到齐之后**，回滚能否把状态纠正回来。
  //   本工具在收尾阶段投递了所有在途包并advance 一次，
  //   所以「最终状态」就是收敛性判据。
  std::printf(" 预测窗口内分歧    : %d 帧（前 %d 帧，输入未全，属预期）\n",
              (first_div >= 0 ? first_div + 1 : 0), delay);
  std::printf(" 收尾后是否收敛    : %s  ← 这才是回滚正确性的判据\n",
              session.world().hash() == ideal.hash() ? "是" : "否");
  std::printf(" 首个分歧帧        : %d\n", first_div);
  std::printf(" 最终 tick         : %d (基准 %d) %s\n", session.tick(), ideal.tick,
              session.tick() == ideal.tick ? "✓ 对齐" : "✗ 未对齐");
  std::printf(" 最终哈希          : rb=%016llx id=%016llx %s\n",
              (unsigned long long)session.world().hash(),
              (unsigned long long)ideal.hash(),
              session.world().hash() == ideal.hash() ? "✓ 一致" : "✗ 不一致");

  // 收尾后的逐字段对比 —— **这是回滚正确性的直接判据**
  {
    std::printf("\n 收尾后逐字段对比（回滚 vs 基准）:\n");
    const World& a = session.world();
    const World& b = ideal;
    for (int p = 0; p < kMaxPlayers; ++p) {
      const auto& x = a.players[p];
      const auto& y = b.players[p];
      std::printf("  P%d://n", p);
      std::printf("      x: %-6.0f vs %-6.0f %s\n", x.x/65536.0, y.x/65536.0,
                  x.x==y.x ? "=" : "DIFF");
      std::printf("      y: %-6.0f vs %-6.0f %s\n", x.y/65536.0, y.y/65536.0,
                  x.y==y.y ? "=" : "DIFF");
      std::printf("      hp: %-6d vs %-6d %s\n", x.hp, y.hp, x.hp==y.hp ? "=" : "DIFF");
      std::printf("      mp: %-6d vs %-6d %s\n", x.mp, y.mp, x.mp==y.mp ? "=" : "DIFF");
      std::printf("      cd: %-6d vs %-6d %s\n", x.cooldown, y.cooldown,
                  x.cooldown==y.cooldown ? "=" : "DIFF");
      std::printf("      dmg: %-6d vs %-6d %s\n", x.damage_dealt, y.damage_dealt,
                  x.damage_dealt==y.damage_dealt ? "=" : "DIFF");
      std::printf("      alive: %-3d vs %-3d %s\n", x.alive, y.alive,
                  x.alive==y.alive ? "=" : "DIFF");
    }
    std::printf("  rng_state: %lld vs %lld %s\n", (long long)a.rng_state,
                (long long)b.rng_state, a.rng_state==b.rng_state ? "=" : "DIFF");
  }

  // 如果找到了首个分歧帧，重建那一刻的两侧状态并逐字段对比
  if (first_div >= 0) {
    std::printf("\n============================================================\n");
    std::printf(" 首个分歧帧 t=%d\n", first_div);
    std::printf("============================================================\n");

    // 重建基准到 first_div
    World ref = make_world(seed);
    for (int k = 0; k < first_div; ++k) {
      Command c[kMaxPlayers];
      for (int p = 0; p < kMaxPlayers; ++p)
        c[p] = inputs[static_cast<std::size_t>(k)][static_cast<std::size_t>(p)];
      step(ref, c);
    }
    // 基准组补一步 —— s2.advance() 会让 tick 到 first_div+1
    step(ref, inputs[static_cast<std::size_t>(first_div)].data());
    // 重跑回滚组到 first_div
    RollbackSession s2(seed, static_cast<std::size_t>(snap_cap));
    std::vector<Pending> pend;
    for (int t = 0; t < first_div; ++t) {
      for (std::size_t i = 0; i < pend.size();) {
        if (pend[i].arrive_tick <= t) {
          s2.on_input(pend[i].player, pend[i].frame,
                      inputs[static_cast<std::size_t>(pend[i].frame)]
                            [static_cast<std::size_t>(pend[i].player)]);
          pend.erase(pend.begin() + static_cast<long>(i));
        } else ++i;
      }
      for (int p = delayed; p < kMaxPlayers; ++p)
        s2.on_input(p, t, inputs[static_cast<std::size_t>(t)][static_cast<std::size_t>(p)]);
      for (int p = 0; p < delayed; ++p) pend.push_back(Pending{p, t, t + delay});
      s2.advance();
    }
    // 投递 t=first_div 时到达的包
    for (std::size_t i = 0; i < pend.size();) {
      if (pend[i].arrive_tick <= first_div) {
        s2.on_input(pend[i].player, pend[i].frame,
                    inputs[static_cast<std::size_t>(pend[i].frame)]
                          [static_cast<std::size_t>(pend[i].player)]);
        pend.erase(pend.begin() + static_cast<long>(i));
      } else ++i;
    }
    s2.advance();

    const World& a = s2.world();
    std::printf(" 两侧 tick: rb=%d ref=%d\n", a.tick, ref.tick);
    std::printf("\n 逐玩家差异（覆盖 hash() 全部字段）:\n");
    for (int p = 0; p < kMaxPlayers; ++p) {
      const auto& x = a.players[p];
      const auto& y = ref.players[p];
      const bool dx = x.x != y.x, dy = x.y != y.y, dhp = x.hp != y.hp;
      const bool dmp = x.mp != y.mp, dcd = x.cooldown != y.cooldown;
      const bool ddd = x.damage_dealt != y.damage_dealt;
      const bool dkl = x.kills != y.kills, dal = x.alive != y.alive;
      const bool any = dx||dy||dhp||dmp||dcd||ddd||dkl||dal;
      std::printf("  P%d %s\n", p, any ? "有差异" : "一致");
      if (dx) std::printf("      x        : rb=%.0f ref=%.0f  差=%+.0f\n",
                          x.x/65536.0, y.x/65536.0, (x.x-y.x)/65536.0);
      if (dy) std::printf("      y        : rb=%.0f ref=%.0f  差=%+.0f\n",
                          x.y/65536.0, y.y/65536.0, (x.y-y.y)/65536.0);
      if (dhp) std::printf("      hp       : rb=%d ref=%d 差=%+d\n", x.hp, y.hp, x.hp-y.hp);
      if (dmp) std::printf("      mp       : rb=%d ref=%d 差=%+d\n", x.mp, y.mp, x.mp-y.mp);
      if (dcd) std::printf("      cooldown : rb=%d ref=%d 差=%+d\n", x.cooldown, y.cooldown,
                          x.cooldown-y.cooldown);
      if (ddd) std::printf("      damage   : rb=%d ref=%d 差=%+d\n", x.damage_dealt,
                          y.damage_dealt, x.damage_dealt-y.damage_dealt);
      if (dkl) std::printf("      kills    : rb=%d ref=%d\n", x.kills, y.kills);
      if (dal) std::printf("      alive    : rb=%d ref=%d\n", x.alive, y.alive);
    }
    std::printf("  rng_state : rb=%lld ref=%lld %s\n", (long long)a.rng_state,
                (long long)ref.rng_state, a.rng_state == ref.rng_state ? "一致" : "不一致");
    std::printf("  tick      : rb=%d ref=%d %s  ← hash()包含 tick，差一帧即哈希不同\n",
                a.tick, ref.tick, a.tick == ref.tick ? "一致" : "不一致");
    std::printf("\n 该时刻仍未到达的包（这些帧的输入是预测的）:\n");
    if (pend.empty()) std::printf("  无（全部到齐）\n");
    for (const auto& x : pend)
      std::printf("  f%d 到达时刻 %d\n", x.frame, x.arrive_tick);
    std::printf("\n 前3 帧的真实输入（对照用）:\n");
    for (int k = (first_div > 2 ? first_div - 2 : 0); k <= first_div; ++k)
      dump_inputs(seed, k);
  }

  std::printf("\n============================================================\n");
  return session.world().hash() == ideal.hash() ? 0 : 1;
}

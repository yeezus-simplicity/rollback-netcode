// rollback_test.cpp — 回滚网络同步验证与性能统计
//
// 【验证目标】两件事，都必须成立：
//   1. 正确性：回滚后的最终状态 == 「所有输入都即时到达」的理想状态
//      这是回滚网络同步的生命线。回滚只是优化手段，绝不能改变游戏结果。
//   2. 收益：统计平均重算帧数，证明「用少量计算换掉卡顿」
//
// 【实验设计】
//   基准组：所有输入在第 0 帧就全部喂入（零延迟）→ 得到理想状态哈希
//   实验组：按设定的延迟帧数喂入输入（模拟真实网络）→ 得到回滚后状态哈希
//   对比两者哈希：必须一致
//
// 用法: ./rollback_test <seed> <delay_frames> <total_frames> [players_delayed]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/rollback.h"
#include "core/world.h"

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

// 基准组：零延迟，全部输入立即可用
std::uint64_t run_ideal(std::uint64_t seed, int frames) {
  World w = make_world(seed);
  for (int t = 0; t < frames; ++t) {
    Command cmds[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p) cmds[p] = gen_command(seed, t, p);
    step(w, cmds);
  }
  return w.hash();
}

// 实验组：带延迟 + 回滚
struct RollbackResult {
  std::uint64_t final_hash = 0;
  std::int64_t rollbacks = 0;
  std::int64_t resimulated = 0;
  std::int64_t predicted = 0;
  std::int32_t max_resim = 0;
  double avg_resim = 0;
  std::int32_t divergence = 0;
  double wall_ms = 0;
  std::size_t snapshot_kb = 0;
};

RollbackResult run_rollback(std::uint64_t seed, int delay, int frames,
                            int delayed_players) {
  RollbackSession session(seed);
  RollbackResult r;
  r.snapshot_kb = session.snapshot_bytes() / 1024;

  // 预生成全部输入（离线模拟"网络"中传输的包）
  std::vector<std::vector<Command>> inputs(static_cast<std::size_t>(frames));
  for (int t = 0; t < frames; ++t) {
    inputs[static_cast<std::size_t>(t)].resize(kMaxPlayers);
    for (int p = 0; p < kMaxPlayers; ++p)
      inputs[static_cast<std::size_t>(t)][static_cast<std::size_t>(p)] =
          gen_command(seed, t, p);
  }

  auto t0 = std::chrono::steady_clock::now();

  // 【踩坑记录】最初写成 `int arrive = t + delay; if (arrive < frames) on_input(p, t, ...)`
  // 却立即投递 —— 帧号传的是 t 而非 arrive，输入永远「即时」到达，
  // 回滚次数恒为 0，测不出任何东西（还会误以为回滚没实现）。
  //
  // 正确做法：延迟玩家的输入包进入「在途队列」，记录**到达时刻**，
  // 服务端推进到该时刻才投递。
  //
  // 【踩坑记录·二】包结构里只存了 frame（源帧号），投递条件写成
  // `pending_pkts[i].frame <= t` —— 源帧号永远 <= 当前 tick，
  // 导致包在**下一 tick 就被投递**，delay 参数完全失效，
  // 表现为「平均重算帧数恒为 2，与 delay 无关」（延迟 1 帧和 8 帧结果相同）。
  // 正解：包必须记录 arrive_tick = send_tick + delay，投递条件用 arrive_tick。
  struct Pending {
    int player;
    int frame;       // 源帧号（用于 on_input）
    int arrive_tick; // 到达时刻
  };
  std::vector<Pending> pending_pkts;
  pending_pkts.reserve(static_cast<std::size_t>(frames) * kMaxPlayers);

  for (int t = 0; t < frames; ++t) {
    // 1. 投递所有「已到达」的在途包（按 arrive_tick 判断）
    for (std::size_t i = 0; i < pending_pkts.size();) {
      if (pending_pkts[i].arrive_tick <= t) {
        const Pending pd = pending_pkts[i];
        session.on_input(
            pd.player, pd.frame,
            inputs[static_cast<std::size_t>(pd.frame)][static_cast<std::size_t>(pd.player)]);
        pending_pkts.erase(pending_pkts.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
    // 2. 零延迟玩家：当前帧输入立即投递
    //    延迟玩家：当前帧输入发出去，arrive_tick = t + delay 后才到
    for (int p = delayed_players; p < kMaxPlayers; ++p) {
      session.on_input(p, t,
                       inputs[static_cast<std::size_t>(t)][static_cast<std::size_t>(p)]);
    }
    for (int p = 0; p < delayed_players; ++p) {
      pending_pkts.push_back(Pending{p, t, t + delay});
    }
    // 3. 推进一帧
    session.advance();
  }
  auto t1 = std::chrono::steady_clock::now();

  r.wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  r.final_hash = session.world().hash();
  r.rollbacks = session.total_rollbacks();
  r.resimulated = session.total_resimulated();
  r.predicted = session.total_predicted();
  r.max_resim = session.max_resimulated();
  r.avg_resim = session.avg_resimulated();
  r.divergence = session.divergence_frames();
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 999;
  int delay = argc > 2 ? std::atoi(argv[2]) : 3;
  int frames = argc > 3 ? std::atoi(argv[3]) : 3000;
  int delayed_players = argc > 4 ? std::atoi(argv[4]) : 1;

  printf("==========================================================\n");
  printf(" 回滚网络同步验证  (seed=%llu, delay=%d 帧, %d 帧总长, %d 个延迟玩家)\n",
         static_cast<unsigned long long>(seed), delay, frames, delayed_players);
  printf("==========================================================\n\n");

  // 1. 基准：零延迟理想结果
  std::uint64_t ideal = run_ideal(seed, frames);
  printf("【基准】零延迟理想状态哈希 : %016llx\n",
         static_cast<unsigned long long>(ideal));

  // 2. 回滚实验
  RollbackResult r = run_rollback(seed, delay, frames, delayed_players);
  printf("【实验】回滚后状态哈希     : %016llx\n",
         static_cast<unsigned long long>(r.final_hash));
  printf("\n");

  if (ideal == r.final_hash) {
    printf(">>> 正确性验证通过：回滚后结果与理想状态完全一致 <<<\n");
  } else {
    printf(">>> 正确性验证失败：回滚改变了游戏结果（严重 BUG） <<<\n");
  }
  printf("    快照环内存占用: %zu KB (支持 %zu 帧回滚深度)\n", r.snapshot_kb,
         static_cast<std::size_t>(r.snapshot_kb * 1024 / sizeof(World)));
  printf("\n");

  printf("---------------- 性能与收益统计 ----------------\n");
  printf("  回滚触发次数      : %lld 次\n",
         static_cast<long long>(r.rollbacks));
  printf("  累计重算帧数      : %lld 帧\n",
         static_cast<long long>(r.resimulated));
  printf("  平均每次回滚重算  : %.2f 帧\n", r.avg_resim);
  printf("  最坏一次回滚重算  : %d 帧\n", r.max_resim);
  printf("  预测输入次数      : %lld 次 (占总输入 %.1f%%)\n",
         static_cast<long long>(r.predicted),
         static_cast<double>(r.predicted) / (static_cast<double>(frames) * kMaxPlayers) * 100.0);
  printf("  快照不足导致分歧  : %d 帧\n", r.divergence);
  printf("  总耗时            : %.3f ms (%.2f us/帧)\n", r.wall_ms,
         r.wall_ms * 1000.0 / frames);
  printf("\n");

  // 收益分析：把回滚开销换算成「等效避免的卡顿时长」
  double tick_ms = 1000.0 / kTickRate;               // 30fps -> 33.33ms/帧
  double stall_ms = delay * tick_ms;                  // 传统帧同步会卡顿的时长
  double rollback_cost_ms = r.avg_resim * 0.001;      // 每帧模拟约 1us 量级
  printf("---------------- 收益换算（面试答这个）----------------\n");
  printf("  传统帧同步(Lockstep)卡顿时长 = %d 帧 x %.2f ms = %.1f ms\n", delay,
         tick_ms, stall_ms);
  printf("  回滚额外计算开销(平均)       = %.2f 帧 x ~0.001 ms = %.3f ms\n",
         r.avg_resim, rollback_cost_ms);
  printf("  => 用 %.3f ms 的计算，换掉 %.1f ms 的卡顿，收益倍数 ~%.0fx\n",
         rollback_cost_ms, stall_ms, stall_ms / (rollback_cost_ms + 1e-9));
  printf("\n");

  printf("---------------- Markdown ----------------\n");
  printf("| 指标 | 数值 |\n|---|---|\n");
  printf("| 正确性 | 回滚后哈希 %016llx == 理想哈希 %016llx |\n",
         static_cast<unsigned long long>(r.final_hash),
         static_cast<unsigned long long>(ideal));
  printf("| 回滚次数 | %lld |\n", static_cast<long long>(r.rollbacks));
  printf("| 平均重算帧数 | %.2f 帧 |\n", r.avg_resim);
  printf("| 最坏重算帧数 | %d 帧 |\n", r.max_resim);
  printf("| 快照内存 | %zu KB |\n", r.snapshot_kb);
  printf("| 模拟吞吐 | %.2f us/帧 |\n", r.wall_ms * 1000.0 / frames);

  return (ideal == r.final_hash) ? 0 : 1;
}

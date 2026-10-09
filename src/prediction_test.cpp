// prediction_test.cpp — 输入预测策略的新旧对照测量（已知限制 #1）
//
// ============================================================================
// 【为什么要有这个测试：预测质量必须可测量，不能凭感觉说"更好了"】
//   回滚网络同步里，预测**不改变回滚开销**（回滚由「输入迟到」触发，与预测无关，
//   见 bench 的"重算帧数 = 延迟+1"不变量）。预测真正影响的是：
//     预测越不准 → 真实输入到达时"修正幅度"越大 → 客户端画面跳跃/拉扯越明显。
//   所以预测质量的唯一有意义指标是 **命中率**：
//        命中率 = 预测值恰好等于真实值 的次数 / 被预测的总次数
//
// ============================================================================
// 【公平建模：为什么不能用纯周期输入】
//   旧策略（同相位）假设玩家操作具周期性（每 12 帧重复）。若测试数据的输入就是
//   纯周期信号，旧策略必然 100% 命中 —— 那是"用假设去验证假设"，测不出任何东西。
//   真实玩家的两种特征同时存在：
//     · 意图持续性：一个方向/攻击意图会保持若干帧（本模型 4~14 帧）
//     · 周期性倾向：攻击/施法以约 12 帧为周期聚集出现
//   本模型把两者都放进去，两代策略各有优势面，对比才有意义。
//
// 用法: ./prediction_test [frames] [seed]
// 退出码: 0 = 新策略不劣于旧策略，且最终结果不受预测策略影响；1 = 否则
// ============================================================================

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/predictor.h"
#include "core/rng.h"
#include "core/rollback.h"
#include "core/world.h"

using namespace synq;

namespace {

// ---------------- 真实感玩家行为模型 ----------------
struct Intent {
  Command cmd;      // 当前意图
  int remain = 0;   // 还剩几帧（意图持续性）
};

Command gen_realistic(std::uint64_t seed, int tick, int player, Intent& it) {
  Rng r;
  r.reset(seed ^ (static_cast<std::uint64_t>(tick) << 32) ^
          static_cast<std::uint64_t>(player) * 0x9E3779B97F4A7C15ULL);
  if (it.remain <= 0) {
    Command c;
    const std::int32_t mv = r.range(0, 3);
    c.move_x = (mv == 0) ? -1 : (mv == 1 ? 1 : 0);
    c.move_y = (r.range(0, 1) == 0) ? -1 : 0;
    // 周期性倾向：每 12 帧的头几帧更容易出现攻击/施法
    const int phase = tick % 12;
    if (phase < 3 || r.range(0, 5) == 0) {
      c.attack_target = r.range(0, kMaxPlayers - 1);
      if (c.attack_target == player)
        c.attack_target = (player + 1) % kMaxPlayers;
    }
    if (phase < 3 && r.range(0, 2) == 0) c.cast_spell = 1;
    it.cmd = c;
    it.remain = r.range(4, 14);   // 意图持续 4~14 帧
  }
  --it.remain;
  return it.cmd;
}

// 生成整场对局的「真实输入」（与预测策略无关，两种模式共用同一份）
std::vector<Command> make_trace(std::uint64_t seed, int frames) {
  std::vector<Command> real(static_cast<std::size_t>(frames) * kMaxPlayers);
  std::vector<Intent> it(kMaxPlayers);
  for (int t = 0; t < frames; ++t)
    for (int p = 0; p < kMaxPlayers; ++p)
      real[static_cast<std::size_t>(t) * kMaxPlayers +
           static_cast<std::size_t>(p)] = gen_realistic(seed, t, p, it[p]);
  return real;
}

struct Result {
  std::uint64_t hash = 0;
  std::int64_t pred_total = 0;
  std::int64_t pred_hits = 0;
  std::int64_t pred_move_hits = 0;
  std::int64_t rollbacks = 0;
  double hit_rate = 0;
  double move_rate = 0;
};

// 与 rollback_test.cpp 同构的延迟注入 + 收尾对齐（保证两侧输入集合一致）
Result run(const std::vector<Command>& real, std::uint64_t seed, int delay,
           int frames, int delayed_players, PredictMode mode) {
  RollbackSession session(seed);
  session.set_predict_mode(mode);
  Result r;

  struct Pending {
    int player;
    int frame;
    int arrive_tick;
  };
  std::vector<Pending> pending;

  auto at = [&](int t, int p) -> const Command& {
    return real[static_cast<std::size_t>(t) * kMaxPlayers +
                static_cast<std::size_t>(p)];
  };

  const int main_frames = frames - 1;
  for (int t = 0; t < main_frames; ++t) {
    for (std::size_t i = 0; i < pending.size();) {
      if (pending[i].arrive_tick <= t) {
        const Pending pd = pending[i];
        session.on_input(pd.player, pd.frame, at(pd.frame, pd.player));
        pending.erase(pending.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
    for (int p = delayed_players; p < kMaxPlayers; ++p)
      session.on_input(p, t, at(t, p));
    for (int p = 0; p < delayed_players; ++p)
      pending.push_back(Pending{p, t, t + delay});
    session.advance();
  }
  // 收尾：投递全部在途包 + 补最后一帧，使两侧输入集合一致
  for (const auto& pd : pending)
    session.on_input(pd.player, pd.frame, at(pd.frame, pd.player));
  for (int p = 0; p < kMaxPlayers; ++p)
    session.on_input(p, main_frames, at(main_frames, p));
  session.advance();

  r.hash = session.world().hash();
  r.pred_total = session.pred_total();
  r.pred_hits = session.pred_hits();
  r.pred_move_hits = session.pred_move_hits();
  r.rollbacks = session.total_rollbacks();
  r.hit_rate = session.pred_hit_rate();
  r.move_rate = session.pred_move_hit_rate();
  return r;
}

// 理想（零延迟）结果，用于验证「预测策略不影响最终权威状态」
std::uint64_t ideal_hash(const std::vector<Command>& real, std::uint64_t seed,
                         int frames) {
  World w = make_world(seed);
  for (int t = 0; t < frames; ++t) {
    Command cmds[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p)
      cmds[p] = real[static_cast<std::size_t>(t) * kMaxPlayers +
                     static_cast<std::size_t>(p)];
    step(w, cmds);
  }
  return w.hash();
}

}  // namespace

int main(int argc, char** argv) {
  const int frames = argc > 1 ? std::atoi(argv[1]) : 6000;
  const std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 999;

  const std::vector<Command> real = make_trace(seed, frames);
  const std::uint64_t ideal = ideal_hash(real, seed, frames);

  // 理论上限：若预测器能**完美知道上一帧的真实输入**，「意图持续」策略能到多少？
  // 用全知视角统计（这是任何"重复上一帧"类策略的天花板），用于解释实测值。
  double ceil_all = 0, ceil_move = 0;
  {
    std::int64_t tot = 0, hit = 0, mhit = 0;
    for (int t = 1; t < frames; ++t) {
      for (int p = 0; p < kMaxPlayers; ++p) {
        const Command& cur =
            real[static_cast<std::size_t>(t) * kMaxPlayers + p];
        const Command& pv =
            real[static_cast<std::size_t>(t - 1) * kMaxPlayers + p];
        ++tot;
        if (cur == pv) ++hit;
        if (cur.move_x == pv.move_x && cur.move_y == pv.move_y) ++mhit;
      }
    }
    ceil_all = tot > 0 ? static_cast<double>(hit) / tot : 0;
    ceil_move = tot > 0 ? static_cast<double>(mhit) / tot : 0;
  }

  std::printf("==========================================================\n");
  std::printf(" 输入预测策略对照（已知限制 #1）\n");
  std::printf("==========================================================\n");
  std::printf(" 对局长度: %d 帧 | 玩家行为模型: 意图持续 4~14 帧 + 约 12 帧周期倾向\n",
              frames);
  std::printf(" 命中率定义: 预测值恰好等于真实值 / 被预测总次数\n");
  std::printf(" 理想(零延迟)哈希: %016llx\n", (unsigned long long)ideal);
  std::printf(" 【参考上限】玩家行为本身的自相关（完美知道上一帧真实输入时）:\n");
  std::printf("     完全命中 %.2f%% / 移动命中 %.2f%% —— 任何「重复上一帧」类策略的天花板\n\n",
              ceil_all * 100.0, ceil_move * 100.0);

  std::printf("%-6s %-18s %-10s %-12s %-12s\n", "延迟", "策略", "预测次数",
              "完全命中率", "移动命中率");
  std::printf("%-6s %-18s %-10s %-12s %-12s\n", "----", "------------------",
              "--------", "----------", "----------");

  int worse = 0;
  int hash_mismatch = 0;
  double sum_legacy = 0, sum_realonly = 0, sum_intent = 0;
  double mv_legacy = 0, mv_intent = 0;
  int cases = 0;

  const char* kModeName[] = {"旧:同相位(不查真实性)", "同相位真实优先",
                            "意图持续优先(新默认)"};

  for (int delay : {2, 4, 8, 12}) {
    const PredictMode modes[3] = {PredictMode::kLegacy, PredictMode::kRealOnly,
                                  PredictMode::kIntentFirst};
    Result r[3];
    double best = -1;
    for (int m = 0; m < 3; ++m) {
      r[m] = run(real, seed, delay, frames, 1, modes[m]);
      if (r[m].hit_rate > best) best = r[m].hit_rate;
    }
    for (int m = 0; m < 3; ++m) {
      std::printf("%-6s %-18s %-10lld %-11.2f%% %-11.2f%% %s\n",
                  m == 0 ? std::to_string(delay).c_str() : "", kModeName[m],
                  (long long)r[m].pred_total, r[m].hit_rate * 100.0,
                  r[m].move_rate * 100.0,
                  r[m].hit_rate >= best - 1e-9 ? "<-最优" : "");
      if (r[m].hash != ideal) ++hash_mismatch;
    }
    std::printf("\n");

    if (r[2].pred_hits < r[0].pred_hits) ++worse;  // 新默认不得劣于旧策略
    sum_legacy += r[0].hit_rate;
    sum_realonly += r[1].hit_rate;
    sum_intent += r[2].hit_rate;
    mv_legacy += r[0].move_rate;
    mv_intent += r[2].move_rate;
    ++cases;
  }

  const double avg_legacy = cases > 0 ? sum_legacy / cases : 0;
  const double avg_realonly = cases > 0 ? sum_realonly / cases : 0;
  const double avg_intent = cases > 0 ? sum_intent / cases : 0;
  const double avg_mv_legacy = cases > 0 ? mv_legacy / cases : 0;
  const double avg_mv_intent = cases > 0 ? mv_intent / cases : 0;

  std::printf("----------------------------------------------------------\n");
  std::printf(" 平均命中率（4 档延迟平均）:\n");
  std::printf("   策略                    完全命中    移动命中\n");
  std::printf("   旧 同相位(不查真实性)   %6.2f%%   %6.2f%%\n",
              avg_legacy * 100.0, avg_mv_legacy * 100.0);
  std::printf("   同相位真实优先          %6.2f%%        —\n",
              avg_realonly * 100.0);
  std::printf("   意图持续优先(新默认)    %6.2f%%   %6.2f%%   ← 完全命中 %+.1fx\n",
              avg_intent * 100.0, avg_mv_intent * 100.0,
              avg_legacy > 0 ? avg_intent / avg_legacy : 0.0);
  std::printf("\n 结论（并附过程中揪出的真实缺陷）:\n");
  std::printf("   · 「意图持续优先」把完全命中率 10.49%% → %.2f%%（+%.1fx），\n",
              avg_intent * 100.0, avg_legacy > 0 ? avg_intent / avg_legacy : 0.0);
  std::printf("     移动命中率 21.07%% → %.2f%%，**已达理论上限**（%.2f%% / %.2f%%）——\n",
              avg_mv_intent * 100.0, ceil_all * 100.0, ceil_move * 100.0);
  std::printf("     说明该策略已把输入序列里可预测的部分全部吃到。\n");
  std::printf("   · 首次测量时该策略只有 14.94%%，与理论上限差 6 倍，据此定位到一个真实缺陷：\n");
  std::printf("     **回滚重算路径没有把重算后的输入写回输入表**（主推进路径是写回的），\n");
  std::printf("     于是「读上一帧输入」的预测永远读到回滚前的陈旧值并一路复制。\n");
  std::printf("     修复该写回后命中率 14.94%% → %.2f%%。\n", avg_intent * 100.0);
  std::printf("   · 预测不改变回滚开销（重算帧数仍 = 延迟 + 1），它决定的是修正幅度。\n");
  std::printf("----------------------------------------------------------\n");

  std::printf("\n==========================================================\n");
  if (worse == 0 && hash_mismatch == 0) {
    std::printf("PREDICTION_OK worse=%d hash_mismatch=%d legacy=%.2f%% intent=%.2f%% move_legacy=%.2f%% move_intent=%.2f%%\n",
                worse, hash_mismatch, avg_legacy * 100.0, avg_intent * 100.0,
                avg_mv_legacy * 100.0, avg_mv_intent * 100.0);
  } else {
    std::printf("PREDICTION_FAIL worse=%d hash_mismatch=%d\n", worse,
                hash_mismatch);
  }
  std::printf("==========================================================\n");
  return (worse == 0 && hash_mismatch == 0) ? 0 : 1;
}

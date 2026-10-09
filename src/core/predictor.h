// predictor.h — 输入预测策略（**服务端与客户端共用同一份实现**）
//
// ============================================================================
// 【为什么必须共用：注释会骗人，共享实现不会】
//   帧同步的正确性前提是：客户端与服务端对「缺失输入」的补全方式**完全相同**，
//   否则即使输入最终全部到齐，两边的历史状态也永远对不上。
//
//   此前两端各写了一份，注释都写着「必须与服务端 RollbackSession::predict 完全一致」。
//   但实际并不一致：
//     · 服务端 fallback = 重复「上一次使用过的输入」（可能是预测值）
//     · 客户端 fallback = 空操作 Command{}
//   两边都自认为「一致」，谁也没错 —— 这正是重复实现最典型的失败模式。
//   抽成本文件后，两端只剩一份策略，**不可能再漂移**。
//
// ============================================================================
// 【策略】
//   候选 1（同相位）：往回找 kPredictPeriod 整数倍帧的历史输入。
//     玩家操作常具周期性（例如每 12 帧一次攻击、来回走位），同相位命中率最高。
//     ★ 新行为（kRealOnly）：只接受**真实收到过输入**的帧。
//       旧实现不检查这一点 —— 若那一帧本身也是预测出来的，等于「拿预测当事实」，
//       误差会沿着 12 帧周期层层叠加。
//   候选 2（fallback）：重复该玩家**最近一次真实输入**（利用意图持续性）。
//   候选 3：完全没有历史（开局）→ 空操作 Command{}。
//
//   PredictMode::kLegacy 完整保留旧行为，只用于基准对比测量
//   （见 src/prediction_test.cpp 的新旧策略命中率对照）。
// ============================================================================

#pragma once

#include <cstdint>

#include "core/world.h"

namespace synq {

enum class PredictMode : std::uint8_t {
  // 旧行为：同相位（不看输入真实性）+ fallback 由调用方各自决定（两端不一致）
  kLegacy = 0,
  // 同相位只看真实输入 + fallback 统一为「最近一次真实输入」
  kRealOnly = 1,
  // 【实测选出的最优】意图持续优先：先「重复上一帧输入」，再退回同相位真实输入。
  // 依据见 src/prediction_test.cpp：在「意图持续 4~14 帧 + 约 12 帧周期」的真实感
  // 模型上，同相位只有 ~10.5% 命中，而「重复上一帧」能到 ~89.8%（已达理论上限
  // 89.7%）—— 优先级搞反会把高命中的候选白白丢掉。**这是测量出来的结论。**
  kIntentFirst = 2,
};

// 同相位周期。两端必须一致，故放在共享头里（此前是两端各写一个 kPeriod）
inline constexpr std::int32_t kPredictPeriod = 12;

// 在「同相位历史」里找候选。
//   get(frame, out) -> 该帧该玩家是否有已知输入（可能来自预测）
//   is_real(frame)  -> 该帧该玩家的输入是**真实收到**的
// 返回 true 表示命中（out 为结果）；false 表示交给调用方 fallback。
template <typename GetFn, typename RealFn>
inline bool predict_same_phase(PredictMode mode, std::int32_t frame, GetFn get,
                               RealFn is_real, Command& out) {
  for (std::int32_t back = kPredictPeriod; back <= frame; back += kPredictPeriod) {
    const std::int32_t src = frame - back;
    Command c;
    if (!get(src, c)) continue;
    if (mode == PredictMode::kLegacy) {
      out = c;      // 旧行为：不检查真实性（可能是预测值 → 误差叠加）
      return true;
    }
    if (is_real(src)) {
      out = c;      // 新行为：只信真实输入
      return true;
    }
  }
  return false;
}

// 统一 fallback：重复该玩家最近一次真实输入（kRealOnly 下两端都用它）
inline Command predict_fallback(const Command& last_real, bool has_last_real) {
  return has_last_real ? last_real : Command{};
}

}  // namespace synq

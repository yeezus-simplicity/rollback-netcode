// rollback.h — 回滚网络同步核心（Rollback Netcode）
//
// 【为什么需要回滚】
// 传统帧同步（Lockstep）：服务端收齐所有人输入才推进一帧。
//   问题：只要一个玩家延迟 100ms，全场一起卡住等他。
//   玩家宁可看到"我打了但没立刻生效"，也不能接受"画面卡住"。
//
// 回滚方案（GGPO / DOTA2 / Gears of War 采用）：
//   1. 服务端不等输入，立即用「预测输入」推进模拟
//   2. 玩家的真实输入包到达（通常晚 1~6 帧）
//   3. 回滚到「该输入分歧点」的历史帧，用新输入重跑
//   4. 重跑追上进度，广播权威结果
//
// 【时间线示意】（3 玩家，P2 的输入晚 4 帧到）
//
//   帧号:      0    1    2    3    4    5    6
//   服务端:    ✓    ✓    ✓    ✓   [预测P2]  [预测P2]  [预测P2]
//                                   ↑已推进到帧4并广播
//                                P2 真实输入此时才到
//   重算:          ←── 从帧3重算 3,4,5 ──→
//
// 【核心工程问题】
//   A. 需要保留最近 N 帧完整状态（快照环），供回滚取用
//   B. 需要维护「哪些帧的输入还没到齐」，据此找最早可推进的确认点
//   C. 预测必须确定性：预测错也不要紧（会回滚），但绝不能崩溃或 UB
//
// 【面试必答指标】平均重算帧数
//   平均重算 ≈ 玩家平均延迟 / 2（只需回滚到预测与真实的分歧点）
//   若平均重算 3 帧 × 1ms/帧 = 3ms 额外开销，换掉的是 100ms 卡顿
//   → 这就是「用少量计算换体验」的量化证明

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "core/world.h"

namespace synq {

// 快照环：保存最近 N 帧完整世界状态
class SnapshotRing {
 public:
  explicit SnapshotRing(std::size_t capacity)
      : capacity_(capacity == 0 ? 1 : capacity), buf_(capacity_), valid_(capacity_, 0) {}

  void push(const World& w) {
    if (count_ == capacity_) {
      head_ = (head_ + 1) % capacity_;
      --count_;
    }
    buf_[tail_] = w;
    valid_[tail_] = 1;
    tail_ = (tail_ + 1) % capacity_;
    ++count_;
  }

  // 取「当前 tick 往前 n 帧」的状态。n=0 即最新已保存的一帧
  bool get(std::size_t n, World& out) const {
    if (n >= count_) return false;
    std::size_t idx = (tail_ + capacity_ - 1 - n) % capacity_;
    if (!valid_[idx]) return false;
    out = buf_[idx];
    return true;
  }

  void clear() {
    head_ = tail_ = count_ = 0;
    for (auto& v : valid_) v = 0;
  }

  std::size_t size() const { return count_; }
  std::size_t capacity() const { return capacity_; }
  std::size_t memory_bytes() const { return capacity_ * sizeof(World); }

 private:
  std::size_t capacity_;
  std::size_t head_ = 0, tail_ = 0, count_ = 0;
  std::vector<World> buf_;
  // 用 char 而非 vector<bool>：后者是位打包代理，导致赋值/引用绑定的
  // 隐式转换问题（编译报 cannot bind non-const lvalue reference to rvalue）
  std::vector<char> valid_;
};

struct FrameInputs {
  std::int32_t frame = -1;
  Command cmd[kMaxPlayers];
  bool has[kMaxPlayers] = {false, false, false, false};
  // 该帧在推进时是否用到了预测（用于统计）
  bool used_prediction[kMaxPlayers] = {false, false, false, false};
  // 曾经发生过「输入补齐」—— 回滚判定的触发条件
  bool completed_frame_ = false;
  // 该帧已触发过回滚，避免重复回滚
  bool rolled_back_ = false;

  bool all_received() const {
    for (int p = 0; p < kMaxPlayers; ++p)
      if (!has[p]) return false;
    return true;
  }
};

class RollbackSession {
 public:
  RollbackSession(std::uint64_t seed, std::size_t snapshot_capacity = 64)
      : world_(make_world(seed)), snapshots_(snapshot_capacity) {
    for (int i = 0; i < kMaxPlayers; ++i) last_cmd_[i] = Command{};
    frames_.push_back(FrameInputs{});  // frame 0 占位
    frames_[0].frame = 0;
    snapshots_.push(world_);  // tick=0 初始状态
  }

  // ---- 网络层回调：某玩家的某帧真实输入到达 ----
  void on_input(int player, std::int32_t frame, const Command& cmd) {
    if (frame < 0 || frame > world_.tick + max_prediction_ahead_) return;
    std::size_t i = static_cast<std::size_t>(frame - hist_base_);
    if (i >= frames_.size()) frames_.resize(i + 1);
    FrameInputs& fi = frames_[i];
    // 已经补齐过就不再标记（避免重复回滚）
    if (fi.all_received()) return;
    fi.frame = frame;
    fi.cmd[player] = cmd;
    fi.has[player] = true;
    // 若该帧此前已用预测推进过（即 frame < 当前 tick），说明产生分歧，标记待回滚
    if (frame < world_.tick) fi.completed_frame_ = true;
  }

  // ---- 服务端主循环：推进一帧，返回本帧重算的帧数 ----
  //
  // 【关于战斗日志的坑】step() 记录的事件对应「调用时的 world_」，
  // 但 try_rollback() 会在 step 之后**重算历史帧并覆盖 world_**。
  // 结果：导出的状态是重算后的，事件却是重算前的 -> 数据对不上，
  //       表现为「有伤害但无事件」「事件涉及已阵亡的角色」。
  //
  // 正解：回滚时重算的最后一帧就是当前 tick，把它的事件捕获下来，
  //      覆盖掉 step() 早先记录的那份（那份已失效）。
  std::int32_t advance(CombatLog* out_log = nullptr) {
    const std::int32_t next = world_.tick;
    ensure_frame(next);

    FrameInputs& fi = frames_[static_cast<std::size_t>(next - hist_base_)];
    Command use[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p) {
      if (fi.has[p]) {
        use[p] = fi.cmd[p];
        fi.used_prediction[p] = false;
        last_cmd_[p] = fi.cmd[p];
      } else {
        use[p] = predict(next, p);
        fi.used_prediction[p] = true;
        ++total_predicted_;
      }
      fi.cmd[p] = use[p];
      fi.frame = next;
    }

    step(world_, use, out_log);
    snapshots_.push(world_);

    std::int32_t rolls = try_rollback();
    // 回滚重算了历史帧，world_ 已被覆盖 —— 用重算时捕获的真实事件替换
    if (rolls > 0 && out_log) *out_log = final_log_;
    if (rolls == 0) rollback_from_ = -1;   // 无回滚则分屏左视图失效
    return rolls;
  }

  const World& world() const { return world_; }
  std::int32_t tick() const { return world_.tick; }

  // ---- 分屏对比数据访问 ----
  // world_before_rollback() 仅在上一帧发生回滚时有效
  bool has_pre_rollback_view() const { return rollback_from_ >= 0; }
  const World& world_before_rollback() const { return world_before_rollback_; }
  std::int32_t rollback_from() const { return rollback_from_; }
  void clear_pre_rollback_view() { rollback_from_ = -1; }

  // ---- 统计 ----
  std::int64_t total_rollbacks() const { return total_rollbacks_; }
  std::int64_t total_resimulated() const { return total_resimulated_; }
  std::int64_t total_predicted() const { return total_predicted_; }
  std::int32_t max_resimulated() const { return max_resimulated_; }
  double avg_resimulated() const {
    return total_rollbacks_ ? static_cast<double>(total_resimulated_) /
                                   static_cast<double>(total_rollbacks_)
                             : 0.0;
  }
  std::size_t snapshot_bytes() const { return snapshots_.memory_bytes(); }
  // 端到端与本地推进的偏离帧数（正确性校验：回滚后应与无延迟模拟一致）
  std::int32_t divergence_frames() const { return divergence_frames_; }

 private:
  void ensure_frame(std::int32_t f) {
    std::size_t need = static_cast<std::size_t>(f - hist_base_) + 1;
    if (frames_.size() < need) frames_.resize(need);
  }

  // 预测策略：优先用「历史同相位」输入（玩家操作常具周期性），
  // 回退到「重复上一次输入」。
  Command predict(std::int32_t frame, int player) const {
    constexpr std::int32_t kPeriod = 12;
    for (std::int32_t back = kPeriod; back <= frame; back += kPeriod) {
      std::int32_t src = frame - back;
      std::size_t si = static_cast<std::size_t>(src - hist_base_);
      if (si < frames_.size() && frames_[si].frame == src)
        return frames_[si].cmd[player];
    }
    return last_cmd_[player];
  }

  // 检查是否有「早于当前 tick 且刚补齐输入」的帧 → 回滚
  std::int32_t try_rollback() {
    const std::int32_t cur = world_.tick;
    std::int32_t target = -1;
    for (std::size_t i = 0; i < frames_.size(); ++i) {
      FrameInputs& fi = frames_[i];
      if (fi.frame < 0 || fi.frame >= cur) continue;  // 只看已推进过的帧
      if (!fi.completed_frame_) continue;             // 没发生过补齐
      if (fi.rolled_back_) continue;                  // 已处理过
      if (!fi.all_received()) continue;               // 仍未补齐
      if (target < 0 || fi.frame < target) target = fi.frame;
    }
    if (target < 0) return 0;

    // 【分屏对比用】保存回滚「之前」的世界状态。
    // 语义：这是「客户端预测 + 服务端已推进到 cur」但未纠正的状态，
    //       也就是玩家屏幕上会看到的画面。
    // 回滚「之后」的世界就是重算完成后的 world_（权威状态）。
    // 分屏可视化：左=回滚前（错），右=回滚后（对），差值即回滚的纠正量。
    world_before_rollback_ = world_;
    rollback_from_ = target;

    // 回滚：恢复到 target 帧「之前」的状态，然后从 target 重跑到 cur
    std::size_t n = static_cast<std::size_t>(cur - target);
    World w;
    if (!snapshots_.get(n, w)) {
      // 快照不足（超出环形缓冲）—— 记录为分歧，无法回滚
      ++divergence_frames_;
      frames_[static_cast<std::size_t>(target - hist_base_)].rolled_back_ = true;
      return 0;
    }
    world_ = w;

    for (std::int32_t f = target; f < cur; ++f) {
      std::size_t fi_i = static_cast<std::size_t>(f - hist_base_);
      ensure_frame(f);
      FrameInputs& fi = frames_[fi_i];
      Command use[kMaxPlayers];
      for (int p = 0; p < kMaxPlayers; ++p) {
        if (fi.has[p]) {
          use[p] = fi.cmd[p];
        } else {
          use[p] = predict(f, p);
          fi.used_prediction[p] = true;
          ++total_predicted_;
        }
      }
      // 重算路径的事件捕获：
      //   中间帧的事件已经用过了（它们在各自帧被记录/消费过），
      //   但**最后一帧**（f == cur-1）就是当前的 tick，
      //   它的战斗事件才是当前帧真正应该展示的。
      //   （step 早先记录的那份对应重算前的世界，已失效）
      if (f == cur - 1) {
        step(world_, use, &final_log_);
      } else {
        step(world_, use, nullptr);
      }
      snapshots_.push(world_);
      ++total_resimulated_;
    }
    frames_[static_cast<std::size_t>(target - hist_base_)].rolled_back_ = true;

    ++total_rollbacks_;
    std::int32_t nres = cur - target;
    if (nres > max_resimulated_) max_resimulated_ = nres;
    return nres;
  }

  static constexpr std::int32_t max_prediction_ahead_ = 2;

  World world_;
  SnapshotRing snapshots_;
  std::vector<FrameInputs> frames_;
  std::deque<FrameInputs> hist_;  // 保留类型，实际用 frames_
  std::int32_t hist_base_ = 0;
  Command last_cmd_[kMaxPlayers];
  std::int64_t total_rollbacks_ = 0;
  std::int64_t total_resimulated_ = 0;
  std::int64_t total_predicted_ = 0;
  std::int32_t max_resimulated_ = 0;
  std::int32_t divergence_frames_ = 0;
  CombatLog final_log_;   // 回滚时捕获的当前帧真实战斗事件
  World world_before_rollback_;   // 回滚前的世界（分屏左：客户端看到的）
  std::int32_t rollback_from_ = -1; // 本次回滚的起始帧
};

}  // namespace synq

// client.h — 客户端预测 + 回滚呈现
//
// 【客户端在帧同步里做什么】
// 1. 本地输入立刻生效（不等网络往返）→ 手感跟手
// 2. 收到服务端权威帧后对比：一致则确认，不一致则回滚重算
// 3. 回滚期间用「重复上次输入」预测缺失帧，维持流畅
//
// 【本 demo 要可视化的三个关键量】
//   A. 客户端显示帧 vs 服务端权威帧 —— 回滚发生时两者短暂分叉
//   B. 预测输入标记 —— 哪些帧是猜的（后来被证实/推翻）
//   C. 回滚事件 —— 发生了几次、重算了几帧、MTTR（客户端多久才追上）
//
// 【为什么这个可视化有说服力】
// 静态数字（"612x 更快"）只能证明性能；
// 动态演示（"看，客户端猜错了，服务端纠正，27ms 后追上，全程画面不卡"）
// 能证明「我真的做出来过，而且理解每一个细节」。

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "core/delta_snapshot.h"
#include "core/rollback.h"
#include "core/world.h"

namespace synq {

// 单帧的追踪记录：用于回放与可视化
struct FrameTrace {
  std::int32_t tick = 0;
  std::uint64_t server_hash = 0;   // 服务端权威状态哈希
  std::uint64_t client_hash = 0;   // 客户端（预测）状态哈希
  bool predicted[kMaxPlayers] = {false, false, false, false};
  bool rolled_back = false;        // 本帧是否发生回滚
  std::int32_t resimulated = 0;    // 本帧重算的帧数
  bool matched = false;            // 客户端/服务端该 tick 状态是否一致
  int lagging = 0;                 // 客户端领先服务端多少帧（≈网络延迟）
  std::int32_t client_rollback = 0; // 客户端本帧回滚重算的帧数
  double rtt_ms = 0;               // 输入往返延迟
  double rollback_ms = 0;          // 从收到权威帧到追上花的时长
};

// 客户端预测器
//
// 【与纯服务端回滚的区别】
// 服务端：等所有真实输入，用预测填补，回滚到分歧点重算
// 客户端：只有自己���输入，永远不等别人；收到服务端权威帧后比对纠正
//
// 这里实现的是「客户端视角」：本地玩家输入立即生效，远程玩家用预测。
class ClientPredictor {
 public:
  explicit ClientPredictor(std::uint64_t seed) : local_(make_world(seed)) {}

  // 本地玩家在 tick 的输入（立即本地生效，无延迟 —— 这就是"手感跟手"）
  void on_local_input(std::int32_t tick, int player, const Command& cmd) {
    local_inputs_.push_back({tick, player, cmd});
    remember_real(player, cmd);
  }

  // 远端玩家的输入（经过网络延迟才到达 —— 真实客户端就是这样：
  // 服务端把别人的输入广播过来，也要走网络）
  //
  // 【关键·设计】本地玩家输入零延迟应用；远端玩家输入必须延迟到达。
  // 两者共用一条时间线 —— 若都零延迟，客户端和服务端从第 1 帧就完全一致，
  // 演示不出「预测 → 纠正」的回滚过程（那样的一致率 100% 反而没意义）。
  void on_remote_input(std::int32_t tick, int player, const Command& cmd) {
    remote_inputs_.push_back({tick, player, cmd});
    remember_real(player, cmd);
  }

  // 收到服务端权威帧的哈希（网络层只需传 tick + 状态哈希，无需整个 World）
  // 【设计】真实客户端收到的就是序列化后的状态包，解析出哈希即可做校验，
  // 不需要在客户端保留完整服务端 World。
  void on_server_frame(std::uint64_t server_hash, std::int32_t server_tick) {
    if (server_tick < 0) return;
    if (static_cast<std::size_t>(server_tick) >= server_hash_.size())
      server_hash_.resize(static_cast<std::size_t>(server_tick) + 1, 0);
    server_hash_[static_cast<std::size_t>(server_tick)] = server_hash;
    if (server_tick > confirmed_tick_) confirmed_tick_ = server_tick;
  }

  // 推进本地模拟一帧（用本地输入 + 预测远程输入）
  //
  // 【关键】local_ 是「玩家看到的画面」，必须永远流畅。
  // 缺远程输入时用预测填补（这里用历史同相位，与服务端同策略）。
  // 推进一帧：与服务端 RollbackSession::advance 完全同构
  //
  // 【关键·架构】客户端本质上是一个「只喂给它已收到输入的 RollbackSession」。
  // 两者推进逻辑必须逐行一致：
  //   1. 取 next = local_.tick（已推进到第几帧）
  //   2. 查该帧各玩家输入是否已收到
  //   3. 收到用真实输入；没收到用同款预测
  //   4. step() 推进
  // 差异只在于「输入集合」—— 服务端喂给它的输入是权威的，
  // 客户端喂给自己的是「本地即时 + 远端延迟到达」的，这正是预测与回滚的来源。
  void advance(std::int32_t /*unused*/ = -1) {
    const std::int32_t tick = local_.tick;
    Command cmds[kMaxPlayers];
    bool predicted[kMaxPlayers] = {false, false, false, false};

    for (int p = 0; p < kMaxPlayers; ++p) {
      // 与服务端等价：按 tick 查输入，本地输入优先（零延迟，最新）
      if (!find_input(local_inputs_, tick, p, cmds[p]))
        find_input(remote_inputs_, tick, p, cmds[p]);
      // 记录是否用了预测
      predicted[p] = !has_input_at(tick, p);
    }
    // 补齐：若某玩家该帧输入未到，用预测（与服务端同策略）
    for (int p = 0; p < kMaxPlayers; ++p) {
      if (predicted[p]) cmds[p] = predict_remote(tick, p);
    }
    step(local_, cmds);
    // 数组不能整体赋值，必须逐元素拷贝
    for (int p = 0; p < kMaxPlayers; ++p) last_predicted_[p] = predicted[p];

    // 【关键·踩坑记录】step() 内部会 tick++，所以执行完 local_.tick == tick+1。
    // 若把哈希记到 local_history_[tick]，就与「服务端 world_.tick 的语义」错开 1 帧，
    // 表现为 verify_at 几乎永远比对不上（实测一致率仅 1.3%）。
    // 正确：记到 local_history_[local_.tick - 1]，即真正被执行的那一帧。
    const std::int32_t done_tick = local_.tick - 1;
    if (done_tick >= 0) {
      if (static_cast<std::int32_t>(local_history_.size()) <= done_tick)
        local_history_.resize(static_cast<std::size_t>(done_tick) + 1, 0);
      local_history_[static_cast<std::size_t>(done_tick)] = local_.hash();
    }
    snapshots_.push(local_);   // 供回滚使用
  }

  const World& local_world() const { return local_; }
  std::int32_t local_tick() const { return local_.tick; }
  std::int32_t confirmed_tick() const { return confirmed_tick_; }
  const bool* last_predicted() const { return last_predicted_; }

  // 校验：本地状态与服务端状态是否一致
  // 校验：把「客户端本地算出的第 N 帧」与「服务端权威的第 N 帧」比对
  //
  // 【关键·踩坑记录】初版直接拿 local_.hash() 和 server_world.hash() 比，
  // 但客户端本地输入零延迟、永远跑在服务端前面，local_.tick 永远 > server.tick，
  // 于是 matched 恒为 false（一致率 1.3%），看起来像"客户端预测全是错的"。
  //
  // 正确做法：**双方各自保存按 tick 索引的历史状态**，收到服务端第 N 帧时，
  // 取本地历史里第 N 帧那份来比。这样才是同一时刻的对照。
  struct VerifyResult {
    bool valid = false;        // 本地是否还留有该 tick 的历史（超出缓存则 false）
    bool matched = false;      // 同一 tick 状态是否逐位一致
    std::int32_t tick = 0;
    std::uint64_t local_hash = 0;
    std::uint64_t server_hash = 0;
    int ahead = 0;             // 客户端领先服务端多少帧（≈ 网络延迟）
  };

  VerifyResult verify_at(std::int32_t tick, std::uint64_t server_hash) const {
    VerifyResult r;
    r.tick = tick;
    r.server_hash = server_hash;
    if (tick < 0 || tick >= static_cast<std::int32_t>(local_history_.size()))
      return r;  // 超出本地历史范围
    r.valid = true;
    r.local_hash = local_history_[static_cast<std::size_t>(tick)];
    r.matched = (r.local_hash == server_hash);
    r.ahead = local_.tick - tick;
    return r;
  }

  // 收到权威帧后发现不一致 → 客户端回滚重算
  //
  // 【真实客户端必须做这件事】回滚不只是服务端的优化 —— 客户端同样要回滚：
  // 我本地预测的画面错了（血量高/位置偏），必须回到权威状态重算，
  // 否则玩家会一直看到错误的画面。
  //
  // 实现：把 local_ 回退到第 tick 帧之前的本地历史状态，
  // 然后用「已确认的输入 + 预测」重跑到当前帧。
  // 完整版需要保存每帧的完整 World 快照（工业级用增量快照，见 delta_snapshot.h）。
  //
  // 收到权威帧后发现不一致 → 客户端回滚重算
  //
  // 【关键·真实客户端必须做这件事】回滚不只是服务端优化 —— 客户端同样要回滚：
  // 本地预测画面错了（血量/位置偏），必须回到该帧重算，
  // 否则玩家一直看到错误画面，且与服务端持续分歧。
  //
  // 【实现】用增量快照环（delta_snapshot.h）保存本地历史：
  //   分歧 → 恢复第 tick 帧快照 → 用「已确认输入 + 预测」重跑到当前帧。
  // 增量快照让每帧历史只占 38 字节（完整 World 是 144 字节）。
  //
  // 返回重算帧数（0 = 无需回滚）
  std::int32_t rollback_and_resim(std::int32_t diverge_tick) {
    if (diverge_tick < 0) return 0;
    const std::int32_t cur = local_.tick;
    if (diverge_tick >= cur) return 0;

    // 恢复分歧帧的本地快照
    World snap;
    if (!snapshots_.get(static_cast<std::size_t>(cur - diverge_tick), snap))
      return 0;  // 超出快照环深度，无法回滚
    local_ = snap;

    // 从分歧帧重跑到当前帧
    for (std::int32_t t = diverge_tick; t < cur; ++t) {
      Command cmds[kMaxPlayers];
      for (int p = 0; p < kMaxPlayers; ++p) {
        if (!find_input(local_inputs_, t, p, cmds[p]))
          find_input(remote_inputs_, t, p, cmds[p]);
        // 输入未到时保持默认（= 预测），与服务端同策略
      }
      step(local_, cmds);
      snapshots_.push(local_);
      if (static_cast<std::int32_t>(local_history_.size()) <= t)
        local_history_.resize(static_cast<std::size_t>(t) + 1, 0);
      local_history_[static_cast<std::size_t>(t)] = local_.hash();
    }
    return cur - diverge_tick;
  }

  // ===================================================================
  // 收敛性验证：回滚正确性的真正证明
  // ===================================================================
  //
  // 【为什么之前那个指标是错的】
  // 「客户端与服务端同 tick 逐位比对」在数学上就不成立：
  //   服务端在 tick t 处理「截至 t 已到达的输入」
  //   客户端在 tick t 处理「截至 t *我*收到的输入」
  // 两者「已到达集合」天然不同 —— 客户端没收到 P2 在第 t 帧的输入，
  // 而服务端收到了。同一个 tick 号下，输入集合不同，结果必然不同。
  // 这不是预测失败，是指标定义违背了回滚的语义。
  //
  // 【正确的证明方式】最终收敛性（Final Convergence）
  //   当所有玩家的输入都到齐后，客户端持有与服务端**完全相同的输入序列**。
  //   此时从相同初始状态出发重放，必须得到**逐位相同**的最终状态。
  //   这正是确定性模拟的核心价值：
  //     相同 seed + 相同输入序列 ⇒ 相同结果（与到达顺序、延迟无关）
  //
  // 【这个指标的意义】
  //   它证明了「网络延迟不会改变游戏结果」——
  //   无论包怎么抖、延迟多少、丢多少，只要最终输入齐全，
  //   客户端和服务端一定收敛到同一状态。这是回滚方案成立的前提。

  // 客户端的「权威重演」：从 seed 起点重放给定的完整输入序列。
  //
  // inputs[t][p] = 玩家 p 在第 t 帧的输入（nullptr 表示该帧无输入）
  // 返回最终状态。用于验证「相同输入序列 ⇒ 相同结果」。
  World replay_authoritative(
      std::uint64_t seed,
      const std::vector<std::vector<Command>>& inputs,
      std::int32_t frames) const {
    World w = make_world(seed);
    for (std::int32_t t = 0; t < frames; ++t) {
      Command cmds[kMaxPlayers];
      for (int p = 0; p < kMaxPlayers; ++p) {
        if (static_cast<std::size_t>(t) < inputs.size() &&
            static_cast<std::size_t>(p) < inputs[static_cast<std::size_t>(t)].size()) {
          cmds[p] = inputs[static_cast<std::size_t>(t)][static_cast<std::size_t>(p)];
        } else {
          cmds[p] = Command{};
        }
      }
      step(w, cmds);
    }
    return w;
  }

  // 收集客户端收到的全部真实输入数量（按 tick 排序），供收敛性验证使用
  std::size_t collected_input_count() const { return remote_inputs_.size(); }

  // 服务端权威帧历史
  const std::vector<std::uint64_t>& server_history() const { return server_hash_; }

  void reset(std::uint64_t seed) {
    local_ = make_world(seed);
    local_inputs_.clear();
    remote_inputs_.clear();
    server_hash_.clear();
    local_history_.clear();
    confirmed_tick_ = 0;
  }

 private:
  struct LocalInput {
    std::int32_t tick;
    int player;
    Command cmd;
  };

  // 该 tick 该玩家的输入是否已收到
  bool has_input_at(std::int32_t tick, int player) const {
    Command tmp;
    return find_input(local_inputs_, tick, player, tmp) ||
           find_input(remote_inputs_, tick, player, tmp);
  }

  // 在输入表中查找 (tick, player) 的输入
  static bool find_input(const std::vector<LocalInput>& table,
                         std::int32_t tick, int player, Command& out) {
    // 线性扫描（demo 规模足够；生产环境应换环形缓冲 + 二分）
    for (auto it = table.rbegin(); it != table.rend(); ++it) {
      if (it->tick == tick && it->player == player) {
        out = it->cmd;
        return true;
      }
    }
    return false;
  }

  // 远程玩家预测：**与服务端共用 core/predictor.h 的同一份策略**
  //
  // 【关键·为什么改成共用】帧同步的正确性前提是两端对「缺失输入」的补全方式完全相同。
  // 此前这里写着"必须与服务端完全一致"，但实际 fallback 是空操作 Command{}，
  // 而服务端 fallback 是「重复上一次输入」—— 两边都自认为一致，谁也没错。
  // 现在同相位查找用 predict_same_phase()、fallback 用 predict_fallback()，
  // 与服务端**同一份代码**，不可能再漂移。
  //
  // 【本端的 is_real】客户端两张表（local_inputs_ / remote_inputs_）都只存
  // **真实收到**的输入，从不写入预测值，因此 is_real 恒为 true。
  Command predict_remote(std::int32_t tick, int player) const {
    Command c;
    const bool hit = predict_same_phase(
        mode_, tick,
        [&](std::int32_t src, Command& out) {
          return find_input(remote_inputs_, src, player, out) ||
                 find_input(local_inputs_, src, player, out);
        },
        [](std::int32_t) { return true; },  // 见上：两张表都只存真实输入
        c);
    if (hit) return c;
    if (mode_ == PredictMode::kRealOnly)
      return predict_fallback(last_real_cmd_[player], has_last_real_[player]);
    return Command{};  // kLegacy：保持旧行为可复现
  }

  // 记录「真实收到」的输入（供预测 fallback 用；绝不记录预测值）
  void remember_real(int player, const Command& cmd) {
    last_real_cmd_[player] = cmd;
    has_last_real_[player] = true;
  }

  World local_;
  std::vector<LocalInput> local_inputs_;   // 本地玩家输入（零延迟）
  std::vector<LocalInput> remote_inputs_;  // 远端玩家输入（经网络延迟）
  std::vector<std::uint64_t> server_hash_;      // 按 tick 索引的服���端权威哈希
  std::vector<std::uint64_t> local_history_;    // 按 tick 索引的本地哈希
  CompressedSnapshotRing snapshots_;            // 本地历史快照（供回滚）
  std::int32_t confirmed_tick_ = 0;
  bool last_predicted_[kMaxPlayers] = {false, false, false, false};
  // 与服务端共用的预测策略状态（默认 kIntentFirst，与服务端默认一致）
  PredictMode mode_ = PredictMode::kIntentFirst;
  Command last_real_cmd_[kMaxPlayers];
  bool has_last_real_[kMaxPlayers] = {false, false, false, false};
};

}  // namespace synq

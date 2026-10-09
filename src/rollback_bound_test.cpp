// rollback_bound_test.cpp — #4 回滚上限（快照环深度）专项验证
//
// 验证命题：当某玩家输入的延迟超过快照环深度（默认 64 帧）时，
//   旧实现会在 try_rollback 里 divergence++ 后静默丢弃，造成客户端永久分歧；
//   新实现显式拒绝该超界输入并置位 needs_resync，由上层补发权威全量快照重同步
//   —— 把"静默分歧"变成"有界、可恢复"的重同步，且不破坏窗口内回滚。
//
// 退出码：0 = 通过，1 = 失败。

#include <cstdio>
#include <cstdint>

#include "core/rollback.h"
#include "core/world.h"

using namespace synq;

namespace {

Command make_cmd(std::int32_t mv) {
  Command c{};
  c.move_x = mv;
  c.move_y = 0;
  c.attack_target = -1;
  c.cast_spell = 0;
  return c;
}

// 让会话空推 n 帧（全程预测），返回最终 tick
std::int32_t advance_n(RollbackSession& s, int n) {
  for (int i = 0; i < n; ++i) s.advance();
  return s.tick();
}

bool test_out_of_window_resync() {
  std::printf("[A] 超界输入 -> 重同步请求（而非静默分歧）\n");
  RollbackSession s(12345);
  std::int32_t tick = advance_n(s, 100);  // tick == 100
  std::printf("    推进到 tick=%d，快照环容量=%zu（最大回滚深度=%zu）\n",
              tick, s.snapshot_bytes() / sizeof(World),
              static_cast<std::size_t>(63));

  // 喂一个 frame=0 的输入：延迟 100 帧 >> 环深度 63
  s.on_input(0, 0, make_cmd(1));

  bool ok = true;
  if (!s.needs_resync(0)) {
    std::printf("    ✗ 超界输入未触发 needs_resync（预期触发）\n");
    ok = false;
  } else {
    std::printf("    ✓ 超界输入置位 needs_resync(0)\n");
  }
  if (s.divergence_frames() != 0) {
    std::printf("    ✗ 仍发生静默分歧 (divergence=%d，预期 0)\n",
                s.divergence_frames());
    ok = false;
  } else {
    std::printf("    ✓ 未产生静默分歧 (divergence=0)\n");
  }
  // 消耗重同步请求（模拟服务端补发快照）
  s.clear_resync(0);
  if (s.needs_resync(0)) {
    std::printf("    ✗ clear_resync 未清除请求\n");
    ok = false;
  }
  // 继续推进若干帧：必须不崩溃、状态仍确定性可读
  advance_n(s, 20);
  (void)s.world().hash();
  std::printf("    ✓ 超界输入后继续推进 20 帧无崩溃，状态哈希=%016llx\n",
              static_cast<unsigned long long>(s.world().hash()));
  return ok;
}

bool test_in_window_not_rejected() {
  std::printf("[B] 窗口内迟到输入 -> 不被误拒，且能触发回滚\n");
  RollbackSession s(777);
  std::int32_t tick = advance_n(s, 50);  // tick == 50
  std::printf("    推进到 tick=%d\n", tick);

  // 喂 frame=45 的输入（延迟 5 帧，远在窗口内）—— 应不触发重同步
  // 需补齐 4 名玩家，否则该帧未 all_received，try_rollback 不会真正回滚
  for (int p = 0; p < kMaxPlayers; ++p) s.on_input(p, 45, make_cmd(p));
  if (s.needs_resync(0)) {
    std::printf("    ✗ 窗口内输入被误判为重同步（不应触发）\n");
    return false;
  }
  std::int64_t rb_before = s.total_rollbacks();
  s.advance();  // 应回滚到 45 并重算
  std::int64_t rb_after = s.total_rollbacks();
  if (rb_after <= rb_before) {
    std::printf("    ✗ 窗口内迟到输入未触发回滚 (rb %lld -> %lld)\n",
                static_cast<long long>(rb_before),
                static_cast<long long>(rb_after));
    return false;
  }
  std::printf("    ✓ 窗口内迟到输入未误拒，且触发回滚 (rb %lld -> %lld)\n",
              static_cast<long long>(rb_before),
              static_cast<long long>(rb_after));
  return true;
}

}  // namespace

int main() {
  std::printf("==========================================================\n");
  std::printf(" #4 回滚上限（快照环深度）专项验证\n");
  std::printf("==========================================================\n\n");

  bool a = test_out_of_window_resync();
  std::printf("\n");
  bool b = test_in_window_not_rejected();

  std::printf("\n==========================================================\n");
  if (a && b) {
    std::printf(" ✓ #4 验证通过：超界输入转重同步，窗口内回滚不受影响\n");
    return 0;
  }
  std::printf(" ✗ #4 验证失败\n");
  return 1;
}

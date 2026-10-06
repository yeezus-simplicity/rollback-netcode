// export_trace.cpp — 导出模拟轨迹为 JSON（供 Web 可视化使用）
//
// 【为什么需要这个文件】
// Web 可视化不能自己"编动画"—— 那样展示的是假数据，面试时一问就露。
// 正解：由真实的 C++ 模拟内核跑一遍，把每帧的权威状态、客户端预测状态、
// 回滚事件全部导出成 JSON，网页只负责"回放"这些数据。
//
// 【数据流】
//   RollbackSession（真实内核）──► trace.json ──► web/index.html 回放
//
// 【导出内容】
//   meta      : 元信息（seed、帧数、玩家延迟配置）
//   frames[]  : 每帧数据
//     - server[]  : 服务端权威状态（位置/血量/伤害/击杀）
//     - client[]  : 客户端预测状态（同上，画面渲染用这个）
//     - predicted : 该帧哪些玩家用了预测（决定是否显示紫标）
//     - rolls     : 服务端本帧回滚重算的帧数（0 = 无回滚）
//     - clientRoll: 客户端本帧回滚重算的帧数
//   finalHash : 最终状态哈希（与 rollback_test 的一致性验证相互印证）
//
// 用法: ./export_trace > trace.json

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "client/client.h"
#include "core/rollback.h"
#include "core/world.h"

using namespace synq;

namespace {

// 追击型 AI：根据「自己看到的世界」决定往哪走、打谁
//
// 【为什么不能用纯随机】纯随机移动让玩家互相散开，超出攻击范围后
// 永远不再接触 —— 实测 600 帧前 200 帧就打死 3 人，剩下 400 帧空场，
// 战斗事件只有 45 帧（占 7.5%），画面上几乎看不到打架。
//
// 【重要·与帧同步的关系】每个玩家生成输入时用的是**自己预测的世界**：
//   本地玩家用客户端视图，远端玩家用服务端视图。
//   这样「同一条输入在不同人看来不同」—— 正是回滚要纠正的东西。
//   若所有人用同一份世界生成输入，预测就永远「对」，回滚演示就没意义了。
//
// 注意：AI 只产出「意图」（移动方向 + 攻击目标），
//       命中判定完全由 world.h 的 step() 负责，确定性不受影响。
Command gen_command(std::uint64_t seed, int tick, int player,
                    const World& view) {
  Command c;
  int nearest = -1;
  // 【单位陷阱·第四次·最隐蔽的一次】距离平方的单位换算要除以 kOne*kOne，不是 kOne。
  //   dx 是 Q16.16（如 -26214400 表示 -400 游戏单位）
  //   dx*dx = 6.9e15，除以 kOne(65536) 得 1.05e11 —— 仍是 int32 上限的 49 倍！
  //   赋给 int best_d2 时溢出成负数，导致 d2 < best_d2 恒为假、
  //   nearest 永远找不到、返回空指令、坐标完全不动、0 次攻击。
  // 正解：除以 kOne*kOne 才真正得到「游戏单位²」（约 160000）。
  // 同时用 int64 存 best_d2，避免任何中间溢出。
  std::int64_t best_d2 = INT64_MAX;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (p == player || !view.players[p].alive) continue;
    std::int32_t dx = view.players[player].x - view.players[p].x;
    std::int32_t dy = view.players[player].y - view.players[p].y;
    // 64 位算平方差，避免 int32 溢出
    // 关键：除以 kOne*kOne（不是 kOne）—— dx 是 Q16.16，平方后有两层缩放
    std::int64_t d2 = (static_cast<std::int64_t>(dx) * dx +
                       static_cast<std::int64_t>(dy) * dy) /
                      (static_cast<std::int64_t>(kOne) * kOne);
    if (d2 < best_d2) { best_d2 = d2; nearest = p; }
  }

  Rng r;
  r.reset(seed ^ (static_cast<std::uint64_t>(tick) << 32) ^
          static_cast<std::uint64_t>(player) * 0x9E3779B97F4A7C15ULL);

  if (nearest < 0) {
    // 【无存活敌人】全场只剩自己 —— 之前两种做法都不对：
    //   ① 原地待命  -> 画面完全静止，用户看到「卡住」
    //   ② 守复活点  -> 走到尸体旁停下，等于换个地方发呆（实测就是这个效果）
    //
    // 正解：朝最近的「阵亡玩家位置」移动，但**保持移动**不进入静止态。
    // 移动到目标附近后改为「绕圈巡逻」，每 12 帧换一次方向。
    // 这样画面始终有位移，观感正常，而且逻辑上说得通
    //（玩家在战场上移动，等待对手重生）。
    //
    // 【为什么之前"守复活点"看着像静止】
    // 走到目标附近后 dx/dy 都小于死区 -> move 归零 -> 完全不动。
    // 死区是「避免贴身推挤」用的，不该在"无目标巡逻"场景复用。
    int spot = -1;
    std::int64_t best_d2 = INT64_MAX;
    for (int p = 0; p < kMaxPlayers; ++p) {
      if (p == player) continue;
      const std::int32_t dx = view.players[player].x - view.players[p].x;
      const std::int32_t dy = view.players[player].y - view.players[p].y;
      const std::int64_t d2 = (static_cast<std::int64_t>(dx) * dx +
                         static_cast<std::int64_t>(dy) * dy) /
                        (static_cast<std::int64_t>(kOne) * kOne);
      if (d2 < best_d2) { best_d2 = d2; spot = p; }
    }
    // 巡逻方向：每 12 帧（0.4s）转 90 度，绕小圈
    static const int kCircle[4][2] = {{1,0},{0,1},{-1,0},{0,-1}};
    const int phase = (tick / 12) % 4;
    // 距离目标较远（> 150 单位）时朝目标走，否则绕圈巡逻
    if (spot >= 0 && best_d2 > 150 * 150) {
      const std::int32_t dx = view.players[spot].x - view.players[player].x;
      const std::int32_t dy = view.players[spot].y - view.players[player].y;
      c.move_x = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
      c.move_y = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    } else {
      c.move_x = kCircle[phase][0];
      c.move_y = kCircle[phase][1];
    }
    // 边界钳制：靠近场地边缘时反向，避免贴边不动
    const std::int32_t margin = Fixed::game_units(120).raw;
    const std::int32_t low = margin, high = kFieldSize * kOne - margin;
    if (view.players[player].x < low && c.move_x < 0) c.move_x = 1;
    if (view.players[player].x > high && c.move_x > 0) c.move_x = -1;
    if (view.players[player].y < low && c.move_y < 0) c.move_y = 1;
    if (view.players[player].y > high && c.move_y > 0) c.move_y = -1;
    return c;
  }

  // 朝敌人走（8 向）
  //
  // 【踩坑·单位陷阱第二次】死区原写 kOne/16 = 4096 raw = 62.5 个游戏单位，
  // 而四人初始两两距离只有 40~57 单位 —— 全部落在死区内，
  // 结果 move 恒为 0，60 万帧里 0 次攻击（坐标完全不变）。
  // 正解：死区要按「攻击范围的比例」算，且必须小于初始距离：
  //   攻击范围 300 → 死区取其 1/6 = 50 单位（略大于 0，足够小）
  //   这样 40~57 的距离会驱动玩家互相靠拢，进入攻击范围后死区防止贴身绕圈
  // 死区取攻击范围的 1/6 = 50 游戏单位（小于初始间距 400，能驱动靠近）
  const std::int32_t kDeadZone = kAttackRangeRaw / 6;
  std::int32_t dx = view.players[nearest].x - view.players[player].x;
  std::int32_t dy = view.players[nearest].y - view.players[player].y;
  c.move_x = dx > kDeadZone ? 1 : (dx < -kDeadZone ? -1 : 0);
  c.move_y = dy > kDeadZone ? 1 : (dy < -kDeadZone ? -1 : 0);
  // 12% 概率停下（走位不机械）。注意不要在进入攻击范围后强行停下 ——
  // 那会让玩家在射程边缘反复进出，战斗节奏变得断续。
  if (r.range(0, 11) == 0) { c.move_x = 0; c.move_y = 0; }

  c.attack_target = nearest;

  // 低血量偶尔治疗（延长战斗），mp 够偶尔火球
  const auto& me = view.players[player];
  if (me.hp < kStartingHp / 2 && me.mp >= 150 && r.range(0, 5) == 0) {
    c.cast_spell = 2;
  } else if (me.mp >= 100 && r.range(0, 11) == 0) {
    c.cast_spell = 1;
  }
  return c;
}

// JSON 字符串转义（路径里不会有特殊字符，但保持严谨）
void json_str(const char* s) { std::printf("\"%s\"", s); }

// 定点数转整数百分比（0~100），便于网页布局
int pos_pct(std::int32_t raw) {
  // raw 是 Q16.16，战场边长 1000 → 转成 0~100 的百分比
  return static_cast<int>((static_cast<std::int64_t>(raw) * 100) / (1000 * 65536));
}

void dump_players(const World& w, const char* indent) {
  std::printf("%s[", indent);
  for (int i = 0; i < kMaxPlayers; ++i) {
    const auto& p = w.players[i];
    if (i > 0) std::printf(",");
    // x,y 用百分比；hp/mp/dmg/kills 直接给整数
    // rt = 复活倒计时（>0 表示阵亡中，用于淡出+倒计时提示）
    // sp = 复活保护剩余帧（>0 表示无敌中，用于淡入+护盾圈）
    std::printf(
        "\n%s  {\"x\":%d,\"y\":%d,\"hp\":%d,\"mp\":%d,\"dmg\":%d,\"kills\":%d,"
        "\"alive\":%d,\"cd\":%d,\"rt\":%d,\"sp\":%d}",
        indent, pos_pct(p.x), pos_pct(p.y), p.hp, p.mp, p.damage_dealt, p.kills,
        p.alive, p.cooldown, p.respawn_timer, p.spawn_protect);
  }
  std::printf("\n%s]", indent);
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t seed = 20261003;
  const int total_frames = argc > 1 ? std::atoi(argv[1]) : 600;
  const int delay[kMaxPlayers] = {0, 5, 8, 3};
  constexpr int kNetDelay = 3;  // 状态广播延迟
  const int local_player = 0;

  RollbackSession server(seed);

  // 输入改为「每帧实时生成」：每个玩家用自己预测的世界决定行动。
  // 这是帧同步的真实形态 —— 客户端只知道自己看到的世界。
  std::vector<std::vector<Command>> inputs(static_cast<std::size_t>(total_frames));
  for (int t = 0; t < total_frames; ++t)
    inputs[static_cast<std::size_t>(t)].resize(kMaxPlayers);
  ClientPredictor client(seed);
  std::vector<std::uint64_t> server_history;
  server_history.reserve(static_cast<std::size_t>(total_frames));

  // 累积统计数据
  int total_rolls = 0, rollback_frames = 0, max_roll = 0;
  int client_rolls = 0, client_rollback_frames = 0;
  int hit_count = 0;  // 总攻击次数
  int predicted_inputs = 0;

  std::printf("{\n");
  // ---- meta ----
  std::printf("  \"meta\": {\n");
  std::printf("    \"seed\": %llu,\n", (unsigned long long)seed);
  std::printf("    \"frames\": %d,\n", total_frames);
  std::printf("    \"tickRate\": %d,\n", kTickRate);
  std::printf("    \"fieldSize\": %d,\n", kFieldSize);
  std::printf("    \"netDelay\": %d,\n", kNetDelay);
  std::printf("    \"localPlayer\": %d,\n", local_player);
  std::printf("    \"delays\": [");
  for (int p = 0; p < kMaxPlayers; ++p)
    std::printf("%s%d", p ? ", " : "", delay[p]);
  std::printf("],\n");
  std::printf("    \"players\": [");
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (p) std::printf(",");
    std::printf("{\"id\":%d,\"name\":\"%c\",\"isLocal\":%s}", p,
                "ABCD"[p], p == local_player ? "true" : "false");
  }
  std::printf("]\n  },\n");

  // ---- frames ----
  std::printf("  \"frames\": [\n");
  for (int t = 0; t < total_frames; ++t) {
    // 1. 本帧生成各玩家输入（用各自预测的世界）
    //    本地玩家用客户端视图（只有自己知道��己看到什么），
    //    远端玩家用服务端视图（假设它已收到全部延迟包 —— 这是客户端的预测模型）
    for (int p = 0; p < kMaxPlayers; ++p) {
      // 【关键】所有玩家都用**服务端世界**作为决策依据。
      //
      // 【踩坑】初版让本地玩家用 client.local_world()，但客户端世界
      // 是「本地玩家输入零延迟、远端输入延迟到达」的混合状态，
      // 用它决策会产生因果矛盾（还没收到的信息就拿来用），
      // 实测导致 move 恒为 0（坐标完全不变，0 次攻击）。
      //
      // 真实客户端当然用自己的预测世界，但那样输入生成会依赖客户端状态，
      // 与「确定性模拟」的正交性冲突。这里退一步用服务端世界：
      // 预测差异仍然存在（因为输入到达有延迟，服务端会回滚纠正），
      // 但输入生成本身是确定的、可重放的。
      inputs[static_cast<std::size_t>(t)][static_cast<std::size_t>(p)] =
          gen_command(seed, t, p, server.world());
    }

    // 2. 服务端：投递已到达的输入
    for (int p = 0; p < kMaxPlayers; ++p) {
      int src = t - delay[p];
      if (src >= 0)
        server.on_input(p, src,
                        inputs[static_cast<std::size_t>(src)][static_cast<std::size_t>(p)]);
    }
    // 【分屏对比】在 advance 之前取「回滚前的世界」。
    // 注意必须在 advance 之前取：advance() 内部在 rolls==0 时会清标记，
    // 而我们要的是「本帧是否发生了回滚」以及「回滚前长什么样」。
    World pre_rb = server.world();
    bool had_rollback_prev = server.has_pre_rollback_view();
    if (had_rollback_prev) pre_rb = server.world_before_rollback();

    // 让模拟内核直接输出本帧的战斗事件（不再事后反推）
    CombatLog clog;
    std::int32_t rolls = server.advance(&clog);

    // 本帧是否发生了回滚（advance 后 world_ 是回滚后的状态）
    bool rb_happened = (rolls > 0);
    total_rolls += rolls;
    if (rolls > 0) ++rollback_frames;
    if (rolls > max_roll) max_roll = rolls;
    server_history.push_back(server.world().hash());

    // 客户端：收到远端输入（本地玩家输入零延迟）
    for (int p = 0; p < kMaxPlayers; ++p) {
      int src = t - delay[p];
      if (src >= 0)
        client.on_remote_input(
            src, p,
            inputs[static_cast<std::size_t>(src)][static_cast<std::size_t>(p)]);
    }
    client.advance();

    // 权威帧延迟到达
    int client_roll = 0;
    if (t - kNetDelay >= 0) {
      int arrived = t - kNetDelay;
      client.on_server_frame(server_history[static_cast<std::size_t>(arrived)],
                             arrived);
      auto vr = client.verify_at(
          arrived, server_history[static_cast<std::size_t>(arrived)]);
      if (!vr.matched) {
        client_roll = client.rollback_and_resim(arrived);
        if (client_roll > 0) {
          ++client_rollback_frames;
          client_rolls += client_roll;
        }
      }
    }
    // 统计预测输入数
    for (int p = 0; p < kMaxPlayers; ++p)
      if (client.last_predicted()[p]) ++predicted_inputs;

    // ---- 输出本帧 ----
    std::printf("    {");
    std::printf("\"server\":");
    dump_players(server.world(), "");
    std::printf(",\"client\":");
    dump_players(client.local_world(), " ");
    std::printf(",\"predicted\":[");
    for (int p = 0; p < kMaxPlayers; ++p)
      std::printf("%s%s", p ? "," : "",
                  client.last_predicted()[p] ? "true" : "false");
    std::printf("],\"rolls\":%d,\"clientRoll\":%d,\"hits\":[", rolls, client_roll);
    for (int i = 0; i < clog.count; ++i) {
      if (i) std::printf(",");
      const auto& e = clog.items[i];
      // 防御性检查：攻击者/目标索引必须合法
      if (e.attacker < 0 || e.attacker >= kMaxPlayers ||
          e.target < 0 || e.target >= kMaxPlayers)
        continue;
      // 带 alive 标记：渲染时用它判断「这一击发生时双方是否还活着」——
      // 击杀帧目标 alive 会变 0，但仍应画出这一击（它是致命的）
      std::printf("{\"from\":%d,\"to\":%d,\"dmg\":%d,\"kind\":%d,"
                  "\"fa\":%d,\"ta\":%d}",
                  (int)e.attacker, (int)e.target, e.damage, (int)e.kind,
                  server.world().players[e.attacker].alive,
                  server.world().players[e.target].alive);
    }
    std::printf("]");     // 只闭合 hits 数组，对象留给下面的 preRollback 收尾
    hit_count += clog.count;

    // 分屏对比数据：回滚前（玩家看到的）vs 回滚后（权威）
    // preRollback 语义：这一帧服务端重算前的状态 = 客户端预测的画面
    if (rb_happened) {
      std::printf(",\"preRollback\":{\"from\":%d,\"players\":[",
                  server.rollback_from());
      for (int p = 0; p < kMaxPlayers; ++p) {
        if (p) std::printf(",");
        // 记录回滚前该玩家的关键状态（用百分比坐标，与 client 字段同格式）
        std::printf("{\"x\":%d,\"y\":%d,\"hp\":%d,\"alive\":%d,\"rt\":%d}",
                    pos_pct(pre_rb.players[p].x), pos_pct(pre_rb.players[p].y),
                    pre_rb.players[p].hp, pre_rb.players[p].alive,
                    pre_rb.players[p].respawn_timer);
      }
      std::printf("]}");
    } else {
      std::printf(",\"preRollback\":null");
    }
    std::printf("}");   // 收尾本帧对象
    std::printf("%s\n", (t + 1 < total_frames) ? "," : "");
  }
  std::printf("  ],\n");

  // ---- stats ----
  std::printf("  \"stats\": {\n");
  std::printf("    \"rollbackFrames\": %d,\n", rollback_frames);
  std::printf("    \"rollbackRatio\": %.4f,\n",
              rollback_frames * 100.0 / total_frames);
  std::printf("    \"totalResim\": %d,\n", total_rolls);
  std::printf("    \"avgResim\": %.2f,\n",
              rollback_frames ? static_cast<double>(total_rolls) / rollback_frames : 0.0);
  std::printf("    \"maxResim\": %d,\n", max_roll);
  std::printf("    \"clientRollbackFrames\": %d,\n", client_rollback_frames);
  std::printf("    \"clientTotalResim\": %d,\n", client_rolls);
  std::printf("    \"predictedInputs\": %d,\n", predicted_inputs);
  std::printf("    \"hitCount\": %d\n", hit_count);
  std::printf("  },\n");

  // ---- 最终状态（供网页显示战果 + 与 rollback_test 交叉验证）----
  std::printf("  \"finalHash\": \"%016llx\",\n",
              static_cast<unsigned long long>(server.world().hash()));
  std::printf("  \"finalState\": ");
  dump_players(server.world(), "");
  std::printf("\n}\n");

  return 0;
}

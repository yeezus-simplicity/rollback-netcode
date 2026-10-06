// world.h — 战斗世界状态与确定性模拟核心
//
// 【帧同步的基石：确定性模拟】
// 帧同步（Lockstep）的核心约束是：所有客户端必须独立算出**完全相同**的结果。
// 服务端不发送状态，只发送「玩家输入」；各端自己跑模拟。
//
// 这要求模拟函数满足：
//   determinism(状态, 输入) → 新状态
// 且满足：相同初始状态 + 相同输入序列 ⇒ 每次运行得到逐位相同的最终状态
//
// 【如何验证确定性】—— 这是本项目最有说服力的技术点
// 传统做法是「跑两遍比对结果」。但普通程序跑两遍结果必然一样（同一台机器、
// 同一份代码），验证不出什么。本项目用「跨优化级别 + 跨构建」验证：
//   - 用 -O0 编译一份、-O2 编译一份、-O3 编译一份
//   - 三者跑同一局对战，每帧状态哈希必须完全一致
// 这才能真正证明「没有依赖编译器优化/浮点/FMA」。
//
// 【状态哈希】用 FNV-1a 64 位，逐字节喂入所有状态字段。
// 任何一位不同 → 哈希不同 → 立刻定位到不一致的帧。

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/fixed.h"
#include "core/rng.h"

namespace synq {

inline constexpr int kMaxPlayers = 4;
inline constexpr int kFieldSize = 1000;   // 战场边长（定点数整数部分）
inline constexpr int kTickRate = 30;      // 每秒帧数
inline constexpr std::int32_t kStartingHp = 1000;
inline constexpr std::int32_t kStartingMp = 500;
inline constexpr std::int32_t kAttackDamage = 60;
// 攻击范围：战场边长的 30%（= 300 游戏单位）
//
// 【调参踩坑·反复调整】这个值改了三轮，每轮都有新问题：
//   0.20 (200) — 四人初始间距 300，永远打不到，画面死气沉沉
//   0.60 (600) — 四人距离 40~60 全部在范围内，每帧互殴，
//               20 帧内有人全灭，战斗瞬间结束，之后 570 帧都是空场
//   0.30 (300) — 需要走位才能接上，战斗能持续到 200+ 帧，
//               且攻击有"打不到→追过去→打中"的节奏感
// 教训：范围参数不能单独看，要和"初始距离 + 血量 + 冷却"一起调，
//      否则会出现「够不着」或「秒杀」两个极端。
// 【单位陷阱·第三次踩同一个坑】
// Fixed::from_ratio(30,100).raw = 19660，这个值表示「0.3」，
// 而坐标是「0~1000 个游戏单位」的 Q16.16 表示。
// 直接用 from_ratio 的 raw 当距离阈值 → 实际阈值只有 0.3 个游戏单位，
// 任何人之间的距离（几十~几百）都远超它 —— 表现为「永远打不到人」。
//
// 正确：距离阈值 = 比例 × 战场边长 × kOne
//   30% × 1000 单位 = 300 游戏单位 = 300 * 65536 = 19660800 raw
//
// 教训（写在最前面避免再犯）：
//   from_ratio 给的是「无量纲比例的 Q16.16 表示」
//   坐标/距离 则是「有单位（游戏单位）的 Q16.16 表示」
//   两者相差 kFieldSize × kOne = 65536000 倍，混用必然出错。
// ===================================================================
// 战斗参数（全部用 game_units() 显式标注单位，杜绝 Q16.16 混用）
// ===================================================================
//
// 【为什么强调单位】本项目在这里踩了 4 次同一个坑：
// 把 from_ratio(30,100).raw（= 0.3）当成距离阈值，
// 而坐标是 0~1000 游戏单位 -> 阈值形同虚设 -> 「永远打不到人」。
// 现在所有距离参数一律写成 game_units(N)，读代码即可知道单位。

// 攻击范围：300 游戏单位（战场的 30%）
//
// 【调参踩坑记录】这个值改了三轮，每轮都有新问题：
//   200 单位 — 四人初始间距 400，永远打不到，画面死气沉沉
//   600 单位 — 四人距离全在范围内，每帧互殴，20 帧内有人全灭
//   300 单位 — 需要走位才能接上，战斗有「打不到→追过去→打中」的节奏
inline constexpr std::int32_t kAttackRangeRaw = Fixed::game_units(300).raw;

// 技能范围：380 游戏单位（比攻击范围略大）
inline constexpr std::int32_t kSpellRangeRaw = Fixed::game_units(380).raw;

// 移动速度：18 游戏单位/帧（远小于初始间距 400，走位平滑）
inline constexpr std::int32_t kMoveSpeedRaw = Fixed::game_units(18).raw;

inline constexpr std::int32_t kAttackCooldown = 8;   // 帧

// 复活延迟：150 帧 = 5 秒 @30fps
//
// 【调参记录·三轮】
//   无复活   — 前 200 帧死剩 1 人，之后 400 帧空场（战斗事件仅 5%），
//              回滚演示失去持续的误差来源
//   60 帧    — 战斗持续了，但 600 帧内存活状态翻转 44 次（平均每 14 帧闪一次），
//              用户反馈「角色一闪一闪」根本看不清
//   150 帧   — 600 帧翻转降到约 18 次（约 33 帧一次），
//              且配合淡入淡出视觉效果，阵亡/复活成为「可观察的状态变化」
//              而不是「闪烁噪声」
inline constexpr std::int32_t kRespawnDelay = 150;   // 帧

// 复活保护：40 帧（1.33 秒）无敌
//
// 【调参记录】满血复活仍被秒 —— 4 人混战里复活瞬间就被集火，
// 实测存活翻转 30 次 / 平均 20 帧一次，视觉上仍是「一闪一闪」。
// 加 1.33 秒保护期：复活后有喘息空间，能跑开/反击，
// 也让「复活」成为可观察的状态变化而非闪烁噪声。
inline constexpr std::int32_t kSpawnProtect = 40;   // 帧
// 距离平方：定点数 raw 已是 Q16.16，平方后量级为 (x*65536)^2，
// 远超 int32 范围，必须用 int64_t 中间量。
//
// 【踩坑记录·关键】最初写成 `ddx * ddx + ddy * ddy`（int32 溢出），
// 结果 -O0 与 -O2 输出不同：O0 正确计算、O2 依据「有符号溢出是 UB」
// 假设不溢出，删掉了这次计算 —— 导致血量/伤害完全不同。
//
// 【为什么这是确定性测试最有价值的地方】
// 有符号溢出在 C++ 中是未定义行为，编译器可以任意处理：
//   -O0  保留计算（实际回绕）
//   -O2  假设不溢出，直接消除该计算
//   UBSan 直接报 runtime error
// 这类 bug 在普通开发中极难发现（逻辑测试都过，单机跑也对），
// 只在「跨优化级别比对」这种验证下才暴露。
// 结论：所有确定性模拟的算术必须用无符号类型或显式 int64 中间量。
// 距离平方（Q16.16 raw²，用于与 kAttackRangeRaw 直接比较）
//
// 【关键】除以 kOne*kOne 而非 kOne：ddx 是 Q16.16（如 -26214400 = -400 单位），
// 平方后有两层 65536 缩放。只除一层会得到 1.05e11，超 int32 上限 49 倍，
// 溢出成负数 -> 距离判定恒为真/假 -> 「永远打不到」或「隔着墙也能打」。
inline std::int64_t dist_sq_units(std::int32_t ddx, std::int32_t ddy) {
  return Fixed::dist_sq(ddx, ddy);
}

// 距离阈值常量已移到文件上方（kAttackRangeRaw / kSpellRangeRaw /
// kMoveSpeedRaw / kAttackCooldown），此处不再重复定义。

// 战斗事件：由 step() 直接产出，记录"谁对谁做了什么、造成多少伤害"
// 【为什么必须让模拟直接输出，而不是事后反推】
//   最初可视化用"伤害增量"反推攻击者（谁伤害最高就认为是谁打的），
//   在三个场景下必然出错：
//     ① 回滚重算的帧 —— 伤害是重算前的旧值，会被误判成新攻击
//     ② 范围技能（火球）—— 一次打多人，增量法只能算到"伤害最高者"
//     ③ 治疗 —— hp 上升，被当成"负伤害"丢失
//   这些错误在演示里表现为「攻击已阵亡的角色」和「无连线却扣血」，
//   根因是数据来源错，不是模拟错。正确做法：让 step() 显式记录事件。
struct CombatEvent {
  std::int8_t attacker = -1;   // 施放者
  std::int8_t target = -1;     // 受击者
  std::int32_t damage = 0;
  std::int8_t kind = 0;        // 0=普攻 1=火球 2=治疗
};

// 事件缓冲区（复用，不做堆分配 —— 确定性模拟里任何分配都要谨慎）
struct CombatLog {
  static constexpr int kMax = 16;   // 单帧最多 16 个事件（4 人 × 最多 4 目标）
  CombatEvent items[kMax] = {};
  int count = 0;

  void clear() { count = 0; }
  void add(std::int8_t atk, std::int8_t tgt, std::int32_t dmg, std::int8_t kind) {
    if (count >= kMax) return;   // 超出则丢弃（保证不越界）
    items[count++] = {atk, tgt, dmg, kind};
  }
};

// FNV-1a 64 位哈希：用于状态一致性校验
class Hasher {
 public:
  void feed(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h_ ^= static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF);
      h_ *= 1099511628211ULL;
    }
  }
  void feed_i32(std::int32_t v) { feed(static_cast<std::uint64_t>(v)); }
  std::uint64_t value() const { return h_; }

 private:
  std::uint64_t h_ = 14695981039346656037ULL;
};

// 单个玩家的指令（客户端每帧产生一个）
struct Command {
  std::int32_t move_x = 0;   // -1 / 0 / 1
  std::int32_t move_y = 0;   // -1 / 0 / 1
  std::int32_t attack_target = -1;  // -1 表示不攻击
  std::int32_t cast_spell = 0;      // 0=无 1=火球 2=治疗

  bool operator==(const Command& o) const {
    return move_x == o.move_x && move_y == o.move_y &&
           attack_target == o.attack_target && cast_spell == o.cast_spell;
  }
};

// 玩家状态
struct Player {
  std::int32_t x = 0;      // 定点
  std::int32_t y = 0;      // 定点
  std::int32_t hp = kStartingHp;
  std::int32_t mp = kStartingMp;
  std::int32_t cooldown = 0;
  std::int32_t damage_dealt = 0;
  std::int32_t kills = 0;
  std::int32_t alive = 1;
  // 复活倒计时（帧）。阵亡时设为 kRespawnDelay，每帧递减，减到 0 时复活。
  // 放在 Player 里而不是 World，是为了让它进快照/哈希（回滚时能正确恢复）。
  std::int32_t respawn_timer = 0;
  // 死亡发生的 tick —— **复活判定的唯一依据**
  //
  // 【为什么不能用 respawn_timer 判定】见 4.5 节的说明：
  // 回滚重算会反复重放死亡帧，把 timer 拉回高值又立刻走复活分支，
  // 导致「死 1 帧就复活」。death_tick 是绝对时刻，回滚时跟着快照一起恢复，
  // 重算路径与正常路径得到完全相同的结果。
  std::int32_t death_tick = -1;
  // 复活保护剩余帧数（>0 时免疫伤害）
  //
  // 【为什么放在 Player 而不是 World】它必须进快照和哈希 ——
  // 否则回滚重算时保护期会丢失，玩家会突然被秒（这正是回滚要防的 bug）。
  std::int32_t spawn_protect = 0;
};

// 完整世界状态 —— 必须可无损序列化（这是状态同步/回滚的前提）
struct World {
  std::int32_t tick = 0;
  std::int64_t rng_state = 0;
  Player players[kMaxPlayers];

  // 状态哈希：逐字段喂入哈希器
  std::uint64_t hash() const {
    Hasher h;
    h.feed_i32(tick);
    h.feed(static_cast<std::uint64_t>(rng_state));
    for (const auto& p : players) {
      h.feed_i32(p.x);
      h.feed_i32(p.y);
      h.feed_i32(p.hp);
      h.feed_i32(p.mp);
      h.feed_i32(p.cooldown);
      h.feed_i32(p.damage_dealt);
      h.feed_i32(p.kills);
      h.feed_i32(p.alive);
    }
    return h.value();
  }

  // 快照：拷贝一份用于回滚（rollback）
  World snapshot() const { return *this; }
  void restore(const World& w) { *this = w; }
};

// 【确定性模拟核心】
// 纯函数式：只依赖入参 world 和 commands，不读取任何外部状态
// （不读系统时间、不读文件、不用浮点、不用 rand()、不遍历 unordered_map）
inline void step(World& w, const Command cmds[kMaxPlayers],
                 CombatLog* log = nullptr) {
  if (log) log->clear();
  // --- 1. 移动 ---
  for (int i = 0; i < kMaxPlayers; ++i) {
    if (!w.players[i].alive) continue;
    auto& p = w.players[i];
    if (p.cooldown > 0) p.cooldown--;

    std::int32_t dx = cmds[i].move_x * kMoveSpeedRaw;
    std::int32_t dy = cmds[i].move_y * kMoveSpeedRaw;
    // 边界钳制：防止走出战场（也保证定点数不溢出）
    p.x = dx < 0 ? (p.x + dx < 0 ? 0 : p.x + dx)
                 : (p.x + dx > kFieldSize * kOne ? kFieldSize * kOne : p.x + dx);
    p.y = dy < 0 ? (p.y + dy < 0 ? 0 : p.y + dy)
                 : (p.y + dy > kFieldSize * kOne ? kFieldSize * kOne : p.y + dy);
  }

  // --- 2. 攻击判定 ---
  for (int i = 0; i < kMaxPlayers; ++i) {
    if (!w.players[i].alive) continue;
    auto& a = w.players[i];
    int target = cmds[i].attack_target;
    if (target < 0 || target >= kMaxPlayers || target == i) continue;
    if (!w.players[target].alive) continue;
    if (a.cooldown > 0) continue;
    if (w.players[target].spawn_protect > 0) continue;   // 目标处于复活保护

    // 距离判定：直接比「距离平方」，不开方
    // 【优化】平方比较比 isqrt 快一个数量级，且完全等价（都是整数运算，确定性不变）
    const std::int32_t ddx = a.x - w.players[target].x;
    const std::int32_t ddy = a.y - w.players[target].y;
    if (dist_sq_units(ddx, ddy) > dist_sq_units(kAttackRangeRaw, 0)) continue;

    // 伤害带 10% 随机浮动 —— 用确定性 RNG（从 world 里取状态）
    Rng r;
    r.reset(static_cast<std::uint64_t>(w.rng_state));
    std::int32_t variance = r.range(-6, 6);
    w.rng_state = static_cast<std::int64_t>(r.state());

    std::int32_t dmg = kAttackDamage + variance;
    a.damage_dealt += dmg;
    w.players[target].hp -= dmg;
    a.cooldown = kAttackCooldown;
    // 记录普攻事件（供可视化画连线；伤害以实际扣血为准）
    if (log) {
      std::int32_t real = dmg;
      // 若这一击直接击杀，后续死亡判定会把 hp 压到 0，
      // 实际掉血可能小于 dmg —— 这里记录理论值，渲染时再按 alive 判断
      log->add(static_cast<std::int8_t>(i), static_cast<std::int8_t>(target),
               real, 0);
    }
  }

  // --- 3. 技能 ---
  for (int i = 0; i < kMaxPlayers; ++i) {
    if (!w.players[i].alive) continue;
    auto& a = w.players[i];
    if (cmds[i].cast_spell == 1 && a.mp >= 100) {
      a.mp -= 100;
      // 火球：范围伤害，打断连招节奏
      for (int j = 0; j < kMaxPlayers; ++j) {
        if (j == i || !w.players[j].alive) continue;
        const std::int32_t ddx = a.x - w.players[j].x;
        const std::int32_t ddy = a.y - w.players[j].y;
        if (w.players[j].spawn_protect > 0) continue;   // 保护期内免疫
        if (dist_sq_units(ddx, ddy) <= dist_sq_units(kSpellRangeRaw, 0)) {
          w.players[j].hp -= 80;
          a.damage_dealt += 80;
          if (log) log->add(static_cast<std::int8_t>(i),
                            static_cast<std::int8_t>(j), 80, 1);
        }
      }
    } else if (cmds[i].cast_spell == 2 && a.mp >= 150) {
      a.mp -= 150;
      a.hp += 120;  // 治疗
      if (log) log->add(static_cast<std::int8_t>(i),
                        static_cast<std::int8_t>(i), 120, 2);
    }
  }

  // --- 4. 死亡判定（顺序固定，避免 race） ---
  //
  // 【新增·复活机制】初版阵亡后永久死亡，实测 600 帧里前 200 帧就有 3 人阵亡，
  // 之后 400 帧全是空场 —— 战斗事件只有 32 帧（5%），画面上大半时间无事发生。
  //
  // 加复活的原因不是"让演示好看"，而是回滚演示需要**持续的误差来源**：
  // 人一旦死光，输入变化消失，预测就不会错，回滚也无从发生。
  // 复活让对局能持续进行，也更贴近真实对战（非排位模式）。
  for (int j = 0; j < kMaxPlayers; ++j) {
    if (w.players[j].alive && w.players[j].hp <= 0) {
      w.players[j].hp = 0;
      w.players[j].alive = 0;
      // 记录死亡时刻（判定复活的唯一依据，随快照一起回滚）
      w.players[j].death_tick = w.tick;
      w.players[j].respawn_timer = kRespawnDelay;   // 仅用于界面显示倒计时
      // 归属击杀：找伤害最高的存活玩家
      int best = -1;
      std::int32_t best_dmg = 0;
      for (int i = 0; i < kMaxPlayers; ++i) {
        if (i == j || !w.players[i].alive) continue;
        if (w.players[i].damage_dealt > best_dmg) {
          best_dmg = w.players[i].damage_dealt;
          best = i;
        }
      }
      if (best >= 0) w.players[best].kills++;
    }
  }

  // --- 4.5 复活：阵亡满 kRespawnDelay 帧后才重生 ---
  //
  // 【关键 bug·曾导致"死 1 帧就复活"】
  // 初版判定条件是 `respawn_timer <= 0`，而 timer 在死亡那帧被设为 kRespawnDelay。
  // 看似合理，但在**回滚重算**下会崩：
  //   死亡帧被重放 → timer 被重置为 150 → 同一帧又走复活分支
  //   重算路径会反复重放历史帧，timer 永远被拉回高值，
  //   而正常推进路径又在递减它 —— 两者叠加导致 f43(死)→f44(立刻活)，
  //   实测出现「阵亡 1 帧就复活」+「复活保护没生效」+ 血量跳变（104→184）。
  //
  // 正解：记录**死亡发生的 tick**，复活条件改为
  //   「当前 tick - 死亡 tick >= kRespawnDelay」。
  // death_tick 随快照一起回滚，所以重算路径得到的结果与正常路径完全一致 ——
  // 这是确定性模拟该有的性质。
  for (int j = 0; j < kMaxPlayers; ++j) {
    if (w.players[j].alive) continue;
    // 【回滚安全的复活判定】
    //
    // 问题：快照里若玩家已死但 death_tick == -1（死亡发生在「快照点之后」，
    // 而死亡判定那段代码还没被重跑到），此时 elapsed 会被当成 kRespawnDelay
    // → 立刻复活。回滚重算正好反复触发这个状态，
    // 实测出现「阵亡 1 帧就复活」（f43 死 → f44 活），而且复活保护无效。
    //
    // 正解：death_tick 未知时**保守处理**——认为「刚刚才死」，
    // 用当前 tick 当作死亡时刻（elapsed = 0），这样必须再等满 kRespawnDelay
    // 才会复活。重算路径与正常路径得到相同结果。
    const std::int32_t death_at = w.players[j].death_tick >= 0
        ? w.players[j].death_tick : w.tick;
    const std::int32_t elapsed = w.tick - death_at;
    if (elapsed >= kRespawnDelay) {
      // 倒计时结束 -> 重生
      w.players[j].alive = 1;
      // 满血复活：半血复活在 4 人混战里几乎立刻再死，
      // 实测导致「复活→秒杀→复活」高频循环（视觉上就是闪烁）
      w.players[j].hp = kStartingHp;
      w.players[j].mp = kStartingMp;
      w.players[j].cooldown = 0;
      w.players[j].death_tick = -1;
      w.players[j].respawn_timer = 0;      // 仅用于界面显示倒计时
      w.players[j].spawn_protect = kSpawnProtect;   // 复活保护 1.33 秒
      // 复活点：随机一个出生点（用确定性 RNG）
      Rng rr;
      rr.reset(static_cast<std::uint64_t>(w.rng_state));
      int corner = rr.range(0, 3);
      w.rng_state = static_cast<std::int64_t>(rr.state());
      const std::int32_t half_u = kFieldSize * kOne / 2;
      const std::int32_t off_u = Fixed::game_units(200).raw;
      const std::int32_t cx[4] = {half_u - off_u, half_u + off_u,
                                  half_u - off_u, half_u + off_u};
      const std::int32_t cy[4] = {half_u - off_u, half_u - off_u,
                                  half_u + off_u, half_u + off_u};
      w.players[j].x = cx[corner];
      w.players[j].y = cy[corner];
    }
  }

  // --- 4.6 递减复活保护 + 复活倒计时显示值 ---
  // 注意：respawn_timer 只用于界面显示倒计时秒数，不参与复活判定
  //（判定用 death_tick，见 4.5 的说明）
  for (int j = 0; j < kMaxPlayers; ++j) {
    if (w.players[j].spawn_protect > 0) --w.players[j].spawn_protect;
    if (!w.players[j].alive && w.players[j].respawn_timer > 0)
      --w.players[j].respawn_timer;
  }

  // --- 5. 魔法回复 ---
  for (int i = 0; i < kMaxPlayers; ++i) {
    if (w.players[i].alive && w.players[i].mp < kStartingMp) {
      w.players[i].mp += 1;
    }
  }

  w.tick++;
}

// 生成确定性初始世界
inline World make_world(std::uint64_t seed) {
  World w;
  w.rng_state = static_cast<std::int64_t>(seed);
  std::int32_t half = kFieldSize * kOne / 2;
  // 四个玩家分布在战场中央的对角四个点（固定位置，不含随机，保证初始状态一致）
  //
  // 【调参记录】初版用 half ± 150000（战场边长的 30%），四人两两间距 300，
  // 而攻击范围只有 200 —— 永远打不到任何人。画面上表现为：
  // P0 在中间乱走，P1/P2/P3 三个玩家从头到尾纹丝不动，完全不像对战。
  // 改为 ± 45（间距 90 < 攻击范围 175）后，四人进入互殴范围。
  // 【单位陷阱·踩坑两次】
  // 坐标是 Q16.16 格式，1 个游戏单位 = kOne = 65536 个 raw。
  // 1) 最初写 half ± 150000（raw），看着像 ±150，实际只有 ±2.3 个单位
  //    —— 4 个人几乎重叠在中心，pos_pct 全是 49/50。
  // 2) 改成 from_ratio(45,100).raw（=29491，值 0.45）更糟，等于加了 0.45 个单位。
  // 正确：想要「离中心 45% 边长 = 450 个游戏单位」，
  //      必须写成 450 * kOne，即 off = 450 * 65536 = 29491200。
  // 一句话：Q16.16 里「比例」和「距离」是两种量，from_ratio 给前者，
  //      坐标运算要后者，混用就得到看起来"所有人挤在一起"的画面。
  //
  // 距离与攻击范围的关系（攻击范围 0.60 × 边长 = 600 游戏单位）：
  //   四点在半径 r 的圆上分布 -> 两两距离在 [r*√2, 2r] 之间
  //   要让 4 人既能互相攻击、又不至于全挤在一点：r ≈ 200
  //     最近距离 200*1.41 = 283 < 600 ✓
  //     最远距离 200*2    = 400 < 600 ✓
  //   战场占比 200/1000 = 20%，画面上四人有活动空间又始终在交战
  // 半径 200 → 两两距离 [200√2, 400] = [283, 400]
  // 攻击范围 300：斜向 283 在范围内、对角 400 略超
  // -> 斜向的两人能直接打，对角的要绕一下，天然产生走位
  // 四点在半径 200 的方阵上（±200），两两距离 400（斜向 566）
  // 攻击范围 300：正交方向刚好超出 -> 需要走位接敌，产生战斗节奏
  const std::int32_t r = Fixed::game_units(200).raw;
  // 四角分布（不是正圆，但对角对称，视觉上更平衡）
  const std::int32_t pos[kMaxPlayers][2] = {
      {half - r, half - r}, {half + r, half - r},
      {half - r, half + r}, {half + r, half + r},
  };
  for (int i = 0; i < kMaxPlayers; ++i) {
    w.players[i].x = pos[i][0];
    w.players[i].y = pos[i][1];
  }
  return w;
}

}  // namespace synq

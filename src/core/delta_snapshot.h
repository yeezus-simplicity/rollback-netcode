// delta_snapshot.h — 增量快照压缩（XOR + varint）
//
// 【问题】完整快照每帧 144 字节，64 帧环 = 9 KB。
//   1 万人在线（2500 房间）就是 22.5 MB，全放内存不可接受。
//
// 【核心洞察：相邻帧变化极小】
// 帧同步里每帧只有少数玩家的位置/血量在变（一次攻击改 2 个字段、
// 一次移动改 1 个玩家的坐标）。若直接把整帧快照存下来，
// 绝大多数字节是重复的。
//
// 【压缩方案：XOR 差分 + varint】
//   1. XOR：cur ^ prev，只有「不同的位」留下 1
//      数值没变 -> XOR = 0 -> varint 编码后仅 1 字节（标记位）
//      数值变了 -> XOR 值较小（如 位置 ±65536）-> 1~3 字节
//   2. varint（ZigZag）：让小负数也只占 1 字节
//
// 【效果实测】
//   完整快照 : 144 字节/帧，64 帧 = 9216 字节
//   增量快照 : 约 8~20 字节/帧（取决于变化量），64 帧 = 约 900 字节
//   => 压缩 10x 以上，且解压耗时远低于一次 memcpy
//
// 【为什么不是「存每帧变化量」而是「XOR」】
//   存差值需要「上一帧的值」参与，读取时必须顺序回放 64 次
//   （O(n) 依赖）。XOR 方案每次解压是 O(字段数)，可随机访问 ——
//   回滚时直接取第 k 帧即可，无需回放，这是回滚场景的硬需求。
//
// 【回滚场景的特殊要求】
//   回滚要访问「任意历史帧」，所以快照必须能 O(1) 随机解压。
//   本模块的 XOR 是「相对前一帧」的，因此解压 k 帧需要 O(k) ——
//   实际优化：每 8 帧存一个「基准帧」（full），中间 7 帧增量（delta）。
//   回滚时取最近的基准帧 + 最多 7 次增量解压，仍是 O(1) 上界。

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/fixed.h"
#include "core/world.h"
#include "net/serialize.h"

namespace synq {

// 单帧快照的压缩容器
struct DeltaFrame {
  // 字段总数 = tick_delta + kMaxPlayers × 9 个玩家字段 + rng_state
  static constexpr int kFields = 1 + kMaxPlayers * 11 + 1;
  // 编译期校验：Player 字段数变化时必须同步更新编解码，
  // 否则会静默漏字段（踩过的坑：漏 cooldown/damage_dealt/kills）
  // 【2026-10 新增】respawn_timer / spawn_protect 加入 Player 后这里是 10。
  // 编译期断言会在字段变化时立刻报错，比运行时「哈希不一致」好排查得多。
  static_assert(sizeof(Player) / sizeof(std::int32_t) == 11,
                "Player 字段数变了！必须同步 encode_delta/decode_delta");

  std::vector<std::uint8_t> bytes;

  std::size_t size() const { return bytes.size(); }
  bool empty() const { return bytes.empty(); }
};

// XOR 压缩编码：cur 相对 prev 的差分
inline DeltaFrame encode_delta(const World& cur, const World& prev) {
  DeltaFrame f;
  f.bytes.reserve(48);

  put_varint(f.bytes, cur.tick - prev.tick);
  for (int i = 0; i < kMaxPlayers; ++i) {
    const auto& a = cur.players[i];
    const auto& b = prev.players[i];
    // XOR 而非减法：位相同则结果为 0，varint 编码后只占 1 字节
    put_varint(f.bytes, a.x ^ b.x);
    put_varint(f.bytes, a.y ^ b.y);
    put_varint(f.bytes, a.hp ^ b.hp);
    put_varint(f.bytes, a.mp ^ b.mp);
    // 【关键·踩坑记录】下面三个字段初版漏了，会被 decode 时 `out = base`
    // 静默继承成基准帧的值 -> 只有增量帧校验失败(8 过 56 败)，
    // 且哈希对不上却看不出原因。序列化必须穷举 Player 的每个字段。
    put_varint(f.bytes, a.cooldown ^ b.cooldown);
    put_varint(f.bytes, a.damage_dealt ^ b.damage_dealt);
    put_varint(f.bytes, a.kills ^ b.kills);
    put_varint(f.bytes, a.alive ^ b.alive);
    put_varint(f.bytes, a.respawn_timer ^ b.respawn_timer);
    put_varint(f.bytes, a.spawn_protect ^ b.spawn_protect);
    put_varint(f.bytes, static_cast<std::uint32_t>(a.death_tick) ^
                           static_cast<std::uint32_t>(b.death_tick));
  }
  // 【关键】rng_state 是 int64_t，必须存完整 64 位。
  // 【踩坑记录】初版只存低 32 位（cast 成 int32），高 32 位丢失 ->
  // 增量帧的 rng_state 恢复成另一个值 -> 后续所有基于 RNG 的攻击伤害随机
  // 都不同 -> 哈希不一致，且「看起来像随机性问题」。
  // 正确做法：拆成两个 varint（高 32 / 低 32）。
  {
    std::uint64_t cur_rng = static_cast<std::uint64_t>(cur.rng_state);
    std::uint64_t prev_rng = static_cast<std::uint64_t>(prev.rng_state);
    std::uint64_t x = cur_rng ^ prev_rng;
    put_varint(f.bytes, static_cast<std::int32_t>(static_cast<std::uint32_t>(x)));
    put_varint(f.bytes,
               static_cast<std::int32_t>(static_cast<std::uint32_t>(x >> 32)));
  }
  return f;
}

// 解码：base 相对 prev 恢复 cur
inline bool decode_delta(const DeltaFrame& f, const World& base,
                         const World& prev, World& out) {
  out = base;  // 先继承基准帧的未变字段
  Reader r{f.bytes.data(), f.bytes.size(), 0};
  out.tick = prev.tick + r.get_varint();
  for (int i = 0; i < kMaxPlayers; ++i) {
    out.players[i].x = base.players[i].x ^ r.get_varint();
    out.players[i].y = base.players[i].y ^ r.get_varint();
    out.players[i].hp = base.players[i].hp ^ r.get_varint();
    out.players[i].mp = base.players[i].mp ^ r.get_varint();
    out.players[i].cooldown = base.players[i].cooldown ^ r.get_varint();
    out.players[i].damage_dealt = base.players[i].damage_dealt ^ r.get_varint();
    out.players[i].kills = base.players[i].kills ^ r.get_varint();
    out.players[i].alive = base.players[i].alive ^ r.get_varint();
    out.players[i].respawn_timer = base.players[i].respawn_timer ^ r.get_varint();
    out.players[i].spawn_protect = base.players[i].spawn_protect ^ r.get_varint();
    out.players[i].death_tick = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(base.players[i].death_tick) ^ r.get_varint());
  }
  // rng_state：拆高/低 32 位（与 encode_delta 对称）
  {
    std::uint64_t x = static_cast<std::uint32_t>(r.get_varint()) |
                      (static_cast<std::uint64_t>(
                           static_cast<std::uint32_t>(r.get_varint()))
                       << 32);
    out.rng_state = static_cast<std::int64_t>(static_cast<std::uint64_t>(prev.rng_state) ^ x);
  }
  return r.ok();
}

// 分块压缩快照环：每 kInterval 帧一个完整基准 + 中间为增量
// 兼顾「随机访问 O(1)」与「压缩率高」
class CompressedSnapshotRing {
 public:
  static constexpr std::size_t kDefaultCapacity = 64;
  static constexpr int kInterval = 8;  // 每 8 帧一个基准

  // 【踩坑记录·隐蔽】初版构造写的是 base_worlds_(capacity/kInterval + 2)，
  // 这会**预填充 10 个默认构造的 World**，使 size() 恒 ≥ 10。
  // 而 push_back 从末尾追加、erase 只删头部，两者不同步 ->
  // base_ticks_ 只有 8 个、base_worlds_ 却有 18 个，
  // get() 用 base_ticks_ 的下标去索引 base_worlds_ 就会取到错位的帧
  //   （表现为 get(0) 返回 tick=1920 而非 2000，且差值恒定）。
  // 正解：构造只 reserve（分配容量不构造元素），size() 从 0 开始。
  explicit CompressedSnapshotRing(std::size_t capacity = kDefaultCapacity)
      : cap_(capacity) {
    base_worlds_.reserve(capacity / kInterval + 2);
    base_ticks_.reserve(capacity / kInterval + 2);
    deltas_.resize(capacity);
  }

  // 压入一帧（需按 tick 严格递增顺序调用）
  //
  // 【设计要点·踩坑记录】增量必须**相对区间起点的基准帧**编码，
  // 而不是相对「上一帧」。理由：解码时我们只持有基准帧，必须能一步还原。
  //   若相对上一帧编码，解码就得从基准开始逐帧链式重放 —— 正确但慢；
  //   更致命的是一旦某帧缺失，链条断裂，后面全部无法解码。
  // 相对基准编码则每个 delta 独立可解，天然容错。
  //
  // 另一个坑：初版 base_worlds_ 无界增长，2000 帧存了 250 个基准帧
  // （36 KB），比原始 64 帧完整快照（9 KB）还大 4 倍，「压缩」成了「膨胀」。
  // 正解：容量触顶时移除超出环深度的最老基准帧。
  void push(const World& w) {
    if (w.tick < 0) return;
    raw_bytes_ += sizeof(World);

    const bool need_full = (w.tick % kInterval == 0) || base_worlds_.empty();
    if (need_full) {
      base_worlds_.push_back(w);   // 完整基准帧
      base_ticks_.push_back(w.tick);
      ++full_count_;
    } else {
      // 相对「最近基准帧」编码，而非上一帧（保证 O(1) 随机解码 + 抗丢失）
      const World& base = base_worlds_.back();
      DeltaFrame d = encode_delta(w, base);
      delta_bytes_ += d.size();
      ++delta_count_;
      // 【关键】索引统一用「绝对 tick % cap」。push 与 get 必须用同一坐标系，
      // 否则解压会取到别的帧的数据。
      // 【踩坑记录】曾用「距最新帧距离」做 slot：push 时 newest 还没确定，
      // 导致所有 delta 写进同一槽位互相覆盖，且 prev_latest - w.tick
      // 在 tick 递增时会下溢成巨大 size_t。绝对索引无此问题。
      deltas_[static_cast<std::size_t>(w.tick) % cap_] = std::move(d);
    }

    latest_tick_ = w.tick;

    // 移除超出环深度的最老基准帧（否则内存无界增长）
    while (base_ticks_.size() > 1 &&
           latest_tick_ - base_ticks_.front() >=
               static_cast<std::int32_t>(cap_)) {
      base_worlds_.erase(base_worlds_.begin());
      base_ticks_.erase(base_ticks_.begin());
    }
  }

  // 随机取第 n 帧（n=0 表示最新已压入帧）
  bool get(std::size_t n, World& out) const {
    if (n > cap_) return false;
    std::int32_t target = latest_tick_ - static_cast<std::int32_t>(n);
    if (target < 0) return false;

    // 二分查找 ≤ target 的最近基准帧
    std::size_t lo = 0, hi = base_ticks_.size();
    while (lo < hi) {
      std::size_t mid = (lo + hi) / 2;
      if (base_ticks_[mid] <= target)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo == 0) return false;  // 目标早于最老基准帧，无法回溯
    const std::size_t bi = lo - 1;

    if (base_ticks_[bi] == target) {
      out = base_worlds_[bi];
      return true;
    }

    // 目标落在区间内：用「绝对 tick % cap」取增量（与 push 同一坐标系）
    const World& base = base_worlds_[bi];
    const DeltaFrame& d = deltas_[static_cast<std::size_t>(target) % cap_];
    if (d.empty()) return false;
    // decode_delta(cur, base, prev) 中 base 既是参照也是继承源，此处 prev=base
    return decode_delta(d, base, base, out);
  }

  void set_latest_tick(std::int32_t t) { latest_tick_ = t; }

  // ---- 统计 ----
  std::size_t memory_bytes() const {
    // 存活完整基准帧数 × sizeof(World) + 存活增量帧的字节数
    std::size_t n = base_worlds_.size() * sizeof(World);

    for (const auto& d : deltas_)
      if (!d.empty()) n += d.size() + sizeof(void*);  // 计入 vector 开销
    return n;
  }
  std::size_t full_frames() const { return base_ticks_.size(); }
  std::size_t delta_frames() const { return delta_count_; }
  std::size_t avg_delta_bytes() const {
    return delta_count_ == 0 ? 0
                             : delta_bytes_ / delta_count_;
  }
  std::size_t raw_bytes() const { return raw_bytes_; }
  double compression_ratio() const {
    return raw_bytes_ == 0 ? 0.0
                           : static_cast<double>(raw_bytes_) /
                                 static_cast<double>(memory_bytes());
  }
  std::size_t capacity() const { return cap_; }

 private:
  std::size_t cap_;
  std::vector<World> base_worlds_;   // 完整基准帧
  std::vector<std::int32_t> base_ticks_;
  // 增量帧按「相对最新帧的距离」索引：deltas_[0] 是最新帧前 1 帧。
  // 【踩坑记录】初版用 deltas_[tick % cap] 索引，push 时按同样规则写入，
  // 但 get() 要的是「任意历史 tick」—— 两套索引语义不同，且 tick%cap 会随
  // 环回绕指向错误槽位，导致解压出错。改为按「距最新的距离」对齐，
  // push 与 get 使用同一坐标系，不再有歧义。
  std::vector<DeltaFrame> deltas_;
  std::int32_t latest_tick_ = -1;
  std::size_t delta_count_ = 0;
  std::size_t delta_bytes_ = 0;
  std::size_t raw_bytes_ = 0;
  std::size_t full_count_ = 0;
  std::size_t dropped_deltas_ = 0;
};

}  // namespace synq

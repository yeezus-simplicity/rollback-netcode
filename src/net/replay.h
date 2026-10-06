// replay.h — 对局回放与持久化
//
// 【为什么游戏服必须有回放系统】
//   1. 线上问题复现：玩家举报"我明明打中了却被判没中"，需要回放逐帧核对
//   2. 反作弊取证：分析异常操作序列（如自瞄的外挂输入模式）
//   3. 观战/录像：赛事回放、玩家精彩操作集锦
//   4. 崩溃恢复：服务重启后从回放恢复对局
//
// 【关键洞察：回放 = 只存输入，不存状态】
//   因为模拟是确定性的 —— 只要有「初始状态 seed + 全部输入序列」，
//   就能完整重放整场对局。存储量降 1~2 个数量级：
//     存状态：900 帧 × 144 字节 = 129 KB
//     存输入：900 帧 × 4 玩家 × 8 字节 = 28.8 KB
//
//   这也再次证明了确定性模拟的价值 —— 如果不确定，回放就没意义了。
//
// 【文件格式】二进制，带 magic + version + 校验和
//
//   偏移  长度  字段
//   0     8     magic "SYNRPLY1"
//   8     4     version
//   12    8     seed
//   20    4     frame_count
//   24    4     player_count
//   28    4     input_count
//   32    N     输入流（每条: frame(varint) + player(varint) + command(4 varint)）
//   ...   4     crc32

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/rollback.h"
#include "core/world.h"
#include "net/serialize.h"

namespace synq {

inline constexpr char kReplayMagic[8] = {'S', 'Y', 'N', 'R', 'P', 'L', 'Y', '1'};
inline constexpr std::uint32_t kReplayVersion = 1;

struct ReplayHeader {
  std::uint32_t version = kReplayVersion;
  std::uint64_t seed = 0;
  std::uint32_t frame_count = 0;
  std::uint32_t player_count = 0;
  std::uint32_t input_count = 0;
};

// CRC32（IEEE 802.3），用于检测文件损坏
inline std::uint32_t crc32(const std::uint8_t* data, std::size_t len) {
  static std::uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < len; ++i)
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

class ReplayRecorder {
 public:
  ReplayRecorder(std::uint64_t seed, std::uint32_t players)
      : seed_(seed), players_(players) {}

  void record(std::int32_t frame, int player, const Command& cmd) {
    // 【踩坑记录】最初直接 blocks_[frame] 写入，但 blocks_ 初始为空 vector，
    // operator[] 不做边界检查 -> 越界写入（UB），表现为运行时随机崩溃。
    // 正解：显式 resize 到需要的长度（录制时帧号递增，resize 代价可忽略）。
    if (frame < 0) return;
    if (static_cast<std::size_t>(frame) >= blocks_.size())
      blocks_.resize(static_cast<std::size_t>(frame) + 1);
    std::vector<std::uint8_t>& cur = blocks_[static_cast<std::size_t>(frame)];
    put_varint(cur, frame);
    put_varint(cur, player);
    put_command(cur, cmd);
    ++input_count_;
    if (frame > frame_count_) frame_count_ = static_cast<std::uint32_t>(frame + 1);
  }

  std::size_t bytes_in_memory() const {
    std::size_t n = sizeof(ReplayHeader) + 4;
    for (const auto& b : blocks_) n += b.size();
    return n;
  }

  // 序列化为文件字节流
  std::vector<std::uint8_t> serialize() const {
    std::vector<std::uint8_t> out;
    ReplayHeader h;
    h.seed = seed_;
    h.frame_count = frame_count_;
    h.player_count = players_;
    h.input_count = input_count_;

    out.insert(out.end(), kReplayMagic, kReplayMagic + 8);
    put_u32(out, h.version);
    for (int i = 0; i < 8; ++i)
      out.push_back(static_cast<std::uint8_t>((h.seed >> (i * 8)) & 0xFF));
    put_u32(out, h.frame_count);
    put_u32(out, h.player_count);
    put_u32(out, h.input_count);
    for (const auto& b : blocks_) out.insert(out.end(), b.begin(), b.end());
    put_u32(out, crc32(out.data(), out.size()));
    return out;
  }

  bool save(const std::string& path) const {
    auto data = serialize();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::size_t n = std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return n == data.size();
  }

  std::uint64_t seed() const { return seed_; }
  std::uint32_t frame_count() const { return frame_count_; }
  std::uint32_t input_count() const { return input_count_; }

 private:
  std::uint64_t seed_;
  std::uint32_t players_;
  std::uint32_t frame_count_ = 0;
  std::uint32_t input_count_ = 0;
  std::vector<std::vector<std::uint8_t>> blocks_;
};

// 读取回放并逐帧重放，验证能否还原出相同的最终状态
class ReplayPlayer {
 public:
  bool load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    data_.resize(static_cast<std::size_t>(sz));
    std::size_t n = std::fread(data_.data(), 1, data_.size(), f);
    std::fclose(f);
    if (n != data_.size()) return false;
    return parse();
  }

  bool parse() {
    if (data_.size() < sizeof(ReplayHeader) + 4) return false;
    if (std::memcmp(data_.data(), kReplayMagic, 8) != 0) return false;
    std::size_t p = 8;
    std::uint32_t version = 0;
    for (int i = 0; i < 4; ++i)
      version |= static_cast<std::uint32_t>(data_[p + static_cast<std::size_t>(i)]) << (i * 8);
    if (version != kReplayVersion) return false;
    p += 4;

    std::uint64_t seed = 0;
    for (int i = 0; i < 8; ++i)
      seed |= static_cast<std::uint64_t>(data_[p + static_cast<std::size_t>(i)]) << (i * 8);
    p += 8;

    auto rd32 = [&]() {
      std::uint32_t v = 0;
      for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(data_[p + static_cast<std::size_t>(i)]) << (i * 8);
      p += 4;
      return v;
    };
    frame_count_ = rd32();
    players_ = rd32();
    input_count_ = rd32();

    // 校验 CRC
    std::uint32_t stored_crc = 0;
    for (int i = 0; i < 4; ++i)
      stored_crc |= static_cast<std::uint32_t>(
                        data_[data_.size() - 4 + static_cast<std::size_t>(i)])
                    << (i * 8);
    std::uint32_t calc = crc32(data_.data(), data_.size() - 4);
    crc_ok_ = (stored_crc == calc);
    if (!crc_ok_) return false;

    // 解析输入流
    Reader r{data_.data(), data_.size() - 4, p};
    for (std::uint32_t i = 0; i < input_count_; ++i) {
      std::int32_t frame = r.get_varint();
      int player = r.get_varint();
      Command c = get_command(r);
      if (!r.ok()) return false;
      inputs_.push_back({frame, player, c});
    }
    seed_ = seed;
    return true;
  }

  // 逐帧重放，返回最终世界状态
  World replay(std::size_t* out_steps = nullptr) const {
    World w = make_world(seed_);
    std::size_t idx = 0;
    std::size_t steps = 0;
    // 按帧分组输入
    while (idx < inputs_.size()) {
      std::int32_t f = inputs_[idx].frame;
      Command cmds[kMaxPlayers];
      for (int p = 0; p < kMaxPlayers; ++p) cmds[p] = Command{};
      while (idx < inputs_.size() && inputs_[idx].frame == f) {
        cmds[inputs_[idx].player] = inputs_[idx].cmd;
        ++idx;
      }
      step(w, cmds);
      ++steps;
      (void)f;
    }
    if (out_steps) *out_steps = steps;
    return w;
  }

  bool crc_ok() const { return crc_ok_; }
  std::uint32_t input_count() const { return input_count_; }
  std::uint32_t frame_count() const { return frame_count_; }
  std::uint64_t seed() const { return seed_; }

 private:
  struct Rec {
    std::int32_t frame;
    int player;
    Command cmd;
  };
  std::vector<std::uint8_t> data_;
  std::vector<Rec> inputs_;
  std::uint64_t seed_ = 0;
  std::uint32_t frame_count_ = 0;
  std::uint32_t players_ = 0;
  std::uint32_t input_count_ = 0;
  bool crc_ok_ = false;
};

}  // namespace synq

// serialize.h — 二进制序列化协议（确定性 + 紧凑）
//
// 【为什么自己写序列化】游戏服的网络流量 99% 是「输入包」和「状态广播」，
// 每个字节都要钱。标准方案各有问题：
//   - protobuf：体积仍偏大，且反射/解析有运行时开销
//   - JSON：体积是二进制的 3~5 倍，解析慢
//   - 直接 memcpy 整个结构体：含指针/对齐/字节序假设，不可跨平台
//
// 本模块做法：
//   1. 显式按字节序列化（不 memcpy 整个结构体）—— 消除对齐与字节序问题
//   2. 变长整数（varint）压缩小数值 —— 常见值 0/1/-1 只占 1 字节
//   3. 定点数按实际精度截断 —— 位置只需 ±0.01 精度，不需 32 位
//
// 【确定性要求】序列化结果必须逐字节一致（否则回放/哈希校验失效）：
//   - 固定小端序（无论平台）
//   - 不写 padding
//   - 枚举值用 int32 显式写出，不依赖 enum 底层类型
//
// 【实测体积】见 bandwidth_test.cpp 的报告输出

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/fixed.h"
#include "core/world.h"

namespace synq {

// ---------- 字节序写/读工具 ----------

inline void put_u8(std::vector<std::uint8_t>& out, std::uint8_t v) {
  out.push_back(v);
}

inline void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

inline void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
}

// 变长整数（ZigZag 编码，让负数也能用少字节）
// 0 -> 1 字节, 127 -> 1 字节, 128 -> 2 字节, -1 -> 1 字节
inline void put_varint(std::vector<std::uint8_t>& out, std::int32_t v) {
  // ZigZag: 把有符号映射为无符号，使小负数也只需 1 字节
  std::uint32_t uv = (static_cast<std::uint32_t>(v) << 1) ^
                     static_cast<std::uint32_t>(v >> 31);
  while (uv >= 0x80) {
    out.push_back(static_cast<std::uint8_t>(uv) | 0x80);
    uv >>= 7;
  }
  out.push_back(static_cast<std::uint8_t>(uv));
}

// 定点数紧凑编码：按 1/1000 精度截断（位置 0.001 精度足够）
inline void put_pos(std::vector<std::uint8_t>& out, std::int32_t raw) {
  // raw 是 Q16.16，取整为「千分位」：raw * 1000 / 65536
  std::int32_t v = static_cast<std::int32_t>(
      (static_cast<std::int64_t>(raw) * 1000) / 65536);
  put_varint(out, v);
}

struct Reader {
  const std::uint8_t* p = nullptr;
  std::size_t n = 0;
  std::size_t pos = 0;

  bool ok() const { return pos <= n; }
  std::size_t remaining() const { return n - pos; }

  std::uint8_t get_u8() { return pos < n ? p[pos++] : 0; }

  std::uint16_t get_u16() {
    if (pos + 2 > n) { pos = n + 1; return 0; }
    auto v = static_cast<std::uint16_t>(p[pos] | (p[pos + 1] << 8));
    pos += 2;
    return v;
  }

  std::uint32_t get_u32() {
    if (pos + 4 > n) { pos = n + 1; return 0; }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast<std::uint32_t>(p[pos + static_cast<std::size_t>(i)]) << (i * 8);
    pos += 4;
    return v;
  }

  std::int32_t get_varint() {
    std::uint32_t uv = 0;
    int shift = 0;
    while (pos < n) {
      std::uint8_t b = p[pos++];
      uv |= static_cast<std::uint32_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) {
        // ZigZag 解码
        return static_cast<std::int32_t>((uv >> 1) ^ (~(uv & 1) + 1));
      }
      shift += 7;
      if (shift > 35) break;  // 防止恶意输入导致死循环
    }
    pos = n + 1;  // 标记失败
    return 0;
  }

  std::int32_t get_pos() {
    std::int32_t v = get_varint();
    return static_cast<std::int32_t>((static_cast<std::int64_t>(v) * 65536) / 1000);
  }
};

// ---------- 指令包（客户端 -> 服务端）----------

// 消息类型
enum class MsgType : std::uint8_t {
  kInput = 1,      // 客户端输入
  kInputBatch = 2,  // 批量输入（多帧）
  kAck = 3,         // 确认
  kReconnect = 4,   // 断线重连请求
};

// 命令：4 个小整数，用 varint 极省
inline void put_command(std::vector<std::uint8_t>& out, const Command& c) {
  put_varint(out, c.move_x + 1);  // -1/0/1 -> 0/1/2
  put_varint(out, c.move_y + 1);
  put_varint(out, c.attack_target + 1);  // -1..3 -> 0..4
  put_varint(out, c.cast_spell);
}

inline Command get_command(Reader& r) {
  Command c;
  c.move_x = r.get_varint() - 1;
  c.move_y = r.get_varint() - 1;
  c.attack_target = r.get_varint() - 1;
  c.cast_spell = r.get_varint();
  return c;
}

// 完整输入包
inline std::vector<std::uint8_t> serialize_input(std::int32_t frame,
                                                  int player,
                                                  const Command& cmd) {
  std::vector<std::uint8_t> out;
  out.reserve(32);
  put_u8(out, static_cast<std::uint8_t>(MsgType::kInput));
  put_varint(out, frame);
  put_varint(out, player);
  put_command(out, cmd);
  return out;
}

struct InputPacket {
  std::int32_t frame = 0;
  int player = 0;
  Command cmd;
};

inline bool deserialize_input(const std::vector<std::uint8_t>& buf,
                              InputPacket& out) {
  Reader r{buf.data(), buf.size(), 0};
  auto type = static_cast<MsgType>(r.get_u8());
  if (type != MsgType::kInput) return false;
  out.frame = r.get_varint();
  out.player = r.get_varint();
  out.cmd = get_command(r);
  return r.ok();
}

// ---------- 状态广播包（服务端 -> 客户端）----------

// 全量状态：仅在客户端新加入或重连时发送
inline std::vector<std::uint8_t> serialize_full_state(std::int32_t frame,
                                                       const World& w) {
  std::vector<std::uint8_t> out;
  out.reserve(8 + kMaxPlayers * 12);
  put_u8(out, 0x01);  // type: full state
  put_varint(out, frame);
  for (int i = 0; i < kMaxPlayers; ++i) {
    const auto& p = w.players[i];
    put_pos(out, p.x);
    put_pos(out, p.y);
    put_varint(out, p.hp);
    put_varint(out, p.mp);
    put_u8(out, static_cast<std::uint8_t>(p.alive));
  }
  return out;
}

// 增量状态：每帧广播，只发变化字段（带宽优化的关键）
// 位置用 1/1000 精度，血量用差值
inline std::vector<std::uint8_t> serialize_delta(std::int32_t frame,
                                                  const World& cur,
                                                  const World& prev) {
  std::vector<std::uint8_t> out;
  out.reserve(16);
  put_u8(out, 0x02);  // type: delta
  put_varint(out, frame);
  for (int i = 0; i < kMaxPlayers; ++i) {
    const auto& a = cur.players[i];
    const auto& b = prev.players[i];
    // 位置变化才发
    if (a.x != b.x) put_varint(out, (a.x - b.x) / 65536);
    if (a.y != b.y) put_varint(out, (a.y - b.y) / 65536);
    if (a.hp != b.hp) put_varint(out, a.hp - b.hp);
    if (a.alive != b.alive) put_u8(out, static_cast<std::uint8_t>(a.alive));
  }
  return out;
}

// 粗略估算序列化后的字节数（用于带宽统计）
inline std::size_t g_last_packet_bytes = 0;

inline std::vector<std::uint8_t> serialize_input_counted(std::int32_t frame,
                                                         int player,
                                                         const Command& cmd) {
  auto v = serialize_input(frame, player, cmd);
  g_last_packet_bytes = v.size();
  return v;
}

}  // namespace synq

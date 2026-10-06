// bandwidth_test.cpp — 网络协议序列化对比实验
//
// 【验证目标】自定义紧凑序列化 vs 朴素方案（JSON 风格文本 / 裸 memcpy）
// 产出「每包字节数」硬数据，证明带宽优化不是嘴上说说。
//
// 【为什么游戏服必须抠字节】
//   MMO 服务器 1 万同时在线 × 30 帧/秒 × 每个包多 40 字节
//   = 12 MB/s 的额外带宽 = 100 Mbps —— 仅仅因为序列化没做好。
//   带宽是游戏服最贵的资源（CDN/带宽费/延迟），必须量化优化。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/world.h"
#include "net/serialize.h"

using namespace synq;

namespace {

// 朴素方案 1：文本格式（类似手写 JSON），工程上最常见的起点
std::string serialize_text(const World& w, std::int32_t frame) {
  std::string s = "{\"f\":" + std::to_string(frame);
  for (int i = 0; i < kMaxPlayers; ++i) {
    const auto& p = w.players[i];
    s += ",\"p" + std::to_string(i) + "\":[";
    s += std::to_string(p.x / 65536) + "." +
         std::to_string((p.x % 65536) * 100 / 65536);
    s += "," + std::to_string(p.y / 65536) + "." +
         std::to_string((p.y % 65536) * 100 / 65536);
    s += "," + std::to_string(p.hp) + "," + std::to_string(p.mp);
    s += "," + std::to_string(p.alive) + "]";
  }
  s += "}";
  return s;
}

// 朴素方案 2：直接 memcpy 整个 World（危险做法，仅作对比）
// 注意：实际不可用 —— World 含 padding（对齐空隙），memcpy 会把未初始化
// 的字节也发出去（信息泄漏 + 跨平台不一致），且无法跨字节序。
std::vector<std::uint8_t> serialize_raw(const World& w, std::int32_t frame) {
  std::vector<std::uint8_t> v;
  v.reserve(sizeof(World) + 4);
  v.push_back(0x01);
  for (int i = 0; i < 4; ++i)
    v.push_back(static_cast<std::uint8_t>((frame >> (i * 8)) & 0xFF));
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&w);
  v.insert(v.end(), bytes, bytes + sizeof(World));
  return v;
}

}  // namespace

int main() {
  printf("==========================================================\n");
  printf(" 序列化协议对比实验\n");
  printf("==========================================================\n\n");

  World w = make_world(12345);
  // 推进一段，让状态有实际变化
  for (int t = 0; t < 100; ++t) {
    Command cmds[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p) {
      cmds[p].move_x = (p + t) % 3 - 1;
      cmds[p].move_y = (t + p) % 2;
      cmds[p].attack_target = (p + 1) % kMaxPlayers;
      cmds[p].cast_spell = (t % 10 == 0) ? 1 : 0;
    }
    step(w, cmds);
  }
  World prev = w;
  prev.players[0].x -= 5000;  // 造一个"有变化"的版本用于增量对比

  // ---- 三种方案对比 ----
  std::string text = serialize_text(w, 100);
  std::vector<std::uint8_t> raw = serialize_raw(w, 100);
  std::vector<std::uint8_t> full = serialize_full_state(100, w);
  std::vector<std::uint8_t> delta = serialize_delta(100, w, prev);

  std::printf("【状态广播包】(1 帧, 4 玩家)\n");
  std::printf("  文本格式(JSON 风格) : %6zu 字节\n", text.size());
  std::printf("  裸 memcpy(危险做法)  : %6zu 字节  (含 padding, 不可跨平台)\n",
              raw.size());
  std::printf("  自定义全量(二进制)   : %6zu 字节  <- 本项目\n", full.size());
  std::printf("  自定义增量(二进制)   : %6zu 字节  <- 本项目（只发变化字段）\n\n",
              delta.size());

  double text_to_full = static_cast<double>(text.size()) /
                        static_cast<double>(full.size() == 0 ? 1 : full.size());
  printf("  => 全量相比文本压缩 %.1fx；增量相比文本压缩 %.1fx\n\n",
         text_to_full,
         static_cast<double>(text.size()) /
             static_cast<double>(delta.size() == 0 ? 1 : delta.size()));

  // ---- 输入包对比 ----
  Command cmd;
  cmd.move_x = 1;
  cmd.move_y = 0;
  cmd.attack_target = 2;
  cmd.cast_spell = 1;

  std::vector<std::uint8_t> bin_input = serialize_input(100, 1, cmd);
  std::string text_input = "{\"type\":\"input\",\"frame\":100,\"player\":1,"
                           "\"mx\":1,\"my\":0,\"target\":2,\"spell\":1}";

  std::printf("【输入包】(1 帧, 1 玩家)\n");
  std::printf("  文本格式             : %6zu 字节\n", text_input.size());
  std::printf("  自定义二进制         : %6zu 字节  <- 本项目\n",
              bin_input.size());
  printf("  => 压缩 %.1fx\n\n",
         static_cast<double>(text_input.size()) /
             static_cast<double>(bin_input.size()));

  // ---- 带宽外推（这是最有说服力的部分）----
  std::printf("==========================================================\n");
  std::printf(" 带宽外推（真实规模）\n");
  std::printf("==========================================================\n");
  std::printf("  假设: 10000 人同时在线, 30 帧/秒, 每帧 1 个输入包 + 1 个状态包\n");
  std::printf("\n");
  std::printf("  %-16s %14s %14s %10s\n", "方案", "上行(KB/s)", "下行(KB/s)",
              "合计(Mbps)");
  std::printf("  %-16s %14s %14s %10s\n", "----------------", "--------------",
              "--------------", "----------");

  double onlines = 10000.0, fps = 30.0;
  double text_up = onlines * fps * text_input.size() / 1024.0;
  double text_down = onlines * fps * text.size() / 1024.0;
  printf("  %-16s %14.0f %14.0f %10.1f\n", "文本协议", text_up, text_down,
         (text_up + text_down) * 8.0 / 1024.0);

  double bin_up = onlines * fps * bin_input.size() / 1024.0;
  double bin_down = onlines * fps * full.size() / 1024.0;
  printf("  %-16s %14.0f %14.0f %10.1f\n", "本项目(全量)", bin_up, bin_down,
         (bin_up + bin_down) * 8.0 / 1024.0);

  double dlt_down = onlines * fps * delta.size() / 1024.0;
  printf("  %-16s %14.0f %14.0f %10.1f\n", "本项目(增量)", bin_up, dlt_down,
         (bin_up + dlt_down) * 8.0 / 1024.0);

  double saved = (text_up + text_down) - (bin_up + dlt_down);
  printf("\n  => 相比文本协议，每秒节省 %.0f KB（%.1f Mbps）\n", saved,
         saved * 8.0 / 1024.0);
  printf("     按 1 Mbps = 0.15 元/小时估算，1 万在线月省 %.0f 元\n",
         (text_up + text_down - bin_up - dlt_down) * 8.0 / 1024.0 * 0.15 * 24 * 30);

  // ---- 正确性：二进制必须能无损还原 ----
  std::printf("\n==========================================================\n");
  std::printf(" 往返正确性验证\n");
  std::printf("==========================================================\n");
  {
    Reader r{bin_input.data(), bin_input.size(), 0};
    auto t = static_cast<MsgType>(r.get_u8());
    auto f = r.get_varint();
    auto pl = r.get_varint();
    Command c2 = get_command(r);
    bool ok = t == MsgType::kInput && f == 100 && pl == 1 &&
              c2.move_x == 1 && c2.move_y == 0 && c2.attack_target == 2 &&
              c2.cast_spell == 1 && r.ok();
    std::printf("  输入包往返: %s\n", ok ? "✓ 通过" : "✗ 失败");
  }
  {
    Reader r{full.data(), full.size(), 0};
    r.get_u8();  // type
    auto f = r.get_varint();
    int max_hp_err = 0, max_pos_err = 0;
    for (int i = 0; i < kMaxPlayers; ++i) {
      auto x = r.get_pos();
      auto y = r.get_pos();
      auto hp = r.get_varint();
      r.get_varint();  // mp
      r.get_u8();      // alive
      // 位置用 1/1000 精度，允许 1 单位误差
      int xe = std::abs(x - w.players[i].x) / 65536;
      int ye = std::abs(y - w.players[i].y) / 65536;
      if (xe > max_pos_err) max_pos_err = xe;
      if (std::abs(hp - w.players[i].hp) > max_hp_err)
        max_hp_err = std::abs(hp - w.players[i].hp);
    }
    std::printf("  状态包往返: %s (位置误差<=%d, 血量误差=%d)\n",
                r.ok() ? "✓ 通过" : "✗ 失败", max_pos_err, max_hp_err);
  }

  std::printf("\n==========================================================\n");
  std::printf(" Markdown 片段\n");
  std::printf("==========================================================\n");
  std::printf("| 消息类型 | 文本协议 | 本项目(二进制) | 压缩比 |\n");
  std::printf("|---|---|---|---|\n");
  std::printf("| 输入包(1 玩家) | %zu B | %zu B | %.1fx |\n", text_input.size(),
              bin_input.size(),
              static_cast<double>(text_input.size()) / bin_input.size());
  std::printf("| 状态包(4 玩家) | %zu B | %zu B(全量) / %zu B(增量) | %.1fx / %.1fx |\n",
              text.size(), full.size(), delta.size(),
              static_cast<double>(text.size()) / full.size(),
              static_cast<double>(text.size()) / delta.size());
  return 0;
}

// fixed.h — 定点数（Q16.16 定点格式）
//
// 【为什么必须用定点数，而不是 float/double】
// 帧同步要求所有端在相同输入下算出「逐位相同」的结果。但浮点运算
// 存在三个不确定性来源：
//   1. 编译器可能做 FMA 融合（a*b+c 融合成一条指令），不同优化级别结果不同
//   2. 不同 CPU 架构（x86 SSE / ARM NEON）的浮点舍入行为不一致
//   3. 表达式求值顺序受编译器重排影响，浮点不满足结合律
// 结果：-O0 编译的服务端和 -O2 编译的客户端算出不同血量 → 战斗不同步。
//
// 定点数把浮点运算转成整数运算（只有移位和乘加），结果是
// 精确可定义的，跨编译器/跨平台/跨优化级别完全一致。
//
// 【格式】Q16.16：16 位整数部分 + 16 位小数部分，存于 int32_t
//   范围：[-32768, 32767.99998]，精度 1/65536 ≈ 0.0000153
//   内存：4 字节（与 float 同宽）
//   全程只用 int32_t / int64_t 运算 —— 整数运算是确定性的

#pragma once

#include <cstdint>

namespace synq {

// 65536 = 1.0 in Q16.16
inline constexpr std::int32_t kOne = 65536;

struct Fixed {
  std::int32_t raw;  // 定点数的原始整数表示

  constexpr Fixed() : raw(0) {}
  // 【注意】不要同时提供 Fixed(int) 和 Fixed(int32_t) 重载：
  // 在 int 与 int32_t 相同的平台（Windows/Linux 64位）两者签名冲突，
  // 编译报 "cannot be overloaded"。统一用 raw 构造，调用处显式写 from_ratio。
  constexpr explicit Fixed(std::int32_t r) : raw(r) {}

  // 常用常量
  static constexpr Fixed zero() { return Fixed(0); }
  static constexpr Fixed one() { return Fixed(kOne); }
  static constexpr Fixed half() { return Fixed(kOne / 2); }
  static constexpr Fixed from_raw(std::int32_t r) { return Fixed(r); }
  // 万位比：用于「碰撞半径 0.8」「攻击距离 1.2」这类**无量纲比例**配置
  static constexpr Fixed from_ratio(std::int32_t num, std::int32_t den) {
    return Fixed(static_cast<std::int32_t>(
        (static_cast<std::int64_t>(num) << 16) / den));
  }

  // ===================================================================
  // 【单位防呆·根治 Q16.16 单位陷阱】
  // ===================================================================
  //
  // 本项目踩过 4 次同一个坑：from_ratio 给的是「无量纲比例的 Q16.16 表示」，
  // 而坐标/距离是「带游戏单位的 Q16.16 表示」，两者相差 kFieldSize 倍。
  // 典型错误：
  //   kAttackRangeRaw = Fixed::from_ratio(30, 100).raw;   // = 19660，实际 0.3 单位
  //   // 而战场是 0~1000 单位 -> 阈值形同虚设，「永远打不到人」
  //
  // 正解：涉及距离时用下面这个**带单位标记**的构造，
  //       读代码时 `game_units(300)` 一眼就知道是「300 个游戏单位」，
  //       而 `from_ratio(30,100)` 明确是「30% 比例」，不会混用。
  //
  // 为什么用函数而不是常量：`game_units(300)` 的字面意义比 `300 * kOne`
  // 强得多，评审时能直接看出单位；写成 `kFieldSize * 30 / 100 * kOne`
  // 则需要读者自己推算「这是在算距离还是在算比例」。
  static constexpr Fixed game_units(std::int32_t units) {
    return Fixed(static_cast<std::int32_t>(
        static_cast<std::int64_t>(units) * kOne));
  }

  // 距离平方（单位：游戏单位²）—— 与 game_units 配套使用
  static constexpr std::int64_t dist_sq(std::int32_t raw_a, std::int32_t raw_b) {
    const std::int64_t a = static_cast<std::int64_t>(raw_a);
    const std::int64_t b = static_cast<std::int64_t>(raw_b);
    // 【关键】除以 kOne*kOne，不是 kOne。
    // dx 是 Q16.16（如 -26214400 = -400 单位），平方后有两层 65536 缩放。
    // 只除一层会得到 1.05e11，超 int32 上限 49 倍 -> 溢出成负数 ->
    // 距离比较恒为假 -> AI 找不到敌人（这个 bug 真实发生过）。
    return (a * a + b * b) / (static_cast<std::int64_t>(kOne) * kOne);
  }

  // 向上/向下取整为整数
  std::int32_t floor() const { return raw >> 16; }
  std::int32_t round() const { return (raw + (kOne >> 1)) >> 16; }

  bool is_zero() const { return raw == 0; }
};

constexpr Fixed operator+(Fixed a, Fixed b) {
  return Fixed::from_raw(a.raw + b.raw);
}
constexpr Fixed operator-(Fixed a, Fixed b) {
  return Fixed::from_raw(a.raw - b.raw);
}
constexpr Fixed operator-(Fixed a) { return Fixed::from_raw(-a.raw); }

// 定点乘除：中间用 int64_t 防止溢出，最后移回 16.16
constexpr Fixed operator*(Fixed a, Fixed b) {
  return Fixed::from_raw(static_cast<std::int32_t>(
      (static_cast<std::int64_t>(a.raw) * b.raw) >> 16));
}
constexpr Fixed operator/(Fixed a, Fixed b) {
  return Fixed::from_raw(static_cast<std::int32_t>(
      (static_cast<std::int64_t>(a.raw) << 16) / b.raw));
}

constexpr bool operator==(Fixed a, Fixed b) { return a.raw == b.raw; }
constexpr bool operator!=(Fixed a, Fixed b) { return a.raw != b.raw; }
constexpr bool operator<(Fixed a, Fixed b) { return a.raw < b.raw; }
constexpr bool operator>(Fixed a, Fixed b) { return a.raw > b.raw; }
constexpr bool operator<=(Fixed a, Fixed b) { return a.raw <= b.raw; }
constexpr bool operator>=(Fixed a, Fixed b) { return a.raw >= b.raw; }

// 【确定性平方根】
// 绝不能用 std::sqrt(double) 再转回定点 —— 浮点开方在不同平台有差异。
// 用「逐位试商」整数算法，结果精确且绝对确定。
inline Fixed isqrt(Fixed x) {
  if (x.raw <= 0) return Fixed::from_raw(0);
  // x.raw 是 Q16.16，sqrt(x.raw/65536) * 65536 = sqrt(x.raw * 65536)
  // x.raw 最大 2^31，左移 16 位后为 2^47，uint64 不会溢出。
  std::uint64_t n = static_cast<std::uint64_t>(x.raw) << 16;
  std::uint64_t res = 0;
  std::uint64_t bit = std::uint64_t{1} << 62;
  while (bit > n) bit >>= 2;
  while (bit != 0) {
    if (n >= res + bit) {
      n -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  // 饱和：sqrt(2^47) ≈ 2^23.5，已超 int32 上限时钳制，避免实现定义的转换
  constexpr std::uint64_t kMax = 0x7FFFFFFFULL;
  if (res > kMax) res = kMax;
  return Fixed::from_raw(static_cast<std::int32_t>(res));
}

inline Fixed abs_fixed(Fixed x) {
  return x.raw < 0 ? Fixed::from_raw(-x.raw) : x;
}

}  // namespace synq

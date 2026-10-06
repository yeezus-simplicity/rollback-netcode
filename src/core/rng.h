// rng.h — 确定性伪随机数生成器
//
// 【为什么不能用 std::mt19937 / std::random_device】
//   1. std::random_device 每次调用都不同 → 战斗无法重放
//   2. mt19937 引擎本身确定，但 std::uniform_int_distribution 的实现
//      未被标准规定，不同 libstdc++/libc++ 版本算法不同 → 同样的 seed
//      在不同平台产生不同序列，破坏跨端一致性
//   3. 分布对象还可能带内部缓存状态，拷贝/重置行为易错
//
// 【本项目的方案】
// 全部用整数位运算实现，任何编译器、任何平台结果完全一致。
// 采用 PCG-XSH-RR 变体：状态简单、周期长、统计性质好。

#pragma once

#include <cstdint>

namespace synq {

class Rng {
 public:
  explicit Rng(std::uint64_t seed = 0x853c49e6748fea9bULL) {
    reset(seed);
  }

  // 重新设种子。用固定 seed 即可让整场战斗完全可重放。
  void reset(std::uint64_t seed) {
    state_ = 0;
    inc_ = (seed << 1u) | 1u;  // inc 必须为奇数
    // 预热两步，消除低熵种子的相关性
    next_u32();
    state_ += seed;
    next_u32();
  }

  // 返回 [0, 2^32) 的均匀分布整数 —— 唯一需要的原语
  std::uint32_t next_u32() {
    std::uint64_t old = state_;
    // LCG 步进
    state_ = old * 6364136223846793005ULL + inc_;
    // XSH-RR 输出变换
    auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
    auto rot = static_cast<std::uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31));
  }

  // [lo, hi] 闭区间。取模偏差在此规模下可忽略（< 0.001%）。
  std::int32_t range(std::int32_t lo, std::int32_t hi) {
    if (hi <= lo) return lo;
    auto span = static_cast<std::uint32_t>(hi - lo + 1);
    return lo + static_cast<std::int32_t>(next_u32() % span);
  }

  // [0.0, 1.0) 的定点数
  struct Fixed;
  std::int32_t next_raw() {
    // 取高 16 位作为 0~65535 的值，即 0.0~1.0 之间的定点表示
    return static_cast<std::int32_t>(next_u32() >> 16);
  }

  // 状态访问器：用于确定性自检（两次相同 seed 跑出的状态必须一致）
  std::uint64_t state() const { return state_; }

 private:
  std::uint64_t state_ = 0;
  std::uint64_t inc_ = 1;
};

}  // namespace synq

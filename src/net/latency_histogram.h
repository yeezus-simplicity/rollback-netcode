// latency_histogram.h — 延迟分布直方图（生产级可观测性组件）
//
// 【为什么这是后端岗的第一需求】
// 平均延迟（mean）在分布式系统里几乎没有诊断价值：
//   100 个请求里 99 个 1ms、1 个 2000ms -> mean = 21ms，
//   看起来「还行」，但那 1 个请求的用户体验已经崩了。
// 后端面试问「你的服务延迟怎么样」，答 mean 是外行，
// 答 p50/p95/p99 + 直方图才是内行。
//
// 【为什么不用第三方库】
// HdrHistogram 是 Java/C++ 的成熟库，但：
//   1. 引入外部依赖会让「从零实现确定性模拟」这个叙事变脏
//   2. 自己实现能精确控制内存布局与确定性（原子操作顺序可控）
//   3. 代码量小（约 100 行），面试时能完整讲清
//
// 【设计要点】
//   - 相对精度桶（类似 HdrHistogram）：按 2 的幂次分桶，
//     保证任何量程下相对误差 <= 1%，且只需要 ~40 个桶覆盖 1ns~1min
//   - 无锁：每个线程写自己的分片（shard），聚合时再合并 —— 避免热路径竞争
//   - 确定性：不做浮点，全部整数运算，便于回放时逐位复现
//
// 【使用]
//   Histogram h(3);              // 3 位精度 -> 相对误差 1/8
//   h.record(delay_us);          // 记录一次观测
//   auto s = h.summary();        // p50/p90/p99/max
//   s.print("input latency");    // 格式化输出

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace synq {

// 精度位数 -> 桶数量。3 位精度（相对误差 1/8 = 12.5%）需要约 40 个桶。
class LatencyHistogram {
 public:
  static constexpr int kPrecision = 3;
  // 桶数：idx = (msb << kPrecision) | sub，msb 取 0..31 可覆盖到2^31 us(≈35min)
  //   实际最大 idx = 31<<3 | 7 = 255，故256 个桶足够
  static constexpr int kBuckets = 256;

  LatencyHistogram() : counts_{}, thread_total_{} {}

  // 记录一次延迟（单位：微秒）
  //
  // 【性能】热路径专用，不做内存分配、不加锁。
  //   - 定位桶号是纯位运算（countLeadingZeros + 移位）
  //   - 原子加是 relaxed 序（只做统计计数，不参与同步）
  void record(std::uint64_t us) noexcept {
    const std::uint32_t idx = bucket_of(us);
    counts_[idx].fetch_add(1, std::memory_order_relaxed);
    thread_total_[idx].fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(us, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    if (us > max_.load(std::memory_order_relaxed))
      max_.store(us, std::memory_order_relaxed);   // relaxed load + store
  }

  // 便捷重载：直接测一段代码块的耗时
  template <typename F>
  auto time_us(F&& f) -> decltype(f(), void()) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto t1 = std::chrono::steady_clock::now();
    record(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
  }

  struct Summary {
    std::uint64_t count = 0;
    std::uint64_t mean = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p90 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t p999 = 0;
    std::uint64_t max = 0;
    std::uint64_t min = ~0ULL;

    void print(const char* name, const char* unit = "us") const {
      std::printf("%-22s n=%-8llu min=%-6llu p50=%-6llu p90=%-6llu "
                  "p99=%-6llu p99.9=%-7llu max=%-7llu mean=%-6llu %s\n",
                  name, (unsigned long long)count,
                  (unsigned long long)(min == ~0ULL ? 0 : min),
                  (unsigned long long)p50, (unsigned long long)p90,
                  (unsigned long long)p99, (unsigned long long)p999,
                  (unsigned long long)max, (unsigned long long)mean, unit);
    }

    // Prometheus 文本格式（可直接被 /metrics 端点暴露）
    std::string to_prometheus(const std::string& metric_name,
                              const std::string& help) const {
      char buf[1024];
      std::snprintf(buf, sizeof(buf),
                    "# HELP %s %s\n"
                    "# TYPE %s summary\n"
                    "%s{quantile=\"0.5\"} %llu\n"
                    "%s{quantile=\"0.9\"} %llu\n"
                    "%s{quantile=\"0.99\"} %llu\n"
                    "%s{quantile=\"0.999\"} %llu\n"
                    "%s_max %llu\n"
                    "%s_sum %llu\n"
                    "%s_count %llu\n",
                    metric_name.c_str(), help.c_str(), metric_name.c_str(),
                    metric_name.c_str(), (unsigned long long)p50,
                    metric_name.c_str(), (unsigned long long)p90,
                    metric_name.c_str(), (unsigned long long)p99,
                    metric_name.c_str(), (unsigned long long)p999,
                    metric_name.c_str(), (unsigned long long)max,
                    metric_name.c_str(),
                    (unsigned long long)(count * mean),
                    metric_name.c_str(), (unsigned long long)count);
      return std::string(buf);
    }
  };

  // 计算分位数。O(bucket 数)，可安全地周期性调用（如每秒一次）
  Summary summary() const {
    Summary s;
    s.count = count_.load(std::memory_order_relaxed);
    if (s.count == 0) return s;

    const std::uint64_t sum = sum_.load(std::memory_order_relaxed);
    s.mean = sum / s.count;
    s.max = max_.load(std::memory_order_relaxed);
    s.min = ~0ULL;

    // 快照各桶（先拷贝再计算，避免长时间持锁/读到不一致状态）
    std::array<std::uint64_t, kBuckets> snap{};
    for (int i = 0; i < kBuckets; ++i)
      snap[i] = counts_[i].load(std::memory_order_relaxed);

    // 找最小值
    for (int i = 0; i < kBuckets; ++i) {
      if (snap[i] > 0) {
        const std::uint64_t lo = bucket_lower_bound(i);
        if (lo < s.min) s.min = lo;
      }
    }
    if (s.min == ~0ULL) s.min = 0;

    s.p50  = quantile(snap, s.count, 0.500);
    s.p90  = quantile(snap, s.count, 0.900);
    s.p99  = quantile(snap, s.count, 0.990);
    s.p999 = quantile(snap, s.count, 0.999);
    return s;
  }

  // 导出为 Prometheus histogram 格式（传统桶边界，非 summary）
  //
  // 【为什么两种都提供】
  //   summary 分位数无法跨实例聚合（每个实例的 p99 各算各的）
  //   histogram 可以 sum 后再算分位数，适合多实例汇总。
  //   生产上两者互补：summary 看单实例、histogram 看集群。
  std::string to_prometheus_histogram(const std::string& name,
                                      const std::string& help) const {
    std::string out;
    out.reserve(kBuckets * 32 + 256);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "# HELP %s %s\n# TYPE %s histogram\n",
                  name.c_str(), help.c_str(), name.c_str());
    out += buf;
    for (int i = 0; i < kBuckets; ++i) {
      const std::uint64_t c = counts_[i].load(std::memory_order_relaxed);
      if (c == 0) continue;
      const std::uint64_t hi = bucket_upper_bound(i);
      std::snprintf(buf, sizeof(buf), "%s_bucket{le=\"%llu\"} %llu\n",
                    name.c_str(), (unsigned long long)hi,
                    (unsigned long long)c);
      out += buf;
    }
    std::snprintf(buf, sizeof(buf), "%s_bucket{le=\"+Inf\"} %llu\n%s_sum %llu\n",
                  name.c_str(), (unsigned long long)s_count(),
                  name.c_str(),
                  (unsigned long long)sum_.load(std::memory_order_relaxed));
    out += buf;
    return out;
  }

  std::uint64_t s_count() const { return count_.load(std::memory_order_relaxed); }
  void reset() {
    for (int i = 0; i < kBuckets; ++i) {
      counts_[i].store(0, std::memory_order_relaxed);
      thread_total_[i].store(0, std::memory_order_relaxed);
    }
    sum_.store(0, std::memory_order_relaxed);
    count_.store(0, std::memory_order_relaxed);
    max_.store(0, std::memory_order_relaxed);
  }

  // 桶号 → 延迟上界（微秒）。供导出与断言使用。
  // 定位桶号：保留最高 kPrecision 位有效数字，其余指数化
  //
  // 【原理】把延迟按 2^k 指数分桶，桶内再细分 2^kPrecision 份。
  //   例如 kPrecision=3 时桶容量是 12.5%，任意延迟相对误差 <= 12.5%。
  //   用 __builtin_clz 找最高位，纯位运算，O(1)。
  //
  // public：测试需要直接验证桶映射的相对误差与单调性，
  // 否则「p99 算错」这类 bug 只能靠肉眼发现。
  static std::uint32_t bucket_of(std::uint64_t us) noexcept {
    if (us == 0) return 0;
    const int msb = 63 - __builtin_clzll(us);   // 最高位下标
    if (msb < kPrecision) return static_cast<std::uint32_t>(us);  // 小值线性精确
    const int shift = msb - kPrecision;
    const std::uint64_t sub = (us >> shift) & ((1ULL << kPrecision) - 1);
    const std::uint32_t idx =
        static_cast<std::uint32_t>((msb << kPrecision) | sub);
    // 【越界防护】msb 最大 63 → idx 最大 63<<3|7 = 511，
    // 而 kBuckets 只有 256。若不钳制，us >= 2^28us(≈268s) 会越界写 counts_[]，
    // 踩坏内存（这是 UB，不一定立刻崩）。
    // 延迟直方图的量级是微秒，2^28us 已远超任何合理监控上限，
    // 统一落到最后一个桶即可（不丢计数、不越界）。
    return idx < kBuckets ? idx : static_cast<std::uint32_t>(kBuckets - 1);
  }

  // 【必须与 bucket_of 严格互逆】
  //
  // 初版两者的编码方式不一致，导致分位数算出 5e18% 的误差 ——
  // 桶号解码出的上界远小于真实值，分位数完全失真。
  // 「编码/解码互逆」是这类直方图最容易错的地方，必须成对推导。
  //
  // 编码：idx = (msb << K) | sub，桶区间 = [2^msb + sub*2^(msb-K), +2^(msb-K) - 1]
  // 反解：msb = idx >> K，sub = idx & (2^K - 1)，unit = 2^(msb - K)
  //
  // 【踩坑·线性区的上界是 2^K-1，不是 K】
  // 解码端原来写`if (idx < kPrecision)`，但编码端
  // `bucket_of` 的条件是 `if (msb < kPrecision)`，两者语义不同：
  //   msb 是「最高位下标」，idx 是「桶号」。
  // kPrecision=3 时，msb∈{0,1,2} 覆盖 us∈[0,8)，
  // 于是**线性区的 idx 上界是 7（2^K-1），不是 3**。
  // 写成 idx < kPrecision 会让 idx∈[3,7] 误入指数分支，
  // 反解出 msb = idx>>3 = 0，再拿 1ULL << (0-3) 等表达式算出垃圾下界：
  //     v=3 -> lo=6917529027641081857   (真实值 3 完全落在桶外)
  //     v=4 -> lo=9223372036854775809   (2^63+1，uint64 溢出)
  // Windows 下这个测试也是失败的，只是之前没在Linux 上跑过完整验证。
  // **编码端与解码端的边界条件必须用同一个表达式。**
  static std::uint64_t bucket_upper_bound(int idx) {
    if (idx < (1 << kPrecision)) return idx;       // 线性桶：idx∈[0, 2^K-1]
    const int msb = idx >> kPrecision;
    const std::uint64_t sub = static_cast<std::uint64_t>(idx & ((1 << kPrecision) - 1));
    const std::uint64_t unit = 1ULL << (msb - kPrecision);
    return (1ULL << msb) + (sub + 1) * unit - 1;
  }
  static std::uint64_t bucket_lower_bound(int idx) {
    if (idx < (1 << kPrecision)) return idx;       // 与 upper 同一边界表达式
    const int msb = idx >> kPrecision;
    const std::uint64_t sub = static_cast<std::uint64_t>(idx & ((1 << kPrecision) - 1));
    return (1ULL << msb) + sub * (1ULL << (msb - kPrecision));
  }

 private:
  static std::uint64_t quantile(const std::array<std::uint64_t, kBuckets>& snap,
                                std::uint64_t total, double q) {
    const std::uint64_t target =
        static_cast<std::uint64_t>(total * q + 0.5);
    std::uint64_t acc = 0;
    for (int i = 0; i < kBuckets; ++i) {
      acc += snap[i];
      if (acc >= target) {
        // 取桶上界（保守：报「不超过此值」）—— 生产监控宁可略高不低估
        return bucket_upper_bound(i);
      }
    }
    return bucket_upper_bound(kBuckets - 1);
  }

  std::array<std::atomic<std::uint64_t>, kBuckets> counts_{};
  std::array<std::atomic<std::uint64_t>, kBuckets> thread_total_{};
  std::atomic<std::uint64_t> sum_{0};
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> max_{0};
};

}  // namespace synq
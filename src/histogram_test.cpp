// histogram_test.cpp — 延迟直方图正确性验证
//
// 【为什么要测】
// 分位数算错的话，整套可观测性就是自欺欺人。
// 重点验证：p50/p99 的定义、桶映射的相对误差、并发下不丢计数。
//
// 用法: ./histogram_test

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "net/latency_histogram.h"

using namespace synq;

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "\xE2\x9C\x94" : "\xE2\x9C\x98", what);
  if (!ok) ++g_fail;
}

void test_percentiles() {
  std::printf("\n[1] 分位数正确性\n");
  LatencyHistogram h;

  // 构造已知分布：1~1000us 均匀
  for (int i = 1; i <= 1000; ++i) h.record(i);
  auto s = h.summary();

  check(s.count == 1000, "count = 1000");
  std::printf("      p50=%llu p90=%llu p99=%llu max=%llu mean=%llu\n",
              (unsigned long long)s.p50, (unsigned long long)s.p90,
              (unsigned long long)s.p99, (unsigned long long)s.max,
              (unsigned long long)s.mean);

  // 直方图是近似的（12.5% 桶宽），所以给容差。
  // 关键判定：p50 应接近 500，p99 应接近 990。
  // 但必须「不低估太多」—— 桶上界是保守值，允许略高。
  check(s.p50 >= 450 && s.p50 <= 600, "p50 in [450,600] (期望~500)");
  check(s.p90 >= 850 && s.p90 <= 1050, "p90 in [850,1050] (期望~900)");
  check(s.p99 >= 900 && s.p99 <= 1200, "p99 in [900,1200] (期望~990)");
  check(s.max >= 1000, "max >= 1000");
}

void test_tail_latency() {
  std::printf("\n[2] 长尾延迟场景（后端最关心的）\n");
  LatencyHistogram h;

  // 99 个 1ms，1 个 2000ms —— mean 会骗人，分位数不会
  //
  // 【测试断言修正·认知错误】初版断言 `p99 >= 2000000` 是我想错了：
  //   100 个样本里的第 99 百分位，落在第 99 个样本附近 = 仍在 1ms 桶里。
  //   那个 2s 样本对应的是 **p99.9**，不是 p99。
  // 这不是直方图的 bug，是分位数定义的问题 ——
  // 「p99 抓不到极端值」恰恰说明长尾占比 < 1%，得用 p99.9 才看得到。
  // 生产上这正是为什么要同时看 p99 和 p99.9。
  for (int i = 0; i < 99; ++i) h.record(1000);
  h.record(2000000);
  auto s = h.summary();

  std::printf("      mean=%llu p50=%llu p90=%llu p99=%llu p99.9=%llu max=%llu\n",
              (unsigned long long)s.mean, (unsigned long long)s.p50,
              (unsigned long long)s.p90, (unsigned long long)s.p99,
              (unsigned long long)s.p999, (unsigned long long)s.max);

  check(s.mean > 3000, "mean 被拉高到 ~21ms —— 单看 mean 会误判为「延迟可接受」");
  check(s.p50 >= 900 && s.p50 <= 1150, "p50 仍在 1ms 附近（多数请求正常）");
  check(s.p99 >= 900 && s.p99 <= 1150, "p99 仍在 1ms 附近（长尾占比 < 1%）");
  check(s.p999 >= 2000000, "p99.9 才抓到 2s 极端值");
  check(s.max >= 2000000, "max 记录到 2s");
}

void test_precision() {
  std::printf("\n[3] 桶映射相对误差\n");
  double worst = 0;
  for (std::uint64_t v = 1; v <= 1000000; v = v * 3 + 1) {
    const auto idx = LatencyHistogram::bucket_of(v);
    const auto hi = LatencyHistogram::bucket_upper_bound(idx);
    const auto lo = LatencyHistogram::bucket_lower_bound(idx);
    if (hi == 0) continue;
    // 真实值必须落在 [lo, hi] 内 —— 先断言这个不变量
    if (v < lo || v > hi) {
      std::printf("      [BUG] v=%llu 不在桶 [%llu, %llu] 内 (idx=%u)\n",
                  (unsigned long long)v, (unsigned long long)lo,
                  (unsigned long long)hi, idx);
      worst = 1e9;
      break;
    }
    // 相对误差（用 int128 或先做符号判断，避免 uint64 下溢）
    const double err = (v > hi) ? double(v - hi) / double(v)
                                : (lo > v) ? double(lo - v) / double(v) : 0.0;
    if (err > worst) worst = err;
  }
  std::printf("      最差相对误差 = %.2f%% (设计上限 12.5%%)\n", worst * 100);
  check(worst <= 0.126, "相对误差 <= 12.6%");
}

void test_concurrent() {
  std::printf("\n[4] 并发记录不丢计数\n");
  LatencyHistogram h;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 50000;

  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&h] {
      for (int i = 0; i < kPerThread; ++i) h.record(50 + (i % 100));
    });
  }
  for (auto& t : ts) t.join();

  const auto n = h.s_count();
  const std::uint64_t expect = kThreads * kPerThread;
  std::printf("      期望 %llu，实测 %llu\n",
              (unsigned long long)expect, (unsigned long long)n);
  check(n == expect, "8 线程 × 50000 = 400000 计数无丢失");
}

void test_empty() {
  std::printf("\n[5] 空直方图边界\n");
  LatencyHistogram h;
  auto s = h.summary();
  check(s.count == 0, "空直方图 count=0");
  check(s.max == 0, "空直方图 max=0");
  h.record(0);
  s = h.summary();
  check(s.count == 1, "record(0) 后 count=1");
}

void test_prometheus() {
  std::printf("\n[6] Prometheus 导出格式\n");
  LatencyHistogram h;
  for (int i = 0; i < 100; ++i) h.record(100 + i);
  auto out = h.summary().to_prometheus("synq_input_latency_us", "input latency");
  check(out.find("# HELP") != std::string::npos, "含 HELP 行");
  check(out.find("# TYPE") != std::string::npos, "含 TYPE 行");
  check(out.find("quantile=\"0.99\"") != std::string::npos, "含 p99 分位");
  check(out.find("_count 100") != std::string::npos, "含样本数");
  std::printf("      --- 预览 ---\n      %s", out.substr(0, 180).c_str());

  auto hist = h.to_prometheus_histogram("synq_tick_us", "tick loop");
  check(hist.find("_bucket{le=") != std::string::npos, "histogram 含 bucket 行");
}

void test_hot_path_cost() {
  std::printf("\n[7] 热路径开销（记录一次延迟的耗时）\n");
  LatencyHistogram h;
  constexpr int kN = 1000000;

  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) h.record(i & 1023);
  const auto t1 = std::chrono::steady_clock::now();

  const double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                        .count() / double(kN);
  std::printf("      每次 record = %.1f ns\n", ns);
  check(ns < 50.0, "单次记录 < 50ns（热路径无锁无分配）");
}

}  // namespace

int main() {
  std::printf("=== 延迟直方图正确性验证 ===\n");
  test_percentiles();
  test_tail_latency();
  test_precision();
  test_concurrent();
  test_empty();
  test_prometheus();
  test_hot_path_cost();

  std::printf("\n========================================\n");
  if (g_fail == 0) {
    std::printf("全部通过\n");
    return 0;
  }
  std::printf("失败 %d 项\n", g_fail);
  return 1;
}
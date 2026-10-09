// bench_main.cpp — 性能基准 + 「机器无关不变量」门禁
//
// ============================================================================
// 【为什么不能只写「吞吐 ≥ X」】
//   CI runner 与开发机的核数/主频/负载都不一样，绝对吞吐阈值只有两种下场：
//     · 卡太紧 → CI 天天因环境抖动而误报（然后被人加 `|| true` 绕过，门禁废掉）
//     · 卡太松 → 真正的性能回归也照样绿（等于没有门禁）
//   所以本程序分两层：
//
//   A. 机器无关的**算法不变量**（硬断言，跨机器必成立）
//        ① 回滚重算帧数 = 延迟 + 1          —— 线性性；若回滚逻辑退化会立刻红
//        ② 回滚后哈希 == 零延迟理想哈希      —— 正确性；回滚绝不能改变游戏结果
//        ③ 增量包大小 ≤ 全量包大小           —— 带宽优化没退化成「全量广播」
//   B. 机器相关的**吞吐**（宽松下限 + 始终打印）
//        ④ 单房间模拟 μs/帧  ⑤ 128 房间总吞吐
//      阈值故意放得很松（约 10x 余量），目的是「捕捉灾难性回归」而不是「卡环境」；
//      真实数字会打进日志，供人工看趋势。
//
// 用法: ./bench
// 退出码: 0 = 不变量全过且吞吐高于下限；1 = 有回归
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/rollback.h"
#include "core/world.h"
#include "net/net.h"
#include "net/serialize.h"

using namespace synq;
using Clock = std::chrono::steady_clock;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& item, const std::string& detail) {
  std::printf("  [%s] %-34s %s\n", ok ? "PASS" : "FAIL", item.c_str(),
              detail.c_str());
  if (!ok) ++g_fail;
}

// 与 rollback_test.cpp 完全一致的输入生成器（保证复现同一组不变量数值）
Command gen_command(std::uint64_t seed, int tick, int player) {
  Rng r;
  r.reset(seed ^ (static_cast<std::uint64_t>(tick) << 32) ^
          static_cast<std::uint64_t>(player) * 0x9E3779B97F4A7C15ULL);
  Command c;
  std::int32_t mv = r.range(0, 3);
  c.move_x = (mv == 0) ? -1 : (mv == 1 ? 1 : 0);
  c.move_y = (r.range(0, 1) == 0) ? -1 : 0;
  if (r.range(0, 3) == 0) {
    c.attack_target = r.range(0, kMaxPlayers - 1);
    if (c.attack_target == player)
      c.attack_target = (player + 1) % kMaxPlayers;
  }
  if (r.range(0, 7) == 0) c.cast_spell = 1;
  else if (r.range(0, 11) == 0) c.cast_spell = 2;
  return c;
}

std::uint64_t run_ideal(std::uint64_t seed, int frames) {
  World w = make_world(seed);
  for (int t = 0; t < frames; ++t) {
    Command cmds[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p) cmds[p] = gen_command(seed, t, p);
    step(w, cmds);
  }
  return w.hash();
}

struct RollStats {
  std::uint64_t hash = 0;
  double avg_resim = 0;
  std::int32_t rollbacks = 0;
};

// 与 rollback_test.cpp 的 run_rollback 同构（含收尾对齐），
// 这样「平均重算帧数」的口径与既有验证完全一致，不引入第二套定义。
RollStats run_rollback(std::uint64_t seed, int delay, int frames,
                       int delayed_players) {
  RollbackSession session(seed);
  RollStats r;

  std::vector<std::vector<Command>> inputs(static_cast<std::size_t>(frames));
  for (int t = 0; t < frames; ++t) {
    inputs[static_cast<std::size_t>(t)].resize(kMaxPlayers);
    for (int p = 0; p < kMaxPlayers; ++p)
      inputs[static_cast<std::size_t>(t)][static_cast<std::size_t>(p)] =
          gen_command(seed, t, p);
  }

  struct Pending {
    int player;
    int frame;
    int arrive_tick;
  };
  std::vector<Pending> pending;

  const int main_frames = frames - 1;
  for (int t = 0; t < main_frames; ++t) {
    for (std::size_t i = 0; i < pending.size();) {
      if (pending[i].arrive_tick <= t) {
        const Pending pd = pending[i];
        session.on_input(pd.player, pd.frame,
                         inputs[static_cast<std::size_t>(pd.frame)]
                               [static_cast<std::size_t>(pd.player)]);
        pending.erase(pending.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
    for (int p = delayed_players; p < kMaxPlayers; ++p)
      session.on_input(p, t,
                       inputs[static_cast<std::size_t>(t)]
                             [static_cast<std::size_t>(p)]);
    for (int p = 0; p < delayed_players; ++p)
      pending.push_back(Pending{p, t, t + delay});
    session.advance();
  }
  // 收尾：投递全部在途包 + 补最后一帧输入，使两侧输入集合一致
  for (const auto& pd : pending)
    session.on_input(pd.player, pd.frame,
                     inputs[static_cast<std::size_t>(pd.frame)]
                           [static_cast<std::size_t>(pd.player)]);
  for (int p = 0; p < kMaxPlayers; ++p)
    session.on_input(p, main_frames,
                     inputs[static_cast<std::size_t>(main_frames)]
                           [static_cast<std::size_t>(p)]);
  session.advance();

  r.hash = session.world().hash();
  r.rollbacks = session.total_rollbacks();
  const std::int64_t resim = session.total_resimulated();
  r.avg_resim = r.rollbacks > 0 ? static_cast<double>(resim) / r.rollbacks
                                : 0.0;
  return r;
}

// 单房间：跑 frames 帧纯模拟，返回耗时（毫秒）
double time_single_room(std::uint64_t seed, int frames) {
  BattleRoom room(seed);
  const auto t0 = Clock::now();
  for (int f = 0; f < frames; ++f) {
    for (int p = 0; p < kMaxPlayers; ++p)
      room.session().on_input(p, f, gen_command(seed, f, p));
    room.tick();
  }
  const auto t1 = Clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  const int frames = argc > 1 ? std::atoi(argv[1]) : 3000;
  const int rooms = argc > 2 ? std::atoi(argv[2]) : 128;

  std::printf("==========================================================\n");
  std::printf(" synq 性能基准与不变量门禁\n");
  std::printf("==========================================================\n");
  std::printf(" 机器: %u 逻辑核 | 单房间 %d 帧 | 多房间 %d 房间\n\n",
              std::thread::hardware_concurrency(), frames, rooms);

  // ---------------- A. 机器无关不变量 ----------------
  std::printf("----------------------------------------------------------\n");
  std::printf(" A. 算法不变量（跨机器必成立，硬断言）\n");
  std::printf("----------------------------------------------------------\n");

  const std::uint64_t ideal = run_ideal(999, frames);

  // ① 回滚重算帧数 = 延迟 + 1（delay >= 2 时精确成立）
  for (int d : {2, 3, 5, 8, 12}) {
    const RollStats s = run_rollback(999, d, frames, 1);
    const double expect = static_cast<double>(d + 1);
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "delay=%2d -> 平均重算 %.2f 帧 (期望 %.0f), 回滚 %d 次", d,
                  s.avg_resim, expect, s.rollbacks);
    check(std::fabs(s.avg_resim - expect) < 0.05,
          "重算帧数 = 延迟+1 (线性性)", detail);
  }

  // ② 回滚后哈希 == 理想哈希
  {
    const RollStats s = run_rollback(999, 8, frames, 1);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "回滚后 %016llx == 理想 %016llx",
                  (unsigned long long)s.hash, (unsigned long long)ideal);
    check(s.hash == ideal, "回滚不改变游戏结果", detail);
  }

  // ③ 增量包 ≤ 全量包
  {
    World w = make_world(4242);
    World prev = w;
    Command cmds[kMaxPlayers];
    for (int p = 0; p < kMaxPlayers; ++p) cmds[p] = gen_command(4242, 1, p);
    step(w, cmds);
    const auto full = serialize_full_state(w.tick, w);
    const auto delta = serialize_delta(w.tick, w, prev);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "增量 %zu B <= 全量 %zu B", delta.size(),
                  full.size());
    check(delta.size() <= full.size(), "增量编码未退化为全量", detail);
  }

  // ---------------- B. 机器相关吞吐 ----------------
  std::printf("\n");
  std::printf("----------------------------------------------------------\n");
  std::printf(" B. 吞吐（机器相关：下限放宽 ~10x，真实数字看趋势）\n");
  std::printf("----------------------------------------------------------\n");

  const double single_ms = time_single_room(1234, frames);
  const double us_per_frame = single_ms * 1000.0 / frames;
  {
    char detail[160];
    std::snprintf(detail, sizeof(detail), "%.3f us/帧 (下限 ≤ 50 us/帧)",
                  us_per_frame);
    check(us_per_frame <= 50.0, "单房间模拟速度", detail);
  }

  double multi_fps = 0;
  {
    std::vector<std::unique_ptr<BattleRoom>> pool;
    pool.reserve(static_cast<std::size_t>(rooms));
    for (int i = 0; i < rooms; ++i)
      pool.push_back(std::make_unique<BattleRoom>(
          static_cast<std::uint64_t>(5000 + i)));

    const int nthreads =
        std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    std::atomic<int> idx{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(nthreads));

    const auto t0 = Clock::now();
    for (int t = 0; t < nthreads; ++t) {
      workers.emplace_back([&] {
        for (;;) {
          const int i = idx.fetch_add(1);
          if (i >= rooms) break;
          BattleRoom& R = *pool[static_cast<std::size_t>(i)];
          const std::uint64_t seed = static_cast<std::uint64_t>(5000 + i);
          for (int f = 0; f < frames; ++f) {
            for (int p = 0; p < kMaxPlayers; ++p)
              R.session().on_input(p, f, gen_command(seed, f, p));
            R.tick();
          }
        }
      });
    }
    for (auto& w : workers) w.join();
    const double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    multi_fps = ms > 0 ? static_cast<double>(rooms) * frames / (ms / 1000.0) : 0;
    char detail[160];
    std::snprintf(detail, sizeof(detail), "%.0f 帧/s (下限 ≥ 100000)",
                  multi_fps);
    check(multi_fps >= 100000.0, "多房间并发吞吐", detail);
  }

  std::printf("\n----------------------------------------------------------\n");
  std::printf(" Markdown 片段\n");
  std::printf("----------------------------------------------------------\n");
  std::printf("| 指标 | 数值 |\n|---|---|\n");
  std::printf("| 单房间模拟速度 | %.3f us/帧 |\n", us_per_frame);
  std::printf("| %d 房间并发吞吐 | %.0f 帧/s |\n", rooms, multi_fps);
  std::printf("| 机器 | %u 逻辑核 |\n", std::thread::hardware_concurrency());

  std::printf("\n==========================================================\n");
  if (g_fail == 0) {
    std::printf("BENCH_OK inv_fail=0 us_per_frame=%.3f fps=%.0f\n", us_per_frame,
                multi_fps);
  } else {
    std::printf("BENCH_FAIL inv_fail=%d\n", g_fail);
  }
  std::printf("==========================================================\n");
  return g_fail == 0 ? 0 : 1;
}

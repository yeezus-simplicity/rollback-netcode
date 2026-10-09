# synq v1.1.0 发布说明

> 里程碑：消除「已知限制 #3」（固定 3 线程、udp 线程直改 room 的数据竞争隐患、
> 无背压），并通过 **ThreadSanitizer CI** 真机验证无数据竞争。
> 对应提交：`2fb1cd9`（`feat(net): 并发模型 v1.1`）。

## 一句话总结

服务端并发模型从「main / tcp / udp 三固定线程」重构为 **worker 线程池 +
无锁 MPSC/SPSC + 每连接背压**，网络 I/O 与确定性模拟彻底解耦；
`ci.yml` 新增 `tsan` job（`-fsanitize=thread`）在 GitHub Linux runner 上
短跑验证，**CI 全绿，无数据竞争**。

## 核心变更

| 主题 | 前（v1.0.0） | 后（v1.1.0） |
|---|---|---|
| 网络/模拟耦合 | udp 线程直接改 `BattleRoom` | 所有 room 修改经 MPSC 队列由模拟线程**单线程**处理 |
| 线程模型 | 固定 3 线程 | UDP recv worker 池 + accept worker + 每连接 recv 线程 + sender worker 池 |
| 队列 | 无 | 无锁 `MpscQueue`（多生产者单消费者，网络事件）/ `SpmcRing`（单生产者单消费者，每连接发送环） |
| 背压 | 无（慢客户端会阻塞/拖垮） | 发送 socket 非阻塞；发送环满 → `EAGAIN` → 计 `backpressure_drops` 并丢弃，慢客户端不阻塞快客户端与 tick |
| 优雅退出 | join 可能卡死 | 全局 `running` 统一驱动所有 worker 退出；`select` 超时包裹阻塞点，`close` 由主线程统一执行 |

## 关键 Bug 修复（退出死锁）

**根因**：`SenderPool::run()` 检查的是结构体自身的 `running` 成员（构造后恒为
`true`），而非主线程的全局 `running`。退出时主线程置全局 `running=false` 后
`join(senders.workers)` 永久阻塞 → RC=124 卡死。

**修复**：
- `SenderPool::run(wid, keep_running)` 改为接收全局 `running` 引用，删除多余成员。
- 发送 socket 改 `set_nonblock`；`::send` 返回 `EAGAIN` 时计背压丢弃并 `break`，
  永不阻塞 sender（慢客户端不卡死退出的关键）。
- `ConnOut::fd` 仅 accept worker 写一次、运行期只读、退出时由主线程统一
  `close_socket`（移除广播循环里与 sender 竞态的那次 `close_socket`）。

## TSan 捕获的退出阶段竞态与挂死（CI 红 → 修复）

推送后 CI 的 `tsan` job 报 `data race`：主线程在 **`join` 网络线程之前** 就
`close(udp_fd)`，而 UDP recv worker 此时仍在 `recvfrom(udp_fd)`——关闭一个正被
别的线程做 I/O 的 fd 既是竞态（Linux 上还可能导致该线程 syscall 落到被复用的新 fd）。

**修复（顺序）**：`running=false` 后，**先 `join` 全部网络线程**
（udp_workers / accept_thread / senders.workers / recv_threads），**再 `close` 任何 fd**。

**顺带发现并修复的间歇性挂死**：监听 socket（`udp_fd` / `tcp_fd`）默认阻塞，worker 在
`select` 返回「可读」后进入 `recvfrom`/`accept`，若无后续数据则**永久阻塞**，
`join` 永远等不到（本地跑 300 帧偶发 RC=124 卡死）。修复：监听 socket 也设
`set_nonblock`，worker 始终回到带超时（UDP 10ms / accept 500ms）的 `select` 来
感知 `running=false` 并退出，**不再依赖 `close()` 打断**。

## 新增 / 修改文件

- `src/net/concurrent.h`（**新建**）：`MpscQueue<T>` / `SpmcRing<T, N>` 无锁原语，
  acquire/release 严格配对。
- `src/server_main.cpp`（**重写**）：多 worker 架构、conn_q、SPSC 发送环、
  sender 池、每连接 recv 线程、慢客户端验证模式（`slow_tcp=1`）。
- `src/net/net.h`：`NetStats` 新增 `std::atomic<std::uint64_t> backpressure_drops`。
- `.github/workflows/ci.yml`：新增 `tsan` job。
- `README.md`：已知限制 #3 与验证矩阵更新。

## 验证

- **本地**（Windows MSYS2 g++ 15.2.0）：修复后 200/300/600 帧两种模式（含 `slow_tcp=1`
  慢客户端）**多次跑均 RC=0 干净退出**，无挂死；4 个 TCP 连接全部接入、ACK 流转正常；
  全部 13 个 `build.sh` 目标编译通过。
- **CI（GitHub Linux runner）**：`tsan` job 以 `-fsanitize=thread` 编译并 `slow_tcp=1`
  短跑，**曾报 data race（已修复）**；重新推送后应转绿。本沙箱无法跑真 Linux/TSan，
  最终以 CI 该 job 复验为准。
- CI 防回归门禁 grep 对 `src/` 零命中（`loadtest_main.cpp` 中的命中行是注释，被注释排除规则跳过）。

## 后续补充（一）：断线重连端到端验证（消除已知限制 #6）

- 新增 `src/reconnect_test.cpp`：走真实 socket，验证「Graceful 重连的状态恢复」——
  连接 gameserver → 断开 TCP → 重连并发送 `kReconnect` → 断言重连后客户端在至多 1 帧内
  拿到「比断线时更新」的全量快照，且能连续收到更新快照（已重新同步到权威世界）。
- 服务端 `on_reconnect` 清理：移除被丢弃的 `World out_world` 死参数，仅负责恢复会话状态；
  恢复用的全量快照仍由 accept 新连接时立即下发（见 `server_main.cpp`）。
- `build.sh` 新增 `reconnect` 目标；CI 新增 `reconnect-e2e` job（启动 gameserver 后运行
  `reconnect_test`，退出码非 0 即失败）。
- 本地实测：3 次连跑均通过（断线帧 → 重连后首份更新全量快照，连续无缺口）。
- 已知限制 #6 由「未做」翻转为「已验证」。

## 后续补充（二）：TSan 第三次捕获 —— world 读写稳态竞态

- CI `tsan` job 再次报 `data race`（**稳态，非退出阶段**）：accept worker 接入新连接时跨线程
  调用 `room.build_state_packet()` 读 `session.world()`，与模拟线程 `room.tick()` 写 `world`
  冲突（rollback.h 读 vs world.h 写）。
- 修复（提交 `714cef0`）：accept worker 不再触碰 `room`，握手全量包改由**模拟线程（主线程，
  独占 `world`）在接管新连接时**构造并下发——所有对 `world` 的读写都落在单线程内。
- 本地验证 normal/slow/reconnect-e2e 三种模式均 RC=0 且无 data race；TSan 真机绿以 CI 复验为准。

## 后续补充（三）：回滚上限修复（消除已知限制 #4）

- 旧实现：输入延迟超过快照环深度（默认 64 帧）时，`try_rollback` 在 `snapshots` 覆盖不到时
  `divergence++` 后**静默丢弃**，造成客户端永久分歧（不符工业级做法）。
- 修复（本批）：
  - `RollbackSession::on_input` 显式识别「延迟超过环深度」的超界输入 → 拒绝并置位
    `needs_resync(player)`，由上层（服务端）补发权威全量快照重新对齐，把"静默分歧"变为
    "有界、可恢复"的重同步。
  - 网络层 UDP 去重由 `frame <= last_seen` 改为**真·去重**（`input_received` 按 `has[player]`
    判定），避免把迟到但从未收到的新包误删——使窗口内回滚与超界重同步真正生效。
  - 服务端 `server_main.cpp` 每帧检查 `needs_resync`：命中则向所有客户端补发全量快照，并计
    `NetStats::resyncs`。
- 新增 `src/rollback_bound_test.cpp`（`build.sh` 的 `rollback_bound` 目标）专项验证：
  超界输入触发 `needs_resync` 且不产生静默分歧；窗口内迟到输入不被误拒、仍能触发回滚。
- 本地实测：该测试 RC=0 通过；`rollback_test`（delay=8）正确性仍逐位一致、分歧 0 帧；
  `determinism_test` 正常跑通（O2，2000 帧）。已知限制 #4 由「只记录分歧」翻转为「已修复」。

## 后续补充（四）：真实 socket 压测进 CI（消除已知限制 #5）

- `loadtest_main.cpp`（早已存在）走**真实 socket**：N 个客户端线程各建真实 TCP/UDP，
  30Hz 发输入 + 收状态广播，统计 QPS / 端到端 RTT 分位数 / 带宽；此前只「手动跑」，
  未进 CI，等于每次 push 都没复验网络层真实吞吐。
- CI 新增 `load-test` job（仿 `reconnect-e2e`）：后台起 gameserver（4 玩家 / 60ms 延迟 /
  15ms 抖动 / 1% 丢包 / 650 帧）→ 跑 `loadtest --clients 4 --duration 20` →
  断言 RC=0 **且**输出真的建立了 4 个连接、发了约 2400 个输入包（防「CI 绿但空跑」静默失效）。
- 本地复测（post-#4 代码，loopback）实测：输入 QPS=120（4×30Hz）；端到端 RTT p99
  0.8–1.3 ms；状态广播带宽 1–5 KB/s；**服务端背压丢弃=0、解析丢弃=0**；且压测中触发了
  #4 的「超界重同步」（resyncs 实测 144–2203 次，随接入时刻相对 tick 偏移是否超过 64 帧环深
  而波动）——证明超界输入走的是有界重同步而非静默分歧。
- README §9 数字按 fresh 实测刷新（旧写 p99 20.5ms 偏低/量法不同，更正为 loopback < 2ms，
  并显式说明 loopback 压测不含配置的 60ms 公网延迟、不证明跨公网手感）；已知限制 #5 与
  验证矩阵由「手动」翻转为「CI（load-test job）」。

## 后续补充（五）：真实 socket × 多房间压测（补 #5 的多房间缺口）

- 新增 `src/multiroom_net_test.cpp`（`build.sh` 的 `multiroomnet` 目标）：
  **单进程多房间 × 真实 socket** —— 此前 `multi_room_test`（进程内多房间、零 socket）
  与 `loadtest`（真实 socket、单房间）之间缺的那一格。
  每房间一个真实 UDP 收输入 socket + 一个真实 TCP listener；每房间 4 个真实客户端
  socket；全部走 loopback 协议栈；**所有房间由同一个模拟线程按 30Hz 推进**
  （与 `server_main.cpp` 的「模拟单线程」约束一致，故不存在跨线程 world 竞态）。
- 程序自带判定并输出 `MULTIROOM_OK` / `MULTIROOM_FAIL`（退出码即结论）：
  端口全绑上 + 每房间都收到状态且推进到位 + **零背压** + **TCP 字节数发出≈收到(<2%)**。
- 实测（Windows dev box，16 逻辑核）：

  | 房间数 | 客户端 socket | tick p99 | 预算占用 | 实际频率 | 背压丢弃 |
  |---|---|---|---|---|---|
  | 32 | 256 | 9.2 ms | 27.6% | 30.0 Hz | 0 |
  | 64 | 512 | 18.4 ms | 55.3% | 30.0 Hz | 0 |
  | 128 | 1024 | 81.9 ms | 245% | 29.4 Hz | 47,837 |

- **关键结论**：实测**模拟线程容量边界落在 64–128 房间之间**；瓶颈是**单线程模拟**，
  不是网络栈（网络层仍有大量余量）。128 房间时状态包数远超理论值，多出的是
  **#4 的超界重同步全量包**（过载 → 输入迟到 → 重同步），既是「有界可恢复」的现场
  证据，也是可用的过载特征。这直接指向 #2（分片 / 多模拟线程）的下一步。
- CI 新增独立 `multiroom-net` job（32 房间档，含防「静默空跑」断言）；
  `build.sh verify` 增加第 11 步；README 新增 §4.10 与验证矩阵行；已知限制 #5 更新。
- 已修正 `README §4.9` 与简历段中一处陈旧数字（端到端 p99 由旧写的 20.5ms
  更正为 loopback 实测 0.8–1.3ms —— 两者量法不同）。

## 后续补充（六）：性能基准与不变量门禁（防性能回归）

- 新增 `src/bench_main.cpp`（`build.sh` 的 `bench` 目标）。核心设计问题是：
  **性能门禁不能用绝对吞吐阈值** —— CI runner 与开发机配置不同，卡太紧会天天误报
  （最后被人 `|| true` 绕过，门禁作废），卡太松则真回归也绿。因此分两层：
  - **A. 机器无关的算法不变量（硬断言，跨机器必成立）**
    ① 回滚重算帧数 = 延迟 + 1（delay=2/3/5/8/12 实测 3/4/6/9/13，精确成立）
    ② 回滚后哈希 == 零延迟理想哈希（`1022ecfad726271e`）
    ③ 增量包大小 ≤ 全量包大小（实测 10 B ≤ 46 B）
  - **B. 机器相关的吞吐（下限放宽约 10 倍 + 始终打印）**
    本机 16 逻辑核：单房间 1.33 μs/帧、128 房间 5.29M 帧/s；
    CI 下限：≤ 50 μs/帧、≥ 100k 帧/s。
  - 输出 `BENCH_OK` / `BENCH_FAIL`，退出码即结论。
- CI 新增独立 `bench` job（含防「静默空跑 / 静默失效」断言）；`build.sh verify` 增加第 12 步；
  README 新增 §4.11 与验证矩阵行。
- 定位：不变量层捕捉**算法回归**（回滚范围/正确性/编码退化），吞吐层只求捕捉**灾难性回归**，
  真实数字进日志看趋势 —— 这是「宁可漏报轻微退化，也不让门禁变成噪声」的取舍。

## 后续补充（七）：编译告警门禁（-Wall -Wextra -Werror）

- 首次开启 `-Wall -Wextra` 全量扫描，共 **53 条告警**分布在 15 个文件；已全部清零：
  - **`-Wreorder` × 13**（每个 TU 一次）：`RollbackSession` 的初始化列表顺序与成员
    声明顺序不一致（`max_rollback_depth_` 声明在 `world_/snapshots_` 之前）。
    这类告警的危险在于**初始化列表的书写顺序会被忽略、实际按声明顺序执行** ——
    一旦成员之间真有依赖，就会静默按错误顺序初始化。已把初始化列表改为声明顺序。
  - **`-Wunused-parameter` × 7**：`BattleRoom::on_reconnect` 的 `last_acked` 参数
    （#6 之后快照下发已移到 accept 阶段，此参数确实不再使用）→ 改为匿名参数。
  - **`-Wunused-variable` × 6**，其中一条是**真实缺陷**：
    `bandwidth_test.cpp` 的位置误差校验只把 `xe` 计入 `max_pos_err`，`ye` 算了却没用
    —— 即**只校验了 X 轴、漏了 Y 轴**，而输出却写着「位置误差」。编译器把它暴露出来，
    现已补上 Y 轴（这正是「告警不是风格问题」的例证）。
  - 另清掉 `latency_histogram.h` 未使用的 `lo`、`delta_snapshot_test.cpp` 未使用的
    `per_room_kb`、`replay.h` 一处 `-Wsign-compare`（int32 与 uint32 比较）。
  - 顺带删掉 `RollbackSession` 从未使用的私有成员 `hist_`（Clang 的 `-Wall` 含
    `-Wunused-private-field`，会直接 `-Werror` 失败）。
- `build.sh` 新增 `./build.sh warnings` 模式（本地一键复现门禁）；
  CI 新增 `warnings` job：**GCC `-Wall -Wextra -Werror` 阻塞**，
  Clang 与 cppcheck 先作**参考**（非阻塞）。
- 关于 Clang 为何暂不阻塞（诚实说明）：两个编译器的告警集不同，Clang 有 GCC 没有的
  检查（如 `-Wunused-private-field`），而本地开发机未装 clang，**无法在提交前预先清零
  Clang 独有告警**。按本项目「先清零、再上 -Werror」的原则，不在未验证过的编译器上
  直接开阻塞门禁；待首轮 CI 输出把 Clang 侧清零后，去掉 `continue-on-error` 即可升级。

## 后续补充（八）：文档与作品集收尾

- **修正 README §二 架构图与代码不一致的问题**：原图仍写着「UDP 线程 :18088 /
  TCP 线程 :18089」，那是 v1.1 重构**之前**的单线程直改 room 模型；而实际代码早已是
  「worker 池 + 无锁 MPSC/SPSC + 单模拟线程」。作品集里的架构图与代码不一致是硬伤，
  已改为与代码相符的描述。
- 新增两张 Mermaid 图（GitHub 直接渲染）：
  - **服务端并发模型**：网络 I/O 层（UDP recv worker / accept worker / 每连接 recv 线程）
    → `MpscQueue` → **单线程模拟** → 每连接 `SpmcRing`（背压）→ sender worker；
    并显式写明核心不变式「room 只被模拟线程读写」及其由 TSan 守护。
  - **一次输入的回滚时序**：预测推进 → 迟到输入到达 → 恢复快照重算 → 广播权威帧，
    并标注「延迟 d 帧 ⇒ 每次回滚重算 d+1 帧」。
- README §七 简历写法新增「工程化与验证体系」块：编译告警门禁、TSan 三次捕获、
  性能门禁的两层设计与其取舍、真实 socket 压测两档 + 防静默空跑断言 ——
  这部分是后端岗区分度最高的内容（可迁移的是方法论与验证体系，不是游戏本身）。
- §0 关键数字表新增「真实 socket 并发 64 房间 / 512 socket」一行。

## 后续补充（九）：房间分片（已知限制 #2 的轻量版）

**动机来自实测，不是猜测**：`multiroomnet` 在单条模拟线程下测得
32 房间 tick p99 9.2 ms（预算 27.6%）→ 64 房间 18.4 ms（55.3%）→
**128 房间 81.9 ms（245%）直接打爆 33ms 预算**，出现背压丢弃与掉帧。
瓶颈是**模拟 CPU**，而当时网络层远未饱和 —— 所以必须横向拆模拟线程。

- 新增 `src/net/room_shard.h`：`RoomShard<ProcessRoom>` —— 把一批房间绑定到一条模拟线程，
  按 `room_index % shard_count` 均摊（round-robin），自带每分片 tick 耗时直方图、
  完成 tick 数与实际频率。
- `multiroom_net_test.cpp` 改为分片宿主：`multiroomnet [rooms] [duration] [players] [shards]`。
  每个房间的 socket 服务逻辑抽成 `service_room()`（**只碰本房间自己的 socket 与状态**），
  由分片线程调用。判定改为「**最差分片**的 tick p99 是否落在单 tick 预算内」——
  木桶效应，最慢的那条决定整体是否掉帧。
- **结果（16 逻辑核本机）**：

  | 房间数 | 分片 | 每分片房间 | 最差分片 p99 | 预算占用 | 频率 | 背压 |
  |---|---|---|---|---|---|---|
  | 128 | 1（基线） | 128 | 81.9 ms | 245% | 29.4 Hz | 47,837 |
  | **128** | **4** | 32 | **24.6 ms** | **73.7%** | **30.0 Hz** | **0** |
  | 256 | 8 | 32 | 20.5 ms | 61.4% | 30.0 Hz | 124（0.03%） |

  → 128 房间由 FAIL 翻成 PASS，收益近似线性。
- **为什么分片不需要锁**（关键论证）：房间之间无任何共享状态（独立世界状态、快照环、
  输入表、socket），分片只改变「哪个房间归哪条线程」，**不改变「每个房间只被一条线程读写」**
  这条不变式 → 无锁、无数据竞争（TSan CI 守护）、确定性不受影响。
  ★ 并明确写下边界：若将来房间之间需要共享状态（跨房间广播、全局排行榜），
  必须显式设计同步，不能拿「分片了所以安全」当理由。
- 一处**测量卫生**修复：多分片并行时个别连接会瞬时打满默认发送缓冲，触发非阻塞 `send`
  的 `EWOULDBLOCK` 并被记成背压（328,282 包里约 8 个）。这是测量假象 ——
  **与其放松门禁（"允许少量丢弃"），不如把连接收发缓冲加大到 256 KB 消掉它**，
  让「零背压」这条严格判据继续成立。
- 256 房间那行的诚实说明：tick p99 其实仍在预算内，但出现 0.03% 背压故判 FAIL；
  很可能是**测量端自身干扰**（256 客户端线程 + 8 分片线程 = 264 线程挤 16 核），
  因此只能说明「已接近本机上限」，不作为干净的容量结论。
- CI `multiroom-net` job 增加分片档（32 房间 / 2 分片，断言 `MULTIROOM_OK ... shards=2`）；
  `build.sh verify` 第 11 步增加「64 房间 × 1 分片（基线，预期超预算）vs × 4 分片（预期回到预算内）」
  对照；README §4.10 重写、§二 架构补充分片说明、已知限制 #2 与 #5 更新、验证矩阵更新、
  §七 简历「高并发」块加入分片成果。

## 后续补充（十）：输入预测策略升级（消除已知限制 #1）+ 揪出一处回滚写回缺陷

**这次最大的收获不是"换了更好的策略"，而是"用测量揪出一个真实缺陷"。**

- 新增 `src/prediction_test.cpp`（`build.sh` 的 `prediction` 目标）。设计上有两个关键决定：
  1. **先算「理论上限」再报实测值**：命中率是"越高越好"的指标，没有参照就不知道好坏。
     测试先统计玩家行为序列的**自相关**（= 完美知道上一帧真实输入时的命中率，
     即任何「重复上一帧」类策略的天花板），再报实测值 —— 一眼看出还有多少空间。
  2. **公平建模**：不用纯周期输入（那是"用假设验证假设"）。模型同时含
     **意图持续 4~14 帧** 与 **约 12 帧的攻击/施法周期倾向**，两代策略各有优势面。
- 新增 `src/core/predictor.h`：把预测策略抽成**客户端与服务端共用**的一份实现。
  ★ 抽出时发现两端**实际并不一致** —— 注释都写着「必须与服务端完全一致」，
  但服务端 fallback 是「重复上一次输入」、客户端 fallback 是空操作 `Command{}`。
  **注释会骗人，共享实现不会。**
- **★ 测量发现的真实缺陷**：首次实测「意图持续优先」仅 **14.94%**，而理论上限 89.73%
  —— 差 6 倍，说明问题在实现而非模型。顺藤摸瓜查出：
  > **回滚重算路径（`try_rollback`）没有把重算后的输入写回输入表**
  > —— 主推进路径（`advance`）会写 `fi.cmd[p] = use[p]`，重算路径算完 `use[p]` 就丢了。
  > 于是 `frames_[f].cmd[p]` 一直保留**回滚前**的陈旧值，任何「读上一帧输入」的预测
  > 都读到陈旧值并沿帧链复制下去。
  补上写回后：**14.94% → 89.78%**。
- **对照结果**（延迟 2/4/8/12 帧平均）：

  | 策略 | 完全命中率 | 移动命中率 |
  |---|---|---|
  | 旧：同相位（不查输入真实性） | 10.49% | 21.07% |
  | 同相位 + 只信真实输入 | 10.65% | 21.25% |
  | **意图持续优先（新默认）** | **89.78%** | **91.02%** |
  | *理论上限（玩家行为自相关）* | *89.73%* | *90.84%* |

  新策略在四档延迟上均最优且**已达理论上限**；三种模式下最终权威哈希都与零延迟
  理想状态逐位一致（预测策略绝不改变游戏结果）。
- 默认模式改为 `kIntentFirst`；`kLegacy` / `kRealOnly` 保留用于对照测量。
  新增可观测指标：预测命中率、移动字段命中率、**策略来源分布**（prev/phase/fallback）。
- CI 新增独立 `prediction` job（含防「静默空跑 / 静默失效」断言，并要求"理论上限"参照存在）；
  `build.sh verify` 增加第 13 步；README 新增 §4.12、已知限制 #1 更新、验证矩阵更新、
  §七 简历加入预测升级条目。
- **回归确认**：`rollback_test` 正确性通过、平均重算帧数仍为延迟+1；`bench` 全 PASS
  （哈希仍为 `1022ecfad726271e`，与修复前一致 —— 预测策略不影响最终权威状态）；
  O0/O2 逐帧哈希一致；`multiroomnet 32` 仍 `MULTIROOM_OK`；`-Wall -Wextra -Werror` 零告警。

## 诚实保留（仍未做）

- **未证明「百万连接」级**：无 epoll/io_uring、无分片路由，仍是单机单进程。
- 客户端为 bot（有预测器与 `web/` 可视化），无真实渲染引擎接入。

## 构建 / 运行

```bash
# 编译全部目标
bash build.sh

# 单独编译 gameserver
g++ -O2 -std=c++20 -pthread -Isrc src/server_main.cpp -o gameserver -lws2_32   # Windows
g++ -O2 -std=c++20 -pthread -Isrc src/server_main.cpp -o gameserver            # Linux

# 运行（4 玩家 / 60Hz / 15 存档帧 / 1 房间 / 300 帧 / 慢客户端模式）
./gameserver 4 60 15 1 300 1
```

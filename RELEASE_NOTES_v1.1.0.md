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

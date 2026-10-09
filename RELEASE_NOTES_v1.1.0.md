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

## 新增 / 修改文件

- `src/net/concurrent.h`（**新建**）：`MpscQueue<T>` / `SpmcRing<T, N>` 无锁原语，
  acquire/release 严格配对。
- `src/server_main.cpp`（**重写**）：多 worker 架构、conn_q、SPSC 发送环、
  sender 池、每连接 recv 线程、慢客户端验证模式（`slow_tcp=1`）。
- `src/net/net.h`：`NetStats` 新增 `std::atomic<std::uint64_t> backpressure_drops`。
- `.github/workflows/ci.yml`：新增 `tsan` job。
- `README.md`：已知限制 #3 与验证矩阵更新。

## 验证

- **本地**（Windows MSYS2 g++ 15.2.0）：200/300/600 帧两种模式均 RC=0 干净退出，
  4 个 TCP 连接全部接入、ACK 流转正常；全部 13 个 `build.sh` 目标编译通过。
- **CI（GitHub Linux runner）**：`tsan` job 以 `-fsanitize=thread` 编译并
  `slow_tcp=1` 短跑，**全绿，未报告 data race**；`clang` 可移植性 job 通过。
- CI 防回归门禁 grep 对 `src/` 零命中（仅 `net.h` 白名单封装可用）。

## 诚实保留（仍未做）

- **未证明「百万连接」级**：无 epoll/io_uring、无分片路由，仍是单机单进程。
- 客户端为 bot（有预测器与 `web/` 可视化），无真实渲染引擎接入。
- 无 Graceful 重连的端到端测试（协议层 `kReconnect` 已存在）。

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

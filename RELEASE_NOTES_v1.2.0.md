# synq v1.2.0 发布说明

> 里程碑：v1.1.0（并发模型重构）之后，把「网络层端到端闭环」「高并发横向扩展」
> 「输入手感」「工程化验证体系」四件事全部收口；并借预测策略升级的实测
> **揪出一处回滚写回缺陷**。
> 版本范围：`v1.1.0`（b4eac80）→ `v1.2.0`（af69f65），共 11 笔提交。
> 主要对应：`6798b07`（#6 重连）/ `f46cedc`（#4 超界重同步）/ `2a0a7f6`（#5 压测进 CI）/
> `c5c521c`（#5 多房间）/ `441267f`（性能门禁）/ `41c2202`（告警门禁）/
> `773d988`（#2 房间分片）/ `af69f65`（#1 预测策略）。

## 一句话总结

- **网络端到端闭环**：断线重连（#6）、超界输入「有界可恢复」重同步（#4）、
  真实 socket 压测（#5）全部进 CI，每次 push 都复验，不再「手动跑过」。
- **高并发**：房间分片（#2 轻量版）把单模拟线程横向拆成多条，128 房间 tick p99
  由 81.9 ms（245% 预算）降到 24.6 ms（73.7% 预算），**零背压**，收益近似线性。
- **手感**：输入预测策略升级 + 回滚写回缺陷修复，完全命中率 **14.94% → 89.78%**
  （≈ 理论上限 89.73%），且**不改变任何最终权威状态**（O0/O2 逐帧哈希一致）。
- **工程化**：性能基准不变量门禁、编译告警 `-Werror` 门禁、ThreadSanitizer 稳态竞态修复。

## 核心变更（按已知限制编号）

### 已知限制 #6 消除：断线重连端到端验证

- 新增 `src/reconnect_test.cpp`：走真实 socket，验证「Graceful 重连的状态恢复」——
  连接 gameserver → 断开 TCP → 重连并发送 `kReconnect` → 断言重连后客户端在至多 1 帧内
  拿到「比断线时更新」的全量快照，且能连续收到更新快照（已重新同步到权威世界）。
- 服务端 `on_reconnect` 清理：移除被丢弃的 `World out_world` 死参数，仅负责恢复会话状态；
  恢复用的全量快照仍由 accept 新连接时立即下发（见 `server_main.cpp`）。
- `build.sh` 新增 `reconnect` 目标；CI 新增 `reconnect-e2e` job（启动 gameserver 后运行
  `reconnect_test`，退出码非 0 即失败）。本地 3 次连跑均通过。

### TSan 第三次捕获：world 读写稳态竞态（修复）

- CI `tsan` job 报稳态 `data race`：accept worker 接入新连接时跨线程调用
  `room.build_state_packet()` 读 `session.world()`，与模拟线程 `room.tick()` 写 `world` 冲突。
- 修复（提交 `714cef0`）：accept worker 不再触碰 `room`，握手全量包改由**模拟线程（独占 `world`）
  在接管新连接时**构造并下发——所有对 `world` 的读写都落在单线程内。
- 本地 normal/slow/reconnect-e2e 三种模式均 RC=0 且无 data race；TSan 真机绿以 CI 复验为准。

### 已知限制 #4 消除：回滚上限修复（超界输入有界重同步）

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
- 新增 `src/rollback_bound_test.cpp`（`build.sh` 的 `rollback_bound` 目标）：超界输入触发
  `needs_resync` 且不产生静默分歧；窗口内迟到输入不被误拒、仍能触发回滚。

### 已知限制 #5 消除：真实 socket 压测进 CI + 多房间压测

- `loadtest_main.cpp` 走**真实 socket**：N 个客户端线程各建真实 TCP/UDP，30Hz 发输入 + 收状态广播，
  统计 QPS / 端到端 RTT 分位数 / 带宽；此前只「手动跑」，未进 CI。
- CI 新增 `load-test` job：后台起 gameserver（4 玩家 / 60ms 延迟 / 15ms 抖动 / 1% 丢包 / 650 帧）
  → 跑 `loadtest --clients 4 --duration 20` → 断言 RC=0 **且**真的建立了 4 个连接、发了约 2400 个
  输入包（防「CI 绿但空跑」静默失效）。
- 新增 `src/multiroom_net_test.cpp`（`build.sh` 的 `multiroomnet` 目标）：**单进程多房间 × 真实 socket**，
  补齐「进程内多房间零 socket（`multi_room_test`）」与「真实 socket 单房间（`loadtest`）」之间缺的那一格。
  自带判定输出 `MULTIROOM_OK` / `MULTIROOM_FAIL`。
- 实测（Windows dev box，16 逻辑核）模拟线程容量边界落在 64–128 房间，瓶颈是**单线程模拟**：

  | 房间数 | 客户端 socket | tick p99 | 预算占用 | 频率 | 背压丢弃 |
  |---|---|---|---|---|---|
  | 32 | 256 | 9.2 ms | 27.6% | 30.0 Hz | 0 |
  | 64 | 512 | 18.4 ms | 55.3% | 30.0 Hz | 0 |
  | 128 | 1024 | 81.9 ms | 245% | 29.4 Hz | 47,837 |

  128 房间时多出的状态包正是 #4 的超界重同步全量包（过载 → 输入迟到 → 重同步），
  既是「有界可恢复」的现场证据，也是可用的过载特征——直接指向 #2（分片）。

### 性能基准与不变量门禁（防性能回归）

- 新增 `src/bench_main.cpp`（`build.sh` 的 `bench` 目标）。性能门禁**不用绝对吞吐阈值**
  （CI runner 与开发机配置不同，卡太紧会天天误报被人 `|| true` 绕过，卡太松则真回归也绿），分两层：
  - **A. 机器无关的算法不变量（硬断言）**：① 回滚重算帧数 = 延迟 + 1（delay=2/3/5/8/12
    实测 3/4/6/9/13，精确成立）；② 回滚后哈希 == 零延迟理想哈希（`1022ecfad726271e`）；
    ③ 增量包大小 ≤ 全量包大小（实测 10 B ≤ 46 B）。
  - **B. 机器相关的吞吐（下限放宽约 10 倍 + 始终打印）**：本机 16 核单房间 1.33 μs/帧、
    128 房间 5.29M 帧/s；CI 下限 ≤ 50 μs/帧、≥ 100k 帧/s。
  - 输出 `BENCH_OK` / `BENCH_FAIL`，退出码即结论。
- CI 新增独立 `bench` job（含防静默空跑断言）；`build.sh verify` 第 12 步；README §4.11 + 验证矩阵。
- 定位：不变量层捕捉**算法回归**（回滚范围/正确性/编码退化），吞吐层只求捕捉**灾难性回归**，
  真实数字进日志看趋势——「宁可漏报轻微退化，也不让门禁变成噪声」。

### 编译告警门禁（-Wall -Wextra -Werror）

- 首次开启 `-Wall -Wextra` 全量扫描，共 **53 条告警**分布在 15 个文件，已全部清零：
  - **`-Wreorder` × 13**：`RollbackSession` 初始化列表顺序与成员声明顺序不一致（危险在于
    初始化列表书写顺序会被忽略、实际按声明顺序执行）。已改为声明顺序。
  - **`-Wunused-parameter` × 7**：`on_reconnect` 不再使用的 `last_acked` → 匿名参数。
  - **`-Wunused-variable` × 6**，其中一条是**真实缺陷**：`bandwidth_test.cpp` 只把 `xe` 计入
    位置误差、`ye` 算了却没用——即**只校验 X 轴漏了 Y 轴**，输出却写「位置误差」。已补上
    Y 轴（"告警不是风格问题"的例证）。
  - 另清掉 `latency_histogram.h` 未用 `lo`、`delta_snapshot_test.cpp` 未用 `per_room_kb`、
    `replay.h` 一处 `-Wsign-compare`；删掉 `RollbackSession` 从未使用的私有成员 `hist_`
    （Clang `-Wunused-private-field` 会直接 `-Werror` 失败）。
- `build.sh` 新增 `warnings` 模式；CI 新增 `warnings` job：**GCC `-Wall -Wextra -Werror` 阻塞**，
  Clang 与 cppcheck 先作**参考**（非阻塞，待首轮 CI 把 Clang 侧清零后去 `continue-on-error`）。

### 文档与作品集收尾

- **修正 README §二 架构图与代码不一致**：原图仍写「UDP 线程 / TCP 线程 直改 room」，是 v1.1
  重构前的模型；已改为与代码相符的「worker 池 + 无锁 MPSC/SPSC + 单模拟线程」。
- 新增两张 Mermaid 图（GitHub 直接渲染）：服务端并发模型（显式写核心不变式「room 只被模拟线程
  读写」+ TSan 守护）、一次输入的回滚时序（标注「延迟 d 帧 ⇒ 每次回滚重算 d+1 帧」）。
- README §七 简历写法新增「工程化与验证体系」块（告警门禁、TSan 三次捕获、性能门禁两层设计、
  真实 socket 压测两档）——这部分是后端岗区分度最高的内容。§0 关键数字表新增并发行。

### 已知限制 #2 轻量版：房间分片

**动机来自实测不是猜测**：单模拟线程下 128 房间 81.9 ms（245%）直接打爆 33ms 预算，
瓶颈是**模拟 CPU**而非网络栈 → 必须横向拆模拟线程。

- 新增 `src/net/room_shard.h`：`RoomShard<ProcessRoom>` 按 `room_index % shard_count` 均摊房间，
  自带每分片 tick 耗时直方图、完成 tick 数与实际频率。
- `multiroom_net_test.cpp` 改为分片宿主：`multiroomnet [rooms] [duration] [players] [shards]`；
  socket 服务逻辑抽成 `service_room()`（只碰本房间），判定改为「**最差分片** tick p99 是否落预算内」。
- **结果（16 逻辑核本机）**：

  | 房间数 | 分片 | 每分片房间 | 最差分片 p99 | 预算占用 | 频率 | 背压 |
  |---|---|---|---|---|---|---|
  | 128 | 1（基线） | 128 | 81.9 ms | 245% | 29.4 Hz | 47,837 |
  | **128** | **4** | 32 | **24.6 ms** | **73.7%** | **30.0 Hz** | **0** |
  | 256 | 8 | 32 | 20.5 ms | 61.4% | 30.0 Hz | 124（0.03%） |

  → 128 房间由 FAIL 翻 PASS，收益近似线性。
- **为什么分片不需要锁**：房间间无任何共享状态（独立世界/快照环/输入表/socket），分片只改变
  「哪个房间归哪条线程」，**不改变「每个房间只被一条线程读写」** → 无锁、无数据竞争（TSan 守护）、
  确定性不受影响。★ 边界：若将来房间间需共享状态（跨房间广播、全局排行榜）必须显式设计同步。
- **测量卫生修复**：多分片并行时个别连接瞬时打满默认发送缓冲触发 `EWOULDBLOCK` 被记成背压
  （328,282 包里约 8 个）。这是测量假象——**不放松门禁，而是把收发缓冲加到 256 KB 消掉它**，
  让「零背压」严格判据继续成立。
- 256 房间行的诚实说明：p99 仍在预算内但有 0.03% 背压故判 FAIL，很可能是**测量端自身干扰**
  （264 线程挤 16 核），只说明「接近本机上限」，不作干净容量结论。
- CI `multiroom-net` job 增加分片档（32 房间 / 2 分片）；`build.sh verify` 第 11 步加
  「64 房间 × 1 分片（基线超预算）vs × 4 分片（回到预算内）」对照；README §4.10 重写 + 验证矩阵。

### 已知限制 #1 消除：输入预测策略升级 + 回滚写回缺陷

**这次最大的收获不是「换了更好的策略」，而是「用测量揪出一个真实缺陷」。**

- 新增 `src/prediction_test.cpp`（`build.sh` 的 `prediction` 目标）两个关键决定：
  1. **先算「理论上限」再报实测值**：命中率是越高越好却无参照不知好坏。测试先统计玩家行为序列的
    **自相关**（= 完美知道上一帧真实输入时的命中率，即任何「重复上一帧」类策略的天花板），再报实测。
  2. **公平建模**：同时含**意图持续 4~14 帧**与**约 12 帧攻击/施法周期倾向**，两代策略各有优势面
    （不用纯周期输入「用假设验证假设」）。
- 新增 `src/core/predictor.h`：把预测策略抽成**客户端与服务端共用**一份实现。★ 抽出时发现两端
  **实际并不一致**——注释都写「必须与服务端完全一致」，但服务端 fallback 是「重复上一次输入」、
  客户端 fallback 是空操作 `Command{}`。**注释会骗人，共享实现不会。**
- **★ 测量发现的真实缺陷**：首次实测「意图持续优先」仅 **14.94%**，而理论上限 89.73%——差 6 倍，
  说明问题在实现而非模型。查出：
  > **回滚重算路径（`try_rollback`）没有把重算后的输入写回输入表**——主推进路径（`advance`）
  > 会写 `fi.cmd[p] = use[p]`，重算路径算完 `use[p]` 就丢了。于是 `frames_[f].cmd[p]` 一直保留
  > **回滚前的陈旧值**，任何「读上一帧输入」的预测都读到陈旧值并沿帧链复制。
  补上写回后：**14.94% → 89.78%**。
- **对照结果**（延迟 2/4/8/12 帧平均）：

  | 策略 | 完全命中率 | 移动命中率 |
  |---|---|---|
  | 旧：同相位（不查输入真实性） | 10.49% | 21.07% |
  | 同相位 + 只信真实输入 | 10.65% | 21.25% |
  | **意图持续优先（新默认）** | **89.78%** | **91.02%** |
  | *理论上限（玩家行为自相关）* | *89.73%* | *90.84%* |

  新策略四档延迟均最优且**已达理论上限**；三种模式最终权威哈希都与零延迟理想状态逐位一致
  （预测策略绝不改变游戏结果）。
- 默认模式改为 `kIntentFirst`；`kLegacy` / `kRealOnly` 保留用于对照。新增可观测指标：
  预测命中率、移动字段命中率、**策略来源分布**（prev/phase/fallback）。
- CI 新增独立 `prediction` job（含防静默空跑/失效断言，并要求「理论上限」参照存在）；
  `build.sh verify` 第 13 步；README §4.12 + 已知限制 #1 + 验证矩阵 + §七 简历预测条目。
- **回归确认**：`rollback_test` 正确性通过、平均重算帧数仍 = 延迟+1；`bench` 全 PASS（哈希仍
  `1022ecfad726271e`）；O0/O2 逐帧哈希一致；`multiroomnet 32` 仍 `MULTIROOM_OK`；
  `-Wall -Wextra -Werror` 零告警。

### 已知限制 #2 延伸：单机多进程房间分片（超出课程范围的降级 demo）

**背景**：#2 轻量版是「单进程内多模拟线程分片」。用户明确选择把「单机的多进程分片」作为
下一步探索方向——这是迈向**跨机分片**的第一道门槛（强隔离 + 跨进程通道），也是课程范围之外、
但最能体现代码分片不变量的一次工程练习。本批作为 **v1.2.0 收口后的附加 demo** 单独实现，
**未改任何既有功能代码**，仅新增文件、接入 `build.sh` 与 CI。

- 新增 `src/net/shm.h`：跨平台**命名共享内存**段封装
  （POSIX `shm_open`/`ftruncate`/`mmap` + Windows `CreateFileMappingA`/`MapViewOfFile`/`OpenFileMappingA`）。
- 新增 `src/mproc_shard_main.cpp`：单二进制双角色
  - `router` 角色：持有**全部客户端真实 socket**（每房间 UDP 收输入 + TCP 广播状态），
    `fork`/`CreateProcess` 出 N 个 `shard` 子进程，经**共享内存 SPSC 无锁环**转发输入/ack/重连、
    回收状态广播给客户端。
  - `shard` 角色：在**独立 OS 进程**里跑分配给自己的房间（`BattleRoom`），只碰共享内存环，
    物理上不与其他 shard 共享地址空间。
- **为什么这条通道值得做**：进程隔离是比线程隔离更强的不变量——shard 间**不可能**共享地址空间，
  任何「共享状态导致竞态」在编译/运行层面都不可能发生。通道用无锁 SPSC 环放在共享内存里，
  router 生产 / shard 消费（每条环唯一生产者与消费者），跨进程原子量落在同一物理页，无需任何锁。
- **与多线程分片完全一致**：复用了 `room_shard.h` 的 `shard_of(room_index, shards)` round-robin
  分配与「**最差分片** tick p99 是否落预算内」的验收标准；差别仅在于 worker 是进程、通道是共享内存环。
- **本地实测（Windows MSYS2，16 逻辑核，router 进程持有 socket + 2 个 shard 进程）**：

  | 房间数 | 分片进程 | 最差分片 p99 | 预算占用 | 频率 | 背压 | TCP 字节差 |
  |---|---|---|---|---|---|---|
  | 8 | 2 | 255 µs | 0.76% | 29.9 Hz | 0 | 0.00% |
  | 9 | 3 | 415 µs | 1.25% | 30.0 Hz | 0 | 0.00% |
  | 16 | 4 | 575 µs | 1.73% | 30.0 Hz | 0 | 0.30% |
  | 8 | 1 | 175 µs | 0.52% | 30.0 Hz | 0 | 0.36% |

  输出 `MPROC_OK rooms=<r> players=<p> shards=<s> qps=<q> tick_p99_us=<u>`；零背压 + TCP
  字节发出≈收到（<2%）是硬判据。
- **踩坑记录（已修复）**：
  1. `Shm` 缺移动语义 → `vector<Shm>::push_back(move)` 触发隐式按成员移动，原对象析构 `close()`
     把「同一份映射」释放，子进程 `OpenFileMapping` 失败、router 写已解映射地址段错误；改为手写
     move（转移所有权 + 源置空）后解决。
  2. Windows `CreateProcess` 命令行未把 exe 作为 `argv[0]` → 子进程 `argv[0]=="shard"` 使 `main`
     读到的 `role=argv[1]=="0"` 而非 `"shard"`；改为 exe 作为首 token，与 POSIX `execv` 的
     `cargs=[exe,"shard",...]` 对齐。
  3. relay 循环早期按「每个房间」drain 整条分片出口环 → 同分片其他房间的包被弹出后丢弃；
     改为按分片一次性 drain 再按 `room_id` 路由。
- **收口加固（POSIX 路径首次真跑前的防御）**：本 demo 在 Windows 上验证通过，但 `fork`+`shm_open`
  的 POSIX 分支要等 ubuntu CI 才第一次真跑，故补三处防御：
  1. `spawn_shard` 的 POSIX 分支 `execv` → `execvp`：以「无斜杠」方式调用（如 PATH 里直接敲
     `mprocshard`）时 `execv` 因不搜 PATH 会失败、子进程退 127、分片根本没起来（虽会因
     `rooms_ok` 不足而 FAIL，但属本可正常的脆弱点）。
  2. `pass` 判定新增 `min_ticks` 满额断言（每个分片 tick 数 ≥ 预算的 95%）：容量 demo 的核心
     不变量是「每个分片都跑满 tick 预算」，缺此则可能把「某分片被饿死/卡住只跑半数 tick」误判为 OK。
  3. CI `multiroom-mproc` 的字节差 grep 由 `差 0\.[0-9]+%`（只认 <1%）放宽为 `差 [01]\.[0-9]+%`
     （覆盖 0–1.99%，与 `MPROC_OK` 的 <2% 硬判据一致），避免 runner 负载高产生 1.x% 差时误杀 CI。
- `build.sh` 新增 `mprocshard` 目标 + `verify` 第 14 步（断言 `MPROC_OK rooms=8`）；
  CI 新增 `multiroom-mproc` job（POSIX 路径 `fork`+`shm_open`+`-lrt`，grep `MPROC_OK` / 字节校验 /
  零背压三道防静默失效断言）。

## 验证总览

- **本地**（Windows MSYS2 g++ 15.2.0）：`build.sh` 全部目标编译通过；`-Wall -Wextra -Werror` 零告警；
  `rollback_test` / `determinism_test`（2000 帧）/ `reconnect` / `multiroomnet`（32/1、32/2、128/4 PASS、
  128/1 FAIL 基线符合预期）/ `bench`（BENCH_OK）/ `prediction` 均 RC=0。
- **CI（GitHub Linux runner）**：CI #14（af69f65）**9 job 全部 Success**——跨优化级别确定性 + 全部验证、
  Clang 编译检查、ThreadSanitizer、断线重连端到端、真实 socket 网络压测、真实 socket × 多房间压测、
  性能基准与不变量门禁、输入预测策略、编译告警门禁。（页面 warning 仅为 Node.js 20 弃用与
  Ubuntu 26 迁移提示，非阻塞。）

## 诚实保留（仍未做）

- **跨机分片路由**：本版已具备「单机多**进程**分片」（独立 OS 进程 + 共享内存 SPSC 环通道，见上节），
  但仍是单机内；跨机路由（一致性哈希 + 网络转发 + 跨机房间迁移）未做。从「多进程」到「跨机」的
  关键新增是：序列化协议 + 网络传输层 + 房间迁移/再平衡——本 demo 故意不做，留作下阶段。
- **客户端为 bot**：有预测器与 `web/` 可视化，无真实渲染引擎接入。
- **未证明「百万连接」级**：无 epoll/io_uring、无分片路由。

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

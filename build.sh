#!/bin/bash
# build.sh — synq 构建与验证脚本
# 用法:
#   ./build.sh          编译全部
#   ./build.sh verify   编译并跑全部验证实验
#   ./build.sh server   启动游戏服务器
set -e
cd "$(dirname "$0")"

CXXFLAGS="-O2 -std=c++20 -pthread"
LIBS=""
if [[ "$OSTYPE" == msys* || "$OS" == "Windows_NT" ]]; then
  LIBS="-lws2_32"
  echo "[build] Windows/MSYS2: 链接 ws2_32"
else
  LIBS="-lrt"
  echo "[build] POSIX: 链接 rt（shm_open/mmap 命名共享内存）"
fi

OUT="${TMPDIR:-/tmp}/synq_build"
mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# 编译告警门禁：./build.sh warnings
# ---------------------------------------------------------------------------
# 【为什么把门禁设成「零告警」而不是「减少告警」】
#   告警只要允许存在，就会一直存在 —— 新告警混进既有告警里没人看。
#   而 -Wall -Wextra 抓的往往不是风格问题：本项目清告警时就抓到一个真实缺陷
#   （bandwidth_test 只校验了 X 轴位置误差、Y 轴算了没用 —— -Wunused-variable 暴露）。
#   所以：**先清零，再上 -Werror，此后任何新告警即 CI 红。**
if [[ "$1" == "warnings" ]]; then
  echo "[warnings] -Wall -Wextra -Werror 全源文件检查..."
  WFAIL=0
  for f in src/*.cpp src/client/*.cpp; do
    [[ -f "$f" ]] || continue
    if ! g++ -std=c++20 -Wall -Wextra -Werror -fsyntax-only -Isrc "$f"; then
      echo "  ✗ $f"
      WFAIL=1
    fi
  done
  if [[ "$WFAIL" == "1" ]]; then
    echo "[warnings] ✗ 存在编译告警（-Werror 已生效），请修复后再提交"
    exit 1
  fi
  echo "[warnings] ✓ 全部源文件在 -Wall -Wextra -Werror 下零告警"
  exit 0
fi

build() {
  local name=$1 src=$2
  g++ $CXXFLAGS -Isrc "src/$src" -o "$OUT/$name" $LIBS
  echo "  ✓ $name"
}

echo "[build] 编译可执行文件..."
build determinism  determinism_test.cpp
build rollback     rollback_test.cpp
build rollbackdiff rollback_diff.cpp
build bandwidth    bandwidth_test.cpp
build replaytest   replay_test.cpp
build multiroom    multi_room_test.cpp
build deltasnap    delta_snapshot_test.cpp
build visualdemo   client/visual_demo.cpp
build gameserver   server_main.cpp
build exporttrace  export_trace.cpp
build histogram    histogram_test.cpp
build configtest   config_test.cpp
build loadtest     loadtest_main.cpp
build reconnect     reconnect_test.cpp
build rollback_bound rollback_bound_test.cpp
build multiroomnet  multiroom_net_test.cpp
build bench         bench_main.cpp
build prediction    prediction_test.cpp
build mprocshard    mproc_shard_main.cpp

if [[ "$1" == "verify" ]]; then
  echo ""
  echo "===== 1. 确定性验证（跨优化级别）====="
  for opt in O0 O1 O2 O3 Os; do
    g++ -$opt -std=c++20 -Isrc src/determinism_test.cpp -o "$OUT/det_$opt"
  done
  for opt in O0 O1 O2 O3 Os; do
    "$OUT/det_$opt" "$opt" 12345 2000 > "$OUT/det_$opt.txt"
  done
  if diff <(grep "^F0" "$OUT/det_O0.txt") <(grep "^F0" "$OUT/det_O2.txt") >/dev/null \
     && diff <(grep "^F0" "$OUT/det_O0.txt") <(grep "^F0" "$OUT/det_O3.txt") >/dev/null \
     && diff <(grep "^F0" "$OUT/det_O0.txt") <(grep "^F0" "$OUT/det_Os.txt") >/dev/null; then
    echo "  ✓ 全部优化级别逐帧哈希一致（确定性成立）"
    grep FINAL_HASH "$OUT/det_O0.txt"
  else
    echo "  ✗ 存在差异，确定性被破坏"
    exit 1
  fi

  echo ""
  echo "===== 2. 回滚正确性 ====="
  for d in 1 3 8 12; do
    printf "  delay=%-3s " "$d"
    "$OUT/rollback" 999 "$d" 3000 1 2>&1 | grep -E "正确性验证|平均每次回滚" | tr '\n' ' '
    echo ""
  done

  echo ""
  echo "===== 3. 带宽对比 ====="
  "$OUT/bandwidth" | grep -E "文本格式|自定义|压缩|合计|Mbps"

  echo ""
  echo "===== 4. 回放系统 ====="
  "$OUT/replaytest" 1800 | grep -E "压缩|哈希|CRC|篡改|重演"

  echo ""
  echo "===== 5. 多房间并发 ====="
  "$OUT/multiroom" 128 2000 | grep -E "基线|^16 |^128 "

  echo ""
  echo "===== 6. 增量快照压缩 ====="
  "$OUT/deltasnap" 64 2000 | grep -E "可回溯|压缩比|增量快照环|压入|随机取"

  echo ""
  echo "===== 7. 可视化 demo（回滚时间轴）====="
  "$OUT/visualdemo" fast 2>&1 | tail -22

  echo ""
  echo "===== 8. 延迟直方图（可观测性）====="
  "$OUT/histogram"

  echo ""
  echo "===== 9. 优雅退出与配置 ====="
  "$OUT/configtest"

  echo ""
  echo "===== 10. 导出 Web 可视化数据 ====="
  "$OUT/exporttrace" 600 > web/trace.json
  echo "  已生成 web/trace.json ($(wc -c < web/trace.json) 字节)"

  echo ""
  echo "===== 11. 真实 socket × 多房间（消除 #5 缺口）====="
  "$OUT/multiroomnet" 32 10 4 | grep -E "正常房间|实际 tick|输入包\(服务端|状态包\(服务端|状态字节|每条分片|容量判定|MULTIROOM"

  echo ""
  echo "----- 11b. 模拟线程分片（已知限制 #2 轻量版）-----"
  echo "  64 房间 / 1 分片（基线，预期 tick 超预算）:"
  "$OUT/multiroomnet" 64 5 4 1 | grep -E "容量判定|MULTIROOM" || true
  echo "  64 房间 / 4 分片（预期回到预算内）:"
  "$OUT/multiroomnet" 64 5 4 4 | grep -E "容量判定|MULTIROOM" || true

  echo ""
  echo "===== 12. 性能基准与不变量门禁 ====="
  "$OUT/bench" | grep -E "PASS|FAIL|BENCH" || true

  echo ""
  echo "===== 13. 输入预测策略（消除 #1）====="
  "$OUT/prediction" | grep -E "参考上限|旧 同相位|同相位真实优先|意图持续优先|PREDICTION" || true

  echo ""
  echo "===== 14. 多进程房间分片（跨进程共享内存 SPSC 环通道）====="
  "$OUT/mprocshard" router 8 2 4 6 > "$OUT/mprocshard.log" 2>&1
  cat "$OUT/mprocshard.log" | grep -E "正常房间|实际 tick|输入包\(服务端|状态包\(服务端|状态字节|每条分片|容量判定|MPROC_" || true
  if ! grep -q "MPROC_OK rooms=8" "$OUT/mprocshard.log"; then
    echo "  ✗ 多进程分片验证失败（未拿到 MPROC_OK rooms=8）"
    exit 1
  fi
  echo "  ✓ 多进程分片验证通过：router 进程持有客户端 socket，shard 进程经共享内存 SPSC 环跑模拟"
fi

if [[ "$1" == "server" ]]; then
  echo ""
  if [[ "$2" == "web" ]]; then
    echo "生成 Web 可视化数据并启动服务器..."
    "$OUT/exporttrace" 600 > web/trace.json
    mkdir -p "$OUT/web" && cp -f web/index.html "$OUT/web/" && cp -f web/trace.json "$OUT/web/"
    echo ""
    echo "  → 浏览器打开: http://localhost:8899"
    echo "  → 按 Ctrl+C 停止"
    cd "$OUT/web" && python -m http.server 8899
    exit 0
  fi
  echo "启动可视化 demo"
  echo "  play  - 录屏模式（30fps 实时播放 20 秒）"
  echo "  web   - Web 图形界面（浏览器打开 http://localhost:8899）"
  echo "  其他  - 交互模式（回车推进）"
  [[ "$2" == "play" ]] && "$OUT/visualdemo" play
  exit 0
  echo ""
  if [[ "$2" == "loadtest" ]]; then
    echo "真实 socket 压测"
    echo "  终端 1: ./gameserver 4 60 15 1 3000"
    echo "  终端 2: ./loadtest --clients 4 --duration 30"
    echo ""
    "$OUT/loadtest" "${@:3}"
    exit 0
  fi
  echo "启动游戏服务器（玩家=4 延迟=60ms 抖动=15 丢包=1% 帧数=900）"
  "$OUT/gameserver" 4 60 15 1 900
fi

echo ""
echo "[done] 产物目录: $OUT"

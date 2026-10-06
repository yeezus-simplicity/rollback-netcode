#!/bin/bash
# build.sh — synq 构建与验证脚本
# 用法:
#   ./build.sh          编译全部
#   ./build.sh verify   编译并跑全部验证实验
#   ./build.sh server   启动游戏服务器
set -e
cd "$(dirname "$0")"

CXXFLAGS="-O2 -std=c++20 -pthread"
OSFLAG=""
if [[ "$OSTYPE" == msys* || "$OS" == "Windows_NT" ]]; then
  OSFLAG="-lws2_32"
  echo "[build] Windows/MSYS2: 链接 ws2_32"
fi

OUT="${TMPDIR:-/tmp}/synq_build"
mkdir -p "$OUT"

build() {
  local name=$1 src=$2
  g++ $CXXFLAGS -Isrc "src/$src" -o "$OUT/$name" $OSFLAG
  echo "  ✓ $name"
}

echo "[build] 编译可执行文件..."
build determinism  determinism_test.cpp
build rollback     rollback_test.cpp
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

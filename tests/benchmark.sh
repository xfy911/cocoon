#!/usr/bin/env bash
#
# benchmark.sh — Cocoon 性能基准测试
#
# 用法: ./tests/benchmark.sh [port] [duration] [connections] [threads]
# 默认: port=9999, duration=10s, connections=100, threads=4
#

set -euo pipefail

PORT="${1:-9999}"
DURATION="${2:-10s}"
CONNECTIONS="${3:-100}"
THREADS="${4:-4}"
URL="http://localhost:${PORT}/index.html"

SERVER="${SERVER:-./cocoon}"
ROOT="${ROOT:-./tests/fixtures}"

echo "[bench] 启动服务器 (端口 $PORT)..."
$SERVER -r "$ROOT" -p "$PORT" -l warn > /tmp/cocoon_bench.log 2>&1 &
SERVER_PID=$!
sleep 0.5

cleanup() {
    echo "[bench] 关闭服务器..."
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

echo "[bench] 预热 1s..."
wrk -t1 -c10 -d1s "$URL" > /dev/null 2>&1 || true

echo "[bench] 正式压测: wrk -t$THREADS -c$CONNECTIONS -d$DURATION $URL"
echo ""
wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$URL"

echo ""
echo "[bench] 单线程模式压测 (wrk -t1 -c50)..."
wrk -t1 -c50 -d"$DURATION" "$URL" || true

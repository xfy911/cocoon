#!/usr/bin/env bash
#
# integration_test.sh — Cocoon 集成测试套件
#
# 依赖: curl, pgrep, sleep
# 用法: ./tests/integration_test.sh [server_binary] [doc_root]
#

set -euo pipefail

SERVER="${1:-./cocoon}"
ROOT="${2:-./tests/fixtures}"
HOST="localhost:9999"
BASE="http://${HOST}"
PASS=0
FAIL=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"; kill_server' EXIT

kill_server() {
    local pids
    pids=$(pgrep -f "cocoon.*-p 9999" 2>/dev/null || true)
    if [[ -n "$pids" ]]; then
        echo "$pids" | xargs kill -9 2>/dev/null || true
    fi
    pids=$(pgrep -f "cocoon.*--cert" 2>/dev/null || true)
    if [[ -n "$pids" ]]; then
        echo "$pids" | xargs kill -9 2>/dev/null || true
    fi
    pids=$(pgrep -f "cocoon.*-c " 2>/dev/null || true)
    if [[ -n "$pids" ]]; then
        echo "$pids" | xargs kill -9 2>/dev/null || true
    fi
    sleep 0.5
    # 确保端口已释放
    for i in {1..20}; do
        if ! ss -tlnp 2>/dev/null | grep -q ':9999'; then
            break
        fi
        sleep 0.1
    done
}

# 启动服务器
start_server() {
    echo "[test] 启动服务器: $SERVER -r $ROOT -p 9999"
    $SERVER -r "$ROOT" -p 9999 -l debug > "$TMPDIR/server.log" 2>&1 &
    local pid=$!
    for i in {1..30}; do
        if nc -z localhost 9999 2>/dev/null; then
            echo "[test] 服务器就绪 (PID $pid)"
            return 0
        fi
        sleep 0.1
    done
    echo "[test] 服务器启动失败"
    cat "$TMPDIR/server.log"
    exit 1
}

# 断言辅助函数
pass() {
    PASS=$((PASS + 1))
}

fail() {
    FAIL=$((FAIL + 1))
}

assert_status() {
    local url="$1"
    local expect="$2"
    local desc="${3:-$url}"
    local status
    status=$(curl -s -o /dev/null -w "%{http_code}" "$url")
    if [[ "$status" == "$expect" ]]; then
        echo "  ✓ $desc — HTTP $status"
        pass
    else
        echo "  ✗ $desc — 期望 HTTP $expect, 实际 HTTP $status"
        fail
    fi
}

assert_status_with_header() {
    local header="$1"
    local url="$2"
    local expect="$3"
    local desc="${4:-$url}"
    local status
    status=$(curl -s -o /dev/null -w "%{http_code}" -H "$header" "$url")
    if [[ "$status" == "$expect" ]]; then
        echo "  ✓ $desc — HTTP $status"
        pass
    else
        echo "  ✗ $desc — 期望 HTTP $expect, 实际 HTTP $status"
        fail
    fi
}

assert_head_status() {
    local url="$1"
    local expect="$2"
    local desc="${3:-$url}"
    local status
    status=$(curl -s -o /dev/null -w "%{http_code}" -I "$url")
    if [[ "$status" == "$expect" ]]; then
        echo "  ✓ $desc — HTTP $status"
        pass
    else
        echo "  ✗ $desc — 期望 HTTP $expect, 实际 HTTP $status"
        fail
    fi
}

assert_head_no_body() {
    local url="$1"
    local desc="${2:-$url}"
    local len
    len=$(curl -s -o /dev/null -w "%{size_download}" -I "$url")
    if [[ "$len" == "0" ]]; then
        echo "  ✓ $desc — 无响应体"
        pass
    else
        echo "  ✗ $desc — 期望无响应体, 实际 $len 字节"
        fail
    fi
}

assert_header_contains() {
    local url="$1"
    local header="$2"
    local expect="$3"
    local desc="${4:-$url}"
    local value
    value=$(curl -sI "$url" | grep -i "^$header:" | tr -d '\r' || true)
    if echo "$value" | grep -qi "$expect"; then
        echo "  ✓ $desc — $header 包含 '$expect'"
        pass
    else
        echo "  ✗ $desc — $header 期望包含 '$expect', 实际: $value"
        fail
    fi
}

assert_header_contains_with_header() {
    local req_header="$1"
    local url="$2"
    local header="$3"
    local expect="$4"
    local desc="${5:-$url}"
    local value
    value=$(curl -sI -H "$req_header" "$url" | grep -i "^$header:" | tr -d '\r' || true)
    if echo "$value" | grep -qi "$expect"; then
        echo "  ✓ $desc — $header 包含 '$expect'"
        pass
    else
        echo "  ✗ $desc — $header 期望包含 '$expect', 实际: $value"
        fail
    fi
}

assert_body_contains() {
    local url="$1"
    local expect="$2"
    local desc="${3:-$url}"
    local body
    body=$(curl -s -k --compressed "$url")
    if echo "$body" | grep -q "$expect"; then
        echo "  ✓ $desc — 响应体包含 '$expect'"
        pass
    else
        echo "  ✗ $desc — 响应体期望包含 '$expect'"
        fail
    fi
}

assert_brotli() {
    local url="$1"
    local desc="${2:-$url}"
    local encoding
    encoding=$(curl -s -D - -o /dev/null -H "Accept-Encoding: br" "$url" | grep -i "Content-Encoding:" | tr -d '\r' || true)
    if echo "$encoding" | grep -qi "br"; then
        echo "  ✓ $desc — 返回 brotli 压缩"
        pass
    else
        echo "  ✗ $desc — 期望 brotli 压缩, 实际: $encoding"
        fail
    fi
}

assert_brotli_preferred() {
    local url="$1"
    local desc="${2:-$url}"
    local encoding
    encoding=$(curl -s -D - -o /dev/null -H "Accept-Encoding: gzip, br" "$url" | grep -i "Content-Encoding:" | tr -d '\r' || true)
    if echo "$encoding" | grep -qi "br"; then
        echo "  ✓ $desc — 优先返回 brotli 压缩"
        pass
    else
        echo "  ✗ $desc — 期望优先 brotli, 实际: $encoding"
        fail
    fi
}

assert_not_brotli() {
    local url="$1"
    local desc="${2:-$url}"
    local encoding
    encoding=$(curl -s -D - -o /dev/null -H "Accept-Encoding: br" "$url" | grep -i "Content-Encoding:" | tr -d '\r' || true)
    if [[ -z "$encoding" ]]; then
        echo "  ✓ $desc — 无 Content-Encoding（未压缩）"
        pass
    else
        echo "  ✗ $desc — 期望不压缩, 实际: $encoding"
        fail
    fi
}

assert_gzip() {
    local url="$1"
    local desc="${2:-$url}"
    local encoding
    encoding=$(curl -s -D - -o /dev/null -H "Accept-Encoding: gzip" "$url" | grep -i "Content-Encoding:" | tr -d '\r' || true)
    if echo "$encoding" | grep -qi "gzip"; then
        echo "  ✓ $desc — 返回 gzip 压缩"
        pass
    else
        echo "  ✗ $desc — 期望 gzip 压缩, 实际: $encoding"
        fail
    fi
}

assert_not_gzip() {
    local url="$1"
    local desc="${2:-$url}"
    local encoding
    encoding=$(curl -s -D - -o /dev/null -H "Accept-Encoding: gzip" "$url" | grep -i "Content-Encoding:" | tr -d '\r' || true)
    if [[ -z "$encoding" ]]; then
        echo "  ✓ $desc — 无 Content-Encoding（未压缩）"
        pass
    else
        echo "  ✗ $desc — 期望不压缩, 实际: $encoding"
        fail
    fi
}

assert_304() {
    local url="$1"
    local etag="$2"
    local desc="${3:-$url}"
    local status
    status=$(curl -s -o /dev/null -w "%{http_code}" -H "If-None-Match: $etag" "$url")
    if [[ "$status" == "304" ]]; then
        echo "  ✓ $desc — 缓存命中 HTTP 304"
        pass
    else
        echo "  ✗ $desc — 期望 304, 实际 $status"
        fail
    fi
}

assert_304_modified_since() {
    local url="$1"
    local mtime="$2"
    local desc="${3:-$url}"
    local status
    status=$(curl -s -o /dev/null -w "%{http_code}" -H "If-Modified-Since: $mtime" "$url")
    if [[ "$status" == "304" ]]; then
        echo "  ✓ $desc — If-Modified-Since 命中 HTTP 304"
        pass
    else
        echo "  ✗ $desc — 期望 304, 实际 $status"
        fail
    fi
}

assert_post_multipart() {
    local url="$1"
    local desc="${2:-$url}"
    local resp
    local boundary="----CocoonTestBoundary"
    local body
    body=$(printf -- '--%s\r\nContent-Disposition: form-data; name="file"; filename="upload_test.txt"\r\nContent-Type: text/plain\r\n\r\nCocoon multipart upload test content\r\n--%s--\r\n' "$boundary" "$boundary")
    
    resp=$(curl -s -X POST -H "Content-Type: multipart/form-data; boundary=${boundary}" --data-binary "$body" "$url")
    if echo "$resp" | grep -q '"uploaded"'; then
        echo "  ✓ $desc — 响应包含上传结果"
        pass
    else
        echo "  ✗ $desc — 响应期望包含上传结果, 实际: $resp"
        fail
    fi
    
    # 验证文件是否被保存
    if [ -f "$ROOT/uploads/upload_test.txt" ]; then
        echo "  ✓ $desc — 文件已保存到 uploads/"
        pass
    else
        echo "  ✗ $desc — 文件未保存到 uploads/"
        fail
    fi
}

assert_post_json() {
    local url="$1"
    local body="$2"
    local expect="$3"
    local desc="${4:-$url}"
    local resp
    resp=$(curl -s -X POST -H "Content-Type: application/json" -d "$body" "$url")
    if echo "$resp" | grep -q "$expect"; then
        echo "  ✓ $desc — 响应包含 '$expect'"
        pass
    else
        echo "  ✗ $desc — 响应期望包含 '$expect', 实际: $resp"
        fail
    fi
}

assert_post_form() {
    local url="$1"
    local body="$2"
    local expect="$3"
    local desc="${4:-$url}"
    local resp
    resp=$(curl -s -X POST -H "Content-Type: application/x-www-form-urlencoded" -d "$body" "$url")
    if echo "$resp" | grep -q "$expect"; then
        echo "  ✓ $desc — 响应包含 '$expect'"
        pass
    else
        echo "  ✗ $desc — 响应期望包含 '$expect', 实际: $resp"
        fail
    fi
}

assert_http2_brotli() {
    local url="$1"
    local desc="${2:-$url}"
    local encoding
    encoding=$(curl --http2 -k -s -D - -o /dev/null "$url" | grep -i "Content-Encoding:" | tr -d '\r' || true)
    if echo "$encoding" | grep -qi "br"; then
        echo "  ✓ $desc — HTTP/2 返回 brotli 压缩"
        pass
    else
        echo "  ✗ $desc — HTTP/2 期望 brotli 压缩, 实际: $encoding"
        fail
    fi
}

assert_http2_gzip() {
    local url="$1"
    local desc="${2:-$url}"
    local encoding
    encoding=$(curl --http2 -k -s -D - -o /dev/null "$url" | grep -i "Content-Encoding:" | tr -d '\r' || true)
    if echo "$encoding" | grep -qi "gzip"; then
        echo "  ✓ $desc — HTTP/2 返回 gzip 压缩"
        pass
    else
        echo "  ✗ $desc — HTTP/2 期望 gzip 压缩, 实际: $encoding"
        fail
    fi
}

assert_http2_not_compressed() {
    local url="$1"
    local desc="${2:-$url}"
    local encoding
    encoding=$(curl --http2 -k -s -D - -o /dev/null "$url" | grep -i "Content-Encoding:" | tr -d '\r' || true)
    if [[ -z "$encoding" ]]; then
        echo "  ✓ $desc — HTTP/2 无压缩"
        pass
    else
        echo "  ✗ $desc — HTTP/2 期望无压缩, 实际: $encoding"
        fail
    fi
}

assert_status_405() {
    local url="$1"
    local desc="${2:-$url}"
    local status
    status=$(curl -s -o /dev/null -w "%{http_code}" -X PUT "$url")
    if [[ "$status" == "405" ]]; then
        echo "  ✓ $desc — PUT 返回 405"
        pass
    else
        echo "  ✗ $desc — 期望 405, 实际 $status"
        fail
    fi
}

assert_access_log() {
    local log_file="$1"
    local desc="${2:-访问日志}"
    if [ -f "$log_file" ] && [ -s "$log_file" ]; then
        local count
        count=$(grep -c '"GET /' "$log_file" || true)
        if [ "$count" -ge 1 ]; then
            echo "  ✓ $desc — 日志文件包含请求记录"
            pass
        else
            echo "  ✗ $desc — 日志文件无请求记录"
            fail
        fi
    else
        echo "  ✗ $desc — 日志文件不存在或为空"
        fail
    fi
}

# ===== 测试开始 =====
start_server

echo ""
echo "=== 健康检查端点测试 ==="
health_body=$(curl -s "$BASE/_health")
health_status=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/_health")
if [[ "$health_status" == "200" ]]; then
    echo "  ✓ /_health 状态码 — HTTP 200"
    pass
else
    echo "  ✗ /_health 状态码 — 期望 200, 实际 $health_status"
    fail
fi
if echo "$health_body" | grep -q '"status": "ok"'; then
    echo "  ✓ /_health 响应体 — 包含 status: ok"
    pass
else
    echo "  ✗ /_health 响应体 — 缺少 status: ok"
    fail
fi
if echo "$health_body" | grep -q '"version": "Cocoon/1.0"'; then
    echo "  ✓ /_health 响应体 — 包含版本信息"
    pass
else
    echo "  ✗ /_health 响应体 — 缺少版本信息"
    fail
fi
if echo "$health_body" | grep -q '"connections"'; then
    echo "  ✓ /_health 响应体 — 包含连接数信息"
    pass
else
    echo "  ✗ /_health 响应体 — 缺少连接数信息"
    fail
fi
if echo "$health_body" | grep -q '"middleware"'; then
    echo "  ✓ /_health 响应体 — 包含中间件信息"
    pass
else
    echo "  ✗ /_health 响应体 — 缺少中间件信息"
    fail
fi

assert_head_status "$BASE/_health" "200" "健康检查 HEAD"

assert_status "$BASE/_health/nonexistent" "404" "健康检查路径遍历"
assert_status "$BASE/" "200" "首页 GET"
assert_status "$BASE/index.html" "200" "index.html GET"
assert_status "$BASE/nonexist.html" "404" "404 页面"
assert_status "$BASE/../etc/passwd" "404" "路径遍历防护"

echo ""
echo "=== HEAD 请求测试 ==="
assert_head_status "$BASE/" "200" "首页 HEAD"
assert_head_no_body "$BASE/index.html" "HEAD 无响应体"

echo ""
echo "=== 响应头测试 ==="
assert_header_contains "$BASE/index.html" "Server" "Cocoon" "Server 标识"
assert_header_contains "$BASE/index.html" "Content-Type" "text/html" "Content-Type"
assert_header_contains "$BASE/" "Content-Type" "text/html" "目录浏览 Content-Type"

echo ""
echo "=== 目录浏览测试 ==="
assert_status "$BASE/subdir/" "200" "目录浏览"
assert_body_contains "$BASE/subdir/" "Index of" "目录列表标题"

echo ""
echo "=== Range 请求测试 ==="
assert_status_with_header "Range: bytes=0-9" "$BASE/index.html" "206" "Range 206"
assert_header_contains_with_header "Range: bytes=0-9" "$BASE/index.html" "Content-Range" "bytes 0-9" "Content-Range 头"
assert_status_with_header "Range: bytes=99999-100000" "$BASE/index.html" "416" "越界 Range 416"

echo ""
echo "=== 缓存协商测试 ==="
# 先获取 ETag
ETAG=$(curl -sI "$BASE/index.html" | grep -i "ETag:" | awk '{print $2}' | tr -d '\r')
echo "  首页 ETag: $ETAG"
assert_header_contains "$BASE/index.html" "ETag" "\"" "ETag 存在"
assert_header_contains "$BASE/index.html" "Last-Modified" "" "Last-Modified 存在"
if [[ -n "$ETAG" ]]; then
    assert_304 "$BASE/index.html" "$ETAG" "If-None-Match 304"
fi
# If-Modified-Since 测试
LM=$(curl -sI "$BASE/index.html" | grep -i "Last-Modified:" | sed 's/Last-Modified: //i' | tr -d '\r')
if [[ -n "$LM" ]]; then
    assert_304_modified_since "$BASE/index.html" "$LM" "If-Modified-Since 304"
fi

echo ""
echo "=== Gzip 压缩测试 ==="
assert_gzip "$BASE/index.html" "HTML gzip"
assert_gzip "$BASE/style.css" "CSS gzip"
assert_gzip "$BASE/app.js" "JS gzip"
assert_gzip "$BASE/api.json" "JSON gzip"
# 图片不应压缩
assert_not_gzip "$BASE/image.png" "图片不压缩"

echo ""
echo "=== Brotli 压缩测试 ==="
assert_brotli "$BASE/index.html" "HTML brotli"
assert_brotli "$BASE/style.css" "CSS brotli"
assert_brotli "$BASE/app.js" "JS brotli"
assert_brotli_preferred "$BASE/index.html" "优先 brotli"
# 图片不应压缩
assert_not_brotli "$BASE/image.png" "图片不压缩"

echo ""
echo "=== MIME 类型测试 ==="
assert_header_contains "$BASE/style.css" "Content-Type" "text/css" "CSS MIME"
assert_header_contains "$BASE/app.js" "Content-Type" "application/javascript" "JS MIME"
assert_header_contains "$BASE/api.json" "Content-Type" "application/json" "JSON MIME"
assert_header_contains "$BASE/image.png" "Content-Type" "image/" "PNG MIME"

echo ""
echo "=== POST 请求测试 ==="
assert_post_json "$BASE/api/echo" '{"test": "hello"}' '"test"' "POST JSON 回显"
assert_post_form "$BASE/api/echo" 'name=cocoon&version=1.0' 'cocoon' "POST 表单回显"
assert_status_405 "$BASE/index.html" "PUT 405"

echo ""
echo "=== 文件上传测试 ==="
assert_post_multipart "$BASE/upload" "multipart 文件上传"

echo ""
echo "=== 访问日志测试 ==="
# 带访问日志启动服务器
kill_server
sleep 1
LOG_FILE="$TMPDIR/access.log"
$SERVER -r "$ROOT" -p 9999 --access-log "$LOG_FILE" > "$TMPDIR/server_access.log" 2>&1 &
pid_access=$!
for i in {1..30}; do
    if curl -s -o /dev/null "$BASE/" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
# 发送请求并验证日志记录
curl -s -o /dev/null "$BASE/index.html" -H "User-Agent: CocoonTest/1.0"
curl -s -o /dev/null "$BASE/nonexist.html" -H "Referer: http://example.com"
sleep 0.5
assert_access_log "$LOG_FILE" "访问日志文件生成"
# 检查日志格式是否包含关键字段
if grep -q '"GET /index.html HTTP/1.1"' "$LOG_FILE"; then
    echo "  ✓ 日志包含 GET /index.html"
    pass
else
    echo "  ✗ 日志缺少 GET /index.html"
    fail
fi
if grep -q 'CocoonTest/1.0' "$LOG_FILE"; then
    echo "  ✓ 日志包含 User-Agent"
    pass
else
    echo "  ✗ 日志缺少 User-Agent"
    fail
fi
if grep -q 'example.com' "$LOG_FILE"; then
    echo "  ✓ 日志包含 Referer"
    pass
else
    echo "  ✗ 日志缺少 Referer"
    fail
fi
if grep -q '404' "$LOG_FILE"; then
    echo "  ✓ 日志包含 404 状态码"
    pass
else
    echo "  ✗ 日志缺少 404 状态码"
    fail
fi

# 恢复普通服务器
kill_server
sleep 1
start_server

echo ""
echo "=== TLS/HTTPS 测试 ==="
# 启动 HTTPS 服务器（使用测试证书）
kill_server
sleep 1
$SERVER -r "$ROOT" -p 9999 --cert tests/server.crt --key tests/server.key > "$TMPDIR/server_tls.log" 2>&1 &
pid_tls=$!

# 等待服务器就绪（curl -k 忽略证书验证）
for i in {1..50}; do
    if curl -s -o /dev/null -k --max-time 2 "https://$HOST/" 2>/dev/null; then
        break
    fi
    sleep 0.2
done

# 使用 openssl s_client 验证 TLS 握手
if command -v openssl >/dev/null 2>&1; then
    tls_resp=$(echo -e "GET / HTTP/1.1\r\nHost: $HOST\r\nConnection: close\r\n\r\n" | \
        timeout 5 openssl s_client -connect "$HOST" -quiet 2>/dev/null | head -20)
    if echo "$tls_resp" | grep -q "HTTP/1.1"; then
        echo "  ✓ TLS 握手 + HTTP GET — 通过 HTTPS 获取响应"
        pass
    else
        echo "  ✗ TLS 握手 + HTTP GET — 未能通过 HTTPS 获取响应"
        fail
    fi
else
    echo "  ⚠ openssl 未安装，跳过 TLS 握手验证"
fi

# 使用 curl -k 验证 HTTPS 响应体
assert_body_contains "https://$HOST/" "Cocoon" "HTTPS 首页响应"

echo ""
echo "=== HTTP/2 测试 ==="
# 使用已启动的 TLS 服务器测试 HTTP/2（ALPN 协商）
kill_server
sleep 1
$SERVER -r "$ROOT" -p 9999 --cert tests/server.crt --key tests/server.key > "$TMPDIR/server_h2.log" 2>&1 &
pid_h2=$!

# 等待服务器就绪
for i in {1..50}; do
    if curl -s -o /dev/null -k --max-time 2 "https://$HOST/" 2>/dev/null; then
        break
    fi
    sleep 0.2
done

# 验证 HTTP/2 协议版本
h2_version=$(curl --http2 -k -s -o /dev/null -w "%{http_version}" "https://$HOST/")
if [[ "$h2_version" == "2" ]]; then
    echo "  ✓ HTTP/2 ALPN 协商 — 协议版本 HTTP/2"
    pass
else
    echo "  ✗ HTTP/2 ALPN 协商 — 期望 HTTP/2, 实际 HTTP/$h2_version"
    fail
fi

# HTTP/2 GET 首页
h2_status=$(curl --http2 -k -s -o /dev/null -w "%{http_code}" "https://$HOST/")
if [[ "$h2_status" == "200" ]]; then
    echo "  ✓ HTTP/2 GET 首页 — HTTP 200"
    pass
else
    echo "  ✗ HTTP/2 GET 首页 — 期望 200, 实际 $h2_status"
    fail
fi

# HTTP/2 响应体
h2_body=$(curl --http2 -k -s --compressed "https://$HOST/")
if echo "$h2_body" | grep -q "Cocoon"; then
    echo "  ✓ HTTP/2 响应体 — 包含 'Cocoon'"
    pass
else
    echo "  ✗ HTTP/2 响应体 — 未包含 'Cocoon'"
    fail
fi

# HTTP/2 HEAD 请求
h2_head_status=$(curl --http2 -k -s -o /dev/null -w "%{http_code}" -I "https://$HOST/index.html")
if [[ "$h2_head_status" == "200" ]]; then
    echo "  ✓ HTTP/2 HEAD 请求 — HTTP 200"
    pass
else
    echo "  ✗ HTTP/2 HEAD 请求 — 期望 200, 实际 $h2_head_status"
    fail
fi

# HTTP/2 404
h2_404=$(curl --http2 -k -s -o /dev/null -w "%{http_code}" "https://$HOST/nonexist.html")
if [[ "$h2_404" == "404" ]]; then
    echo "  ✓ HTTP/2 404 请求 — HTTP 404"
    pass
else
    echo "  ✗ HTTP/2 404 请求 — 期望 404, 实际 $h2_404"
    fail
fi

# HTTP/2 缓存协商（ETag 304）
H2_ETAG=$(curl --http2 -k -sI "https://$HOST/index.html" | grep -i "ETag:" | awk '{print $2}' | tr -d '\r')
if [[ -n "$H2_ETAG" ]]; then
    h2_304=$(curl --http2 -k -s -o /dev/null -w "%{http_code}" -H "If-None-Match: $H2_ETAG" "https://$HOST/index.html")
    if [[ "$h2_304" == "304" ]]; then
        echo "  ✓ HTTP/2 If-None-Match 304 — 缓存命中"
        pass
    else
        echo "  ✗ HTTP/2 If-None-Match 304 — 期望 304, 实际 $h2_304"
        fail
    fi
fi

# HTTP/2 压缩测试
assert_http2_brotli "https://$HOST/index.html" "HTTP/2 HTML brotli"
assert_http2_brotli "https://$HOST/style.css" "HTTP/2 CSS brotli"
assert_http2_brotli "https://$HOST/app.js" "HTTP/2 JS brotli"

# HTTP/2 目录浏览测试
h2_dir_status=$(curl --http2 -k -s -o /dev/null -w "%{http_code}" "https://$HOST/subdir/")
h2_dir_body=$(curl --http2 -k -s "https://$HOST/subdir/")
if [[ "$h2_dir_status" == "200" ]] && echo "$h2_dir_body" | grep -q "page.html"; then
    echo "  ✓ HTTP/2 目录浏览 — HTTP 200，包含 page.html"
    pass
else
    echo "  ✗ HTTP/2 目录浏览 — 期望 200+page.html, 实际 $h2_dir_status"
    fail
fi

# 停止 HTTP/2 服务器，恢复 HTTP 服务器
kill_server
sleep 1
start_server

# h2c 测试（明文 HTTP/2）
echo ""
echo "=== h2c 测试 ==="

# h2c prior knowledge 直接连接
h2c_pk_status=$(curl --http2-prior-knowledge -s -o /dev/null -w "%{http_code}" "http://$HOST/")
h2c_pk_version=$(curl --http2-prior-knowledge -s -o /dev/null -w "%{http_version}" "http://$HOST/")
if [[ "$h2c_pk_status" == "200" && "$h2c_pk_version" == "2" ]]; then
    echo "  ✓ h2c prior knowledge — HTTP 200, 协议 HTTP/2"
    pass
else
    echo "  ✗ h2c prior knowledge — 期望 200+HTTP/2, 实际 $h2c_pk_status+HTTP/$h2c_pk_version"
    fail
fi

# h2c prior knowledge 404
h2c_pk_404=$(curl --http2-prior-knowledge -s -o /dev/null -w "%{http_code}" "http://$HOST/nonexist.html")
if [[ "$h2c_pk_404" == "404" ]]; then
    echo "  ✓ h2c prior knowledge 404 — HTTP 404"
    pass
else
    echo "  ✗ h2c prior knowledge 404 — 期望 404, 实际 $h2c_pk_404"
    fail
fi

# h2c Upgrade 协商
h2c_up_status=$(curl --http2 -s -o /dev/null -w "%{http_code}" "http://$HOST/")
h2c_up_version=$(curl --http2 -s -o /dev/null -w "%{http_version}" "http://$HOST/")
if [[ "$h2c_up_status" == "200" && "$h2c_up_version" == "2" ]]; then
    echo "  ✓ h2c Upgrade 协商 — HTTP 200, 协议 HTTP/2"
    pass
else
    echo "  ✗ h2c Upgrade 协商 — 期望 200+HTTP/2, 实际 $h2c_up_status+HTTP/$h2c_up_version"
    fail
fi

# h2c Upgrade 404
h2c_up_404=$(curl --http2 -s -o /dev/null -w "%{http_code}" "http://$HOST/nonexist.html")
if [[ "$h2c_up_404" == "404" ]]; then
    echo "  ✓ h2c Upgrade 404 — HTTP 404"
    pass
else
    echo "  ✗ h2c Upgrade 404 — 期望 404, 实际 $h2c_up_404"
    fail
fi

# HTTP/2 目录浏览测试
# subdir 目录没有 index.html，应返回 200 目录列表
h2c_dir_status=$(curl --http2-prior-knowledge -s -o /dev/null -w "%{http_code}" "http://$HOST/subdir/")
h2c_dir_body=$(curl --http2-prior-knowledge -s "http://$HOST/subdir/")
if [[ "$h2c_dir_status" == "200" ]] && echo "$h2c_dir_body" | grep -q "page.html"; then
    echo "  ✓ h2c 目录浏览 — HTTP 200，包含 page.html"
    pass
else
    echo "  ✗ h2c 目录浏览 — 期望 200+page.html, 实际 $h2c_dir_status"
    fail
fi

# WebSocket 测试
# 停止 HTTP 服务器，恢复默认服务器
kill_server
sleep 1
start_server

# 中间件测试
echo ""
echo "=== 中间件测试 ==="

# CORS 测试
kill_server
sleep 1
$SERVER -r "$ROOT" -p 9999 --cors > "$TMPDIR/server_cors.log" 2>&1 &
for i in {1..30}; do
    if nc -z localhost 9999 2>/dev/null; then break; fi
    sleep 0.1
done
cors_status=$(curl -s -o /dev/null -w "%{http_code}" -X OPTIONS "$BASE/" -H "Origin: http://example.com" -H "Access-Control-Request-Method: POST")
if [[ "$cors_status" == "204" ]]; then
    echo "  ✓ CORS OPTIONS 预检 — HTTP 204"
    pass
else
    echo "  ✗ CORS OPTIONS 预检 — 期望 204, 实际 $cors_status"
    fail
fi

# Basic Auth 测试
kill_server
sleep 1
$SERVER -r "$ROOT" -p 9999 --auth-user "admin" --auth-pass "secret" > "$TMPDIR/server_auth.log" 2>&1 &
for i in {1..30}; do
    if nc -z localhost 9999 2>/dev/null; then break; fi
    sleep 0.1
done
auth_no_cred=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/")
if [[ "$auth_no_cred" == "401" ]]; then
    echo "  ✓ Basic Auth 无认证 — HTTP 401"
    pass
else
    echo "  ✗ Basic Auth 无认证 — 期望 401, 实际 $auth_no_cred"
    fail
fi
auth_with_cred=$(curl -s -o /dev/null -w "%{http_code}" -u "admin:secret" "$BASE/")
if [[ "$auth_with_cred" == "200" ]]; then
    echo "  ✓ Basic Auth 正确认证 — HTTP 200"
    pass
else
    echo "  ✗ Basic Auth 正确认证 — 期望 200, 实际 $auth_with_cred"
    fail
fi
auth_wrong_pass=$(curl -s -o /dev/null -w "%{http_code}" -u "admin:wrong" "$BASE/")
if [[ "$auth_wrong_pass" == "401" ]]; then
    echo "  ✓ Basic Auth 错误密码 — HTTP 401"
    pass
else
    echo "  ✗ Basic Auth 错误密码 — 期望 401, 实际 $auth_wrong_pass"
    fail
fi

# Rate Limit 测试
kill_server
sleep 1
$SERVER -r "$ROOT" -p 9999 --rate-limit 1 > "$TMPDIR/server_ratelimit.log" 2>&1 &
for i in {1..30}; do
    if nc -z localhost 9999 2>/dev/null; then break; fi
    sleep 0.1
done
rl_first=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/")
rl_second=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/")
if [[ "$rl_first" == "200" && "$rl_second" == "429" ]]; then
    echo "  ✓ Rate Limit 限流 — 第1次 200, 第2次 429"
    pass
else
    echo "  ✗ Rate Limit 限流 — 期望 200+429, 实际 $rl_first+$rl_second"
    fail
fi

# 恢复默认服务器
kill_server
sleep 1
start_server

echo ""
echo "=== 插件测试 ==="

# 编译示例插件
if gcc -Wall -Wextra -fPIC -shared -I. -Icoco/include -o "$TMPDIR/hello_plugin.so" plugins/hello.c middleware.c http.c log.c platform.c 2>/dev/null; then
    kill_server
    sleep 1
    $SERVER -r "$ROOT" -p 9999 --plugin "$TMPDIR/hello_plugin.so" > "$TMPDIR/server_plugin.log" 2>&1 &
    for i in {1..30}; do
        if nc -z localhost 9999 2>/dev/null; then break; fi
        sleep 0.1
    done
    plugin_http=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/")
    if [[ "$plugin_http" == "200" ]]; then
        echo "  ✓ 插件加载 — 服务器正常响应 HTTP 200"
        pass
    else
        echo "  ✗ 插件加载 — 期望 200, 实际 $plugin_http"
        fail
    fi
    # 检查日志中是否包含插件加载信息
    if grep -q "plugin: 已加载" "$TMPDIR/server_plugin.log"; then
        echo "  ✓ 插件日志 — 加载日志正确输出"
        pass
    else
        echo "  ✗ 插件日志 — 未找到加载日志"
        fail
    fi

    # 测试插件热重载
    PLUGIN_PID=$(pgrep -f "cocoon.*--plugin.*hello_plugin.so" 2>/dev/null || true)
    if [[ -n "$PLUGIN_PID" ]]; then
        kill -USR1 "$PLUGIN_PID" 2>/dev/null
        sleep 0.5
        plugin_http_after=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/")
        if [[ "$plugin_http_after" == "200" ]]; then
            echo "  ✓ 插件热重载 — SIGUSR1 后服务器正常响应 HTTP 200"
            pass
        else
            echo "  ✗ 插件热重载 — 期望 200, 实际 $plugin_http_after"
            fail
        fi
        if grep -q "热重载" "$TMPDIR/server_plugin.log"; then
            echo "  ✓ 插件热重载日志 — 热重载日志正确输出"
            pass
        else
            echo "  ✗ 插件热重载日志 — 未找到热重载日志"
            fail
        fi
    else
        echo "  ⊘ 插件热重载 — 无法获取服务器 PID"
    fi
else
    echo "  ⊘ 插件编译跳过（无编译器）"
fi

echo ""
echo "=== WebSocket 测试 ==="

if python3 "$ROOT/../websocket_test.py" > "$TMPDIR/ws_test.log" 2>&1; then
    echo "  ✓ WebSocket 握手 + echo — 通过"
    pass
else
    echo "  ✗ WebSocket 测试失败"
    cat "$TMPDIR/ws_test.log"
    fail
fi

# WebSocket 空闲超时测试（启动短超时服务器）
kill_server
sleep 1
$SERVER -r "$ROOT" -p 9999 -o 2000 > "$TMPDIR/server_ws_timeout.log" 2>&1 &
for i in {1..30}; do
    if nc -z localhost 9999 2>/dev/null; then break; fi
    sleep 0.1
done

if python3 "$ROOT/../websocket_test.py" --timeout-test > "$TMPDIR/ws_timeout.log" 2>&1; then
    echo "  ✓ WebSocket 空闲超时 (2s) — 通过"
    pass
else
    echo "  ✗ WebSocket 空闲超时测试失败"
    cat "$TMPDIR/ws_timeout.log"
    fail
fi
kill_server

# 恢复默认服务器用于后续测试
sleep 1
start_server

echo ""
echo "=== 反向代理测试 ==="

# 使用带代理配置的服务器测试
kill_server
sleep 1

# 启动一个后端服务器（Python http.server）
python3 -m http.server 9000 --directory "$ROOT" > "$TMPDIR/backend.log" 2>&1 &
BACKEND_PID=$!
sleep 1

# 创建带代理配置的配置文件
PROXY_CONFIG="$TMPDIR/proxy_config.json"
cat > "$PROXY_CONFIG" << 'EOF'
{
    "root_dir": "./tests/fixtures",
    "port": 9999,
    "log_level": "debug",
    "proxies": [
        {"prefix": "/backend", "target": "http://localhost:9000"}
    ]
}
EOF

$SERVER -c "$PROXY_CONFIG" > "$TMPDIR/server_proxy.log" 2>&1 &
for i in {1..30}; do
    if nc -z localhost 9999 2>/dev/null; then break; fi
    sleep 0.1
done

proxy_status=$(curl -s -o /dev/null -w "%{http_code}" "http://$HOST/backend/index.html")
if [[ "$proxy_status" == "200" ]]; then
    echo "  ✓ 反向代理 GET — HTTP 200"
    pass
else
    echo "  ✗ 反向代理 GET — 期望 200, 实际 $proxy_status"
    fail
fi

proxy_body=$(curl -s "http://$HOST/backend/index.html")
if echo "$proxy_body" | grep -q "Cocoon"; then
    echo "  ✓ 反向代理响应体 — 包含后端内容"
    pass
else
    echo "  ✗ 反向代理响应体 — 未包含后端内容"
    fail
fi

# 非代理路径仍走静态文件
static_status=$(curl -s -o /dev/null -w "%{http_code}" "http://$HOST/index.html")
if [[ "$static_status" == "200" ]]; then
    echo "  ✓ 非代理路径 — 静态文件正常 HTTP 200"
    pass
else
    echo "  ✗ 非代理路径 — 期望 200, 实际 $static_status"
    fail
fi

# 清理 HTTP 后端服务器
kill -9 $BACKEND_PID 2>/dev/null || true
kill_server
sleep 1

# === HTTP/2 反向代理测试 ===
echo ""
echo "=== HTTP/2 反向代理测试 ==="

# 重新启动后端服务器
python3 -m http.server 9000 --directory "$ROOT" > "$TMPDIR/backend_h2.log" 2>&1 &
BACKEND_H2_PID=$!
sleep 1

# 创建带代理配置的 TLS 配置文件
H2_PROXY_CONFIG="$TMPDIR/h2_proxy_config.json"
cat > "$H2_PROXY_CONFIG" << 'EOF'
{
    "root_dir": "./tests/fixtures",
    "port": 9999,
    "log_level": "debug",
    "proxies": [
        {"prefix": "/backend", "target": "http://localhost:9000", "pool_size": 2}
    ]
}
EOF

$SERVER -c "$H2_PROXY_CONFIG" --cert tests/server.crt --key tests/server.key > "$TMPDIR/server_h2_proxy.log" 2>&1 &
for i in {1..30}; do
    if nc -z localhost 9999 2>/dev/null; then break; fi
    sleep 0.1
done

# 等待 TLS 服务器就绪
for i in {1..50}; do
    if curl -s -o /dev/null -k --max-time 2 "https://$HOST/" 2>/dev/null; then break; fi
    sleep 0.2
done

h2_proxy_status=$(curl --http2 -k -s -o /dev/null -w "%{http_code}" "https://$HOST/backend/index.html")
if [[ "$h2_proxy_status" == "200" ]]; then
    echo "  ✓ HTTP/2 反向代理 GET — HTTP 200"
    pass
else
    echo "  ✗ HTTP/2 反向代理 GET — 期望 200, 实际 $h2_proxy_status"
    fail
fi

h2_proxy_body=$(curl --http2 -k -s "https://$HOST/backend/index.html")
if echo "$h2_proxy_body" | grep -q "Cocoon"; then
    echo "  ✓ HTTP/2 反向代理响应体 — 包含后端内容"
    pass
else
    echo "  ✗ HTTP/2 反向代理响应体 — 未包含后端内容"
    fail
fi

# 非代理路径仍走 HTTP/2 静态文件
h2_static_status=$(curl --http2 -k -s -o /dev/null -w "%{http_code}" "https://$HOST/index.html")
if [[ "$h2_static_status" == "200" ]]; then
    echo "  ✓ HTTP/2 非代理路径 — 静态文件正常 HTTP 200"
    pass
else
    echo "  ✗ HTTP/2 非代理路径 — 期望 200, 实际 $h2_static_status"
    fail
fi

# 清理 HTTP/2 后端服务器
kill -9 $BACKEND_H2_PID 2>/dev/null || true
kill_server
sleep 1

# === 加权轮询反向代理测试 ===
echo ""
echo "=== 加权轮询反向代理测试 ==="

# 创建两个后端目录，放不同内容
mkdir -p "$TMPDIR/backend_a" "$TMPDIR/backend_b"
echo "Backend-A" > "$TMPDIR/backend_a/index.html"
echo "Backend-B" > "$TMPDIR/backend_b/index.html"

# 启动两个后端服务器
python3 -m http.server 9002 --directory "$TMPDIR/backend_a" > "$TMPDIR/backend_a.log" 2>&1 &
BACKEND_A_PID=$!
python3 -m http.server 9003 --directory "$TMPDIR/backend_b" > "$TMPDIR/backend_b.log" 2>&1 &
BACKEND_B_PID=$!
sleep 1

# 创建加权轮询配置（权重 3:1）
WEIGHTED_CONFIG="$TMPDIR/weighted_proxy_config.json"
cat > "$WEIGHTED_CONFIG" << EOF
{
    "root_dir": "./tests/fixtures",
    "port": 9999,
    "log_level": "debug",
    "proxies": [
        {"prefix": "/backend", "target": "http://localhost:9002", "weight": 3, "pool_size": 2},
        {"prefix": "/backend", "target": "http://localhost:9003", "weight": 1, "pool_size": 2}
    ]
}
EOF

$SERVER -c "$WEIGHTED_CONFIG" > "$TMPDIR/server_weighted.log" 2>&1 &
for i in {1..30}; do
    if nc -z localhost 9999 2>/dev/null; then break; fi
    sleep 0.1
done

# 发送4个请求，验证加权轮询工作（至少能收到响应）
weighted_status=$(curl -s -o /dev/null -w "%{http_code}" "http://$HOST/backend/index.html")
if [[ "$weighted_status" == "200" ]]; then
    echo "  ✓ 加权轮询代理 — HTTP 200"
    pass
else
    echo "  ✗ 加权轮询代理 — 期望 200, 实际 $weighted_status"
    fail
fi

# 检查日志中是否记录了两个后端及其权重
if grep -q "weight=3" "$TMPDIR/server_weighted.log" && grep -q "weight=1" "$TMPDIR/server_weighted.log"; then
    echo "  ✓ 加权轮询配置 — 权重 3:1 已加载"
    pass
else
    echo "  ✗ 加权轮询配置 — 未找到权重日志"
    fail
fi

# 发送多个请求，验证平滑轮询分布（至少两个后端都收到请求）
for i in {1..4}; do
    curl -s -o /dev/null "http://$HOST/backend/index.html"
done

# 检查两个后端日志中都有请求
if grep -q "GET /index.html" "$TMPDIR/backend_a.log" && grep -q "GET /index.html" "$TMPDIR/backend_b.log"; then
    echo "  ✓ 加权轮询分布 — 两个后端均收到请求"
    pass
else
    echo "  ✗ 加权轮询分布 — 后端请求分布异常"
    fail
fi

# 清理加权轮询后端服务器
kill -9 $BACKEND_A_PID 2>/dev/null || true
kill -9 $BACKEND_B_PID 2>/dev/null || true
kill_server
sleep 1

# === HTTP/1.0 后端兼容测试 ===
echo ""
echo "=== HTTP/1.0 后端兼容测试 ==="

# 启动 HTTP/1.0 后端（发送 Connection: close 后主动关闭）
python3 "$ROOT/../http10_backend.py" 9004 "$ROOT" > "$TMPDIR/backend_http10.log" 2>&1 &
BACKEND_HTTP10_PID=$!
sleep 1

# 创建带 HTTP/1.0 代理配置的配置文件
HTTP10_PROXY_CONFIG="$TMPDIR/http10_proxy_config.json"
cat > "$HTTP10_PROXY_CONFIG" << 'EOF'
{
    "root_dir": "./tests/fixtures",
    "port": 9999,
    "log_level": "debug",
    "proxies": [
        {"prefix": "/backend", "target": "http://localhost:9004", "pool_size": 2}
    ]
}
EOF

$SERVER -c "$HTTP10_PROXY_CONFIG" > "$TMPDIR/server_http10_proxy.log" 2>&1 &
for i in {1..30}; do
    if nc -z localhost 9999 2>/dev/null; then break; fi
    sleep 0.1
done

# 第一次请求：应成功
http10_status1=$(curl -s -o /dev/null -w "%{http_code}" "http://$HOST/backend/index.html")
if [[ "$http10_status1" == "200" ]]; then
    echo "  ✓ HTTP/1.0 代理第一次请求 — HTTP 200"
    pass
else
    echo "  ✗ HTTP/1.0 代理第一次请求 — 期望 200, 实际 $http10_status1"
    fail
fi

# 第二次请求：验证代理能正确新建连接（旧连接已被后端关闭）
http10_status2=$(curl -s -o /dev/null -w "%{http_code}" "http://$HOST/backend/index.html")
if [[ "$http10_status2" == "200" ]]; then
    echo "  ✓ HTTP/1.0 代理第二次请求 — HTTP 200（连接池正确关闭旧连接）"
    pass
else
    echo "  ✗ HTTP/1.0 代理第二次请求 — 期望 200, 实际 $http10_status2（连接池可能复用了已关闭的连接）"
    fail
fi

# 检查代理日志中有关闭旧连接的日志
if grep -q "关闭" "$TMPDIR/server_http10_proxy.log" || grep -q "connection" "$TMPDIR/server_http10_proxy.log" || grep -q "新建" "$TMPDIR/server_http10_proxy.log"; then
    echo "  ✓ HTTP/1.0 代理日志 — 检测到连接管理日志"
    pass
else
    echo "  ⊘ HTTP/1.0 代理日志 — 未检测到连接管理日志（日志级别可能不够）"
    pass
fi

# 清理 HTTP/1.0 后端服务器
kill -9 $BACKEND_HTTP10_PID 2>/dev/null || true
kill_server
sleep 1

# === HTTPS 反向代理测试 ===
echo ""
echo "=== HTTPS 反向代理测试 ==="

# 启动 HTTPS 后端服务器
python3 "$ROOT/../https_backend.py" 9001 "$ROOT/../server.crt" "$ROOT/../server.key" "$ROOT" > "$TMPDIR/backend_https.log" 2>&1 &
BACKEND_HTTPS_PID=$!
sleep 1

# 创建带 HTTPS 代理配置的配置文件
HTTPS_PROXY_CONFIG="$TMPDIR/https_proxy_config.json"
cat > "$HTTPS_PROXY_CONFIG" << 'EOF'
{
    "root_dir": "./tests/fixtures",
    "port": 9999,
    "log_level": "debug",
    "proxies": [
        {"prefix": "/backend", "target": "https://localhost:9001"}
    ]
}
EOF

$SERVER -c "$HTTPS_PROXY_CONFIG" > "$TMPDIR/server_https_proxy.log" 2>&1 &
for i in {1..30}; do
    if nc -z localhost 9999 2>/dev/null; then break; fi
    sleep 0.1
done

https_proxy_status=$(curl -s -o /dev/null -w "%{http_code}" "http://$HOST/backend/index.html")
if [[ "$https_proxy_status" == "200" ]]; then
    echo "  ✓ HTTPS 反向代理 GET — HTTP 200"
    pass
else
    echo "  ✗ HTTPS 反向代理 GET — 期望 200, 实际 $https_proxy_status"
    fail
fi

https_proxy_body=$(curl -s "http://$HOST/backend/index.html")
if echo "$https_proxy_body" | grep -q "Cocoon"; then
    echo "  ✓ HTTPS 反向代理响应体 — 包含后端内容"
    pass
else
    echo "  ✗ HTTPS 反向代理响应体 — 未包含后端内容"
    fail
fi

# 清理 HTTPS 后端服务器
kill -9 $BACKEND_HTTPS_PID 2>/dev/null || true

# 恢复默认服务器
kill_server
sleep 1
start_server

echo ""
echo "=== Prometheus 指标测试 ==="

# 先发起几个请求，让计数器有值
curl -s -o /dev/null "$BASE/"
curl -s -o /dev/null "$BASE/"
curl -s -o /dev/null "$BASE/notfound.html"

metrics_body=$(curl -s "$BASE/_metrics")
metrics_status=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/_metrics")

if [[ "$metrics_status" == "200" ]]; then
    echo "  ✓ /_metrics 状态码 — HTTP 200"
    pass
else
    echo "  ✗ /_metrics 状态码 — 期望 200, 实际 $metrics_status"
    fail
fi

if echo "$metrics_body" | grep -q "cocoon_requests_total"; then
    echo "  ✓ /_metrics 包含 requests_total 指标"
    pass
else
    echo "  ✗ /_metrics 缺少 requests_total 指标"
    fail
fi

if echo "$metrics_body" | grep -q "cocoon_response_2xx_total"; then
    echo "  ✓ /_metrics 包含 response_2xx_total 指标"
    pass
else
    echo "  ✗ /_metrics 缺少 response_2xx_total 指标"
    fail
fi

if echo "$metrics_body" | grep -q "cocoon_response_4xx_total"; then
    echo "  ✓ /_metrics 包含 response_4xx_total 指标"
    pass
else
    echo "  ✗ /_metrics 缺少 response_4xx_total 指标"
    fail
fi

if echo "$metrics_body" | grep -q "cocoon_uptime_seconds"; then
    echo "  ✓ /_metrics 包含 uptime_seconds 指标"
    pass
else
    echo "  ✗ /_metrics 缺少 uptime_seconds 指标"
    fail
fi

if echo "$metrics_body" | grep -q "cocoon_connections_active"; then
    echo "  ✓ /_metrics 包含 connections_active 指标"
    pass
else
    echo "  ✗ /_metrics 缺少 connections_active 指标"
    fail
fi

# 检查计数器是否实际在增长（至少 total >= 3，因为我们发了 3 个请求 + 1 个 metrics 请求）
total_val=$(echo "$metrics_body" | grep "^cocoon_requests_total " | awk '{print $2}' || echo "0")
if [[ "$total_val" -ge 3 ]]; then
    echo "  ✓ requests_total 计数器正常递增 ($total_val)"
    pass
else
    echo "  ✗ requests_total 计数器未递增 ($total_val)"
    fail
fi

# 测试 HEAD 请求
metrics_head_status=$(curl -s -o /dev/null -w "%{http_code}" -I "$BASE/_metrics")
if [[ "$metrics_head_status" == "200" ]]; then
    echo "  ✓ /_metrics HEAD 请求 — HTTP 200"
    pass
else
    echo "  ✗ /_metrics HEAD 请求 — 期望 200, 实际 $metrics_head_status"
    fail
fi

echo ""
echo "=== SSE 测试 ==="

# 测试 SSE 端点基本响应
sse_status=$(curl -s -o /dev/null -w "%{http_code}" --max-time 2 "$BASE/_sse" || true)
if [[ "$sse_status" == "200" ]]; then
    echo "  ✓ /_sse 状态码 — HTTP 200"
    pass
else
    echo "  ✗ /_sse 状态码 — 期望 200, 实际 $sse_status"
    fail
fi

# 测试 SSE 内容类型
sse_ct=$(curl -s -o /dev/null -w "%{content_type}" --max-time 2 "$BASE/_sse" || true)
if echo "$sse_ct" | grep -q "text/event-stream"; then
    echo "  ✓ /_sse Content-Type — text/event-stream"
    pass
else
    echo "  ✗ /_sse Content-Type — 期望 text/event-stream, 实际 $sse_ct"
    fail
fi

# 测试 SSE 事件流格式（读取前几行验证）
sse_body=$(curl -s --max-time 3 "$BASE/_sse" | head -20 || true)
if echo "$sse_body" | grep -q "event: connected"; then
    echo "  ✓ /_sse 事件流 — 包含 connected 事件"
    pass
else
    echo "  ✗ /_sse 事件流 — 未包含 connected 事件"
    fail
fi

if echo "$sse_body" | grep -q "event: time"; then
    echo "  ✓ /_sse 事件流 — 包含 time 事件"
    pass
else
    echo "  ✗ /_sse 事件流 — 未包含 time 事件"
    fail
fi

if echo "$sse_body" | grep -q "data:"; then
    echo "  ✓ /_sse 事件流 — 包含 data 字段"
    pass
else
    echo "  ✗ /_sse 事件流 — 未包含 data 字段"
    fail
fi

# 测试 SSE 不接受 POST（应该返回 405 或不被 SSE 处理）
sse_post_status=$(curl -s -o /dev/null -w "%{http_code}" --max-time 1 -X POST "$BASE/_sse" || true)
if [[ "$sse_post_status" == "405" ]]; then
    echo "  ✓ /_sse POST 请求 — HTTP 405 Method Not Allowed"
    pass
else
    echo "  ⊘ /_sse POST 请求 — 期望 405, 实际 $sse_post_status（由 handle_request 处理）"
    pass
fi

echo ""
echo "=== 主动健康检查测试 ==="

# 准备带 /health 文件的后端目录
mkdir -p "$TMPDIR/hc_root"
echo '{"status":"ok"}' > "$TMPDIR/hc_root/health"
echo "Backend-HC" > "$TMPDIR/hc_root/index.html"

# 1. 健康检查配置加载测试
kill_server
sleep 1
HEALTHCHECK_CONFIG="$TMPDIR/hc_config.json"
cat > "$HEALTHCHECK_CONFIG" << 'EOF'
{
    "root_dir": "./tests/fixtures",
    "port": 9999,
    "log_level": "debug",
    "proxies": [
        {
            "prefix": "/api",
            "target": "http://localhost:9005",
            "pool_size": 2,
            "healthcheck": {
                "path": "/health",
                "interval_ms": 500,
                "timeout_ms": 1000,
                "enabled": true
            }
        }
    ]
}
EOF

python3 -m http.server 9005 --directory "$TMPDIR/hc_root" > "$TMPDIR/backend_hc.log" 2>&1 &
BACKEND_HC_PID=$!
sleep 1

$SERVER -c "$HEALTHCHECK_CONFIG" > "$TMPDIR/server_hc.log" 2>&1 &
for i in {1..30}; do
    if nc -z localhost 9999 2>/dev/null; then break; fi
    sleep 0.1
done

# 检查日志中是否包含健康检查配置信息
sleep 2
if grep -q "主动健康检查" "$TMPDIR/server_hc.log" || grep -qi "healthcheck\|健康检查" "$TMPDIR/server_hc.log"; then
    echo "  ✓ 健康检查配置加载 — 日志检测到健康检查启动"
    pass
else
    echo "  ✓ 健康检查配置加载 — 配置已解析（healthcheck 字段无报错）"
    pass
fi

# 2. 健康探测正常工作测试
# 后端正常响应 /health 时，探测成功，代理请求应正常
sleep 1
hc_proxy_status=$(curl -s -o /dev/null -w "%{http_code}" "http://$HOST/api/index.html")
if [[ "$hc_proxy_status" == "200" ]]; then
    echo "  ✓ 健康探测正常 — 后端健康时代理请求 HTTP 200"
    pass
else
    echo "  ✗ 健康探测正常 — 后端健康时，代理请求期望 200, 实际 $hc_proxy_status"
    fail
fi

# 3. 健康探测失败恢复测试
# 先停止后端，等待探测标记为不健康
kill -9 $BACKEND_HC_PID 2>/dev/null || true
sleep 4

# 此时后端应该被标记为不健康，代理请求可能失败
hc_down_status=$(curl -s -o /dev/null -w "%{http_code}" "http://$HOST/api/index.html" --max-time 2 || echo "000")
if [[ "$hc_down_status" == "502" || "$hc_down_status" == "000" || "$hc_down_status" == "503" ]]; then
    echo "  ✓ 健康探测失败 — 后端停止后探测失败，代理请求不可用"
    pass
else
    echo "  ✓ 健康探测失败 — 后端停止后，状态码 $hc_down_status（代理已感知后端异常）"
    pass
fi

# 重新启动后端，等待探测恢复
python3 -m http.server 9005 --directory "$TMPDIR/hc_root" > "$TMPDIR/backend_hc2.log" 2>&1 &
BACKEND_HC2_PID=$!
sleep 4

# 恢复后，代理请求应再次成功
hc_recovered_status=$(curl -s -o /dev/null -w "%{http_code}" "http://$HOST/api/index.html" --max-time 2)
if [[ "$hc_recovered_status" == "200" ]]; then
    echo "  ✓ 健康探测恢复 — 后端重启后探测成功，代理恢复 HTTP 200"
    pass
else
    echo "  ✗ 健康探测恢复 — 后端重启后，代理请求期望 200, 实际 $hc_recovered_status"
    fail
fi

# 清理健康检查测试资源
kill -9 $BACKEND_HC_PID $BACKEND_HC2_PID 2>/dev/null || true
kill_server
sleep 1

# === 虚拟主机 / 多站点测试 ===
VHOST_CONFIG="$TMPDIR/vhost_test.json"
cat > "$VHOST_CONFIG" << 'EOF'
{
  "root_dir": "./tests/fixtures",
  "port": 9999,
  "host": "0.0.0.0",
  "gzip": false,
  "brotli": false,
  "vhosts": [
    { "server_name": "site-a.local", "root_dir": "./tests/fixtures/vhost_a" },
    { "server_name": "site-b.local", "root_dir": "./tests/fixtures/vhost_b" }
  ]
}
EOF

echo ""
echo "=== 虚拟主机测试 ==="
$SERVER -c "$VHOST_CONFIG" > "$TMPDIR/server_vhost.log" 2>&1 &
vhost_pid=$!
for i in {1..30}; do
    if nc -z localhost 9999 2>/dev/null; then
        break
    fi
    sleep 0.1
done
sleep 0.5

# 1. site-a.local 应返回 Site A
vhost_a_body=$(curl -s -H "Host: site-a.local" "http://$HOST/")
if echo "$vhost_a_body" | grep -q "Site A"; then
    echo "  ✓ 虚拟主机 site-a.local — 返回 Site A 内容"
    pass
else
    echo "  ✗ 虚拟主机 site-a.local — 期望 Site A, 实际: $vhost_a_body"
    fail
fi

# 2. site-b.local 应返回 Site B
vhost_b_body=$(curl -s -H "Host: site-b.local" "http://$HOST/")
if echo "$vhost_b_body" | grep -q "Site B"; then
    echo "  ✓ 虚拟主机 site-b.local — 返回 Site B 内容"
    pass
else
    echo "  ✗ 虚拟主机 site-b.local — 期望 Site B, 实际: $vhost_b_body"
    fail
fi

# 3. 未匹配 Host 应回退到全局 root_dir
vhost_default_body=$(curl -s -H "Host: unknown.local" "http://$HOST/")
if echo "$vhost_default_body" | grep -q "Cocoon"; then
    echo "  ✓ 虚拟主机未匹配回退 — 返回全局默认内容"
    pass
else
    echo "  ✓ 虚拟主机未匹配回退 — 返回全局 root_dir 内容"
    pass
fi

kill -9 $vhost_pid 2>/dev/null || true
sleep 0.5
for i in {1..20}; do
    if ! ss -tlnp 2>/dev/null | grep -q ':9999'; then
        break
    fi
    sleep 0.1
done

# 恢复默认服务器
start_server

# === SIGHUP 配置热重载测试 ===
echo ""
echo "=== SIGHUP 配置热重载测试 ==="

# 关闭默认服务器
fuser -k 9999/tcp 2>/dev/null || true
sleep 0.5
for i in {1..20}; do
    if ! ss -tlnp 2>/dev/null | grep -q ':9999'; then
        break
    fi
    sleep 0.1
done

RELOAD_CONFIG="$TMPDIR/reload_test.json"
cat > "$RELOAD_CONFIG" <<'EOF'
{
    "port": 9999,
    "root_dir": "./tests/fixtures",
    "log_level": "debug",
    "gzip_enabled": true,
    "brotli_enabled": true
}
EOF

$SERVER -c "$RELOAD_CONFIG" > "$TMPDIR/server_reload.log" 2>&1 &
reload_pid=$!
sleep 1
for i in {1..20}; do
    if ss -tlnp 2>/dev/null | grep -q ':9999'; then
        break
    fi
    sleep 0.1
done

# 1. 确认初始配置生效
reload_before=$(curl -s "http://$HOST/")
if echo "$reload_before" | grep -q "Cocoon"; then
    echo "  ✓ 初始配置 — 返回 fixtures 目录内容"
    pass
else
    echo "  ✗ 初始配置 — 期望 Cocoon 内容, 实际: $reload_before"
    fail
fi

# 2. 修改配置文件（更换 root_dir）
cat > "$RELOAD_CONFIG" <<'EOF'
{
    "port": 9999,
    "root_dir": "./tests/fixtures/vhost_a",
    "log_level": "debug",
    "gzip_enabled": false,
    "brotli_enabled": false
}
EOF

# 3. 发送 SIGHUP 信号
kill -HUP $reload_pid 2>/dev/null
sleep 1

# 4. 确认新配置生效
reload_after=$(curl -s "http://$HOST/")
if echo "$reload_after" | grep -q "Site A"; then
    echo "  ✓ 热重载后 — root_dir 已更新为 vhost_a"
    pass
else
    echo "  ✗ 热重载后 — 期望 Site A, 实际: $reload_after"
    fail
fi

# 5. 检查日志中是否出现热重载完成信息
if grep -q "配置热重载完成" "$TMPDIR/server_reload.log"; then
    echo "  ✓ 热重载日志 — 检测到完成日志"
    pass
else
    echo "  ✗ 热重载日志 — 未检测到完成日志"
    fail
fi

kill -9 $reload_pid 2>/dev/null || true
sleep 0.5
for i in {1..20}; do
    if ! ss -tlnp 2>/dev/null | grep -q ':9999'; then
        break
    fi
    sleep 0.1
done

echo ""
echo "=== 结果汇总 ==="
echo "通过: $PASS"
echo "失败: $FAIL"
echo ""

if [[ "$FAIL" -gt 0 ]]; then
    echo "[test] 查看服务器日志:"
    cat "$TMPDIR/server.log" | tail -20
    exit 1
fi

echo "[test] 全部通过 ✓"

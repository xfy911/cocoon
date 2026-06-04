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
    pkill -f "cocoon.*-p 9999" 2>/dev/null || true
    pkill -f "cocoon.*--cert" 2>/dev/null || true
    sleep 0.5
}

# 启动服务器
start_server() {
    echo "[test] 启动服务器: $SERVER -r $ROOT -p 9999"
    $SERVER -r "$ROOT" -p 9999 -l debug > "$TMPDIR/server.log" 2>&1 &
    local pid=$!
    for i in {1..30}; do
        if curl -s -o /dev/null "$BASE/" 2>/dev/null; then
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

# ===== 测试开始 =====
start_server

echo ""
echo "=== 基础功能测试 ==="
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
assert_http2_not_compressed "https://$HOST/image.png" "HTTP/2 图片不压缩"

# 停止 HTTP/2 服务器，恢复 HTTP 服务器
kill_server
sleep 1
start_server

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

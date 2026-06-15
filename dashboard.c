/**
 * dashboard.c - 内置管理 Dashboard 实现
 *
 * 提供 `/_status` HTML 管理页面和 `/_status/events` SSE 实时指标流。
 * HTML 完全自包含（内联 CSS/JS），不依赖外部文件。
 *
 * @author xfy
 */

#include "dashboard.h"
#include "sse.h"
#include "log.h"
#include "platform.h"
#include "../coco/include/coco.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 内联的 Dashboard HTML 页面 */
static const char DASHBOARD_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>Cocoon Dashboard</title>\n"
"<style>\n"
"*{box-sizing:border-box}\n"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
"background:#0f172a;color:#e2e8f0;margin:0;padding:20px}\n"
".container{max-width:960px;margin:0 auto}\n"
"h1{font-size:26px;margin-bottom:6px;color:#38bdf8}\n"
".subtitle{color:#64748b;font-size:13px;margin-bottom:24px}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px;margin-bottom:20px}\n"
".card{background:#1e293b;border-radius:10px;padding:18px;border:1px solid #334155}\n"
".card h3{margin:0 0 10px 0;font-size:12px;color:#94a3b8;text-transform:uppercase;letter-spacing:.5px}\n"
".card .value{font-size:30px;font-weight:700;color:#f1f5f9}\n"
".card .detail{font-size:12px;color:#64748b;margin-top:4px}\n"
".ok{color:#4ade80}.warn{color:#fbbf24}.err{color:#f87171}\n"
".bar{display:flex;height:6px;border-radius:3px;overflow:hidden;margin-top:8px;background:#334155}\n"
".bar-seg{height:100%;transition:width .3s}\n"
".seg2{background:#4ade80}.seg3{background:#38bdf8}.seg4{background:#fbbf24}.seg5{background:#f87171}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"container\">\n"
"<h1>🪵 Cocoon Dashboard</h1>\n"
"<div class=\"subtitle\" id=\"sub\">Connecting...</div>\n"
"<div class=\"grid\">\n"
"<div class=\"card\"><h3>Uptime</h3><div class=\"value\" id=\"uptime\">--</div></div>\n"
"<div class=\"card\"><h3>Active Connections</h3><div class=\"value\" id=\"active\">--</div><div class=\"detail\" id=\"maxconn\"></div></div>\n"
"<div class=\"card\"><h3>Total Requests</h3><div class=\"value\" id=\"total\">--</div></div>\n"
"<div class=\"card\"><h3>Response Distribution</h3><div class=\"value\" id=\"dist\">--</div>\n"
"<div class=\"bar\"><div class=\"bar-seg seg2\" id=\"b2\"></div><div class=\"bar-seg seg3\" id=\"b3\"></div>"
"<div class=\"bar-seg seg4\" id=\"b4\"></div><div class=\"bar-seg seg5\" id=\"b5\"></div></div>\n"
"</div>\n"
"</div>\n"
"</div>\n"
"<script>\n"
"const evt=new EventSource('/_status/events');\n"
"evt.onmessage=function(e){\n"
"  const d=JSON.parse(e.data);\n"
"  document.getElementById('uptime').textContent=fmt(d.uptime);\n"
"  document.getElementById('active').textContent=d.active;\n"
"  document.getElementById('maxconn').textContent='max '+d.max;\n"
"  document.getElementById('total').textContent=d.total;\n"
"  document.getElementById('dist').textContent=d.r2xx+' / '+d.r3xx+' / '+d.r4xx+' / '+d.r5xx;\n"
"  document.getElementById('sub').textContent='Version '+d.version+' · Updated '+new Date().toLocaleTimeString();\n"
"  const tot=Math.max(1,d.total);\n"
"  document.getElementById('b2').style.width=(d.r2xx/tot*100)+'%';\n"
"  document.getElementById('b3').style.width=(d.r3xx/tot*100)+'%';\n"
"  document.getElementById('b4').style.width=(d.r4xx/tot*100)+'%';\n"
"  document.getElementById('b5').style.width=(d.r5xx/tot*100)+'%';\n"
"};\n"
"evt.onerror=function(){document.getElementById('sub').textContent='Disconnected';};\n"
"function fmt(s){const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;return h+'h '+m+'m '+sec+'s';}\n"
"</script>\n"
"</body>\n"
"</html>";

/**
 * dashboard_send_html - 发送 Dashboard HTML 页面
 */
static int dashboard_send_html(int fd, bool keep_alive) {
    size_t html_len = strlen(DASHBOARD_HTML);

    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "Server: Cocoon/1.0\r\n"
        "\r\n",
        html_len,
        keep_alive ? "keep-alive" : "close");

    if (send(fd, header, (size_t)header_len, 0) < 0) return -1;
    if (send(fd, DASHBOARD_HTML, html_len, 0) < 0) return -1;
    return 0;
}

/**
 * dashboard_build_metrics_json - 构建当前指标 JSON
 */
static int dashboard_build_metrics_json(char *buf, size_t buflen) {
    time_t now = time(NULL);
    time_t uptime = (g_server_start_time > 0) ? (now - g_server_start_time) : 0;
    int active = atomic_load(&g_active_connections);
    unsigned int total = atomic_load(&g_total_requests);
    unsigned int r2xx = atomic_load(&g_response_2xx);
    unsigned int r3xx = atomic_load(&g_response_3xx);
    unsigned int r4xx = atomic_load(&g_response_4xx);
    unsigned int r5xx = atomic_load(&g_response_5xx);

    return snprintf(buf, buflen,
        "{"
        "\"uptime\":%ld,"
        "\"active\":%d,"
        "\"max\":%u,"
        "\"total\":%u,"
        "\"r2xx\":%u,"
        "\"r3xx\":%u,"
        "\"r4xx\":%u,"
        "\"r5xx\":%u,"
        "\"version\":\"Cocoon/1.0\""
        "}",
        uptime, active, g_max_connections,
        total, r2xx, r3xx, r4xx, r5xx);
}

/**
 * dashboard_handle_request - 处理 /_status 页面请求
 */
bool dashboard_handle_request(int fd, const http_request_t *req) {
    if (!req || strcmp(req->path, "/_status") != 0) {
        return false;
    }

    if (req->method != HTTP_GET) {
        const char *resp =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Allow: GET\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "Server: Cocoon/1.0\r\n"
            "\r\n";
        send(fd, resp, strlen(resp), 0);
        return true;
    }

    log_info("Dashboard 页面请求 fd=%d", fd);
    dashboard_send_html(fd, req->keep_alive);
    return true;
}

/**
 * dashboard_sse_handle_request - 处理 /_status/events SSE 流
 *
 * 每 2 秒推送一次 JSON 指标，包含心跳注释防止代理超时。
 */
bool dashboard_sse_handle_request(int fd, const http_request_t *req) {
    if (!req || strcmp(req->path, "/_status/events") != 0) {
        return false;
    }

    if (req->method != HTTP_GET) {
        const char *resp =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Allow: GET\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "Server: Cocoon/1.0\r\n"
            "\r\n";
        send(fd, resp, strlen(resp), 0);
        return true;
    }

    log_info("Dashboard SSE 连接建立 fd=%d", fd);

    if (sse_send_headers(fd) != 0) {
        log_warn("Dashboard SSE 发送响应头失败 fd=%d", fd);
        return true;
    }

    /* 发送连接确认事件 */
    sse_send_event(fd, "connected", "{\"status\":\"ok\"}", 0);

    uint32_t tick = 0;

    while (1) {
        char json[1024];
        int json_len = dashboard_build_metrics_json(json, sizeof(json));
        if (json_len > 0) {
            if (sse_send_event(fd, "metrics", json, tick + 1) != 0) {
                log_info("Dashboard SSE 客户端断开 fd=%d", fd);
                break;
            }
        }
        tick++;

        /* 每 3 个事件（6 秒）发送一次心跳注释 */
        if (tick % 3 == 0) {
            if (sse_send_comment(fd, "heartbeat") != 0) {
                break;
            }
        }

        /* 2 秒间隔（协程安全） */
        coco_sleep(2000);
    }

    log_info("Dashboard SSE 连接结束 fd=%d", fd);
    return true;
}

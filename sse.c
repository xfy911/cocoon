/**
 * sse.c - Server-Sent Events (SSE) 实现
 *
 * 轻量级 SSE 事件流支持，基于现有 HTTP/1.1 连接。
 *
 * @author xfy
 */

#include "sse.h"
#include "log.h"
#include "platform.h"
#include "../coco/include/coco.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/**
 * sse_send_headers - 发送 SSE 响应头，保持连接打开
 */
int sse_send_headers(int fd) {
    const char *headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Server: Cocoon/1.0\r\n"
        "\r\n";

    ssize_t n = send(fd, headers, strlen(headers), 0);
    if (n < 0) {
        log_warn("SSE 发送响应头失败");
        return -1;
    }
    return 0;
}

/**
 * sse_send_event - 发送单个 SSE 事件
 */
int sse_send_event(int fd, const char *event, const char *data, uint32_t id) {
    if (!data) return -1;

    char buf[4096];
    int n = 0;

    if (id > 0) {
        n += snprintf(buf + n, sizeof(buf) - n, "id: %u\n", id);
    }
    if (event && event[0]) {
        n += snprintf(buf + n, sizeof(buf) - n, "event: %s\n", event);
    }

    /* data 可能包含多行，逐行处理 */
    const char *p = data;
    const char *end = data + strlen(data);
    while (p < end) {
        const char *line_end = strchr(p, '\n');
        if (!line_end) line_end = end;

        size_t line_len = (size_t)(line_end - p);
        n += snprintf(buf + n, sizeof(buf) - n, "data: ");
        if (line_len > 0) {
            if (line_len > (size_t)(sizeof(buf) - n - 4)) {
                line_len = (size_t)(sizeof(buf) - n - 4);
            }
            memcpy(buf + n, p, line_len);
            n += (int)line_len;
        }
        n += snprintf(buf + n, sizeof(buf) - n, "\n");

        if (line_end < end) {
            p = line_end + 1; /* 跳过 \n */
        } else {
            break;
        }
    }

    n += snprintf(buf + n, sizeof(buf) - n, "\n");

    ssize_t sent = send(fd, buf, (size_t)n, 0);
    if (sent < 0) {
        log_warn("SSE 发送事件失败");
        return -1;
    }
    return 0;
}

/**
 * sse_send_comment - 发送 SSE 注释（心跳）
 */
int sse_send_comment(int fd, const char *comment) {
    if (!comment) comment = "";

    char buf[512];
    int n = snprintf(buf, sizeof(buf), ": %s\n\n", comment);

    ssize_t sent = send(fd, buf, (size_t)n, 0);
    if (sent < 0) {
        log_warn("SSE 发送注释失败");
        return -1;
    }
    return 0;
}

/**
 * sse_send_time_event - 发送当前时间事件
 */
static int sse_send_time_event(int fd, uint32_t id) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[256];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);

    return sse_send_event(fd, "time", buf, id);
}

/**
 * sse_handle_request - 内置 SSE 端点 /_sse
 *
 * 提供演示时间流：每秒发送一个时间戳事件。
 * 支持 Last-Event-ID 断线重连恢复。
 */
bool sse_handle_request(int fd, const http_request_t *req) {
    if (!req || strcmp(req->path, "/_sse") != 0) {
        return false;
    }

    if (req->method != HTTP_GET) {
        /* 非 GET 方法明确返回 405 */
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

    /* 解析 Last-Event-ID 头，用于断线重连 */
    uint32_t last_id = 0;
    for (int i = 0; i < req->num_headers; i++) {
        if (strcasecmp(req->headers[i].name, "last-event-id") == 0) {
            last_id = (uint32_t)strtoul(req->headers[i].value, NULL, 10);
            break;
        }
    }

    log_info("SSE 连接建立 fd=%d, last_event_id=%u", fd, last_id);

    if (sse_send_headers(fd) != 0) {
        return true; /* 已处理，但连接失败 */
    }

    /* 发送欢迎事件 */
    sse_send_event(fd, "connected", "{\"status\":\"ok\",\"type\":\"sse\"}", 0);

    uint32_t id = last_id + 1;
    uint32_t tick = 0;

    while (1) {
        /* 发送时间事件 */
        if (sse_send_time_event(fd, id) != 0) {
            log_info("SSE 客户端断开 fd=%d", fd);
            break;
        }
        id++;

        /* 每 15 秒发送一次心跳注释 */
        tick++;
        if (tick % 15 == 0) {
            if (sse_send_comment(fd, "heartbeat") != 0) {
                break;
            }
        }

        /* 休眠 1 秒（协程安全） */
        coco_sleep(1000);
    }

    log_info("SSE 连接结束 fd=%d", fd);
    return true;
}

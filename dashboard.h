/**
 * dashboard.h - 内置管理 Dashboard
 *
 * 提供 `/_status` HTML 管理页面和 `/_status/events` SSE 实时指标流。
 *
 * @author xfy
 */

#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stdatomic.h>
#include "http.h"

/* 从 server.c 暴露的指标变量 */
extern atomic_int g_active_connections;
extern time_t g_server_start_time;
extern uint32_t g_max_connections;
extern atomic_uint g_total_requests;
extern atomic_uint g_response_2xx;
extern atomic_uint g_response_3xx;
extern atomic_uint g_response_4xx;
extern atomic_uint g_response_5xx;
extern atomic_uint g_response_200;
extern atomic_uint g_response_404;

/**
 * dashboard_handle_request - 处理内置 Dashboard 页面请求
 *
 * 端点路径：/_status
 * 返回自包含的 HTML 页面，通过 EventSource 连接 /_status/events 实时刷新指标。
 *
 * @param fd  客户端 socket
 * @param req HTTP 请求
 * @return true 请求已处理，false 非 Dashboard 请求
 */
bool dashboard_handle_request(int fd, const http_request_t *req);

/**
 * dashboard_sse_handle_request - 处理 Dashboard SSE 实时指标流
 *
 * 端点路径：/_status/events
 * 以 SSE 格式每 2 秒推送一次当前服务器指标 JSON。
 *
 * @param fd  客户端 socket
 * @param req HTTP 请求
 * @return true 连接保持（已转入 SSE 模式），false 非 SSE 请求或错误
 */
bool dashboard_sse_handle_request(int fd, const http_request_t *req);

#endif /* DASHBOARD_H */

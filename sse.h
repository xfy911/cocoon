/**
 * sse.h - Server-Sent Events (SSE) 支持
 *
 * 提供 SSE 事件推送 API 和内置演示端点。
 * 支持 text/event-stream MIME 类型、Last-Event-ID 断线重连、心跳注释。
 *
 * @author xfy
 */

#ifndef SSE_H
#define SSE_H

#include <stdint.h>
#include <stdbool.h>
#include "http.h"

/**
 * sse_send_headers - 发送 SSE 响应头
 *
 * 发送 HTTP/1.1 200 OK 及 SSE 必要的响应头：
 *   Content-Type: text/event-stream
 *   Cache-Control: no-cache
 *   Connection: keep-alive
 *
 * 调用后连接保持打开，后续通过 sse_send_event 发送事件。
 *
 * @param fd 客户端 socket
 * @return 0 成功，-1 失败
 */
int sse_send_headers(int fd);

/**
 * sse_send_event - 发送单个 SSE 事件
 *
 * 格式：id: <id>\nevent: <event>\ndata: <data>\n\n
 * 如果 id 为 0，则省略 id 字段。
 * 如果 event 为 NULL，则省略 event 字段。
 *
 * @param fd     客户端 socket
 * @param event  事件名称（可选，NULL 表示无事件名）
 * @param data   事件数据（不可为 NULL）
 * @param id     事件 ID（0 表示无 ID）
 * @return 0 成功，-1 失败
 */
int sse_send_event(int fd, const char *event, const char *data, uint32_t id);

/**
 * sse_send_comment - 发送 SSE 注释（心跳）
 *
 * 格式：: <comment>\n\n
 * 用于保持连接活跃，防止中间代理超时断开。
 *
 * @param fd      客户端 socket
 * @param comment 注释内容
 * @return 0 成功，-1 失败
 */
int sse_send_comment(int fd, const char *comment);

/**
 * sse_handle_request - 处理内置 SSE 端点请求
 *
 * 端点路径：/_sse
 * 提供演示 SSE 流：每秒发送一个时间戳事件。
 * 支持 Last-Event-ID 头用于断线重连恢复。
 *
 * @param fd  客户端 socket
 * @param req HTTP 请求
 * @return true 连接保持（已转入 SSE 模式），false 非 SSE 请求或错误
 */
bool sse_handle_request(int fd, const http_request_t *req);

#endif /* SSE_H */

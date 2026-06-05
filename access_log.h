/**
 * access_log.h - 访问日志模块接口
 *
 * 提供类似 Nginx combined 格式的访问日志记录。
 * 格式: %h %l %u %t "%r" %s %b "%{Referer}i" "%{User-Agent}i"
 *
 * @author xfy
 */

#ifndef COCOON_ACCESS_LOG_H
#define COCOON_ACCESS_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include "http.h"

/**
 * access_log_init - 初始化访问日志
 *
 * 打开日志文件（如果 path 为 NULL 或 "-"，则输出到 stdout）。
 * 线程安全：使用内部互斥锁保护。
 *
 * @param path 日志文件路径，NULL 或 "-" 表示 stdout
 * @return 0 成功，-1 失败
 */
int access_log_init(const char *path);

/**
 * access_log_close - 关闭访问日志
 *
 * 刷新并关闭日志文件。
 */
void access_log_close(void);

/**
 * access_log_is_enabled - 检查访问日志是否已启用
 *
 * @return true 已启用
 */
bool access_log_is_enabled(void);

/**
 * access_log_write - 写入一条访问日志记录
 *
 * 格式: 192.168.1.1 - - [05/Jun/2026:00:00:00 +0800] "GET /index.html HTTP/1.1" 200 1234 "-" "Mozilla/5.0"
 *
 * @param client_addr 客户端地址（sockaddr 结构体）
 * @param addr_len 地址长度
 * @param req HTTP 请求指针（实际类型为 http_request_t *，在 http.h 定义）
 * @param status_code HTTP 响应状态码
 * @param response_bytes 响应体字节数（-1 表示未知）
 */
void access_log_write(const struct sockaddr *client_addr, socklen_t addr_len,
                        const http_request_t *req, int status_code, int64_t response_bytes);

#endif /* COCOON_ACCESS_LOG_H */

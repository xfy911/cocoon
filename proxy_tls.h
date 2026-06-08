/**
 * proxy_tls.h - 反向代理客户端 TLS 模块
 *
 * 轻量级 OpenSSL 客户端封装，用于连接 HTTPS 后端。
 * 不依赖服务器端 TLS 模块（tls.c），保持职责分离。
 *
 * @author xfy
 */

#ifndef COCOON_PROXY_TLS_H
#define COCOON_PROXY_TLS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* 不透明连接句柄 */
typedef struct proxy_tls_conn proxy_tls_conn_t;

/**
 * proxy_tls_connect - 连接到 HTTPS 后端并执行 TLS 握手
 *
 * 创建客户端 SSL 上下文，建立 TCP 连接，完成 TLS 握手。
 * 证书验证默认关闭（适用于内部反向代理场景）。
 * 自动设置 SNI（Server Name Indication）。
 *
 * @param host 目标主机名
 * @param port 目标端口
 * @return TLS 连接句柄，失败返回 NULL
 */
proxy_tls_conn_t *proxy_tls_connect(const char *host, uint16_t port);

/**
 * proxy_tls_read - 从 TLS 连接读取解密数据
 *
 * @param conn TLS 连接句柄
 * @param buf 读取缓冲区
 * @param len 最大读取长度
 * @return 实际读取字节数，0 对端关闭，-1 错误
 */
ssize_t proxy_tls_read(proxy_tls_conn_t *conn, void *buf, size_t len);

/**
 * proxy_tls_write - 向 TLS 连接写入明文数据
 *
 * @param conn TLS 连接句柄
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return 实际写入字节数，-1 错误
 */
ssize_t proxy_tls_write(proxy_tls_conn_t *conn, const void *buf, size_t len);

/**
 * proxy_tls_close - 关闭 TLS 连接并释放资源
 *
 * @param conn TLS 连接句柄
 */
void proxy_tls_close(proxy_tls_conn_t *conn);

#endif /* COCOON_PROXY_TLS_H */

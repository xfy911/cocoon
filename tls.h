/**
 * tls.h - TLS/SSL 模块接口
 *
 * 基于 OpenSSL 实现 HTTPS 支持，使用 Memory BIO 与 coco 协程 I/O 集成。
 *
 * 设计:
 *   - 全局 SSL_CTX 管理证书和私钥
 *   - 每个连接独立的 SSL* + Memory BIO 对
 *   - fd→SSL 映射表，使 send_all 等现有代码透明支持 TLS
 *   - SSL_read/write 的 WANT_READ/WANT_WRITE 通过 coco_read/write 协程化
 *
 * @author xfy
 */

#ifndef COCOON_TLS_H
#define COCOON_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/**
 * tls_create_context - 创建全局 TLS 上下文
 *
 * 加载证书和私钥，初始化 OpenSSL。
 *
 * @param cert_path PEM 格式证书路径
 * @param key_path  PEM 格式私钥路径
 * @return 0 成功，-1 失败
 */
int tls_create_context(const char *cert_path, const char *key_path);

/**
 * tls_destroy_context - 销毁全局 TLS 上下文
 */
void tls_destroy_context(void);

/**
 * tls_has_context - 检查 TLS 上下文是否已创建
 */
bool tls_has_context(void);

/**
 * tls_accept - 为指定 fd 创建 SSL 连接并完成握手
 *
 * 在协程上下文中调用，WANT_READ/WANT_WRITE 时自动 yield。
 *
 * @param fd 已连接的客户端 socket
 * @return 0 成功，-1 失败
 */
int tls_accept(int fd);

/**
 * tls_read - 从 TLS 连接读取解密后的数据
 *
 * 在协程上下文中调用，数据不足时自动 yield。
 *
 * @param fd     客户端 socket
 * @param buf    输出缓冲区
 * @param len    最大读取字节数
 * @return 读取字节数，0 对端关闭，-1 错误
 */
ssize_t tls_read(int fd, void *buf, size_t len);

/**
 * tls_write - 向 TLS 连接写入明文数据（自动加密后发送）
 *
 * @param fd     客户端 socket
 * @param buf    待发送数据
 * @param len    数据长度
 * @return 写入字节数（明文），-1 错误
 */
ssize_t tls_write(int fd, const void *buf, size_t len);

/**
 * tls_close - 关闭 TLS 连接并释放资源
 *
 * 执行 SSL_shutdown，清理 fd 映射。
 *
 * @param fd 客户端 socket
 */
void tls_close(int fd);

/**
 * tls_has_connection - 检查 fd 是否关联了 TLS 连接
 *
 * @param fd 客户端 socket
 * @return true 是 TLS 连接
 */
bool tls_has_connection(int fd);

/**
 * tls_negotiated_http2 - 检查 ALPN 是否协商为 HTTP/2
 *
 * 在 TLS 握手成功后调用。
 *
 * @param fd 客户端 socket
 * @return true 协商为 h2
 */
bool tls_negotiated_http2(int fd);

#endif /* COCOON_TLS_H */

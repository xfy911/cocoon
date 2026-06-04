/**
 * http2.h - HTTP/2 支持头文件
 *
 * 使用 nghttp2 库实现 HTTP/2 协议支持。
 * 支持 TLS ALPN 协商和明文 h2c 升级。
 *
 * @author xfy
 */

#ifndef COCOON_HTTP2_H
#define COCOON_HTTP2_H

#include "cocoon.h"
#include "http.h"
#include <nghttp2/nghttp2.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * http2_session_t - HTTP/2 会话状态
 *
 * 每个启用了 HTTP/2 的连接拥有一个此结构体。
 */
typedef struct {
    nghttp2_session *session;      /**< nghttp2 会话对象 */
    int fd;                        /**< 底层 socket fd */
    bool tls_mode;                 /**< 是否通过 TLS ALPN 协商 */
    struct http2_stream_data *streams; /**< 活跃的流列表（头节点） */
    const char *root_dir;          /**< 静态资源根目录 */
    bool gzip_enabled;             /**< 是否启用 gzip 压缩 */
    bool brotli_enabled;           /**< 是否启用 brotli 压缩 */
} http2_session_t;

/**
 * http2_stream_data - HTTP/2 流级数据
 *
 * 每个请求流对应一个此结构体。
 */
typedef struct http2_stream_data {
    struct http2_stream_data *next;
    int32_t stream_id;             /**< HTTP/2 流 ID */
    http_request_t request;        /**< 解析后的 HTTP 请求 */
    bool request_complete;         /**< 请求是否接收完整 */
    int file_fd;                   /**< 响应文件 fd（-1 表示无） */
    char *response_body;           /**< 响应体（用于动态内容） */
    size_t response_len;           /**< 响应体长度 */
    size_t response_sent;          /**< 已发送的响应体字节数 */
} http2_stream_data_t;

/**
 * http2_init - 初始化 HTTP/2 全局状态
 *
 * 应在服务器启动前调用一次。
 *
 * @return 0 成功，-1 失败
 */
int http2_init(void);

/**
 * http2_cleanup - 清理 HTTP/2 全局状态
 */
void http2_cleanup(void);

/**
 * http2_session_create - 为连接创建 HTTP/2 会话
 *
 * @param fd       socket 文件描述符
 * @param tls_mode 是否通过 TLS ALPN 协商（true=TLS，false=h2c）
 * @return 新会话对象，失败返回 NULL
 */
http2_session_t *http2_session_create(int fd, bool tls_mode);

/**
 * http2_session_destroy - 销毁 HTTP/2 会话
 *
 * @param h2 会话对象
 */
void http2_session_destroy(http2_session_t *h2);

/**
 * http2_session_is_http2 - 判断连接是否已升级为 HTTP/2
 *
 * @param fd socket 文件描述符
 * @return true 是 HTTP/2 连接
 */
bool http2_session_is_http2(int fd);

/**
 * http2_session_get - 获取 fd 对应的 HTTP/2 会话
 *
 * @param fd socket 文件描述符
 * @return 会话对象，NULL 表示非 HTTP/2 连接
 */
http2_session_t *http2_session_get(int fd);

/**
 * http2_recv - 接收并处理客户端数据
 *
 * 将读取的数据喂给 nghttp2 库，触发回调处理请求。
 *
 * @param h2  会话对象
 * @param buf 接收缓冲区
 * @param len 数据长度
 * @return 0 成功，-1 错误（应关闭连接）
 */
int http2_recv(http2_session_t *h2, const uint8_t *buf, size_t len);

/**
 * http2_send_pending - 发送挂起的 HTTP/2 帧
 *
 * 应在 socket 可写时调用。
 *
 * @param h2 会话对象
 * @return 0 成功，-1 错误
 */
int http2_send_pending(http2_session_t *h2);

/**
 * http2_want_read - 检查 nghttp2 是否还需要读取数据
 *
 * @param h2 会话对象
 * @return true 需要继续读取
 */
bool http2_want_read(http2_session_t *h2);

/**
 * http2_want_write - 检查 nghttp2 是否还需要写入数据
 *
 * @param h2 会话对象
 * @return true 需要继续写入
 */
bool http2_want_write(http2_session_t *h2);

/**
 * http2_on_connection_accepted - 新连接接受后的处理
 *
 * 对于 TLS 连接，在 TLS 握手完成后调用；
 * 对于明文连接，在读取到客户端魔术字后调用。
 *
 * @param fd       socket 文件描述符
 * @param tls_mode 是否通过 TLS ALPN 协商
 * @return 0 成功，-1 失败（应关闭连接）
 */
int http2_on_connection_accepted(int fd, bool tls_mode);

/**
 * http2_session_set_context - 设置 HTTP/2 会话的服务上下文
 *
 * @param h2              会话对象
 * @param root_dir        静态资源根目录
 * @param gzip_enabled    是否启用 gzip
 * @param brotli_enabled  是否启用 brotli
 */
void http2_session_set_context(http2_session_t *h2, const char *root_dir, bool gzip_enabled, bool brotli_enabled);

#ifdef __cplusplus
}
#endif

#endif /* COCOON_HTTP2_H */

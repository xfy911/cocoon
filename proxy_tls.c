/**
 * proxy_tls.c - 反向代理客户端 TLS 实现
 *
 * 使用 OpenSSL 作为 TLS 客户端连接到 HTTPS 后端。
 * 独立模块，不共享服务器端 TLS 上下文。
 *
 * @author xfy
 */

#include "proxy_tls.h"
#include "log.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "platform.h"

struct proxy_tls_conn {
    cocoon_socket_t fd;
    SSL *ssl;
    SSL_CTX *ctx;
};

/**
 * proxy_tls_connect_tcp - 建立到后端的 TCP 连接
 */
static cocoon_socket_t proxy_tls_connect_tcp(const char *host, uint16_t port) {
    struct hostent *h = gethostbyname(host);
    if (!h) {
        log_error("代理 TLS: 无法解析主机 %s", host);
        return COCOON_INVALID_SOCKET;
    }

    cocoon_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == COCOON_INVALID_SOCKET) {
        log_error("代理 TLS: 创建 socket 失败");
        return COCOON_INVALID_SOCKET;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, h->h_addr_list[0], (size_t)h->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_error("代理 TLS: 连接后端失败 %s:%d", host, port);
        cocoon_socket_close(fd);
        return COCOON_INVALID_SOCKET;
    }

    return fd;
}

proxy_tls_conn_t *proxy_tls_connect(const char *host, uint16_t port) {
    if (!host || host[0] == '\0') return NULL;

    /* 建立 TCP 连接 */
    cocoon_socket_t fd = proxy_tls_connect_tcp(host, port);
    if (fd == COCOON_INVALID_SOCKET) return NULL;

    /* 创建客户端 SSL 上下文 */
    const SSL_METHOD *method = TLS_client_method();
    if (!method) {
        cocoon_socket_close(fd);
        return NULL;
    }

    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        cocoon_socket_close(fd);
        return NULL;
    }

    /* 最低 TLS 1.2 */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    /* 跳过证书验证（内部反向代理场景） */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    /* 创建 SSL 连接 */
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        cocoon_socket_close(fd);
        return NULL;
    }

    /* 设置 SNI */
    SSL_set_tlsext_host_name(ssl, host);

    /* 绑定 socket 到 SSL */
    SSL_set_fd(ssl, fd);

    /* 执行握手 */
    int ret = SSL_connect(ssl);
    if (ret != 1) {
        int err = SSL_get_error(ssl, ret);
        log_error("代理 TLS: 握手失败 %s:%d, err=%d", host, port, err);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        cocoon_socket_close(fd);
        return NULL;
    }

    log_debug("代理 TLS: 握手成功 %s:%d", host, port);

    proxy_tls_conn_t *conn = (proxy_tls_conn_t *)calloc(1, sizeof(proxy_tls_conn_t));
    if (!conn) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        cocoon_socket_close(fd);
        return NULL;
    }

    conn->fd = fd;
    conn->ssl = ssl;
    conn->ctx = ctx;
    return conn;
}

ssize_t proxy_tls_read(proxy_tls_conn_t *conn, void *buf, size_t len) {
    if (!conn || !conn->ssl) return -1;

    int ret = SSL_read(conn->ssl, buf, (int)len);
    if (ret > 0) return (ssize_t)ret;

    int err = SSL_get_error(conn->ssl, ret);
    if (err == SSL_ERROR_ZERO_RETURN) return 0; /* 对端关闭 */
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    return -1;
}

ssize_t proxy_tls_write(proxy_tls_conn_t *conn, const void *buf, size_t len) {
    if (!conn || !conn->ssl) return -1;

    int ret = SSL_write(conn->ssl, buf, (int)len);
    if (ret > 0) return (ssize_t)ret;

    int err = SSL_get_error(conn->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        return -1;
    }
    return -1;
}

void proxy_tls_close(proxy_tls_conn_t *conn) {
    if (!conn) return;

    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
    }
    if (conn->ctx) {
        SSL_CTX_free(conn->ctx);
    }
    if (conn->fd != COCOON_INVALID_SOCKET) {
        cocoon_socket_close(conn->fd);
    }
    free(conn);
}

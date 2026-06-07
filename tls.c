/**
 * tls.c - TLS/SSL 模块实现
 *
 * 使用 OpenSSL Memory BIO 与 coco 协程 I/O 集成：
 *   - SSL_read  从 read BIO 消费解密数据
 *   - SSL_write 向 write BIO 产出加密数据
 *   - 网络 I/O 由 coco_read/coco_write 或 read/write 完成
 *
 * @author xfy
 */

#include "tls.h"
#include "log.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "../coco/include/coco.h"

/* 内部：TLS 连接条目 */
typedef struct {
    SSL *ssl;
    BIO *rbio;  /* 从 socket 读取的加密数据写入此处 */
    BIO *wbio;  /* SSL_write 产出的加密数据从此读取 */
} tls_conn_t;

/* fd → TLS 连接映射（动态数组，O(1) 索引） */
static tls_conn_t **g_map = NULL;
static int g_map_cap = 0;
static SSL_CTX *g_ctx = NULL;

/**
 * tls_lookup - 根据 fd 查找 TLS 连接
 *
 * O(1) 查表，使用动态数组索引。
 *
 * @param fd socket 文件描述符
 * @return TLS 连接结构，未找到返回 NULL
 */
static tls_conn_t* tls_lookup(int fd) {
    if (fd >= 0 && fd < g_map_cap) return g_map[fd];
    return NULL;
}

/**
 * tls_map_set - 将 fd 与 TLS 连接关联
 *
 * 若数组容量不足则自动扩容。
 *
 * @param fd socket 文件描述符
 * @param t TLS 连接结构
 */
static void tls_map_set(int fd, tls_conn_t *t) {
    if (fd >= g_map_cap) {
        int old_cap = g_map_cap;
        g_map_cap = fd + 1;
        g_map = (tls_conn_t **)realloc(g_map, g_map_cap * sizeof(tls_conn_t *));
        for (int i = old_cap; i < g_map_cap; i++) g_map[i] = NULL;
    }
    g_map[fd] = t;
}

/**
 * tls_map_clear - 清除 fd 与 TLS 连接的关联
 *
 * @param fd socket 文件描述符
 */
static void tls_map_clear(int fd) {
    if (fd >= 0 && fd < g_map_cap) g_map[fd] = NULL;
}

/**
 * socket_read - 从 socket 读取原始数据（协程感知）
 *
 * 若当前在协程调度器中则使用 coco_read，否则使用标准 read。
 *
 * @param fd socket 文件描述符
 * @param buf 读取缓冲区
 * @param len 最大读取长度
 * @return 实际读取字节数，失败返回 -1
 */
static ssize_t socket_read(int fd, void *buf, size_t len) {
    if (coco_sched_get_current() != NULL) {
        return coco_read(fd, buf, len);
    }
    return read(fd, buf, len);
}

/**
 * socket_write_all - 向 socket 写入全部数据（协程感知）
 *
 * 循环写入直至全部数据发送完毕。若当前在协程调度器中则使用 coco_write。
 *
 * @param fd socket 文件描述符
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return 实际发送字节数，失败返回 -1
 */
static ssize_t socket_write_all(int fd, const void *buf, size_t len) {
    if (coco_sched_get_current() != NULL) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = coco_write(fd, (const char *)buf + sent, len - sent);
            if (n > 0) {
                sent += (size_t)n;
            } else if (n < 0) {
                if (n == -EAGAIN || n == -EINTR || n == COCO_ERROR_WOULD_BLOCK) continue;
                return -1;
            } else {
                return -1;
            }
        }
        return (ssize_t)sent;
    } else {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = write(fd, (const char *)buf + sent, len - sent);
            if (n > 0) {
                sent += (size_t)n;
            } else if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                return -1;
            } else {
                return -1;
            }
        }
        return (ssize_t)sent;
    }
}

/**
 * flush_wbio - 将 write BIO 中的加密数据刷到 socket
 *
 * @param fd socket 文件描述符
 * @param wbio write BIO
 * @return 0 成功，-1 失败
 */
static int flush_wbio(int fd, BIO *wbio) {
    char buf[16384];
    int pending = BIO_read(wbio, buf, sizeof(buf));
    while (pending > 0) {
        if (socket_write_all(fd, buf, (size_t)pending) < 0) return -1;
        pending = BIO_read(wbio, buf, sizeof(buf));
    }
    return 0;
}

/**
 * tls_alpn_select_cb - ALPN 协议选择回调
 *
 * 优先选择 h2（HTTP/2），次选 http/1.1。
 *
 * @param ssl SSL 对象（未使用）
 * @param out 输出选中的协议
 * @param outlen 输出协议长度
 * @param in 客户端 ALPN 列表
 * @param inlen 客户端 ALPN 列表长度
 * @param arg 用户参数（未使用）
 * @return SSL_TLSEXT_ERR_OK 成功，SSL_TLSEXT_ERR_NOACK 无匹配
 */
static int tls_alpn_select_cb(SSL *ssl __attribute__((unused)), const unsigned char **out,
                               unsigned char *outlen, const unsigned char *in,
                               unsigned int inlen, void *arg __attribute__((unused))) {
    const unsigned char *h2 = (const unsigned char *)"h2";
    const unsigned char *http11 = (const unsigned char *)"http/1.1";
    unsigned int h2_len = 2;
    unsigned int http11_len = 8;
    
    /* 客户端发送的 ALPN 列表格式：长度前缀字符串序列 */
    const unsigned char *p = in;
    unsigned int remaining = inlen;
    
    while (remaining > 0) {
        unsigned char len = *p;
        p++;
        remaining--;
        if (len > remaining) break;
        
        /* 优先匹配 h2 */
        if (len == h2_len && memcmp(p, h2, h2_len) == 0) {
            *out = p;
            *outlen = h2_len;
            return SSL_TLSEXT_ERR_OK;
        }
        /* 次选 http/1.1 */
        if (len == http11_len && memcmp(p, http11, http11_len) == 0) {
            *out = p;
            *outlen = http11_len;
            return SSL_TLSEXT_ERR_OK;
        }
        
        p += len;
        remaining -= len;
    }
    
    return SSL_TLSEXT_ERR_NOACK;
}

/* ===== 公共 API ===== */

/**
 * tls_create_context - 创建 TLS 服务器上下文
 *
 * 初始化 OpenSSL，加载证书和私钥，设置 ALPN 回调。
 * 最低支持 TLS 1.2。
 *
 * @param cert_path 证书文件路径（PEM 格式）
 * @param key_path 私钥文件路径（PEM 格式）
 * @return 0 成功，-1 失败
 */
int tls_create_context(const char *cert_path, const char *key_path) {
    if (!cert_path || !key_path) return -1;

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    const SSL_METHOD *method = TLS_server_method();
    if (!method) return -1;

    g_ctx = SSL_CTX_new(method);
    if (!g_ctx) return -1;

    /* 最低 TLS 1.2 */
    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);

    /* 加载证书 */
    if (SSL_CTX_use_certificate_file(g_ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        log_error("加载 TLS 证书失败: %s", cert_path);
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
        return -1;
    }

    /* 加载私钥 */
    if (SSL_CTX_use_PrivateKey_file(g_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        log_error("加载 TLS 私钥失败: %s", key_path);
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
        return -1;
    }

    /* 验证私钥与证书匹配 */
    if (!SSL_CTX_check_private_key(g_ctx)) {
        log_error("TLS 私钥与证书不匹配");
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
        return -1;
    }

    /* 注册 ALPN 回调，支持 HTTP/2 和 HTTP/1.1 协商 */
    SSL_CTX_set_alpn_select_cb(g_ctx, tls_alpn_select_cb, NULL);

    log_info("TLS 上下文已初始化: cert=%s", cert_path);
    return 0;
}

/**
 * tls_destroy_context - 销毁 TLS 上下文并清理所有连接
 *
 * 释放 SSL_CTX、所有 TLS 连接映射表项及其关联资源。
 */
void tls_destroy_context(void) {
    if (g_ctx) {
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
    }
    if (g_map) {
        for (int i = 0; i < g_map_cap; i++) {
            if (g_map[i]) {
                if (g_map[i]->ssl) SSL_free(g_map[i]->ssl);
                if (g_map[i]->rbio) BIO_free(g_map[i]->rbio);
                if (g_map[i]->wbio) BIO_free(g_map[i]->wbio);
                free(g_map[i]);
            }
        }
        free(g_map);
        g_map = NULL;
        g_map_cap = 0;
    }
}

/**
 * tls_has_context - 检查是否已创建 TLS 上下文
 *
 * @return true 已创建，false 未创建
 */
bool tls_has_context(void) {
    return g_ctx != NULL;
}

/**
 * tls_accept - 接受新 TLS 连接并执行握手
 *
 * 为指定 fd 创建 TLS 连接对象，执行完整的握手循环。
 * 使用 Memory BIO 与协程 I/O 集成。
 *
 * @param fd socket 文件描述符
 * @return 0 成功，-1 失败
 */
int tls_accept(int fd) {
    if (!g_ctx) return -1;

    tls_conn_t *t = (tls_conn_t *)calloc(1, sizeof(tls_conn_t));
    if (!t) return -1;

    t->ssl = SSL_new(g_ctx);
    if (!t->ssl) {
        free(t);
        return -1;
    }

    /* 创建 Memory BIO 对 */
    t->rbio = BIO_new(BIO_s_mem());
    t->wbio = BIO_new(BIO_s_mem());
    if (!t->rbio || !t->wbio) {
        SSL_free(t->ssl);
        free(t);
        return -1;
    }

    BIO_set_mem_eof_return(t->rbio, -1);
    BIO_set_mem_eof_return(t->wbio, -1);
    SSL_set_bio(t->ssl, t->rbio, t->wbio);
    SSL_set_accept_state(t->ssl);

    tls_map_set(fd, t);

    /* 执行握手循环 */
    while (1) {
        int ret = SSL_do_handshake(t->ssl);
        if (ret == 1) {
            log_debug("TLS 握手完成 fd=%d", fd);
            return 0; /* 成功 */
        }

        int err = SSL_get_error(t->ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            /* 尝试先刷出 write BIO 中的数据 */
            if (flush_wbio(fd, t->wbio) < 0) goto fail;

            /* 从 socket 读取加密数据 */
            char buf[8192];
            ssize_t n = socket_read(fd, buf, sizeof(buf));
            if (n > 0) {
                BIO_write(t->rbio, buf, (int)n);
            } else if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    /* 非阻塞模式下暂时无数据，继续尝试 */
                    continue;
                }
                goto fail;
            } else {
                goto fail;
            }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            if (flush_wbio(fd, t->wbio) < 0) goto fail;
        } else {
            log_error("TLS 握手失败 fd=%d, err=%d", fd, err);
            goto fail;
        }
    }

fail:
    tls_map_clear(fd);
    SSL_free(t->ssl);
    free(t);
    return -1;
}

/**
 * tls_read - 从 TLS 连接读取解密数据
 *
 * 若 SSL 缓冲区为空，则从 socket 读取加密数据并解密。
 *
 * @param fd socket 文件描述符
 * @param buf 读取缓冲区
 * @param len 最大读取长度
 * @return 实际读取字节数，0 对端关闭，-1 错误
 */
ssize_t tls_read(int fd, void *buf, size_t len) {
    tls_conn_t *t = tls_lookup(fd);
    if (!t || !t->ssl) return -1;

    while (1) {
        int ret = SSL_read(t->ssl, buf, (int)len);
        if (ret > 0) return (ssize_t)ret;

        int err = SSL_get_error(t->ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            /* 从 socket 读取加密数据 */
            char tmp[8192];
            ssize_t n = socket_read(fd, tmp, sizeof(tmp));
            if (n > 0) {
                BIO_write(t->rbio, tmp, (int)n);
            } else {
                return n; /* 0 或 -1 */
            }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            if (flush_wbio(fd, t->wbio) < 0) return -1;
        } else if (err == SSL_ERROR_ZERO_RETURN) {
            return 0; /* 对端关闭 */
        } else {
            return -1;
        }
    }
}

/**
 * tls_write - 向 TLS 连接写入数据
 *
 * 数据经 SSL 加密后通过 socket 发送。
 *
 * @param fd socket 文件描述符
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return 实际发送字节数，-1 错误
 */
ssize_t tls_write(int fd, const void *buf, size_t len) {
    tls_conn_t *t = tls_lookup(fd);
    if (!t || !t->ssl) return -1;

    size_t total = 0;
    while (total < len) {
        int ret = SSL_write(t->ssl, (const char *)buf + total, (int)(len - total));
        if (ret > 0) {
            total += (size_t)ret;
            /* 刷出 write BIO */
            if (flush_wbio(fd, t->wbio) < 0) return -1;
        } else {
            int err = SSL_get_error(t->ssl, ret);
            if (err == SSL_ERROR_WANT_READ) {
                char tmp[8192];
                ssize_t n = socket_read(fd, tmp, sizeof(tmp));
                if (n > 0) {
                    BIO_write(t->rbio, tmp, (int)n);
                } else {
                    return -1;
                }
            } else if (err == SSL_ERROR_WANT_WRITE) {
                if (flush_wbio(fd, t->wbio) < 0) return -1;
            } else {
                return -1;
            }
        }
    }
    return (ssize_t)total;
}

/**
 * tls_close - 关闭 TLS 连接并释放资源
 *
 * 发送 close_notify，释放 SSL 对象和连接映射。
 *
 * @param fd socket 文件描述符
 */
void tls_close(int fd) {
    tls_conn_t *t = tls_lookup(fd);
    if (!t) return;

    if (t->ssl) {
        SSL_shutdown(t->ssl);
        /* 刷出最后的 close_notify */
        flush_wbio(fd, t->wbio);
        SSL_free(t->ssl);
    }
    /* rbio/wbio 已被 SSL_free 释放（SSL_set_bio 转移所有权） */

    tls_map_clear(fd);
    free(t);
}

/**
 * tls_has_connection - 检查 fd 是否有关联的 TLS 连接
 *
 * @param fd socket 文件描述符
 * @return true 有 TLS 连接，false 无
 */
bool tls_has_connection(int fd) {
    return tls_lookup(fd) != NULL;
}

/**
 * tls_negotiated_http2 - 检查 ALPN 协商结果是否为 HTTP/2
 *
 * @param fd socket 文件描述符
 * @return true 协商为 h2，false 未协商或为 http/1.1
 */
bool tls_negotiated_http2(int fd) {
    tls_conn_t *t = tls_lookup(fd);
    if (!t || !t->ssl) return false;

    const unsigned char *alpn = NULL;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(t->ssl, &alpn, &alpn_len);

    if (alpn_len == 2 && memcmp(alpn, "h2", 2) == 0) {
        return true;
    }
    return false;
}

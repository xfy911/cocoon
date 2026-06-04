/**
 * http2.c - HTTP/2 支持实现
 *
 * 使用 nghttp2 库实现 HTTP/2 协议支持。
 * 与 cocoon 的协程 I/O 和 TLS 层集成。
 *
 * @author xfy
 */

#include "http2.h"
#include "static.h"
#include "log.h"
#include "tls.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

/* 最大 HTTP/2 会话数 */
#define MAX_HTTP2_SESSIONS 1024

static http2_session_t *g_sessions[MAX_HTTP2_SESSIONS];
static int g_session_count = 0;

/* 内部函数声明 */
static ssize_t send_callback(nghttp2_session *session __attribute__((unused)), const uint8_t *data,
                             size_t length, int flags __attribute__((unused)), void *user_data);
static int on_begin_headers_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data);
static int on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, const uint8_t *name,
                              size_t namelen, const uint8_t *value,
                              size_t valuelen, uint8_t flags,
                              void *user_data);
static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data);
static int on_stream_close_callback(nghttp2_session *session,
                                    int32_t stream_id, uint32_t error_code,
                                    void *user_data);
static int on_data_chunk_recv_callback(nghttp2_session *session,
                                       uint8_t flags, int32_t stream_id,
                                       const uint8_t *data,
                                       size_t len, void *user_data);

/* ===================== 会话管理 ===================== */

int http2_init(void) {
    memset(g_sessions, 0, sizeof(g_sessions));
    g_session_count = 0;
    return 0;
}

void http2_cleanup(void) {
    for (int i = 0; i < MAX_HTTP2_SESSIONS; i++) {
        if (g_sessions[i]) {
            http2_session_destroy(g_sessions[i]);
            g_sessions[i] = NULL;
        }
    }
    g_session_count = 0;
}

http2_session_t *http2_session_create(int fd, bool tls_mode) {
    if (fd < 0 || fd >= MAX_HTTP2_SESSIONS) {
        return NULL;
    }
    if (g_sessions[fd] != NULL) {
        /* 已存在，先销毁 */
        http2_session_destroy(g_sessions[fd]);
    }

    http2_session_t *h2 = calloc(1, sizeof(http2_session_t));
    if (!h2) {
        return NULL;
    }

    h2->fd = fd;
    h2->tls_mode = tls_mode;

    /* 创建 nghttp2 会话 */
    nghttp2_session_callbacks *callbacks = NULL;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        free(h2);
        return NULL;
    }

    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(
        callbacks, on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                      on_header_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                        on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(
        callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        callbacks, on_data_chunk_recv_callback);

    if (nghttp2_session_server_new(&h2->session, callbacks, h2) != 0) {
        nghttp2_session_callbacks_del(callbacks);
        free(h2);
        return NULL;
    }

    nghttp2_session_callbacks_del(callbacks);

    /* 发送服务器连接前言（SETTINGS 帧） */
    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, (1 << 16) - 1}};
    if (nghttp2_submit_settings(h2->session, NGHTTP2_FLAG_NONE, iv,
                                sizeof(iv) / sizeof(iv[0])) != 0) {
        nghttp2_session_del(h2->session);
        free(h2);
        return NULL;
    }

    g_sessions[fd] = h2;
    g_session_count++;

    return h2;
}

void http2_session_destroy(http2_session_t *h2) {
    if (!h2) return;

    if (h2->fd >= 0 && h2->fd < MAX_HTTP2_SESSIONS) {
        g_sessions[h2->fd] = NULL;
    }
    g_session_count--;

    if (h2->session) {
        nghttp2_session_del(h2->session);
    }

    /* 清理所有流 */
    http2_stream_data_t *stream = h2->streams;
    while (stream) {
        http2_stream_data_t *next = stream->next;
        if (stream->file_fd >= 0) {
            close(stream->file_fd);
        }
        free(stream->response_body);
        http_request_free(&stream->request);
        free(stream);
        stream = next;
    }

    free(h2);
}

bool http2_session_is_http2(int fd) {
    if (fd < 0 || fd >= MAX_HTTP2_SESSIONS) return false;
    return g_sessions[fd] != NULL;
}

http2_session_t *http2_session_get(int fd) {
    if (fd < 0 || fd >= MAX_HTTP2_SESSIONS) return NULL;
    return g_sessions[fd];
}

/* ===================== 数据收发 ===================== */

int http2_recv(http2_session_t *h2, const uint8_t *buf, size_t len) {
    if (!h2 || !h2->session) return -1;

    ssize_t readlen = nghttp2_session_mem_recv(h2->session, buf, len);
    if (readlen < 0) {
        log_error("HTTP/2 接收错误: %s", nghttp2_strerror((int)readlen));
        return -1;
    }

    /* 发送任何挂起的输出帧 */
    return http2_send_pending(h2);
}

int http2_send_pending(http2_session_t *h2) {
    if (!h2 || !h2->session) return -1;

    int rv = nghttp2_session_send(h2->session);
    if (rv != 0) {
        log_error("HTTP/2 发送错误: %s", nghttp2_strerror(rv));
        return -1;
    }
    return 0;
}

bool http2_want_read(http2_session_t *h2) {
    if (!h2 || !h2->session) return false;
    return nghttp2_session_want_read(h2->session) != 0;
}

bool http2_want_write(http2_session_t *h2) {
    if (!h2 || !h2->session) return false;
    return nghttp2_session_want_write(h2->session) != 0;
}

/* ===================== nghttp2 回调 ===================== */

/**
 * send_callback - nghttp2 发送回调
 *
 * 将 nghttp2 序列化后的帧数据发送到 socket。
 */
static ssize_t send_callback(nghttp2_session *session __attribute__((unused)),
                             const uint8_t *data, size_t length,
                             int flags __attribute__((unused)),
                             void *user_data) {
    http2_session_t *h2 = (http2_session_t *)user_data;

    /* 使用 TLS 或原始 socket 发送 */
    ssize_t sent;
    if (h2->tls_mode && tls_has_connection(h2->fd)) {
        sent = tls_write(h2->fd, (const char *)data, length);
    } else {
        sent = send_all(h2->fd, (const char *)data, length);
    }

    if (sent < 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return (int)sent;
}

/**
 * on_begin_headers_callback - 开始接收请求头
 */
static int on_begin_headers_callback(nghttp2_session *session __attribute__((unused)),
                                     const nghttp2_frame *frame,
                                     void *user_data) {
    http2_session_t *h2 = (http2_session_t *)user_data;

    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    /* 创建新流数据 */
    http2_stream_data_t *stream = calloc(1, sizeof(http2_stream_data_t));
    if (!stream) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    stream->stream_id = frame->hd.stream_id;
    stream->file_fd = -1;

    /* 插入链表头部 */
    stream->next = h2->streams;
    h2->streams = stream;

    /* 关联到 nghttp2 流 */
    nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, stream);

    return 0;
}

/**
 * on_header_callback - 接收单个请求头
 */
static int on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, const uint8_t *name,
                              size_t namelen, const uint8_t *value,
                              size_t valuelen, uint8_t flags __attribute__((unused)),
                              void *user_data __attribute__((unused))) {
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    http2_stream_data_t *stream =
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (!stream) {
        return 0;
    }

    /* 收集伪头和普通头 */
    if (namelen == 4 && memcmp(":path", name, 4) == 0) {
        /* 解析路径（可能包含 query string） */
        size_t path_len = valuelen;
        for (size_t i = 0; i < valuelen; i++) {
            if (value[i] == '?') {
                path_len = i;
                break;
            }
        }
        if (path_len > 0) {
            memcpy(stream->request.path, value,
                   path_len < sizeof(stream->request.path) - 1
                       ? path_len
                       : sizeof(stream->request.path) - 1);
        }
    } else if (namelen == 7 && memcmp(":method", name, 7) == 0) {
        if (valuelen == 3 && memcmp("GET", value, 3) == 0) {
            stream->request.method = HTTP_GET;
        } else if (valuelen == 4 && memcmp("HEAD", value, 4) == 0) {
            stream->request.method = HTTP_HEAD;
        } else if (valuelen == 4 && memcmp("POST", value, 4) == 0) {
            stream->request.method = HTTP_POST;
        } else {
            stream->request.method = HTTP_UNKNOWN;
        }
    } else if (namelen == 7 && memcmp(":scheme", name, 7) == 0) {
        /* 忽略 scheme */
    } else if (namelen == 10 && memcmp(":authority", name, 10) == 0) {
        /* 忽略 authority，host 可以从 path 推断 */
    } else {
        /* 普通头字段 */
        if (stream->request.num_headers < HTTP_MAX_HEADERS) {
            int idx = stream->request.num_headers;
            memcpy(stream->request.headers[idx].name, name,
                   namelen < sizeof(stream->request.headers[idx].name) - 1 ? namelen : sizeof(stream->request.headers[idx].name) - 1);
            memcpy(stream->request.headers[idx].value, value,
                   valuelen < sizeof(stream->request.headers[idx].value) - 1 ? valuelen : sizeof(stream->request.headers[idx].value) - 1);
            stream->request.num_headers++;
        }
    }

    return 0;
}

/**
 * on_frame_recv_callback - 帧接收完成
 */
static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *user_data) {
    http2_session_t *h2 = (http2_session_t *)user_data;

    switch (frame->hd.type) {
    case NGHTTP2_DATA:
    case NGHTTP2_HEADERS:
        /* 检查请求是否完整（END_STREAM） */
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            http2_stream_data_t *stream =
                nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
            if (!stream) {
                return 0;
            }
            stream->request_complete = true;

            /* TODO: 处理请求并生成响应 */
            /* 目前返回 501 Not Implemented */
            nghttp2_nv hdrs[] = {
                {(uint8_t *)":status", (uint8_t *)"501", 7, 3, 0}};
            nghttp2_submit_response(session, frame->hd.stream_id, hdrs, 1,
                                     NULL);
            http2_send_pending(h2);
        }
        break;
    default:
        break;
    }

    return 0;
}

/**
 * on_stream_close_callback - 流关闭
 */
static int on_stream_close_callback(nghttp2_session *session __attribute__((unused)),
                                    int32_t stream_id, uint32_t error_code __attribute__((unused)),
                                    void *user_data) {
    http2_session_t *h2 = (http2_session_t *)user_data;

    /* 从链表中移除并释放 */
    http2_stream_data_t **pp = &h2->streams;
    while (*pp) {
        if ((*pp)->stream_id == stream_id) {
            http2_stream_data_t *to_free = *pp;
            *pp = to_free->next;

            if (to_free->file_fd >= 0) {
                close(to_free->file_fd);
            }
            free(to_free->response_body);
            http_request_free(&to_free->request);
            free(to_free);
            return 0;
        }
        pp = &(*pp)->next;
    }

    return 0;
}

/**
 * on_data_chunk_recv_callback - 接收请求体数据
 */
static int on_data_chunk_recv_callback(nghttp2_session *session __attribute__((unused)),
                                       uint8_t flags __attribute__((unused)),
                                       int32_t stream_id, const uint8_t *data,
                                       size_t len, void *user_data __attribute__((unused))) {
    http2_stream_data_t *stream =
        nghttp2_session_get_stream_user_data(session, stream_id);
    if (!stream) {
        return 0;
    }

    /* TODO: 将数据追加到请求体缓冲区 */
    (void)data;
    (void)len;

    return 0;
}

/* ===================== 连接处理 ===================== */

int http2_on_connection_accepted(int fd, bool tls_mode) {
    /* 检查是否应启用 HTTP/2 */
    if (!tls_mode) {
        /* 明文模式：需要读取客户端魔术字 "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" */
        /* 这由 server.c 在读取前几个字节时检测 */
        return 0;
    }

    /* TLS 模式：ALPN 已协商为 h2 */
    http2_session_t *h2 = http2_session_create(fd, true);
    if (!h2) {
        return -1;
    }

    /* 发送服务器连接前言 */
    if (http2_send_pending(h2) != 0) {
        http2_session_destroy(h2);
        return -1;
    }

    return 0;
}

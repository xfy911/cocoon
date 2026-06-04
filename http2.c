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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

/* 最大 HTTP/2 会话数 */
#define MAX_HTTP2_SESSIONS 1024

static http2_session_t *g_sessions[MAX_HTTP2_SESSIONS];
static int g_session_count = 0;

/* 静态文件服务辅助函数 */
static void format_http_time(time_t t, char *buf, size_t buf_size);
static void generate_etag(const struct stat *st, char *buf, size_t buf_size);
static time_t parse_http_time(const char *str);
static bool match_etag(const char *etag, const char *if_none_match);

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
static ssize_t http2_data_source_read_callback(nghttp2_session *session, int32_t stream_id,
                                               uint8_t *buf, size_t length, uint32_t *data_flags,
                                               nghttp2_data_source *source, void *user_data);
static void http2_serve_static(http2_session_t *h2, http2_stream_data_t *stream);

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

void http2_session_set_context(http2_session_t *h2, const char *root_dir, bool gzip_enabled, bool brotli_enabled) {
    if (!h2) return;
    h2->root_dir = root_dir;
    h2->gzip_enabled = gzip_enabled;
    h2->brotli_enabled = brotli_enabled;
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
        if (send_all(h2->fd, (const char *)data, length) != 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        sent = (ssize_t)length;
    }

    if (sent < 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return (int)sent;
}

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
    if (namelen == 5 && memcmp(":path", name, 5) == 0) {
        /* 解析路径（可能包含 query string） */
        size_t path_len = valuelen;
        for (size_t i = 0; i < valuelen; i++) {
            if (value[i] == '?') {
                path_len = i;
                break;
            }
        }
        if (path_len > 0) {
            size_t copy_len = path_len < sizeof(stream->request.path) - 1
                ? path_len
                : sizeof(stream->request.path) - 1;
            memcpy(stream->request.path, value, copy_len);
            stream->request.path[copy_len] = '\0';
        } else {
            stream->request.path[0] = '/';
            stream->request.path[1] = '\0';
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

            /* 设置缓存相关标志 */
            if (namelen == 13 && memcmp("if-none-match", name, 13) == 0) {
                stream->request.has_if_none_match = true;
                size_t copy_len = valuelen < sizeof(stream->request.if_none_match) - 1
                    ? valuelen : sizeof(stream->request.if_none_match) - 1;
                memcpy(stream->request.if_none_match, value, copy_len);
                stream->request.if_none_match[copy_len] = '\0';
            } else if (namelen == 17 && memcmp("if-modified-since", name, 17) == 0) {
                stream->request.has_if_modified_since = true;
                size_t copy_len = valuelen < sizeof(stream->request.if_modified_since) - 1
                    ? valuelen : sizeof(stream->request.if_modified_since) - 1;
                memcpy(stream->request.if_modified_since, value, copy_len);
                stream->request.if_modified_since[copy_len] = '\0';
            }
        }
    }

    return 0;
}

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

            /* 处理静态文件请求 */
            http2_serve_static(h2, stream);
            http2_send_pending(h2);
        }
        break;
    default:
        break;
    }

    return 0;
}

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

/* ===================== HTTP/2 静态文件服务 ===================== */

static void format_http_time(time_t t, char *buf, size_t buf_size) {
    struct tm *gmt = gmtime(&t);
    if (gmt) {
        strftime(buf, buf_size, "%a, %d %b %Y %H:%M:%S GMT", gmt);
    } else {
        buf[0] = '\0';
    }
}

static void generate_etag(const struct stat *st, char *buf, size_t buf_size) {
    snprintf(buf, buf_size, "\"%lx-%lx\"", (unsigned long)st->st_size, (unsigned long)st->st_mtime);
}

static time_t parse_http_time(const char *str) {
    struct tm tm = {0};
    if (strptime(str, "%a, %d %b %Y %H:%M:%S GMT", &tm) != NULL ||
        strptime(str, "%A, %d-%b-%y %H:%M:%S GMT", &tm) != NULL ||
        strptime(str, "%a %b %d %H:%M:%S %Y", &tm) != NULL) {
        return timegm(&tm);
    }
    return -1;
}

static bool match_etag(const char *etag, const char *if_none_match) {
    if (!etag || !if_none_match) return false;
    if (strcmp(if_none_match, "*") == 0) return true;
    const char *client = if_none_match;
    if (strncmp(client, "W/", 2) == 0) client += 2;
    return strcmp(client, etag) == 0;
}

static ssize_t http2_data_source_read_callback(
    nghttp2_session *session __attribute__((unused)),
    int32_t stream_id __attribute__((unused)),
    uint8_t *buf, size_t length,
    uint32_t *data_flags,
    nghttp2_data_source *source,
    void *user_data __attribute__((unused))) {
    
    if (source->fd < 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    
    ssize_t n = read(source->fd, buf, length);
    if (n < 0) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    if (n == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t)n;
}

static void http2_serve_static(http2_session_t *h2, http2_stream_data_t *stream) {
    if (!h2->root_dir) {
        nghttp2_nv hdrs[] = {
            {(uint8_t *)":status", (uint8_t *)"503", 7, 3, 0}};
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
        return;
    }
    
    /* 构建真实路径 */
    char real_path[4096];
    char root_normalized[4096];
    
    if (!realpath(h2->root_dir, root_normalized)) {
        snprintf(root_normalized, sizeof(root_normalized), "%s", h2->root_dir);
    }
    
    int n = snprintf(real_path, sizeof(real_path), "%s%s", root_normalized, stream->request.path);
    if (n < 0 || (size_t)n >= sizeof(real_path)) {
        nghttp2_nv hdrs[] = {
            {(uint8_t *)":status", (uint8_t *)"400", 7, 3, 0}};
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
        return;
    }
    
    /* 路径遍历检查 */
    if (strstr(stream->request.path, "..") != NULL) {
        char resolved[4096];
        if (!realpath(real_path, resolved) ||
            strncmp(resolved, root_normalized, strlen(root_normalized)) != 0) {
            nghttp2_nv hdrs[] = {
                {(uint8_t *)":status", (uint8_t *)"403", 7, 3, 0}};
            nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
            return;
        }
        snprintf(real_path, sizeof(real_path), "%s", resolved);
    }
    
    struct stat st;
    if (stat(real_path, &st) != 0) {
        nghttp2_nv hdrs[] = {
            {(uint8_t *)":status", (uint8_t *)"404", 7, 3, 0}};
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
        return;
    }
    
    /* 目录：尝试 index.html */
    if (S_ISDIR(st.st_mode)) {
        char index_path[4096];
        size_t real_len = strlen(real_path);
        if (real_len + 12 >= sizeof(index_path)) {
            nghttp2_nv hdrs[] = {
                {(uint8_t *)":status", (uint8_t *)"404", 7, 3, 0}};
            nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
            return;
        }
        snprintf(index_path, sizeof(index_path), "%s/index.html", real_path);
        struct stat index_st;
        if (stat(index_path, &index_st) == 0 && S_ISREG(index_st.st_mode)) {
            snprintf(real_path, sizeof(real_path), "%s", index_path);
            stat(real_path, &st);
        } else {
            nghttp2_nv hdrs[] = {
                {(uint8_t *)":status", (uint8_t *)"404", 7, 3, 0}};
            nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
            return;
        }
    }
    
    if (!S_ISREG(st.st_mode)) {
        nghttp2_nv hdrs[] = {
            {(uint8_t *)":status", (uint8_t *)"403", 7, 3, 0}};
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
        return;
    }
    
    /* 生成 ETag 和 Last-Modified */
    char etag[64];
    char last_modified[64];
    generate_etag(&st, etag, sizeof(etag));
    format_http_time(st.st_mtime, last_modified, sizeof(last_modified));
    
    /* 检查 If-None-Match */
    if (stream->request.has_if_none_match && match_etag(etag, stream->request.if_none_match)) {
        nghttp2_nv hdrs[] = {
            {(uint8_t *)":status", (uint8_t *)"304", 7, 3, 0},
            {(uint8_t *)"etag", (uint8_t *)etag, 4, strlen(etag), 0},
            {(uint8_t *)"last-modified", (uint8_t *)last_modified, 13, strlen(last_modified), 0},
        };
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 3, NULL);
        return;
    }
    
    /* 检查 If-Modified-Since */
    if (stream->request.has_if_modified_since) {
        time_t client_time = parse_http_time(stream->request.if_modified_since);
        if (client_time >= 0 && st.st_mtime <= client_time) {
            nghttp2_nv hdrs[] = {
                {(uint8_t *)":status", (uint8_t *)"304", 7, 3, 0},
                {(uint8_t *)"etag", (uint8_t *)etag, 4, strlen(etag), 0},
                {(uint8_t *)"last-modified", (uint8_t *)last_modified, 13, strlen(last_modified), 0},
            };
            nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 3, NULL);
            return;
        }
    }
    
    /* 打开文件 */
    int file_fd = open(real_path, O_RDONLY);
    if (file_fd < 0) {
        nghttp2_nv hdrs[] = {
            {(uint8_t *)":status", (uint8_t *)"403", 7, 3, 0}};
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
        return;
    }
    
    /* 构建响应头 */
    nghttp2_nv hdrs[16];
    int num_hdrs = 0;
    
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)":status", (uint8_t *)"200", 7, 3, 0};
    
    const char *mime = http_mime_type(real_path);
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"content-type", (uint8_t *)mime, 12, strlen(mime), 0};
    
    char content_length_str[32];
    snprintf(content_length_str, sizeof(content_length_str), "%ld", (long)st.st_size);
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"content-length", (uint8_t *)content_length_str, 14, strlen(content_length_str), 0};
    
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"etag", (uint8_t *)etag, 4, strlen(etag), 0};
    
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"last-modified", (uint8_t *)last_modified, 13, strlen(last_modified), 0};
    
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"server", (uint8_t *)"Cocoon/1.0", 6, 10, 0};
    
    /* HEAD 请求：不发送 body */
    if (stream->request.method == HTTP_HEAD) {
        close(file_fd);
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, num_hdrs, NULL);
    } else {
        nghttp2_data_provider provider;
        provider.source.fd = file_fd;
        provider.read_callback = http2_data_source_read_callback;
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, num_hdrs, &provider);
        stream->file_fd = file_fd;
    }
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

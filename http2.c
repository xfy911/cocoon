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
#include <dirent.h>
#include <zlib.h>
#include <brotli/encode.h>

/* 最大 HTTP/2 会话数 */
#define MAX_HTTP2_SESSIONS 1024

/* 压缩辅助函数（与 static.c 独立，保持模块边界） */
static bool is_compressible_mime(const char *mime_type) {
    if (!mime_type) return false;
    return (
        strstr(mime_type, "text/") != NULL ||
        strstr(mime_type, "application/javascript") != NULL ||
        strstr(mime_type, "application/json") != NULL ||
        strstr(mime_type, "application/xml") != NULL ||
        strstr(mime_type, "application/manifest") != NULL ||
        strstr(mime_type, "image/svg") != NULL
    );
}

static ssize_t gzip_compress(const char *src, size_t src_len,
                             char *dst, size_t dst_cap) {
    z_stream strm = {0};
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return -1;
    }
    strm.avail_in = (uInt)src_len;
    strm.next_in = (Bytef *)src;
    strm.avail_out = (uInt)dst_cap;
    strm.next_out = (Bytef *)dst;
    if (deflate(&strm, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&strm);
        return -1;
    }
    size_t compressed_len = dst_cap - strm.avail_out;
    deflateEnd(&strm);
    if (compressed_len >= src_len * 0.95) return 0;
    return (ssize_t)compressed_len;
}

static ssize_t brotli_compress(const char *src, size_t src_len,
                                 char *dst, size_t dst_cap) {
    size_t encoded_size = dst_cap;
    BROTLI_BOOL ok = BrotliEncoderCompress(
        BROTLI_DEFAULT_QUALITY,
        BROTLI_DEFAULT_WINDOW,
        BROTLI_MODE_GENERIC,
        src_len,
        (const uint8_t *)src,
        &encoded_size,
        (uint8_t *)dst
    );
    if (!ok) return -1;
    if (encoded_size >= src_len * 0.95) return 0;
    return (ssize_t)encoded_size;
}

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
static bool http2_serve_directory(http2_session_t *h2, http2_stream_data_t *stream,
                                   const char *real_path, const char *request_path);
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

int http2_session_upgrade(http2_session_t *h2, const http_request_t *req) {
    if (!h2 || !h2->session || !req) return -1;

    /* 创建流数据（stream_id=1） */
    http2_stream_data_t *stream = calloc(1, sizeof(http2_stream_data_t));
    if (!stream) return -1;

    stream->stream_id = 1;
    stream->file_fd = -1;
    stream->request = *req;
    stream->request.body = NULL;  /* 升级请求通常无 body，避免双重释放 */
    stream->request_complete = true;

    /* 注册升级流到 nghttp2 */
    int is_head = (req->method == HTTP_HEAD) ? 1 : 0;
    if (nghttp2_session_upgrade2(h2->session, NULL, 0, is_head, stream) != 0) {
        free(stream);
        return -1;
    }

    /* 关联到会话 */
    stream->next = h2->streams;
    h2->streams = stream;
    nghttp2_session_set_stream_user_data(h2->session, 1, stream);

    /* 提交静态文件响应 */
    http2_serve_static(h2, stream);

    return 0;
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
    
    http2_stream_data_t *stream = (http2_stream_data_t *)source->ptr;
    if (!stream || !stream->response_body) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    
    size_t remaining = stream->response_len - stream->response_sent;
    if (remaining == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    
    size_t to_send = length < remaining ? length : remaining;
    memcpy(buf, stream->response_body + stream->response_sent, to_send);
    stream->response_sent += to_send;
    
    if (stream->response_sent >= stream->response_len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t)to_send;
}

static bool http2_serve_directory(http2_session_t *h2, http2_stream_data_t *stream,
                                   const char *real_path, const char *request_path) {
    struct stat st;
    if (stat(real_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }

    DIR *dir = opendir(real_path);
    if (!dir) {
        nghttp2_nv hdrs[] = {
            {(uint8_t *)":status", (uint8_t *)"403", 7, 3, 0}};
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
        return true;
    }

    /* 收集所有目录项 */
    struct dirent *entry;
    char *entries[4096];
    int num_entries = 0;
    while ((entry = readdir(dir)) != NULL && num_entries < 4096) {
        if (entry->d_name[0] == '.') continue;
        entries[num_entries] = strdup(entry->d_name);
        num_entries++;
    }
    closedir(dir);

    /* 构建 HTML */
    char *html = (char *)malloc(65536);
    if (!html) {
        for (int i = 0; i < num_entries; i++) free(entries[i]);
        nghttp2_nv hdrs[] = {
            {(uint8_t *)":status", (uint8_t *)"500", 7, 3, 0}};
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
        return true;
    }

    int n = snprintf(html, 65536,
        "<!DOCTYPE html>\n"
        "<html><head>\n"
        "<meta charset=\"utf-8\">\n"
        "<title>Index of %s</title>\n"
        "<style>"
        "body{font-family:system-ui,-apple-system,sans-serif;max-width:800px;margin:40px auto;padding:0 20px}"
        "h1{border-bottom:1px solid #ddd;padding-bottom:10px}"
        "table{width:100%%;border-collapse:collapse}"
        "th{text-align:left;padding:8px;border-bottom:2px solid #ddd}"
        "td{padding:8px;border-bottom:1px solid #eee}"
        "a{text-decoration:none;color:#0066cc}"
        "a:hover{text-decoration:underline}"
        "</style>\n"
        "</head><body>\n"
        "<h1>Index of %s</h1>\n"
        "<table>\n"
        "<tr><th>Name</th><th>Size</th><th>Modified</th></tr>\n",
        request_path, request_path);

    /* 添加返回上级链接 */
    if (strcmp(request_path, "/") != 0) {
        n += snprintf(html + n, 65536 - n,
            "<tr><td><a href=\"../\">../</a></td><td>-</td><td>-</td></tr>\n");
    }

    /* HTML 转义辅助 */
    auto void html_escape(const char *src, char *dst, size_t dst_size) {
        size_t j = 0;
        for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
            switch (src[i]) {
                case '&':
                    if (j + 5 < dst_size) { memcpy(dst + j, "&amp;", 5); j += 5; }
                    break;
                case '<':
                    if (j + 4 < dst_size) { memcpy(dst + j, "&lt;", 4); j += 4; }
                    break;
                case '>':
                    if (j + 4 < dst_size) { memcpy(dst + j, "&gt;", 4); j += 4; }
                    break;
                case '"':
                    if (j + 6 < dst_size) { memcpy(dst + j, "&quot;", 6); j += 6; }
                    break;
                default:
                    dst[j++] = src[i];
            }
        }
        dst[j] = '\0';
    }

    /* 添加目录项 */
    for (int i = 0; i < num_entries; i++) {
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", real_path, entries[i]);

        struct stat entry_st;
        char size_str[32] = "-";
        char mtime_str[32] = "-";

        if (stat(full_path, &entry_st) == 0) {
            if (S_ISDIR(entry_st.st_mode)) {
                strncpy(size_str, "-", sizeof(size_str));
            } else if (entry_st.st_size < 1024) {
                snprintf(size_str, sizeof(size_str), "%ld B", (long)entry_st.st_size);
            } else if (entry_st.st_size < 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1f KB", entry_st.st_size / 1024.0);
            } else if (entry_st.st_size < 1024 * 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1f MB", entry_st.st_size / (1024.0 * 1024));
            } else {
                snprintf(size_str, sizeof(size_str), "%.1f GB", entry_st.st_size / (1024.0 * 1024 * 1024));
            }

            struct tm *tm_info = localtime(&entry_st.st_mtime);
            if (tm_info) {
                strftime(mtime_str, sizeof(mtime_str), "%Y-%m-%d %H:%M", tm_info);
            }
        }

        char escaped_name[512];
        html_escape(entries[i], escaped_name, sizeof(escaped_name));

        n += snprintf(html + n, 65536 - n,
            "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n",
            escaped_name,
            S_ISDIR(entry_st.st_mode) ? "/" : "",
            escaped_name,
            S_ISDIR(entry_st.st_mode) ? "/" : "",
            size_str, mtime_str);

        free(entries[i]);
    }

    n += snprintf(html + n, 65536 - n,
        "</table>\n"
        "<hr>\n"
        "<p><em>Cocoon Server</em></p>\n"
        "</body></html>\n");

    /* 存储响应并发送 */
    stream->response_body = html;
    stream->response_len = (size_t)n;
    stream->response_sent = 0;

    nghttp2_nv hdrs[8];
    int num_hdrs = 0;

    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)":status", (uint8_t *)"200", 7, 3, 0};

    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"content-type", (uint8_t *)"text/html; charset=utf-8", 12, 24, 0};

    char content_length_str[32];
    snprintf(content_length_str, sizeof(content_length_str), "%d", n);
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"content-length", (uint8_t *)content_length_str, 14, strlen(content_length_str), 0};

    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"server", (uint8_t *)"Cocoon/1.0", 6, 10, 0};

    if (stream->request.method == HTTP_HEAD) {
        free(stream->response_body);
        stream->response_body = NULL;
        stream->response_len = 0;
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, num_hdrs, NULL);
    } else {
        nghttp2_data_provider provider;
        provider.source.ptr = stream;
        provider.read_callback = http2_data_source_read_callback;
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, num_hdrs, &provider);
    }

    return true;
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
    
    /* 目录：尝试 index.html，否则生成目录列表 */
    if (S_ISDIR(st.st_mode)) {
        char index_path[4096];
        size_t real_len = strlen(real_path);
        if (real_len + 12 < sizeof(index_path)) {
            snprintf(index_path, sizeof(index_path), "%s/index.html", real_path);
            struct stat index_st;
            if (stat(index_path, &index_st) == 0 && S_ISREG(index_st.st_mode)) {
                snprintf(real_path, sizeof(real_path), "%s", index_path);
                stat(real_path, &st);
            } else {
                /* 生成目录浏览页面 */
                if (http2_serve_directory(h2, stream, real_path, stream->request.path)) {
                    return;
                }
            }
        } else {
            if (http2_serve_directory(h2, stream, real_path, stream->request.path)) {
                return;
            }
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
    
    /* 读取文件内容到内存 */
    int64_t file_size = st.st_size;
    char *file_buf = (char *)malloc((size_t)file_size);
    if (!file_buf) {
        close(file_fd);
        nghttp2_nv hdrs[] = {
            {(uint8_t *)":status", (uint8_t *)"500", 7, 3, 0}};
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
        return;
    }
    
    ssize_t read_total = 0;
    while (read_total < file_size) {
        ssize_t n = read(file_fd, file_buf + read_total, (size_t)(file_size - read_total));
        if (n <= 0) break;
        read_total += n;
    }
    close(file_fd);
    
    if (read_total != file_size) {
        free(file_buf);
        nghttp2_nv hdrs[] = {
            {(uint8_t *)":status", (uint8_t *)"500", 7, 3, 0}};
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, 1, NULL);
        return;
    }
    
    /* 判断是否需要压缩 */
    bool use_gzip = false;
    bool use_brotli = false;
    const char *mime = http_mime_type(real_path);
    
    if (stream->request.method != HTTP_HEAD && is_compressible_mime(mime) && file_size > 256) {
        char *compress_buf = (char *)malloc((size_t)file_size);
        if (compress_buf) {
            if (h2->brotli_enabled) {
                ssize_t cl = brotli_compress(file_buf, (size_t)file_size, compress_buf, (size_t)file_size);
                if (cl > 0) {
                    free(file_buf);
                    file_buf = compress_buf;
                    file_size = cl;
                    use_brotli = true;
                } else {
                    free(compress_buf);
                }
            } else if (h2->gzip_enabled) {
                ssize_t cl = gzip_compress(file_buf, (size_t)file_size, compress_buf, (size_t)file_size);
                if (cl > 0) {
                    free(file_buf);
                    file_buf = compress_buf;
                    file_size = cl;
                    use_gzip = true;
                } else {
                    free(compress_buf);
                }
            } else {
                free(compress_buf);
            }
        }
    }
    
    /* 存储到流 */
    stream->response_body = file_buf;
    stream->response_len = (size_t)file_size;
    stream->response_sent = 0;
    
    /* 构建响应头 */
    nghttp2_nv hdrs[16];
    int num_hdrs = 0;
    
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)":status", (uint8_t *)"200", 7, 3, 0};
    
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"content-type", (uint8_t *)mime, 12, strlen(mime), 0};
    
    char content_length_str[32];
    snprintf(content_length_str, sizeof(content_length_str), "%ld", (long)file_size);
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"content-length", (uint8_t *)content_length_str, 14, strlen(content_length_str), 0};
    
    if (use_brotli) {
        hdrs[num_hdrs++] = (nghttp2_nv){
            (uint8_t *)"content-encoding", (uint8_t *)"br", 16, 2, 0};
    } else if (use_gzip) {
        hdrs[num_hdrs++] = (nghttp2_nv){
            (uint8_t *)"content-encoding", (uint8_t *)"gzip", 16, 4, 0};
    }
    
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"etag", (uint8_t *)etag, 4, strlen(etag), 0};
    
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"last-modified", (uint8_t *)last_modified, 13, strlen(last_modified), 0};
    
    hdrs[num_hdrs++] = (nghttp2_nv){
        (uint8_t *)"server", (uint8_t *)"Cocoon/1.0", 6, 10, 0};
    
    /* HEAD 请求：不发送 body */
    if (stream->request.method == HTTP_HEAD) {
        free(stream->response_body);
        stream->response_body = NULL;
        stream->response_len = 0;
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, num_hdrs, NULL);
    } else {
        nghttp2_data_provider provider;
        provider.source.ptr = stream;
        provider.read_callback = http2_data_source_read_callback;
        nghttp2_submit_response(h2->session, stream->stream_id, hdrs, num_hdrs, &provider);
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

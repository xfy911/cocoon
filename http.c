
/**
 * http.c - HTTP 解析与响应构建
 *
 * 轻量级 HTTP/1.1 解析器，支持请求解析、响应头格式化、MIME 类型识别。
 *
 * @author xfy
 */

#include "http.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

/**
 * http_method_str - HTTP 方法枚举转字符串
 *
 * @param method HTTP 方法枚举值
 * @return 方法名称字符串，未知方法返回 "UNKNOWN"
 */
const char *http_method_str(http_method_t method) {
    switch (method) {
        case HTTP_GET:     return "GET";
        case HTTP_HEAD:    return "HEAD";
        case HTTP_POST:    return "POST";
        case HTTP_PUT:     return "PUT";
        case HTTP_DELETE:  return "DELETE";
        case HTTP_OPTIONS: return "OPTIONS";
        default:           return "UNKNOWN";
    }
}

/**
 * parse_method - 从缓冲区开头解析 HTTP 方法
 *
 * @param buf 输入缓冲区
 * @return 解析出的 HTTP 方法枚举
 */
static http_method_t parse_method(const char *buf) {
    if (strncmp(buf, "GET ", 4) == 0)     return HTTP_GET;
    if (strncmp(buf, "HEAD ", 5) == 0)    return HTTP_HEAD;
    if (strncmp(buf, "POST ", 5) == 0)   return HTTP_POST;
    if (strncmp(buf, "PUT ", 4) == 0)    return HTTP_PUT;
    if (strncmp(buf, "DELETE ", 7) == 0) return HTTP_DELETE;
    if (strncmp(buf, "OPTIONS ", 8) == 0) return HTTP_OPTIONS;
    return HTTP_UNKNOWN;
}

/**
 * parse_headers - 解析请求头键值对
 *
 * 逐行解析，填充 headers 数组，同时提取 content-length、connection 等常用头。
 *
 * @param p 当前解析位置指针（会被推进）
 * @param end 缓冲区结尾
 * @param req 请求结构体
 */
static void parse_headers(const char **p, const char *end, http_request_t *req) {
    req->num_headers = 0;
    req->content_length = -1;
    req->keep_alive = false;
    req->has_range = false;
    req->range_start = 0;
    req->range_end = -1;
    req->has_if_none_match = false;
    req->has_if_modified_since = false;
    req->accept_gzip = false;
    req->accept_deflate = false;
    req->has_accept_encoding = false;

    while (*p < end && req->num_headers < HTTP_MAX_HEADERS) {
        const char *line_start = *p;

        /* 查找行尾 */
        const char *line_end = memchr(line_start, '\n', end - line_start);
        if (!line_end) break;

        /* 空行表示头部结束 */
        if (line_end == line_start || (line_end == line_start + 1 && line_start[0] == '\r')) {
            *p = line_end + 1;
            break;
        }

        /* 跳过末尾 \r */
        const char *line_tail = line_end;
        if (line_tail > line_start && line_tail[-1] == '\r') line_tail--;

        /* 查找冒号分隔符 */
        const char *colon = memchr(line_start, ':', line_tail - line_start);
        if (colon) {
            int name_len = colon - line_start;
            if (name_len > 0 && name_len < HTTP_HEADER_NAME_MAX) {
                /* 复制名称（转小写便于匹配） */
                for (int i = 0; i < name_len && i < HTTP_HEADER_NAME_MAX - 1; i++) {
                    req->headers[req->num_headers].name[i] = (char)tolower((unsigned char)line_start[i]);
                }
                req->headers[req->num_headers].name[name_len > HTTP_HEADER_NAME_MAX - 1 ? HTTP_HEADER_NAME_MAX - 1 : name_len] = '\0';

                /* 复制值（跳过冒号后空格） */
                const char *val = colon + 1;
                while (val < line_tail && (*val == ' ' || *val == '\t')) val++;
                int val_len = line_tail - val;
                if (val_len > 0 && val_len < HTTP_HEADER_VALUE_MAX) {
                    int copy_len = val_len > HTTP_HEADER_VALUE_MAX - 1 ? HTTP_HEADER_VALUE_MAX - 1 : val_len;
                    memcpy(req->headers[req->num_headers].value, val, copy_len);
                    req->headers[req->num_headers].value[copy_len] = '\0';
                }

                /* 提取常用头 */
                if (strcmp(req->headers[req->num_headers].name, "content-length") == 0) {
                    req->content_length = atoll(req->headers[req->num_headers].value);
                } else if (strcmp(req->headers[req->num_headers].name, "connection") == 0) {
                    req->keep_alive = (strcasecmp(req->headers[req->num_headers].value, "keep-alive") == 0);
                } else if (strcmp(req->headers[req->num_headers].name, "if-none-match") == 0) {
                    req->has_if_none_match = true;
                    int copy_len = strlen(req->headers[req->num_headers].value);
                    if (copy_len >= 64) copy_len = 63;
                    memcpy(req->if_none_match, req->headers[req->num_headers].value, copy_len);
                    req->if_none_match[copy_len] = '\0';
                } else if (strcmp(req->headers[req->num_headers].name, "if-modified-since") == 0) {
                    req->has_if_modified_since = true;
                    int copy_len = strlen(req->headers[req->num_headers].value);
                    if (copy_len >= 64) copy_len = 63;
                    memcpy(req->if_modified_since, req->headers[req->num_headers].value, copy_len);
                    req->if_modified_since[copy_len] = '\0';
                } else if (strcmp(req->headers[req->num_headers].name, "accept-encoding") == 0) {
                    req->has_accept_encoding = true;
                    const char *val = req->headers[req->num_headers].value;
                    if (strstr(val, "gzip") != NULL) req->accept_gzip = true;
                    if (strstr(val, "deflate") != NULL) req->accept_deflate = true;
                } else if (strcmp(req->headers[req->num_headers].name, "content-type") == 0) {
                    int copy_len = strlen(req->headers[req->num_headers].value);
                    if (copy_len >= HTTP_HEADER_VALUE_MAX) copy_len = HTTP_HEADER_VALUE_MAX - 1;
                    memcpy(req->content_type, req->headers[req->num_headers].value, copy_len);
                    req->content_type[copy_len] = '\0';
                } else if (strcmp(req->headers[req->num_headers].name, "range") == 0) {
                    const char *range_val = req->headers[req->num_headers].value;
                    if (strncasecmp(range_val, "bytes=", 6) == 0) {
                        req->has_range = true;
                        req->range_start = atoll(range_val + 6);
                        const char *dash = strchr(range_val + 6, '-');
                        if (dash) {
                            if (dash[1] == '\0') {
                                req->range_end = -1;  /* open-end range */
                            } else {
                                req->range_end = atoll(dash + 1);
                            }
                        }
                    }
                }

                req->num_headers++;
            }
        }

        *p = line_end + 1;
    }
}

/**
 * http_parse_request - 解析 HTTP 请求
 *
 * 从缓冲区解析 HTTP/1.x 请求行和头部，填充 http_request_t 结构体。
 * 不解析请求体。
 *
 * @param buf 输入缓冲区
 * @param len 缓冲区长度
 * @param req 输出请求结构体
 * @return 成功解析的字节数，-1 表示数据不完整，-2 表示格式错误
 */
int http_parse_request(const char *buf, size_t len, http_request_t *req) {
    if (!buf || !req || len == 0) return -2;

    memset(req, 0, sizeof(*req));

    /* 查找请求行结尾（\r\n） */
    const char *line_end = memchr(buf, '\n', len);
    if (!line_end) return -1; /* 数据不完整 */

    int line_len = line_end - buf;
    if (line_len > 0 && buf[line_len - 1] == '\r') line_len--;
    if (line_len < 10) return -2; /* 请求行太短 */

    /* 解析方法 */
    req->method = parse_method(buf);
    if (req->method == HTTP_UNKNOWN) return -2;

    /* 提取路径 */
    const char *path_start = strchr(buf, ' ');
    if (!path_start) return -2;
    path_start++;

    const char *path_end = strchr(path_start, ' ');
    if (!path_end) return -2;

    int path_len = path_end - path_start;
    if (path_len <= 0 || path_len >= HTTP_MAX_PATH) return -2;
    memcpy(req->path, path_start, path_len);
    req->path[path_len] = '\0';

    /* 提取 HTTP 版本 */
    const char *ver_start = path_end + 1;
    int ver_len = line_len - (ver_start - buf);
    if (ver_len > 0 && ver_len < (int)sizeof(req->version)) {
        memcpy(req->version, ver_start, ver_len);
        req->version[ver_len] = '\0';
    }

    /* 解析头部 */
    const char *p = line_end + 1;
    const char *end = buf + len;
    parse_headers(&p, end, req);

    /* HTTP/1.1 默认 keep-alive，但 Connection 头可覆盖 */
    bool has_connection = false;
    for (int i = 0; i < req->num_headers; i++) {
        if (strcmp(req->headers[i].name, "connection") == 0) {
            has_connection = true;
            break;
        }
    }
    if (!has_connection && strstr(req->version, "1.1") != NULL) {
        req->keep_alive = true;
    }

    return p - buf;
}

/**
 * http_format_response_header - 格式化 HTTP 响应头
 *
 * 将响应信息格式化为标准 HTTP/1.1 响应头字符串。
 *
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param resp 响应信息结构体
 * @return 实际写入的字节数，负值表示缓冲区不足
 */
int http_format_response_header(char *buf, size_t buf_size, const http_response_t *resp) {
    if (!buf || !resp || buf_size == 0) return -1;

    int n = 0;

    /* 状态行 */
    if (resp->has_range) {
        n += snprintf(buf + n, buf_size - (size_t)n > buf_size ? 0 : buf_size - (size_t)n,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-Range: bytes %ld-%ld/%ld\r\n",
            resp->status_code, resp->status_text,
            resp->content_type ? resp->content_type : "application/octet-stream",
            (long)resp->content_length,
            (long)resp->range_start, (long)resp->range_end, (long)resp->total_length);
    } else {
        n += snprintf(buf + n, buf_size - (size_t)n > buf_size ? 0 : buf_size - (size_t)n,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Accept-Ranges: bytes\r\n",
            resp->status_code, resp->status_text,
            resp->content_type ? resp->content_type : "application/octet-stream",
            (long)resp->content_length);
    }

    /* 缓存头 */
    if (resp->etag && resp->etag[0]) {
        if ((size_t)n < buf_size) {
            n += snprintf(buf + n, buf_size - (size_t)n, "ETag: %s\r\n", resp->etag);
        } else {
            n += (int)strlen(resp->etag) + 8;
        }
    }
    if (resp->last_modified && resp->last_modified[0]) {
        if ((size_t)n < buf_size) {
            n += snprintf(buf + n, buf_size - (size_t)n, "Last-Modified: %s\r\n", resp->last_modified);
        } else {
            n += (int)strlen(resp->last_modified) + 16;
        }
    }
    if (resp->content_encoding && resp->content_encoding[0]) {
        if ((size_t)n < buf_size) {
            n += snprintf(buf + n, buf_size - (size_t)n, "Content-Encoding: %s\r\n", resp->content_encoding);
        } else {
            n += (int)strlen(resp->content_encoding) + 20;
        }
    }

    /* 连接头 + 空行 */
    if ((size_t)n < buf_size) {
        n += snprintf(buf + n, buf_size - (size_t)n,
            "Connection: %s\r\n"
            "Server: Cocoon/1.0\r\n"
            "\r\n",
            resp->keep_alive ? "keep-alive" : "close");
    } else {
        n += 35; /* approximate */
    }

    if ((size_t)n >= buf_size) return -1;
    return n;
}

/**
 * http_request_free - 释放 HTTP 请求中动态分配的资源
 *
 * @param req 请求结构体
 */
void http_request_free(http_request_t *req) {
    if (req && req->body) {
        free(req->body);
        req->body = NULL;
        req->body_len = 0;
    }
}

/**
 * http_mime_type - 根据文件扩展名推断 MIME 类型
 *
 * 支持常见静态资源类型的自动识别。
 *
 * @param path 文件路径
 * @return MIME 类型字符串
 */
const char *http_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    /* 转小写比较 */
    char lower_ext[16];
    int i = 0;
    for (const char *p = ext + 1; *p && i < 15; p++, i++) {
        lower_ext[i] = (char)tolower((unsigned char)*p);
    }
    lower_ext[i] = '\0';

    if (strcmp(lower_ext, "html") == 0 || strcmp(lower_ext, "htm") == 0)  return "text/html; charset=utf-8";
    if (strcmp(lower_ext, "css") == 0)   return "text/css; charset=utf-8";
    if (strcmp(lower_ext, "js") == 0)    return "application/javascript";
    if (strcmp(lower_ext, "json") == 0)  return "application/json";
    if (strcmp(lower_ext, "png") == 0)   return "image/png";
    if (strcmp(lower_ext, "jpg") == 0 || strcmp(lower_ext, "jpeg") == 0) return "image/jpeg";
    if (strcmp(lower_ext, "gif") == 0)   return "image/gif";
    if (strcmp(lower_ext, "svg") == 0)   return "image/svg+xml";
    if (strcmp(lower_ext, "ico") == 0)   return "image/x-icon";
    if (strcmp(lower_ext, "woff2") == 0) return "font/woff2";
    if (strcmp(lower_ext, "woff") == 0)  return "font/woff";
    if (strcmp(lower_ext, "ttf") == 0)   return "font/ttf";
    if (strcmp(lower_ext, "txt") == 0)   return "text/plain; charset=utf-8";
    if (strcmp(lower_ext, "md") == 0)    return "text/markdown; charset=utf-8";
    if (strcmp(lower_ext, "xml") == 0)   return "application/xml";
    if (strcmp(lower_ext, "pdf") == 0)   return "application/pdf";
    if (strcmp(lower_ext, "zip") == 0)   return "application/zip";
    if (strcmp(lower_ext, "gz") == 0)    return "application/gzip";
    if (strcmp(lower_ext, "mp4") == 0)   return "video/mp4";
    if (strcmp(lower_ext, "webm") == 0)  return "video/webm";
    if (strcmp(lower_ext, "mp3") == 0)   return "audio/mpeg";
    if (strcmp(lower_ext, "ogg") == 0)   return "audio/ogg";
    if (strcmp(lower_ext, "wasm") == 0)  return "application/wasm";
    if (strcmp(lower_ext, "webmanifest") == 0) return "application/manifest+json";

    return "application/octet-stream";
}

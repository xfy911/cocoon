/**
 * http.h - HTTP 解析模块接口
 *
 * 提供 HTTP 请求解析和响应构建的接口。
 *
 * @author xfy
 */

#ifndef COCOON_HTTP_H
#define COCOON_HTTP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* === HTTP 方法 === */
typedef enum {
    HTTP_GET,
    HTTP_HEAD,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_OPTIONS,
    HTTP_UNKNOWN
} http_method_t;

/* === HTTP 请求 === */
#define HTTP_MAX_PATH       4096
#define HTTP_MAX_HEADERS    32
#define HTTP_HEADER_NAME_MAX 64
#define HTTP_HEADER_VALUE_MAX 1024
#define HTTP_MAX_BODY       8388608  /* 8MB 最大请求体 */

typedef struct {
    http_method_t method;
    char path[HTTP_MAX_PATH];
    char version[16];

    /* 请求头 */
    struct {
        char name[HTTP_HEADER_NAME_MAX];
        char value[HTTP_HEADER_VALUE_MAX];
    } headers[HTTP_MAX_HEADERS];
    int num_headers;

    /* 常用头快速访问 */
    int64_t content_length;
    bool keep_alive;
    bool has_range;
    int64_t range_start;
    int64_t range_end;
    
    /* 缓存相关头 */
    char if_none_match[64];
    char if_modified_since[64];
    bool has_if_none_match;
    bool has_if_modified_since;
    
    /* 压缩相关头 */
    bool accept_gzip;       /* 客户端支持 gzip */
    bool accept_brotli;     /* 客户端支持 brotli */
    bool accept_deflate;    /* 客户端支持 deflate */
    bool has_accept_encoding;

    /* 请求体 */
    char *body;             /* 请求体数据（动态分配） */
    size_t body_len;        /* 请求体实际长度 */
    char content_type[HTTP_HEADER_VALUE_MAX]; /* 解析后的 Content-Type */
} http_request_t;

/* === HTTP 响应 === */
typedef struct {
    int status_code;
    const char *status_text;
    const char *content_type;
    int64_t content_length;
    bool keep_alive;
    bool has_range;
    int64_t range_start;
    int64_t range_end;
    int64_t total_length;
    
    /* 缓存 */
    const char *etag;
    const char *last_modified;
    
    /* 压缩 */
    const char *content_encoding;

    /* CORS */
    const char *cors_origin;
} http_response_t;

/* === API === */
/**
 * http_parse_request - 从缓冲区解析 HTTP 请求
 *
 * @param buf 输入缓冲区
 * @param len 缓冲区长度
 * @param req 输出请求结构体
 * @return 成功解析的字节数，< 0 表示错误（-1 = 不完整，-2 = 格式错误）
 */
int http_parse_request(const char *buf, size_t len, http_request_t *req);

/**
 * http_format_response_header - 格式化响应头到缓冲区
 *
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param resp 响应信息
 * @return 写入的字节数，< 0 表示缓冲区不足
 */
int http_format_response_header(char *buf, size_t buf_size, const http_response_t *resp);

/**
 * http_method_str - HTTP 方法转字符串
 *
 * @param method HTTP 方法
 * @return 方法字符串（如 "GET"）
 */
const char *http_method_str(http_method_t method);

/**
 * http_request_free - 释放 HTTP 请求中动态分配的资源
 *
 * @param req 请求结构体
 */
void http_request_free(http_request_t *req);

/**
 * http_mime_type - 根据文件扩展名获取 MIME 类型
 *
 * @param path 文件路径
 * @return MIME 类型字符串
 */
const char *http_mime_type(const char *path);

#endif /* COCOON_HTTP_H */

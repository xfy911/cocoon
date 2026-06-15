/*
 * fastcgi.h - FastCGI 协议客户端实现
 *
 * 支持 FastCGI 1.0 协议 (RFC 未正式标准化，但广泛使用)
 * 用于连接 PHP-FPM、uWSGI 等 FastCGI 后端
 */

#ifndef COCOON_FASTCGI_H
#define COCOON_FASTCGI_H

#include "cocoon.h"
#include <stdint.h>
#include <stdbool.h>

/* FastCGI 协议版本 */
#define FCGI_VERSION_1       1

/* FastCGI 消息类型 */
#define FCGI_BEGIN_REQUEST   1
#define FCGI_ABORT_REQUEST   2
#define FCGI_END_REQUEST     3
#define FCGI_PARAMS          4
#define FCGI_STDIN           5
#define FCGI_STDOUT          6
#define FCGI_STDERR          7
#define FCGI_DATA            8
#define FCGI_GET_VALUES      9
#define FCGI_GET_VALUES_RESULT 10
#define FCGI_UNKNOWN_TYPE    11

/* FastCGI 请求角色 */
#define FCGI_RESPONDER       1
#define FCGI_AUTHORIZER      2
#define FCGI_FILTER          3

/* FastCGI 标志 */
#define FCGI_KEEP_CONN       1

/* FastCGI 协议状态码 */
#define FCGI_REQUEST_COMPLETE 0
#define FCGI_CANT_MPX_CONN    1
#define FCGI_OVERLOADED       2
#define FCGI_UNKNOWN_ROLE     3

/* FastCGI 记录头大小 */
#define FCGI_HEADER_LEN       8
#define FCGI_MAX_CONTENT_LEN  65535

/* FastCGI 记录头 */
typedef struct {
    uint8_t version;
    uint8_t type;
    uint16_t requestId;
    uint16_t contentLength;
    uint8_t paddingLength;
    uint8_t reserved;
} fcgi_header_t;

/* FastCGI BeginRequest 体 */
typedef struct {
    uint16_t role;
    uint8_t flags;
    uint8_t reserved[5];
} fcgi_begin_request_t;

/* FastCGI EndRequest 体 */
typedef struct {
    uint32_t appStatus;
    uint8_t protocolStatus;
    uint8_t reserved[3];
} fcgi_end_request_t;

/* FastCGI 参数对（名值对） */
typedef struct fcgi_param {
    char *name;
    char *value;
    struct fcgi_param *next;
} fcgi_param_t;

/* FastCGI 请求上下文 */
typedef struct {
    uint16_t requestId;
    int backend_fd;           /* 到后端 FastCGI 服务器的连接 */
    bool keep_conn;           /* 是否保持连接 */
    fcgi_param_t *params;     /* 参数链表 */
    uint16_t next_requestId;  /* 下一个请求 ID（连接复用） */
} fcgi_request_t;

/* FastCGI 响应解析状态 */
typedef enum {
    FCGI_PARSE_HEADER,        /* 解析记录头 */
    FCGI_PARSE_CONTENT,       /* 解析内容 */
    FCGI_PARSE_PADDING,       /* 解析填充 */
    FCGI_PARSE_DONE           /* 完成 */
} fcgi_parse_state_t;

/* FastCGI 响应 */
typedef struct {
    int status;               /* HTTP 状态码 */
    char *stdout_data;        /* stdout 数据 */
    size_t stdout_len;
    char *stderr_data;        /* stderr 数据 */
    size_t stderr_len;
    bool complete;            /* 是否收到 END_REQUEST */
    uint32_t appStatus;         /* FastCGI 应用状态码 */
    uint8_t protocolStatus;     /* FastCGI 协议状态码 */
} fcgi_response_t;

/* FastCGI 后端配置 */
typedef struct {
    char *host;               /* 主机或 Unix socket 路径 */
    int port;                 /* 端口（TCP 时有效） */
    bool is_unix_socket;      /* 是否为 Unix domain socket */
    int max_conns;            /* 最大连接数 */
    int timeout_ms;           /* 连接超时（毫秒） */
} fcgi_backend_t;

/* FastCGI 连接池项 */
typedef struct fcgi_pool_item {
    int fd;                   /* 连接文件描述符 */
    bool in_use;              /* 是否正在使用 */
    bool available;           /* 是否可用（连接正常） */
    uint16_t last_requestId;  /* 最后使用的请求 ID */
    struct fcgi_pool_item *next;
} fcgi_pool_item_t;

/* FastCGI 连接池 */
typedef struct {
    fcgi_backend_t *backend;
    fcgi_pool_item_t *pool;   /* 连接链表 */
    int pool_size;            /* 当前连接数 */
    int max_pool_size;        /* 最大连接数 */
} fcgi_pool_t;

/* ========== 核心 API ========== */

/* 创建/销毁请求 */
bool fcgi_request_init(fcgi_request_t *req, uint16_t requestId);
void fcgi_request_free(fcgi_request_t *req);

/* 添加参数（CGI 环境变量） */
bool fcgi_add_param(fcgi_request_t *req, const char *name, const char *value);

/* 构建 FastCGI 记录 */
int fcgi_build_record(uint8_t type, uint16_t requestId,
                      const uint8_t *data, uint16_t len,
                      uint8_t *out, size_t out_size);

/* 构建 BeginRequest 记录 */
int fcgi_build_begin_request(uint16_t requestId, uint16_t role,
                             uint8_t flags, uint8_t *out, size_t out_size);

/* 构建 Params 记录（编码所有参数） */
int fcgi_build_params(uint16_t requestId, const fcgi_param_t *params,
                      uint8_t *out, size_t out_size);

/* 构建空 Stdin 记录（表示输入结束） */
int fcgi_build_empty_stdin(uint16_t requestId, uint8_t *out, size_t out_size);

/* 解析响应 */
bool fcgi_parse_response(const uint8_t *data, size_t len,
                         fcgi_response_t *resp, size_t *consumed);

/* 解析响应记录（流式） */
bool fcgi_parse_record(const uint8_t *data, size_t len,
                       fcgi_header_t *header, const uint8_t **content,
                       size_t *consumed);

/* 提取 HTTP 状态码（从 stdout 的 Status 头） */
int fcgi_extract_status(const char *stdout_data, size_t len);

/* 提取 HTTP 响应体（跳过头部） */
bool fcgi_extract_body(const char *stdout_data, size_t len,
                       const char **body, size_t *body_len);

/* ========== 连接池 API ========== */

/* 初始化/销毁连接池 */
bool fcgi_pool_init(fcgi_pool_t *pool, fcgi_backend_t *backend, int max_conns);
void fcgi_pool_destroy(fcgi_pool_t *pool);

/* 获取连接 */
fcgi_pool_item_t *fcgi_pool_acquire(fcgi_pool_t *pool);

/* 释放连接（归还到池） */
void fcgi_pool_release(fcgi_pool_t *pool, fcgi_pool_item_t *item);

/* 连接到后端 */
bool fcgi_backend_connect(fcgi_backend_t *backend);

/* 发送完整请求 */
bool fcgi_send_request(int fd, const fcgi_request_t *req,
                       const uint8_t *body, size_t body_len);

/* 接收完整响应 */
bool fcgi_recv_response(int fd, fcgi_response_t *resp, int timeout_ms);

/* 清理响应 */
void fcgi_response_free(fcgi_response_t *resp);

/* 完整请求流程（连接池版） */
bool fcgi_request(fcgi_pool_t *pool, fcgi_request_t *req,
                  const uint8_t *body, size_t body_len,
                  fcgi_response_t *resp);

/* 实用函数：编码名值对长度 */
bool fcgi_encode_name_value_len(const char *name, const char *value,
                                uint8_t *out, size_t *out_len);

#endif /* COCOON_FASTCGI_H */

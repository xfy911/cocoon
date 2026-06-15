/*
 * fastcgi.c - FastCGI 协议客户端实现
 *
 * 实现 FastCGI 1.0 协议的核心功能：
 * - 记录编码/解码
 * - 参数序列化
 * - 响应解析
 * - 连接池管理
 */

#include "fastcgi.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

/* 内部辅助函数：设置非阻塞（暂留供后续使用） */
__attribute__((unused)) static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* 内部辅助函数：带超时的读取 */
static ssize_t read_with_timeout(int fd, void *buf, size_t len, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0) return -1;
    if (rc == 0) { errno = ETIMEDOUT; return -1; }
    return read(fd, buf, len);
}

/* 内部辅助函数：带超时的写入 */
static ssize_t write_with_timeout(int fd, const void *buf, size_t len, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0) return -1;
    if (rc == 0) { errno = ETIMEDOUT; return -1; }
    return write(fd, buf, len);
}

/* ========== 请求初始化 ========== */

bool fcgi_request_init(fcgi_request_t *req, uint16_t requestId) {
    if (!req) return false;
    memset(req, 0, sizeof(*req));
    req->requestId = requestId;
    req->backend_fd = -1;
    req->keep_conn = true;
    req->next_requestId = 1;
    return true;
}

void fcgi_request_free(fcgi_request_t *req) {
    if (!req) return;
    fcgi_param_t *p = req->params;
    while (p) {
        fcgi_param_t *next = p->next;
        free(p->name);
        free(p->value);
        free(p);
        p = next;
    }
    req->params = NULL;
}

/* ========== 参数管理 ========== */

bool fcgi_add_param(fcgi_request_t *req, const char *name, const char *value) {
    if (!req || !name || !value) return false;
    
    fcgi_param_t *param = calloc(1, sizeof(fcgi_param_t));
    if (!param) return false;
    
    param->name = strdup(name);
    param->value = strdup(value);
    if (!param->name || !param->value) {
        free(param->name);
        free(param->value);
        free(param);
        return false;
    }
    
    /* 头插法 */
    param->next = req->params;
    req->params = param;
    return true;
}

/* ========== 记录编码 ========== */

int fcgi_build_record(uint8_t type, uint16_t requestId,
                      const uint8_t *data, uint16_t len,
                      uint8_t *out, size_t out_size) {
    uint16_t total_len = FCGI_HEADER_LEN + len + ((len % 8) ? (8 - len % 8) : 0);
    if (!out || out_size < total_len) {
        return -1;
    }
    
    uint8_t padding = (len % 8) ? (8 - len % 8) : 0;
    
    out[0] = FCGI_VERSION_1;
    out[1] = type;
    out[2] = (requestId >> 8) & 0xFF;
    out[3] = requestId & 0xFF;
    out[4] = (len >> 8) & 0xFF;
    out[5] = len & 0xFF;
    out[6] = padding;
    out[7] = 0;
    
    if (len > 0 && data) {
        memcpy(out + FCGI_HEADER_LEN, data, len);
    }
    memset(out + FCGI_HEADER_LEN + len, 0, padding);
    
    return FCGI_HEADER_LEN + len + padding;
}

int fcgi_build_begin_request(uint16_t requestId, uint16_t role,
                             uint8_t flags, uint8_t *out, size_t out_size) {
    fcgi_begin_request_t body;
    memset(&body, 0, sizeof(body));
    body.role = (role >> 8) | ((role & 0xFF) << 8); /* 转大端 */
    body.flags = flags;
    return fcgi_build_record(FCGI_BEGIN_REQUEST, requestId,
                             (const uint8_t *)&body, sizeof(body), out, out_size);
}

int fcgi_build_empty_stdin(uint16_t requestId, uint8_t *out, size_t out_size) {
    return fcgi_build_record(FCGI_STDIN, requestId, NULL, 0, out, out_size);
}

/* ========== 名值对编码 ========== */

bool fcgi_encode_name_value_len(const char *name, const char *value,
                                uint8_t *out, size_t *out_len) {
    if (!name || !value || !out || !out_len) return false;
    
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    size_t idx = 0;
    
    /* 编码名称长度 */
    if (name_len < 128) {
        out[idx++] = (uint8_t)name_len;
    } else {
        out[idx++] = (uint8_t)((name_len >> 24) | 0x80);
        out[idx++] = (uint8_t)(name_len >> 16);
        out[idx++] = (uint8_t)(name_len >> 8);
        out[idx++] = (uint8_t)name_len;
    }
    
    /* 编码值长度 */
    if (value_len < 128) {
        out[idx++] = (uint8_t)value_len;
    } else {
        out[idx++] = (uint8_t)((value_len >> 24) | 0x80);
        out[idx++] = (uint8_t)(value_len >> 16);
        out[idx++] = (uint8_t)(value_len >> 8);
        out[idx++] = (uint8_t)value_len;
    }
    
    memcpy(out + idx, name, name_len);
    idx += name_len;
    memcpy(out + idx, value, value_len);
    idx += value_len;
    
    *out_len = idx;
    return true;
}

int fcgi_build_params(uint16_t requestId, const fcgi_param_t *params,
                      uint8_t *out, size_t out_size) {
    if (!out || out_size < FCGI_HEADER_LEN + 16) return -1;
    
    /* 先计算所有参数总大小 */
    size_t total_param_size = 0;
    for (const fcgi_param_t *p = params; p; p = p->next) {
        size_t nl = strlen(p->name);
        size_t vl = strlen(p->value);
        total_param_size += nl + vl + (nl < 128 ? 1 : 4) + (vl < 128 ? 1 : 4);
    }
    
    if (total_param_size == 0) {
        /* 空 params 记录 */
        return fcgi_build_record(FCGI_PARAMS, requestId, NULL, 0, out, out_size);
    }
    
    /* 参数可能超过 65535，需要分片 */
    uint8_t *buf = malloc(total_param_size);
    if (!buf) return -1;
    
    size_t offset = 0;
    for (const fcgi_param_t *p = params; p; p = p->next) {
        size_t len = 0;
        fcgi_encode_name_value_len(p->name, p->value, buf + offset, &len);
        offset += len;
    }
    
    /* 分片发送 */
    size_t sent = 0;
    int total_written = 0;
    while (sent < offset) {
        uint16_t chunk = (offset - sent > FCGI_MAX_CONTENT_LEN) ? 
                         FCGI_MAX_CONTENT_LEN : (uint16_t)(offset - sent);
        
        uint16_t need = FCGI_HEADER_LEN + chunk + 8;
        if (out_size < need) {
            free(buf);
            return -1;
        }
        
        int n = fcgi_build_record(FCGI_PARAMS, requestId,
                                  buf + sent, chunk, out, out_size);
        if (n < 0) {
            free(buf);
            return -1;
        }
        out += n;
        out_size -= n;
        total_written += n;
        sent += chunk;
    }
    
    /* 结束标记 */
    int n = fcgi_build_record(FCGI_PARAMS, requestId, NULL, 0, out, out_size);
    if (n < 0) {
        free(buf);
        return -1;
    }
    total_written += n;
    
    free(buf);
    return total_written;
}

/* ========== 记录解析 ========== */

bool fcgi_parse_record(const uint8_t *data, size_t len,
                       fcgi_header_t *header, const uint8_t **content,
                       size_t *consumed) {
    if (!data || len < FCGI_HEADER_LEN || !header || !consumed) return false;
    
    header->version = data[0];
    header->type = data[1];
    header->requestId = ((uint16_t)data[2] << 8) | data[3];
    header->contentLength = ((uint16_t)data[4] << 8) | data[5];
    header->paddingLength = data[6];
    header->reserved = data[7];
    
    size_t total = FCGI_HEADER_LEN + header->contentLength + header->paddingLength;
    if (len < total) return false; /* 数据不足 */
    
    *content = data + FCGI_HEADER_LEN;
    *consumed = total;
    return true;
}

/* ========== 响应解析 ========== */

bool fcgi_parse_response(const uint8_t *data, size_t len,
                         fcgi_response_t *resp, size_t *consumed) {
    if (!data || !resp || !consumed) return false;
    
    *consumed = 0;
    size_t offset = 0;
    
    while (offset < len) {
        fcgi_header_t header;
        const uint8_t *content = NULL;
        size_t rec_consumed = 0;
        
        if (!fcgi_parse_record(data + offset, len - offset, &header, &content, &rec_consumed)) {
            break; /* 数据不足，等下次 */
        }
        
        offset += rec_consumed;
        
        switch (header.type) {
            case FCGI_STDOUT:
                if (header.contentLength > 0) {
                    size_t new_len = resp->stdout_len + header.contentLength;
                    char *new_data = realloc(resp->stdout_data, new_len + 1);
                    if (!new_data) return false;
                    resp->stdout_data = new_data;
                    memcpy(resp->stdout_data + resp->stdout_len, content, header.contentLength);
                    resp->stdout_len = new_len;
                    resp->stdout_data[resp->stdout_len] = '\0';
                }
                break;
                
            case FCGI_STDERR:
                if (header.contentLength > 0) {
                    size_t new_len = resp->stderr_len + header.contentLength;
                    char *new_data = realloc(resp->stderr_data, new_len + 1);
                    if (!new_data) return false;
                    resp->stderr_data = new_data;
                    memcpy(resp->stderr_data + resp->stderr_len, content, header.contentLength);
                    resp->stderr_len = new_len;
                    resp->stderr_data[resp->stderr_len] = '\0';
                }
                break;
                
            case FCGI_END_REQUEST:
                if (header.contentLength >= sizeof(fcgi_end_request_t)) {
                    const fcgi_end_request_t *end = (const fcgi_end_request_t *)content;
                    resp->appStatus = ((end->appStatus >> 24) & 0xFF) |
                                      ((end->appStatus >> 8) & 0xFF00) |
                                      ((end->appStatus << 8) & 0xFF0000) |
                                      ((end->appStatus << 24) & 0xFF000000);
                    resp->protocolStatus = end->protocolStatus;
                    resp->complete = true;
                }
                break;
                
            case FCGI_GET_VALUES_RESULT:
                /* 暂不处理 */
                break;
                
            default:
                /* 忽略未知类型 */
                break;
        }
    }
    
    *consumed = offset;
    return true;
}

void fcgi_response_free(fcgi_response_t *resp) {
    if (!resp) return;
    free(resp->stdout_data);
    free(resp->stderr_data);
    memset(resp, 0, sizeof(*resp));
}

/* ========== 状态码提取 ========== */

int fcgi_extract_status(const char *stdout_data, size_t len) {
    if (!stdout_data || len == 0) return 200;
    
    const char *p = stdout_data;
    const char *end = stdout_data + len;
    
    /* 查找 Status: 头 */
    while (p < end) {
        const char *line_end = memchr(p, '\n', end - p);
        if (!line_end) line_end = end;
        
        size_t line_len = line_end - p;
        if (line_len > 8 && strncasecmp(p, "Status: ", 8) == 0) {
            int status = atoi(p + 8);
            return status > 0 ? status : 200;
        }
        
        /* 空行表示头部结束 */
        if (line_len == 0 || (line_len == 1 && p[0] == '\r')) {
            break;
        }
        p = line_end + 1;
    }
    
    return 200; /* 默认 200 */
}

bool fcgi_extract_body(const char *stdout_data, size_t len,
                       const char **body, size_t *body_len) {
    if (!stdout_data || !body || !body_len) return false;
    
    const char *p = stdout_data;
    const char *end = stdout_data + len;
    
    /* 查找空行分隔 */
    while (p < end - 1) {
        if ((p[0] == '\n' && p[1] == '\n') ||
            (p[0] == '\r' && p[1] == '\n' && p + 2 < end && p[2] == '\r' && p[3] == '\n')) {
            if (p[0] == '\r') {
                *body = p + 4;
                *body_len = end - (p + 4);
            } else {
                *body = p + 2;
                *body_len = end - (p + 2);
            }
            return true;
        }
        if (p[0] == '\n' && p[1] == '\r' && p + 2 < end && p[2] == '\n') {
            *body = p + 3;
            *body_len = end - (p + 3);
            return true;
        }
        p++;
    }
    
    /* 没找到空行，全部视为 body */
    *body = stdout_data;
    *body_len = len;
    return true;
}

/* ========== 后端连接 ========== */

bool fcgi_backend_connect(fcgi_backend_t *backend) {
    if (!backend) return false;
    
    int fd = -1;
    
    if (backend->is_unix_socket) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, backend->host, sizeof(addr.sun_path) - 1);
        
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return false;
        }
    } else {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(backend->port);
        if (inet_pton(AF_INET, backend->host, &addr.sin_addr) <= 0) {
            close(fd);
            return false;
        }
        
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return false;
        }
    }
    
    return fd >= 0;
}

/* ========== 连接池 ========== */

bool fcgi_pool_init(fcgi_pool_t *pool, fcgi_backend_t *backend, int max_conns) {
    if (!pool || !backend || max_conns <= 0) return false;
    memset(pool, 0, sizeof(*pool));
    
    pool->backend = backend;
    pool->max_pool_size = max_conns;
    pool->pool = NULL;
    pool->pool_size = 0;
    
    /* 预创建连接 */
    for (int i = 0; i < max_conns; i++) {
        int fd = fcgi_backend_connect(backend);
        if (fd < 0) {
            /* 部分连接失败也可以接受 */
            if (i == 0) {
                log_error("FastCGI: 无法连接后端 %s", backend->host);
                return false;
            }
            break;
        }
        
        fcgi_pool_item_t *item = calloc(1, sizeof(fcgi_pool_item_t));
        if (!item) {
            close(fd);
            continue;
        }
        item->fd = fd;
        item->in_use = false;
        item->available = true;
        item->last_requestId = 0;
        item->next = pool->pool;
        pool->pool = item;
        pool->pool_size++;
    }
    
    log_info("FastCGI: 连接池初始化完成，%d/%d 连接就绪", pool->pool_size, max_conns);
    return pool->pool_size > 0;
}

void fcgi_pool_destroy(fcgi_pool_t *pool) {
    if (!pool) return;
    
    fcgi_pool_item_t *item = pool->pool;
    while (item) {
        fcgi_pool_item_t *next = item->next;
        if (item->fd >= 0) close(item->fd);
        free(item);
        item = next;
    }
    pool->pool = NULL;
    pool->pool_size = 0;
}

fcgi_pool_item_t *fcgi_pool_acquire(fcgi_pool_t *pool) {
    if (!pool) return NULL;
    
    /* 简单遍历找可用连接 */
    fcgi_pool_item_t *item = pool->pool;
    while (item) {
        if (!item->in_use && item->available) {
            item->in_use = true;
            return item;
        }
        item = item->next;
    }
    
    /* 没有可用连接，尝试新建一个 */
    if (pool->pool_size < pool->max_pool_size) {
        int fd = fcgi_backend_connect(pool->backend);
        if (fd >= 0) {
            fcgi_pool_item_t *item = calloc(1, sizeof(fcgi_pool_item_t));
            if (item) {
                item->fd = fd;
                item->in_use = true;
                item->available = true;
                item->next = pool->pool;
                pool->pool = item;
                pool->pool_size++;
                return item;
            }
            close(fd);
        }
    }
    
    return NULL;
}

void fcgi_pool_release(fcgi_pool_t *pool, fcgi_pool_item_t *item) {
    if (!pool || !item) return;
    item->in_use = false;
    
    /* 检查连接是否还可用（简单检查） */
    char buf[1];
    int rc = recv(item->fd, buf, 1, MSG_DONTWAIT | MSG_PEEK);
    if (rc == 0) {
        /* 对端关闭 */
        item->available = false;
        close(item->fd);
        item->fd = -1;
    } else if (rc < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        /* 连接错误 */
        item->available = false;
        close(item->fd);
        item->fd = -1;
    }
}

/* ========== 发送请求 ========== */

bool fcgi_send_request(int fd, const fcgi_request_t *req,
                       const uint8_t *body, size_t body_len) {
    if (fd < 0 || !req) return false;
    
    uint8_t buf[65536];
    int n;
    
    /* 1. BeginRequest */
    n = fcgi_build_begin_request(req->requestId, FCGI_RESPONDER,
                                 req->keep_conn ? FCGI_KEEP_CONN : 0,
                                 buf, sizeof(buf));
    if (n < 0) return false;
    if (write_with_timeout(fd, buf, n, 30000) != n) return false;
    
    /* 2. Params */
    n = fcgi_build_params(req->requestId, req->params, buf, sizeof(buf));
    if (n < 0) return false;
    if (write_with_timeout(fd, buf, n, 30000) != n) return false;
    
    /* 3. Stdin */
    if (body && body_len > 0) {
        size_t sent = 0;
        while (sent < body_len) {
            uint16_t chunk = (body_len - sent > FCGI_MAX_CONTENT_LEN) ?
                             FCGI_MAX_CONTENT_LEN : (uint16_t)(body_len - sent);
            n = fcgi_build_record(FCGI_STDIN, req->requestId, body + sent, chunk, buf, sizeof(buf));
            if (n < 0) return false;
            if (write_with_timeout(fd, buf, n, 30000) != n) return false;
            sent += chunk;
        }
    }
    
    /* 4. 空 Stdin 结束 */
    n = fcgi_build_empty_stdin(req->requestId, buf, sizeof(buf));
    if (n < 0) return false;
    if (write_with_timeout(fd, buf, n, 30000) != n) return false;
    
    return true;
}

/* ========== 接收响应 ========== */

bool fcgi_recv_response(int fd, fcgi_response_t *resp, int timeout_ms) {
    if (fd < 0 || !resp) return false;
    
    memset(resp, 0, sizeof(*resp));
    
    uint8_t buf[65536];
    int total_timeout = timeout_ms;
    int elapsed = 0;
    
    while (!resp->complete && elapsed < total_timeout) {
        int remaining = total_timeout - elapsed;
        int start = elapsed;
        
        ssize_t n = read_with_timeout(fd, buf, sizeof(buf), remaining);
        if (n < 0) {
            if (errno == ETIMEDOUT) break;
            return false;
        }
        if (n == 0) break; /* 对端关闭 */
        
        size_t consumed = 0;
        if (!fcgi_parse_response(buf, n, resp, &consumed)) {
            return false;
        }
        
        elapsed += (elapsed - start); /* 实际已过时间 */
    }
    
    return resp->complete;
}

/* ========== 完整请求 ========== */

bool fcgi_request(fcgi_pool_t *pool, fcgi_request_t *req,
                  const uint8_t *body, size_t body_len,
                  fcgi_response_t *resp) {
    if (!pool || !req || !resp) return false;
    
    fcgi_pool_item_t *item = fcgi_pool_acquire(pool);
    if (!item) return false;
    
    /* 分配请求 ID */
    req->requestId = ++item->last_requestId;
    if (req->requestId == 0) req->requestId = 1; /* 避免溢出 */
    
    bool ok = fcgi_send_request(item->fd, req, body, body_len) &&
              fcgi_recv_response(item->fd, resp, pool->backend->timeout_ms);
    
    fcgi_pool_release(pool, item);
    return ok;
}

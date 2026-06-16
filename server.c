/**
 *
 * 基于 coco 协程库实现高并发静态资源服务器。
 * 每个客户端连接由一个独立的协程处理，主线程负责 accept。
 *
 * 架构:
 *   主线程: socket() → bind() → listen() → accept() → 创建协程
 *   协程:   读取请求 → 解析 HTTP → 服务静态资源 → 关闭连接
 *
 * 新增功能（2026-06-03）:
 *   - 连接空闲超时管理（自动清理僵尸连接）
 *   - 最大并发连接数限制（防止资源耗尽）
 *   - 分级日志输出（替代 printf）
 *
 * @author xfy
 */

#include "server.h"
#include "plugin.h"
#include "http.h"
#include "static.h"
#include "cocoon.h"
#include "log.h"
#include "multipart.h"
#include "tls.h"
#include "http2.h"
#include "access_log.h"
#include "websocket.h"
#include "platform.h"
#include "middleware.h"
#include "proxy.h"
#include "healthcheck.h"
#include "sse.h"
#include "config.h"
#include "fcgi_handler.h"
#include "cache.h"
#include "dashboard.h"
#include "acme.h"
#include "throttle.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <poll.h>

#include "coco.h"

/* 单个连接缓冲区大小 */
#define CONN_BUF_SIZE   8192
/* 默认连接超时（毫秒） */
#define CONN_TIMEOUT_MS 30000

/**
 * connection_t - 单个客户端连接上下文
 *
 * 包含 socket fd、接收缓冲区、解析状态、超时管理、客户端地址、响应状态。
 */
typedef struct {
    cocoon_socket_t fd;             /**< 客户端 socket */
    char            buf[CONN_BUF_SIZE]; /**< 接收缓冲区 */
    size_t          buf_len;        /**< 缓冲区已用长度 */
    bool            keep_alive;     /**< 当前连接是否保持 */
    bool            closed;         /**< 连接是否已关闭 */
    const char     *root_dir;       /**< 静态资源根目录（引用，不拥有） */
    uint32_t        timeout_ms;     /**< 连接空闲超时毫秒（从配置复制） */
    coco_timer_t   *timer;          /**< 空闲超时定时器 */
    coco_coro_t    *coro;           /**< 当前处理协程 */
    bool        gzip_enabled;    /**< 是否启用 gzip 压缩 */
    bool        brotli_enabled;  /**< 是否启用 brotli 压缩 */
    struct sockaddr_storage client_addr; /**< 客户端地址 */
    socklen_t       addr_len;       /**< 地址长度 */
    int             response_status; /**< 最后响应的 HTTP 状态码 */
    server_context_t *ctx;            /**< 服务器上下文（用于访问代理配置） */
    cocoon_throttle_t *throttle;      /**< 每连接限速器（NULL 表示不限制） */
} connection_t;
struct server_context {
    cocoon_socket_t     listen_fd;    /**< 监听 socket */
    cocoon_config_t     config;       /**< 配置副本 */
    cocoon_middleware_config_t mw_config; /**< 中间件配置（持久化，避免栈变量悬空） */
    cocoon_proxy_config_t proxy_config; /**< 反向代理配置 */
    cocoon_healthcheck_manager_t hc_manager; /**< 主动健康检查管理器 */
    volatile int        running;      /**< 运行标志 */
    coco_sched_t       *sched;        /**< 协程调度器 */
    const char         *config_file_path; /**< 配置文件路径（用于热重载） */
    /* FastCGI 配置 */
    cocoon_fcgi_config_t fcgi_config;
    volatile int        reload_requested; /**< 配置热重载请求标志 */
    /* 内存缓存 */
    cocoon_cache_t     *cache;        /**< 内存响应缓存（NULL 表示未启用） */
    /* ACME 客户端 */
    acme_ctx_t         *acme;         /**< ACME 上下文（NULL 表示未启用） */
    /* 全局限速器 */
    cocoon_throttle_t  *global_throttle; /**< 全局总限速（NULL 表示未启用） */
};
/* 服务器启动时间 */
time_t g_server_start_time = 0;
/* 最大连接数（供健康检查端点使用） */
uint32_t g_max_connections = 0;
/* Prometheus 指标计数器 */
atomic_int g_active_connections = 0;
atomic_uint g_total_requests = 0;
atomic_uint g_response_2xx = 0;
atomic_uint g_response_3xx = 0;
atomic_uint g_response_4xx = 0;
atomic_uint g_response_5xx = 0;
atomic_uint g_response_200 = 0;
atomic_uint g_response_404 = 0;

/**
 * update_metrics - 更新 Prometheus 指标计数器
 *
 * 根据 HTTP 响应状态码更新分类计数器。
 *
 * @param status HTTP 响应状态码
 */
static void update_metrics(int status) {
    atomic_fetch_add(&g_total_requests, 1);
    if (status >= 200 && status < 300) atomic_fetch_add(&g_response_2xx, 1);
    else if (status >= 300 && status < 400) atomic_fetch_add(&g_response_3xx, 1);
    else if (status >= 400 && status < 500) atomic_fetch_add(&g_response_4xx, 1);
    else if (status >= 500) atomic_fetch_add(&g_response_5xx, 1);
    if (status == 200) atomic_fetch_add(&g_response_200, 1);
    if (status == 404) atomic_fetch_add(&g_response_404, 1);
}

/**
 * set_nonblocking - 设置 socket 为非阻塞模式
 *
 * @param fd socket 文件描述符
 */
static void set_nonblocking(cocoon_socket_t fd) {
    cocoon_socket_nonblock(fd);
}

/**
 * close_connection - 安全关闭连接
 *
 * 关闭 socket，递减活跃连接计数。
 *
 * @param conn 连接上下文
 */
static void close_connection(connection_t *conn) {
    if (conn && conn->fd != COCOON_INVALID_SOCKET) {
        tls_close(conn->fd);
        cocoon_socket_close(conn->fd);
        conn->fd = COCOON_INVALID_SOCKET;
        conn->closed = true;
        /* 注销限速 */
        if (conn->throttle) {
            throttle_clear_fd(conn->fd);
            free(conn->throttle);
            conn->throttle = NULL;
        }
        atomic_fetch_sub(&g_active_connections, 1);
    }
}

/**
 * conn_read - 从连接读取数据（协程安全）
 *
 * 使用 coco 的 I/O API 进行非阻塞读取，协程自动 yield 等待数据就绪。
 * 如果协程调度器不可用（多线程模式），回退到普通 read。
 *
 * @param conn 连接上下文
 * @return 读取的字节数，0 表示对端关闭，-1 表示错误
 */
static ssize_t conn_read(connection_t *conn) {
    if (!conn || conn->fd == COCOON_INVALID_SOCKET || conn->closed) return -1;

    size_t space = CONN_BUF_SIZE - conn->buf_len;
    if (space == 0) return -1; /* 缓冲区满 */

    ssize_t n;

    if (tls_has_connection(conn->fd)) {
        n = tls_read(conn->fd, conn->buf + conn->buf_len, space);
    } else if (coco_sched_get_current() != NULL) {
        n = coco_read(conn->fd, conn->buf + conn->buf_len, space);
    } else {
        /* 无调度器时直接 read */
        n = read(conn->fd, conn->buf + conn->buf_len, space);
    }

    if (n > 0) {
        conn->buf_len += (size_t)n;
    }
    return n;
}

/**
 * conn_read_body - 读取请求体
 *
 * 从连接缓冲区或 socket 读取剩余请求体数据。
 * 如果缓冲区中已有部分数据，优先使用。
 *
 * @param conn 连接上下文
 * @param req HTTP 请求
 * @param need 需要读取的字节数
 * @return 0 成功，-1 错误
 */
static int conn_read_body(connection_t *conn, http_request_t *req, size_t need) {
    if (need == 0) return 0;
    if (need > HTTP_MAX_BODY) {
        log_warn("请求体过大 (%zu > %d)，拒绝", need, HTTP_MAX_BODY);
        return -1;
    }

    req->body = (char *)malloc(need + 1);
    if (!req->body) return -1;
    req->body[need] = '\0';

    size_t got = 0;

    /* 先消费缓冲区中的数据 */
    if (conn->buf_len > 0) {
        size_t from_buf = conn->buf_len < need ? conn->buf_len : need;
        memcpy(req->body, conn->buf, from_buf);
        got = from_buf;
        if (from_buf < conn->buf_len) {
            memmove(conn->buf, conn->buf + from_buf, conn->buf_len - from_buf);
        }
        conn->buf_len -= from_buf;
    }

    /* 从 socket 读取剩余数据 */
    while (got < need) {
        ssize_t n;
        if (tls_has_connection(conn->fd)) {
            n = tls_read(conn->fd, req->body + got, need - got);
        } else if (coco_sched_get_current() != NULL) {
            n = coco_read(conn->fd, req->body + got, need - got);
        } else {
            n = cocoon_socket_recv(conn->fd, req->body + got, need - got);
        }
        if (n > 0) {
            got += (size_t)n;
        } else if (n < 0) {
            int err = cocoon_get_last_error();
            if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) continue;
            free(req->body);
            req->body = NULL;
            return -1;
        } else {
            /* 对端关闭 */
            free(req->body);
            req->body = NULL;
            return -1;
        }
    }

    req->body_len = got;
    return 0;
}

/**
 * handle_post_request - 处理 POST 请求
 *
 * 支持 multipart/form-data 文件上传、JSON 回显和表单回显。
 *
 * @param fd 客户端 socket
 * @param req HTTP 请求
 * @param root_dir 静态资源根目录（用于保存上传文件）
 * @return true 保持连接
 */
static bool handle_post_request(int fd, const http_request_t *req, const char *root_dir) {
    char response[4096];
    int n = 0;

    /* multipart/form-data 文件上传 */
    if (strstr(req->content_type, "multipart/form-data") != NULL) {
        char boundary[256];
        if (!multipart_extract_boundary(req->content_type, boundary, sizeof(boundary))) {
            static_send_error(fd, 400, req->keep_alive);
            return req->keep_alive;
        }

        multipart_part_t *parts = NULL;
        int num_parts = 0;
        if (multipart_parse(req->body, req->body_len, boundary, &parts, &num_parts) != 0) {
            static_send_error(fd, 400, req->keep_alive);
            return req->keep_alive;
        }

        /* 构建 JSON 响应 */
        n += snprintf(response + n, sizeof(response) - n,
            "{\"method\":\"%s\",\"path\":\"%s\",\"uploaded\":%d,\"files\":[",
            http_method_str(req->method), req->path, num_parts);

        int files_saved = 0;
        for (int i = 0; i < num_parts; i++) {
            if (parts[i].filename && parts[i].filename[0] && parts[i].data_len > 0) {
                /* 保存文件到 root_dir/uploads/ */
                char upload_dir[4096];
                int r = snprintf(upload_dir, sizeof(upload_dir), "%s/uploads", root_dir);
                if (r > 0 && r < (int)sizeof(upload_dir)) {
                    /* 创建目录（忽略已存在错误） */
                    cocoon_mkdir(upload_dir);

                    char file_path[4096];
                    r = snprintf(file_path, sizeof(file_path), "%s/%s", upload_dir, parts[i].filename);
                    if (r > 0 && r < (int)sizeof(file_path)) {
                        FILE *fp = fopen(file_path, "wb");
                        if (fp) {
                            fwrite(parts[i].data, 1, parts[i].data_len, fp);
                            fclose(fp);
                            files_saved++;

                            if (files_saved > 1) {
                                n += snprintf(response + n, sizeof(response) - n, ",");
                            }
                            n += snprintf(response + n, sizeof(response) - n,
                                "{\"field\":\"%s\",\"filename\":\"%s\",\"size\":%zu,\"path\":\"%s\"}",
                                parts[i].name ? parts[i].name : "",
                                parts[i].filename,
                                parts[i].data_len,
                                file_path);
                        }
                    }
                }
            }
        }

        n += snprintf(response + n, sizeof(response) - n, "]}");

        multipart_parts_free(parts, num_parts);

        char header[512];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: %s\r\n"
            "Server: Cocoon/1.0\r\n"
            "\r\n",
            n, req->keep_alive ? "keep-alive" : "close");

        send_all(fd, header, (size_t)header_len);
        send_all(fd, response, (size_t)n);

        return req->keep_alive;
    }

    n += snprintf(response + n, sizeof(response) - n,
        "{\"method\":\"%s\",\"path\":\"%s\",\"content_type\":\"%s\",\"body_length\":%zu",
        http_method_str(req->method), req->path, req->content_type, req->body_len);

    if (req->body_len > 0) {
        /* 对于 JSON 类型，尝试回显 body */
        if (strstr(req->content_type, "application/json") != NULL) {
            n += snprintf(response + n, sizeof(response) - n, ",\"body\": ");
            /* 直接拼接 JSON body（假设客户端发送的是合法 JSON） */
            size_t body_copy = req->body_len;
            if (body_copy > sizeof(response) - n - 64) {
                body_copy = sizeof(response) - n - 64;
            }
            memcpy(response + n, req->body, body_copy);
            n += (int)body_copy;
            n += snprintf(response + n, sizeof(response) - n, "}");
        } else if (strstr(req->content_type, "x-www-form-urlencoded") != NULL) {
            n += snprintf(response + n, sizeof(response) - n, ",\"body\":\"");
            size_t body_copy = req->body_len;
            if (body_copy > sizeof(response) - n - 64) {
                body_copy = sizeof(response) - n - 64;
            }
            memcpy(response + n, req->body, body_copy);
            n += (int)body_copy;
            n += snprintf(response + n, sizeof(response) - n, "\"}");
        } else {
            n += snprintf(response + n, sizeof(response) - n, "}");
        }
    } else {
        n += snprintf(response + n, sizeof(response) - n, "}");
    }

    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "Server: Cocoon/1.0\r\n"
        "\r\n",
        n, req->keep_alive ? "keep-alive" : "close");

    send_all(fd, header, (size_t)header_len);
    send_all(fd, response, (size_t)n);

    return req->keep_alive;
}

/**
 * vhost_match_root_dir - 根据 Host 头匹配虚拟主机根目录
 *
 * @param config 服务器配置
 * @param host   Host 头值
 * @return 匹配的 root_dir，未匹配则返回全局 root_dir
 */
static const char *vhost_match_root_dir(const cocoon_config_t *config, const char *host) {
    if (!host || !config) return config ? config->root_dir : NULL;
    for (size_t i = 0; i < config->num_vhosts; i++) {
        if (strcmp(config->vhosts[i].server_name, host) == 0) {
            return config->vhosts[i].root_dir;
        }
    }
    return config->root_dir;
}

/**
 * conn_get_host - 从请求中提取 Host 头
 *
 * @param req HTTP 请求
 * @return Host 头值，未找到返回 NULL
 */
static const char *conn_get_host(const http_request_t *req) {
    for (int i = 0; i < req->num_headers; i++) {
        if (strcasecmp(req->headers[i].name, "Host") == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

/**
 * handle_request - 处理单个 HTTP 请求
 *
 * 从缓冲区解析请求，判断是文件还是目录，调用对应的服务函数。
 * 新增：支持 POST 请求体读取和简单回显。
 * 新增：记录响应状态码到访问日志。
 *
 * @param conn 连接上下文
 * @param root_dir 静态资源根目录
 * @return true 保持连接，false 关闭连接
 */
static bool handle_request(connection_t *conn, const char *root_dir) {
    http_request_t req;
    int parsed = http_parse_request(conn->buf, conn->buf_len, &req);

    if (parsed < 0) {
        if (parsed == -1) {
            /* 数据不完整，等待更多数据 */
            return true;
        }
        /* 格式错误 */
        conn->response_status = 400;
        update_metrics(conn->response_status);
        static_send_error(conn->fd, 400, false);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        return false;
    }

    /* 消费已解析的数据 */
    if ((size_t)parsed < conn->buf_len) {
        memmove(conn->buf, conn->buf + parsed, conn->buf_len - (size_t)parsed);
    }
    conn->buf_len -= (size_t)parsed;

    /* 虚拟主机匹配：根据 Host 头选择 root_dir */
    const char *effective_root_dir = root_dir;
    if (conn->ctx) {
        const char *host = conn_get_host(&req);
        effective_root_dir = vhost_match_root_dir(&conn->ctx->config, host);
    }

    /* 执行中间件链 */
    if (cocoon_middleware_run(&req, conn->fd) != 0) {
        /* 某个中间件已短路请求，清理并返回 */
        if (conn->response_status > 0) update_metrics(conn->response_status);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        http_request_free(&req);
        return req.keep_alive;
    }

    /* 读取请求体（如果需要） */
    if (req.content_length > 0) {
        size_t need = (size_t)req.content_length;
        if (conn_read_body(conn, &req, need) != 0) {
            conn->response_status = 413;
            update_metrics(conn->response_status);
            static_send_error(conn->fd, 413, req.keep_alive); /* Payload Too Large */
            access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                             &req, conn->response_status, -1);
            http_request_free(&req);
            return req.keep_alive;
        }
    }

    /* Dashboard SSE 端点（必须在 /_status 之前检查） */
    if (dashboard_sse_handle_request(conn->fd, &req)) {
        conn->response_status = 200;
        update_metrics(conn->response_status);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        http_request_free(&req);
        return true; /* SSE 保持连接 */
    }

    /* Dashboard 页面端点 */
    if (dashboard_handle_request(conn->fd, &req)) {
        conn->response_status = 200;
        update_metrics(conn->response_status);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        http_request_free(&req);
        return req.keep_alive;
    }

    /* SSE 端点 */
    if (sse_handle_request(conn->fd, &req)) {
        conn->response_status = 200;
        update_metrics(conn->response_status);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        http_request_free(&req);
        return true; /* SSE 保持连接 */
    }

    /* SSE 端点 */
    if (sse_handle_request(conn->fd, &req)) {
        conn->response_status = 200;
        update_metrics(conn->response_status);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        http_request_free(&req);
        return true; /* SSE 保持连接 */
    }

    /* 处理 POST */
    if (req.method == HTTP_POST) {
        bool keep = handle_post_request(conn->fd, &req, effective_root_dir);
        conn->response_status = 200; /* POST 目前总是返回 200 */
        update_metrics(conn->response_status);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        http_request_free(&req);
        return keep;
    }

    /* 反向代理检查 */
    if (conn->ctx && conn->ctx->proxy_config.count > 0) {
        cocoon_proxy_rule_t *rule = proxy_match(&conn->ctx->proxy_config, req.path);
        if (rule) {
            bool keep = proxy_forward(conn->fd, &req, rule, &conn->client_addr);
            conn->response_status = 200; /* 代理响应状态由后端决定，这里记录一个通用值 */
            update_metrics(conn->response_status);
            access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                             &req, conn->response_status, -1);
            http_request_free(&req);
            return keep;
        }
    }

    /* FastCGI 检查 */
    if (conn->ctx && conn->ctx->fcgi_config.count > 0) {
        cocoon_fcgi_rule_t *fcgi_rule = fcgi_handler_match(&conn->ctx->fcgi_config, req.path);
        if (fcgi_rule) {
            bool keep = fcgi_handler_forward(conn->fd, &req, fcgi_rule);
            conn->response_status = 200; /* FastCGI 响应状态由后端决定 */
            update_metrics(conn->response_status);
            access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                             &req, conn->response_status, -1);
            http_request_free(&req);
            return keep;
        }
    }

    /* 只支持 GET 和 HEAD */
    if (req.method != HTTP_GET && req.method != HTTP_HEAD) {
        conn->response_status = 405;
        update_metrics(conn->response_status);
        static_send_error(conn->fd, 405, req.keep_alive);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        http_request_free(&req);
        return req.keep_alive;
    }

    /* ACME HTTP-01 挑战端点 */
    if (strncmp(req.path, "/.well-known/acme-challenge/", 28) == 0) {
        const char *token = req.path + 28;
        if (token[0] != '\0' && conn->ctx && conn->ctx->acme) {
            char *keyauth = NULL;
            if (acme_get_keyauth(conn->ctx->acme, token, &keyauth) == 0) {
                size_t len = strlen(keyauth);
                char header[512];
                int n = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: %s\r\n"
                    "Server: Cocoon/1.0\r\n"
                    "\r\n",
                    len, req.keep_alive ? "keep-alive" : "close");
                send_all(conn->fd, header, (size_t)n);
                send_all(conn->fd, keyauth, len);
                free(keyauth);
                conn->response_status = 200;
                update_metrics(conn->response_status);
                access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                                 &req, conn->response_status, (int)len);
                http_request_free(&req);
                return req.keep_alive;
            }
        }
        /* token 无效或 ACME 未配置 */
        conn->response_status = 404;
        update_metrics(conn->response_status);
        static_send_error(conn->fd, 404, req.keep_alive);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        http_request_free(&req);
        return req.keep_alive;
    }

    /* 健康检查端点 */
    if (strcmp(req.path, "/_health") == 0) {
        time_t now = time(NULL);
        time_t uptime = now - g_server_start_time;
        int active = atomic_load(&g_active_connections);

        char mw_names[16][32];
        int mw_count = cocoon_middleware_list(mw_names, 16);

        /* 构建 JSON */
        char body[2048];
        int n = snprintf(body, sizeof(body),
            "{\n"
            "  \"status\": \"ok\",\n"
            "  \"version\": \"Cocoon/1.0\",\n"
            "  \"uptime_seconds\": %ld,\n"
            "  \"connections\": {\n"
            "    \"active\": %d,\n"
            "    \"max\": %u\n"
            "  },\n"
            "  \"plugins\": {\n"
            "    \"count\": %zu,\n"
            "    \"list\": [\n",
            uptime, active,
            g_max_connections,
            cocoon_plugin_count());

        for (size_t i = 0; i < cocoon_plugin_count(); i++) {
            const char *path = cocoon_plugin_get_path(i);
            const char *ver = cocoon_plugin_get_version(i);
            n += snprintf(body + n, sizeof(body) - n,
                "      {\"path\":\"%s\",\"version\":\"%s\"}%s\n",
                path ? path : "",
                ver ? ver : "unknown",
                (i + 1 < cocoon_plugin_count()) ? "," : "");
        }

        n += snprintf(body + n, sizeof(body) - n,
            "    ]\n"
            "  },\n"
            "  \"middleware\": {\n"
            "    \"count\": %d,\n"
            "    \"names\": [\n",
            mw_count);

        for (int i = 0; i < mw_count; i++) {
            n += snprintf(body + n, sizeof(body) - n,
                "      \"%s\"%s\n",
                mw_names[i],
                (i + 1 < mw_count) ? "," : "");
        }

        n += snprintf(body + n, sizeof(body) - n,
            "    ]\n"
            "  }\n"
            "}");

        char header[512];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: %s\r\n"
            "Server: Cocoon/1.0\r\n"
            "\r\n",
            n, req.keep_alive ? "keep-alive" : "close");

        send_all(conn->fd, header, (size_t)header_len);
        send_all(conn->fd, body, (size_t)n);

        conn->response_status = 200;
        update_metrics(conn->response_status);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, n);
        http_request_free(&req);
        return req.keep_alive;
    }

    /* SSE 端点 */
    if (sse_handle_request(conn->fd, &req)) {
        conn->response_status = 200;
        update_metrics(conn->response_status);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        http_request_free(&req);
        return true; /* SSE 保持连接 */
    }

    /* Prometheus 指标端点 */
    if (strcmp(req.path, "/_metrics") == 0) {
        time_t now = time(NULL);
        time_t uptime = now - g_server_start_time;
        int active = atomic_load(&g_active_connections);
        unsigned int total = atomic_load(&g_total_requests);
        unsigned int r2xx = atomic_load(&g_response_2xx);
        unsigned int r3xx = atomic_load(&g_response_3xx);
        unsigned int r4xx = atomic_load(&g_response_4xx);
        unsigned int r5xx = atomic_load(&g_response_5xx);
        unsigned int r200 = atomic_load(&g_response_200);
        unsigned int r404 = atomic_load(&g_response_404);

        char body[4096];
        int n = snprintf(body, sizeof(body),
            "# HELP cocoon_uptime_seconds Server uptime in seconds\n"
            "# TYPE cocoon_uptime_seconds gauge\n"
            "cocoon_uptime_seconds %ld\n"
            "# HELP cocoon_connections_active Current active connections\n"
            "# TYPE cocoon_connections_active gauge\n"
            "cocoon_connections_active %d\n"
            "# HELP cocoon_connections_max Maximum allowed connections\n"
            "# TYPE cocoon_connections_max gauge\n"
            "cocoon_connections_max %u\n"
            "# HELP cocoon_requests_total Total HTTP requests served\n"
            "# TYPE cocoon_requests_total counter\n"
            "cocoon_requests_total %u\n"
            "# HELP cocoon_response_2xx_total Total 2xx responses\n"
            "# TYPE cocoon_response_2xx_total counter\n"
            "cocoon_response_2xx_total %u\n"
            "# HELP cocoon_response_3xx_total Total 3xx responses\n"
            "# TYPE cocoon_response_3xx_total counter\n"
            "cocoon_response_3xx_total %u\n"
            "# HELP cocoon_response_4xx_total Total 4xx responses\n"
            "# TYPE cocoon_response_4xx_total counter\n"
            "cocoon_response_4xx_total %u\n"
            "# HELP cocoon_response_5xx_total Total 5xx responses\n"
            "# TYPE cocoon_response_5xx_total counter\n"
            "cocoon_response_5xx_total %u\n"
            "# HELP cocoon_response_200_total Total 200 responses\n"
            "# TYPE cocoon_response_200_total counter\n"
            "cocoon_response_200_total %u\n"
            "# HELP cocoon_response_404_total Total 404 responses\n"
            "# TYPE cocoon_response_404_total counter\n"
            "cocoon_response_404_total %u\n",
            uptime, active, g_max_connections,
            total, r2xx, r3xx, r4xx, r5xx, r200, r404);

        char header[512];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; version=0.0.4\r\n"
            "Content-Length: %d\r\n"
            "Connection: %s\r\n"
            "Server: Cocoon/1.0\r\n"
            "\r\n",
            n, req.keep_alive ? "keep-alive" : "close");

        send_all(conn->fd, header, (size_t)header_len);
        send_all(conn->fd, body, (size_t)n);

        conn->response_status = 200;
        update_metrics(conn->response_status);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, n);
        http_request_free(&req);
        return req.keep_alive;
    }

    /* 安全路径拼接 */
    char real_path[4096];
    char root_normalized[4096];
    if (!cocoon_realpath(effective_root_dir, root_normalized, sizeof(root_normalized))) {
        strncpy(root_normalized, effective_root_dir, sizeof(root_normalized) - 1);
        root_normalized[sizeof(root_normalized) - 1] = '\0';
    }

    int n = snprintf(real_path, sizeof(real_path), "%s%s", root_normalized, req.path);
    if (n < 0 || (size_t)n >= sizeof(real_path)) {
        conn->response_status = 400;
        update_metrics(conn->response_status);
        static_send_error(conn->fd, 400, req.keep_alive);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        http_request_free(&req);
        return req.keep_alive;
    }

    /* 路径遍历检查 */
    if (strstr(req.path, "..") != NULL) {
        char resolved[4096];
        if (!cocoon_realpath(real_path, resolved, sizeof(resolved)) ||
            strncmp(resolved, root_normalized, strlen(root_normalized)) != 0) {
            conn->response_status = 403;
            update_metrics(conn->response_status);
            static_send_error(conn->fd, 403, req.keep_alive);
            access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                             &req, conn->response_status, -1);
            http_request_free(&req);
            return req.keep_alive;
        }
        snprintf(real_path, sizeof(real_path), "%s", resolved);
    }

    /* 判断文件类型 */
    cocoon_stat_t st;
    if (cocoon_file_stat(real_path, &st) != 0) {
        conn->response_status = 404;
        update_metrics(conn->response_status);
        static_send_error(conn->fd, 404, req.keep_alive);
        access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                         &req, conn->response_status, -1);
        http_request_free(&req);
        return req.keep_alive;
    }

    if (cocoon_stat_isdir(&st)) {
        /* 目录：尝试 index.html */
        char index_path[4096];
        if (snprintf(index_path, sizeof(index_path), "%s/index.html", real_path) >= (int)sizeof(index_path)) {
            conn->response_status = 400;
            update_metrics(conn->response_status);
            static_send_error(conn->fd, 400, req.keep_alive);
            access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                             &req, conn->response_status, -1);
            http_request_free(&req);
            return req.keep_alive;
        }
        cocoon_stat_t index_st;
        if (cocoon_file_stat(index_path, &index_st) == 0 && cocoon_stat_isreg(&index_st)) {
            /* 有 index.html，作为文件服务 */
            http_request_t index_req = req;
            if (snprintf(index_req.path, sizeof(index_req.path), "%s/index.html", req.path) >= (int)sizeof(index_req.path)) {
                conn->response_status = 400;
                update_metrics(conn->response_status);
                static_send_error(conn->fd, 400, req.keep_alive);
                access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                                 &req, conn->response_status, -1);
                http_request_free(&req);
                return req.keep_alive;
            }
            conn->response_status = 200;
            static_serve_file(conn->fd, &index_req, effective_root_dir, conn->gzip_enabled, conn->brotli_enabled, conn->ctx->cache);
        } else {
            /* 无 index.html，生成目录列表 */
            conn->response_status = 200;
            static_serve_directory(conn->fd, &req, effective_root_dir, real_path);
        }
    } else if (cocoon_stat_isreg(&st)) {
        /* 普通文件 */
        conn->response_status = 200;
        static_serve_file(conn->fd, &req, effective_root_dir, conn->gzip_enabled, conn->brotli_enabled, conn->ctx->cache);
    } else {
        conn->response_status = 403;
        static_send_error(conn->fd, 403, req.keep_alive);
    }

    update_metrics(conn->response_status);
    access_log_write((struct sockaddr *)&conn->client_addr, conn->addr_len,
                     &req, conn->response_status, -1);
    http_request_free(&req);
    return req.keep_alive;
}

/**
 * conn_timeout_handler - 连接空闲超时回调
 *
 * 定时器触发时关闭 socket，唤醒阻塞在 coco_read 的协程，
 * 并发起协程取消请求。
 *
 * @param arg 连接上下文指针
 */
static void conn_timeout_handler(void *arg) {
    connection_t *conn = (connection_t *)arg;
    if (!conn) return;

    conn->timer = NULL;  /* 定时器已触发，自动释放 */

    if (conn->fd != COCOON_INVALID_SOCKET) {
        log_debug("连接 fd=%llu 空闲超时，强制关闭", (unsigned long long)conn->fd);
        conn->closed = true;
        /* shutdown 唤醒阻塞在 coco_read 的协程 */
        cocoon_socket_shutdown(conn->fd);
    }

    if (conn->coro) {
        coco_cancel(conn->coro);
    }
}

/**
 * conn_reset_timer - 重置连接空闲定时器
 *
 * 每次收到数据时调用，取消旧定时器并创建新定时器。
 *
 * @param conn 连接上下文
 * @param timeout_ms 超时毫秒数
 */
static void conn_reset_timer(connection_t *conn, uint32_t timeout_ms) {
    if (conn->timer) {
        coco_timer_cancel(conn->timer);
    }
    conn->timer = coco_timer(timeout_ms, conn_timeout_handler, conn);
}

/**
 * conn_cancel_timer - 取消连接定时器
 *
 * @param conn 连接上下文
 */
static void conn_cancel_timer(connection_t *conn) {
    if (conn->timer) {
        coco_timer_cancel(conn->timer);
        conn->timer = NULL;
    }
}

/**
 * client_handler - 客户端连接协程入口
 *
 * 每个连接一个协程，循环读取请求并处理，直到连接关闭或超时。
 *
 * @param arg 连接上下文指针（connection_t*）
 */
/**
 * handle_http2 - 处理 HTTP/2 连接
 *
 * 使用 nghttp2 库处理 HTTP/2 帧，服务静态资源。
 *
 * @param conn 连接上下文
 */
static void handle_http2(connection_t *conn) {
    http2_session_t *h2 = http2_session_get(conn->fd);
    if (!h2) return;
    http2_session_set_context(h2, conn->root_dir, conn->gzip_enabled, conn->brotli_enabled);
    http2_session_set_server_config(h2, &conn->ctx->config);
    if (conn->ctx && conn->ctx->proxy_config.count > 0) {
        http2_session_set_proxy_config(h2, &conn->ctx->proxy_config, &conn->client_addr);
    }

    uint32_t timeout_ms = conn->timeout_ms > 0 ? conn->timeout_ms : CONN_TIMEOUT_MS;

    while (!conn->closed && http2_want_read(h2)) {
        /* 读取数据 */
        char buf[8192];
        ssize_t n;
        if (tls_has_connection(conn->fd)) {
            n = tls_read(conn->fd, buf, sizeof(buf));
        } else if (coco_sched_get_current() != NULL) {
            n = coco_read(conn->fd, buf, sizeof(buf));
        } else {
            n = read(conn->fd, buf, sizeof(buf));
        }

        if (n > 0) {
            conn_reset_timer(conn, timeout_ms);
            if (http2_recv(h2, (const uint8_t *)buf, (size_t)n) != 0) {
                log_debug("HTTP/2 接收错误 fd=%d", conn->fd);
                break;
            }
        } else if (n == 0) {
            break; /* 对端关闭 */
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            break;
        }

        /* 发送挂起的帧 */
        if (http2_want_write(h2)) {
            if (http2_send_pending(h2) != 0) {
                break;
            }
        }
    }

    http2_session_destroy(h2);
}

/* HTTP/2 h2c 连接前言（24字节） */
static const char H2C_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
#define H2C_PREFACE_LEN 24

/**
 * is_h2c_upgrade_request - 检查是否是 HTTP/1.1 Upgrade: h2c 请求
 *
 * @param req HTTP 请求
 * @return true 是 h2c 升级请求
 */
static bool is_h2c_upgrade_request(const http_request_t *req) {
    if (strcmp(req->version, "HTTP/1.1") != 0) return false;

    bool has_upgrade = false;
    bool has_connection_upgrade = false;

    for (int i = 0; i < req->num_headers; i++) {
        if (strcasecmp(req->headers[i].name, "upgrade") == 0) {
            if (strcasestr(req->headers[i].value, "h2c") != NULL) {
                has_upgrade = true;
            }
        } else if (strcasecmp(req->headers[i].name, "connection") == 0) {
            if (strcasestr(req->headers[i].value, "upgrade") != NULL) {
                has_connection_upgrade = true;
            }
        }
    }
    return has_upgrade && has_connection_upgrade;
}

/**
 * is_websocket_upgrade_request - 检查是否是 WebSocket Upgrade 请求
 *
 * 检查请求头是否包含 Upgrade: websocket、Connection: Upgrade 和 Sec-WebSocket-Key。
 *
 * @param req HTTP 请求
 * @return true 是 WebSocket 升级请求
 */
static bool is_websocket_upgrade_request(const http_request_t *req) {
    if (strcmp(req->version, "HTTP/1.1") != 0) return false;
    if (req->method != HTTP_GET) return false;

    bool has_upgrade = false;
    bool has_connection_upgrade = false;
    const char *key = NULL;

    for (int i = 0; i < req->num_headers; i++) {
        if (strcasecmp(req->headers[i].name, "upgrade") == 0) {
            if (strcasestr(req->headers[i].value, "websocket") != NULL) {
                has_upgrade = true;
            }
        } else if (strcasecmp(req->headers[i].name, "connection") == 0) {
            if (strcasestr(req->headers[i].value, "upgrade") != NULL) {
                has_connection_upgrade = true;
            }
        } else if (strcasecmp(req->headers[i].name, "sec-websocket-key") == 0) {
            key = req->headers[i].value;
        }
    }
    return has_upgrade && has_connection_upgrade && key != NULL;
}

/**
 * send_h2c_upgrade_response - 发送 101 Switching Protocols
 *
 * @param conn 连接上下文
 * @return true 发送成功
 */
static bool send_h2c_upgrade_response(connection_t *conn) {
    const char *resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: h2c\r\n"
        "\r\n";
    return send_all(conn->fd, resp, strlen(resp)) == 0;
}

static void client_handler(void *arg) {
    connection_t *conn = (connection_t *)arg;
    if (!conn) return;

    conn->coro = coco_self();
    log_debug("client_handler 启动 fd=%d", conn->fd);

    log_debug("fd=%d 检查 HTTP/2...", conn->fd);
    /* 检查是否是 HTTP/2 连接（TLS ALPN 协商） */
    if (http2_session_is_http2(conn->fd)) {
        log_debug("fd=%d 是 HTTP/2 连接", conn->fd);
        handle_http2(conn);
        conn_cancel_timer(conn);
        close_connection(conn);
        free(conn);
        return;
    }
    log_debug("fd=%d 不是 HTTP/2", conn->fd);

    /* 启动空闲定时器 */
    log_debug("fd=%d 创建定时器...", conn->fd);
    uint32_t timeout_ms = conn->timeout_ms > 0 ? conn->timeout_ms : CONN_TIMEOUT_MS;
    conn->timer = coco_timer(timeout_ms, conn_timeout_handler, conn);
    log_debug("fd=%d 定时器已创建 timer=%p", conn->fd, (void*)conn->timer);

    while (!conn->closed) {
        /* 读取数据 */
        ssize_t n = conn_read(conn);
        log_debug("fd=%d conn_read 返回 %zd", conn->fd, n);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                log_debug("fd=%d EAGAIN，继续等待", conn->fd);
                continue;
            }
            /* coco_read 返回负值错误码（如 COCO_ERROR_CANCELLED）或系统错误 */
            log_debug("fd=%d conn_read 错误: %d %s", conn->fd, (int)n, strerror(errno));
            break;
        }
        if (n == 0) {
            log_debug("fd=%d 对端关闭", conn->fd);
            break; /* 对端关闭 */
        }

        /* 有数据读取，重置定时器 */
        conn_reset_timer(conn, timeout_ms);

        /* 检查 h2c 直接连接前言 */
        if (conn->buf_len >= H2C_PREFACE_LEN &&
            memcmp(conn->buf, H2C_PREFACE, H2C_PREFACE_LEN) == 0) {
            log_debug("fd=%d 检测到 h2c 直接连接前言", conn->fd);
            http2_session_t *h2 = http2_session_create(conn->fd, false);
            if (h2) {
                http2_session_set_context(h2, conn->root_dir,
                                          conn->gzip_enabled,
                                          conn->brotli_enabled);
                http2_session_set_server_config(h2, &conn->ctx->config);
                if (http2_send_pending(h2) == 0) {
                    if (http2_recv(h2, (const uint8_t *)conn->buf, conn->buf_len) == 0) {
                        conn->buf_len = 0;
                        handle_http2(conn);
                        break; /* handle_http2 内部已销毁会话 */
                    }
                }
                http2_session_destroy(h2);
            }
            break;
        }

        /* 检查 HTTP/1.1 Upgrade: h2c 请求 */
        http_request_t req;
        int parsed = http_parse_request(conn->buf, conn->buf_len, &req);
        if (parsed > 0 && is_h2c_upgrade_request(&req)) {
            /* 消费已解析的 HTTP/1.1 请求数据 */
            if ((size_t)parsed < conn->buf_len) {
                memmove(conn->buf, conn->buf + parsed, conn->buf_len - (size_t)parsed);
            }
            conn->buf_len -= (size_t)parsed;

            if (send_h2c_upgrade_response(conn)) {
                http2_session_t *h2 = http2_session_create(conn->fd, false);
                if (h2) {
                    http2_session_set_context(h2, conn->root_dir,
                                              conn->gzip_enabled,
                                              conn->brotli_enabled);
                http2_session_set_server_config(h2, &conn->ctx->config);
                    if (http2_send_pending(h2) == 0) {
                        if (http2_session_upgrade(h2, &req) == 0) {
                            if (http2_send_pending(h2) == 0) {
                                /* 消费缓冲区中的剩余数据 */
                                if (conn->buf_len > 0) {
                                    http2_recv(h2, (const uint8_t *)conn->buf, conn->buf_len);
                                    conn->buf_len = 0;
                                }
                                handle_http2(conn);
                                break; /* handle_http2 内部已销毁会话 */
                            }
                        }
                        http2_session_destroy(h2);
                    }
                }
            }
            http_request_free(&req);
            break;
        }

        /* 检查 WebSocket Upgrade 请求 */
        if (parsed > 0 && is_websocket_upgrade_request(&req)) {
            const char *key = NULL;
            for (int i = 0; i < req.num_headers; i++) {
                if (strcasecmp(req.headers[i].name, "sec-websocket-key") == 0) {
                    key = req.headers[i].value;
                    break;
                }
            }
            if (key) {
                conn_cancel_timer(conn);
                if (ws_handshake(conn->fd, key) == 0) {
                    http_request_free(&req);
                    /* 消费已解析的请求数据 */
                    if ((size_t)parsed < conn->buf_len) {
                        memmove(conn->buf, conn->buf + parsed, conn->buf_len - (size_t)parsed);
                    }
                    conn->buf_len -= (size_t)parsed;
                    ws_handle_connection(conn->fd, conn->timeout_ms, req.path);
                    break;
                }
            }
        }

        http_request_free(&req);

        /* 尝试处理请求 */
        bool keep = handle_request(conn, conn->root_dir);
        log_debug("fd=%d handle_request 返回 %d", conn->fd, keep);
        if (!keep) {
            break;
        }
    }

    conn_cancel_timer(conn);
    close_connection(conn);
    log_debug("client_handler 结束 fd=%d", conn->fd);
    free(conn);
}

/**
 * accept_loop - 主 accept 循环
 *
 * 在单线程模式下直接运行，在多线程模式下作为协程运行。
 * 循环 accept 新连接，为每个连接创建处理协程。
 *
 * @param arg 服务器上下文指针
 */
static void accept_loop(void *arg) {
    log_debug("=== accept_loop 启动 ===");
    server_context_t *ctx = (server_context_t *)arg;
    if (!ctx) return;

    uint32_t num_workers = ctx->config.num_workers;
    if (num_workers == 0) {
        num_workers = cocoon_cpu_count();
        if (num_workers == 0) num_workers = 4;
    }

    log_info("服务器启动于端口 %d", ctx->config.port);
    if (ctx->config.threaded) {
        log_info("多线程模式: %d 个工作线程", num_workers);
    } else {
        log_info("单线程模式");
    }
    log_info("静态资源根目录: %s", ctx->config.root_dir);

    if (ctx->config.max_connections > 0) {
        log_info("最大并发连接数: %u", ctx->config.max_connections);
    }
    log_info("连接空闲超时: %u ms", ctx->config.timeout_ms > 0 ? ctx->config.timeout_ms : CONN_TIMEOUT_MS);

    while (ctx->running) {
        if (ctx->listen_fd < 0) break;

        if (ctx->reload_requested) {
            ctx->reload_requested = 0;
            server_reload_config(ctx);
        }

        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        cocoon_socket_t client_fd = accept(ctx->listen_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == COCOON_INVALID_SOCKET) {
            int err = cocoon_get_last_error();
            if (err == EAGAIN || err == EINTR) {
                continue;
            }
            log_error("accept 失败: %s", cocoon_strerror(err));
            break;
        }

        /* accept 返回后再次检查热重载请求，确保新连接使用最新配置 */
        if (ctx->reload_requested) {
            ctx->reload_requested = 0;
            server_reload_config(ctx);
        }

        /* 检查最大连接数限制 */
        if (ctx->config.max_connections > 0) {
            int current = atomic_load(&g_active_connections);
            if (current >= (int)ctx->config.max_connections) {
                log_warn("连接数已达上限 (%d/%u)，拒绝新连接 fd=%d",
                         current, ctx->config.max_connections, client_fd);
                /* 发送 503 后关闭 */
                const char *resp = "HTTP/1.1 503 Service Unavailable\r\n"
                                     "Content-Length: 0\r\n"
                                     "Connection: close\r\n\r\n";
                if (tls_has_context()) {
                    if (tls_accept(client_fd) == 0) {
                        tls_write(client_fd, resp, strlen(resp));
                        tls_close(client_fd);
                    }
                    cocoon_socket_close(client_fd);
                } else {
                    cocoon_socket_send(client_fd, resp, strlen(resp));
                    cocoon_socket_close(client_fd);
                }
                continue;
            }
        }

        /* 设置 TCP_NODELAY 减少延迟 */
        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt));

        /* 多线程模式下设置 client_fd 非阻塞（用于 coco_read/coco_write） */
        if (ctx->config.threaded) {
            set_nonblocking(client_fd);
        }

        /* TLS 握手（如果启用） */
        if (tls_has_context()) {
            if (tls_accept(client_fd) != 0) {
                log_warn("TLS 握手失败 fd=%d，关闭连接", client_fd);
                cocoon_socket_close(client_fd);
                continue;
            }
            /* 检查 ALPN 协商结果，创建 HTTP/2 会话 */
            if (tls_negotiated_http2(client_fd)) {
                log_debug("fd=%d ALPN 协商为 HTTP/2", client_fd);
                if (http2_on_connection_accepted(client_fd, true) != 0) {
                    log_warn("HTTP/2 会话创建失败 fd=%d", client_fd);
                    cocoon_socket_close(client_fd);
                    continue;
                }
                http2_session_t *h2 = http2_session_get(client_fd);
                if (h2) {
                    http2_session_set_context(h2, ctx->config.root_dir,
                                              ctx->config.gzip_enabled,
                                              ctx->config.brotli_enabled);
                    http2_session_set_server_config(h2, &ctx->config);
                }
            }
        }

        /* 创建连接上下文 */
        connection_t *conn = (connection_t *)calloc(1, sizeof(connection_t));
        if (!conn) {
            http2_session_t *h2 = http2_session_get(client_fd);
            if (h2) http2_session_destroy(h2);
            cocoon_socket_close(client_fd);
            continue;
        }
        conn->fd = client_fd;
        conn->keep_alive = true;
        conn->closed = false;
        conn->root_dir = ctx->config.root_dir;
        conn->timeout_ms = ctx->config.timeout_ms > 0 ? ctx->config.timeout_ms : CONN_TIMEOUT_MS;
        conn->gzip_enabled = ctx->config.gzip_enabled;
        conn->brotli_enabled = ctx->config.brotli_enabled;
        memcpy(&conn->client_addr, &client_addr, sizeof(client_addr));
        conn->addr_len = addr_len;
        conn->response_status = 0;
        conn->ctx = ctx;
        conn->throttle = NULL;

        /* 初始化 per-connection 限速 */
        if (ctx->config.throttle_conn_rate > 0) {
            conn->throttle = (cocoon_throttle_t *)malloc(sizeof(cocoon_throttle_t));
            if (conn->throttle) {
                throttle_init(conn->throttle, ctx->config.throttle_conn_rate, 0);
                throttle_set_fd(conn->fd, conn->throttle);
                log_debug("fd=%d 已启用 per-connection 限速: %u bytes/sec", conn->fd, ctx->config.throttle_conn_rate);
            }
        }

        atomic_fetch_add(&g_active_connections, 1);
        log_debug("新连接 fd=%d，当前活跃连接: %d", client_fd,
                  atomic_load(&g_active_connections));

        if (ctx->config.threaded) {
            /* 多线程模式：使用 coco_go_with_opts 创建工作协程
             * stack_size = 0 使用默认栈（共享热栈，128KB，自动复用）
             * p_id = client_fd % num_workers 按连接哈希绑定到固定 P，
             * 提升 CPU 缓存局部性，减少跨核迁移
             */
            uint32_t worker_idx = client_fd % num_workers;
            coco_go_opts_t opts = {
                .stack_size = 0,   /* 默认：使用共享热栈 */
                .context = NULL,
                .priority = -1,
                .p_id = (int)worker_idx
            };
            coco_coro_t *coro = coco_go_with_opts(client_handler, conn, &opts);
            if (!coro) {
                log_error("coco_go_with_opts(client_handler) 返回 NULL，无法创建协程 fd=%d", client_fd);
                http2_session_t *h2 = http2_session_get(client_fd);
                if (h2) http2_session_destroy(h2);
                cocoon_socket_close(client_fd);
                free(conn);
                atomic_fetch_sub(&g_active_connections, 1);
                continue;
            }
            log_debug("client_handler 协程已创建 coro=%p fd=%d", (void*)coro, client_fd);
        } else {
            /* 单线程模式：直接调用处理函数（阻塞） */
            client_handler(conn);
        }
    }
}

/**
 * server_create - 创建服务器上下文
 *
 * 初始化监听 socket、配置副本。
 *
 * @param config 配置指针
 * @return 服务器上下文，失败返回 NULL
 */
server_context_t *server_create(const cocoon_config_t *config, const char *config_file_path) {
    if (!config || !config->root_dir) return NULL;

    server_context_t *ctx = (server_context_t *)calloc(1, sizeof(server_context_t));
    if (!ctx) return NULL;

    /* 复制配置 */
    ctx->config = *config;
    ctx->config.root_dir = strdup(config->root_dir);
    ctx->running = 1;

    if (config_file_path) {
        ctx->config_file_path = strdup(config_file_path);
    }

    g_max_connections = config->max_connections;

    /* 初始化全局限速 */
    if (config->throttle_global_rate > 0) {
        ctx->global_throttle = (cocoon_throttle_t *)malloc(sizeof(cocoon_throttle_t));
        if (ctx->global_throttle) {
            throttle_init(ctx->global_throttle, config->throttle_global_rate, 0);
            log_info("全局限速已启用: %u bytes/sec", config->throttle_global_rate);
        }
    }

    /* 创建监听 socket */
    ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_fd == COCOON_INVALID_SOCKET) {
        free(ctx);
        return NULL;
    }

    /* 允许端口复用 */
    int opt = 1;
    setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 绑定地址 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config->port);

    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        cocoon_socket_close(ctx->listen_fd);
        free(ctx);
        return NULL;
    }

    /* 开始监听 */
    if (listen(ctx->listen_fd, 128) < 0) {
        cocoon_socket_close(ctx->listen_fd);
        free(ctx);
        return NULL;
    }

    /* 初始化 TLS 上下文（如果配置了证书） */
    if (ctx->config.tls_cert && ctx->config.tls_key) {
        if (tls_create_context(ctx->config.tls_cert, ctx->config.tls_key) != 0) {
            log_error("TLS 上下文初始化失败");
            close(ctx->listen_fd);
            free(ctx);
            return NULL;
        }
    }

    /* 初始化内置中间件（使用 ctx->mw_config 避免栈变量悬空） */
    ctx->mw_config.cors_enabled = ctx->config.cors_enabled;
    ctx->mw_config.auth_user    = ctx->config.auth_user;
    ctx->mw_config.auth_pass    = ctx->config.auth_pass;
    ctx->mw_config.rate_limit   = ctx->config.rate_limit;
    cocoon_middleware_init_builtin(&ctx->mw_config);

    /* 初始化反向代理配置 */
    proxy_init(&ctx->proxy_config);
    for (size_t i = 0; i < ctx->config.num_proxies; i++) {
        proxy_add_rule(&ctx->proxy_config, ctx->config.proxies[i].prefix, ctx->config.proxies[i].target, ctx->config.proxies[i].pool_size, ctx->config.proxies[i].weight, &ctx->config.proxies[i].healthcheck);
    }

    /* 加载插件 */
    for (size_t i = 0; i < ctx->config.num_plugins; i++) {
        if (cocoon_plugin_load(ctx->config.plugins[i]) != 0) {
            log_error("插件加载失败: %s", ctx->config.plugins[i]);
        }
    }

    /* 初始化 FastCGI 处理器 */
    if (!fcgi_handler_init(&ctx->fcgi_config, &ctx->config)) {
        log_warn("FastCGI 初始化失败，FastCGI 路由不可用");
    }

    /* 初始化内存缓存 */
    if (ctx->config.cache_enabled) {
        ctx->cache = cache_create(ctx->config.cache_max_size, ctx->config.cache_ttl_seconds, ctx->config.cache_max_entry_size);
        if (ctx->cache) {
            log_info("内存缓存已启用: 最大 %zu 字节, TTL %u 秒, 单条上限 %zu 字节",
                     ctx->config.cache_max_size, ctx->config.cache_ttl_seconds, ctx->config.cache_max_entry_size);
        } else {
            log_warn("内存缓存初始化失败，已禁用");
        }
    }

    /* 初始化 ACME 自动证书 */
    if (ctx->config.acme_enabled) {
        const char *directory_url = ctx->config.acme_directory_url[0] ?
            ctx->config.acme_directory_url :
            "https://acme-v02.api.letsencrypt.org/directory";

        /* 检查现有证书是否有效 */
        int days_left = acme_cert_days_until_expiry(ctx->config.acme_cert_path);
        if (days_left >= (int)ctx->config.acme_renew_days) {
            log_info("ACME: 现有证书仍有效（剩余 %d 天），跳过签发", days_left);
        } else {
            if (days_left < 0) {
                log_info("ACME: 证书不存在或已过期，开始签发新证书");
            } else {
                log_info("ACME: 证书即将过期（剩余 %d 天，阈值 %u 天），开始续期", days_left, ctx->config.acme_renew_days);
            }

            ctx->acme = acme_create(directory_url, NULL);
            if (ctx->acme) {
                char *cert_pem = NULL;
                char *key_pem = NULL;

                const char *domains[8];
                for (size_t i = 0; i < ctx->config.acme_num_domains; i++) {
                    domains[i] = ctx->config.acme_domains[i];
                }

                if (acme_issue_certificate(ctx->acme, domains, ctx->config.acme_num_domains,
                                           ctx->config.acme_email, &cert_pem, &key_pem) == 0) {
                    if (acme_save_certificate(cert_pem, key_pem,
                                              ctx->config.acme_cert_path,
                                              ctx->config.acme_key_path) == 0) {
                        log_info("ACME: 证书签发并保存成功");
                    } else {
                        log_error("ACME: 证书保存失败");
                    }
                    free(cert_pem);
                    free(key_pem);
                } else {
                    log_error("ACME: 证书签发失败");
                }
                /* ACME 上下文继续保留，用于挑战响应和后续续期 */
            } else {
                log_error("ACME: 客户端初始化失败");
            }
        }
    }

    /* 启动主动健康检查 */
    healthcheck_manager_init(&ctx->hc_manager);
    healthcheck_start(&ctx->hc_manager, &ctx->proxy_config, ctx->config.timeout_ms);

    return ctx;
}

/**
 * acme_renewal_coroutine - ACME 证书自动续期后台协程
 *
 * 每 24 小时检查一次证书有效期，到期前自动续期。
 *
 * @param arg 服务器上下文指针
 */
static void acme_renewal_coroutine(void *arg) {
    server_context_t *ctx = (server_context_t *)arg;
    if (!ctx || !ctx->config.acme_enabled) return;

    /* 如果 ACME 上下文未创建（证书已存在且有效），创建一个用于续期 */
    if (!ctx->acme) {
        const char *directory_url = ctx->config.acme_directory_url[0] ?
            ctx->config.acme_directory_url :
            "https://acme-v02.api.letsencrypt.org/directory";
        ctx->acme = acme_create(directory_url, NULL);
        if (!ctx->acme) {
            log_error("ACME 续期: 客户端初始化失败");
            return;
        }
    }

    const uint32_t check_interval_ms = 24 * 60 * 60 * 1000; /* 24 小时 */

    while (ctx->running) {
        coco_sleep(check_interval_ms);
        if (!ctx->running) break;

        int days_left = acme_cert_days_until_expiry(ctx->config.acme_cert_path);
        if (days_left < 0 || days_left < (int)ctx->config.acme_renew_days) {
            if (days_left < 0) {
                log_info("ACME 续期: 证书已过期或不存在，开始重新签发");
            } else {
                log_info("ACME 续期: 证书即将过期（剩余 %d 天），开始续期", days_left);
            }

            char *cert_pem = NULL;
            char *key_pem = NULL;
            const char *domains[8];
            for (size_t i = 0; i < ctx->config.acme_num_domains; i++) {
                domains[i] = ctx->config.acme_domains[i];
            }

            if (acme_issue_certificate(ctx->acme, domains, ctx->config.acme_num_domains,
                                       ctx->config.acme_email, &cert_pem, &key_pem) == 0) {
                if (acme_save_certificate(cert_pem, key_pem,
                                          ctx->config.acme_cert_path,
                                          ctx->config.acme_key_path) == 0) {
                    log_info("ACME 续期: 证书续期并保存成功");
                } else {
                    log_error("ACME 续期: 证书保存失败");
                }
                free(cert_pem);
                free(key_pem);
            } else {
                log_error("ACME 续期: 证书签发失败");
            }
        } else {
            log_debug("ACME 续期: 证书仍有效（剩余 %d 天），跳过", days_left);
        }
    }
}

/**
 * server_start - 启动服务器（阻塞）
 *
 * 根据配置选择单线程或多线程模式运行。
 *
 * @param ctx 服务器上下文
 * @return COCOON_OK 成功，负值错误码
 */
int server_start(server_context_t *ctx) {
    if (!ctx) return COCOON_ERROR;

    g_server_start_time = time(NULL);

    if (ctx->config.threaded) {
        /* 多线程协程模式 */
        uint32_t num_workers = ctx->config.num_workers;
        if (num_workers == 0) {
            num_workers = cocoon_cpu_count();
            if (num_workers == 0) num_workers = 4;
        }

        /* 启动全局调度器 */
        int ret = coco_global_sched_start(num_workers);
        if (ret != COCO_OK) {
            log_error("启动多线程调度器失败: %d", ret);
            return COCOON_ERROR;
        }

        /* 多线程模式下 listen_fd 需要非阻塞（用于 accept + poll 等待） */
        set_nonblocking(ctx->listen_fd);

        /* 启动 ACME 自动续期后台协程 */
        if (ctx->config.acme_enabled) {
            coco_go(acme_renewal_coroutine, ctx);
        }

        /* 在主线程中运行 accept_loop */
        accept_loop(ctx);

        /* 等待所有协程完成 */
        coco_global_sched_wait();
        coco_global_sched_stop();
    } else {
        /* 单线程模式：listen_fd 保持阻塞 */
        /* 启动 ACME 自动续期后台协程 */
        if (ctx->config.acme_enabled) {
            coco_go(acme_renewal_coroutine, ctx);
        }
        accept_loop(ctx);
    }

    return COCOON_OK;
}

/**
 * server_stop - 请求服务器停止
 *
 * 设置停止标志，accept 循环将在下一次迭代时退出。
 *
 * @param ctx 服务器上下文
 */
void server_stop(server_context_t *ctx) {
    if (ctx) {
        ctx->running = 0;
        if (ctx->listen_fd >= 0) {
            close(ctx->listen_fd);
            ctx->listen_fd = -1;
        }
    }
}

/**
 * server_destroy - 销毁服务器上下文
 *
 * 关闭监听 socket，释放配置内存。
 *
 * @param ctx 服务器上下文
 */
void server_request_reload(server_context_t *ctx) {
    if (ctx) {
        ctx->reload_requested = 1;
    }
}

void server_reload_config(server_context_t *ctx) {
    if (!ctx || !ctx->config_file_path) {
        log_warn("配置热重载：未指定配置文件路径");
        return;
    }

    log_info("配置热重载开始：%s", ctx->config_file_path);

    cocoon_config_t new_config = {0};
    new_config.port = 8080;
    new_config.log_level = LOG_LEVEL_INFO;
    new_config.gzip_enabled = true;
    new_config.brotli_enabled = true;

    if (!config_load_from_file(ctx->config_file_path, &new_config)) {
        log_error("配置热重载失败：无法加载配置文件");
        return;
    }

    /* 校验新配置 */
    char err_buf[256] = {0};
    if (!config_validate(&new_config, err_buf, sizeof(err_buf))) {
        log_error("配置热重载被拒绝：新配置校验失败 — %s", err_buf);
        /* 清理 new_config 分配的内存 */
        if (new_config.root_dir) free((void*)new_config.root_dir);
        if (new_config.tls_cert) free((void*)new_config.tls_cert);
        if (new_config.tls_key) free((void*)new_config.tls_key);
        if (new_config.access_log_path) free((void*)new_config.access_log_path);
        if (new_config.auth_user) free((void*)new_config.auth_user);
        if (new_config.auth_pass) free((void*)new_config.auth_pass);
        for (size_t i = 0; i < new_config.num_plugins; i++) {
            free((void*)new_config.plugins[i]);
        }
        return;
    }

    log_info("配置热重载：新配置校验通过");

    /* 检查不可热重载项 */
    if (new_config.port != ctx->config.port) {
        log_warn("端口变化（%d -> %d）需要重启才能生效", ctx->config.port, new_config.port);
    }
    if (new_config.threaded != ctx->config.threaded) {
        log_warn("线程模式变化需要重启才能生效");
    }
    if (new_config.num_workers != ctx->config.num_workers) {
        log_warn("工作线程数变化（%u -> %u）需要重启才能生效", ctx->config.num_workers, new_config.num_workers);
    }
    if ((new_config.tls_cert && !ctx->config.tls_cert) ||
        (!new_config.tls_cert && ctx->config.tls_cert) ||
        (new_config.tls_cert && ctx->config.tls_cert && strcmp(new_config.tls_cert, ctx->config.tls_cert) != 0)) {
        log_warn("TLS 证书变化需要重启才能生效");
    }

    /* 热重载：root_dir（不释放旧指针，避免竞态） */
    if (new_config.root_dir && (!ctx->config.root_dir || strcmp(new_config.root_dir, ctx->config.root_dir) != 0)) {
        ctx->config.root_dir = strdup(new_config.root_dir);
        log_info("根目录已更新：%s", ctx->config.root_dir);
    }

    /* 热重载：log_level */
    if (new_config.log_level != ctx->config.log_level) {
        ctx->config.log_level = new_config.log_level;
        log_set_level(new_config.log_level);
        log_info("日志级别已更新：%d", new_config.log_level);
    }

    /* 热重载：access_log */
    if (new_config.access_log_path) {
        if (!ctx->config.access_log_path || strcmp(new_config.access_log_path, ctx->config.access_log_path) != 0) {
            access_log_close();
            access_log_init(new_config.access_log_path);
            ctx->config.access_log_path = strdup(new_config.access_log_path);
            log_info("访问日志路径已更新：%s", ctx->config.access_log_path);
        }
    } else if (ctx->config.access_log_path) {
        access_log_close();
        ctx->config.access_log_path = NULL;
        log_info("访问日志已关闭");
    }

    /* 热重载：压缩 */
    if (new_config.gzip_enabled != ctx->config.gzip_enabled) {
        ctx->config.gzip_enabled = new_config.gzip_enabled;
        log_info("gzip 压缩已%s", ctx->config.gzip_enabled ? "启用" : "禁用");
    }
    if (new_config.brotli_enabled != ctx->config.brotli_enabled) {
        ctx->config.brotli_enabled = new_config.brotli_enabled;
        log_info("brotli 压缩已%s", ctx->config.brotli_enabled ? "启用" : "禁用");
    }

    /* 热重载：超时和连接限制 */
    if (new_config.timeout_ms != ctx->config.timeout_ms) {
        ctx->config.timeout_ms = new_config.timeout_ms;
        log_info("连接超时已更新：%u ms", ctx->config.timeout_ms);
    }
    if (new_config.max_connections != ctx->config.max_connections) {
        ctx->config.max_connections = new_config.max_connections;
        g_max_connections = new_config.max_connections;
        log_info("最大连接数已更新：%u", ctx->config.max_connections);
    }

    /* 热重载：vhosts */
    if (new_config.num_vhosts > 0 || ctx->config.num_vhosts > 0) {
        if (new_config.num_vhosts != ctx->config.num_vhosts ||
            memcmp(new_config.vhosts, ctx->config.vhosts, sizeof(new_config.vhosts)) != 0) {
            memcpy(ctx->config.vhosts, new_config.vhosts, sizeof(new_config.vhosts));
            ctx->config.num_vhosts = new_config.num_vhosts;
            log_info("虚拟主机已更新：%zu 个", ctx->config.num_vhosts);
        }
    }

    /* 热重载：proxies */
    if (new_config.num_proxies > 0 || ctx->config.num_proxies > 0) {
        proxy_config_destroy(&ctx->proxy_config);
        proxy_init(&ctx->proxy_config);
        for (size_t i = 0; i < new_config.num_proxies; i++) {
            proxy_add_rule(&ctx->proxy_config, new_config.proxies[i].prefix, new_config.proxies[i].target, new_config.proxies[i].pool_size, new_config.proxies[i].weight, &new_config.proxies[i].healthcheck);
        }
        ctx->config.num_proxies = new_config.num_proxies;
        memcpy(ctx->config.proxies, new_config.proxies, sizeof(new_config.proxies));
        log_info("代理规则已更新：%zu 条", ctx->config.num_proxies);
    }

    /* 热重载：healthcheck */
    healthcheck_stop(&ctx->hc_manager);
    healthcheck_manager_init(&ctx->hc_manager);
    healthcheck_start(&ctx->hc_manager, &ctx->proxy_config, ctx->config.timeout_ms);
    log_info("健康检查已重新配置");

    log_info("配置热重载完成");

    /* 清理 new_config 分配的内存 */
    if (new_config.root_dir) free((void*)new_config.root_dir);
    if (new_config.tls_cert) free((void*)new_config.tls_cert);
    if (new_config.tls_key) free((void*)new_config.tls_key);
    if (new_config.access_log_path) free((void*)new_config.access_log_path);
    if (new_config.auth_user) free((void*)new_config.auth_user);
    if (new_config.auth_pass) free((void*)new_config.auth_pass);
    for (size_t i = 0; i < new_config.num_plugins; i++) {
        free((void*)new_config.plugins[i]);
    }
}

void server_destroy(server_context_t *ctx) {
    if (!ctx) return;

    /* 停止主动健康检查 */
    healthcheck_stop(&ctx->hc_manager);

    /* 关闭反向代理连接池 */
    proxy_config_destroy(&ctx->proxy_config);

    /* 关闭 FastCGI 连接池 */
    fcgi_handler_destroy(&ctx->fcgi_config);

    /* 销毁内存缓存 */
    if (ctx->cache) {
        cache_destroy(ctx->cache);
        ctx->cache = NULL;
    }

    /* 销毁 ACME 上下文 */
    if (ctx->acme) {
        acme_destroy(ctx->acme);
        ctx->acme = NULL;
    }

    /* 卸载插件 */
    cocoon_plugin_unload_all();

    cocoon_middleware_cleanup();

    tls_destroy_context();

    if (ctx->listen_fd != COCOON_INVALID_SOCKET) {
        cocoon_socket_close(ctx->listen_fd);
        ctx->listen_fd = COCOON_INVALID_SOCKET;
    }

    if (ctx->config_file_path) {
        free((void*)ctx->config_file_path);
        ctx->config_file_path = NULL;
    }

    if (ctx->config.root_dir) {
        free((void *)ctx->config.root_dir);
        ctx->config.root_dir = NULL;
    }

    /* 销毁全局限速器 */
    if (ctx->global_throttle) {
        free(ctx->global_throttle);
        ctx->global_throttle = NULL;
    }

    free(ctx);
}

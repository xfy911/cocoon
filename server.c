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
} connection_t;
struct server_context {
    cocoon_socket_t     listen_fd;    /**< 监听 socket */
    cocoon_config_t     config;       /**< 配置副本 */
    cocoon_middleware_config_t mw_config; /**< 中间件配置（持久化，避免栈变量悬空） */
    cocoon_proxy_config_t proxy_config; /**< 反向代理配置 */
    volatile int        running;      /**< 运行标志 */
    coco_sched_t       *sched;        /**< 协程调度器 */
};

/* 全局活跃连接计数器（线程安全） */
static atomic_int g_active_connections = 0;
/* 服务器启动时间 */
static time_t g_server_start_time = 0;
/* 最大连接数（供健康检查端点使用） */
static uint32_t g_max_connections = 0;
/* Prometheus 指标计数器 */
static atomic_uint g_total_requests = 0;
static atomic_uint g_response_2xx = 0;
static atomic_uint g_response_3xx = 0;
static atomic_uint g_response_4xx = 0;
static atomic_uint g_response_5xx = 0;
static atomic_uint g_response_200 = 0;
static atomic_uint g_response_404 = 0;

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

    /* 处理 POST */
    if (req.method == HTTP_POST) {
        bool keep = handle_post_request(conn->fd, &req, conn->root_dir);
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
    if (!cocoon_realpath(root_dir, root_normalized, sizeof(root_normalized))) {
        strncpy(root_normalized, root_dir, sizeof(root_normalized) - 1);
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
            static_serve_file(conn->fd, &index_req, root_dir, conn->gzip_enabled, conn->brotli_enabled);
        } else {
            /* 无 index.html，生成目录列表 */
            conn->response_status = 200;
            static_serve_directory(conn->fd, &req, root_dir, real_path);
        }
    } else if (cocoon_stat_isreg(&st)) {
        /* 普通文件 */
        conn->response_status = 200;
        static_serve_file(conn->fd, &req, root_dir, conn->gzip_enabled, conn->brotli_enabled);
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
server_context_t *server_create(const cocoon_config_t *config) {
    if (!config || !config->root_dir) return NULL;

    server_context_t *ctx = (server_context_t *)calloc(1, sizeof(server_context_t));
    if (!ctx) return NULL;

    /* 复制配置 */
    ctx->config = *config;
    ctx->config.root_dir = strdup(config->root_dir);
    ctx->running = 1;

    g_max_connections = config->max_connections;

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
        proxy_add_rule(&ctx->proxy_config, ctx->config.proxies[i].prefix, ctx->config.proxies[i].target);
    }

    /* 加载插件 */
    for (size_t i = 0; i < ctx->config.num_plugins; i++) {
        if (cocoon_plugin_load(ctx->config.plugins[i]) != 0) {
            log_error("插件加载失败: %s", ctx->config.plugins[i]);
        }
    }

    return ctx;
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

        /* 在主线程中运行 accept_loop */
        accept_loop(ctx);

        /* 等待所有协程完成 */
        coco_global_sched_wait();
        coco_global_sched_stop();
    } else {
        /* 单线程模式：listen_fd 保持阻塞 */
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
void server_destroy(server_context_t *ctx) {
    if (!ctx) return;

    /* 关闭反向代理连接池 */
    proxy_config_destroy(&ctx->proxy_config);

    /* 卸载插件 */
    cocoon_plugin_unload_all();

    cocoon_middleware_cleanup();

    tls_destroy_context();

    if (ctx->listen_fd != COCOON_INVALID_SOCKET) {
        cocoon_socket_close(ctx->listen_fd);
        ctx->listen_fd = COCOON_INVALID_SOCKET;
    }

    if (ctx->config.root_dir) {
        free((void *)ctx->config.root_dir);
        ctx->config.root_dir = NULL;
    }

    free(ctx);
}

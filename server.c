#define _GNU_SOURCE

/**
 * server.c - 服务器核心实现
 *
 * 基于 coco 协程库实现高并发静态资源服务器。
 * 每个客户端连接由一个独立的协程处理，主线程负责 accept。
 *
 * 架构:
 *   主线程: socket() → bind() → listen() → accept() → 创建协程
 *   协程:   读取请求 → 解析 HTTP → 服务静态资源 → 关闭连接
 *
 * @author xfy
 */

#include "server.h"
#include "http.h"
#include "static.h"
#include "cocoon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>

#include "../coco/include/coco.h"

/* 单个连接缓冲区大小 */
#define CONN_BUF_SIZE   8192
/* 连接超时（毫秒） */
#define CONN_TIMEOUT_MS 30000

/**
 * connection_t - 单个客户端连接上下文
 *
 * 包含 socket fd、接收缓冲区、解析状态。
 */
typedef struct {
    int         fd;             /**< 客户端 socket */
    char        buf[CONN_BUF_SIZE]; /**< 接收缓冲区 */
    size_t      buf_len;        /**< 缓冲区已用长度 */
    bool        keep_alive;     /**< 当前连接是否保持 */
    bool        closed;         /**< 连接是否已关闭 */
    const char *root_dir;       /**< 静态资源根目录（引用，不拥有） */
} connection_t;

/**
 * server_context - 服务器运行时上下文
 */
struct server_context {
    int                 listen_fd;    /**< 监听 socket */
    cocoon_config_t     config;       /**< 配置副本 */
    volatile int        running;      /**< 运行标志 */
    coco_sched_t       *sched;        /**< 协程调度器 */
};

/**
 * set_nonblocking - 设置 socket 为非阻塞模式
 *
 * @param fd socket 文件描述符
 */
static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * close_connection - 安全关闭连接
 *
 * 关闭 socket 并释放连接结构体内存。
 *
 * @param conn 连接上下文
 */
static void close_connection(connection_t *conn) {
    if (conn && conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
        conn->closed = true;
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
    if (!conn || conn->fd < 0 || conn->closed) return -1;

    size_t space = CONN_BUF_SIZE - conn->buf_len;
    if (space == 0) return -1; /* 缓冲区满 */

    ssize_t n;

    /* 尝试使用 coco 的异步 I/O */
    if (coco_sched_get_current() != NULL) {
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
 * handle_request - 处理单个 HTTP 请求
 *
 * 从缓冲区解析请求，判断是文件还是目录，调用对应的服务函数。
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
        static_send_error(conn->fd, 400, false);
        return false;
    }

    /* 消费已解析的数据 */
    if ((size_t)parsed < conn->buf_len) {
        memmove(conn->buf, conn->buf + parsed, conn->buf_len - (size_t)parsed);
    }
    conn->buf_len -= (size_t)parsed;

    /* 只支持 GET 和 HEAD */
    if (req.method != HTTP_GET && req.method != HTTP_HEAD) {
        static_send_error(conn->fd, 405, req.keep_alive);
        return req.keep_alive;
    }

    /* 安全路径拼接 */
    char real_path[4096];
    char root_normalized[4096];
    if (!realpath(root_dir, root_normalized)) {
        strncpy(root_normalized, root_dir, sizeof(root_normalized) - 1);
        root_normalized[sizeof(root_normalized) - 1] = '\0';
    }

    int n = snprintf(real_path, sizeof(real_path), "%s%s", root_normalized, req.path);
    if (n < 0 || (size_t)n >= sizeof(real_path)) {
        static_send_error(conn->fd, 400, req.keep_alive);
        return req.keep_alive;
    }

    /* 路径遍历检查 */
    if (strstr(req.path, "..") != NULL) {
        char resolved[4096];
        if (!realpath(real_path, resolved) ||
            strncmp(resolved, root_normalized, strlen(root_normalized)) != 0) {
            static_send_error(conn->fd, 403, req.keep_alive);
            return req.keep_alive;
        }
        snprintf(real_path, sizeof(real_path), "%s", resolved);
    }

    /* 判断文件类型 */
    struct stat st;
    if (stat(real_path, &st) != 0) {
        static_send_error(conn->fd, 404, req.keep_alive);
        return req.keep_alive;
    }

    if (S_ISDIR(st.st_mode)) {
        /* 目录：尝试 index.html */
        char index_path[4096];
        if (snprintf(index_path, sizeof(index_path), "%s/index.html", real_path) >= (int)sizeof(index_path)) {
            static_send_error(conn->fd, 400, req.keep_alive);
            return req.keep_alive;
        }
        struct stat index_st;
        if (stat(index_path, &index_st) == 0 && S_ISREG(index_st.st_mode)) {
            /* 有 index.html，作为文件服务 */
            http_request_t index_req = req;
            if (snprintf(index_req.path, sizeof(index_req.path), "%s/index.html", req.path) >= (int)sizeof(index_req.path)) {
                static_send_error(conn->fd, 400, req.keep_alive);
                return req.keep_alive;
            }
            static_serve_file(conn->fd, &index_req, root_dir);
        } else {
            /* 无 index.html，生成目录列表 */
            static_serve_directory(conn->fd, &req, root_dir, real_path);
        }
    } else if (S_ISREG(st.st_mode)) {
        /* 普通文件 */
        static_serve_file(conn->fd, &req, root_dir);
    } else {
        static_send_error(conn->fd, 403, req.keep_alive);
    }

    return req.keep_alive;
}

/**
 * client_handler - 客户端连接协程入口
 *
 * 每个连接一个协程，循环读取请求并处理，直到连接关闭或超时。
 *
 * @param arg 连接上下文指针（connection_t*）
 */
static void client_handler(void *arg) {
    connection_t *conn = (connection_t *)arg;
    if (!conn) return;

    while (!conn->closed) {
        /* 读取数据 */
        ssize_t n = conn_read(conn);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 数据不足，继续等待 */
                continue;
            }
            break; /* 错误 */
        }
        if (n == 0) {
            break; /* 对端关闭 */
        }

        /* 尝试处理请求 */
        bool keep = handle_request(conn, conn->root_dir);
        if (!keep) {
            break;
        }
    }

    close_connection(conn);
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
    server_context_t *ctx = (server_context_t *)arg;
    if (!ctx) return;

    printf("[Cocoon] 服务器启动于端口 %d\n", ctx->config.port);
    if (ctx->config.threaded) {
        printf("[Cocoon] 多线程模式: %d 个工作线程\n",
               ctx->config.num_workers > 0 ? ctx->config.num_workers : (uint32_t)sysconf(_SC_NPROCESSORS_ONLN));
    }
    printf("[Cocoon] 静态资源根目录: %s\n", ctx->config.root_dir);

    while (ctx->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(ctx->listen_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        /* 设置 TCP_NODELAY 减少延迟 */
        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        /* 创建连接上下文 */
        connection_t *conn = (connection_t *)calloc(1, sizeof(connection_t));
        if (!conn) {
            close(client_fd);
            continue;
        }
        conn->fd = client_fd;
        conn->keep_alive = true;
        conn->closed = false;
        conn->root_dir = ctx->config.root_dir;

        if (ctx->config.threaded && coco_sched_get_current()) {
            /* 多线程协程模式：创建协程处理连接 */
            coco_coro_t *coro = coco_create(coco_sched_get_current(),
                                            client_handler, conn, 0);
            if (!coro) {
                close_connection(conn);
                free(conn);
            }
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

    /* 创建监听 socket */
    ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) {
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
        close(ctx->listen_fd);
        free(ctx);
        return NULL;
    }

    /* 开始监听 */
    if (listen(ctx->listen_fd, 128) < 0) {
        close(ctx->listen_fd);
        free(ctx);
        return NULL;
    }

    /* 非阻塞模式 */
    set_nonblocking(ctx->listen_fd);

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

    if (ctx->config.threaded) {
        /* 多线程协程模式 */
        uint32_t num_workers = ctx->config.num_workers;
        if (num_workers == 0) {
            num_workers = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
            if (num_workers == 0) num_workers = 4;
        }

        /* 启动全局调度器 */
        int ret = coco_global_sched_start(num_workers);
        if (ret != COCO_OK) {
            fprintf(stderr, "[Cocoon] 启动多线程调度器失败: %d\n", ret);
            return COCOON_ERROR;
        }

        /* 在调度器上运行 accept 循环 */
        /* 注意：当前实现使用主线程直接 accept，多线程仅用于工作协程 */
        accept_loop(ctx);

        coco_global_sched_wait();
        coco_global_sched_stop();
    } else {
        /* 单线程模式 */
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

    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }

    if (ctx->config.root_dir) {
        free((void *)ctx->config.root_dir);
        ctx->config.root_dir = NULL;
    }

    free(ctx);
}

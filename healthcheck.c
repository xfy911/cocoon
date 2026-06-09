/**
 * healthcheck.c - 主动健康检查实现
 *
 * 为每个代理规则启动一个后台线程，周期性向所有后端发送
 * HTTP GET 探测请求，根据响应状态码更新后端健康状态。
 *
 * 与被动健康检查共用 cocoon_proxy_backend_t 中的状态字段：
 *   healthy, fail_count, success_count, last_check
 *
 * @author xfy
 */

#include "healthcheck.h"
#include "log.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <poll.h>

/**
 * healthcheck_build_request - 构建 HTTP 探测请求
 */
static void healthcheck_build_request(const cocoon_proxy_backend_t *backend,
                                      const char *path,
                                      char *buf, size_t buf_len) {
    snprintf(buf, buf_len,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "User-Agent: Cocoon-Healthcheck/1.0\r\n"
        "\r\n",
        path && path[0] ? path : HC_DEFAULT_PATH,
        backend->target_host,
        backend->target_port);
}

/**
 * healthcheck_connect - 建立到后端的 TCP 连接（非阻塞）
 *
 * 使用 poll 等待连接完成，支持超时。
 *
 * @param backend 后端
 * @param timeout_ms 超时毫秒
 * @return socket fd，失败返回 COCOON_INVALID_SOCKET
 */
static cocoon_socket_t healthcheck_connect(const cocoon_proxy_backend_t *backend,
                                            uint32_t timeout_ms) {
    struct hostent *host = gethostbyname(backend->target_host);
    if (!host) {
        log_debug("[hc] 无法解析主机: %s", backend->target_host);
        return COCOON_INVALID_SOCKET;
    }

    cocoon_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == COCOON_INVALID_SOCKET) {
        return COCOON_INVALID_SOCKET;
    }

    /* 设为非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend->target_port);
    memcpy(&addr.sin_addr, host->h_addr_list[0], (size_t)host->h_length);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        /* 立即连接成功 */
        return fd;
    }
    if (rc < 0 && errno != EINPROGRESS && errno != EAGAIN) {
        cocoon_socket_close(fd);
        return COCOON_INVALID_SOCKET;
    }

    /* 等待连接完成 */
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    rc = poll(&pfd, 1, (int)timeout_ms);
    if (rc <= 0) {
        cocoon_socket_close(fd);
        return COCOON_INVALID_SOCKET;
    }

    int soerr = 0;
    socklen_t soerr_len = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len) < 0 || soerr != 0) {
        cocoon_socket_close(fd);
        return COCOON_INVALID_SOCKET;
    }

    return fd;
}

/**
 * healthcheck_parse_status - 从 HTTP 响应解析状态码
 *
 * 只读取首行，不关心 body。
 *
 * @param data 响应数据
 * @param len 数据长度
 * @return 状态码，解析失败返回 -1
 */
static int healthcheck_parse_status(const char *data, size_t len) {
    if (len < 12) return -1;
    if (strncmp(data, "HTTP/1.1", 8) != 0 && strncmp(data, "HTTP/1.0", 8) != 0) return -1;

    const char *p = data + 8;
    while (p < data + len && (*p == ' ' || *p == '\t')) p++;
    if (p + 3 > data + len) return -1;

    int status = 0;
    for (int i = 0; i < 3; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        status = status * 10 + (p[i] - '0');
    }
    return status;
}

/**
 * healthcheck_probe_backend - 对单个后端执行探测
 *
 * 发送 HTTP GET 请求，解析响应状态码。2xx 视为健康，其他视为不健康。
 *
 * @param backend 后端
 * @param path 探测路径
 * @param timeout_ms 超时毫秒
 * @return true 健康
 */
static bool healthcheck_probe_backend(cocoon_proxy_backend_t *backend,
                                        const char *path,
                                        uint32_t timeout_ms) {
    /* 跳过 HTTPS 后端（暂不支持 TLS 探测） */
    if (backend->target_https) {
        log_debug("[hc] 跳过 HTTPS 后端探测: %s:%d",
                  backend->target_host, backend->target_port);
        return backend->healthy; /* 保持原状态 */
    }

    cocoon_socket_t fd = healthcheck_connect(backend, timeout_ms);
    if (fd == COCOON_INVALID_SOCKET) {
        log_debug("[hc] 连接失败: %s:%d", backend->target_host, backend->target_port);
        return false;
    }

    char request[512];
    healthcheck_build_request(backend, path, request, sizeof(request));

    if (send(fd, request, strlen(request), 0) < 0) {
        cocoon_socket_close(fd);
        return false;
    }

    char response[4096];
    ssize_t total = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int poll_rc = poll(&pfd, 1, (int)timeout_ms);

    if (poll_rc > 0) {
        ssize_t r = recv(fd, response, sizeof(response) - 1, 0);
        if (r > 0) total = r;
    }

    cocoon_socket_close(fd);

    if (total <= 0) {
        log_debug("[hc] 无响应: %s:%d", backend->target_host, backend->target_port);
        return false;
    }

    response[total] = '\0';
    int status = healthcheck_parse_status(response, (size_t)total);
    bool healthy = (status >= 200 && status < 300);

    log_debug("[hc] %s:%d -> HTTP %d (%s)",
              backend->target_host, backend->target_port,
              status, healthy ? "healthy" : "unhealthy");
    return healthy;
}

/**
 * healthcheck_probe_once - 对单个后端执行一次探测（公共 API）
 */
bool healthcheck_probe_once(cocoon_proxy_backend_t *backend, uint32_t timeout_ms) {
    if (!backend) return false;
    return healthcheck_probe_backend(backend, HC_DEFAULT_PATH, timeout_ms);
}

/**
 * healthcheck_update_state - 更新后端健康状态（与被动检查共用逻辑）
 *
 * 连续 success_count 次成功恢复，连续 fail_count 次失败标记不健康。
 */
static void healthcheck_update_state(cocoon_proxy_backend_t *backend, bool success) {
    time_t now = time(NULL);
    backend->last_check = now;

    if (success) {
        backend->fail_count = 0;
        backend->success_count++;
        if (!backend->healthy && backend->success_count >= COCOON_HEALTHY_THRESHOLD) {
            backend->healthy = true;
            log_info("[hc] 后端恢复健康: %s:%d",
                     backend->target_host, backend->target_port);
        }
    } else {
        backend->success_count = 0;
        backend->fail_count++;
        if (backend->healthy && backend->fail_count >= COCOON_UNHEALTHY_THRESHOLD) {
            backend->healthy = false;
            log_warn("[hc] 后端标记不健康: %s:%d (连续失败 %d 次)",
                     backend->target_host, backend->target_port, backend->fail_count);
        }
    }
}

/**
 * healthcheck_thread_func - 探测线程主函数
 *
 * 每个代理规则一个线程，周期性探测所有后端。
 */
static void *healthcheck_thread_func(void *arg) {
    cocoon_healthcheck_thread_t *ctx = (cocoon_healthcheck_thread_t *)arg;
    cocoon_proxy_rule_t *rule = ctx->rule;
    if (!rule) return NULL;

    log_info("[hc] 探测线程启动: %s (%zu 后端)", rule->path_prefix, rule->backend_count);

    while (ctx->running) {
        for (size_t i = 0; i < rule->backend_count; i++) {
            cocoon_proxy_backend_t *backend = &rule->backends[i];
            cocoon_healthcheck_config_t *hc = &backend->hc_config;

            if (!hc->enabled) continue;

            uint32_t interval_ms = hc->interval_ms > 0 ? hc->interval_ms : HC_DEFAULT_INTERVAL_MS;
            uint32_t timeout_ms = hc->timeout_ms > 0 ? hc->timeout_ms : HC_DEFAULT_TIMEOUT_MS;
            const char *path = hc->path[0] ? hc->path : HC_DEFAULT_PATH;

            bool healthy = healthcheck_probe_backend(backend, path, timeout_ms);
            healthcheck_update_state(backend, healthy);
            (void)interval_ms; /* 可能用于后续扩展 */
        }

        /* 等待间隔（可被提前中断） */
        uint32_t sleep_ms = HC_DEFAULT_INTERVAL_MS;
        if (rule->backend_count > 0) {
            cocoon_healthcheck_config_t *hc = &rule->backends[0].hc_config;
            if (hc->interval_ms > 0) sleep_ms = hc->interval_ms;
        }

        /* 分步睡眠，检查 running 标志 */
        uint32_t slept = 0;
        while (ctx->running && slept < sleep_ms) {
            uint32_t step = sleep_ms - slept;
            if (step > 500) step = 500;
            usleep(step * 1000);
            slept += step;
        }
    }

    log_info("[hc] 探测线程退出: %s", rule->path_prefix);
    return NULL;
}

/**
 * healthcheck_manager_init - 初始化管理器
 */
void healthcheck_manager_init(cocoon_healthcheck_manager_t *mgr) {
    memset(mgr, 0, sizeof(*mgr));
}

/**
 * healthcheck_start - 启动所有探测线程
 */
bool healthcheck_start(cocoon_healthcheck_manager_t *mgr,
                       cocoon_proxy_config_t *proxy_cfg,
                       uint32_t global_timeout_ms) {
    (void)global_timeout_ms; /* 预留：可作为未配置后端的默认超时 */
    if (!mgr || !proxy_cfg) return false;

    healthcheck_manager_init(mgr);

    for (size_t i = 0; i < proxy_cfg->count; i++) {
        cocoon_proxy_rule_t *rule = &proxy_cfg->rules[i];
        if (rule->backend_count == 0) continue;

        /* 检查是否有后端启用了健康检查 */
        bool any_enabled = false;
        for (size_t j = 0; j < rule->backend_count; j++) {
            if (rule->backends[j].hc_config.enabled) {
                any_enabled = true;
                break;
            }
        }
        if (!any_enabled) continue;

        if (mgr->count >= COCOON_MAX_PROXY_RULES) {
            log_warn("[hc] 规则数超过上限，跳过: %s", rule->path_prefix);
            continue;
        }

        cocoon_healthcheck_thread_t *t = &mgr->threads[mgr->count];
        t->rule = rule;
        t->running = true;

        int rc = pthread_create(&t->thread, NULL, healthcheck_thread_func, t);
        if (rc != 0) {
            log_error("[hc] 探测线程创建失败: %s (%d)", rule->path_prefix, rc);
            t->running = false;
            t->rule = NULL;
            continue;
        }

        mgr->count++;
    }

    log_info("[hc] 已启动 %zu 个探测线程", mgr->count);
    return true;
}

/**
 * healthcheck_stop - 停止所有探测线程并 join
 */
void healthcheck_stop(cocoon_healthcheck_manager_t *mgr) {
    if (!mgr) return;

    for (size_t i = 0; i < mgr->count; i++) {
        cocoon_healthcheck_thread_t *t = &mgr->threads[i];
        if (t->running) {
            t->running = false;
            pthread_join(t->thread, NULL);
            t->rule = NULL;
        }
    }
    mgr->count = 0;
}

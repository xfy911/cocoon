/**
 * proxy.c - 反向代理实现
 *
 * 轻量级 HTTP/1.1 反向代理，支持流式转发。
 *
 * @author xfy
 */

#include "proxy.h"
#include "proxy_tls.h"
#include "log.h"
#include "throttle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <pthread.h>

void proxy_init(cocoon_proxy_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->count = 0;
}

static void backend_init(cocoon_proxy_backend_t *backend, uint32_t weight) {
    backend->healthy = true;
    backend->fail_count = 0;
    backend->success_count = 0;
    backend->last_check = 0;
    backend->weight = weight > 0 ? weight : 1;
    backend->current_weight = 0;
}

void proxy_pool_init(cocoon_proxy_backend_t *backend, size_t max_pool_size) {
    memset(&backend->pool, 0, sizeof(backend->pool));
    backend->pool.max_size = max_pool_size;
    if (backend->pool.max_size > COCOON_POOL_MAX_CAPACITY) {
        backend->pool.max_size = COCOON_POOL_MAX_CAPACITY;
    }
    if (backend->pool.max_size == 0) {
        backend->pool.max_size = COCOON_POOL_DEFAULT_SIZE;
    }
    memset(&backend->pool.stats, 0, sizeof(backend->pool.stats));
    pthread_mutex_init(&backend->pool.mutex, NULL);
    for (size_t i = 0; i < COCOON_POOL_MAX_CAPACITY; i++) {
        backend->pool.conns[i].fd = COCOON_INVALID_SOCKET;
        backend->pool.conns[i].tls_conn = NULL;
        backend->pool.conns[i].in_use = false;
        backend->pool.conns[i].last_used = 0;
    }
}

/**
 * proxy_pool_conn_is_alive - 检测连接是否仍有效
 *
 * 使用 recv(MSG_PEEK | MSG_DONTWAIT) 非阻塞检测连接是否被对端关闭。
 * 不消耗 socket 缓冲区中的数据。
 */
bool proxy_pool_conn_is_alive(cocoon_socket_t fd) {
    if (fd == COCOON_INVALID_SOCKET) return false;

    char buf[1];
    ssize_t r = recv(fd, buf, sizeof(buf), MSG_PEEK | MSG_DONTWAIT);
    if (r == 0) {
        /* 对端已关闭 */
        return false;
    }
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            /* 无数据但连接仍有效 */
            return true;
        }
        /* 其他错误（ECONNRESET 等） */
        return false;
    }
    /* 有数据可读，连接有效 */
    return true;
}

size_t proxy_pool_get_stats(cocoon_proxy_backend_t *backend, cocoon_pool_stats_t *stats) {
    if (!backend) return 0;
    pthread_mutex_lock(&backend->pool.mutex);
    if (stats) {
        *stats = backend->pool.stats;
    }
    size_t idle = 0;
    for (size_t i = 0; i < backend->pool.max_size; i++) {
        if (!backend->pool.conns[i].in_use &&
            (backend->pool.conns[i].fd != COCOON_INVALID_SOCKET || backend->pool.conns[i].tls_conn)) {
            idle++;
        }
    }
    pthread_mutex_unlock(&backend->pool.mutex);
    return idle;
}

void proxy_pool_destroy(cocoon_proxy_backend_t *backend) {
    if (!backend) return;
    pthread_mutex_lock(&backend->pool.mutex);
    for (size_t i = 0; i < backend->pool.max_size; i++) {
        cocoon_pooled_conn_t *pc = &backend->pool.conns[i];
        if (pc->fd != COCOON_INVALID_SOCKET) {
            cocoon_socket_close(pc->fd);
            pc->fd = COCOON_INVALID_SOCKET;
        }
        if (pc->tls_conn) {
            proxy_tls_close(pc->tls_conn);
            pc->tls_conn = NULL;
        }
        pc->in_use = false;
    }
    pthread_mutex_unlock(&backend->pool.mutex);
    pthread_mutex_destroy(&backend->pool.mutex);
}

/* proxy_connect_backend 前向声明 */
static cocoon_socket_t proxy_connect_backend(const cocoon_proxy_backend_t *backend);

bool proxy_pool_acquire(cocoon_proxy_backend_t *backend, cocoon_socket_t *pfd, proxy_tls_conn_t **ptls) {
    if (!backend || !pfd || !ptls) return false;

    bool use_https = backend->target_https;
    time_t now = time(NULL);

    pthread_mutex_lock(&backend->pool.mutex);

    /* 1. 查找可用空闲连接（未超时） */
    for (size_t i = 0; i < backend->pool.max_size; i++) {
        cocoon_pooled_conn_t *pc = &backend->pool.conns[i];
        if (pc->in_use) continue;
        if (pc->fd == COCOON_INVALID_SOCKET && !pc->tls_conn) continue;

        /* 检查超时 */
        if ((now - pc->last_used) * 1000 > COCOON_POOL_IDLE_TIMEOUT_MS) {
            /* 超时，关闭旧连接 */
            backend->pool.stats.evict_count++;
            if (pc->fd != COCOON_INVALID_SOCKET) {
                cocoon_socket_close(pc->fd);
                pc->fd = COCOON_INVALID_SOCKET;
            }
            if (pc->tls_conn) {
                proxy_tls_close(pc->tls_conn);
                pc->tls_conn = NULL;
            }
            continue;
        }

        /* 检查连接有效性（非 HTTPS 连接） */
        if (!use_https && !proxy_pool_conn_is_alive(pc->fd)) {
            backend->pool.stats.alive_check_fail++;
            if (pc->fd != COCOON_INVALID_SOCKET) {
                cocoon_socket_close(pc->fd);
                pc->fd = COCOON_INVALID_SOCKET;
            }
            continue;
        }

        /* 标记为使用中 */
        pc->in_use = true;
        pc->last_used = now;
        backend->pool.stats.hit_count++;
        backend->pool.stats.total_requests++;
        backend->pool.stats.active_conns++;
        *pfd = pc->fd;
        *ptls = pc->tls_conn;
        pthread_mutex_unlock(&backend->pool.mutex);
        log_debug("连接池复用: %s:%d (fd=%d, tls=%p)",
                  backend->target_host, backend->target_port,
                  (int)pc->fd, (void*)pc->tls_conn);
        return true;
    }

    pthread_mutex_unlock(&backend->pool.mutex);

    /* 2. 没有可用连接，新建一个 */
    if (use_https) {
        proxy_tls_conn_t *tls = proxy_tls_connect(backend->target_host, backend->target_port);
        if (!tls) {
            log_warn("连接池新建 TLS 连接失败: %s:%d", backend->target_host, backend->target_port);
            return false;
        }
        *pfd = COCOON_INVALID_SOCKET;
        *ptls = tls;
    } else {
        cocoon_socket_t fd = proxy_connect_backend(backend);
        if (fd == COCOON_INVALID_SOCKET) {
            log_warn("连接池新建连接失败: %s:%d", backend->target_host, backend->target_port);
            return false;
        }
        *pfd = fd;
        *ptls = NULL;
    }

    log_debug("连接池新建: %s:%d (fd=%d, tls=%p)",
              backend->target_host, backend->target_port,
              (int)*pfd, (void*)*ptls);
    backend->pool.stats.miss_count++;
    backend->pool.stats.total_requests++;
    backend->pool.stats.active_conns++;
    return true;
}

void proxy_pool_release(cocoon_proxy_backend_t *backend, cocoon_socket_t fd, proxy_tls_conn_t *tls_conn) {
    if (!backend) return;

    bool use_https = backend->target_https;
    time_t now = time(NULL);

    pthread_mutex_lock(&backend->pool.mutex);

    if (backend->pool.stats.active_conns > 0) {
        backend->pool.stats.active_conns--;
    }

    /* 先尝试找到这个连接的槽位（如果它原本就在池中） */
    for (size_t i = 0; i < backend->pool.max_size; i++) {
        cocoon_pooled_conn_t *pc = &backend->pool.conns[i];
        if (pc->in_use &&
            ((use_https && pc->tls_conn == tls_conn) ||
             (!use_https && pc->fd == fd))) {
            pc->in_use = false;
            pc->last_used = now;
            pthread_mutex_unlock(&backend->pool.mutex);
            log_debug("连接池归还(原位): %s:%d", backend->target_host, backend->target_port);
            return;
        }
    }

    /* 如果是新建连接，找一个空槽位放入 */
    for (size_t i = 0; i < backend->pool.max_size; i++) {
        cocoon_pooled_conn_t *pc = &backend->pool.conns[i];
        if (pc->fd == COCOON_INVALID_SOCKET && !pc->tls_conn) {
            pc->fd = fd;
            pc->tls_conn = tls_conn;
            pc->in_use = false;
            pc->last_used = now;
            pthread_mutex_unlock(&backend->pool.mutex);
            log_debug("连接池归还(新槽): %s:%d", backend->target_host, backend->target_port);
            return;
        }
    }

    pthread_mutex_unlock(&backend->pool.mutex);

    /* 池满，直接关闭 */
    log_debug("连接池满，关闭连接: %s:%d", backend->target_host, backend->target_port);
    if (use_https && tls_conn) {
        proxy_tls_close(tls_conn);
    } else if (fd != COCOON_INVALID_SOCKET) {
        cocoon_socket_close(fd);
    }
}

static void parse_backend_url(const char *target_url, cocoon_proxy_backend_t *backend, size_t pool_size) {
    /* 解析目标URL */
    bool https = false;
    const char *url = target_url;

    backend->target_https = false;
    backend->target_port = 80;
    backend->target_host[0] = '\0';
    backend->target_path[0] = '\0';

    if (strncmp(url, "http://", 7) == 0) {
        url += 7;
        backend->target_https = false;
    } else if (strncmp(url, "https://", 8) == 0) {
        url += 8;
        backend->target_https = true;
        https = true;
    } else {
        /* 默认 http */
        backend->target_https = false;
    }

    /* 提取 host:port 和剩余路径 */
    const char *slash = strchr(url, '/');
    char host_port[256];
    if (slash) {
        size_t host_len = (size_t)(slash - url);
        if (host_len >= sizeof(host_port)) host_len = sizeof(host_port) - 1;
        memcpy(host_port, url, host_len);
        host_port[host_len] = '\0';
        strncpy(backend->target_path, slash, sizeof(backend->target_path) - 1);
    } else {
        strncpy(host_port, url, sizeof(host_port) - 1);
        host_port[sizeof(host_port) - 1] = '\0';
        backend->target_path[0] = '\0';
    }

    /* 解析 host 和 port */
    const char *colon = strrchr(host_port, ':');
    if (colon) {
        size_t host_len = (size_t)(colon - host_port);
        if (host_len >= sizeof(backend->target_host)) host_len = sizeof(backend->target_host) - 1;
        memcpy(backend->target_host, host_port, host_len);
        backend->target_host[host_len] = '\0';
        backend->target_port = (uint16_t)atoi(colon + 1);
    } else {
        size_t host_len = strlen(host_port);
        if (host_len >= sizeof(backend->target_host)) host_len = sizeof(backend->target_host) - 1;
        memcpy(backend->target_host, host_port, host_len);
        backend->target_host[host_len] = '\0';
        backend->target_port = https ? 443 : 80;
    }

    if (backend->target_port == 0) {
        backend->target_port = https ? 443 : 80;
    }
    
    /* 初始化连接池 */
    proxy_pool_init(backend, pool_size);
}

bool proxy_add_rule(cocoon_proxy_config_t *cfg, const char *prefix, const char *target_url, size_t pool_size, uint32_t weight, const cocoon_healthcheck_config_t *hc) {
    if (!prefix || !target_url || prefix[0] == '\0' || target_url[0] == '\0') {
        return false;
    }

    /* 查找是否已有相同前缀的规则 */
    cocoon_proxy_rule_t *rule = NULL;
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->rules[i].path_prefix, prefix) == 0) {
            rule = &cfg->rules[i];
            break;
        }
    }

    if (rule) {
        /* 追加后端 */
        if (rule->backend_count >= COCOON_MAX_PROXY_BACKENDS) {
            log_error("后端数量超过上限 %d", COCOON_MAX_PROXY_BACKENDS);
            return false;
        }
        cocoon_proxy_backend_t *backend = &rule->backends[rule->backend_count];
        parse_backend_url(target_url, backend, pool_size);
        backend_init(backend, weight);
        if (hc) {
            backend->hc_config = *hc;
        }
        log_info("追加后端: %s -> %s://%s:%d%s (weight=%u)",
                 rule->path_prefix,
                 backend->target_https ? "https" : "http",
                 backend->target_host,
                 backend->target_port,
                 backend->target_path,
                 backend->weight);
        rule->backend_count++;
        return true;
    }

    /* 新建规则 */
    if (cfg->count >= COCOON_MAX_PROXY_RULES) {
        log_error("代理规则数量超过上限 %d", COCOON_MAX_PROXY_RULES);
        return false;
    }

    rule = &cfg->rules[cfg->count];
    strncpy(rule->path_prefix, prefix, sizeof(rule->path_prefix) - 1);
    rule->path_prefix[sizeof(rule->path_prefix) - 1] = '\0';
    rule->backend_count = 0;
    rule->current_index = 0;

    cocoon_proxy_backend_t *backend = &rule->backends[rule->backend_count];
    parse_backend_url(target_url, backend, pool_size);
    backend_init(backend, weight);
    if (hc) {
        backend->hc_config = *hc;
    }
    rule->backend_count++;

    log_info("添加代理规则: %s -> %s://%s:%d%s (weight=%u)",
             rule->path_prefix,
             backend->target_https ? "https" : "http",
             backend->target_host,
             backend->target_port,
             backend->target_path,
             backend->weight);

    cfg->count++;
    return true;
}

cocoon_proxy_rule_t *proxy_match(cocoon_proxy_config_t *cfg, const char *path) {
    if (!cfg || !path) return NULL;

    for (size_t i = 0; i < cfg->count; i++) {
        cocoon_proxy_rule_t *rule = &cfg->rules[i];
        size_t prefix_len = strlen(rule->path_prefix);
        if (strncmp(path, rule->path_prefix, prefix_len) == 0) {
            return rule;
        }
    }
    return NULL;
}

/**
 * proxy_connect_backend - 连接到指定后端
 */
static void proxy_update_health(cocoon_proxy_backend_t *backend, bool success) {
    time_t now = time(NULL);
    backend->last_check = now;

    if (success) {
        backend->fail_count = 0;
        backend->success_count++;
        if (!backend->healthy && backend->success_count >= COCOON_HEALTHY_THRESHOLD) {
            backend->healthy = true;
            log_info("后端恢复健康: %s:%d", backend->target_host, backend->target_port);
        }
    } else {
        backend->success_count = 0;
        backend->fail_count++;
        if (backend->healthy && backend->fail_count >= COCOON_UNHEALTHY_THRESHOLD) {
            backend->healthy = false;
            log_warn("后端标记不健康: %s:%d (连续失败 %d 次)",
                     backend->target_host, backend->target_port, backend->fail_count);
        }
    }
}
static cocoon_socket_t proxy_connect_backend(const cocoon_proxy_backend_t *backend) {
    struct hostent *host = gethostbyname(backend->target_host);
    if (!host) {
        log_error("无法解析主机: %s", backend->target_host);
        return COCOON_INVALID_SOCKET;
    }

    cocoon_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == COCOON_INVALID_SOCKET) {
        log_error("创建 socket 失败");
        return COCOON_INVALID_SOCKET;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(backend->target_port);
    memcpy(&addr.sin_addr, host->h_addr_list[0], (size_t)host->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_warn("连接后端失败: %s:%d", backend->target_host, backend->target_port);
        cocoon_socket_close(fd);
        return COCOON_INVALID_SOCKET;
    }

    return fd;
}

/**
 * proxy_build_forwarded_path - 构建转发路径
 */
static void proxy_build_forwarded_path(const cocoon_proxy_rule_t *rule,
                                       const cocoon_proxy_backend_t *backend,
                                       const char *original_path,
                                       char *out, size_t out_len) {
    size_t prefix_len = strlen(rule->path_prefix);
    const char *remaining = original_path + prefix_len;

    if (backend->target_path[0] == '\0') {
        /* 无目标路径前缀，直接转发剩余部分 */
        if (remaining[0] == '\0') {
            strncpy(out, "/", out_len - 1);
        } else {
            strncpy(out, remaining, out_len - 1);
        }
    } else {
        /* 将目标路径前缀 + 剩余路径拼接 */
        int n = snprintf(out, out_len, "%s%s", backend->target_path, remaining);
        if (n < 0 || (size_t)n >= out_len) {
            out[out_len - 1] = '\0';
        }
    }
    out[out_len - 1] = '\0';
}

/**
 * proxy_build_xff - 构建 X-Forwarded-For 值
 */
static void proxy_build_xff(const struct sockaddr_storage *client_addr,
                            char *out, size_t out_len) {
    if (client_addr->ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)client_addr;
        inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)out_len);
    } else if (client_addr->ss_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)client_addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, out, (socklen_t)out_len);
    } else {
        strncpy(out, "unknown", out_len - 1);
        out[out_len - 1] = '\0';
    }
}

/**
 * send_all_fd - 确保数据全部发送
 */
static int send_all_fd(cocoon_socket_t fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        /* 应用限速 */
        cocoon_throttle_t *t = throttle_lookup(fd);
        if (t) {
            size_t chunk = len - sent;
            uint64_t wait_usec = throttle_consume(t, chunk);
            if (wait_usec > 0) {
                struct timespec ts = {
                    .tv_sec = (time_t)(wait_usec / 1000000),
                    .tv_nsec = (long)((wait_usec % 1000000) * 1000)
                };
                nanosleep(&ts, NULL);
                continue;
            }
        }
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/**
 * proxy_send_all_tls - 通过 TLS 连接发送全部数据
 */
static int proxy_send_all_tls(proxy_tls_conn_t *conn, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = proxy_tls_write(conn, data + sent, len - sent);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

/**
 * proxy_relay_backend - 尝试连接并转发请求到单个后端
 *
 * 从连接池获取连接（或新建），发送请求，转发响应回客户端。
 * 请求完成后将连接归还到池中（如果成功）。
 *
 * @return true 成功，false 失败（连接、发送或转发响应出错）
 */
static bool proxy_relay_backend(cocoon_socket_t client_fd, const http_request_t *req,
                                  cocoon_proxy_rule_t *rule, cocoon_proxy_backend_t *backend,
                                  const struct sockaddr_storage *client_addr,
                                  ssize_t *total_forwarded) {
    cocoon_socket_t backend_fd = COCOON_INVALID_SOCKET;
    proxy_tls_conn_t *tls_conn = NULL;
    bool use_https = backend->target_https;

    /* 从连接池获取连接（优先复用） */
    if (!proxy_pool_acquire(backend, &backend_fd, &tls_conn)) {
        log_warn("后端 %s:%d 连接失败（连接池）", backend->target_host, backend->target_port);
        return false;
    }

    /* 构建转发路径 */
    char forwarded_path[512];
    proxy_build_forwarded_path(rule, backend, req->path, forwarded_path, sizeof(forwarded_path));

    /* 构建 X-Forwarded-For */
    char xff[64];
    proxy_build_xff(client_addr, xff, sizeof(xff));

    /* 构建转发请求 */
    char request_buf[4096];
    int n = snprintf(request_buf, sizeof(request_buf),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Forwarded-For: %s\r\n"
        "X-Forwarded-Proto: %s\r\n"
        "Connection: keep-alive\r\n",
        http_method_str(req->method),
        forwarded_path,
        backend->target_host,
        backend->target_port,
        xff,
        use_https ? "https" : "http");

    /* 透传常见请求头 */
    if (req->content_type[0] != '\0') {
        n += snprintf(request_buf + n, sizeof(request_buf) - n,
            "Content-Type: %s\r\n", req->content_type);
    }
    if (req->content_length > 0) {
        n += snprintf(request_buf + n, sizeof(request_buf) - n,
            "Content-Length: %ld\r\n", (long)req->content_length);
    }

    n += snprintf(request_buf + n, sizeof(request_buf) - n, "\r\n");

    /* 发送请求头 */
    bool send_ok = true;
    if (use_https) {
        if (proxy_send_all_tls(tls_conn, request_buf, (size_t)n) != 0) {
            log_warn("转发请求头到 HTTPS 后端失败: %s:%d", backend->target_host, backend->target_port);
            send_ok = false;
        }
    } else {
        if (send_all_fd(backend_fd, request_buf, (size_t)n) != 0) {
            log_warn("转发请求头到后端失败: %s:%d", backend->target_host, backend->target_port);
            send_ok = false;
        }
    }

    /* 发送请求体 */
    if (send_ok && req->body && req->body_len > 0) {
        if (use_https) {
            if (proxy_send_all_tls(tls_conn, req->body, req->body_len) != 0) {
                log_warn("转发请求体到 HTTPS 后端失败");
                send_ok = false;
            }
        } else {
            if (send_all_fd(backend_fd, req->body, req->body_len) != 0) {
                log_warn("转发请求体到后端失败");
                send_ok = false;
            }
        }
    }

    /* 流式转发响应回客户端 */
    bool success = false;
    ssize_t total = 0;
    bool connection_closed = false;  /* 后端是否主动关闭连接 */

    if (send_ok) {
        char relay_buf[8192];
        bool recv_ok = true;
        while (1) {
            ssize_t r;
            if (use_https) {
                r = proxy_tls_read(tls_conn, relay_buf, sizeof(relay_buf));
            } else {
                r = recv(backend_fd, relay_buf, sizeof(relay_buf), 0);
            }
            if (r < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                recv_ok = false;
                break;
            }
            if (r == 0) {
                /* 后端关闭连接，如果尚未转发任何数据则视为失败
                 * 否则标记 connection_closed，请求成功但连接不可复用
                 */
                if (total == 0) recv_ok = false;
                connection_closed = true;
                break;
            }

            if (send_all_fd(client_fd, relay_buf, (size_t)r) != 0) {
                log_error("转发响应到客户端失败");
                recv_ok = false;
                break;
            }
            total += r;
        }

        if (recv_ok) {
            log_debug("代理完成: %s -> 后端 %s:%d%s, 转发 %zd bytes",
                      req->path, backend->target_host, backend->target_port,
                      forwarded_path, total);
            success = true;
        }
    }

    if (total_forwarded) *total_forwarded = total;

    /* 归还或关闭连接：只有后端未主动关闭时才归还 */
    if (success && total > 0 && !connection_closed) {
        proxy_pool_release(backend, backend_fd, tls_conn);
    } else {
        if (use_https && tls_conn) {
            proxy_tls_close(tls_conn);
        } else if (backend_fd != COCOON_INVALID_SOCKET) {
            cocoon_socket_close(backend_fd);
        }
    }

    return success;
}

/**
 * select_backend_sww - 平滑加权轮询选择后端
 *
 * Nginx 同款平滑加权轮询算法：
 * 1. 所有健康后端 current_weight += weight
 * 2. 选择 current_weight 最大的
 * 3. 选中后端 current_weight -= total_weight
 * 4. 返回选中后端
 *
 * 这样能保证权重比例严格满足，且分布更均匀。
 *
 * @param rule 代理规则
 * @return 选中的后端，无健康后端返回 NULL
 */
static cocoon_proxy_backend_t *select_backend_sww(cocoon_proxy_rule_t *rule) {
    if (!rule || rule->backend_count == 0) return NULL;

    cocoon_proxy_backend_t *best = NULL;
    int32_t total_weight = 0;

    for (size_t i = 0; i < rule->backend_count; i++) {
        cocoon_proxy_backend_t *b = &rule->backends[i];
        if (!b->healthy) continue;

        b->current_weight += (int32_t)b->weight;
        total_weight += (int32_t)b->weight;

        if (!best || b->current_weight > best->current_weight) {
            best = b;
        }
    }

    if (best) {
        best->current_weight -= total_weight;
    }

    return best;
}

/**
 * select_backend_sww_all - 平滑加权轮询（包含不健康后端）
 *
 * 用于 fallback 场景：所有后端都标记为不健康时，仍然尝试请求。
 */
static cocoon_proxy_backend_t *select_backend_sww_all(cocoon_proxy_rule_t *rule) {
    if (!rule || rule->backend_count == 0) return NULL;

    cocoon_proxy_backend_t *best = NULL;
    int32_t total_weight = 0;

    for (size_t i = 0; i < rule->backend_count; i++) {
        cocoon_proxy_backend_t *b = &rule->backends[i];
        b->current_weight += (int32_t)b->weight;
        total_weight += (int32_t)b->weight;

        if (!best || b->current_weight > best->current_weight) {
            best = b;
        }
    }

    if (best) {
        best->current_weight -= total_weight;
    }

    return best;
}

bool proxy_forward(cocoon_socket_t client_fd, const http_request_t *req,
                   cocoon_proxy_rule_t *rule,
                   const struct sockaddr_storage *client_addr) {
    if (!rule || rule->backend_count == 0) {
        log_error("代理规则无后端");
        return false;
    }

    /* 第一轮：优先尝试 healthy 后端（平滑加权轮询） */
    cocoon_proxy_backend_t *backend = select_backend_sww(rule);
    if (backend) {
        ssize_t total = 0;
        if (proxy_relay_backend(client_fd, req, rule, backend, client_addr, &total)) {
            proxy_update_health(backend, true);
            return req->keep_alive;
        }
        proxy_update_health(backend, false);
    }

    /* 如果唯一选中的后端失败了，尝试其他 healthy 后端 */
    for (size_t t = 0; t < rule->backend_count; t++) {
        backend = select_backend_sww(rule);
        if (!backend) break;

        ssize_t total = 0;
        if (proxy_relay_backend(client_fd, req, rule, backend, client_addr, &total)) {
            proxy_update_health(backend, true);
            return req->keep_alive;
        }
        proxy_update_health(backend, false);
    }

    /* 第二轮：fallback 到所有后端（包括 unhealthy） */
    backend = select_backend_sww_all(rule);
    if (backend) {
        ssize_t total = 0;
        if (proxy_relay_backend(client_fd, req, rule, backend, client_addr, &total)) {
            proxy_update_health(backend, true);
            return req->keep_alive;
        }
        proxy_update_health(backend, false);
    }

    log_error("所有后端均不可用: %s", rule->path_prefix);
    return false;
}

void proxy_config_destroy(cocoon_proxy_config_t *cfg) {
    if (!cfg) return;
    for (size_t i = 0; i < cfg->count; i++) {
        cocoon_proxy_rule_t *rule = &cfg->rules[i];
        for (size_t j = 0; j < rule->backend_count; j++) {
            proxy_pool_destroy(&rule->backends[j]);
        }
    }
}


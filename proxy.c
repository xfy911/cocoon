/**
 * proxy.c - 反向代理实现
 *
 * 轻量级 HTTP/1.1 反向代理，支持流式转发。
 *
 * @author xfy
 */

#include "proxy.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

void proxy_init(cocoon_proxy_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->count = 0;
}

bool proxy_add_rule(cocoon_proxy_config_t *cfg, const char *prefix, const char *target_url) {
    if (cfg->count >= COCOON_MAX_PROXY_RULES) {
        log_error("代理规则数量超过上限 %d", COCOON_MAX_PROXY_RULES);
        return false;
    }

    if (!prefix || !target_url || prefix[0] == '\0' || target_url[0] == '\0') {
        return false;
    }

    cocoon_proxy_rule_t *rule = &cfg->rules[cfg->count];

    /* 复制路径前缀 */
    strncpy(rule->path_prefix, prefix, sizeof(rule->path_prefix) - 1);
    rule->path_prefix[sizeof(rule->path_prefix) - 1] = '\0';

    /* 解析目标URL */
    bool https = false;
    const char *url = target_url;

    if (strncmp(url, "http://", 7) == 0) {
        url += 7;
        rule->target_https = false;
    } else if (strncmp(url, "https://", 8) == 0) {
        url += 8;
        rule->target_https = true;
        https = true;
    } else {
        /* 默认 http */
        rule->target_https = false;
    }

    /* 提取 host:port 和剩余路径 */
    const char *slash = strchr(url, '/');
    char host_port[256];
    if (slash) {
        size_t host_len = (size_t)(slash - url);
        if (host_len >= sizeof(host_port)) host_len = sizeof(host_port) - 1;
        memcpy(host_port, url, host_len);
        host_port[host_len] = '\0';
        strncpy(rule->target_path, slash, sizeof(rule->target_path) - 1);
    } else {
        strncpy(host_port, url, sizeof(host_port) - 1);
        host_port[sizeof(host_port) - 1] = '\0';
        rule->target_path[0] = '\0';
    }

    /* 解析 host 和 port */
    const char *colon = strrchr(host_port, ':');
    if (colon) {
        size_t host_len = (size_t)(colon - host_port);
        if (host_len >= sizeof(rule->target_host)) host_len = sizeof(rule->target_host) - 1;
        memcpy(rule->target_host, host_port, host_len);
        rule->target_host[host_len] = '\0';
        rule->target_port = (uint16_t)atoi(colon + 1);
    } else {
        size_t host_len = strlen(host_port);
        if (host_len >= sizeof(rule->target_host)) host_len = sizeof(rule->target_host) - 1;
        memcpy(rule->target_host, host_port, host_len);
        rule->target_host[host_len] = '\0';
        rule->target_port = https ? 443 : 80;
    }

    if (rule->target_port == 0) {
        rule->target_port = https ? 443 : 80;
    }

    log_info("添加代理规则: %s -> %s://%s:%d%s",
             rule->path_prefix,
             https ? "https" : "http",
             rule->target_host,
             rule->target_port,
             rule->target_path);

    cfg->count++;
    return true;
}

const cocoon_proxy_rule_t *proxy_match(const cocoon_proxy_config_t *cfg, const char *path) {
    if (!cfg || !path) return NULL;

    for (size_t i = 0; i < cfg->count; i++) {
        const cocoon_proxy_rule_t *rule = &cfg->rules[i];
        size_t prefix_len = strlen(rule->path_prefix);
        if (strncmp(path, rule->path_prefix, prefix_len) == 0) {
            return rule;
        }
    }
    return NULL;
}

/**
 * proxy_connect_backend - 连接到目标后端
 */
static cocoon_socket_t proxy_connect_backend(const cocoon_proxy_rule_t *rule) {
    struct hostent *host = gethostbyname(rule->target_host);
    if (!host) {
        log_error("无法解析主机: %s", rule->target_host);
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
    addr.sin_port = htons(rule->target_port);
    memcpy(&addr.sin_addr, host->h_addr_list[0], (size_t)host->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_error("连接后端失败: %s:%d", rule->target_host, rule->target_port);
        cocoon_socket_close(fd);
        return COCOON_INVALID_SOCKET;
    }

    return fd;
}

/**
 * proxy_build_forwarded_path - 构建转发路径
 */
static void proxy_build_forwarded_path(const cocoon_proxy_rule_t *rule,
                                       const char *original_path,
                                       char *out, size_t out_len) {
    size_t prefix_len = strlen(rule->path_prefix);
    const char *remaining = original_path + prefix_len;

    if (rule->target_path[0] == '\0') {
        /* 无目标路径前缀，直接转发剩余部分 */
        if (remaining[0] == '\0') {
            strncpy(out, "/", out_len - 1);
        } else {
            strncpy(out, remaining, out_len - 1);
        }
    } else {
        /* 将目标路径前缀 + 剩余路径拼接 */
        int n = snprintf(out, out_len, "%s%s", rule->target_path, remaining);
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

bool proxy_forward(cocoon_socket_t client_fd, const http_request_t *req,
                   const cocoon_proxy_rule_t *rule,
                   const struct sockaddr_storage *client_addr) {
    /* 目前不支持 HTTPS 后端 */
    if (rule->target_https) {
        log_warn("HTTPS 后端暂未支持，拒绝代理: %s", rule->target_host);
        return false;
    }

    cocoon_socket_t backend_fd = proxy_connect_backend(rule);
    if (backend_fd == COCOON_INVALID_SOCKET) {
        return false;
    }

    /* 构建转发路径 */
    char forwarded_path[512];
    proxy_build_forwarded_path(rule, req->path, forwarded_path, sizeof(forwarded_path));

    /* 构建 X-Forwarded-For */
    char xff[64];
    proxy_build_xff(client_addr, xff, sizeof(xff));

    /* 构建转发请求 */
    char request_buf[4096];
    int n = snprintf(request_buf, sizeof(request_buf),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Forwarded-For: %s\r\n"
        "X-Forwarded-Proto: http\r\n"
        "Connection: close\r\n",
        http_method_str(req->method),
        forwarded_path,
        rule->target_host,
        rule->target_port,
        xff);

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
    if (send_all_fd(backend_fd, request_buf, (size_t)n) != 0) {
        log_error("转发请求头到后端失败");
        cocoon_socket_close(backend_fd);
        return false;
    }

    /* 转发请求体 */
    if (req->body && req->body_len > 0) {
        if (send_all_fd(backend_fd, req->body, req->body_len) != 0) {
            log_error("转发请求体到后端失败");
            cocoon_socket_close(backend_fd);
            return false;
        }
    }

    /* 流式转发响应回客户端 */
    char relay_buf[8192];
    ssize_t total_forwarded = 0;
    while (1) {
        ssize_t r = recv(backend_fd, relay_buf, sizeof(relay_buf), 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            break;
        }
        if (r == 0) break;

        if (send_all_fd(client_fd, relay_buf, (size_t)r) != 0) {
            log_error("转发响应到客户端失败");
            break;
        }
        total_forwarded += r;
    }

    log_debug("代理完成: %s -> %s:%d%s, 转发 %zd bytes",
              req->path, rule->target_host, rule->target_port,
              forwarded_path, total_forwarded);

    cocoon_socket_close(backend_fd);
    return req->keep_alive;
}

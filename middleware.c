/**
 * middleware.c - 中间件框架实现
 *
 * 提供轻量级中间件注册表和内置中间件实现。
 * 保持零依赖，不使用外部库。
 *
 * @author xfy
 */

#include "middleware.h"
#include "static.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

/* === 注册表 === */
#define MAX_MIDDLEWARE 16

typedef struct {
    char                        name[32];
    cocoon_middleware_func_t    func;
    void                       *user_data;
    bool                        active;
} middleware_entry_t;

static middleware_entry_t g_registry[MAX_MIDDLEWARE];
static int g_count = 0;

/* === 限流哈希表 === */
#define RATE_LIMIT_BUCKETS 256

typedef struct rate_limit_node {
    struct sockaddr_storage     addr;
    time_t                      last_update;
    uint32_t                    count;
    struct rate_limit_node     *next;
} rate_limit_node_t;

static rate_limit_node_t *g_rate_limit_buckets[RATE_LIMIT_BUCKETS];
static pthread_mutex_t g_rate_limit_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * rate_limit_hash - 对 sockaddr 做简单哈希
 */
static uint32_t rate_limit_hash(const struct sockaddr_storage *addr) {
    uint32_t h = 0;
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *a4 = (const struct sockaddr_in *)addr;
        const uint8_t *p = (const uint8_t *)&a4->sin_addr;
        for (size_t i = 0; i < 4; i++) {
            h = h * 31 + p[i];
        }
    } else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)addr;
        const uint8_t *p = (const uint8_t *)&a6->sin6_addr;
        for (size_t i = 0; i < 16; i++) {
            h = h * 31 + p[i];
        }
    }
    return h % RATE_LIMIT_BUCKETS;
}

/**
 * rate_limit_addr_eq - 比较两个地址是否相同
 */
static bool rate_limit_addr_eq(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family) return false;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *a4 = (const struct sockaddr_in *)a;
        const struct sockaddr_in *b4 = (const struct sockaddr_in *)b;
        return a4->sin_addr.s_addr == b4->sin_addr.s_addr;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)a;
        const struct sockaddr_in6 *b6 = (const struct sockaddr_in6 *)b;
        return memcmp(&a6->sin6_addr, &b6->sin6_addr, 16) == 0;
    }
    return false;
}

/**
 * rate_limit_get_addr - 从 socket 获取对端地址
 */
static bool rate_limit_get_addr(cocoon_socket_t fd, struct sockaddr_storage *addr) {
    socklen_t len = sizeof(*addr);
    return getpeername(fd, (struct sockaddr *)addr, &len) == 0;
}

/**
 * rate_limit_check - 检查是否超过限流阈值
 *
 * @param fd 客户端 socket
 * @param limit 每秒最大请求数
 * @return true 未超限（允许），false 已超限（拒绝）
 */
static bool rate_limit_check(cocoon_socket_t fd, uint32_t limit) {
    if (limit == 0) return true;

    struct sockaddr_storage addr;
    if (!rate_limit_get_addr(fd, &addr)) return true;

    uint32_t h = rate_limit_hash(&addr);
    time_t now = time(NULL);
    bool allow = true;

    pthread_mutex_lock(&g_rate_limit_mutex);

    rate_limit_node_t *node = g_rate_limit_buckets[h];
    while (node) {
        if (rate_limit_addr_eq(&node->addr, &addr)) {
            if (now - node->last_update >= 1) {
                node->last_update = now;
                node->count = 1;
                log_debug("Rate Limit 重置: 同一 IP 新秒");
            } else {
                node->count++;
                log_debug("Rate Limit 计数: %u/%u", node->count, limit);
                if (node->count > limit) {
                    allow = false;
                }
            }
            break;
        }
        node = node->next;
    }

    if (!node) {
        node = (rate_limit_node_t *)malloc(sizeof(rate_limit_node_t));
        if (node) {
            node->addr = addr;
            node->last_update = now;
            node->count = 1;
            node->next = g_rate_limit_buckets[h];
            g_rate_limit_buckets[h] = node;
            log_debug("Rate Limit 新节点: bucket=%u", h);
        }
    }

    pthread_mutex_unlock(&g_rate_limit_mutex);
    return allow;
}

/* === Base64 解码（内部实现） === */
static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_decode(const char *in, char *out, int out_size) {
    int val = 0, valb = -8;
    int out_len = 0;
    for (const char *p = in; *p; p++) {
        const char *pos = strchr(base64_chars, *p);
        if (!pos) {
            if (*p == '=') break;
            continue;
        }
        int c = (int)(pos - base64_chars);
        val = (val << 6) + c;
        valb += 6;
        if (valb >= 0) {
            if (out_len < out_size - 1) {
                out[out_len++] = (char)((val >> valb) & 0xFF);
            }
            valb -= 8;
        }
    }
    out[out_len] = '\0';
    return out_len;
}

/* === 注册表 API === */

int cocoon_middleware_register(const char *name, cocoon_middleware_func_t func, void *user_data) {
    if (!name || !func || g_count >= MAX_MIDDLEWARE) return -1;

    for (int i = 0; i < g_count; i++) {
        if (g_registry[i].active && strcmp(g_registry[i].name, name) == 0) {
            /* 已存在，更新 */
            g_registry[i].func = func;
            g_registry[i].user_data = user_data;
            return 0;
        }
    }

    if (g_count < MAX_MIDDLEWARE) {
        strncpy(g_registry[g_count].name, name, sizeof(g_registry[g_count].name) - 1);
        g_registry[g_count].name[sizeof(g_registry[g_count].name) - 1] = '\0';
        g_registry[g_count].func = func;
        g_registry[g_count].user_data = user_data;
        g_registry[g_count].active = true;
        g_count++;
        log_info("中间件已注册: %s", name);
        return 0;
    }
    return -1;
}

int cocoon_middleware_unregister(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < g_count; i++) {
        if (g_registry[i].active && strcmp(g_registry[i].name, name) == 0) {
            g_registry[i].active = false;
            /* 压缩数组 */
            for (int j = i; j < g_count - 1; j++) {
                g_registry[j] = g_registry[j + 1];
            }
            g_count--;
            return 0;
        }
    }
    return -1;
}

int cocoon_middleware_run(http_request_t *req, cocoon_socket_t fd) {
    for (int i = 0; i < g_count; i++) {
        if (!g_registry[i].active) continue;
        int ret = g_registry[i].func(req, fd, g_registry[i].user_data);
        if (ret != 0) {
            log_debug("中间件 %s 短路了请求", g_registry[i].name);
            return ret;
        }
    }
    return 0;
}

void cocoon_middleware_cleanup(void) {
    g_count = 0;
    memset(g_registry, 0, sizeof(g_registry));

    pthread_mutex_lock(&g_rate_limit_mutex);
    for (int i = 0; i < RATE_LIMIT_BUCKETS; i++) {
        rate_limit_node_t *node = g_rate_limit_buckets[i];
        while (node) {
            rate_limit_node_t *next = node->next;
            free(node);
            node = next;
        }
        g_rate_limit_buckets[i] = NULL;
    }
    pthread_mutex_unlock(&g_rate_limit_mutex);
}

int cocoon_middleware_list(char names[][32], int count) {
    if (!names || count <= 0) return 0;
    int n = 0;
    for (int i = 0; i < g_count && n < count; i++) {
        if (g_registry[i].active) {
            size_t len = strlen(g_registry[i].name);
            if (len > 31) len = 31;
            memcpy(names[n], g_registry[i].name, len);
            names[n][len] = '\0';
            n++;
        }
    }
    return n;
}

/* === 内置中间件 === */

int cocoon_middleware_cors(http_request_t *req, cocoon_socket_t fd, void *user_data) {
    (void)user_data;

    if (req->method == HTTP_OPTIONS) {
        char response[512];
        int n = snprintf(response, sizeof(response),
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, HEAD, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
            "Access-Control-Max-Age: 86400\r\n"
            "Connection: %s\r\n"
            "Server: Cocoon/1.0\r\n"
            "\r\n",
            req->keep_alive ? "keep-alive" : "close");

        cocoon_socket_send(fd, response, (size_t)n);
        log_debug("CORS 中间件: 响应 OPTIONS 预检请求");
        return 1; /* 短路 */
    }
    return 0; /* 继续 */
}

int cocoon_middleware_basic_auth(http_request_t *req, cocoon_socket_t fd, void *user_data) {
    const cocoon_middleware_config_t *cfg = (const cocoon_middleware_config_t *)user_data;
    if (!cfg || !cfg->auth_user || !cfg->auth_pass) return 0; /* 未配置，跳过 */

    const char *auth_header = NULL;
    for (int i = 0; i < req->num_headers; i++) {
        if (strcasecmp(req->headers[i].name, "authorization") == 0) {
            auth_header = req->headers[i].value;
            break;
        }
    }

    bool valid = false;
    if (auth_header && strncasecmp(auth_header, "Basic ", 6) == 0) {
        char decoded[256];
        int decoded_len = base64_decode(auth_header + 6, decoded, sizeof(decoded));
        if (decoded_len > 0) {
            char *colon = strchr(decoded, ':');
            if (colon) {
                *colon = '\0';
                valid = (strcmp(decoded, cfg->auth_user) == 0 &&
                         strcmp(colon + 1, cfg->auth_pass) == 0);
            }
        }
    }

    if (!valid) {
        char response[512];
        int n = snprintf(response, sizeof(response),
            "HTTP/1.1 401 Unauthorized\r\n"
            "WWW-Authenticate: Basic realm=\"Cocoon\"\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: 41\r\n"
            "Connection: %s\r\n"
            "Server: Cocoon/1.0\r\n"
            "\r\n"
            "<html><body>401 Unauthorized</body></html>",
            req->keep_alive ? "keep-alive" : "close");
        cocoon_socket_send(fd, response, (size_t)n);
        log_warn("Basic Auth 失败: %s %s", http_method_str(req->method), req->path);
        return 1; /* 短路 */
    }

    return 0; /* 认证通过，继续 */
}

int cocoon_middleware_rate_limit(http_request_t *req, cocoon_socket_t fd, void *user_data) {
    const cocoon_middleware_config_t *cfg = (const cocoon_middleware_config_t *)user_data;
    if (!cfg || cfg->rate_limit == 0) return 0; /* 未配置，跳过 */

    if (!rate_limit_check(fd, cfg->rate_limit)) {
        char response[512];
        int n = snprintf(response, sizeof(response),
            "HTTP/1.1 429 Too Many Requests\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: 48\r\n"
            "Retry-After: 1\r\n"
            "Connection: %s\r\n"
            "Server: Cocoon/1.0\r\n"
            "\r\n"
            "<html><body>429 Too Many Requests</body></html>\r\n",
            req->keep_alive ? "keep-alive" : "close");
        cocoon_socket_send(fd, response, (size_t)n);
        log_warn("Rate Limit 触发: %s %s", http_method_str(req->method), req->path);
        return 1; /* 短路 */
    }

    return 0; /* 未限流，继续 */
}

void cocoon_middleware_init_builtin(const cocoon_middleware_config_t *config) {
    if (!config) return;

    if (config->cors_enabled) {
        cocoon_middleware_register("cors", cocoon_middleware_cors, NULL);
    }
    if (config->auth_user && config->auth_pass) {
        cocoon_middleware_register("basic_auth", cocoon_middleware_basic_auth, (void *)config);
    }
    if (config->rate_limit > 0) {
        cocoon_middleware_register("rate_limit", cocoon_middleware_rate_limit, (void *)config);
    }
}

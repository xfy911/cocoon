/**
 * proxy.h - 反向代理模块
 *
 * 轻量级反向代理，支持路径前缀匹配到目标后端。
 *
 * @author xfy
 */

#ifndef COCOON_PROXY_H
#define COCOON_PROXY_H

#include "cocoon.h"
#include "http.h"
#include "platform.h"
#include "proxy_tls.h"
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

/* === 连接池可配置 === */
#define COCOON_POOL_MAX_CAPACITY 16  /**< 连接池最大容量上限 */
#define COCOON_POOL_DEFAULT_SIZE 4   /**< 默认连接池大小 */
#define COCOON_POOL_IDLE_TIMEOUT_MS 30000

#define COCOON_MAX_PROXY_BACKENDS 8
#define COCOON_HEALTHY_THRESHOLD 2  /* 连续成功次数恢复健康 */
#define COCOON_UNHEALTHY_THRESHOLD 3  /* 连续失败次数标记不健康 */

/**
 * cocoon_pooled_conn_t - 连接池中的单个连接
 */
typedef struct {
    cocoon_socket_t fd;           /**< socket 文件描述符 */
    proxy_tls_conn_t *tls_conn;   /**< TLS 连接（NULL 表示非 HTTPS） */
    time_t last_used;             /**< 上次使用时间 */
    bool in_use;                  /**< 是否正在使用 */
} cocoon_pooled_conn_t;

/**
 * cocoon_conn_pool_t - 连接池
 */
typedef struct {
    cocoon_pooled_conn_t conns[COCOON_POOL_MAX_CAPACITY]; /**< 空闲连接数组（上限16） */
    size_t max_size;                                      /**< 实际配置大小（默认4） */
    pthread_mutex_t mutex;                                /**< 保护锁 */
} cocoon_conn_pool_t;

/**
 * cocoon_proxy_backend_t - 单个后端服务器配置
 */
typedef struct {
    char target_host[256];
    char target_path[256];
    uint16_t target_port;
    bool target_https;
    /* 健康状态 */
    bool healthy;           /* 当前是否健康 */
    int fail_count;         /* 连续失败次数 */
    int success_count;      /* 连续成功次数 */
    time_t last_check;      /* 上次检测时间 */
    /* 连接池 */
    cocoon_conn_pool_t pool; /**< 后端连接池 */
    /* 权重 */
    uint32_t weight;         /**< 配置权重（默认1） */
    int32_t  current_weight; /**< 当前动态权重（平滑轮询算法用） */
    /* 主动健康检查配置 */
    cocoon_healthcheck_config_t hc_config; /**< 健康检查配置 */
} cocoon_proxy_backend_t;

/**
 * cocoon_proxy_rule_t - 代理规则（支持多后端）
 */
typedef struct {
    char path_prefix[256];
    cocoon_proxy_backend_t backends[COCOON_MAX_PROXY_BACKENDS];
    size_t backend_count;
    size_t current_index;  /* 轮询索引 */
} cocoon_proxy_rule_t;

typedef struct {
    cocoon_proxy_rule_t rules[COCOON_MAX_PROXY_RULES];
    size_t count;
} cocoon_proxy_config_t;

/**
 * proxy_init - 初始化代理配置
 */
void proxy_init(cocoon_proxy_config_t *cfg);

/**
 * proxy_add_rule - 添加代理规则或向后端追加
 *
 * 如果 prefix 已存在，将 target 追加为同一规则的新后端。
 * 如果 prefix 不存在，创建新规则。
 *
 * @param cfg 代理配置
 * @param prefix 路径前缀（如 "/api/"）
 * @param target_url 目标URL（如 "http://localhost:3000"）
 * @param pool_size 连接池大小
 * @param weight 权重
 * @param hc 健康检查配置（可为 NULL）
 * @return true 成功
 */
bool proxy_add_rule(cocoon_proxy_config_t *cfg, const char *prefix, const char *target_url, size_t pool_size, uint32_t weight, const cocoon_healthcheck_config_t *hc);

/**
 * proxy_match - 查找匹配路径的代理规则
 *
 * @param cfg 代理配置
 * @param path 请求路径
 * @return 匹配的规则，未匹配返回 NULL
 */
cocoon_proxy_rule_t *proxy_match(cocoon_proxy_config_t *cfg, const char *path);

/**
 * proxy_config_destroy - 销毁代理配置中的所有连接池
 *
 * 关闭所有后端连接池中的空闲连接。
 *
 * @param cfg 代理配置
 */
void proxy_config_destroy(cocoon_proxy_config_t *cfg);

/**
 * proxy_forward - 将请求转发到目标后端（轮询+failover）
 *
 * 使用轮询算法选择后端，当前后端失败时自动尝试下一个。
 * 建立到后端的连接，转发请求，将响应回写给客户端。
 *
 * @param client_fd 客户端 socket
 * @param req HTTP 请求
 * @param rule 代理规则
 * @param client_addr 客户端地址（用于 X-Forwarded-For）
 * @return true 保持连接，false 关闭
 */
bool proxy_forward(cocoon_socket_t client_fd, const http_request_t *req,
                   cocoon_proxy_rule_t *rule,
                   const struct sockaddr_storage *client_addr);

/**
 * proxy_pool_init - 初始化后端连接池
 */
void proxy_pool_init(cocoon_proxy_backend_t *backend, size_t max_pool_size);

/**
 * proxy_pool_destroy - 销毁后端连接池，关闭所有连接
 */
void proxy_pool_destroy(cocoon_proxy_backend_t *backend);

/**
 * proxy_pool_acquire - 从连接池获取一个可用连接
 *
 * 优先复用空闲连接，没有则新建连接。
 * 返回的连接标记为 in_use。
 *
 * @param backend 后端配置
 * @param pfd 输出 socket fd（COCOON_INVALID_SOCKET 表示使用 TLS）
 * @param ptls 输出 TLS 连接（NULL 表示非 HTTPS）
 * @return true 成功
 */
bool proxy_pool_acquire(cocoon_proxy_backend_t *backend, cocoon_socket_t *pfd, proxy_tls_conn_t **ptls);

/**
 * proxy_pool_release - 将连接放回池中
 *
 * 如果池满或连接不健康，直接关闭。
 *
 * @param backend 后端配置
 * @param fd socket fd（COCOON_INVALID_SOCKET 表示使用 TLS）
 * @param tls_conn TLS 连接（NULL 表示非 HTTPS）
 */
void proxy_pool_release(cocoon_proxy_backend_t *backend, cocoon_socket_t fd, proxy_tls_conn_t *tls_conn);

#endif /* COCOON_PROXY_H */

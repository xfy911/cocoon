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
#include <stdbool.h>

#define COCOON_MAX_PROXY_BACKENDS 8
#define COCOON_HEALTHY_THRESHOLD 2  /* 连续成功次数恢复健康 */
#define COCOON_UNHEALTHY_THRESHOLD 3  /* 连续失败次数标记不健康 */

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
 * @return true 成功
 */
bool proxy_add_rule(cocoon_proxy_config_t *cfg, const char *prefix, const char *target_url);

/**
 * proxy_match - 查找匹配路径的代理规则
 *
 * @param cfg 代理配置
 * @param path 请求路径
 * @return 匹配的规则，未匹配返回 NULL
 */
cocoon_proxy_rule_t *proxy_match(cocoon_proxy_config_t *cfg, const char *path);

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

#endif /* COCOON_PROXY_H */

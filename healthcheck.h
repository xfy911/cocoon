/**
 * healthcheck.h - 主动健康检查模块
 *
 * 周期性向后端发送 HTTP 健康探测请求，提前发现并标记不健康后端。
 * 与 proxy.c 中的被动健康检查共用 healthy/fail_count/success_count 字段。
 *
 * @author xfy
 */

#ifndef COCOON_HEALTHCHECK_H
#define COCOON_HEALTHCHECK_H

#include "proxy.h"
#include <pthread.h>
#include <stdbool.h>

/* 默认健康检查配置 */
#define HC_DEFAULT_PATH         "/health"
#define HC_DEFAULT_INTERVAL_MS  5000
#define HC_DEFAULT_TIMEOUT_MS   2000

/**
 * cocoon_healthcheck_thread_t - 健康检查线程上下文
 *
 * 每个代理规则（含多个后端）对应一个探测线程。
 */
typedef struct {
    pthread_t              thread;       /**< 探测线程 */
    volatile bool          running;      /**< 运行标志 */
    cocoon_proxy_rule_t   *rule;         /**< 指向的代理规则（引用，不拥有） */
    pthread_mutex_t       *config_mutex;   /**< 保护配置访问 */
} cocoon_healthcheck_thread_t;

/**
 * cocoon_healthcheck_manager_t - 健康检查管理器
 */
typedef struct {
    cocoon_healthcheck_thread_t threads[COCOON_MAX_PROXY_RULES]; /**< 每个规则一个线程 */
    size_t count;                                                   /**< 当前线程数 */
} cocoon_healthcheck_manager_t;

/**
 * healthcheck_manager_init - 初始化管理器
 */
void healthcheck_manager_init(cocoon_healthcheck_manager_t *mgr);

/**
 * healthcheck_start - 为代理配置启动所有探测线程
 *
 * @param mgr 管理器
 * @param proxy_cfg 代理配置
 * @param global_timeout_ms 全局超时（作为默认值）
 * @return true 成功
 */
bool healthcheck_start(cocoon_healthcheck_manager_t *mgr,
                       cocoon_proxy_config_t *proxy_cfg,
                       uint32_t global_timeout_ms);

/**
 * healthcheck_stop - 停止所有探测线程并 join
 *
 * @param mgr 管理器
 */
void healthcheck_stop(cocoon_healthcheck_manager_t *mgr);

/**
 * healthcheck_probe_once - 对单个后端执行一次探测
 *
 * 用于测试或手动触发。
 *
 * @param backend 后端
 * @param timeout_ms 超时毫秒
 * @return true 健康
 */
bool healthcheck_probe_once(cocoon_proxy_backend_t *backend, uint32_t timeout_ms);

#endif /* COCOON_HEALTHCHECK_H */

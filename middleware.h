/**
 * middleware.h - 中间件框架接口
 *
 * 提供请求处理前后的 hook 系统，支持注册自定义中间件和内置中间件。
 * 中间件在请求到达业务逻辑之前执行，可以短路请求（直接返回响应）。
 *
 * 内置中间件：
 *   - CORS：处理 OPTIONS 预检请求
 *   - Basic Auth：HTTP Basic 认证
 *   - Rate Limit：基于 IP 的滑动窗口限流
 *
 * @author xfy
 */

#ifndef COCOON_MIDDLEWARE_H
#define COCOON_MIDDLEWARE_H

#include "http.h"
#include "platform.h"
#include <stdbool.h>
#include <stdint.h>

/* === 中间件配置 === */
/**
 * cocoon_middleware_config_t - 中间件相关配置
 *
 * 从 cocoon.json 或命令行解析出的中间件配置。
 */
typedef struct {
    bool        cors_enabled;      /**< 是否启用 CORS 支持 */
    const char *auth_user;         /**< Basic Auth 用户名（NULL 表示禁用） */
    const char *auth_pass;         /**< Basic Auth 密码 */
    uint32_t    rate_limit;        /**< 每秒最大请求数（0 表示禁用限流） */
} cocoon_middleware_config_t;

/* === 中间件函数类型 === */
/**
 * cocoon_middleware_func_t - 中间件函数签名
 *
 * 每个中间件接收解析后的 HTTP 请求和客户端 socket。
 * 返回 0 表示继续执行后续中间件和业务逻辑。
 * 返回非 0 表示中间件已处理请求（通常已发送响应），停止后续处理。
 *
 * @param req  HTTP 请求（可修改，但修改不影响已解析的缓冲区）
 * @param fd   客户端 socket（用于发送响应）
 * @param user_data  注册时传入的用户数据
 * @return 0 继续，非 0 短路
 */
typedef int (*cocoon_middleware_func_t)(http_request_t *req, cocoon_socket_t fd, void *user_data);

/* === 注册表 API === */

/**
 * cocoon_middleware_register - 注册一个中间件
 *
 * 按注册顺序执行。最多支持 16 个中间件。
 *
 * @param name      中间件名称（用于调试和注销）
 * @param func      中间件函数
 * @param user_data 用户数据（可为 NULL）
 * @return 0 成功，-1 注册表已满
 */
int cocoon_middleware_register(const char *name, cocoon_middleware_func_t func, void *user_data);

/**
 * cocoon_middleware_unregister - 注销指定名称的中间件
 *
 * @param name 中间件名称
 * @return 0 成功，-1 未找到
 */
int cocoon_middleware_unregister(const char *name);

/**
 * cocoon_middleware_run - 按顺序执行所有已注册的中间件
 *
 * 遇到第一个返回非 0 的中间件即停止。
 *
 * @param req HTTP 请求
 * @param fd  客户端 socket
 * @return 0 所有中间件通过，非 0 某个中间件短路了请求
 */
int cocoon_middleware_run(http_request_t *req, cocoon_socket_t fd);

/**
 * cocoon_middleware_cleanup - 清空注册表并释放资源
 */
void cocoon_middleware_cleanup(void);

/* === 内置中间件 === */

/**
 * cocoon_middleware_cors - CORS 预检请求中间件
 *
 * 如果请求方法是 OPTIONS，直接发送 204 No Content 并短路。
 * 正常请求返回 0 继续。
 *
 * 配合 cocoon.json 中 "cors_enabled": true 使用。
 * 响应头中的 CORS 头由 static.c / http.c 统一添加。
 *
 * @param req HTTP 请求
 * @param fd  客户端 socket
 * @param user_data 未使用（传 NULL）
 * @return 0 继续，1 短路（已发送 OPTIONS 响应）
 */
int cocoon_middleware_cors(http_request_t *req, cocoon_socket_t fd, void *user_data);

/**
 * cocoon_middleware_basic_auth - Basic HTTP 认证中间件
 *
 * 检查 Authorization: Basic 头。失败时发送 401 并短路。
 * 配置通过 cocoon_middleware_config_t 传入 user_data。
 *
 * @param req HTTP 请求
 * @param fd  客户端 socket
 * @param user_data 指向 cocoon_middleware_config_t 的指针
 * @return 0 认证通过，1 认证失败（已发送 401）
 */
int cocoon_middleware_basic_auth(http_request_t *req, cocoon_socket_t fd, void *user_data);

/**
 * cocoon_middleware_rate_limit - 基于 IP 的限流中间件
 *
 * 使用固定大小哈希表记录每个 IP 的请求频率。
 * 超过配置阈值时发送 429 Too Many Requests 并短路。
 *
 * @param req HTTP 请求
 * @param fd  客户端 socket
 * @param user_data 指向 cocoon_middleware_config_t 的指针（rate_limit 字段）
 * @return 0 未限流，1 已限流（已发送 429）
 */
int cocoon_middleware_rate_limit(http_request_t *req, cocoon_socket_t fd, void *user_data);

/**
 * cocoon_middleware_init_builtin - 根据配置初始化内置中间件
 *
 * 一键注册所有启用配置的内置中间件。
 *
 * @param config 中间件配置
 */
void cocoon_middleware_init_builtin(const cocoon_middleware_config_t *config);

#endif /* COCOON_MIDDLEWARE_H */

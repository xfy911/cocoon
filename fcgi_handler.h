/**
 * fcgi_handler.h - FastCGI 服务器集成模块
 *
 * 将 FastCGI 协议接入 Cocoon 请求处理流程，
 * 支持路径前缀匹配到 FastCGI 后端（PHP-FPM 等）。
 *
 * @author xfy
 */

#ifndef COCOON_FCGI_HANDLER_H
#define COCOON_FCGI_HANDLER_H

#include "cocoon.h"
#include "platform.h"
#include "http.h"
#include "fastcgi.h"
#include <stdbool.h>

/**
 * cocoon_fcgi_rule_t - FastCGI 路由规则
 *
 * 路径前缀匹配 + 后端连接池。
 */
typedef struct {
    char prefix[256];        /**< 路径前缀（如 "/api.php"） */
    fcgi_pool_t pool;        /**< 后端连接池 */
    fcgi_backend_t backend;  /**< 后端配置（内嵌在规则中） */
} cocoon_fcgi_rule_t;

/**
 * cocoon_fcgi_config_t - FastCGI 配置集合
 */
typedef struct {
    cocoon_fcgi_rule_t rules[COCOON_MAX_FASTCGI_RULES];
    size_t count;
} cocoon_fcgi_config_t;

/**
 * fcgi_handler_init - 初始化 FastCGI 处理器
 *
 * 根据服务器配置创建连接池。
 *
 * @param cfg 输出配置
 * @param config 服务器配置（含 fastcgi 数组）
 * @return true 成功
 */
bool fcgi_handler_init(cocoon_fcgi_config_t *cfg, const cocoon_config_t *config);

/**
 * fcgi_handler_destroy - 销毁 FastCGI 处理器
 *
 * 关闭所有连接池。
 *
 * @param cfg FastCGI 配置
 */
void fcgi_handler_destroy(cocoon_fcgi_config_t *cfg);

/**
 * fcgi_handler_match - 匹配路径到 FastCGI 规则
 *
 * @param cfg FastCGI 配置
 * @param path 请求路径
 * @return 匹配的规则，未匹配返回 NULL
 */
cocoon_fcgi_rule_t *fcgi_handler_match(cocoon_fcgi_config_t *cfg, const char *path);

/**
 * fcgi_handler_forward - 将 HTTP 请求转发到 FastCGI 后端
 *
 * 构建 CGI 环境变量，通过 FastCGI 连接池发送请求，
 * 将后端响应（含 HTTP 头）回写给客户端。
 *
 * @param client_fd 客户端 socket
 * @param req HTTP 请求
 * @param rule FastCGI 规则
 * @return true 保持连接，false 关闭
 */
bool fcgi_handler_forward(cocoon_socket_t client_fd, const http_request_t *req,
                          cocoon_fcgi_rule_t *rule);

#endif /* COCOON_FCGI_HANDLER_H */

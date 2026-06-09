/**
 * server.h - 服务器核心模块接口
 *
 * @author xfy
 */

#ifndef COCOON_SERVER_H
#define COCOON_SERVER_H

#include "cocoon.h"
#include <stdint.h>

/**
 * server_context_t - 服务器运行时上下文
 *
 * 包含监听 socket、配置、运行状态等。
 */
typedef struct server_context server_context_t;

/**
 * server_request_reload - 请求配置热重载
 *
 * 设置热重载标志，由 accept_loop 在下次迭代时执行。
 * 安全地从信号处理器中调用。
 *
 * @param ctx 服务器上下文
 */
void server_request_reload(server_context_t *ctx);

/**
 * server_reload_config - 热重载配置
 *
 * 重新加载配置文件，应用可热重载的配置项。
 * 不可热重载的项（端口、线程数等）如有变化会日志提示。
 *
 * @param ctx 服务器上下文
 */
void server_reload_config(server_context_t *ctx);

/**
 * server_start - 启动服务器（阻塞直到 stop 被调用）
 *
 * @param ctx 服务器上下文
 * @return COCOON_OK 成功，负值错误码
 */
int server_start(server_context_t *ctx);

/**
 * server_stop - 请求服务器停止
 *
 * 设置停止标志，等待当前连接处理完毕后退出。
 *
 * @param ctx 服务器上下文
 */
void server_stop(server_context_t *ctx);

/**
 * server_create - 创建服务器上下文
 *
 * @param config 配置指针
 * @param config_file_path 配置文件路径（用于热重载），可为 NULL
 * @return 服务器上下文，失败返回 NULL
 */
server_context_t *server_create(const cocoon_config_t *config, const char *config_file_path);

/**
 * server_destroy - 销毁服务器上下文
 *
 * @param ctx 服务器上下文
 */
void server_destroy(server_context_t *ctx);

#endif /* COCOON_SERVER_H */

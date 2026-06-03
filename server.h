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
 * @return 服务器上下文，失败返回 NULL
 */
server_context_t *server_create(const cocoon_config_t *config);

/**
 * server_destroy - 销毁服务器上下文
 *
 * @param ctx 服务器上下文
 */
void server_destroy(server_context_t *ctx);

#endif /* COCOON_SERVER_H */

/**
 * cocoon.h - Cocoon 公共头文件
 *
 * 定义错误码、配置结构体、函数声明。
 *
 * @author xfy
 */

#ifndef COCOON_H
#define COCOON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "log.h"

#define COCOON_OK           0   /**< 成功 */
#define COCOON_ERROR       -1   /**< 通用错误 */
#define COCOON_NOMEM       -2   /**< 内存不足 */
#define COCOON_NOTFOUND    -3   /**< 文件未找到 */
#define COCOON_FORBIDDEN    -4   /**< 禁止访问 */
#define COCOON_BADREQUEST  -5   /**< 请求格式错误 */

/* === 服务器配置 === */
/**
 * cocoon_config - 服务器配置结构体
 *
 * 命令行参数解析后的配置。
 */#define COCOON_MAX_PLUGINS 8   /**< 最大插件数量 */
#define COCOON_MAX_PROXY_RULES 8 /**< 最大代理规则数量 */

typedef struct cocoon_config {
    const char *root_dir;       /**< 静态资源根目录 */
    uint16_t    port;           /**< 监听端口 */
    bool        threaded;       /**< 是否启用多线程调度 */
    uint32_t    num_workers;    /**< 工作线程数（0 = 自动检测） */
    uint32_t    max_connections; /**< 最大并发连接数（0 = 无限制） */
    uint32_t    timeout_ms;      /**< 连接空闲超时毫秒（0 = 默认30000） */
    log_level_t log_level;       /**< 日志级别 */
    bool        gzip_enabled;    /**< 是否启用 gzip 压缩（默认 true） */
    bool        brotli_enabled;   /**< 是否启用 brotli 压缩（默认 true） */
    bool        tls_enabled;     /**< 是否启用 TLS（由 cert/key 自动推断） */
    const char *tls_cert;        /**< TLS 证书路径 */
    const char *tls_key;         /**< TLS 私钥路径 */
    const char *access_log_path; /**< 访问日志文件路径（NULL 或 "-" 表示 stdout） */
    /* 中间件配置 */
    bool        cors_enabled;      /**< 是否启用 CORS 支持 */
    const char *auth_user;         /**< Basic Auth 用户名 */
    const char *auth_pass;         /**< Basic Auth 密码 */
    uint32_t    rate_limit;        /**< 每秒最大请求数（0 表示禁用） */
    /* 插件配置 */
    const char *plugins[COCOON_MAX_PLUGINS]; /**< 插件路径列表 */
    size_t      num_plugins;                   /**< 插件数量 */
    /* 代理配置 */
    struct {
        char prefix[256];
        char target[256];
        uint32_t pool_size;      /* 连接池大小（默认4，最大16） */
        uint32_t weight;         /* 权重（默认1） */
    } proxies[COCOON_MAX_PROXY_RULES];
    size_t num_proxies;
} cocoon_config_t;

/* === 服务器生命周期 API === */
/**
 * cocoon_server_create - 创建服务器实例
 *
 * @param config 配置指针
 * @return 服务器句柄，失败返回 NULL
 */
void *cocoon_server_create(const cocoon_config_t *config);

/**
 * cocoon_server_run - 启动服务器（阻塞）
 *
 * @param server 服务器句柄
 * @return COCOON_OK 成功，负值错误码
 */
int cocoon_server_run(void *server);

/**
 * cocoon_server_stop - 优雅关闭服务器
 *
 * @param server 服务器句柄
 */
void cocoon_server_stop(void *server);

/**
 * cocoon_server_destroy - 销毁服务器实例
 *
 * @param server 服务器句柄
 */
void cocoon_server_destroy(void *server);

#endif /* COCOON_H */

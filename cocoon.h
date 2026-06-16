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

/**
 * cocoon_healthcheck_config_t - 健康检查配置
 */
typedef struct {
    char path[256];         /* 探测路径（默认 /health） */
    uint32_t interval_ms;   /* 探测间隔（毫秒，默认5000） */
    uint32_t timeout_ms;    /* 探测超时（毫秒，默认2000） */
    bool enabled;           /* 是否启用（默认 false） */
} cocoon_healthcheck_config_t;

/* === 服务器配置 === */
/**
 * cocoon_config - 服务器配置结构体
 *
 * 命令行参数解析后的配置。
 */#define COCOON_MAX_PLUGINS 8   /**< 最大插件数量 */
#define COCOON_MAX_PROXY_RULES 8 /**< 最大代理规则数量 */
#define COCOON_MAX_VHOSTS 8      /**< 最大虚拟主机数量 */

/**
 * cocoon_vhost_t - 虚拟主机配置
 *
 * 根据请求的 Host 头匹配不同的根目录，实现多站点托管。
 */
typedef struct {
    char server_name[256];      /**< 域名匹配（如 "example.com" 或 "*.example.com"） */
    char root_dir[512];         /**< 该虚拟主机的静态资源根目录 */
} cocoon_vhost_t;

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
        /* 主动健康检查 */
        cocoon_healthcheck_config_t healthcheck;
    } proxies[COCOON_MAX_PROXY_RULES];
    size_t num_proxies;
    /* 虚拟主机 */
    cocoon_vhost_t vhosts[COCOON_MAX_VHOSTS];
    size_t num_vhosts;
    /* FastCGI 配置 */
#define COCOON_MAX_FASTCGI_RULES 4
    struct {
        char prefix[256];      /**< 路径前缀匹配 */
        char host[256];        /**< 后端主机或 Unix socket 路径 */
        int port;              /**< 端口（TCP 时有效） */
        bool is_unix_socket;   /**< 是否为 Unix domain socket */
        int pool_size;         /**< 连接池大小 */
        int timeout_ms;        /**< 请求超时 */
    } fastcgi[COCOON_MAX_FASTCGI_RULES];
    size_t num_fastcgi;
    /* 内存缓存配置 */
    bool        cache_enabled;       /**< 是否启用内存缓存（默认 false） */
    size_t      cache_max_size;      /**< 最大缓存总容量（字节，默认 64MB） */
    uint32_t    cache_ttl_seconds;   /**< 缓存 TTL 秒数（默认 60） */
    size_t      cache_max_entry_size; /**< 单条缓存最大大小（字节，默认 1MB） */
    /* ACME 自动证书配置 */
    bool        acme_enabled;        /**< 是否启用 ACME 自动证书（默认 false） */
    char        acme_directory_url[512]; /**< ACME 目录 URL（默认 Let's Encrypt 生产环境） */
    char        acme_email[256];     /**< ACME 账户邮箱 */
    char        acme_domains[8][256]; /**< 需要签发证书的域名列表 */
    size_t      acme_num_domains;    /**< 域名数量 */
    char        acme_cert_path[512]; /**< 证书保存路径 */
    char        acme_key_path[512];  /**< 私钥保存路径 */
    uint32_t    acme_renew_days;     /**< 到期前 N 天自动续期（默认 30） */
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

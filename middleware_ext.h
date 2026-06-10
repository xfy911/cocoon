/**
 * @file middleware_ext.h - 扩展内置中间件接口
 *
 * Phase 4 第一项：扩展中间件系统，新增 4 个内置中间件
 *   - JWT 认证中间件 (middleware_jwt)
 *   - Security Headers 中间件 (middleware_security_headers)
 *   - Request ID 中间件 (middleware_request_id)
 *   - IP 过滤中间件 (middleware_ip_filter)
 *
 * @author xfy
 */

#ifndef COCOON_MIDDLEWARE_EXT_H
#define COCOON_MIDDLEWARE_EXT_H

#include "middleware.h"
#include "platform.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== JWT 认证中间件 ===== */

/** JWT 密钥最大长度 */
#define COCOON_JWT_SECRET_MAX 256

/** JWT Token 最大长度 */
#define COCOON_JWT_TOKEN_MAX 2048

/**
 * @brief JWT 中间件配置
 *
 * 用于 cocoon_middleware_jwt() 的用户数据。
 */
typedef struct {
    char     secret[COCOON_JWT_SECRET_MAX]; /**< JWT 签名密钥 */
    char     header_name[64];               /**< 默认 "Authorization" */
    char     prefix[16];                    /**< 默认 "Bearer " */
    uint32_t max_age;                       /**< Token 最大有效期（秒，默认 3600） */
    bool     skip_preflight;                /**< 是否跳过 OPTIONS 预检请求 */
} cocoon_jwt_config_t;

/**
 * @brief JWT 认证中间件
 *
 * 验证 Authorization: Bearer <token> 中的 JWT token。
 * 使用 HS256 (HMAC-SHA256) 签名验证。
 * Token 格式：header.payload.signature（Base64Url 编码）
 *
 * @param req       HTTP 请求
 * @param fd        客户端 socket
 * @param user_data 指向 cocoon_jwt_config_t 的指针
 * @return 0 验证通过继续处理，1 验证失败（已发送 401）
 */
int cocoon_middleware_jwt(http_request_t *req, cocoon_socket_t fd, void *user_data);

/* ===== Security Headers 中间件 ===== */

/**
 * @brief Security Headers 全局配置
 *
 * 由中间件设置，server.c 发送响应前读取。
 */
typedef struct {
    bool     hsts_enabled;           /**< Strict-Transport-Security */
    uint32_t hsts_max_age;           /**< max-age（秒，默认 31536000 = 1年） */
    bool     hsts_include_subdomains;/**< includeSubDomains */
    bool     frame_options_enabled;  /**< X-Frame-Options */
    char     frame_options[32];      /**< DENY 或 SAMEORIGIN（默认 DENY） */
    bool     xss_protection_enabled; /**< X-XSS-Protection */
    bool     csp_enabled;            /**< Content-Security-Policy */
    char     csp_policy[512];        /**< CSP 策略字符串 */
    bool     content_type_options;   /**< X-Content-Type-Options: nosniff */
    bool     referrer_policy_enabled;/**< Referrer-Policy */
    char     referrer_policy[32];    /**< strict-origin-when-cross-origin */
} cocoon_security_headers_config_t;

/**
 * @brief Security Headers 中间件
 *
 * 设置全局安全头配置。中间件本身返回 0（继续处理），
 * 由 server.c 在发送响应时调用 cocoon_middleware_security_headers_get() 获取配置。
 *
 * @param req       HTTP 请求
 * @param fd        客户端 socket（未使用）
 * @param user_data 指向 cocoon_security_headers_config_t 的指针
 * @return 始终返回 0（继续处理）
 */
int cocoon_middleware_security_headers(http_request_t *req, cocoon_socket_t fd, void *user_data);

/**
 * @brief 获取当前 Security Headers 配置
 *
 * 供 server.c 在发送响应前调用，将安全头追加到响应中。
 *
 * @return 指向当前全局配置的指针，未设置时返回 NULL
 */
const cocoon_security_headers_config_t *cocoon_middleware_security_headers_get(void);

/* ===== Request ID 中间件 ===== */

/** Request ID 长度（32 字符 hex） */
#define COCOON_REQUEST_ID_LEN 32

/**
 * @brief Request ID 中间件配置
 */
typedef struct {
    char  header_name[32];  /**< 默认 "X-Request-ID" */
    bool  trust_incoming;   /**< 是否信任客户端传入的 ID（默认 true） */
} cocoon_request_id_config_t;

/**
 * @brief Request ID 中间件
 *
 * 生成 32 字符 hex 唯一追踪 ID。
 * 如果 trust_incoming=true 且请求头已有 ID，则复用。
 * 将 ID 添加到响应头 X-Request-ID。
 *
 * @param req       HTTP 请求
 * @param fd        客户端 socket
 * @param user_data 指向 cocoon_request_id_config_t 的指针
 * @return 始终返回 0（继续处理）
 */
int cocoon_middleware_request_id(http_request_t *req, cocoon_socket_t fd, void *user_data);

/* ===== IP 过滤中间件 ===== */

/** IP 过滤列表最大条目数 */
#define COCOON_IP_FILTER_MAX 64

/**
 * @brief IP 过滤模式
 */
typedef enum {
    COCOON_IP_FILTER_DENY,  /**< 黑名单模式：匹配则拒绝 */
    COCOON_IP_FILTER_ALLOW  /**< 白名单模式：不匹配则拒绝 */
} cocoon_ip_filter_mode_t;

/**
 * @brief IP 过滤中间件配置
 *
 * 支持 IPv4 地址和 CIDR（如 "192.168.1.0/24"）。
 */
typedef struct {
    char entries[COCOON_IP_FILTER_MAX][64]; /**< IP 或 CIDR 字符串 */
    size_t count;                           /**< 实际条目数 */
    cocoon_ip_filter_mode_t mode;           /**< 黑名单或白名单模式 */
    char deny_message[256];                 /**< 拒绝时的响应消息 */
} cocoon_ip_filter_config_t;

/**
 * @brief IP 过滤中间件
 *
 * 黑名单模式：匹配则返回 403
 * 白名单模式：不匹配则返回 403
 * 支持解析 X-Forwarded-For 头获取真实 IP。
 *
 * @param req       HTTP 请求
 * @param fd        客户端 socket
 * @param user_data 指向 cocoon_ip_filter_config_t 的指针
 * @return 0 允许继续处理，1 已拒绝（已发送 403）
 */
int cocoon_middleware_ip_filter(http_request_t *req, cocoon_socket_t fd, void *user_data);

/* ===== 一键初始化 ===== */

/**
 * @brief 一键注册所有扩展中间件
 *
 * 根据配置注册 Phase 4 新增的 4 个内置中间件。
 *
 * @param server_config 服务器配置（可包含中间件配置）
 */
void cocoon_middleware_init_extended(void *server_config);

#ifdef __cplusplus
}
#endif

#endif /* COCOON_MIDDLEWARE_EXT_H */

/**
 * acme.h - ACME / Let's Encrypt 自动证书模块接口
 *
 * 实现 RFC 8555 ACME 协议客户端，支持 HTTP-01 挑战。
 * 自动完成证书签发和续期，无需手动配置。
 *
 * 设计:
 *   - 独立 ACME 客户端上下文，管理账户、订单、证书
 *   - JWS 签名使用 OpenSSL EVP 接口
 *   - HTTP-01 挑战响应通过回调集成到服务器
 *   - 后台协程定时检查证书过期并自动续期
 *
 * @author xfy
 */

#ifndef COCOON_ACME_H
#define COCOON_ACME_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* ACME 目录端点结构 */
typedef struct {
    char newNonce[512];
    char newAccount[512];
    char newOrder[512];
    char revokeCert[512];
    char keyChange[512];
} acme_directory_t;

/* ACME 账户信息 */
typedef struct {
    char kid[512];          /* 账户 URL (Key ID) */
    char contact[256];      /* 联系邮箱 */
    bool termsAgreed;       /* 是否同意服务条款 */
} acme_account_t;

/* ACME 授权 */
typedef struct {
    char domain[256];       /* 授权域名 */
    char status[32];        /* pending / valid / invalid / deactivated / expired / revoked */
    char token[256];        /* HTTP-01 挑战 token */
    char uri[512];          /* 授权 URI */
} acme_authz_t;

/* ACME 订单 */
typedef struct {
    char uri[512];          /* 订单 URI */
    char status[32];        /* pending / ready / processing / valid / invalid */
    char finalize[512];     /* finalize 端点 */
    char certificate[512];  /* 证书下载端点 */
    time_t expires;         /* 过期时间 */
    acme_authz_t *authz;    /* 授权数组 */
    size_t num_authz;       /* 授权数量 */
} acme_order_t;

/* ACME 客户端上下文 */
typedef struct acme_ctx acme_ctx_t;

/**
 * acme_create - 创建 ACME 客户端上下文
 *
 * @param directory_url ACME 目录 URL（如 Let's Encrypt 生产环境或测试环境）
 * @param account_key_pem 账户私钥 PEM 字符串（EC P-256），NULL 则自动生成
 * @return ACME 上下文，失败返回 NULL
 */
acme_ctx_t *acme_create(const char *directory_url, const char *account_key_pem);

/**
 * acme_destroy - 销毁 ACME 客户端上下文
 *
 * @param ctx ACME 上下文
 */
void acme_destroy(acme_ctx_t *ctx);

/**
 * acme_get_directory - 获取 ACME 服务器目录
 *
 * 向 directory_url 发送 GET 请求，解析返回的 JSON 目录。
 *
 * @param ctx ACME 上下文
 * @param out 输出目录结构
 * @return 0 成功，-1 失败
 */
int acme_get_directory(acme_ctx_t *ctx, acme_directory_t *out);

/**
 * acme_get_nonce - 获取新的 nonce
 *
 * ACME 协议要求每个 POST 请求都包含一个 Replay-Nonce。
 *
 * @param ctx ACME 上下文
 * @param nonce_url newNonce 端点 URL
 * @return 0 成功，-1 失败
 */
int acme_get_nonce(acme_ctx_t *ctx, const char *nonce_url);

/**
 * acme_create_account - 创建或查找 ACME 账户
 *
 * 如果账户已存在（服务器返回 200），则复用现有账户。
 *
 * @param ctx ACME 上下文
 * @param email 联系邮箱（如 "admin@example.com"）
 * @param terms_agreed 是否同意服务条款
 * @return 0 成功，-1 失败
 */
int acme_create_account(acme_ctx_t *ctx, const char *email, bool terms_agreed);

/**
 * acme_create_order - 创建证书订单
 *
 * @param ctx ACME 上下文
 * @param domains 域名数组
 * @param num_domains 域名数量
 * @param out 输出订单结构（调用者负责释放 out->authz）
 * @return 0 成功，-1 失败
 */
int acme_create_order(acme_ctx_t *ctx, const char **domains, size_t num_domains,
                      acme_order_t *out);

/**
 * acme_order_free - 释放订单结构中的动态内存
 *
 * @param order 订单结构
 */
void acme_order_free(acme_order_t *order);

/**
 * acme_fetch_authz - 获取授权详情和挑战
 *
 * @param ctx ACME 上下文
 * @param authz 授权结构（输入 uri，输出 token 等）
 * @return 0 成功，-1 失败
 */
int acme_fetch_authz(acme_ctx_t *ctx, acme_authz_t *authz);

/**
 * acme_respond_challenge - 响应 HTTP-01 挑战
 *
 * 通知 ACME 服务器可以验证挑战了。
 *
 * @param ctx ACME 上下文
 * @param authz 授权结构
 * @return 0 成功，-1 失败
 */
int acme_respond_challenge(acme_ctx_t *ctx, const acme_authz_t *authz);

/**
 * acme_poll_authz - 轮询授权状态直到完成或失败
 *
 * @param ctx ACME 上下文
 * @param authz 授权结构
 * @param timeout_ms 超时毫秒
 * @return 0 成功（valid），-1 失败
 */
int acme_poll_authz(acme_ctx_t *ctx, acme_authz_t *authz, int timeout_ms);

/**
 * acme_finalize_order - 发送 CSR 完成订单
 *
 * @param ctx ACME 上下文
 * @param order 订单结构
 * @param domains 域名数组
 * @param num_domains 域名数量
 * @return 0 成功，-1 失败
 */
int acme_finalize_order(acme_ctx_t *ctx, acme_order_t *order,
                        const char **domains, size_t num_domains);

/**
 * acme_poll_order - 轮询订单状态直到证书就绪
 *
 * @param ctx ACME 上下文
 * @param order 订单结构
 * @param timeout_ms 超时毫秒
 * @return 0 成功（valid），-1 失败
 */
int acme_poll_order(acme_ctx_t *ctx, acme_order_t *order, int timeout_ms);

/**
 * acme_download_certificate - 下载签发的证书链
 *
 * @param ctx ACME 上下文
 * @param cert_url 证书下载 URL
 * @param out_pem 输出 PEM 证书链（调用者负责 free）
 * @return 0 成功，-1 失败
 */
int acme_download_certificate(acme_ctx_t *ctx, const char *cert_url, char **out_pem);

/**
 * acme_get_thumbprint - 获取账户密钥的 JWK Thumbprint
 *
 * HTTP-01 挑战响应内容 = token + "." + base64url(thumbprint)
 *
 * @param ctx ACME 上下文
 * @param out 输出 thumbprint（base64url 编码，调用者负责 free）
 * @return 0 成功，-1 失败
 */
int acme_get_thumbprint(acme_ctx_t *ctx, char **out);

/**
 * acme_issue_certificate - 一键签发证书（完整流程）
 *
 * 封装目录发现 → 创建账户 → 创建订单 → 挑战 → 下载证书 的完整流程。
 * 此函数会阻塞，需要在协程中调用。
 *
 * @param ctx ACME 上下文
 * @param domains 域名数组
 * @param num_domains 域名数量
 * @param email 联系邮箱
 * @param cert_pem 输出证书 PEM（调用者负责 free）
 * @param key_pem 输出私钥 PEM（调用者负责 free）
 * @return 0 成功，-1 失败
 */
int acme_issue_certificate(acme_ctx_t *ctx, const char **domains, size_t num_domains,
                           const char *email, char **cert_pem, char **key_pem);

/* HTTP 请求辅助（内部使用，暴露给单元测试） */
typedef struct {
    int status;
    char *body;
    size_t body_len;
    char location[512];
    char replay_nonce[256];
} acme_http_response_t;

/**
 * acme_http_post_jws - 发送带 JWS 签名的 POST 请求
 *
 * @param ctx ACME 上下文
 * @param url 请求 URL
 * @param payload JSON payload（可为 NULL 表示空）
 * @param out 输出响应
 * @return 0 成功，-1 失败
 */
int acme_http_post_jws(acme_ctx_t *ctx, const char *url, const char *payload,
                       acme_http_response_t *out);

/**
 * acme_http_get - 发送 GET 请求
 *
 * @param ctx ACME 上下文
 * @param url 请求 URL
 * @param out 输出响应
 * @return 0 成功，-1 失败
 */
int acme_http_get(acme_ctx_t *ctx, const char *url, acme_http_response_t *out);

/**
 * acme_http_response_free - 释放 HTTP 响应资源
 *
 * @param resp 响应结构
 */
void acme_http_response_free(acme_http_response_t *resp);

#endif /* COCOON_ACME_H */

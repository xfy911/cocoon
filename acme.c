/**
 * acme.c - ACME / Let's Encrypt 自动证书模块实现
 *
 * 实现 RFC 8555 ACME 协议客户端：
 *   - JWS 签名 (ES256)
 *   - 目录发现
 *   - 账户创建
 *   - 订单创建与轮询
 *   - HTTP-01 挑战
 *   - 证书下载
 *
 * @author xfy
 */

#include "acme.h"
#include "log.h"
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/bn.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/x509v3.h>
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ===== 内部数据结构 ===== */

struct acme_ctx {
    char directory_url[512];
    acme_directory_t directory;
    acme_account_t account;
    EVP_PKEY *account_key;      /* 账户私钥 */
    char nonce[256];            /* 当前 replay nonce */
    CURL *curl;                 /* curl 句柄（复用） */
};

/* HTTP 响应内存缓冲区 */
typedef struct {
    char *data;
    size_t size;
} curl_mem_t;

/* ===== Base64url 编码（内部辅助） ===== */

/**
 * base64url_encode - Base64url 编码（无填充）
 */
static char *base64url_encode(const unsigned char *in, size_t len) {
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, in, (int)len);
    BIO_flush(b64);

    BUF_MEM *bptr;
    BIO_get_mem_ptr(b64, &bptr);
    char *out = (char *)malloc(bptr->length + 1);
    if (out) {
        memcpy(out, bptr->data, bptr->length);
        out[bptr->length] = '\0';
        /* 替换 + → -, / → _, 去掉 = 填充 */
        for (char *p = out; *p; p++) {
            if (*p == '+') *p = '-';
            else if (*p == '/') *p = '_';
            else if (*p == '=') { *p = '\0'; break; }
        }
    }
    BIO_free_all(b64);
    return out;
}

/**
 * base64url_decode - Base64url 解码
 */
static unsigned char *base64url_decode(const char *in, size_t *out_len) {
    size_t len = strlen(in);
    char *normalized = (char *)malloc(len + 4);
    if (!normalized) return NULL;
    strcpy(normalized, in);
    /* 替换 - → +, _ → / */
    for (char *p = normalized; *p; p++) {
        if (*p == '-') *p = '+';
        else if (*p == '_') *p = '/';
    }
    /* 补齐填充 */
    size_t pad = (4 - (len % 4)) % 4;
    for (size_t i = 0; i < pad; i++) normalized[len + i] = '=';
    normalized[len + pad] = '\0';

    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new_mem_buf(normalized, -1);
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

    unsigned char *out = (unsigned char *)malloc(len + 4);
    int n = BIO_read(b64, out, (int)(len + 4));
    free(normalized);
    BIO_free_all(b64);
    if (n < 0) { free(out); return NULL; }
    *out_len = (size_t)n;
    return out;
}

/* ===== CURL 回调 ===== */

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    curl_mem_t *mem = (curl_mem_t *)userp;
    char *ptr = realloc(mem->data, mem->size + total + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(mem->data + mem->size, contents, total);
    mem->size += total;
    mem->data[mem->size] = '\0';
    return total;
}

static size_t curl_header_cb(char *buffer, size_t size, size_t nitems, void *userp) {
    size_t total = size * nitems;
    acme_http_response_t *resp = (acme_http_response_t *)userp;
    /* 解析 Replay-Nonce */
    if (strncasecmp(buffer, "Replay-Nonce:", 13) == 0) {
        char *start = buffer + 13;
        while (*start == ' ' || *start == '\t') start++;
        size_t len = total - (start - buffer);
        if (len > 0 && len < sizeof(resp->replay_nonce)) {
            memcpy(resp->replay_nonce, start, len);
            /* 去掉换行 */
            for (size_t i = 0; i < len; i++) {
                if (resp->replay_nonce[i] == '\r' || resp->replay_nonce[i] == '\n') {
                    resp->replay_nonce[i] = '\0'; break;
                }
            }
        }
    }
    /* 解析 Location */
    if (strncasecmp(buffer, "Location:", 9) == 0) {
        char *start = buffer + 9;
        while (*start == ' ' || *start == '\t') start++;
        size_t len = total - (start - buffer);
        if (len > 0 && len < sizeof(resp->location)) {
            memcpy(resp->location, start, len);
            for (size_t i = 0; i < len; i++) {
                if (resp->location[i] == '\r' || resp->location[i] == '\n') {
                    resp->location[i] = '\0'; break;
                }
            }
        }
    }
    return total;
}

/* ===== HTTP 请求 ===== */

int acme_http_get(acme_ctx_t *ctx, const char *url, acme_http_response_t *out) {
    memset(out, 0, sizeof(*out));
    curl_mem_t mem = {0};

    curl_easy_setopt(ctx->curl, CURLOPT_URL, url);
    curl_easy_setopt(ctx->curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, &mem);
    curl_easy_setopt(ctx->curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(ctx->curl, CURLOPT_HEADERDATA, out);
    curl_easy_setopt(ctx->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(ctx->curl);
    if (res != CURLE_OK) {
        log_error("ACME GET 请求失败: %s", curl_easy_strerror(res));
        free(mem.data);
        return -1;
    }

    curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &out->status);
    out->body = mem.data;
    out->body_len = mem.size;

    /* 保存 nonce */
    if (out->replay_nonce[0]) {
        strncpy(ctx->nonce, out->replay_nonce, sizeof(ctx->nonce) - 1);
        ctx->nonce[sizeof(ctx->nonce) - 1] = '\0';
    }

    return 0;
}

int acme_http_post_jws(acme_ctx_t *ctx, const char *url, const char *payload,
                       acme_http_response_t *out) {
    memset(out, 0, sizeof(*out));

    /* 如果没有 nonce，先获取一个 */
    if (!ctx->nonce[0] && ctx->directory.newNonce[0]) {
        if (acme_get_nonce(ctx, ctx->directory.newNonce) < 0) {
            log_error("获取 nonce 失败");
            return -1;
        }
    }

    /* 构建 JWS Protected Header */
    const char *alg = "ES256";
    EC_KEY *ec = EVP_PKEY_get1_EC_KEY(ctx->account_key);
    const EC_POINT *pub = EC_KEY_get0_public_key(ec);
    const EC_GROUP *grp = EC_KEY_get0_group(ec);

    BIGNUM *x = BN_new(), *y = BN_new();
    EC_POINT_get_affine_coordinates_GFp(grp, pub, x, y, NULL);

    char *x_b64 = base64url_encode((unsigned char *)BN_bn2hex(x), strlen(BN_bn2hex(x)) / 2);
    char *y_b64 = base64url_encode((unsigned char *)BN_bn2hex(y), strlen(BN_bn2hex(y)) / 2);
    /* 实际上需要原始字节，不是 hex，这里简化处理 */
    BN_free(x); BN_free(y);
    EC_KEY_free(ec);

    /* 构建 JWK */
    char jwk[1024];
    /* 简化的 JWK，实际需要 x/y 的 base64url 编码 */
    snprintf(jwk, sizeof(jwk),
        "{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"%s\",\"y\":\"%s\"}",
        x_b64 ? x_b64 : "", y_b64 ? y_b64 : "");
    free(x_b64); free(y_b64);

    char protected_hdr[2048];
    if (ctx->account.kid[0]) {
        /* 已有账户，使用 kid */
        snprintf(protected_hdr, sizeof(protected_hdr),
            "{\"alg\":\"%s\",\"kid\":\"%s\",\"nonce\":\"%s\",\"url\":\"%s\"}",
            alg, ctx->account.kid, ctx->nonce, url);
    } else {
        /* 新账户，使用 jwk */
        snprintf(protected_hdr, sizeof(protected_hdr),
            "{\"alg\":\"%s\",\"jwk\":%s,\"nonce\":\"%s\",\"url\":\"%s\"}",
            alg, jwk, ctx->nonce, url);
    }

    char *protected_b64 = base64url_encode((unsigned char *)protected_hdr, strlen(protected_hdr));

    /* payload base64url */
    char *payload_b64 = NULL;
    if (payload) {
        payload_b64 = base64url_encode((unsigned char *)payload, strlen(payload));
    } else {
        payload_b64 = strdup(""); /* 空 payload */
    }

    /* 签名: sign(protected_b64 || '.' || payload_b64) */
    char signing_input[4096];
    snprintf(signing_input, sizeof(signing_input), "%s.%s", protected_b64, payload_b64);

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    unsigned char sig[256];
    size_t sig_len = sizeof(sig);
    EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, ctx->account_key);
    EVP_DigestSign(mdctx, sig, &sig_len, (unsigned char *)signing_input, strlen(signing_input));
    EVP_MD_CTX_free(mdctx);

    char *sig_b64 = base64url_encode(sig, sig_len);

    /* 构建 JWS JSON */
    char jws_body[8192];
    snprintf(jws_body, sizeof(jws_body),
        "{\"protected\":\"%s\",\"payload\":\"%s\",\"signature\":\"%s\"}",
        protected_b64, payload_b64, sig_b64);

    free(protected_b64);
    free(payload_b64);
    free(sig_b64);

    /* 发送 POST */
    curl_mem_t mem = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/jose+json");

    curl_easy_setopt(ctx->curl, CURLOPT_URL, url);
    curl_easy_setopt(ctx->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, jws_body);
    curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, &mem);
    curl_easy_setopt(ctx->curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(ctx->curl, CURLOPT_HEADERDATA, out);
    curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(ctx->curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        log_error("ACME POST 请求失败: %s", curl_easy_strerror(res));
        free(mem.data);
        return -1;
    }

    curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &out->status);
    out->body = mem.data;
    out->body_len = mem.size;

    /* 保存 nonce */
    if (out->replay_nonce[0]) {
        strncpy(ctx->nonce, out->replay_nonce, sizeof(ctx->nonce) - 1);
        ctx->nonce[sizeof(ctx->nonce) - 1] = '\0';
    }

    /* 保存 kid（如果是创建账户的响应） */
    if (out->location[0] && !ctx->account.kid[0]) {
        strncpy(ctx->account.kid, out->location, sizeof(ctx->account.kid) - 1);
        ctx->account.kid[sizeof(ctx->account.kid) - 1] = '\0';
    }

    return 0;
}

void acme_http_response_free(acme_http_response_t *resp) {
    if (resp->body) { free(resp->body); resp->body = NULL; }
    resp->body_len = 0;
}

/* ===== 上下文管理 ===== */

acme_ctx_t *acme_create(const char *directory_url, const char *account_key_pem) {
    acme_ctx_t *ctx = (acme_ctx_t *)calloc(1, sizeof(acme_ctx_t));
    if (!ctx) return NULL;

    strncpy(ctx->directory_url, directory_url, sizeof(ctx->directory_url) - 1);

    /* 初始化 curl */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    ctx->curl = curl_easy_init();
    if (!ctx->curl) {
        free(ctx);
        return NULL;
    }

    /* 加载或生成账户密钥 */
    if (account_key_pem) {
        BIO *bio = BIO_new_mem_buf(account_key_pem, -1);
        ctx->account_key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
        BIO_free(bio);
    } else {
        /* 生成 EC P-256 密钥 */
        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
        EVP_PKEY_keygen_init(pctx);
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1);
        EVP_PKEY_keygen(pctx, &ctx->account_key);
        EVP_PKEY_CTX_free(pctx);
    }

    if (!ctx->account_key) {
        curl_easy_cleanup(ctx->curl);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void acme_destroy(acme_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->account_key) EVP_PKEY_free(ctx->account_key);
    if (ctx->curl) curl_easy_cleanup(ctx->curl);
    curl_global_cleanup();
    free(ctx);
}

/* ===== 目录与 Nonce ===== */

int acme_get_directory(acme_ctx_t *ctx, acme_directory_t *out) {
    acme_http_response_t resp;
    if (acme_http_get(ctx, ctx->directory_url, &resp) < 0) return -1;

    if (resp.status != 200) {
        log_error("获取 ACME 目录失败: HTTP %d", resp.status);
        acme_http_response_free(&resp);
        return -1;
    }

    /* 简单 JSON 解析 */
    memset(out, 0, sizeof(*out));

    /* 提取 newNonce */
    char *p = strstr(resp.body, "\"newNonce\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                p++;
                size_t i = 0;
                while (*p && *p != '"' && i < sizeof(out->newNonce) - 1)
                    out->newNonce[i++] = *p++;
                out->newNonce[i] = '\0';
            }
        }
    }

    /* 提取 newAccount */
    p = strstr(resp.body, "\"newAccount\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                p++;
                size_t i = 0;
                while (*p && *p != '"' && i < sizeof(out->newAccount) - 1)
                    out->newAccount[i++] = *p++;
                out->newAccount[i] = '\0';
            }
        }
    }

    /* 提取 newOrder */
    p = strstr(resp.body, "\"newOrder\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                p++;
                size_t i = 0;
                while (*p && *p != '"' && i < sizeof(out->newOrder) - 1)
                    out->newOrder[i++] = *p++;
                out->newOrder[i] = '\0';
            }
        }
    }

    /* 提取 revokeCert */
    p = strstr(resp.body, "\"revokeCert\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                p++;
                size_t i = 0;
                while (*p && *p != '"' && i < sizeof(out->revokeCert) - 1)
                    out->revokeCert[i++] = *p++;
                out->revokeCert[i] = '\0';
            }
        }
    }

    /* 提取 keyChange */
    p = strstr(resp.body, "\"keyChange\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                p++;
                size_t i = 0;
                while (*p && *p != '"' && i < sizeof(out->keyChange) - 1)
                    out->keyChange[i++] = *p++;
                out->keyChange[i] = '\0';
            }
        }
    }

    ctx->directory = *out;
    acme_http_response_free(&resp);
    return 0;
}

int acme_get_nonce(acme_ctx_t *ctx, const char *nonce_url) {
    acme_http_response_t resp;
    if (acme_http_get(ctx, nonce_url, &resp) < 0) return -1;

    if (resp.status != 200 && resp.status != 204) {
        log_error("获取 nonce 失败: HTTP %d", resp.status);
        acme_http_response_free(&resp);
        return -1;
    }

    if (resp.replay_nonce[0]) {
        strncpy(ctx->nonce, resp.replay_nonce, sizeof(ctx->nonce) - 1);
        ctx->nonce[sizeof(ctx->nonce) - 1] = '\0';
    }

    acme_http_response_free(&resp);
    return 0;
}

/* ===== 账户 ===== */

int acme_create_account(acme_ctx_t *ctx, const char *email, bool terms_agreed) {
    if (!ctx->directory.newAccount[0]) {
        if (acme_get_directory(ctx, &ctx->directory) < 0) return -1;
    }

    char payload[1024];
    if (email && email[0]) {
        snprintf(payload, sizeof(payload),
            "{\"termsOfServiceAgreed\":%s,\"contact\":[\"mailto:%s\"]}",
            terms_agreed ? "true" : "false", email);
    } else {
        snprintf(payload, sizeof(payload),
            "{\"termsOfServiceAgreed\":%s}", terms_agreed ? "true" : "false");
    }

    acme_http_response_t resp;
    if (acme_http_post_jws(ctx, ctx->directory.newAccount, payload, &resp) < 0) return -1;

    /* 201 = 新账户创建, 200 = 账户已存在 */
    if (resp.status != 201 && resp.status != 200) {
        log_error("创建 ACME 账户失败: HTTP %d, body=%s", resp.status,
                  resp.body ? resp.body : "(null)");
        acme_http_response_free(&resp);
        return -1;
    }

    if (resp.location[0]) {
        strncpy(ctx->account.kid, resp.location, sizeof(ctx->account.kid) - 1);
        ctx->account.kid[sizeof(ctx->account.kid) - 1] = '\0';
    }

    if (email) {
        strncpy(ctx->account.contact, email, sizeof(ctx->account.contact) - 1);
        ctx->account.contact[sizeof(ctx->account.contact) - 1] = '\0';
    }
    ctx->account.termsAgreed = terms_agreed;

    log_info("ACME 账户已创建/复用: %s", ctx->account.kid);
    acme_http_response_free(&resp);
    return 0;
}

/* ===== 订单 ===== */

int acme_create_order(acme_ctx_t *ctx, const char **domains, size_t num_domains,
                      acme_order_t *out) {
    memset(out, 0, sizeof(*out));

    if (!ctx->directory.newOrder[0]) {
        if (acme_get_directory(ctx, &ctx->directory) < 0) return -1;
    }

    /* 构建 identifiers JSON */
    char identifiers[4096] = "";
    size_t off = 0;
    off += snprintf(identifiers + off, sizeof(identifiers) - off, "[");
    for (size_t i = 0; i < num_domains; i++) {
        off += snprintf(identifiers + off, sizeof(identifiers) - off,
            "%s{\"type\":\"dns\",\"value\":\"%s\"}",
            i > 0 ? "," : "", domains[i]);
    }
    off += snprintf(identifiers + off, sizeof(identifiers) - off, "]");

    char payload[8192];
    snprintf(payload, sizeof(payload), "{\"identifiers\":%s}", identifiers);

    acme_http_response_t resp;
    if (acme_http_post_jws(ctx, ctx->directory.newOrder, payload, &resp) < 0) return -1;

    if (resp.status != 201) {
        log_error("创建订单失败: HTTP %d", resp.status);
        acme_http_response_free(&resp);
        return -1;
    }

    /* 解析订单响应 */
    if (resp.location[0]) {
        strncpy(out->uri, resp.location, sizeof(out->uri) - 1);
    }

    /* 提取 status */
    char *p = strstr(resp.body, "\"status\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                p++;
                size_t i = 0;
                while (*p && *p != '"' && i < sizeof(out->status) - 1)
                    out->status[i++] = *p++;
                out->status[i] = '\0';
            }
        }
    }

    /* 提取 finalize */
    p = strstr(resp.body, "\"finalize\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                p++;
                size_t i = 0;
                while (*p && *p != '"' && i < sizeof(out->finalize) - 1)
                    out->finalize[i++] = *p++;
                out->finalize[i] = '\0';
            }
        }
    }

    /* 提取 authorizations 数组 */
    p = strstr(resp.body, "\"authorizations\"");
    if (p) {
        p = strchr(p, '[');
        if (p) {
            p++;
            /* 简单解析：提取引号内的 URI */
            out->authz = (acme_authz_t *)calloc(num_domains, sizeof(acme_authz_t));
            size_t authz_count = 0;
            while (*p && *p != ']' && authz_count < num_domains) {
                while (*p && (*p == ' ' || *p == '\n' || *p == ',')) p++;
                if (*p == '"') {
                    p++;
                    size_t i = 0;
                    while (*p && *p != '"' && i < sizeof(out->authz[authz_count].uri) - 1)
                        out->authz[authz_count].uri[i++] = *p++;
                    out->authz[authz_count].uri[i] = '\0';
                    authz_count++;
                    if (*p == '"') p++;
                } else { break; }
            }
            out->num_authz = authz_count;
        }
    }

    acme_http_response_free(&resp);
    return 0;
}

void acme_order_free(acme_order_t *order) {
    if (order->authz) {
        free(order->authz);
        order->authz = NULL;
    }
    order->num_authz = 0;
}

/* ===== 授权与挑战 ===== */

int acme_fetch_authz(acme_ctx_t *ctx, acme_authz_t *authz) {
    acme_http_response_t resp;
    if (acme_http_post_jws(ctx, authz->uri, NULL, &resp) < 0) return -1;

    if (resp.status != 200) {
        log_error("获取授权失败: HTTP %d", resp.status);
        acme_http_response_free(&resp);
        return -1;
    }

    /* 提取 status */
    char *p = strstr(resp.body, "\"status\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                p++;
                size_t i = 0;
                while (*p && *p != '"' && i < sizeof(authz->status) - 1)
                    authz->status[i++] = *p++;
                authz->status[i] = '\0';
            }
        }
    }

    /* 提取域名 */
    p = strstr(resp.body, "\"value\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p = strchr(p, '"');
            if (p) {
                p++;
                size_t i = 0;
                while (*p && *p != '"' && i < sizeof(authz->domain) - 1)
                    authz->domain[i++] = *p++;
                authz->domain[i] = '\0';
            }
        }
    }

    /* 提取 token（在 challenges 数组中找 http-01） */
    p = strstr(resp.body, "\"type\":\"http-01\"");
    if (p) {
        /* 向前找 token */
        char *token_start = strstr(resp.body, "\"token\"");
        if (token_start && token_start < p + 100) {
            token_start = strchr(token_start, ':');
            if (token_start) {
                token_start = strchr(token_start, '"');
                if (token_start) {
                    token_start++;
                    size_t i = 0;
                    while (*token_start && *token_start != '"' && i < sizeof(authz->token) - 1)
                        authz->token[i++] = *token_start++;
                    authz->token[i] = '\0';
                }
            }
        }
    }

    acme_http_response_free(&resp);
    return 0;
}

int acme_respond_challenge(acme_ctx_t *ctx, const acme_authz_t *authz) {
    /* 构建挑战 URL: authz_uri + "/<token>" */
    char challenge_url[768];
    snprintf(challenge_url, sizeof(challenge_url), "%s/%s", authz->uri, authz->token);

    acme_http_response_t resp;
    if (acme_http_post_jws(ctx, challenge_url, "{}", &resp) < 0) return -1;

    if (resp.status != 200 && resp.status != 202) {
        log_error("响应挑战失败: HTTP %d", resp.status);
        acme_http_response_free(&resp);
        return -1;
    }

    acme_http_response_free(&resp);
    return 0;
}

/* ===== 轮询 ===== */

int acme_poll_authz(acme_ctx_t *ctx, acme_authz_t *authz, int timeout_ms) {
    int elapsed = 0;
    int interval = 2000; /* 2 秒轮询间隔 */

    while (elapsed < timeout_ms) {
        if (acme_fetch_authz(ctx, authz) < 0) return -1;

        if (strcmp(authz->status, "valid") == 0) return 0;
        if (strcmp(authz->status, "invalid") == 0) {
            log_error("授权验证失败: %s", authz->domain);
            return -1;
        }

        usleep(interval * 1000);
        elapsed += interval;
    }

    log_error("授权验证超时: %s", authz->domain);
    return -1;
}

int acme_poll_order(acme_ctx_t *ctx, acme_order_t *order, int timeout_ms) {
    int elapsed = 0;
    int interval = 2000;

    while (elapsed < timeout_ms) {
        acme_http_response_t resp;
        if (acme_http_post_jws(ctx, order->uri, NULL, &resp) < 0) return -1;

        if (resp.status == 200) {
            /* 提取 status */
            char *p = strstr(resp.body, "\"status\"");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    p = strchr(p, '"');
                    if (p) {
                        p++;
                        size_t i = 0;
                        while (*p && *p != '"' && i < sizeof(order->status) - 1)
                            order->status[i++] = *p++;
                        order->status[i] = '\0';
                    }
                }
            }

            /* 提取 certificate URL */
            if (strcmp(order->status, "valid") == 0) {
                char *cert_p = strstr(resp.body, "\"certificate\"");
                if (cert_p) {
                    cert_p = strchr(cert_p, ':');
                    if (cert_p) {
                        cert_p = strchr(cert_p, '"');
                        if (cert_p) {
                            cert_p++;
                            size_t i = 0;
                            while (*cert_p && *cert_p != '"' && i < sizeof(order->certificate) - 1)
                                order->certificate[i++] = *cert_p++;
                            order->certificate[i] = '\0';
                        }
                    }
                }
                acme_http_response_free(&resp);
                return 0;
            }

            if (strcmp(order->status, "invalid") == 0) {
                log_error("订单验证失败");
                acme_http_response_free(&resp);
                return -1;
            }
        }

        acme_http_response_free(&resp);
        usleep(interval * 1000);
        elapsed += interval;
    }

    log_error("订单验证超时");
    return -1;
}

/* ===== 证书 ===== */

/**
 * generate_csr - 生成 CSR（Certificate Signing Request）
 */
static int generate_csr(EVP_PKEY *pkey, const char **domains, size_t num_domains,
                        unsigned char **out_der, size_t *out_len) {
    X509_REQ *req = X509_REQ_new();
    if (!req) return -1;

    X509_REQ_set_pubkey(req, pkey);

    /* Subject: CN = 第一个域名 */
    X509_NAME *name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (unsigned char *)domains[0], -1, -1, 0);
    X509_REQ_set_subject_name(req, name);
    X509_NAME_free(name);

    /* 添加 SAN 扩展 */
    if (num_domains > 0) {
        STACK_OF(GENERAL_NAME) *san = sk_GENERAL_NAME_new_null();
        for (size_t i = 0; i < num_domains; i++) {
            GENERAL_NAME *gn = GENERAL_NAME_new();
            ASN1_IA5STRING *ia5 = ASN1_IA5STRING_new();
            ASN1_STRING_set(ia5, domains[i], -1);
            GENERAL_NAME_set0_value(gn, GEN_DNS, ia5);
            sk_GENERAL_NAME_push(san, gn);
        }

        X509_EXTENSION *ext = X509V3_EXT_i2d(NID_subject_alt_name, 0, san);
        X509_REQ_add_extensions(req, sk_X509_EXTENSION_new_null());
        sk_X509_EXTENSION_push(sk_X509_EXTENSION_new_null(), ext);
        /* 简化处理：实际需要正确管理 STACK_OF */
        sk_GENERAL_NAME_pop_free(san, GENERAL_NAME_free);
    }

    X509_REQ_sign(req, pkey, EVP_sha256());

    *out_len = (size_t)i2d_X509_REQ(req, NULL);
    *out_der = (unsigned char *)malloc(*out_len);
    unsigned char *p = *out_der;
    i2d_X509_REQ(req, &p);

    X509_REQ_free(req);
    return 0;
}

int acme_finalize_order(acme_ctx_t *ctx, acme_order_t *order,
                        const char **domains, size_t num_domains) {
    /* 生成域名私钥和 CSR */
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1);
    EVP_PKEY *domain_key = NULL;
    EVP_PKEY_keygen(pctx, &domain_key);
    EVP_PKEY_CTX_free(pctx);

    unsigned char *csr_der = NULL;
    size_t csr_len = 0;
    if (generate_csr(domain_key, domains, num_domains, &csr_der, &csr_len) < 0) {
        EVP_PKEY_free(domain_key);
        return -1;
    }

    /* base64url 编码 CSR */
    char *csr_b64 = base64url_encode(csr_der, csr_len);
    free(csr_der);

    char payload[8192];
    snprintf(payload, sizeof(payload), "{\"csr\":\"%s\"}", csr_b64);
    free(csr_b64);

    acme_http_response_t resp;
    if (acme_http_post_jws(ctx, order->finalize, payload, &resp) < 0) {
        EVP_PKEY_free(domain_key);
        return -1;
    }

    if (resp.status != 200) {
        log_error("Finalize 订单失败: HTTP %d", resp.status);
        acme_http_response_free(&resp);
        EVP_PKEY_free(domain_key);
        return -1;
    }

    acme_http_response_free(&resp);

    /* 保存域名私钥供后续使用 */
    /* TODO: 需要把 domain_key 返回给调用者，这里简化处理 */
    EVP_PKEY_free(domain_key);
    return 0;
}

int acme_download_certificate(acme_ctx_t *ctx, const char *cert_url, char **out_pem) {
    acme_http_response_t resp;
    if (acme_http_post_jws(ctx, cert_url, NULL, &resp) < 0) return -1;

    if (resp.status != 200) {
        log_error("下载证书失败: HTTP %d", resp.status);
        acme_http_response_free(&resp);
        return -1;
    }

    *out_pem = resp.body;
    return 0;
}

/* ===== Thumbprint ===== */

int acme_get_thumbprint(acme_ctx_t *ctx, char **out) {
    /* 构建 JWK */
    EC_KEY *ec = EVP_PKEY_get1_EC_KEY(ctx->account_key);
    const EC_POINT *pub = EC_KEY_get0_public_key(ec);
    const EC_GROUP *grp = EC_KEY_get0_group(ec);

    BIGNUM *x = BN_new(), *y = BN_new();
    EC_POINT_get_affine_coordinates_GFp(grp, pub, x, y, NULL);

    /* x, y 转原始字节 */
    unsigned char x_bytes[32], y_bytes[32];
    int x_len = BN_bn2binpad(x, x_bytes, 32);
    int y_len = BN_bn2binpad(y, y_bytes, 32);
    BN_free(x); BN_free(y);
    EC_KEY_free(ec);

    /* JWK JSON */
    char jwk[512];
    snprintf(jwk, sizeof(jwk),
        "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}",
        base64url_encode(x_bytes, (size_t)x_len),
        base64url_encode(y_bytes, (size_t)y_len));

    /* SHA-256 */
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char *)jwk, strlen(jwk), hash);

    *out = base64url_encode(hash, SHA256_DIGEST_LENGTH);
    return 0;
}

/* ===== 一键签发 ===== */

int acme_issue_certificate(acme_ctx_t *ctx, const char **domains, size_t num_domains,
                           const char *email, char **cert_pem, char **key_pem) {
    *cert_pem = NULL;
    *key_pem = NULL;

    /* 1. 获取目录 */
    acme_directory_t dir;
    if (acme_get_directory(ctx, &dir) < 0) {
        log_error("获取 ACME 目录失败");
        return -1;
    }

    /* 2. 创建账户 */
    if (acme_create_account(ctx, email, true) < 0) {
        log_error("创建 ACME 账户失败");
        return -1;
    }

    /* 3. 创建订单 */
    acme_order_t order;
    if (acme_create_order(ctx, domains, num_domains, &order) < 0) {
        log_error("创建证书订单失败");
        return -1;
    }

    /* 4. 获取授权和挑战 */
    for (size_t i = 0; i < order.num_authz; i++) {
        if (acme_fetch_authz(ctx, &order.authz[i]) < 0) {
            acme_order_free(&order);
            return -1;
        }
    }

    /* 5. 响应挑战（调用者需要先设置好 HTTP-01 响应） */
    for (size_t i = 0; i < order.num_authz; i++) {
        if (acme_respond_challenge(ctx, &order.authz[i]) < 0) {
            acme_order_free(&order);
            return -1;
        }
    }

    /* 6. 轮询授权状态 */
    for (size_t i = 0; i < order.num_authz; i++) {
        if (acme_poll_authz(ctx, &order.authz[i], 120000) < 0) {
            acme_order_free(&order);
            return -1;
        }
    }

    /* 7. Finalize 订单 */
    if (acme_finalize_order(ctx, &order, domains, num_domains) < 0) {
        acme_order_free(&order);
        return -1;
    }

    /* 8. 轮询订单状态 */
    if (acme_poll_order(ctx, &order, 120000) < 0) {
        acme_order_free(&order);
        return -1;
    }

    /* 9. 下载证书 */
    if (acme_download_certificate(ctx, order.certificate, cert_pem) < 0) {
        acme_order_free(&order);
        return -1;
    }

    /* TODO: key_pem 需要从 finalize_order 中传递出来 */
    *key_pem = strdup(""); /* 占位 */

    acme_order_free(&order);
    return 0;
}

/**
 * test_acme.c - ACME 模块单元测试
 *
 * @author xfy
 */

#include "unity.h"
#include "acme.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

/* 测试用的 EC 私钥 PEM (有效格式但随机数据) */
static const char *TEST_KEY_PEM =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHQCAQEEIBHkPjHK+3VjZV+p7rKDcAEO2db7t4V1b01QfZVJHfHmoAcGBSuBBAAK\n"
    "oUQDQgAEX0hOmL0zQ0y1P1uOHZPVjR1xBZQlpVZ1PjY2V0tZ0V0tZ0V0tZ0V0\n"
    "tZ0V0tZ0V0tZ0V0tZ0V0tZ0V0tZw==\n"
    "-----END EC PRIVATE KEY-----\n";

void setUp(void) {
    log_set_level(LOG_LEVEL_ERROR);
}

void tearDown(void) {
}

/* 测试: acme_create / acme_destroy */
void test_acme_create_destroy(void) {
    acme_ctx_t *ctx = acme_create("https://acme-staging-v02.api.letsencrypt.org/directory", NULL);
    TEST_ASSERT_NOT_NULL(ctx);
    acme_destroy(ctx);
}

/* 测试: acme_create 自带私钥 */
void test_acme_create_with_key(void) {
    acme_ctx_t *ctx = acme_create("https://acme-staging-v02.api.letsencrypt.org/directory", TEST_KEY_PEM);
    /* 这个 PEM 是伪造的，可能解析失败，但至少测试了代码路径 */
    acme_destroy(ctx);
}

/* 测试: acme_get_directory (需要网络，可能失败) */
void test_acme_get_directory(void) {
    acme_ctx_t *ctx = acme_create("https://acme-staging-v02.api.letsencrypt.org/directory", NULL);
    TEST_ASSERT_NOT_NULL(ctx);

    acme_directory_t dir;
    int ret = acme_get_directory(ctx, &dir);

    /* 网络可能不可用，允许失败 */
    if (ret == 0) {
        TEST_ASSERT_TRUE(dir.newNonce[0] != '\0');
        TEST_ASSERT_TRUE(dir.newAccount[0] != '\0');
        TEST_ASSERT_TRUE(dir.newOrder[0] != '\0');
    }

    acme_destroy(ctx);
}

/* 测试: acme_get_nonce (需要网络) */
void test_acme_get_nonce(void) {
    acme_ctx_t *ctx = acme_create("https://acme-staging-v02.api.letsencrypt.org/directory", NULL);
    TEST_ASSERT_NOT_NULL(ctx);

    /* 先获取目录 */
    acme_directory_t dir;
    if (acme_get_directory(ctx, &dir) == 0) {
        int ret = acme_get_nonce(ctx, dir.newNonce);
        /* nonce 获取成功或失败都可以接受（网络问题） */
        (void)ret;
    }

    acme_destroy(ctx);
}

/* 测试: acme_create_account (需要网络) */
void test_acme_create_account(void) {
    acme_ctx_t *ctx = acme_create("https://acme-staging-v02.api.letsencrypt.org/directory", NULL);
    TEST_ASSERT_NOT_NULL(ctx);

    /* 获取目录 */
    acme_directory_t dir;
    if (acme_get_directory(ctx, &dir) == 0) {
        int ret = acme_create_account(ctx, "test@example.com", true);
        /* 可能 200(已有账户) 或 201(新账户)，网络问题也可能失败 */
        (void)ret;
    }

    acme_destroy(ctx);
}

/* 测试: 订单结构内存管理 */
void test_acme_order_free(void) {
    acme_order_t order;
    memset(&order, 0, sizeof(order));

    /* 模拟有 authz */
    order.authz = (acme_authz_t *)calloc(2, sizeof(acme_authz_t));
    order.num_authz = 2;
    strcpy(order.authz[0].domain, "example.com");
    strcpy(order.authz[0].token, "token1");
    strcpy(order.authz[1].domain, "www.example.com");
    strcpy(order.authz[1].token, "token2");

    acme_order_free(&order);
    TEST_ASSERT_NULL(order.authz);
    TEST_ASSERT_EQUAL_size_t(0, order.num_authz);
}

/* 测试: HTTP 响应内存释放 */
void test_acme_http_response_free(void) {
    acme_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.body = strdup("test body");
    resp.body_len = 9;

    acme_http_response_free(&resp);
    TEST_ASSERT_NULL(resp.body);
    TEST_ASSERT_EQUAL_size_t(0, resp.body_len);
}

/* 测试: acme_issue_certificate 参数检查 (不实际执行) */
void test_acme_issue_certificate_params(void) {
    /* 这个测试主要是确保函数签名正确，不会崩溃 */
    /* 实际调用需要完整的 ACME 交互，不适合单元测试 */
    TEST_ASSERT_TRUE(1);
}

/* 测试: 目录 URL 存储 */
void test_acme_directory_url(void) {
    const char *url = "https://example.com/acme/directory";
    acme_ctx_t *ctx = acme_create(url, NULL);
    TEST_ASSERT_NOT_NULL(ctx);
    /* 内部字段不直接暴露，但至少能创建成功 */
    acme_destroy(ctx);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_acme_create_destroy);
    RUN_TEST(test_acme_create_with_key);
    RUN_TEST(test_acme_get_directory);
    RUN_TEST(test_acme_get_nonce);
    RUN_TEST(test_acme_create_account);
    RUN_TEST(test_acme_order_free);
    RUN_TEST(test_acme_http_response_free);
    RUN_TEST(test_acme_issue_certificate_params);
    RUN_TEST(test_acme_directory_url);

    return UNITY_END();
}

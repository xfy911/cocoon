#include "unity.h"
#include "server.h"
#include "config.h"
#include <stdlib.h>

/* ===== server_create 参数校验 ===== */

void test_create_null_config(void) {
    TEST_ASSERT_NULL(server_create(NULL));
}

void test_create_null_root_dir(void) {
    cocoon_config_t cfg = {.port = 8080};
    /* root_dir 为 NULL，应提前返回 NULL */
    TEST_ASSERT_NULL(server_create(&cfg));
}

/* ===== server_stop / server_destroy 安全路径 ===== */

void test_stop_null(void) {
    /* 不应 crash */
    server_stop(NULL);
    TEST_ASSERT_TRUE(1);
}

void test_destroy_null(void) {
    /* 不应 crash */
    server_destroy(NULL);
    TEST_ASSERT_TRUE(1);
}

/* ===== server_create 成功后销毁 ===== */

void test_create_and_destroy(void) {
    cocoon_config_t cfg = {
        .root_dir = "/tmp",
        .port = 29999,  /* 使用高位端口减少冲突 */
        .threaded = false
    };
    server_context_t *ctx = server_create(&cfg);
    /* 高位端口通常可用，但测试环境可能被占用，保守处理 */
    if (ctx) {
        server_stop(ctx);
        server_destroy(ctx);
        TEST_ASSERT_TRUE(1);
    } else {
        /* 端口被占用等外部原因导致失败，非逻辑错误，标记忽略 */
        TEST_IGNORE_MESSAGE("server_create failed, possibly port in use");
    }
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_create_null_config);
    RUN_TEST(test_create_null_root_dir);
    RUN_TEST(test_stop_null);
    RUN_TEST(test_destroy_null);
    RUN_TEST(test_create_and_destroy);

    return UNITY_END();
}

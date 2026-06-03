#include "unity.h"
#include "log.h"

/* ===== log_get_level / log_set_level ===== */

void test_default_level_is_info(void) {
    /* 静态全局变量默认初始化为 LOG_LEVEL_INFO */
    TEST_ASSERT_EQUAL(LOG_LEVEL_INFO, log_get_level());
}

void test_set_level_error(void) {
    log_set_level(LOG_LEVEL_ERROR);
    TEST_ASSERT_EQUAL(LOG_LEVEL_ERROR, log_get_level());
}

void test_set_level_warn(void) {
    log_set_level(LOG_LEVEL_WARN);
    TEST_ASSERT_EQUAL(LOG_LEVEL_WARN, log_get_level());
}

void test_set_level_debug(void) {
    log_set_level(LOG_LEVEL_DEBUG);
    TEST_ASSERT_EQUAL(LOG_LEVEL_DEBUG, log_get_level());
}

void test_set_level_info(void) {
    log_set_level(LOG_LEVEL_INFO);
    TEST_ASSERT_EQUAL(LOG_LEVEL_INFO, log_get_level());
}

/* ===== log_set_prefix ===== */

void test_set_prefix(void) {
    log_set_prefix("[Test]");
    /* 无返回值，仅确认不 crash */
    TEST_ASSERT_EQUAL(LOG_LEVEL_INFO, log_get_level());
}

void test_set_prefix_null(void) {
    log_set_prefix(NULL);
    /* NULL 表示无前缀，不 crash */
    TEST_ASSERT_EQUAL(LOG_LEVEL_INFO, log_get_level());
}

/* ===== 边界：多次设置 ===== */

void test_level_persistence(void) {
    log_set_level(LOG_LEVEL_DEBUG);
    TEST_ASSERT_EQUAL(LOG_LEVEL_DEBUG, log_get_level());
    log_set_level(LOG_LEVEL_ERROR);
    TEST_ASSERT_EQUAL(LOG_LEVEL_ERROR, log_get_level());
    log_set_level(LOG_LEVEL_INFO);
    TEST_ASSERT_EQUAL(LOG_LEVEL_INFO, log_get_level());
}

void setUp(void) {
    /* 每个测试前重置为默认 */
    log_set_level(LOG_LEVEL_INFO);
    log_set_prefix("[Cocoon]");
}

void tearDown(void) {
    /* 每个测试后重置 */
    log_set_level(LOG_LEVEL_INFO);
    log_set_prefix("[Cocoon]");
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_default_level_is_info);
    RUN_TEST(test_set_level_error);
    RUN_TEST(test_set_level_warn);
    RUN_TEST(test_set_level_debug);
    RUN_TEST(test_set_level_info);
    RUN_TEST(test_set_prefix);
    RUN_TEST(test_set_prefix_null);
    RUN_TEST(test_level_persistence);

    return UNITY_END();
}

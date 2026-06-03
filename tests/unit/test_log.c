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

/* ===== log_* 函数不 crash ===== */

void test_log_calls_no_crash(void) {
    /* 正常调用各等级日志，确认不崩溃 */
    log_set_level(LOG_LEVEL_DEBUG);
    log_error("error %d", 1);
    log_warn("warn %s", "x");
    log_info("info %f", 1.0);
    log_debug("debug %p", (void*)0);
}

/* ===== stderr 过滤测试 ===== */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

void test_log_level_filter(void) {
    /* 临时将 stderr 重定向到管道，验证高等级日志被过滤 */
    int pipefd[2];
    TEST_ASSERT_EQUAL(0, pipe(pipefd));

    int old_stderr = dup(STDERR_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    /* 设置 ERROR 级别，调用 INFO 不应输出 */
    log_set_level(LOG_LEVEL_ERROR);
    log_info("this should be filtered");

    /* 恢复 stderr */
    fflush(stderr);
    dup2(old_stderr, STDERR_FILENO);
    close(old_stderr);

    /* 读取管道 */
    char buf[256] = {0};
    int n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);

    /* INFO 被过滤，管道应无输出或只有前缀/时间（不含消息） */
    if (n > 0) {
        TEST_ASSERT_NULL(strstr(buf, "this should be filtered"));
    }

    /* ERROR 应正常输出 */
    int pipefd2[2];
    TEST_ASSERT_EQUAL(0, pipe(pipefd2));
    old_stderr = dup(STDERR_FILENO);
    dup2(pipefd2[1], STDERR_FILENO);
    close(pipefd2[1]);

    log_error("this is an error");

    fflush(stderr);
    dup2(old_stderr, STDERR_FILENO);
    close(old_stderr);

    char buf2[256] = {0};
    n = read(pipefd2[0], buf2, sizeof(buf2) - 1);
    close(pipefd2[0]);

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf2, "this is an error"));
}

void setUp(void) {
    log_set_level(LOG_LEVEL_INFO);
    log_set_prefix("[Cocoon]");
}

void tearDown(void) {
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
    RUN_TEST(test_log_calls_no_crash);
    RUN_TEST(test_log_level_filter);

    return UNITY_END();
}

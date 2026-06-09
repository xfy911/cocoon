#include "unity.h"
#include "healthcheck.h"
#include "proxy.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* 白盒测试：包含 healthcheck.c 以访问静态函数 */
#define UNIT_TEST
#include "healthcheck.c"
#undef UNIT_TEST

/* Unity 测试框架要求的 setUp/tearDown */
void setUp(void) { }
void tearDown(void) { }

/* ===== healthcheck_parse_status ===== */

void test_parse_status_200(void) {
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    int status = healthcheck_parse_status(resp, strlen(resp));
    TEST_ASSERT_EQUAL(200, status);
}

void test_parse_status_404(void) {
    const char *resp = "HTTP/1.1 404 Not Found\r\n\r\n";
    int status = healthcheck_parse_status(resp, strlen(resp));
    TEST_ASSERT_EQUAL(404, status);
}

void test_parse_status_500(void) {
    const char *resp = "HTTP/1.0 500 Internal Server Error\r\n\r\n";
    int status = healthcheck_parse_status(resp, strlen(resp));
    TEST_ASSERT_EQUAL(500, status);
}

void test_parse_status_too_short(void) {
    TEST_ASSERT_EQUAL(-1, healthcheck_parse_status("HTTP/1.1", 8));
    TEST_ASSERT_EQUAL(-1, healthcheck_parse_status("", 0));
    TEST_ASSERT_EQUAL(-1, healthcheck_parse_status("HTTP", 4));
}

void test_parse_status_not_http(void) {
    const char *resp = "SSH-2.0-OpenSSH_8.0\r\n";
    TEST_ASSERT_EQUAL(-1, healthcheck_parse_status(resp, strlen(resp)));
}

void test_parse_status_malformed_status(void) {
    const char *resp = "HTTP/1.1 abc OK\r\n\r\n";
    TEST_ASSERT_EQUAL(-1, healthcheck_parse_status(resp, strlen(resp)));
}

void test_parse_status_edge_cases(void) {
    /* 100 Continue */
    const char *resp = "HTTP/1.1 100 Continue\r\n\r\n";
    TEST_ASSERT_EQUAL(100, healthcheck_parse_status(resp, strlen(resp)));

    /* 302 Found */
    const char *resp2 = "HTTP/1.1 302 Found\r\n\r\n";
    TEST_ASSERT_EQUAL(302, healthcheck_parse_status(resp2, strlen(resp2)));

    /* 503 Service Unavailable */
    const char *resp3 = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
    TEST_ASSERT_EQUAL(503, healthcheck_parse_status(resp3, strlen(resp3)));
}

/* ===== healthcheck_build_request ===== */

void test_build_request_default_path(void) {
    cocoon_proxy_backend_t backend = {0};
    strncpy(backend.target_host, "localhost", sizeof(backend.target_host) - 1);
    backend.target_port = 3000;

    char buf[512];
    healthcheck_build_request(&backend, NULL, buf, sizeof(buf));

    TEST_ASSERT_TRUE(strstr(buf, "GET /health HTTP/1.1") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "Host: localhost:3000") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "Connection: close") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "Cocoon-Healthcheck/1.0") != NULL);
}

void test_build_request_custom_path(void) {
    cocoon_proxy_backend_t backend = {0};
    strncpy(backend.target_host, "api.example.com", sizeof(backend.target_host) - 1);
    backend.target_port = 8080;

    char buf[512];
    healthcheck_build_request(&backend, "/ready", buf, sizeof(buf));

    TEST_ASSERT_TRUE(strstr(buf, "GET /ready HTTP/1.1") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "Host: api.example.com:8080") != NULL);
}

void test_build_request_empty_path_uses_default(void) {
    cocoon_proxy_backend_t backend = {0};
    strncpy(backend.target_host, "127.0.0.1", sizeof(backend.target_host) - 1);
    backend.target_port = 5000;

    char buf[512];
    healthcheck_build_request(&backend, "", buf, sizeof(buf));

    TEST_ASSERT_TRUE(strstr(buf, "GET /health HTTP/1.1") != NULL);
}

/* ===== healthcheck_update_state ===== */

void test_update_state_healthy_threshold(void) {
    cocoon_proxy_backend_t backend = {0};
    backend.healthy = false;

    /* 第1次成功 — 仍不健康 */
    healthcheck_update_state(&backend, true);
    TEST_ASSERT_FALSE(backend.healthy);
    TEST_ASSERT_EQUAL(1, backend.success_count);

    /* 第2次成功 — 达到阈值，恢复健康 */
    healthcheck_update_state(&backend, true);
    TEST_ASSERT_TRUE(backend.healthy);
    TEST_ASSERT_EQUAL(2, backend.success_count);
    TEST_ASSERT_EQUAL(0, backend.fail_count);
}

void test_update_state_unhealthy_threshold(void) {
    cocoon_proxy_backend_t backend = {0};
    backend.healthy = true;

    /* 连续3次失败 — 达到阈值，标记不健康 */
    healthcheck_update_state(&backend, false);
    TEST_ASSERT_TRUE(backend.healthy); /* 第1次，仍健康 */
    TEST_ASSERT_EQUAL(1, backend.fail_count);

    healthcheck_update_state(&backend, false);
    TEST_ASSERT_TRUE(backend.healthy); /* 第2次，仍健康 */
    TEST_ASSERT_EQUAL(2, backend.fail_count);

    healthcheck_update_state(&backend, false);
    TEST_ASSERT_FALSE(backend.healthy); /* 第3次，标记不健康 */
    TEST_ASSERT_EQUAL(3, backend.fail_count);
    TEST_ASSERT_EQUAL(0, backend.success_count);
}

void test_update_state_reset_counts(void) {
    cocoon_proxy_backend_t backend = {0};
    backend.healthy = true;

    /* 先失败1次 */
    healthcheck_update_state(&backend, false);
    TEST_ASSERT_EQUAL(1, backend.fail_count);

    /* 然后成功1次 — fail_count 重置 */
    healthcheck_update_state(&backend, true);
    TEST_ASSERT_TRUE(backend.healthy);
    TEST_ASSERT_EQUAL(0, backend.fail_count);
    TEST_ASSERT_EQUAL(1, backend.success_count);

    /* 再失败1次 — success_count 重置 */
    healthcheck_update_state(&backend, false);
    TEST_ASSERT_TRUE(backend.healthy); /* 只失败1次，仍健康 */
    TEST_ASSERT_EQUAL(1, backend.fail_count);
    TEST_ASSERT_EQUAL(0, backend.success_count);
}

void test_update_state_stays_healthy_after_single_failure(void) {
    cocoon_proxy_backend_t backend = {0};
    backend.healthy = true;

    /* 只失败1次，不应标记不健康 */
    healthcheck_update_state(&backend, false);
    TEST_ASSERT_TRUE(backend.healthy);
    TEST_ASSERT_EQUAL(1, backend.fail_count);

    /* 再次成功 */
    healthcheck_update_state(&backend, true);
    TEST_ASSERT_TRUE(backend.healthy);
}

void test_update_state_stays_unhealthy_after_single_success(void) {
    cocoon_proxy_backend_t backend = {0};
    backend.healthy = false;

    /* 只成功1次，不应标记健康 */
    healthcheck_update_state(&backend, true);
    TEST_ASSERT_FALSE(backend.healthy);
    TEST_ASSERT_EQUAL(1, backend.success_count);
}

void test_update_state_last_check_updated(void) {
    cocoon_proxy_backend_t backend = {0};
    time_t before = time(NULL);

    healthcheck_update_state(&backend, true);

    time_t after = time(NULL);
    TEST_ASSERT_TRUE(backend.last_check >= before);
    TEST_ASSERT_TRUE(backend.last_check <= after);
}

/* ===== healthcheck_probe_once ===== */

void test_probe_once_null_backend(void) {
    TEST_ASSERT_FALSE(healthcheck_probe_once(NULL, 1000));
}

/* ===== 主函数 ===== */

int main(void) {
    UNITY_BEGIN();

    /* parse_status */
    RUN_TEST(test_parse_status_200);
    RUN_TEST(test_parse_status_404);
    RUN_TEST(test_parse_status_500);
    RUN_TEST(test_parse_status_too_short);
    RUN_TEST(test_parse_status_not_http);
    RUN_TEST(test_parse_status_malformed_status);
    RUN_TEST(test_parse_status_edge_cases);

    /* build_request */
    RUN_TEST(test_build_request_default_path);
    RUN_TEST(test_build_request_custom_path);
    RUN_TEST(test_build_request_empty_path_uses_default);

    /* update_state */
    RUN_TEST(test_update_state_healthy_threshold);
    RUN_TEST(test_update_state_unhealthy_threshold);
    RUN_TEST(test_update_state_reset_counts);
    RUN_TEST(test_update_state_stays_healthy_after_single_failure);
    RUN_TEST(test_update_state_stays_unhealthy_after_single_success);
    RUN_TEST(test_update_state_last_check_updated);

    /* probe_once */
    RUN_TEST(test_probe_once_null_backend);

    return UNITY_END();
}

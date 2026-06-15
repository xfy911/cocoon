#include "unity.h"
#include "dashboard.h"
#include "http.h"
#include <string.h>

/* 为测试提供全局指标变量的定义（server.c 中定义的副本） */
atomic_int g_active_connections = 0;
time_t g_server_start_time = 0;
uint32_t g_max_connections = 0;
atomic_uint g_total_requests = 0;
atomic_uint g_response_2xx = 0;
atomic_uint g_response_3xx = 0;
atomic_uint g_response_4xx = 0;
atomic_uint g_response_5xx = 0;
atomic_uint g_response_200 = 0;
atomic_uint g_response_404 = 0;

/* ===== dashboard_handle_request 路径匹配测试 ===== */

void test_dashboard_not_match(void) {
    http_request_t req = {0};
    req.method = HTTP_GET;
    strncpy(req.path, "/index.html", sizeof(req.path) - 1);
    /* fd = -1 不会实际发送，但匹配逻辑不受影响 */
    TEST_ASSERT_FALSE(dashboard_handle_request(-1, &req));
}

void test_dashboard_match_status(void) {
    http_request_t req = {0};
    req.method = HTTP_GET;
    strncpy(req.path, "/_status", sizeof(req.path) - 1);
    req.keep_alive = false;
    /* fd = -1 会导致 send 失败，但函数应返回 true（请求已识别） */
    TEST_ASSERT_TRUE(dashboard_handle_request(-1, &req));
}

void test_dashboard_post_not_allowed(void) {
    http_request_t req = {0};
    req.method = HTTP_POST;
    strncpy(req.path, "/_status", sizeof(req.path) - 1);
    /* 非 GET 应返回 405，但函数仍返回 true（请求已处理） */
    TEST_ASSERT_TRUE(dashboard_handle_request(-1, &req));
}

void test_dashboard_null_req(void) {
    TEST_ASSERT_FALSE(dashboard_handle_request(-1, NULL));
}

/* ===== dashboard_sse_handle_request 路径匹配测试 ===== */

void test_dashboard_sse_not_match(void) {
    http_request_t req = {0};
    req.method = HTTP_GET;
    strncpy(req.path, "/_sse", sizeof(req.path) - 1);
    TEST_ASSERT_FALSE(dashboard_sse_handle_request(-1, &req));
}

void test_dashboard_sse_match_events(void) {
    http_request_t req = {0};
    req.method = HTTP_GET;
    strncpy(req.path, "/_status/events", sizeof(req.path) - 1);
    /* fd = -1 会导致 send 失败，快速退出循环，返回 true */
    TEST_ASSERT_TRUE(dashboard_sse_handle_request(-1, &req));
}

void test_dashboard_sse_post_not_allowed(void) {
    http_request_t req = {0};
    req.method = HTTP_POST;
    strncpy(req.path, "/_status/events", sizeof(req.path) - 1);
    TEST_ASSERT_TRUE(dashboard_sse_handle_request(-1, &req));
}

void test_dashboard_sse_null_req(void) {
    TEST_ASSERT_FALSE(dashboard_sse_handle_request(-1, NULL));
}

/* ===== 指标变量存在性验证 ===== */

void test_metrics_variables_exist(void) {
    /* 确认全局指标变量可被访问（链接期检查） */
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_active_connections));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_total_requests));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_response_2xx));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_response_3xx));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_response_4xx));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_response_5xx));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_response_200));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_response_404));
    TEST_ASSERT_EQUAL_UINT(0, g_max_connections);
    TEST_ASSERT_EQUAL_INT64(0, g_server_start_time);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_dashboard_not_match);
    RUN_TEST(test_dashboard_match_status);
    RUN_TEST(test_dashboard_post_not_allowed);
    RUN_TEST(test_dashboard_null_req);
    RUN_TEST(test_dashboard_sse_not_match);
    RUN_TEST(test_dashboard_sse_match_events);
    RUN_TEST(test_dashboard_sse_post_not_allowed);
    RUN_TEST(test_dashboard_sse_null_req);
    RUN_TEST(test_metrics_variables_exist);

    return UNITY_END();
}

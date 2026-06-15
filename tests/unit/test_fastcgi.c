/*
 * test_fastcgi.c - FastCGI 模块单元测试
 */

#include "unity.h"
#include "fastcgi.h"
#include <string.h>
#include <stdlib.h>

void setUp(void) {}
void tearDown(void) {}

/* ========== 记录编码测试 ========== */

static void test_fcgi_build_record(void) {
    uint8_t buf[256];
    uint8_t data[] = "hello";
    
    int n = fcgi_build_record(FCGI_PARAMS, 1, data, 5, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    
    TEST_ASSERT_EQUAL(FCGI_VERSION_1, buf[0]);
    TEST_ASSERT_EQUAL(FCGI_PARAMS, buf[1]);
    TEST_ASSERT_EQUAL(0, buf[2]); /* requestId high */
    TEST_ASSERT_EQUAL(1, buf[3]); /* requestId low */
    TEST_ASSERT_EQUAL(0, buf[4]); /* contentLength high */
    TEST_ASSERT_EQUAL(5, buf[5]); /* contentLength low */
    TEST_ASSERT_EQUAL(3, buf[6]); /* padding = 8-5%8 = 3 */
    TEST_ASSERT_EQUAL(0, buf[7]); /* reserved */
    
    TEST_ASSERT_EQUAL_MEMORY(data, buf + 8, 5);
}

static void test_fcgi_build_begin_request(void) {
    uint8_t buf[256];
    
    int n = fcgi_build_begin_request(42, FCGI_RESPONDER, FCGI_KEEP_CONN, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(16, n); /* header 8 + body 8 + padding 0 */
    
    TEST_ASSERT_EQUAL(FCGI_VERSION_1, buf[0]);
    TEST_ASSERT_EQUAL(FCGI_BEGIN_REQUEST, buf[1]);
    TEST_ASSERT_EQUAL(0, buf[2]);
    TEST_ASSERT_EQUAL(42, buf[3]);
    
    /* role 应该是大端：FCGI_RESPONDER = 1 */
    TEST_ASSERT_EQUAL(0, buf[8]);
    TEST_ASSERT_EQUAL(1, buf[9]); /* role low byte */
    TEST_ASSERT_EQUAL(FCGI_KEEP_CONN, buf[10]); /* flags */
}

static void test_fcgi_build_empty_stdin(void) {
    uint8_t buf[256];
    
    int n = fcgi_build_empty_stdin(7, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(8, n); /* 只有 header，无内容 */
    
    TEST_ASSERT_EQUAL(FCGI_VERSION_1, buf[0]);
    TEST_ASSERT_EQUAL(FCGI_STDIN, buf[1]);
    TEST_ASSERT_EQUAL(0, buf[2]);
    TEST_ASSERT_EQUAL(7, buf[3]);
    TEST_ASSERT_EQUAL(0, buf[4]); /* contentLength = 0 */
    TEST_ASSERT_EQUAL(0, buf[5]);
    TEST_ASSERT_EQUAL(0, buf[6]); /* padding = 0 */
}

/* ========== 名值对编码测试 ========== */

static void test_fcgi_encode_name_value_short(void) {
    uint8_t buf[256];
    size_t len;
    
    bool ok = fcgi_encode_name_value_len("name", "value", buf, &len);
    TEST_ASSERT_TRUE(ok);
    
    /* name_len=4 (1 byte), value_len=5 (1 byte) */
    TEST_ASSERT_EQUAL(4, buf[0]);
    TEST_ASSERT_EQUAL(5, buf[1]);
    TEST_ASSERT_EQUAL_MEMORY("name", buf + 2, 4);
    TEST_ASSERT_EQUAL_MEMORY("value", buf + 6, 5);
    TEST_ASSERT_EQUAL(2 + 4 + 5, len);
}

static void test_fcgi_encode_name_value_long(void) {
    uint8_t buf[256];
    size_t len;
    
    /* 构造一个长度 > 127 的 name */
    char long_name[200];
    memset(long_name, 'a', 199);
    long_name[199] = '\0';
    
    bool ok = fcgi_encode_name_value_len(long_name, "v", buf, &len);
    TEST_ASSERT_TRUE(ok);
    
    /* 长度编码应该用 4 bytes */
    TEST_ASSERT_TRUE((buf[0] & 0x80) != 0); /* 高位置1 */
}

/* ========== 参数管理测试 ========== */

static void test_fcgi_add_param(void) {
    fcgi_request_t req;
    fcgi_request_init(&req, 1);
    
    TEST_ASSERT_TRUE(fcgi_add_param(&req, "SCRIPT_NAME", "/index.php"));
    TEST_ASSERT_TRUE(fcgi_add_param(&req, "REQUEST_METHOD", "GET"));
    
    TEST_ASSERT_NOT_NULL(req.params);
    fcgi_request_free(&req);
}

static void test_fcgi_build_params(void) {
    fcgi_request_t req;
    fcgi_request_init(&req, 1);
    
    fcgi_add_param(&req, "SCRIPT_NAME", "/index.php");
    fcgi_add_param(&req, "REQUEST_METHOD", "GET");
    
    uint8_t buf[1024];
    int n = fcgi_build_params(1, req.params, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    
    /* 应该至少包含一个 PARAMS 记录和一个空的 PARAMS 结束记录 */
    TEST_ASSERT_GREATER_OR_EQUAL(16, n); /* 两个 header */
    
    fcgi_request_free(&req);
}

static void test_fcgi_build_params_empty(void) {
    fcgi_request_t req;
    fcgi_request_init(&req, 1);
    
    uint8_t buf[1024];
    int n = fcgi_build_params(1, req.params, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(8, n); /* 只有空的 PARAMS 记录 */
    
    fcgi_request_free(&req);
}

/* ========== 记录解析测试 ========== */

static void test_fcgi_parse_record(void) {
    uint8_t data[] = {
        1, FCGI_STDOUT, 0, 42, 0, 5, 3, 0,  /* header */
        'h', 'e', 'l', 'l', 'o',            /* content */
        0, 0, 0                             /* padding */
    };
    
    fcgi_header_t header;
    const uint8_t *content = NULL;
    size_t consumed = 0;
    
    bool ok = fcgi_parse_record(data, sizeof(data), &header, &content, &consumed);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(FCGI_VERSION_1, header.version);
    TEST_ASSERT_EQUAL(FCGI_STDOUT, header.type);
    TEST_ASSERT_EQUAL(42, header.requestId);
    TEST_ASSERT_EQUAL(5, header.contentLength);
    TEST_ASSERT_EQUAL(3, header.paddingLength);
    TEST_ASSERT_EQUAL(16, consumed);
    TEST_ASSERT_EQUAL_MEMORY("hello", content, 5);
}

static void test_fcgi_parse_record_insufficient(void) {
    uint8_t data[] = { 1, FCGI_STDOUT, 0, 42 }; /* 只有 4 bytes，不够 header */
    
    fcgi_header_t header;
    const uint8_t *content = NULL;
    size_t consumed = 0;
    
    bool ok = fcgi_parse_record(data, sizeof(data), &header, &content, &consumed);
    TEST_ASSERT_FALSE(ok);
}

/* ========== 响应解析测试 ========== */

static void test_fcgi_parse_response_stdout(void) {
    uint8_t stdout_rec[] = {
        1, FCGI_STDOUT, 0, 1, 0, 5, 3, 0,
        'h', 'e', 'l', 'l', 'o',
        0, 0, 0
    };
    uint8_t end_rec[] = {
        1, FCGI_END_REQUEST, 0, 1, 0, 8, 0, 0,
        0, 0, 0, 0, /* appStatus = 0 */
        0,          /* protocolStatus = FCGI_REQUEST_COMPLETE */
        0, 0, 0
    };
    
    uint8_t data[sizeof(stdout_rec) + sizeof(end_rec)];
    memcpy(data, stdout_rec, sizeof(stdout_rec));
    memcpy(data + sizeof(stdout_rec), end_rec, sizeof(end_rec));
    
    fcgi_response_t resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;
    
    bool ok = fcgi_parse_response(data, sizeof(data), &resp, &consumed);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(resp.complete);
    TEST_ASSERT_EQUAL(0, resp.appStatus);
    TEST_ASSERT_EQUAL(5, resp.stdout_len);
    TEST_ASSERT_EQUAL_MEMORY("hello", resp.stdout_data, 5);
    
    fcgi_response_free(&resp);
}

static void test_fcgi_parse_response_status(void) {
    /* 模拟 PHP 输出 Status: 404 */
    uint8_t stdout_rec[] = {
        1, FCGI_STDOUT, 0, 1,
        0, 27, 5, 0, /* contentLength = 27 */
        'S', 't', 'a', 't', 'u', 's', ':', ' ', '4', '0', '4', '\r', '\n',
        'C', 'o', 'n', 't', 'e', 'n', 't', '-', 't', 'y', 'p', 'e', ':',
        ' ', 't', 'e', 'x', 't', '/', 'h', 't', 'm', 'l', '\r', '\n',
        '\r', '\n',
        'N', 'o', 't', ' ', 'F', 'o', 'u', 'n', 'd',
        0, 0, 0, 0, 0 /* padding */
    };
    /* 修正长度计算... */
    
    fcgi_response_t resp;
    memset(&resp, 0, sizeof(resp));
    size_t consumed = 0;
    
    /* 用简单测试 */
    const char *status_line = "Status: 404\r\n\r\n";
    uint8_t rec[64];
    rec[0] = 1; rec[1] = FCGI_STDOUT; rec[2] = 0; rec[3] = 1;
    rec[4] = 0; rec[5] = strlen(status_line); rec[6] = 0; rec[7] = 0;
    memcpy(rec + 8, status_line, strlen(status_line));
    
    uint8_t end[] = { 1, FCGI_END_REQUEST, 0, 1, 0, 8, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0 };
    
    uint8_t data[128];
    memcpy(data, rec, 8 + strlen(status_line));
    memcpy(data + 8 + strlen(status_line), end, 16);
    
    bool ok = fcgi_parse_response(data, 8 + strlen(status_line) + 16, &resp, &consumed);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(resp.complete);
    TEST_ASSERT_EQUAL(404, fcgi_extract_status(resp.stdout_data, resp.stdout_len));
    
    fcgi_response_free(&resp);
}

/* ========== 状态码/Body 提取测试 ========== */

static void test_fcgi_extract_status(void) {
    const char *data = "Status: 404\r\nContent-type: text/html\r\n\r\nNot found";
    int status = fcgi_extract_status(data, strlen(data));
    TEST_ASSERT_EQUAL(404, status);
}

static void test_fcgi_extract_status_default(void) {
    const char *data = "Content-type: text/html\r\n\r\nHello";
    int status = fcgi_extract_status(data, strlen(data));
    TEST_ASSERT_EQUAL(200, status);
}

static void test_fcgi_extract_body(void) {
    const char *data = "Content-type: text/html\r\n\r\nHello World";
    const char *body = NULL;
    size_t body_len = 0;
    
    bool ok = fcgi_extract_body(data, strlen(data), &body, &body_len);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(11, body_len);
    TEST_ASSERT_EQUAL_MEMORY("Hello World", body, 11);
}

static void test_fcgi_extract_body_lf_only(void) {
    const char *data = "Content-type: text/html\n\nHello World";
    const char *body = NULL;
    size_t body_len = 0;
    
    bool ok = fcgi_extract_body(data, strlen(data), &body, &body_len);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(11, body_len);
}

/* ========== 请求 ID 管理 ========== */

static void test_fcgi_request_init(void) {
    fcgi_request_t req;
    bool ok = fcgi_request_init(&req, 42);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(42, req.requestId);
    TEST_ASSERT_EQUAL(-1, req.backend_fd);
    TEST_ASSERT_TRUE(req.keep_conn);
    TEST_ASSERT_NULL(req.params);
}

/* ========== 主函数 ========== */

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_fcgi_build_record);
    RUN_TEST(test_fcgi_build_begin_request);
    RUN_TEST(test_fcgi_build_empty_stdin);
    RUN_TEST(test_fcgi_encode_name_value_short);
    RUN_TEST(test_fcgi_encode_name_value_long);
    RUN_TEST(test_fcgi_add_param);
    RUN_TEST(test_fcgi_build_params);
    RUN_TEST(test_fcgi_build_params_empty);
    RUN_TEST(test_fcgi_parse_record);
    RUN_TEST(test_fcgi_parse_record_insufficient);
    RUN_TEST(test_fcgi_parse_response_stdout);
    RUN_TEST(test_fcgi_parse_response_status);
    RUN_TEST(test_fcgi_extract_status);
    RUN_TEST(test_fcgi_extract_status_default);
    RUN_TEST(test_fcgi_extract_body);
    RUN_TEST(test_fcgi_extract_body_lf_only);
    RUN_TEST(test_fcgi_request_init);
    
    return UNITY_END();
}

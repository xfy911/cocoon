/**
 * @file test_http3.c
 * @brief HTTP/3 (QUIC) 模块单元测试
 *
 * 使用 Unity 测试框架，覆盖以下功能：
 *   - Variable-length integer 编解码
 *   - HTTP/3 帧头编解码
 *   - QPACK 静态表编码/解码
 *   - QUIC 流管理
 *   - QUIC 连接管理
 *   - HTTP/3 会话管理
 *   - SETTINGS / GOAWAY 帧处理
 *
 * @author Cocoon Team
 */

#include "unity.h"
#include "http3.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

/* ===== 测试前置/后置 ===== */

void setUp(void) {
    http3_init();
}

void tearDown(void) {
    http3_cleanup();
}

/* ===== Variable-Length Integer 测试 ===== */

/** @test varint 编码：0（最小1字节值） */
void test_varint_encode_zero(void) {
    uint8_t buf[8];
    size_t n = http3_encode_varint(0, buf);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(0, buf[0]);
}

/** @test varint 编码：63（1字节最大值） */
void test_varint_encode_63(void) {
    uint8_t buf[8];
    size_t n = http3_encode_varint(63, buf);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(63, buf[0]);
}

/** @test varint 编码：64（需要2字节） */
void test_varint_encode_64(void) {
    uint8_t buf[8];
    size_t n = http3_encode_varint(64, buf);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(0x40, buf[0]);
    TEST_ASSERT_EQUAL(0x40, buf[1]);
}

/** @test varint 编码：16383（2字节最大值） */
void test_varint_encode_16383(void) {
    uint8_t buf[8];
    size_t n = http3_encode_varint(16383, buf);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(0x7F, buf[0]);
    TEST_ASSERT_EQUAL(0xFF, buf[1]);
}

/** @test varint 编码：16384（需要4字节） */
void test_varint_encode_16384(void) {
    uint8_t buf[8];
    size_t n = http3_encode_varint(16384, buf);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL(0x80, buf[0]);
    TEST_ASSERT_EQUAL(0x00, buf[1]);
    TEST_ASSERT_EQUAL(0x40, buf[2]);
    TEST_ASSERT_EQUAL(0x00, buf[3]);
}

/** @test varint 编码：1073741823（4字节最大值） */
void test_varint_encode_4byte_max(void) {
    uint8_t buf[8];
    size_t n = http3_encode_varint(1073741823ULL, buf);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL(0xBF, buf[0]);
    TEST_ASSERT_EQUAL(0xFF, buf[1]);
    TEST_ASSERT_EQUAL(0xFF, buf[2]);
    TEST_ASSERT_EQUAL(0xFF, buf[3]);
}

/** @test varint 编码：1073741824（需要8字节） */
void test_varint_encode_8byte(void) {
    uint8_t buf[8];
    size_t n = http3_encode_varint(1073741824ULL, buf);
    TEST_ASSERT_EQUAL(8, n);
    TEST_ASSERT_EQUAL(0xC0, buf[0]);
}

/** @test varint 编码：最大值 */
void test_varint_encode_max(void) {
    uint8_t buf[8];
    size_t n = http3_encode_varint(4611686018427387903ULL, buf);
    TEST_ASSERT_EQUAL(8, n);
    TEST_ASSERT_EQUAL(0xFF, buf[0]);
    TEST_ASSERT_EQUAL(0xFF, buf[1]);
    TEST_ASSERT_EQUAL(0xFF, buf[2]);
    TEST_ASSERT_EQUAL(0xFF, buf[3]);
    TEST_ASSERT_EQUAL(0xFF, buf[4]);
    TEST_ASSERT_EQUAL(0xFF, buf[5]);
    TEST_ASSERT_EQUAL(0xFF, buf[6]);
    TEST_ASSERT_EQUAL(0xFF, buf[7]);
}

/** @test varint 编码：NULL 缓冲区 */
void test_varint_encode_null_buf(void) {
    size_t n = http3_encode_varint(42, NULL);
    TEST_ASSERT_EQUAL(0, n);
}

/** @test varint 解码：0 */
void test_varint_decode_zero(void) {
    uint8_t buf[1] = {0x00};
    uint64_t value = 0;
    int n = http3_decode_varint(buf, 1, &value);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(0ULL, value);
}

/** @test varint 解码：63 */
void test_varint_decode_63(void) {
    uint8_t buf[1] = {0x3F};
    uint64_t value = 0;
    int n = http3_decode_varint(buf, 1, &value);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(63ULL, value);
}

/** @test varint 解码：64 */
void test_varint_decode_64(void) {
    uint8_t buf[2] = {0x40, 0x40};
    uint64_t value = 0;
    int n = http3_decode_varint(buf, 2, &value);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(64ULL, value);
}

/** @test varint 解码：16383 */
void test_varint_decode_16383(void) {
    uint8_t buf[2] = {0x7F, 0xFF};
    uint64_t value = 0;
    int n = http3_decode_varint(buf, 2, &value);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(16383ULL, value);
}

/** @test varint 解码：4字节值 */
void test_varint_decode_4byte(void) {
    uint8_t buf[4] = {0x80, 0x00, 0x00, 0x01};
    uint64_t value = 0;
    int n = http3_decode_varint(buf, 4, &value);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL(1ULL, value);
}

/** @test varint 解码：数据不足 */
void test_varint_decode_insufficient_data(void) {
    uint8_t buf[1] = {0x40}; /* 需要2字节，但只给1字节 */
    uint64_t value = 0;
    int n = http3_decode_varint(buf, 1, &value);
    TEST_ASSERT_EQUAL(-1, n);
}

/** @test varint 解码：NULL 输入 */
void test_varint_decode_null(void) {
    uint64_t value = 0;
    int n = http3_decode_varint(NULL, 0, &value);
    TEST_ASSERT_EQUAL(-1, n);
}

/** @test varint 编解码往返测试 */
void test_varint_roundtrip(void) {
    uint64_t test_values[] = {0, 1, 63, 64, 100, 16383, 16384, 100000,
                               1073741823ULL, 1073741824ULL,
                               4294967295ULL, 4611686018427387903ULL};
    int num_values = sizeof(test_values) / sizeof(test_values[0]);

    for (int i = 0; i < num_values; i++) {
        uint8_t buf[8];
        size_t enc_n = http3_encode_varint(test_values[i], buf);
        TEST_ASSERT_MESSAGE(enc_n > 0, "编码失败");

        uint64_t decoded = 0;
        int dec_n = http3_decode_varint(buf, enc_n, &decoded);
        TEST_ASSERT_EQUAL_MESSAGE(enc_n, (size_t)dec_n, "编解码长度不匹配");
        TEST_ASSERT_EQUAL_MESSAGE(test_values[i], decoded, "编解码值不匹配");
    }
}

/** @test 帧头大小计算 */
void test_frame_header_size(void) {
    TEST_ASSERT_EQUAL(2, http3_frame_header_size(0, 0));
    TEST_ASSERT_EQUAL(2, http3_frame_header_size(1, 10));
    TEST_ASSERT_EQUAL(4, http3_frame_header_size(100, 1000));
}

/* ===== HTTP/3 帧处理测试 ===== */

/** @test 帧头编码/解码 */
void test_frame_header_encode_decode(void) {
    uint8_t buf[16];
    uint64_t ft = HTTP3_FRAME_HEADERS;
    uint64_t flen = 42;

    size_t n = http3_encode_frame_header(ft, flen, buf);
    TEST_ASSERT_GREATER_THAN(0, n);

    uint64_t decoded_ft = 0, decoded_len = 0;
    int dn = http3_decode_frame_header(buf, n, &decoded_ft, &decoded_len);
    TEST_ASSERT_EQUAL(n, (size_t)dn);
    TEST_ASSERT_EQUAL(ft, decoded_ft);
    TEST_ASSERT_EQUAL(flen, decoded_len);
}

/** @test 完整帧编码 */
void test_full_frame_encode(void) {
    uint8_t payload[] = "Hello HTTP/3";
    uint8_t buf[64];

    int n = http3_encode_frame(HTTP3_FRAME_DATA, payload, sizeof(payload) - 1,
                                buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    /* 验证帧头 */
    uint64_t ft = 0, flen = 0;
    int hd = http3_decode_frame_header(buf, (size_t)n, &ft, &flen);
    TEST_ASSERT_GREATER_THAN(0, hd);
    TEST_ASSERT_EQUAL(HTTP3_FRAME_DATA, ft);
    TEST_ASSERT_EQUAL(sizeof(payload) - 1, flen);
}

/** @test 帧头解码：数据不足 */
void test_frame_header_decode_insufficient(void) {
    uint8_t buf[1] = {0x80}; /* 需要更多数据 */
    uint64_t ft = 0, flen = 0;
    int n = http3_decode_frame_header(buf, 1, &ft, &flen);
    TEST_ASSERT_EQUAL(-1, n);
}

/** @test 帧头解码：NULL 输入 */
void test_frame_header_decode_null(void) {
    uint64_t ft = 0, flen = 0;
    int n = http3_decode_frame_header(NULL, 0, &ft, &flen);
    TEST_ASSERT_EQUAL(-1, n);
}

/** @test 编码 DATA 帧 */
void test_encode_data_frame(void) {
    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t buf[32];
    int n = http3_encode_frame(HTTP3_FRAME_DATA, payload, 4, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    uint64_t ft = 0, flen = 0;
    int hd = http3_decode_frame_header(buf, (size_t)n, &ft, &flen);
    TEST_ASSERT_GREATER_THAN(0, hd);
    TEST_ASSERT_EQUAL(HTTP3_FRAME_DATA, ft);
    TEST_ASSERT_EQUAL(4, flen);
}

/** @test 编码 HEADERS 帧 */
void test_encode_headers_frame(void) {
    uint8_t payload[] = {0xC0, 0x01, 0xD5}; /* 一些 QPACK 编码数据 */
    uint8_t buf[32];
    int n = http3_encode_frame(HTTP3_FRAME_HEADERS, payload, 3, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    uint64_t ft = 0, flen = 0;
    int hd = http3_decode_frame_header(buf, (size_t)n, &ft, &flen);
    TEST_ASSERT_GREATER_THAN(0, hd);
    TEST_ASSERT_EQUAL(HTTP3_FRAME_HEADERS, ft);
    TEST_ASSERT_EQUAL(3, flen);
}

/** @test 编码 SETTINGS 帧 */
void test_encode_settings_frame(void) {
    uint8_t payload[16];
    size_t pos = 0;
    pos += http3_encode_varint(HTTP3_SETTING_MAX_FIELD_SECTION_SIZE, payload + pos);
    pos += http3_encode_varint(16384, payload + pos);

    uint8_t buf[32];
    int n = http3_encode_frame(HTTP3_FRAME_SETTINGS, payload, pos, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    uint64_t ft = 0, flen = 0;
    int hd = http3_decode_frame_header(buf, (size_t)n, &ft, &flen);
    TEST_ASSERT_GREATER_THAN(0, hd);
    TEST_ASSERT_EQUAL(HTTP3_FRAME_SETTINGS, ft);
}

/** @test 编码 GOAWAY 帧 */
void test_encode_goaway_frame(void) {
    uint8_t payload[8];
    size_t pos = http3_encode_varint(100, payload);

    uint8_t buf[32];
    int n = http3_encode_frame(HTTP3_FRAME_GOAWAY, payload, pos, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    uint64_t ft = 0, flen = 0;
    int hd = http3_decode_frame_header(buf, (size_t)n, &ft, &flen);
    TEST_ASSERT_GREATER_THAN(0, hd);
    TEST_ASSERT_EQUAL(HTTP3_FRAME_GOAWAY, ft);
}

/* ===== QPACK 编码/解码测试 ===== */

/** @test QPACK 编码：静态表完全匹配 :method=GET */
void test_qpack_encode_static_match_method_get(void) {
    uint8_t buf[16];
    int n = qpack_encode_header(":method", "GET", buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    /* Should use indexed encoding (0b11xxxxxx) with static index 2 */
    /* buf[0] should be 0xC0 | 0x02 = 0xC2 = 194 */
    TEST_ASSERT_EQUAL(0xC0, buf[0] & 0xC0); /* 0b11 prefix */
}

/** @test QPACK 编码：静态表完全匹配 :path=/ */
void test_qpack_encode_static_match_path(void) {
    uint8_t buf[16];
    int n = qpack_encode_header(":path", "/", buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL(0xC0, buf[0] & 0xC0); /* 0b11 prefix */
}

/** @test QPACK 编码：静态表完全匹配 :scheme=https */
void test_qpack_encode_static_match_scheme(void) {
    uint8_t buf[16];
    int n = qpack_encode_header(":scheme", "https", buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL(0xC0, buf[0] & 0xC0); /* 0b11 prefix */
}

/** @test QPACK 编码：字面量编码未知字段 */
void test_qpack_encode_literal_field(void) {
    uint8_t buf[64];
    int n = qpack_encode_header("x-custom-header", "custom-value", buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    /* Should use literal encoding (0b001xxxxx) */
    TEST_ASSERT_EQUAL(0x20, buf[0]);
}

/** @test QPACK 编码：名称匹配静态表，值用字面量 */
void test_qpack_encode_name_ref_value_literal(void) {
    uint8_t buf[64];
    int n = qpack_encode_header("accept", "text/html", buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    /* accept=text/html is NOT a full static table match (accept="" at idx 17),
     * so it uses literal with name reference (0b0101xxxx) */
    TEST_ASSERT_EQUAL(0x40, buf[0] & 0xE0); /* 0b010 prefix */
}

/** @test QPACK 解码：索引编码的头部 */
void test_qpack_decode_indexed_header(void) {
    /* Encode :method=GET - static index 2 */
    uint8_t encoded[16];
    int enc_n = qpack_encode_header(":method", "GET", encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, enc_n);

    /* Now decode it */
    qpack_decoded_t decoded;
    size_t consumed = 0;
    int rc = qpack_decode_header(encoded, (size_t)enc_n, &decoded, &consumed);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_TRUE(decoded.valid);
    TEST_ASSERT_EQUAL_STRING(":method", decoded.name);
    TEST_ASSERT_EQUAL_STRING("GET", decoded.value);
}

/** @test QPACK 解码：字面量编码的头部 */
void test_qpack_decode_literal_header(void) {
    /* Encode a literal field */
    uint8_t encoded[64];
    int enc_n = qpack_encode_header("x-test", "test-value", encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, enc_n);

    /* Decode it */
    qpack_decoded_t decoded;
    size_t consumed = 0;
    int rc = qpack_decode_header(encoded, (size_t)enc_n, &decoded, &consumed);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_TRUE(decoded.valid);
    TEST_ASSERT_EQUAL_STRING("x-test", decoded.name);
    TEST_ASSERT_EQUAL_STRING("test-value", decoded.value);
}

/** @test QPACK 编解码往返 */
void test_qpack_roundtrip(void) {
    const char *test_names[] = {
        ":method", ":path", ":scheme", ":authority",
        "content-type", "accept", "user-agent"
    };
    const char *test_values[] = {
        "GET", "/api/v1/test", "https", "example.com",
        "application/json", "*/*", "TestAgent/1.0"
    };
    int num = sizeof(test_names) / sizeof(test_names[0]);

    for (int i = 0; i < num; i++) {
        uint8_t encoded[128];
        int enc_n = qpack_encode_header(test_names[i], test_values[i],
                                         encoded, sizeof(encoded));
        TEST_ASSERT_GREATER_THAN_MESSAGE(0, enc_n, "编码失败");

        qpack_decoded_t decoded;
        size_t consumed = 0;
        int rc = qpack_decode_header(encoded, (size_t)enc_n, &decoded, &consumed);
        TEST_ASSERT_EQUAL_MESSAGE(0, rc, "解码失败");
        TEST_ASSERT_TRUE_MESSAGE(decoded.valid, "解码结果无效");
        TEST_ASSERT_EQUAL_STRING_MESSAGE(test_names[i], decoded.name, "名称不匹配");
        TEST_ASSERT_EQUAL_STRING_MESSAGE(test_values[i], decoded.value, "值不匹配");
    }
}

/** @test QPACK 编码完整请求头 */
void test_qpack_encode_request_headers(void) {
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = HTTP_GET;
    strncpy(req.path, "/index.html", sizeof(req.path) - 1);
    strncpy(req.headers[0].name, "host", sizeof(req.headers[0].name) - 1);
    strncpy(req.headers[0].value, "example.com", sizeof(req.headers[0].value) - 1);
    req.num_headers = 1;

    uint8_t buf[1024];
    int n = qpack_encode_request_headers(&req, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
}

/** @test QPACK 解码完整请求头 */
void test_qpack_decode_request_headers(void) {
    /* Build an encoded request */
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = HTTP_GET;
    strncpy(req.path, "/test", sizeof(req.path) - 1);
    strncpy(req.headers[0].name, "host", sizeof(req.headers[0].name) - 1);
    strncpy(req.headers[0].value, "example.com", sizeof(req.headers[0].value) - 1);
    req.num_headers = 1;

    uint8_t encoded[1024];
    int enc_n = qpack_encode_request_headers(&req, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, enc_n);

    /* Decode it */
    http_request_t decoded_req;
    int rc = qpack_decode_request_headers(encoded, (size_t)enc_n, &decoded_req);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL(HTTP_GET, decoded_req.method);
    TEST_ASSERT_EQUAL_STRING("/test", decoded_req.path);
    http_request_free(&decoded_req);
}

/** @test QPACK 编码响应头 */
void test_qpack_encode_response_headers(void) {
    http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status_code = 200;
    resp.content_type = "text/html";
    resp.content_length = 42;

    uint8_t buf[1024];
    int n = qpack_encode_response_headers(&resp, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
}

/** @test QPACK 解码响应头 */
void test_qpack_decode_response_headers(void) {
    /* Build an encoded response */
    http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status_code = 404;
    resp.content_type = "text/plain";
    resp.content_length = 0;

    uint8_t encoded[1024];
    int enc_n = qpack_encode_response_headers(&resp, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, enc_n);

    /* Decode it */
    http_response_t decoded_resp;
    int rc = qpack_decode_response_headers(encoded, (size_t)enc_n, &decoded_resp);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL(404, decoded_resp.status_code);

    if (decoded_resp.content_type) {
        free((void *)decoded_resp.content_type);
    }
}

/** @test QPACK 解码：无效输入 */
void test_qpack_decode_invalid(void) {
    qpack_decoded_t decoded;
    size_t consumed = 0;
    int rc = qpack_decode_header(NULL, 0, &decoded, &consumed);
    TEST_ASSERT_EQUAL(-1, rc);
}

/* ===== QUIC 流管理测试 ===== */

/** @test QUIC 流创建和查找 */
void test_quic_stream_create_and_find(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn = quic_connection_create(12345,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(conn);

    quic_stream_t *s = quic_stream_get_or_create(conn, 0);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL(0, s->stream_id);

    quic_stream_t *found = quic_stream_find(conn, 0);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_PTR(s, found);

    quic_connection_destroy(conn);
}

/** @test QUIC 流数据写入和读取 */
void test_quic_stream_write_read(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn = quic_connection_create(12345,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(conn);

    quic_stream_t *s = quic_stream_get_or_create(conn, 0);
    TEST_ASSERT_NOT_NULL(s);

    uint8_t data[] = "Hello, QUIC Stream!";
    int rc = quic_stream_write(s, data, sizeof(data));
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL(sizeof(data), s->recv_buf_len);

    uint8_t read_buf[64];
    ssize_t n = quic_stream_read(s, read_buf, sizeof(read_buf));
    TEST_ASSERT_EQUAL(sizeof(data), n);
    TEST_ASSERT_EQUAL_MEMORY(data, read_buf, sizeof(data));
    TEST_ASSERT_EQUAL(0, s->recv_buf_len);

    quic_connection_destroy(conn);
}

/** @test QUIC 流 FIN 标志 */
void test_quic_stream_fin(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn = quic_connection_create(12345,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(conn);

    quic_stream_t *s = quic_stream_get_or_create(conn, 0);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_FALSE(s->local_fin);

    quic_stream_set_fin(s);
    TEST_ASSERT_TRUE(s->local_fin);

    quic_connection_destroy(conn);
}

/** @test QUIC 流销毁 */
void test_quic_stream_destroy(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn = quic_connection_create(12345,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(conn);

    quic_stream_t *s = quic_stream_get_or_create(conn, 4);
    TEST_ASSERT_NOT_NULL(s);

    quic_stream_destroy(conn, s);
    quic_stream_t *found = quic_stream_find(conn, 4);
    TEST_ASSERT_NULL(found);

    quic_connection_destroy(conn);
}

/** @test QUIC 流：多个流 */
void test_quic_multiple_streams(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn = quic_connection_create(12345,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(conn);

    quic_stream_t *s0 = quic_stream_get_or_create(conn, 0);
    quic_stream_t *s4 = quic_stream_get_or_create(conn, 4);
    quic_stream_t *s8 = quic_stream_get_or_create(conn, 8);

    TEST_ASSERT_NOT_NULL(s0);
    TEST_ASSERT_NOT_NULL(s4);
    TEST_ASSERT_NOT_NULL(s8);

    TEST_ASSERT(s0 != s4);
    TEST_ASSERT(s4 != s8);

    quic_connection_destroy(conn);
}

/** @test QUIC 流：写入大数据 */
void test_quic_stream_large_write(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn = quic_connection_create(12345,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(conn);

    quic_stream_t *s = quic_stream_get_or_create(conn, 0);
    TEST_ASSERT_NOT_NULL(s);

    uint8_t *large_data = (uint8_t *)malloc(10000);
    TEST_ASSERT_NOT_NULL(large_data);
    memset(large_data, 0xAB, 10000);

    int rc = quic_stream_write(s, large_data, 10000);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL(10000, s->recv_buf_len);

    uint8_t *read_buf = (uint8_t *)malloc(10000);
    TEST_ASSERT_NOT_NULL(read_buf);
    ssize_t n = quic_stream_read(s, read_buf, 10000);
    TEST_ASSERT_EQUAL(10000, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(large_data, read_buf, 10000, "大数据读写不一致");

    free(large_data);
    free(read_buf);
    quic_connection_destroy(conn);
}

/* ===== QUIC 连接管理测试 ===== */

/** @test QUIC 连接创建和销毁 */
void test_quic_connection_create_destroy(void) {
    struct sockaddr_storage addr = {0};
    ((struct sockaddr_in *)&addr)->sin_family = AF_INET;

    quic_connection_t *conn = quic_connection_create(99999,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL(99999, conn->conn_id);
    TEST_ASSERT_FALSE(conn->handshake_complete);
    TEST_ASSERT_FALSE(conn->closed);

    quic_connection_destroy(conn);
}

/** @test QUIC 连接查找 */
void test_quic_find_connection(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn = quic_connection_create(11111,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(conn);

    quic_connection_t *found = quic_find_connection(11111);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_PTR(conn, found);

    quic_connection_t *not_found = quic_find_connection(99999);
    TEST_ASSERT_NULL(not_found);

    quic_connection_destroy(conn);
}

/** @test QUIC 连接计数 */
void test_quic_connection_count(void) {
    size_t before = quic_get_connection_count();

    struct sockaddr_storage addr = {0};
    quic_connection_t *c1 = quic_connection_create(100,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(c1);
    TEST_ASSERT_EQUAL(before + 1, quic_get_connection_count());

    quic_connection_t *c2 = quic_connection_create(200,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(c2);
    TEST_ASSERT_EQUAL(before + 2, quic_get_connection_count());

    quic_connection_destroy(c1);
    TEST_ASSERT_EQUAL(before + 1, quic_get_connection_count());

    quic_connection_destroy(c2);
    TEST_ASSERT_EQUAL(before, quic_get_connection_count());
}

/** @test QUIC 连接 ID 生成 */
void test_quic_generate_conn_id(void) {
    uint64_t id1 = quic_generate_conn_id();
    uint64_t id2 = quic_generate_conn_id();

    TEST_ASSERT_NOT_EQUAL(id1, id2);
    TEST_ASSERT_NOT_EQUAL(0, id1);
    TEST_ASSERT_NOT_EQUAL(0, id2);
}

/** @test QUIC 超时连接清理 */
void test_quic_cleanup_timeout_connections(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn = quic_connection_create(300,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(conn);
    size_t count_before = quic_get_connection_count();
    TEST_ASSERT_GREATER_THAN(0, count_before);

    /* 设置最后活动时间为很久以前 */
    conn->last_activity = 0;

    /* 清理超时连接（使用 1ms 超时） */
    quic_cleanup_timeout_connections(1);

    /* 连接应该被清理 */
    quic_connection_t *found = quic_find_connection(300);
    TEST_ASSERT_NULL(found);
}

/* ===== HTTP/3 会话管理测试 ===== */

/** @test HTTP/3 会话创建和销毁 */
void test_http3_session_create_destroy(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn = quic_connection_create(400,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(conn);

    http3_session_t *session = http3_session_create(conn);
    TEST_ASSERT_NOT_NULL(session);
    TEST_ASSERT_EQUAL(conn, session->conn);
    TEST_ASSERT_EQUAL(HTTP3_DEFAULT_MAX_FIELD_SECTION_SIZE,
                       session->max_field_section_size);

    http3_session_destroy(session);
    quic_connection_destroy(conn);
}

/** @test HTTP/3 多次会话创建 */
void test_http3_session_multiple(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn1 = quic_connection_create(500,
        COCOON_INVALID_SOCKET, &addr);
    quic_connection_t *conn2 = quic_connection_create(600,
        COCOON_INVALID_SOCKET, &addr);

    http3_session_t *s1 = http3_session_create(conn1);
    http3_session_t *s2 = http3_session_create(conn2);

    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT(s1 != s2);

    http3_session_destroy(s1);
    http3_session_destroy(s2);
    quic_connection_destroy(conn1);
    quic_connection_destroy(conn2);
}

/** @test HTTP/3 会话创建：NULL 连接 */
void test_http3_session_create_null(void) {
    http3_session_t *session = http3_session_create(NULL);
    TEST_ASSERT_NULL(session);
}

/* ===== 错误码测试 ===== */

/** @test HTTP/3 错误码值 */
void test_http3_error_codes(void) {
    TEST_ASSERT_EQUAL(0x0100, HTTP3_NO_ERROR);
    TEST_ASSERT_EQUAL(0x0101, HTTP3_GENERAL_PROTOCOL_ERROR);
    TEST_ASSERT_EQUAL(0x0102, HTTP3_INTERNAL_ERROR);
    TEST_ASSERT_EQUAL(0x0103, HTTP3_STREAM_CREATION_ERROR);
    TEST_ASSERT_EQUAL(0x0110, HTTP3_VERSION_FALLBACK);
}

/* ===== 时间戳测试 ===== */

/** @test 当前时间戳 */
void test_quic_current_time_ms(void) {
    uint64_t t1 = quic_current_time_ms();
    TEST_ASSERT_GREATER_THAN(0, t1);

    /* 应该是单调递增的（至少不减） */
    uint64_t t2 = quic_current_time_ms();
    TEST_ASSERT_GREATER_OR_EQUAL(t1, t2);
}

/* ===== 边界条件测试 ===== */

/** @test varint 编解码边界值 */
void test_varint_boundary_values(void) {
    uint64_t boundaries[] = {0, 63, 64, 16383, 16384, 1073741823ULL,
                              1073741824ULL, 4611686018427387903ULL};
    int num = sizeof(boundaries) / sizeof(boundaries[0]);

    for (int i = 0; i < num; i++) {
        uint8_t buf[8];
        size_t enc_n = http3_encode_varint(boundaries[i], buf);
        TEST_ASSERT_GREATER_THAN(0, enc_n);

        uint64_t decoded = 0;
        int dec_n = http3_decode_varint(buf, enc_n, &decoded);
        TEST_ASSERT_EQUAL(enc_n, (size_t)dec_n);
        TEST_ASSERT_EQUAL(boundaries[i], decoded);
    }
}

/** @test QUIC 流：NULL 参数处理 */
void test_quic_stream_null_params(void) {
    int rc = quic_stream_write(NULL, (uint8_t *)"test", 4);
    TEST_ASSERT_EQUAL(-1, rc);

    ssize_t n = quic_stream_read(NULL, NULL, 0);
    TEST_ASSERT_EQUAL(-1, n);

    quic_stream_set_fin(NULL); /* 不应崩溃 */
}

/** @test QUIC 连接：NULL 参数处理 */
void test_quic_connection_null(void) {
    quic_connection_destroy(NULL); /* 不应崩溃 */

    quic_stream_t *s = quic_stream_get_or_create(NULL, 0);
    TEST_ASSERT_NULL(s);

    quic_stream_t *f = quic_stream_find(NULL, 0);
    TEST_ASSERT_NULL(f);
}

/** @test http3_close_connection: NULL */
void test_http3_close_connection_null(void) {
    http3_close_connection(NULL, HTTP3_NO_ERROR); /* 不应崩溃 */
}

/** @test http3_send_error: NULL session */
void test_http3_send_error_null(void) {
    http3_send_error(NULL, 0, 404, "Not Found"); /* 不应崩溃 */
}

/** @test http3_send_settings: NULL */
void test_http3_send_settings_null(void) {
    int rc = http3_send_settings(NULL);
    TEST_ASSERT_EQUAL(-1, rc);
}

/** @test http3_process_datagram: NULL params */
void test_http3_process_datagram_null(void) {
    uint8_t data[] = "test";
    struct sockaddr_storage addr = {0};
    http3_process_datagram(COCOON_INVALID_SOCKET, NULL, 0, &addr);
    http3_process_datagram(COCOON_INVALID_SOCKET, data, 4, NULL);
}

/** @test quic_send_datagram: invalid socket */
void test_quic_send_datagram_invalid(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn = quic_connection_create(700,
        COCOON_INVALID_SOCKET, &addr);
    TEST_ASSERT_NOT_NULL(conn);

    uint8_t data[] = "test";
    int rc = quic_send_datagram(conn, data, 4);
    TEST_ASSERT_EQUAL(-1, rc);

    quic_connection_destroy(conn);
}

/** @test 帧解码：数据不足 */
void test_parse_frame_insufficient_data(void) {
    struct sockaddr_storage addr = {0};
    quic_connection_t *conn = quic_connection_create(800,
        COCOON_INVALID_SOCKET, &addr);
    quic_stream_t *stream = quic_stream_get_or_create(conn, 0);

    /* 写入不完整的帧数据 */
    uint8_t partial[] = {0x00}; /* 只有类型，没有长度 */
    quic_stream_write(stream, partial, 1);

    uint64_t ft = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rc = http3_parse_frame(stream, &ft, &payload, &payload_len);
    TEST_ASSERT_EQUAL(1, rc); /* 数据不足 */

    quic_connection_destroy(conn);
}

/* ===== main ===== */

int main(void) {
    UNITY_BEGIN();

    /* Variable-length integer encode */
    RUN_TEST(test_varint_encode_zero);
    RUN_TEST(test_varint_encode_63);
    RUN_TEST(test_varint_encode_64);
    RUN_TEST(test_varint_encode_16383);
    RUN_TEST(test_varint_encode_16384);
    RUN_TEST(test_varint_encode_4byte_max);
    RUN_TEST(test_varint_encode_8byte);
    RUN_TEST(test_varint_encode_max);
    RUN_TEST(test_varint_encode_null_buf);

    /* Variable-length integer decode */
    RUN_TEST(test_varint_decode_zero);
    RUN_TEST(test_varint_decode_63);
    RUN_TEST(test_varint_decode_64);
    RUN_TEST(test_varint_decode_16383);
    RUN_TEST(test_varint_decode_4byte);
    RUN_TEST(test_varint_decode_insufficient_data);
    RUN_TEST(test_varint_decode_null);
    RUN_TEST(test_varint_roundtrip);
    RUN_TEST(test_varint_boundary_values);

    /* Frame header */
    RUN_TEST(test_frame_header_size);
    RUN_TEST(test_frame_header_encode_decode);
    RUN_TEST(test_frame_header_decode_insufficient);
    RUN_TEST(test_frame_header_decode_null);

    /* Full frame encoding */
    RUN_TEST(test_full_frame_encode);
    RUN_TEST(test_encode_data_frame);
    RUN_TEST(test_encode_headers_frame);
    RUN_TEST(test_encode_settings_frame);
    RUN_TEST(test_encode_goaway_frame);

    /* QPACK encode */
    RUN_TEST(test_qpack_encode_static_match_method_get);
    RUN_TEST(test_qpack_encode_static_match_path);
    RUN_TEST(test_qpack_encode_static_match_scheme);
    RUN_TEST(test_qpack_encode_literal_field);
    RUN_TEST(test_qpack_encode_name_ref_value_literal);

    /* QPACK decode */
    RUN_TEST(test_qpack_decode_indexed_header);
    RUN_TEST(test_qpack_decode_literal_header);
    RUN_TEST(test_qpack_decode_invalid);

    /* QPACK roundtrip */
    RUN_TEST(test_qpack_roundtrip);
    RUN_TEST(test_qpack_encode_request_headers);
    RUN_TEST(test_qpack_decode_request_headers);
    RUN_TEST(test_qpack_encode_response_headers);
    RUN_TEST(test_qpack_decode_response_headers);

    /* QUIC stream management */
    RUN_TEST(test_quic_stream_create_and_find);
    RUN_TEST(test_quic_stream_write_read);
    RUN_TEST(test_quic_stream_fin);
    RUN_TEST(test_quic_stream_destroy);
    RUN_TEST(test_quic_multiple_streams);
    RUN_TEST(test_quic_stream_large_write);
    RUN_TEST(test_quic_stream_null_params);

    /* QUIC connection management */
    RUN_TEST(test_quic_connection_create_destroy);
    RUN_TEST(test_quic_find_connection);
    RUN_TEST(test_quic_connection_count);
    RUN_TEST(test_quic_generate_conn_id);
    RUN_TEST(test_quic_cleanup_timeout_connections);
    RUN_TEST(test_quic_connection_null);

    /* HTTP/3 session management */
    RUN_TEST(test_http3_session_create_destroy);
    RUN_TEST(test_http3_session_multiple);
    RUN_TEST(test_http3_session_create_null);

    /* Error codes */
    RUN_TEST(test_http3_error_codes);

    /* Time utilities */
    RUN_TEST(test_quic_current_time_ms);

    /* Boundary / NULL tests */
    RUN_TEST(test_http3_close_connection_null);
    RUN_TEST(test_http3_send_error_null);
    RUN_TEST(test_http3_send_settings_null);
    RUN_TEST(test_http3_process_datagram_null);
    RUN_TEST(test_quic_send_datagram_invalid);
    RUN_TEST(test_parse_frame_insufficient_data);

    return UNITY_END();
}

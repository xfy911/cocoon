#include "unity.h"
#include "grpc.h"
#include <string.h>
#include <stdlib.h>

/* ===== 辅助函数 ===== */

/**
 * fill_http_request - 填充 HTTP 请求结构体（用于测试）
 */
static void fill_http_request(http_request_t *req,
                              const char *path,
                              const char *content_type,
                              const uint8_t *body,
                              size_t body_len) {
    memset(req, 0, sizeof(http_request_t));
    if (path) {
        snprintf(req->path, sizeof(req->path), "%s", path);
    }
    if (content_type) {
        snprintf(req->content_type, sizeof(req->content_type), "%s", content_type);
        /* 同时加入 headers 数组 */
        snprintf(req->headers[0].name, sizeof(req->headers[0].name), "content-type");
        snprintf(req->headers[0].value, sizeof(req->headers[0].value), "%s", content_type);
        req->num_headers = 1;
    }
    if (body && body_len > 0) {
        req->body = (char *)malloc(body_len);
        memcpy(req->body, body, body_len);
        req->body_len = body_len;
    }
}

/**
 * make_grpc_frame - 构造 gRPC 消息帧（用于测试输入）
 *
 * @param compressed 压缩标志
 * @param payload    payload 数据
 * @param payload_len payload 长度
 * @param out_len    输出帧长度
 * @return 帧缓冲区（需由调用者释放）
 */
static uint8_t *make_grpc_frame(uint8_t compressed, const uint8_t *payload,
                                uint32_t payload_len, size_t *out_len) {
    *out_len = 5 + payload_len;
    uint8_t *frame = (uint8_t *)malloc(*out_len);
    frame[0] = compressed;
    frame[1] = (uint8_t)((payload_len >> 24) & 0xFF);
    frame[2] = (uint8_t)((payload_len >> 16) & 0xFF);
    frame[3] = (uint8_t)((payload_len >> 8) & 0xFF);
    frame[4] = (uint8_t)(payload_len & 0xFF);
    if (payload_len > 0) {
        memcpy(frame + 5, payload, payload_len);
    }
    return frame;
}

/* ===== grpc_detect ===== */

void test_detect_grpc_basic(void) {
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type), "application/grpc");
    TEST_ASSERT_TRUE(grpc_detect(&req));
}

void test_detect_grpc_proto(void) {
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type), "application/grpc+proto");
    TEST_ASSERT_TRUE(grpc_detect(&req));
}

void test_detect_grpc_json(void) {
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type), "application/grpc+json");
    TEST_ASSERT_TRUE(grpc_detect(&req));
}

void test_detect_grpc_with_charset(void) {
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type),
             "application/grpc; charset=utf-8");
    TEST_ASSERT_TRUE(grpc_detect(&req));
}

void test_detect_grpc_web(void) {
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type), "application/grpc-web");
    /* grpc_detect 应该也返回 true（因为 grpc-web 以 grpc 开头） */
    TEST_ASSERT_TRUE(grpc_detect(&req));
    /* grpc_is_grpc_web 应该返回 true */
    TEST_ASSERT_TRUE(grpc_is_grpc_web(&req));
}

void test_detect_grpc_web_proto(void) {
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type),
             "application/grpc-web+proto");
    TEST_ASSERT_TRUE(grpc_detect(&req));
    TEST_ASSERT_TRUE(grpc_is_grpc_web(&req));
}

void test_detect_not_grpc(void) {
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type), "application/json");
    TEST_ASSERT_FALSE(grpc_detect(&req));
}

void test_detect_text_plain(void) {
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type), "text/plain");
    TEST_ASSERT_FALSE(grpc_detect(&req));
}

void test_detect_grpc_uppercase(void) {
    /* Content-Type 检测应该是大小写不敏感的 */
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type), "APPLICATION/GRPC");
    TEST_ASSERT_TRUE(grpc_detect(&req));
}

void test_detect_null_request(void) {
    TEST_ASSERT_FALSE(grpc_detect(NULL));
}

void test_detect_empty_content_type(void) {
    http_request_t req = {0};
    /* content_type 为空字符串 */
    TEST_ASSERT_FALSE(grpc_detect(&req));
}

void test_detect_grpc_similar_prefix(void) {
    /* "application/grpc-like" 不应该匹配 */
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type), "application/grpc-like");
    TEST_ASSERT_FALSE(grpc_detect(&req));
}

/* ===== grpc_is_grpc_web ===== */

void test_is_grpc_web_direct(void) {
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type), "application/grpc-web");
    TEST_ASSERT_TRUE(grpc_is_grpc_web(&req));
}

void test_is_grpc_web_not_grpc(void) {
    http_request_t req = {0};
    snprintf(req.content_type, sizeof(req.content_type), "application/grpc");
    TEST_ASSERT_FALSE(grpc_is_grpc_web(&req));
}

void test_is_grpc_web_null(void) {
    TEST_ASSERT_FALSE(grpc_is_grpc_web(NULL));
}

/* ===== grpc_decode_message ===== */

void test_decode_simple_message(void) {
    const uint8_t payload[] = {0x0A, 0x04, 0x74, 0x65, 0x73, 0x74}; /* protobuf-like */
    size_t frame_len;
    uint8_t *frame = make_grpc_frame(0x00, payload, sizeof(payload), &frame_len);

    grpc_message_t msg = {0};
    int ret = grpc_decode_message(frame, frame_len, &msg);

    TEST_ASSERT_EQUAL_INT(5 + (int)sizeof(payload), ret);
    TEST_ASSERT_EQUAL_UINT8(0x00, msg.compressed);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), msg.length);
    TEST_ASSERT_NOT_NULL(msg.payload);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, msg.payload, sizeof(payload));

    grpc_message_free(&msg);
    free(frame);
}

void test_decode_empty_payload(void) {
    size_t frame_len;
    uint8_t *frame = make_grpc_frame(0x00, NULL, 0, &frame_len);

    grpc_message_t msg = {0};
    int ret = grpc_decode_message(frame, frame_len, &msg);

    TEST_ASSERT_EQUAL_INT(5, ret);
    TEST_ASSERT_EQUAL_UINT8(0x00, msg.compressed);
    TEST_ASSERT_EQUAL_UINT32(0, msg.length);
    TEST_ASSERT_NULL(msg.payload);

    grpc_message_free(&msg);
    free(frame);
}

void test_decode_compressed_flag(void) {
    const uint8_t payload[] = {0x01, 0x02, 0x03};
    size_t frame_len;
    uint8_t *frame = make_grpc_frame(0x01, payload, sizeof(payload), &frame_len);

    grpc_message_t msg = {0};
    int ret = grpc_decode_message(frame, frame_len, &msg);

    TEST_ASSERT_EQUAL_INT(5 + (int)sizeof(payload), ret);
    TEST_ASSERT_EQUAL_UINT8(0x01, msg.compressed);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), msg.length);

    grpc_message_free(&msg);
    free(frame);
}

void test_decode_large_payload(void) {
    /* 测试大 payload（1KB） */
    uint8_t *large_payload = (uint8_t *)malloc(1024);
    for (int i = 0; i < 1024; i++) large_payload[i] = (uint8_t)(i & 0xFF);

    size_t frame_len;
    uint8_t *frame = make_grpc_frame(0x00, large_payload, 1024, &frame_len);

    grpc_message_t msg = {0};
    int ret = grpc_decode_message(frame, frame_len, &msg);

    TEST_ASSERT_EQUAL_INT(5 + 1024, ret);
    TEST_ASSERT_EQUAL_UINT32(1024, msg.length);
    TEST_ASSERT_NOT_NULL(msg.payload);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(large_payload, msg.payload, 1024);

    grpc_message_free(&msg);
    free(frame);
    free(large_payload);
}

void test_decode_incomplete_header(void) {
    /* 只有 3 字节，不足 5 字节头部 */
    const uint8_t buf[] = {0x00, 0x00, 0x00};
    grpc_message_t msg = {0};
    int ret = grpc_decode_message(buf, sizeof(buf), &msg);

    TEST_ASSERT_EQUAL_INT(-1, ret);
    /* msg 不应该被修改（payload 为 NULL） */
    TEST_ASSERT_NULL(msg.payload);
}

void test_decode_incomplete_payload(void) {
    /* 头部完整但 payload 不完整 */
    const uint8_t buf[] = {0x00, 0x00, 0x00, 0x00, 0x10}; /* length=16, 但无 payload */
    grpc_message_t msg = {0};
    int ret = grpc_decode_message(buf, sizeof(buf), &msg);

    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_decode_null_buffer(void) {
    grpc_message_t msg = {0};
    int ret = grpc_decode_message(NULL, 10, &msg);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_decode_zero_length(void) {
    const uint8_t buf[] = {0x00, 0x00, 0x00, 0x00, 0x00};
    grpc_message_t msg = {0};
    int ret = grpc_decode_message(buf, 0, &msg);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_decode_null_message(void) {
    const uint8_t buf[] = {0x00, 0x00, 0x00, 0x00, 0x00};
    int ret = grpc_decode_message(buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ===== grpc_encode_message ===== */

void test_encode_simple_message(void) {
    const uint8_t payload[] = {0x0A, 0x04, 0x74, 0x65, 0x73, 0x74};
    grpc_message_t msg = {
        .compressed = 0x00,
        .length = sizeof(payload),
        .payload = (uint8_t *)payload
    };

    uint8_t buf[64];
    int ret = grpc_encode_message(&msg, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(5 + (int)sizeof(payload), ret);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[3]);
    TEST_ASSERT_EQUAL_UINT8((int)sizeof(payload), buf[4]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, buf + 5, sizeof(payload));
}

void test_encode_empty_payload(void) {
    grpc_message_t msg = {
        .compressed = 0x00,
        .length = 0,
        .payload = NULL
    };

    uint8_t buf[16];
    int ret = grpc_encode_message(&msg, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(5, ret);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[4]);
}

void test_encode_buffer_too_small(void) {
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    grpc_message_t msg = {
        .compressed = 0x00,
        .length = sizeof(payload),
        .payload = (uint8_t *)payload
    };

    uint8_t buf[8]; /* 太小，需要 10 字节 */
    int ret = grpc_encode_message(&msg, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_encode_null_params(void) {
    uint8_t buf[16];
    int ret = grpc_encode_message(NULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, ret);

    uint8_t payload[] = {0x01};
    grpc_message_t msg = {0};
    msg.payload = payload;
    msg.length = 1;
    ret = grpc_encode_message(&msg, NULL, 16);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_encode_exact_buffer_size(void) {
    /* 缓冲区大小恰好等于所需大小 */
    const uint8_t payload[] = {0xAB, 0xCD};
    grpc_message_t msg = {
        .compressed = 0x01,
        .length = sizeof(payload),
        .payload = (uint8_t *)payload
    };

    uint8_t buf[7]; /* 5 + 2 = 7 */
    int ret = grpc_encode_message(&msg, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(7, ret);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, buf + 5, sizeof(payload));
}

/* ===== grpc_parse_request ===== */

void test_parse_request_basic(void) {
    http_request_t req = {0};
    fill_http_request(&req, "/myPackage.MyService/MyMethod",
                      "application/grpc", NULL, 0);

    grpc_request_t grpc_req = {0};
    int ret = grpc_parse_request(&req, &grpc_req);

    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_STRING("myPackage.MyService", grpc_req.service_name);
    TEST_ASSERT_EQUAL_STRING("MyMethod", grpc_req.method_name);
    TEST_ASSERT_FALSE(grpc_req.is_grpc_web);
    TEST_ASSERT_FALSE(grpc_req.client_streaming);
    TEST_ASSERT_FALSE(grpc_req.server_streaming);

    grpc_request_free(&grpc_req);
    http_request_free(&req);
}

void test_parse_request_no_package(void) {
    http_request_t req = {0};
    fill_http_request(&req, "/MyService/MyMethod",
                      "application/grpc", NULL, 0);

    grpc_request_t grpc_req = {0};
    int ret = grpc_parse_request(&req, &grpc_req);

    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_STRING("MyService", grpc_req.service_name);
    TEST_ASSERT_EQUAL_STRING("MyMethod", grpc_req.method_name);

    grpc_request_free(&grpc_req);
    http_request_free(&req);
}

void test_parse_request_with_body(void) {
    const uint8_t payload[] = {0x08, 0x01, 0x12, 0x04, 0x74, 0x65, 0x73, 0x74};
    size_t frame_len;
    uint8_t *frame = make_grpc_frame(0x00, payload, sizeof(payload), &frame_len);

    http_request_t req = {0};
    fill_http_request(&req, "/TestService/Echo",
                      "application/grpc", frame, frame_len);

    grpc_request_t grpc_req = {0};
    int ret = grpc_parse_request(&req, &grpc_req);

    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_STRING("TestService", grpc_req.service_name);
    TEST_ASSERT_EQUAL_STRING("Echo", grpc_req.method_name);
    TEST_ASSERT_EQUAL_UINT8(0x00, grpc_req.message.compressed);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), grpc_req.message.length);
    TEST_ASSERT_NOT_NULL(grpc_req.message.payload);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, grpc_req.message.payload, sizeof(payload));

    grpc_request_free(&grpc_req);
    http_request_free(&req);
    free(frame);
}

void test_parse_request_grpc_web(void) {
    http_request_t req = {0};
    fill_http_request(&req, "/WebService/WebMethod",
                      "application/grpc-web", NULL, 0);

    grpc_request_t grpc_req = {0};
    int ret = grpc_parse_request(&req, &grpc_req);

    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_TRUE(grpc_req.is_grpc_web);

    grpc_request_free(&grpc_req);
    http_request_free(&req);
}

void test_parse_request_invalid_path_no_slash(void) {
    http_request_t req = {0};
    fill_http_request(&req, "invalid",
                      "application/grpc", NULL, 0);

    grpc_request_t grpc_req = {0};
    int ret = grpc_parse_request(&req, &grpc_req);

    TEST_ASSERT_EQUAL_INT(-1, ret);

    grpc_request_free(&grpc_req);
    http_request_free(&req);
}

void test_parse_request_empty_path(void) {
    http_request_t req = {0};
    fill_http_request(&req, "",
                      "application/grpc", NULL, 0);

    grpc_request_t grpc_req = {0};
    int ret = grpc_parse_request(&req, &grpc_req);

    TEST_ASSERT_EQUAL_INT(-1, ret);

    grpc_request_free(&grpc_req);
    http_request_free(&req);
}

void test_parse_request_null_params(void) {
    int ret = grpc_parse_request(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_parse_request_metadata(void) {
    http_request_t req = {0};
    fill_http_request(&req, "/Svc/Method",
                      "application/grpc", NULL, 0);
    /* 添加额外元数据头 */
    snprintf(req.headers[1].name, sizeof(req.headers[1].name), "x-request-id");
    snprintf(req.headers[1].value, sizeof(req.headers[1].value), "abc-123");
    snprintf(req.headers[2].name, sizeof(req.headers[2].name), "authorization");
    snprintf(req.headers[2].value, sizeof(req.headers[2].value), "Bearer token");
    req.num_headers = 3;

    grpc_request_t grpc_req = {0};
    int ret = grpc_parse_request(&req, &grpc_req);

    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_GREATER_THAN(0, (int)grpc_req.metadata_count);

    grpc_request_free(&grpc_req);
    http_request_free(&req);
}

/* ===== grpc_format_response_trailers ===== */

void test_format_trailers_ok(void) {
    grpc_request_t grpc_req = {0};
    grpc_req.status = GRPC_OK;
    snprintf(grpc_req.status_message, sizeof(grpc_req.status_message), "OK");

    char buf[256];
    int ret = grpc_format_response_trailers(&grpc_req, buf, sizeof(buf));

    TEST_ASSERT_GREATER_THAN(0, ret);
    TEST_ASSERT_TRUE(strstr(buf, "grpc-status: 0") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "grpc-message: OK") != NULL);
}

void test_format_trailers_error(void) {
    grpc_request_t grpc_req = {0};
    grpc_req.status = GRPC_NOT_FOUND;
    snprintf(grpc_req.status_message, sizeof(grpc_req.status_message),
             "Method not found");

    char buf[256];
    int ret = grpc_format_response_trailers(&grpc_req, buf, sizeof(buf));

    TEST_ASSERT_GREATER_THAN(0, ret);
    TEST_ASSERT_TRUE(strstr(buf, "grpc-status: 5") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "grpc-message: Method not found") != NULL);
}

void test_format_trailers_empty_message(void) {
    grpc_request_t grpc_req = {0};
    grpc_req.status = GRPC_OK;
    /* status_message 保持空 */

    char buf[256];
    int ret = grpc_format_response_trailers(&grpc_req, buf, sizeof(buf));

    TEST_ASSERT_GREATER_THAN(0, ret);
    TEST_ASSERT_TRUE(strstr(buf, "grpc-status: 0") != NULL);
}

void test_format_trailers_buffer_too_small(void) {
    grpc_request_t grpc_req = {0};
    grpc_req.status = GRPC_OK;
    snprintf(grpc_req.status_message, sizeof(grpc_req.status_message),
             "A very long message that will definitely not fit in a tiny buffer");

    char buf[10];
    int ret = grpc_format_response_trailers(&grpc_req, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_format_trailers_null_params(void) {
    char buf[256];
    int ret = grpc_format_response_trailers(NULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, ret);

    grpc_request_t grpc_req = {0};
    ret = grpc_format_response_trailers(&grpc_req, NULL, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ===== grpc_status_to_http ===== */

void test_status_to_http_ok(void) {
    TEST_ASSERT_EQUAL_INT(200, grpc_status_to_http(GRPC_OK));
}

void test_status_to_http_cancelled(void) {
    TEST_ASSERT_EQUAL_INT(499, grpc_status_to_http(GRPC_CANCELLED));
}

void test_status_to_http_unknown(void) {
    TEST_ASSERT_EQUAL_INT(500, grpc_status_to_http(GRPC_UNKNOWN));
}

void test_status_to_http_invalid_argument(void) {
    TEST_ASSERT_EQUAL_INT(400, grpc_status_to_http(GRPC_INVALID_ARGUMENT));
}

void test_status_to_http_deadline_exceeded(void) {
    TEST_ASSERT_EQUAL_INT(504, grpc_status_to_http(GRPC_DEADLINE_EXCEEDED));
}

void test_status_to_http_not_found(void) {
    TEST_ASSERT_EQUAL_INT(404, grpc_status_to_http(GRPC_NOT_FOUND));
}

void test_status_to_http_already_exists(void) {
    TEST_ASSERT_EQUAL_INT(409, grpc_status_to_http(GRPC_ALREADY_EXISTS));
}

void test_status_to_http_permission_denied(void) {
    TEST_ASSERT_EQUAL_INT(403, grpc_status_to_http(GRPC_PERMISSION_DENIED));
}

void test_status_to_http_resource_exhausted(void) {
    TEST_ASSERT_EQUAL_INT(429, grpc_status_to_http(GRPC_RESOURCE_EXHAUSTED));
}

void test_status_to_http_unimplemented(void) {
    TEST_ASSERT_EQUAL_INT(501, grpc_status_to_http(GRPC_UNIMPLEMENTED));
}

void test_status_to_http_unavailable(void) {
    TEST_ASSERT_EQUAL_INT(503, grpc_status_to_http(GRPC_UNAVAILABLE));
}

void test_status_to_http_unauthenticated(void) {
    TEST_ASSERT_EQUAL_INT(401, grpc_status_to_http(GRPC_UNAUTHENTICATED));
}

void test_status_to_http_internal(void) {
    TEST_ASSERT_EQUAL_INT(500, grpc_status_to_http(GRPC_INTERNAL));
}

void test_status_to_http_data_loss(void) {
    TEST_ASSERT_EQUAL_INT(500, grpc_status_to_http(GRPC_DATA_LOSS));
}

void test_status_to_http_all_codes(void) {
    /* 验证所有 17 个状态码都能映射（不崩溃） */
    for (int i = 0; i < GRPC_STATUS_MAX; i++) {
        int http = grpc_status_to_http((grpc_status_t)i);
        TEST_ASSERT_GREATER_THAN(0, http);
        TEST_ASSERT_LESS_THAN(600, http);
    }
}

/* ===== grpc_http_to_status ===== */

void test_http_to_status_200(void) {
    TEST_ASSERT_EQUAL_INT(GRPC_OK, grpc_http_to_status(200));
}

void test_http_to_status_400(void) {
    TEST_ASSERT_EQUAL_INT(GRPC_INVALID_ARGUMENT, grpc_http_to_status(400));
}

void test_http_to_status_401(void) {
    TEST_ASSERT_EQUAL_INT(GRPC_UNAUTHENTICATED, grpc_http_to_status(401));
}

void test_http_to_status_403(void) {
    TEST_ASSERT_EQUAL_INT(GRPC_PERMISSION_DENIED, grpc_http_to_status(403));
}

void test_http_to_status_404(void) {
    TEST_ASSERT_EQUAL_INT(GRPC_NOT_FOUND, grpc_http_to_status(404));
}

void test_http_to_status_409(void) {
    TEST_ASSERT_EQUAL_INT(GRPC_ABORTED, grpc_http_to_status(409));
}

void test_http_to_status_429(void) {
    TEST_ASSERT_EQUAL_INT(GRPC_RESOURCE_EXHAUSTED, grpc_http_to_status(429));
}

void test_http_to_status_500(void) {
    TEST_ASSERT_EQUAL_INT(GRPC_INTERNAL, grpc_http_to_status(500));
}

void test_http_to_status_501(void) {
    TEST_ASSERT_EQUAL_INT(GRPC_UNIMPLEMENTED, grpc_http_to_status(501));
}

void test_http_to_status_503(void) {
    TEST_ASSERT_EQUAL_INT(GRPC_UNAVAILABLE, grpc_http_to_status(503));
}

void test_http_to_status_504(void) {
    TEST_ASSERT_EQUAL_INT(GRPC_DEADLINE_EXCEEDED, grpc_http_to_status(504));
}

void test_http_to_status_2xx_success(void) {
    /* 201-299 都应映射到 OK */
    TEST_ASSERT_EQUAL_INT(GRPC_OK, grpc_http_to_status(201));
    TEST_ASSERT_EQUAL_INT(GRPC_OK, grpc_http_to_status(204));
    TEST_ASSERT_EQUAL_INT(GRPC_OK, grpc_http_to_status(299));
}

void test_http_to_status_4xx_default(void) {
    /* 未明确映射的 4xx 应返回 INVALID_ARGUMENT */
    TEST_ASSERT_EQUAL_INT(GRPC_INVALID_ARGUMENT, grpc_http_to_status(418));
}

/* ===== grpc_status_to_string ===== */

void test_status_to_string_all(void) {
    TEST_ASSERT_EQUAL_STRING("OK", grpc_status_to_string(GRPC_OK));
    TEST_ASSERT_EQUAL_STRING("CANCELLED", grpc_status_to_string(GRPC_CANCELLED));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", grpc_status_to_string(GRPC_UNKNOWN));
    TEST_ASSERT_EQUAL_STRING("INVALID_ARGUMENT",
                             grpc_status_to_string(GRPC_INVALID_ARGUMENT));
    TEST_ASSERT_EQUAL_STRING("DEADLINE_EXCEEDED",
                             grpc_status_to_string(GRPC_DEADLINE_EXCEEDED));
    TEST_ASSERT_EQUAL_STRING("NOT_FOUND", grpc_status_to_string(GRPC_NOT_FOUND));
    TEST_ASSERT_EQUAL_STRING("ALREADY_EXISTS",
                             grpc_status_to_string(GRPC_ALREADY_EXISTS));
    TEST_ASSERT_EQUAL_STRING("PERMISSION_DENIED",
                             grpc_status_to_string(GRPC_PERMISSION_DENIED));
    TEST_ASSERT_EQUAL_STRING("RESOURCE_EXHAUSTED",
                             grpc_status_to_string(GRPC_RESOURCE_EXHAUSTED));
    TEST_ASSERT_EQUAL_STRING("FAILED_PRECONDITION",
                             grpc_status_to_string(GRPC_FAILED_PRECONDITION));
    TEST_ASSERT_EQUAL_STRING("ABORTED", grpc_status_to_string(GRPC_ABORTED));
    TEST_ASSERT_EQUAL_STRING("OUT_OF_RANGE",
                             grpc_status_to_string(GRPC_OUT_OF_RANGE));
    TEST_ASSERT_EQUAL_STRING("UNIMPLEMENTED",
                             grpc_status_to_string(GRPC_UNIMPLEMENTED));
    TEST_ASSERT_EQUAL_STRING("INTERNAL",
                             grpc_status_to_string(GRPC_INTERNAL));
    TEST_ASSERT_EQUAL_STRING("UNAVAILABLE",
                             grpc_status_to_string(GRPC_UNAVAILABLE));
    TEST_ASSERT_EQUAL_STRING("DATA_LOSS",
                             grpc_status_to_string(GRPC_DATA_LOSS));
    TEST_ASSERT_EQUAL_STRING("UNAUTHENTICATED",
                             grpc_status_to_string(GRPC_UNAUTHENTICATED));
}

void test_status_to_string_unknown(void) {
    /* 非法状态码应返回 "UNKNOWN" */
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", grpc_status_to_string(99));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", grpc_status_to_string(GRPC_STATUS_MAX));
}

/* ===== grpc_message_free ===== */

void test_message_free_null(void) {
    /* 不应崩溃 */
    grpc_message_free(NULL);
}

void test_message_free_null_payload(void) {
    grpc_message_t msg = {0};
    msg.payload = NULL;
    msg.length = 0;
    /* 不应崩溃 */
    grpc_message_free(&msg);
}

void test_message_free_with_payload(void) {
    grpc_message_t msg = {0};
    msg.payload = (uint8_t *)malloc(16);
    msg.length = 16;
    msg.compressed = 1;

    grpc_message_free(&msg);

    TEST_ASSERT_NULL(msg.payload);
    TEST_ASSERT_EQUAL_UINT32(0, msg.length);
    TEST_ASSERT_EQUAL_UINT8(0, msg.compressed);
}

/* ===== grpc_request_free ===== */

void test_request_free_null(void) {
    /* 不应崩溃 */
    grpc_request_free(NULL);
}

void test_request_free_empty(void) {
    grpc_request_t grpc_req = {0};
    /* 不应崩溃 */
    grpc_request_free(&grpc_req);
}

void test_request_free_with_message(void) {
    grpc_request_t grpc_req = {0};
    grpc_req.message.payload = (uint8_t *)malloc(32);
    grpc_req.message.length = 32;
    grpc_req.message.compressed = 0;

    grpc_request_free(&grpc_req);

    TEST_ASSERT_NULL(grpc_req.message.payload);
    TEST_ASSERT_EQUAL_UINT32(0, grpc_req.message.length);
}

/* ===== grpc_error_response ===== */

void test_error_response_invalid_socket(void) {
    /* 无效 socket 不应崩溃 */
    grpc_error_response(COCOON_INVALID_SOCKET, 1, GRPC_INTERNAL, "error");
}

/* ===== 双向转换一致性 ===== */

void test_status_http_roundtrip(void) {
    /* 常见状态码的双向转换一致性 */
    grpc_status_t statuses[] = {
        GRPC_OK, GRPC_UNKNOWN, GRPC_INVALID_ARGUMENT, GRPC_NOT_FOUND,
        GRPC_PERMISSION_DENIED, GRPC_UNIMPLEMENTED, GRPC_UNAVAILABLE,
        GRPC_INTERNAL, GRPC_UNAUTHENTICATED
    };
    int http_codes[] = {200, 500, 400, 404, 403, 501, 503, 500, 401};

    for (size_t i = 0; i < sizeof(statuses)/sizeof(statuses[0]); i++) {
        int http = grpc_status_to_http(statuses[i]);
        TEST_ASSERT_EQUAL_INT(http_codes[i], http);

        grpc_status_t back = grpc_http_to_status(http);
        /* 由于多对一映射，反向不一定完全一致，但应合理 */
        TEST_ASSERT_GREATER_OR_EQUAL(GRPC_OK, (int)back);
        TEST_ASSERT_LESS_THAN(GRPC_STATUS_MAX, (int)back);
    }
}

/* ===== RPC 模式标记 ===== */

void test_rpc_mode_unary(void) {
    grpc_request_t grpc_req = {0};
    grpc_req.client_streaming = false;
    grpc_req.server_streaming = false;
    grpc_req.is_streaming = grpc_req.client_streaming || grpc_req.server_streaming;
    TEST_ASSERT_FALSE(grpc_req.is_streaming);
}

void test_rpc_mode_server_streaming(void) {
    grpc_request_t grpc_req = {0};
    grpc_req.client_streaming = false;
    grpc_req.server_streaming = true;
    grpc_req.is_streaming = grpc_req.client_streaming || grpc_req.server_streaming;
    TEST_ASSERT_TRUE(grpc_req.is_streaming);
}

void test_rpc_mode_client_streaming(void) {
    grpc_request_t grpc_req = {0};
    grpc_req.client_streaming = true;
    grpc_req.server_streaming = false;
    grpc_req.is_streaming = grpc_req.client_streaming || grpc_req.server_streaming;
    TEST_ASSERT_TRUE(grpc_req.is_streaming);
}

void test_rpc_mode_bidirectional(void) {
    grpc_request_t grpc_req = {0};
    grpc_req.client_streaming = true;
    grpc_req.server_streaming = true;
    grpc_req.is_streaming = grpc_req.client_streaming || grpc_req.server_streaming;
    TEST_ASSERT_TRUE(grpc_req.is_streaming);
}

/* ===== 边界条件：超长路径 ===== */

void test_parse_long_path(void) {
    /* 构造接近 256 字节限制的路径 */
    char long_svc[260];
    char path[300];
    memset(long_svc, 'A', 250);
    long_svc[250] = '\0';
    snprintf(path, sizeof(path), "/%s/MyMethod", long_svc);

    http_request_t req = {0};
    fill_http_request(&req, path, "application/grpc", NULL, 0);

    grpc_request_t grpc_req = {0};
    int ret = grpc_parse_request(&req, &grpc_req);

    TEST_ASSERT_EQUAL_INT(0, ret);
    /* service_name 被截断到 255 字符 */
    TEST_ASSERT_GREATER_THAN(0, strlen(grpc_req.service_name));
    TEST_ASSERT_EQUAL_STRING("MyMethod", grpc_req.method_name);

    grpc_request_free(&grpc_req);
    http_request_free(&req);
}

/* ===== 编码/解码对称性 ===== */

void test_encode_decode_symmetry(void) {
    /* 编码后再解码应得到原始数据 */
    const uint8_t payload[] = {
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0
    };
    grpc_message_t orig = {
        .compressed = 0x01,
        .length = sizeof(payload),
        .payload = (uint8_t *)payload
    };

    uint8_t buf[64];
    int encoded = grpc_encode_message(&orig, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, encoded);

    grpc_message_t decoded = {0};
    int decoded_len = grpc_decode_message(buf, (size_t)encoded, &decoded);

    TEST_ASSERT_EQUAL_INT(encoded, decoded_len);
    TEST_ASSERT_EQUAL_UINT8(orig.compressed, decoded.compressed);
    TEST_ASSERT_EQUAL_UINT32(orig.length, decoded.length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, decoded.payload, sizeof(payload));

    grpc_message_free(&decoded);
}

void test_encode_decode_empty(void) {
    /* 空 payload 编码/解码对称性 */
    grpc_message_t orig = {
        .compressed = 0x00,
        .length = 0,
        .payload = NULL
    };

    uint8_t buf[16];
    int encoded = grpc_encode_message(&orig, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(5, encoded);

    grpc_message_t decoded = {0};
    int decoded_len = grpc_decode_message(buf, (size_t)encoded, &decoded);

    TEST_ASSERT_EQUAL_INT(5, decoded_len);
    TEST_ASSERT_EQUAL_UINT8(0x00, decoded.compressed);
    TEST_ASSERT_EQUAL_UINT32(0, decoded.length);
    TEST_ASSERT_NULL(decoded.payload);

    grpc_message_free(&decoded);
}

/* ===== gRPC-Web 特殊处理 ===== */

void test_grpc_web_flag_set(void) {
    http_request_t req = {0};
    fill_http_request(&req, "/WebService/Method",
                      "application/grpc-web", NULL, 0);

    grpc_request_t grpc_req = {0};
    grpc_parse_request(&req, &grpc_req);

    TEST_ASSERT_TRUE(grpc_req.is_grpc_web);
    TEST_ASSERT_FALSE(grpc_req.client_streaming);
    TEST_ASSERT_FALSE(grpc_req.server_streaming);

    grpc_request_free(&grpc_req);
    http_request_free(&req);
}

/* ===== 主函数 ===== */

void setUp(void) {
    /* 每个测试前执行 */
}

void tearDown(void) {
    /* 每个测试后执行 */
}

int main(void) {
    UNITY_BEGIN();

    /* grpc_detect */
    RUN_TEST(test_detect_grpc_basic);
    RUN_TEST(test_detect_grpc_proto);
    RUN_TEST(test_detect_grpc_json);
    RUN_TEST(test_detect_grpc_with_charset);
    RUN_TEST(test_detect_grpc_web);
    RUN_TEST(test_detect_grpc_web_proto);
    RUN_TEST(test_detect_not_grpc);
    RUN_TEST(test_detect_text_plain);
    RUN_TEST(test_detect_grpc_uppercase);
    RUN_TEST(test_detect_null_request);
    RUN_TEST(test_detect_empty_content_type);
    RUN_TEST(test_detect_grpc_similar_prefix);

    /* grpc_is_grpc_web */
    RUN_TEST(test_is_grpc_web_direct);
    RUN_TEST(test_is_grpc_web_not_grpc);
    RUN_TEST(test_is_grpc_web_null);

    /* grpc_decode_message */
    RUN_TEST(test_decode_simple_message);
    RUN_TEST(test_decode_empty_payload);
    RUN_TEST(test_decode_compressed_flag);
    RUN_TEST(test_decode_large_payload);
    RUN_TEST(test_decode_incomplete_header);
    RUN_TEST(test_decode_incomplete_payload);
    RUN_TEST(test_decode_null_buffer);
    RUN_TEST(test_decode_zero_length);
    RUN_TEST(test_decode_null_message);

    /* grpc_encode_message */
    RUN_TEST(test_encode_simple_message);
    RUN_TEST(test_encode_empty_payload);
    RUN_TEST(test_encode_buffer_too_small);
    RUN_TEST(test_encode_null_params);
    RUN_TEST(test_encode_exact_buffer_size);

    /* grpc_parse_request */
    RUN_TEST(test_parse_request_basic);
    RUN_TEST(test_parse_request_no_package);
    RUN_TEST(test_parse_request_with_body);
    RUN_TEST(test_parse_request_grpc_web);
    RUN_TEST(test_parse_request_invalid_path_no_slash);
    RUN_TEST(test_parse_request_empty_path);
    RUN_TEST(test_parse_request_null_params);
    RUN_TEST(test_parse_request_metadata);

    /* grpc_format_response_trailers */
    RUN_TEST(test_format_trailers_ok);
    RUN_TEST(test_format_trailers_error);
    RUN_TEST(test_format_trailers_empty_message);
    RUN_TEST(test_format_trailers_buffer_too_small);
    RUN_TEST(test_format_trailers_null_params);

    /* grpc_status_to_http */
    RUN_TEST(test_status_to_http_ok);
    RUN_TEST(test_status_to_http_cancelled);
    RUN_TEST(test_status_to_http_unknown);
    RUN_TEST(test_status_to_http_invalid_argument);
    RUN_TEST(test_status_to_http_deadline_exceeded);
    RUN_TEST(test_status_to_http_not_found);
    RUN_TEST(test_status_to_http_already_exists);
    RUN_TEST(test_status_to_http_permission_denied);
    RUN_TEST(test_status_to_http_resource_exhausted);
    RUN_TEST(test_status_to_http_unimplemented);
    RUN_TEST(test_status_to_http_unavailable);
    RUN_TEST(test_status_to_http_unauthenticated);
    RUN_TEST(test_status_to_http_internal);
    RUN_TEST(test_status_to_http_data_loss);
    RUN_TEST(test_status_to_http_all_codes);

    /* grpc_http_to_status */
    RUN_TEST(test_http_to_status_200);
    RUN_TEST(test_http_to_status_400);
    RUN_TEST(test_http_to_status_401);
    RUN_TEST(test_http_to_status_403);
    RUN_TEST(test_http_to_status_404);
    RUN_TEST(test_http_to_status_409);
    RUN_TEST(test_http_to_status_429);
    RUN_TEST(test_http_to_status_500);
    RUN_TEST(test_http_to_status_501);
    RUN_TEST(test_http_to_status_503);
    RUN_TEST(test_http_to_status_504);
    RUN_TEST(test_http_to_status_2xx_success);
    RUN_TEST(test_http_to_status_4xx_default);

    /* grpc_status_to_string */
    RUN_TEST(test_status_to_string_all);
    RUN_TEST(test_status_to_string_unknown);

    /* grpc_message_free */
    RUN_TEST(test_message_free_null);
    RUN_TEST(test_message_free_null_payload);
    RUN_TEST(test_message_free_with_payload);

    /* grpc_request_free */
    RUN_TEST(test_request_free_null);
    RUN_TEST(test_request_free_empty);
    RUN_TEST(test_request_free_with_message);

    /* grpc_error_response */
    RUN_TEST(test_error_response_invalid_socket);

    /* 双向转换一致性 */
    RUN_TEST(test_status_http_roundtrip);

    /* RPC 模式标记 */
    RUN_TEST(test_rpc_mode_unary);
    RUN_TEST(test_rpc_mode_server_streaming);
    RUN_TEST(test_rpc_mode_client_streaming);
    RUN_TEST(test_rpc_mode_bidirectional);

    /* 边界条件 */
    RUN_TEST(test_parse_long_path);

    /* 编码/解码对称性 */
    RUN_TEST(test_encode_decode_symmetry);
    RUN_TEST(test_encode_decode_empty);

    /* gRPC-Web */
    RUN_TEST(test_grpc_web_flag_set);

    return UNITY_END();
}

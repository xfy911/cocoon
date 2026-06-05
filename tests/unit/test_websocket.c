/**
 * @file test_websocket.c
 * @brief WebSocket 协议单元测试
 *
 * 覆盖帧解析、帧编码、握手、广播注册表等核心逻辑。
 * 使用 socketpair 创建内部通信管道，避免真实网络依赖。
 */

#include "unity.h"
#include "websocket.h"
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

static int client_fd, server_fd;

void setUp(void) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        TEST_FAIL_MESSAGE("socketpair 失败");
    }
    client_fd = fds[0];
    server_fd = fds[1];
}

void tearDown(void) {
    close(client_fd);
    close(server_fd);
}

/* 测试 ws_parse_frame — 基本文本帧（无掩码） */
void test_parse_text_frame(void) {
    uint8_t data[] = { 0x81, 0x05, 'H', 'e', 'l', 'l', 'o' };
    ws_frame_t frame = {0};
    size_t consumed = 0;
    TEST_ASSERT_EQUAL_INT(0, ws_parse_frame(data, sizeof(data), &frame, &consumed));
    TEST_ASSERT_EQUAL_INT(7, consumed);
    TEST_ASSERT_TRUE(frame.fin);
    TEST_ASSERT_EQUAL_INT(WS_OP_TEXT, frame.opcode);
    TEST_ASSERT_FALSE(frame.masked);
    TEST_ASSERT_EQUAL_INT(5, frame.payload_len);
    TEST_ASSERT_EQUAL_STRING("Hello", (char *)frame.payload);
    ws_frame_free(&frame);
}

/* 测试 ws_parse_frame — 掩码帧（客户端发来的帧） */
void test_parse_masked_frame(void) {
    /* mask_key = {0x12, 0x34, 0x56, 0x78}
     * payload = XOR(0x41, 0x12) = 0x53 'S'
     *         = XOR(0x59, 0x34) = 0x6D 'm'
     *         = XOR(0x32, 0x56) = 0x64 'd'
     *         = XOR(0x0C, 0x78) = 0x74 't'
     *         = XOR(0x5D, 0x12) = 0x4F 'O'
     */
    uint8_t data[] = { 0x81, 0x85, 0x12, 0x34, 0x56, 0x78,
                       0x41, 0x59, 0x32, 0x0C, 0x5D };
    ws_frame_t frame = {0};
    size_t consumed = 0;
    TEST_ASSERT_EQUAL_INT(0, ws_parse_frame(data, sizeof(data), &frame, &consumed));
    TEST_ASSERT_EQUAL_INT(11, consumed);
    TEST_ASSERT_TRUE(frame.masked);
    TEST_ASSERT_EQUAL_INT(5, frame.payload_len);
    TEST_ASSERT_EQUAL_STRING("SmdtO", (char *)frame.payload);
    ws_frame_free(&frame);
}

/* 测试 ws_parse_frame — 数据不完整应返回 -1 */
void test_parse_incomplete_frame(void) {
    uint8_t data[] = { 0x81, 0x05, 'H' }; /* 需要 5 字节 payload，只有 1 字节 */
    ws_frame_t frame = {0};
    size_t consumed = 0;
    TEST_ASSERT_EQUAL_INT(-1, ws_parse_frame(data, sizeof(data), &frame, &consumed));
}

/* 测试 ws_parse_frame — 16 位扩展长度（126 ~ 65535） */
void test_parse_16bit_length(void) {
    uint8_t payload[130];
    memset(payload, 'A', 130);
    uint8_t data[134] = { 0x82, 0x7E, 0x00, 0x82 }; /* BINARY, 长度=130 */
    memcpy(data + 4, payload, 130);
    ws_frame_t frame = {0};
    size_t consumed = 0;
    TEST_ASSERT_EQUAL_INT(0, ws_parse_frame(data, sizeof(data), &frame, &consumed));
    TEST_ASSERT_EQUAL_INT(134, consumed);
    TEST_ASSERT_EQUAL_INT(WS_OP_BINARY, frame.opcode);
    TEST_ASSERT_EQUAL_INT(130, frame.payload_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(frame.payload, payload, 130));
    ws_frame_free(&frame);
}

/* 测试 ws_send_frame + ws_parse_frame 往返（文本帧） */
void test_send_parse_roundtrip_text(void) {
    const char *text = "Cocoon WebSocket";
    TEST_ASSERT_EQUAL_INT(0, ws_send_text(server_fd, text));

    uint8_t buf[64];
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    ws_frame_t frame = {0};
    size_t consumed = 0;
    TEST_ASSERT_EQUAL_INT(0, ws_parse_frame(buf, n, &frame, &consumed));
    TEST_ASSERT_TRUE(frame.fin);
    TEST_ASSERT_EQUAL_INT(WS_OP_TEXT, frame.opcode);
    TEST_ASSERT_EQUAL_INT(strlen(text), frame.payload_len);
    TEST_ASSERT_EQUAL_STRING(text, (char *)frame.payload);
    ws_frame_free(&frame);
}

/* 测试 ws_send_close — 带关闭码和原因 */
void test_send_close_with_reason(void) {
    TEST_ASSERT_EQUAL_INT(0, ws_send_close(server_fd, 1000, "bye"));
    uint8_t buf[32];
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    ws_frame_t frame = {0};
    size_t consumed = 0;
    TEST_ASSERT_EQUAL_INT(0, ws_parse_frame(buf, n, &frame, &consumed));
    TEST_ASSERT_EQUAL_INT(WS_OP_CLOSE, frame.opcode);
    TEST_ASSERT_EQUAL_INT(5, frame.payload_len); /* 2 字节 code + 3 字节 "bye" */
    ws_frame_free(&frame);
}

/* 测试 ws_send_ping / ws_send_pong */
void test_send_ping_pong(void) {
    /* Ping 帧（无负载） */
    TEST_ASSERT_EQUAL_INT(0, ws_send_ping(server_fd));
    uint8_t buf[16];
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    ws_frame_t frame = {0};
    size_t consumed = 0;
    TEST_ASSERT_EQUAL_INT(0, ws_parse_frame(buf, n, &frame, &consumed));
    TEST_ASSERT_EQUAL_INT(WS_OP_PING, frame.opcode);
    TEST_ASSERT_EQUAL_INT(0, frame.payload_len);
    ws_frame_free(&frame);

    /* Pong 帧（带负载） */
    uint8_t payload[] = { 0x01, 0x02, 0x03 };
    TEST_ASSERT_EQUAL_INT(0, ws_send_pong(server_fd, payload, 3));
    n = recv(client_fd, buf, sizeof(buf), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_INT(0, ws_parse_frame(buf, n, &frame, &consumed));
    TEST_ASSERT_EQUAL_INT(WS_OP_PONG, frame.opcode);
    TEST_ASSERT_EQUAL_INT(3, frame.payload_len);
    ws_frame_free(&frame);
}

/* 测试 ws_handshake — 验证响应头和 Sec-WebSocket-Accept */
void test_handshake_accept(void) {
    const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
    TEST_ASSERT_EQUAL_INT(0, ws_handshake(server_fd, key));

    char buf[512];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    buf[n] = '\0';

    TEST_ASSERT_NOT_NULL(strstr(buf, "HTTP/1.1 101 Switching Protocols"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Upgrade: websocket"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Connection: Upgrade"));
}

/* 测试 ws_connection_count — 初始为 0 */
void test_connection_count_initial(void) {
    TEST_ASSERT_EQUAL_INT(0, ws_connection_count());
}

/* 测试 ws_send_frame 大负载（64 位长度编码，>65535） */
void test_send_64bit_length(void) {
    uint8_t *payload = (uint8_t *)malloc(65536);
    TEST_ASSERT_NOT_NULL(payload);
    memset(payload, 0xAB, 65536);

    TEST_ASSERT_EQUAL_INT(0, ws_send_frame(server_fd, WS_OP_BINARY, payload, 65536));

    uint8_t *buf = (uint8_t *)malloc(65546);
    TEST_ASSERT_NOT_NULL(buf);
    ssize_t total = 0;
    while (total < 65546) {
        ssize_t n = recv(client_fd, buf + total, 65546 - total, 0);
        TEST_ASSERT_GREATER_THAN_INT(0, n);
        total += n;
    }

    ws_frame_t frame = {0};
    size_t consumed = 0;
    TEST_ASSERT_EQUAL_INT(0, ws_parse_frame(buf, total, &frame, &consumed));
    TEST_ASSERT_EQUAL_INT(WS_OP_BINARY, frame.opcode);
    TEST_ASSERT_EQUAL_INT(65536, frame.payload_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(frame.payload, payload, 65536));
    ws_frame_free(&frame);
    free(payload);
    free(buf);
}

/* 测试 ws_parse_frame — 多帧连续解析（模拟粘包） */
void test_parse_multiple_frames(void) {
    uint8_t frame1[] = { 0x81, 0x03, 'a', 'b', 'c' };           /* 文本 "abc" */
    uint8_t frame2[] = { 0x82, 0x02, 0x01, 0x02 };              /* 二进制 2 字节 */
    uint8_t data[16];
    memcpy(data, frame1, 5);
    memcpy(data + 5, frame2, 4);

    ws_frame_t frame = {0};
    size_t consumed = 0;
    size_t offset = 0;

    TEST_ASSERT_EQUAL_INT(0, ws_parse_frame(data + offset, 9 - offset, &frame, &consumed));
    TEST_ASSERT_EQUAL_INT(5, consumed);
    TEST_ASSERT_EQUAL_INT(WS_OP_TEXT, frame.opcode);
    TEST_ASSERT_EQUAL_STRING("abc", (char *)frame.payload);
    ws_frame_free(&frame);

    offset = 5;
    TEST_ASSERT_EQUAL_INT(0, ws_parse_frame(data + offset, 9 - offset, &frame, &consumed));
    TEST_ASSERT_EQUAL_INT(4, consumed);
    TEST_ASSERT_EQUAL_INT(WS_OP_BINARY, frame.opcode);
    TEST_ASSERT_EQUAL_INT(2, frame.payload_len);
    ws_frame_free(&frame);
}

/* 测试 ws_parse_frame — 空负载帧 */
void test_parse_empty_payload(void) {
    uint8_t data[] = { 0x88, 0x00 }; /* close 帧，无负载 */
    ws_frame_t frame = {0};
    size_t consumed = 0;
    TEST_ASSERT_EQUAL_INT(0, ws_parse_frame(data, sizeof(data), &frame, &consumed));
    TEST_ASSERT_EQUAL_INT(2, consumed);
    TEST_ASSERT_EQUAL_INT(WS_OP_CLOSE, frame.opcode);
    TEST_ASSERT_EQUAL_INT(0, frame.payload_len);
    TEST_ASSERT_NULL(frame.payload);
    ws_frame_free(&frame);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_text_frame);
    RUN_TEST(test_parse_masked_frame);
    RUN_TEST(test_parse_incomplete_frame);
    RUN_TEST(test_parse_16bit_length);
    RUN_TEST(test_send_parse_roundtrip_text);
    RUN_TEST(test_send_close_with_reason);
    RUN_TEST(test_send_ping_pong);
    RUN_TEST(test_handshake_accept);
    RUN_TEST(test_connection_count_initial);
    RUN_TEST(test_send_64bit_length);
    RUN_TEST(test_parse_multiple_frames);
    RUN_TEST(test_parse_empty_payload);
    return UNITY_END();
}

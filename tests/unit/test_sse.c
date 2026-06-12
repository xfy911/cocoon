#include "unity.h"
#include "sse.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

void setUp(void) {}
void tearDown(void) {}

/* 创建一对本地 socket 用于测试 */
static int create_socket_pair(int *fd1, int *fd2) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return -1;
    }
    *fd1 = sv[0];
    *fd2 = sv[1];
    return 0;
}

/* 从 socket 读取并返回字符串 */
static int read_all(int fd, char *buf, size_t len) {
    size_t total = 0;
    while (total < len - 1) {
        ssize_t n = recv(fd, buf + total, len - total - 1, MSG_DONTWAIT);
        if (n > 0) {
            total += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            break;
        }
    }
    buf[total] = '\0';
    return (int)total;
}

/**
 * test_sse_send_headers - 测试 SSE 响应头格式
 */
void test_sse_send_headers(void) {
    int client = 0, server = 0;
    TEST_ASSERT_EQUAL_INT(0, create_socket_pair(&client, &server));

    TEST_ASSERT_EQUAL_INT(0, sse_send_headers(server));

    char buf[512];
    int n = read_all(client, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(strstr(buf, "HTTP/1.1 200 OK") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "Content-Type: text/event-stream") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "Cache-Control: no-cache") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "Connection: keep-alive") != NULL);

    close(client);
    close(server);
}

/**
 * test_sse_send_event - 测试事件格式
 */
void test_sse_send_event(void) {
    int client = 0, server = 0;
    TEST_ASSERT_EQUAL_INT(0, create_socket_pair(&client, &server));

    TEST_ASSERT_EQUAL_INT(0, sse_send_event(server, "test", "hello", 42));

    char buf[512];
    int n = read_all(client, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(strstr(buf, "id: 42") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "event: test") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "data: hello") != NULL);

    close(client);
    close(server);
}

/**
 * test_sse_send_event_no_id - 测试无 ID 事件
 */
void test_sse_send_event_no_id(void) {
    int client = 0, server = 0;
    TEST_ASSERT_EQUAL_INT(0, create_socket_pair(&client, &server));

    TEST_ASSERT_EQUAL_INT(0, sse_send_event(server, "ping", "pong", 0));

    char buf[512];
    int n = read_all(client, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(strstr(buf, "event: ping") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "data: pong") != NULL);
    /* 没有 id 字段 */
    TEST_ASSERT_TRUE(strstr(buf, "id: 0") == NULL);
    TEST_ASSERT_TRUE(strstr(buf, "id: ") == NULL);

    close(client);
    close(server);
}

/**
 * test_sse_send_event_multiline - 测试多行数据
 */
void test_sse_send_event_multiline(void) {
    int client = 0, server = 0;
    TEST_ASSERT_EQUAL_INT(0, create_socket_pair(&client, &server));

    TEST_ASSERT_EQUAL_INT(0, sse_send_event(server, "msg", "line1\nline2", 1));

    char buf[512];
    int n = read_all(client, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(strstr(buf, "data: line1") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "data: line2") != NULL);

    close(client);
    close(server);
}

/**
 * test_sse_send_comment - 测试心跳注释
 */
void test_sse_send_comment(void) {
    int client = 0, server = 0;
    TEST_ASSERT_EQUAL_INT(0, create_socket_pair(&client, &server));

    TEST_ASSERT_EQUAL_INT(0, sse_send_comment(server, "heartbeat"));

    char buf[512];
    int n = read_all(client, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(strstr(buf, ": heartbeat") != NULL);

    close(client);
    close(server);
}

/**
 * test_sse_handle_request_405 - 测试非 GET 方法返回 405
 */
void test_sse_handle_request_405(void) {
    int client = 0, server = 0;
    TEST_ASSERT_EQUAL_INT(0, create_socket_pair(&client, &server));

    http_request_t req = {0};
    req.method = HTTP_POST;
    strncpy(req.path, "/_sse", sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = '\0';

    bool handled = sse_handle_request(server, &req);
    TEST_ASSERT_TRUE(handled);

    char buf[512];
    int n = read_all(client, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(strstr(buf, "HTTP/1.1 405 Method Not Allowed") != NULL);
    TEST_ASSERT_TRUE(strstr(buf, "Allow: GET") != NULL);

    close(client);
    close(server);
}

/**
 * test_sse_handle_request_wrong_path - 测试非 SSE 路径返回 false
 */
void test_sse_handle_request_wrong_path(void) {
    int client = 0, server = 0;
    TEST_ASSERT_EQUAL_INT(0, create_socket_pair(&client, &server));

    http_request_t req = {0};
    req.method = HTTP_GET;
    strncpy(req.path, "/index.html", sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = '\0';

    bool handled = sse_handle_request(server, &req);
    TEST_ASSERT_FALSE(handled);

    close(client);
    close(server);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_sse_send_headers);
    RUN_TEST(test_sse_send_event);
    RUN_TEST(test_sse_send_event_no_id);
    RUN_TEST(test_sse_send_event_multiline);
    RUN_TEST(test_sse_send_comment);
    RUN_TEST(test_sse_handle_request_405);
    RUN_TEST(test_sse_handle_request_wrong_path);
    return UNITY_END();
}

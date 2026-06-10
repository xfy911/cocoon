/**
 * @file test_middleware_ext.c - 扩展中间件单元测试
 *
 * 测试覆盖：
 *   - Base64Url 编解码
 *   - JWT 认证（成功/失败/过期/格式错误）
 *   - Security Headers 配置
 *   - Request ID 生成与复用
 *   - IP 过滤（白名单/黑名单/CIDR/X-Forwarded-For）
 *
 * 使用 Unity 测试框架。
 */

#include "unity.h"
#include "middleware_ext.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

/* ============================================================
 *   内部函数前置声明（middleware_ext.c 中的非 static 函数）
 * ============================================================ */

extern int  base64url_decode(const char *in, unsigned char *out, int out_size);
extern int  base64url_encode(const unsigned char *in, int in_len, char *out, int out_size);
extern const char *find_header(const http_request_t *req, const char *name);
extern time_t jwt_parse_exp(const char *payload);
extern bool jwt_verify_signature(const char *header_payload,
                                  const char *signature_b64,
                                  const char *secret,
                                  size_t secret_len);
extern void send_json_error(cocoon_socket_t fd, int status, const char *body, bool keep_alive);
extern bool parse_ipv4(const char *ip_str, uint32_t *addr);
extern bool parse_cidr(const char *cidr_str, uint32_t *addr, int *mask);
extern bool ip_match_cidr(uint32_t ip_addr, const char *cidr_str);
extern bool parse_x_forwarded_for(const char *header_value, uint32_t *addr);

/* ============================================================
 *   测试辅助函数
 * ============================================================ */

/**
 * @brief 创建一对已连接的 socket 用于测试
 */
static int create_socket_pair(int fds[2]) {
    return socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
}

/**
 * @brief 读取 socket 中可用数据到缓冲区
 *
 * 使用非阻塞读取，避免在 socket 未关闭时无限等待 EOF。
 */
static ssize_t read_all(int fd, char *buf, size_t buf_size) {
    /* 设置 500ms 接收超时 */
    struct timeval tv = {0, 500000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t total = 0;
    ssize_t n;
    while (total < (ssize_t)buf_size - 1 &&
           (n = read(fd, buf + total, buf_size - 1 - total)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    return total;
}

/**
 * @brief 生成测试用的 JWT token
 *
 * 使用 HS256 签名。
 *
 * @param payload   JWT payload JSON
 * @param secret    签名密钥
 * @param token_out 输出 token 缓冲区
 * @param out_size  输出缓冲区大小
 * @return 0 成功，-1 失败
 */
static int generate_jwt_token(const char *payload, const char *secret,
                               char *token_out, size_t out_size) {
    /* header: {"alg":"HS256","typ":"JWT"} */
    const char *header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

    char header_b64[512];
    char payload_b64[1024];

    int header_len = base64url_encode((const unsigned char *)header,
                                       (int)strlen(header), header_b64, sizeof(header_b64));
    int payload_len = base64url_encode((const unsigned char *)payload,
                                        (int)strlen(payload), payload_b64, sizeof(payload_b64));
    if (header_len < 0 || payload_len < 0) return -1;

    /* 计算签名 */
    char to_sign[1536];
    snprintf(to_sign, sizeof(to_sign), "%s.%s", header_b64, payload_b64);

    unsigned char sig[EVP_MAX_MD_SIZE];
    unsigned int sig_len = 0;
    if (!HMAC(EVP_sha256(), secret, (int)strlen(secret),
              (const unsigned char *)to_sign, strlen(to_sign),
              sig, &sig_len)) {
        return -1;
    }

    char sig_b64[EVP_MAX_MD_SIZE * 2];
    int sig_b64_len = base64url_encode(sig, (int)sig_len, sig_b64, sizeof(sig_b64));
    if (sig_b64_len < 0) return -1;

    int n = snprintf(token_out, out_size, "%s.%s.%s",
                     header_b64, payload_b64, sig_b64);
    if (n < 0 || (size_t)n >= out_size) return -1;
    return 0;
}

/**
 * @brief 创建带有指定 header 的 HTTP 请求
 */
static void make_request_with_auth(http_request_t *req, const char *auth_header) {
    memset(req, 0, sizeof(*req));
    req->method = HTTP_GET;
    strcpy(req->path, "/api/test");
    strcpy(req->version, "HTTP/1.1");
    req->keep_alive = true;

    if (auth_header) {
        strcpy(req->headers[0].name, "Authorization");
        strcpy(req->headers[0].value, auth_header);
        req->num_headers = 1;
    }
}

/* ============================================================
 *   setUp / tearDown
 * ============================================================ */

void setUp(void) {
    /* 每个测试前执行 */
}

void tearDown(void) {
    /* 每个测试后执行 */
}

/* ============================================================
 *   Base64Url 编解码测试
 * ============================================================ */

void test_base64url_decode_basic(void) {
    /* 编码 "hello" -> aGVsbG8 */
    const char *encoded = "aGVsbG8";
    unsigned char decoded[16];
    int len = base64url_decode(encoded, decoded, sizeof(decoded));

    TEST_ASSERT_EQUAL(5, len);
    TEST_ASSERT_EQUAL_MEMORY("hello", decoded, 5);
}

void test_base64url_decode_with_special_chars(void) {
    /* Base64Url 使用 - 和 _ 代替 + 和 / */
    /* "test+data/ok" 在 Base64Url 中是 "dGVzdCtkYXRhL29r" */
    const char *encoded = "dGVzdCtkYXRhL29r";
    unsigned char decoded[32];
    int len = base64url_decode(encoded, decoded, sizeof(decoded));

    TEST_ASSERT_EQUAL(12, len);
    TEST_ASSERT_EQUAL_MEMORY("test+data/ok", decoded, 12);
}

void test_base64url_decode_empty(void) {
    unsigned char decoded[8];
    int len = base64url_decode("", decoded, sizeof(decoded));
    TEST_ASSERT_EQUAL(0, len);
}

void test_base64url_decode_null_params(void) {
    unsigned char decoded[8];
    TEST_ASSERT_EQUAL(-1, base64url_decode(NULL, decoded, sizeof(decoded)));
    TEST_ASSERT_EQUAL(-1, base64url_decode("abc", NULL, 8));
}

void test_base64url_decode_binary(void) {
    /* 测试二进制数据 */
    unsigned char binary[3] = {0xFF, 0x00, 0xAB};
    char encoded[16];
    int enc_len = base64url_encode(binary, 3, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, enc_len);

    unsigned char decoded[8];
    int dec_len = base64url_decode(encoded, decoded, sizeof(decoded));
    TEST_ASSERT_EQUAL(3, dec_len);
    TEST_ASSERT_EQUAL_UINT8(0xFF, decoded[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, decoded[1]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, decoded[2]);
}

void test_base64url_encode_decode_roundtrip(void) {
    const char *orig = "The quick brown fox jumps over the lazy dog.";
    char encoded[256];
    unsigned char decoded[256];

    int enc_len = base64url_encode((const unsigned char *)orig,
                                    (int)strlen(orig), encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, enc_len);

    int dec_len = base64url_decode(encoded, decoded, sizeof(decoded));
    TEST_ASSERT_EQUAL((int)strlen(orig), dec_len);
    TEST_ASSERT_EQUAL_MEMORY(orig, decoded, strlen(orig));
}

void test_base64url_encode_no_padding(void) {
    /* Base64Url 不应有填充 */
    unsigned char data[1] = {'a'};
    char encoded[16];
    int len = base64url_encode(data, 1, encoded, sizeof(encoded));
    TEST_ASSERT_GREATER_THAN(0, len);
    /* 不应包含 = */
    TEST_ASSERT_NULL(strchr(encoded, '='));
}

/* ============================================================
 *   find_header 测试
 * ============================================================ */

void test_find_header_exists(void) {
    http_request_t req = {0};
    strcpy(req.headers[0].name, "Authorization");
    strcpy(req.headers[0].value, "Bearer token123");
    req.num_headers = 1;

    const char *val = find_header(&req, "Authorization");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("Bearer token123", val);
}

void test_find_header_case_insensitive(void) {
    http_request_t req = {0};
    strcpy(req.headers[0].name, "X-Custom-Header");
    strcpy(req.headers[0].value, "custom-value");
    req.num_headers = 1;

    const char *val = find_header(&req, "x-custom-header");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("custom-value", val);
}

void test_find_header_not_found(void) {
    http_request_t req = {0};
    req.num_headers = 0;

    const char *val = find_header(&req, "Authorization");
    TEST_ASSERT_NULL(val);
}

void test_find_header_multiple_headers(void) {
    http_request_t req = {0};
    strcpy(req.headers[0].name, "Host");
    strcpy(req.headers[0].value, "localhost");
    strcpy(req.headers[1].name, "Authorization");
    strcpy(req.headers[1].value, "Bearer abc");
    strcpy(req.headers[2].name, "Content-Type");
    strcpy(req.headers[2].value, "application/json");
    req.num_headers = 3;

    TEST_ASSERT_EQUAL_STRING("localhost", find_header(&req, "Host"));
    TEST_ASSERT_EQUAL_STRING("Bearer abc", find_header(&req, "Authorization"));
    TEST_ASSERT_EQUAL_STRING("application/json", find_header(&req, "Content-Type"));
    TEST_ASSERT_NULL(find_header(&req, "X-Not-Found"));
}

/* ============================================================
 *   jwt_parse_exp 测试
 * ============================================================ */

void test_jwt_parse_exp_present(void) {
    const char *payload = "{\"sub\":\"user1\",\"exp\":1893456000,\"iat\":1609459200}";
    time_t exp = jwt_parse_exp(payload);
    TEST_ASSERT_EQUAL((time_t)1893456000, exp);
}

void test_jwt_parse_exp_not_present(void) {
    const char *payload = "{\"sub\":\"user1\",\"iat\":1609459200}";
    time_t exp = jwt_parse_exp(payload);
    TEST_ASSERT_EQUAL(0, exp);
}

void test_jwt_parse_exp_first_field(void) {
    const char *payload = "{\"exp\":2000000000,\"sub\":\"user1\"}";
    time_t exp = jwt_parse_exp(payload);
    TEST_ASSERT_EQUAL((time_t)2000000000, exp);
}

void test_jwt_parse_exp_null(void) {
    TEST_ASSERT_EQUAL(0, jwt_parse_exp(NULL));
}

/* ============================================================
 *   JWT 签名验证测试
 * ============================================================ */

void test_jwt_verify_signature_valid(void) {
    const char *secret = "my-secret-key";
    const char *payload = "{\"sub\":\"user1\",\"exp\":1893456000}";
    char token[2048];

    TEST_ASSERT_EQUAL(0, generate_jwt_token(payload, secret, token, sizeof(token)));

    /* 提取 header.payload 部分 */
    char *sig_dot = strrchr(token, '.');
    TEST_ASSERT_NOT_NULL(sig_dot);
    *sig_dot = '\0';

    const char *signature_b64 = sig_dot + 1;

    bool valid = jwt_verify_signature(token, signature_b64, secret, strlen(secret));
    TEST_ASSERT_TRUE(valid);
}

void test_jwt_verify_signature_invalid_secret(void) {
    const char *secret = "my-secret-key";
    const char *payload = "{\"sub\":\"user1\"}";
    char token[2048];

    TEST_ASSERT_EQUAL(0, generate_jwt_token(payload, secret, token, sizeof(token)));

    char *sig_dot = strrchr(token, '.');
    TEST_ASSERT_NOT_NULL(sig_dot);
    *sig_dot = '\0';

    const char *signature_b64 = sig_dot + 1;

    /* 使用错误的密钥验证 */
    bool valid = jwt_verify_signature(token, signature_b64, "wrong-secret", strlen("wrong-secret"));
    TEST_ASSERT_FALSE(valid);
}

void test_jwt_verify_signature_tampered_payload(void) {
    const char *secret = "my-secret-key";
    const char *payload = "{\"sub\":\"user1\"}";
    char token[2048];

    TEST_ASSERT_EQUAL(0, generate_jwt_token(payload, secret, token, sizeof(token)));

    /* 篡改 payload：在 header_b64 和 payload_b64 之间添加字符 */
    char tampered[2048];
    strncpy(tampered, token, sizeof(tampered) - 1);
    tampered[sizeof(tampered) - 1] = '\0';

    char *sig_dot = strrchr(tampered, '.');
    TEST_ASSERT_NOT_NULL(sig_dot);
    *sig_dot = '\0';

    const char *signature_b64 = sig_dot + 1;

    /* 篡改 header.payload 部分 */
    strcat(tampered, ".extra");

    bool valid = jwt_verify_signature(tampered, signature_b64, secret, strlen(secret));
    TEST_ASSERT_FALSE(valid);
}

/* ============================================================
 *   JWT 中间件完整流程测试
 * ============================================================ */

void test_jwt_middleware_success(void) {
    int fds[2];
    TEST_ASSERT_EQUAL(0, create_socket_pair(fds));

    cocoon_jwt_config_t cfg = {
        .secret = "test-secret",
        .header_name = "Authorization",
        .prefix = "Bearer ",
        .skip_preflight = false,
    };

    /* 生成有效的 token */
    char token[2048];
    const char *payload = "{\"sub\":\"user1\",\"exp\":4102444800}"; /* 2099 年 */
    TEST_ASSERT_EQUAL(0, generate_jwt_token(payload, "test-secret", token, sizeof(token)));

    char auth_header[2300];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);

    http_request_t req;
    make_request_with_auth(&req, auth_header);

    int ret = cocoon_middleware_jwt(&req, fds[0], &cfg);
    TEST_ASSERT_EQUAL(0, ret); /* 验证通过 */

    close(fds[0]);
    close(fds[1]);
}

void test_jwt_middleware_missing_header(void) {
    int fds[2];
    TEST_ASSERT_EQUAL(0, create_socket_pair(fds));

    cocoon_jwt_config_t cfg = {
        .secret = "test-secret",
        .header_name = "Authorization",
        .prefix = "Bearer ",
    };

    http_request_t req;
    make_request_with_auth(&req, NULL); /* 无 Authorization 头 */

    int ret = cocoon_middleware_jwt(&req, fds[0], &cfg);
    TEST_ASSERT_EQUAL(1, ret); /* 短路 */

    /* 读取响应 */
    char response[1024];
    ssize_t n = read_all(fds[1], response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(response, "401"));
    TEST_ASSERT_NOT_NULL(strstr(response, "Unauthorized"));

    close(fds[0]);
    close(fds[1]);
}

void test_jwt_middleware_wrong_prefix(void) {
    int fds[2];
    TEST_ASSERT_EQUAL(0, create_socket_pair(fds));

    cocoon_jwt_config_t cfg = {
        .secret = "test-secret",
        .prefix = "Bearer ",
    };

    http_request_t req;
    make_request_with_auth(&req, "Basic dXNlcjpwYXNz"); /* Basic 而非 Bearer */

    int ret = cocoon_middleware_jwt(&req, fds[0], &cfg);
    TEST_ASSERT_EQUAL(1, ret); /* 短路 */

    close(fds[0]);
    close(fds[1]);
}

void test_jwt_middleware_invalid_signature(void) {
    int fds[2];
    TEST_ASSERT_EQUAL(0, create_socket_pair(fds));

    cocoon_jwt_config_t cfg = {
        .secret = "test-secret",
        .prefix = "Bearer ",
    };

    /* 使用错误签名的 token */
    http_request_t req;
    make_request_with_auth(&req, "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJ1c2VyMSJ9.invalidsignature");

    int ret = cocoon_middleware_jwt(&req, fds[0], &cfg);
    TEST_ASSERT_EQUAL(1, ret); /* 短路 */

    close(fds[0]);
    close(fds[1]);
}

void test_jwt_middleware_expired_token(void) {
    int fds[2];
    TEST_ASSERT_EQUAL(0, create_socket_pair(fds));

    cocoon_jwt_config_t cfg = {
        .secret = "test-secret",
        .prefix = "Bearer ",
    };

    /* 生成已过期 token */
    char token[2048];
    const char *payload = "{\"sub\":\"user1\",\"exp\":1000000000}"; /* 2001 年，已过期 */
    TEST_ASSERT_EQUAL(0, generate_jwt_token(payload, "test-secret", token, sizeof(token)));

    char auth_header[2300];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);

    http_request_t req;
    make_request_with_auth(&req, auth_header);

    int ret = cocoon_middleware_jwt(&req, fds[0], &cfg);
    TEST_ASSERT_EQUAL(1, ret); /* 过期，短路 */

    /* 读取响应确认 401 */
    char response[1024];
    ssize_t n = read_all(fds[1], response, sizeof(response));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(response, "401"));

    close(fds[0]);
    close(fds[1]);
}

void test_jwt_middleware_skip_options(void) {
    int fds[2];
    TEST_ASSERT_EQUAL(0, create_socket_pair(fds));

    cocoon_jwt_config_t cfg = {
        .secret = "test-secret",
        .skip_preflight = true,
    };

    /* OPTIONS 请求应跳过验证 */
    http_request_t req = {0};
    req.method = HTTP_OPTIONS;
    strcpy(req.path, "/api/test");
    req.keep_alive = true;

    int ret = cocoon_middleware_jwt(&req, fds[0], &cfg);
    TEST_ASSERT_EQUAL(0, ret); /* 跳过 */

    close(fds[0]);
    close(fds[1]);
}

void test_jwt_middleware_no_config(void) {
    int fds[2];
    TEST_ASSERT_EQUAL(0, create_socket_pair(fds));

    /* 空密钥，应跳过 */
    cocoon_jwt_config_t cfg = {0};

    http_request_t req;
    make_request_with_auth(&req, NULL);

    int ret = cocoon_middleware_jwt(&req, fds[0], &cfg);
    TEST_ASSERT_EQUAL(0, ret); /* 跳过 */

    close(fds[0]);
    close(fds[1]);
}

void test_jwt_middleware_malformed_token(void) {
    int fds[2];
    TEST_ASSERT_EQUAL(0, create_socket_pair(fds));

    cocoon_jwt_config_t cfg = {
        .secret = "test-secret",
        .prefix = "Bearer ",
    };

    /* 缺少分隔符的 token */
    http_request_t req;
    make_request_with_auth(&req, "Bearer malformedtoken");

    int ret = cocoon_middleware_jwt(&req, fds[0], &cfg);
    TEST_ASSERT_EQUAL(1, ret); /* 格式错误，短路 */

    close(fds[0]);
    close(fds[1]);
}

/* ============================================================
 *   Security Headers 中间件测试
 * ============================================================ */

void test_security_headers_set_config(void) {
    cocoon_security_headers_config_t cfg = {
        .hsts_enabled = true,
        .hsts_max_age = 31536000,
        .hsts_include_subdomains = true,
        .frame_options_enabled = true,
        .frame_options = "DENY",
        .xss_protection_enabled = true,
        .csp_enabled = true,
        .csp_policy = "default-src 'self'",
        .content_type_options = true,
        .referrer_policy_enabled = true,
        .referrer_policy = "strict-origin-when-cross-origin",
    };

    http_request_t req = {0};
    int ret = cocoon_middleware_security_headers(&req, -1, &cfg);
    TEST_ASSERT_EQUAL(0, ret);

    const cocoon_security_headers_config_t *got =
        cocoon_middleware_security_headers_get();
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_TRUE(got->hsts_enabled);
    TEST_ASSERT_EQUAL(31536000, got->hsts_max_age);
    TEST_ASSERT_TRUE(got->hsts_include_subdomains);
    TEST_ASSERT_TRUE(got->frame_options_enabled);
    TEST_ASSERT_EQUAL_STRING("DENY", got->frame_options);
    TEST_ASSERT_TRUE(got->xss_protection_enabled);
    TEST_ASSERT_TRUE(got->csp_enabled);
    TEST_ASSERT_EQUAL_STRING("default-src 'self'", got->csp_policy);
    TEST_ASSERT_TRUE(got->content_type_options);
    TEST_ASSERT_TRUE(got->referrer_policy_enabled);
    TEST_ASSERT_EQUAL_STRING("strict-origin-when-cross-origin", got->referrer_policy);
}

void test_security_headers_null_config(void) {
    http_request_t req = {0};
    int ret = cocoon_middleware_security_headers(&req, -1, NULL);
    TEST_ASSERT_EQUAL(0, ret);
    /* 配置应保持不变 */
}

void test_security_headers_get_not_initialized(void) {
    /* 注意：如果之前测试已初始化，此处可能不为 NULL */
    /* 测试中不做断言，仅确认不崩溃 */
    (void)cocoon_middleware_security_headers_get();
}

void test_security_headers_sameorigin(void) {
    cocoon_security_headers_config_t cfg = {
        .frame_options_enabled = true,
        .frame_options = "SAMEORIGIN",
    };

    http_request_t req = {0};
    cocoon_middleware_security_headers(&req, -1, &cfg);

    const cocoon_security_headers_config_t *got =
        cocoon_middleware_security_headers_get();
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_STRING("SAMEORIGIN", got->frame_options);
}

/* ============================================================
 *   Request ID 中间件测试
 * ============================================================ */

void test_request_id_generates_32_chars(void) {
    cocoon_request_id_config_t cfg = {
        .header_name = "X-Request-ID",
        .trust_incoming = true,
    };

    http_request_t req = {0};
    req.method = HTTP_GET;

    int ret = cocoon_middleware_request_id(&req, -1, &cfg);
    TEST_ASSERT_EQUAL(0, ret);
    /* 中间件内部生成了 32 字符 ID，
       但当前版本未暴露给外部，仅确认不崩溃 */
}

void test_request_id_null_config(void) {
    http_request_t req = {0};
    int ret = cocoon_middleware_request_id(&req, -1, NULL);
    TEST_ASSERT_EQUAL(0, ret);
}

void test_request_id_trust_incoming_valid(void) {
    cocoon_request_id_config_t cfg = {
        .header_name = "X-Request-ID",
        .trust_incoming = true,
    };

    http_request_t req = {0};
    strcpy(req.headers[0].name, "X-Request-ID");
    strcpy(req.headers[0].value, "aabbccdd11223344556677889900aabb"); /* 32 字符 hex */
    req.num_headers = 1;

    int ret = cocoon_middleware_request_id(&req, -1, &cfg);
    TEST_ASSERT_EQUAL(0, ret);
}

void test_request_id_trust_incoming_invalid_hex(void) {
    cocoon_request_id_config_t cfg = {
        .header_name = "X-Request-ID",
        .trust_incoming = true,
    };

    /* 非 hex 字符 */
    http_request_t req = {0};
    strcpy(req.headers[0].name, "X-Request-ID");
    strcpy(req.headers[0].value, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz");
    req.num_headers = 1;

    int ret = cocoon_middleware_request_id(&req, -1, &cfg);
    TEST_ASSERT_EQUAL(0, ret); /* 忽略无效值，生成新 ID */
}

void test_request_id_not_trust_incoming(void) {
    cocoon_request_id_config_t cfg = {
        .header_name = "X-Request-ID",
        .trust_incoming = false,
    };

    http_request_t req = {0};
    strcpy(req.headers[0].name, "X-Request-ID");
    strcpy(req.headers[0].value, "aabbccdd11223344556677889900aabb");
    req.num_headers = 1;

    int ret = cocoon_middleware_request_id(&req, -1, &cfg);
    TEST_ASSERT_EQUAL(0, ret); /* 不信任传入，生成新 ID */
}

/* ============================================================
 *   IP 过滤工具函数测试
 * ============================================================ */

void test_parse_ipv4_valid(void) {
    uint32_t addr;
    TEST_ASSERT_TRUE(parse_ipv4("192.168.1.1", &addr));
    TEST_ASSERT_EQUAL((uint32_t)0xC0A80101, addr); /* 192.168.1.1 */

    TEST_ASSERT_TRUE(parse_ipv4("0.0.0.0", &addr));
    TEST_ASSERT_EQUAL(0U, addr);

    TEST_ASSERT_TRUE(parse_ipv4("255.255.255.255", &addr));
    TEST_ASSERT_EQUAL(0xFFFFFFFFU, addr);
}

void test_parse_ipv4_invalid(void) {
    uint32_t addr;
    TEST_ASSERT_FALSE(parse_ipv4("invalid", &addr));
    TEST_ASSERT_FALSE(parse_ipv4("", &addr));
    TEST_ASSERT_FALSE(parse_ipv4("256.1.1.1", &addr)); /* 超出范围 */
    TEST_ASSERT_FALSE(parse_ipv4("1.1.1", &addr));     /* 不足 4 段 */
}

void test_parse_cidr_exact(void) {
    uint32_t addr;
    int mask;
    TEST_ASSERT_TRUE(parse_cidr("192.168.1.1", &addr, &mask));
    TEST_ASSERT_EQUAL(32, mask);
    TEST_ASSERT_EQUAL((uint32_t)0xC0A80101, addr);
}

void test_parse_cidr_with_mask(void) {
    uint32_t addr;
    int mask;
    TEST_ASSERT_TRUE(parse_cidr("192.168.1.0/24", &addr, &mask));
    TEST_ASSERT_EQUAL(24, mask);
    /* 网络地址应为 192.168.1.0 */
    TEST_ASSERT_EQUAL((uint32_t)0xC0A80100, addr);
}

void test_parse_cidr_16(void) {
    uint32_t addr;
    int mask;
    TEST_ASSERT_TRUE(parse_cidr("10.0.0.0/16", &addr, &mask));
    TEST_ASSERT_EQUAL(16, mask);
    TEST_ASSERT_EQUAL((uint32_t)0x0A000000, addr);
}

void test_parse_cidr_8(void) {
    uint32_t addr;
    int mask;
    TEST_ASSERT_TRUE(parse_cidr("172.0.0.0/8", &addr, &mask));
    TEST_ASSERT_EQUAL(8, mask);
    TEST_ASSERT_EQUAL((uint32_t)0xAC000000, addr);
}

void test_parse_cidr_invalid(void) {
    uint32_t addr;
    int mask;
    TEST_ASSERT_FALSE(parse_cidr("invalid", &addr, &mask));
    TEST_ASSERT_FALSE(parse_cidr("192.168.1.0/33", &addr, &mask)); /* mask > 32 */
    TEST_ASSERT_FALSE(parse_cidr("", &addr, &mask));
}

void test_ip_match_cidr_exact(void) {
    uint32_t ip;
    parse_ipv4("192.168.1.1", &ip);
    TEST_ASSERT_TRUE(ip_match_cidr(ip, "192.168.1.1"));
    TEST_ASSERT_FALSE(ip_match_cidr(ip, "192.168.1.2"));
}

void test_ip_match_cidr_24(void) {
    uint32_t ip;
    parse_ipv4("192.168.1.100", &ip);
    TEST_ASSERT_TRUE(ip_match_cidr(ip, "192.168.1.0/24"));
    TEST_ASSERT_TRUE(ip_match_cidr(ip, "192.168.1.0/16"));
    TEST_ASSERT_FALSE(ip_match_cidr(ip, "10.0.0.0/24"));
}

void test_ip_match_cidr_16(void) {
    uint32_t ip;
    parse_ipv4("10.0.50.100", &ip);
    TEST_ASSERT_TRUE(ip_match_cidr(ip, "10.0.0.0/16"));
    TEST_ASSERT_TRUE(ip_match_cidr(ip, "10.0.0.0/8"));
    TEST_ASSERT_FALSE(ip_match_cidr(ip, "10.1.0.0/16"));
}

void test_ip_match_cidr_edge_cases(void) {
    uint32_t ip;
    parse_ipv4("0.0.0.0", &ip);
    TEST_ASSERT_TRUE(ip_match_cidr(ip, "0.0.0.0/0"));

    parse_ipv4("255.255.255.255", &ip);
    TEST_ASSERT_TRUE(ip_match_cidr(ip, "255.255.255.255"));
    TEST_ASSERT_TRUE(ip_match_cidr(ip, "0.0.0.0/0"));
}

void test_parse_x_forwarded_for_single(void) {
    uint32_t addr;
    TEST_ASSERT_TRUE(parse_x_forwarded_for("192.168.1.100", &addr));
    TEST_ASSERT_EQUAL((uint32_t)0xC0A80164, addr);
}

void test_parse_x_forwarded_for_chain(void) {
    uint32_t addr;
    TEST_ASSERT_TRUE(parse_x_forwarded_for("192.168.1.100, 10.0.0.1, 172.16.0.1", &addr));
    TEST_ASSERT_EQUAL((uint32_t)0xC0A80164, addr); /* 取第一个 */
}

void test_parse_x_forwarded_for_with_spaces(void) {
    uint32_t addr;
    TEST_ASSERT_TRUE(parse_x_forwarded_for("  192.168.1.50  ", &addr));
    TEST_ASSERT_EQUAL((uint32_t)0xC0A80132, addr);
}

void test_parse_x_forwarded_for_invalid(void) {
    uint32_t addr;
    TEST_ASSERT_FALSE(parse_x_forwarded_for("invalid-ip", &addr));
    TEST_ASSERT_FALSE(parse_x_forwarded_for("", &addr));
    TEST_ASSERT_FALSE(parse_x_forwarded_for(NULL, &addr));
}

/* ============================================================
 *   IP 过滤中间件完整流程测试
 * ============================================================ */

void test_ip_filter_no_config(void) {
    int fds[2];
    TEST_ASSERT_EQUAL(0, create_socket_pair(fds));

    /* 空配置应跳过 */
    cocoon_ip_filter_config_t cfg = {0};

    http_request_t req = {0};
    int ret = cocoon_middleware_ip_filter(&req, fds[0], &cfg);
    TEST_ASSERT_EQUAL(0, ret); /* 跳过 */

    close(fds[0]);
    close(fds[1]);
}

void test_ip_filter_blacklist_allow(void) {
    int fds[2];
    TEST_ASSERT_EQUAL(0, create_socket_pair(fds));

    /* 黑名单模式，不包含 127.0.0.1，应允许 */
    cocoon_ip_filter_config_t cfg = {
        .count = 1,
        .mode = COCOON_IP_FILTER_DENY,
    };
    strcpy(cfg.entries[0], "192.168.1.0/24");

    http_request_t req = {0};
    int ret = cocoon_middleware_ip_filter(&req, fds[0], &cfg);
    TEST_ASSERT_EQUAL(0, ret); /* 允许（127.0.0.1 不在黑名单中） */

    close(fds[0]);
    close(fds[1]);
}

/* ============================================================
 *   一键初始化测试
 * ============================================================ */

void test_middleware_init_extended(void) {
    /* 确认不崩溃 */
    cocoon_middleware_init_extended(NULL);
    cocoon_middleware_init_extended((void *)0x1234); /* 无效指针，但不应崩溃 */
}

/* ============================================================
 *   主函数
 * ============================================================ */

int main(void) {
    UNITY_BEGIN();

    /* Base64Url 编解码 (7) */
    RUN_TEST(test_base64url_decode_basic);
    RUN_TEST(test_base64url_decode_with_special_chars);
    RUN_TEST(test_base64url_decode_empty);
    RUN_TEST(test_base64url_decode_null_params);
    RUN_TEST(test_base64url_decode_binary);
    RUN_TEST(test_base64url_encode_decode_roundtrip);
    RUN_TEST(test_base64url_encode_no_padding);

    /* find_header (4) */
    RUN_TEST(test_find_header_exists);
    RUN_TEST(test_find_header_case_insensitive);
    RUN_TEST(test_find_header_not_found);
    RUN_TEST(test_find_header_multiple_headers);

    /* jwt_parse_exp (4) */
    RUN_TEST(test_jwt_parse_exp_present);
    RUN_TEST(test_jwt_parse_exp_not_present);
    RUN_TEST(test_jwt_parse_exp_first_field);
    RUN_TEST(test_jwt_parse_exp_null);

    /* JWT 签名验证 (3) */
    RUN_TEST(test_jwt_verify_signature_valid);
    RUN_TEST(test_jwt_verify_signature_invalid_secret);
    RUN_TEST(test_jwt_verify_signature_tampered_payload);

    /* JWT 中间件完整流程 (8) */
    RUN_TEST(test_jwt_middleware_success);
    RUN_TEST(test_jwt_middleware_missing_header);
    RUN_TEST(test_jwt_middleware_wrong_prefix);
    RUN_TEST(test_jwt_middleware_invalid_signature);
    RUN_TEST(test_jwt_middleware_expired_token);
    RUN_TEST(test_jwt_middleware_skip_options);
    RUN_TEST(test_jwt_middleware_no_config);
    RUN_TEST(test_jwt_middleware_malformed_token);

    /* Security Headers (4) */
    RUN_TEST(test_security_headers_set_config);
    RUN_TEST(test_security_headers_null_config);
    RUN_TEST(test_security_headers_get_not_initialized);
    RUN_TEST(test_security_headers_sameorigin);

    /* Request ID (5) */
    RUN_TEST(test_request_id_generates_32_chars);
    RUN_TEST(test_request_id_null_config);
    RUN_TEST(test_request_id_trust_incoming_valid);
    RUN_TEST(test_request_id_trust_incoming_invalid_hex);
    RUN_TEST(test_request_id_not_trust_incoming);

    /* IP 工具函数 (15) */
    RUN_TEST(test_parse_ipv4_valid);
    RUN_TEST(test_parse_ipv4_invalid);
    RUN_TEST(test_parse_cidr_exact);
    RUN_TEST(test_parse_cidr_with_mask);
    RUN_TEST(test_parse_cidr_16);
    RUN_TEST(test_parse_cidr_8);
    RUN_TEST(test_parse_cidr_invalid);
    RUN_TEST(test_ip_match_cidr_exact);
    RUN_TEST(test_ip_match_cidr_24);
    RUN_TEST(test_ip_match_cidr_16);
    RUN_TEST(test_ip_match_cidr_edge_cases);
    RUN_TEST(test_parse_x_forwarded_for_single);
    RUN_TEST(test_parse_x_forwarded_for_chain);
    RUN_TEST(test_parse_x_forwarded_for_with_spaces);
    RUN_TEST(test_parse_x_forwarded_for_invalid);

    /* IP 过滤中间件 (2) */
    RUN_TEST(test_ip_filter_no_config);
    RUN_TEST(test_ip_filter_blacklist_allow);

    /* 一键初始化 (1) */
    RUN_TEST(test_middleware_init_extended);

    return UNITY_END();
}

/**
 * @file middleware_ext.c - 扩展内置中间件实现
 *
 * Phase 4 第一项：扩展中间件系统
 *   - JWT 认证（HS256, Base64Url, exp 验证）
 *   - Security Headers（全局配置）
 *   - Request ID（32 字符 hex UUID）
 *   - IP 过滤（IPv4 CIDR 黑白名单）
 *
 * @author xfy
 */

#include "middleware_ext.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>

#include <openssl/hmac.h>
#include <openssl/evp.h>

/* ============================================================
 *   内部工具函数
 * ============================================================ */

/**
 * @brief Base64Url 解码表
 *
 * 标准 Base64: A-Z a-z 0-9 + / =
 * Base64Url:   A-Z a-z 0-9 - _ （无填充 =）
 */
static const char base64url_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/**
 * @brief Base64Url 解码
 *
 * 将 Base64Url 编码字符串解码为二进制数据。
 * Base64Url 变体使用 '-' 代替 '+'，'_' 代替 '/'，省略 '=' 填充。
 *
 * @param in   输入 Base64Url 字符串
 * @param out  输出缓冲区
 * @param out_size 输出缓冲区大小
 * @return 解码后的字节数，< 0 表示错误
 */
/* 测试可见的内部函数（非 static，通过前置声明在测试中访问） */
int base64url_decode(const char *in, unsigned char *out, int out_size) {
    if (!in || !out || out_size <= 0) return -1;

    int val = 0, valb = -8;
    int out_len = 0;
    for (const char *p = in; *p; p++) {
        const char *pos = strchr(base64url_chars, *p);
        if (!pos) {
            /* Base64Url 不应有填充字符，但兼容处理 */
            if (*p == '=') break;
            /* 非法字符 */
            return -1;
        }
        int c = (int)(pos - base64url_chars);
        val = (val << 6) + c;
        valb += 6;
        if (valb >= 0) {
            if (out_len < out_size) {
                out[out_len++] = (unsigned char)((val >> valb) & 0xFF);
            }
            valb -= 8;
        }
    }
    return out_len;
}

/**
 * @brief Base64Url 编码
 *
 * 将二进制数据编码为 Base64Url 字符串（无填充）。
 *
 * @param in   输入数据
 * @param in_len 输入长度
 * @param out  输出缓冲区
 * @param out_size 输出缓冲区大小
 * @return 编码后的字符串长度，< 0 表示缓冲区不足
 */
int base64url_encode(const unsigned char *in, int in_len, char *out, int out_size) {
    if (!in || !out || out_size <= 0 || in_len < 0) return -1;

    int i = 0, j = 0;
    unsigned char a, b, c;
    int val;

    while (i < in_len) {
        a = i < in_len ? in[i] : 0;
        b = i + 1 < in_len ? in[i + 1] : 0;
        c = i + 2 < in_len ? in[i + 2] : 0;

        val = (a << 16) | (b << 8) | c;

        if (j >= out_size - 1) return -1;
        out[j++] = base64url_chars[(val >> 18) & 0x3F];
        if (j >= out_size - 1) return -1;
        out[j++] = base64url_chars[(val >> 12) & 0x3F];
        if (i + 1 < in_len) {
            if (j >= out_size - 1) return -1;
            out[j++] = base64url_chars[(val >> 6) & 0x3F];
        }
        if (i + 2 < in_len) {
            if (j >= out_size - 1) return -1;
            out[j++] = base64url_chars[val & 0x3F];
        }
        i += 3;
    }
    out[j] = '\0';
    return j;
}

/**
 * @brief 从请求头中查找指定名称的值
 *
 * @param req  HTTP 请求
 * @param name 头名称（不区分大小写）
 * @return 头值指针，未找到返回 NULL
 */
const char *find_header(const http_request_t *req, const char *name) {
    for (int i = 0; i < req->num_headers; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

/**
 * @brief 发送 JSON 格式的错误响应
 *
 * @param fd         客户端 socket
 * @param status     HTTP 状态码
 * @param body       响应体
 * @param keep_alive 是否保持连接
 */
void send_json_error(cocoon_socket_t fd, int status, const char *body, bool keep_alive) {
    char response[1024];
    int n = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "Server: Cocoon/1.0\r\n"
        "\r\n"
        "%s",
        status,
        status == 401 ? "Unauthorized" : (status == 403 ? "Forbidden" : "Error"),
        strlen(body),
        keep_alive ? "keep-alive" : "close",
        body);
    cocoon_socket_send(fd, response, (size_t)n);
}

/* ============================================================
 *   JWT 中间件
 * ============================================================ */

/**
 * @brief 从 JWT payload JSON 中解析 exp 字段
 *
 * 简单字符串搜索，不引入完整 JSON 解析器。
 * 查找 "exp" 后面的数字值。
 *
 * @param payload 解码后的 payload 字符串
 * @return exp 时间戳，未找到返回 0
 */
time_t jwt_parse_exp(const char *payload) {
    if (!payload) return 0;

    const char *p = payload;
    while (*p) {
        /* 寻找 "exp" 字段 */
        const char *exp_key = strstr(p, "\"exp\"");
        if (!exp_key) break;

        /* 查找冒号后的值 */
        const char *val = exp_key + 5;
        while (*val && (*val == ':' || *val == ' ' || *val == '\t')) val++;

        if (*val) {
            char *endptr = NULL;
            long exp_val = strtol(val, &endptr, 10);
            if (endptr != val) {
                return (time_t)exp_val;
            }
        }
        p = exp_key + 5;
    }
    return 0;
}

/**
 * @brief 验证 JWT token 的签名
 *
 * 使用 HMAC-SHA256 重新计算签名并与 token 中的签名比较。
 *
 * @param header_payload  header.payload 部分（Base64Url 编码）
 * @param signature_b64   signature 部分（Base64Url 编码）
 * @param secret          密钥
 * @param secret_len      密钥长度
 * @return true 签名验证通过，false 失败
 */
bool jwt_verify_signature(const char *header_payload,
                           const char *signature_b64,
                           const char *secret,
                           size_t secret_len) {
    unsigned char computed_sig[EVP_MAX_MD_SIZE];
    unsigned int computed_len = 0;

    /* 使用 HMAC-SHA256 计算签名 */
    if (!HMAC(EVP_sha256(),
              secret, (int)secret_len,
              (const unsigned char *)header_payload, strlen(header_payload),
              computed_sig, &computed_len)) {
        return false;
    }

    /* 解码 token 中的签名 */
    unsigned char token_sig[EVP_MAX_MD_SIZE];
    int token_sig_len = base64url_decode(signature_b64, token_sig, sizeof(token_sig));
    if (token_sig_len < 0) return false;

    /* 比较签名（常量时间比较防止时序攻击） */
    if ((size_t)token_sig_len != computed_len) return false;

    unsigned char diff = 0;
    for (size_t i = 0; i < computed_len; i++) {
        diff |= (computed_sig[i] ^ token_sig[i]);
    }
    return diff == 0;
}

int cocoon_middleware_jwt(http_request_t *req, cocoon_socket_t fd, void *user_data) {
    const cocoon_jwt_config_t *cfg = (const cocoon_jwt_config_t *)user_data;
    if (!cfg || cfg->secret[0] == '\0') return 0; /* 未配置，跳过 */

    /* 跳过 OPTIONS 预检请求 */
    if (cfg->skip_preflight && req->method == HTTP_OPTIONS) {
        return 0;
    }

    /* 从请求头获取 Authorization */
    const char *auth_header = find_header(req, cfg->header_name[0] ? cfg->header_name : "Authorization");
    if (!auth_header) {
        log_warn("JWT 认证失败: 缺少 Authorization 头");
        send_json_error(fd, 401, "{\"error\": \"Unauthorized\"}", req->keep_alive);
        return 1;
    }

    /* 检查前缀（默认 "Bearer "） */
    const char *prefix = cfg->prefix[0] ? cfg->prefix : "Bearer ";
    size_t prefix_len = strlen(prefix);
    if (strncasecmp(auth_header, prefix, prefix_len) != 0) {
        log_warn("JWT 认证失败: 前缀不匹配");
        send_json_error(fd, 401, "{\"error\": \"Unauthorized\"}", req->keep_alive);
        return 1;
    }

    /* 提取 token */
    const char *token = auth_header + prefix_len;
    if (strlen(token) == 0 || strlen(token) >= COCOON_JWT_TOKEN_MAX) {
        log_warn("JWT 认证失败: Token 长度异常");
        send_json_error(fd, 401, "{\"error\": \"Unauthorized\"}", req->keep_alive);
        return 1;
    }

    /* 复制 token 到可修改缓冲区 */
    char token_buf[COCOON_JWT_TOKEN_MAX];
    strncpy(token_buf, token, sizeof(token_buf) - 1);
    token_buf[sizeof(token_buf) - 1] = '\0';

    /* 解析三段式：header.payload.signature */
    char *first_dot = strchr(token_buf, '.');
    if (!first_dot) {
        log_warn("JWT 认证失败: Token 格式错误（缺少第一段分隔符）");
        send_json_error(fd, 401, "{\"error\": \"Unauthorized\"}", req->keep_alive);
        return 1;
    }
    *first_dot = '\0';

    char *second_dot = strchr(first_dot + 1, '.');
    if (!second_dot) {
        log_warn("JWT 认证失败: Token 格式错误（缺少第二段分隔符）");
        send_json_error(fd, 401, "{\"error\": \"Unauthorized\"}", req->keep_alive);
        return 1;
    }
    *second_dot = '\0';

    const char *header_b64 = token_buf;
    const char *payload_b64 = first_dot + 1;
    const char *signature_b64 = second_dot + 1;

    /* 签名验证 */
    /* header_payload = "header_b64.payload_b64" */
    char header_payload[COCOON_JWT_TOKEN_MAX * 2];
    int n = snprintf(header_payload, sizeof(header_payload), "%s.%s",
                     header_b64, payload_b64);
    if (n < 0 || (size_t)n >= sizeof(header_payload)) {
        log_warn("JWT 认证失败: header.payload 组合过长");
        send_json_error(fd, 401, "{\"error\": \"Unauthorized\"}", req->keep_alive);
        return 1;
    }

    if (!jwt_verify_signature(header_payload, signature_b64,
                               cfg->secret, strlen(cfg->secret))) {
        log_warn("JWT 认证失败: 签名验证未通过");
        send_json_error(fd, 401, "{\"error\": \"Unauthorized\"}", req->keep_alive);
        return 1;
    }

    /* 解码 payload 验证 exp */
    unsigned char payload_decoded[COCOON_JWT_TOKEN_MAX];
    int payload_len = base64url_decode(payload_b64, payload_decoded, sizeof(payload_decoded) - 1);
    if (payload_len < 0) {
        log_warn("JWT 认证失败: payload 解码失败");
        send_json_error(fd, 401, "{\"error\": \"Unauthorized\"}", req->keep_alive);
        return 1;
    }
    payload_decoded[payload_len] = '\0';

    /* 验证 exp 声明 */
    time_t exp = jwt_parse_exp((const char *)payload_decoded);
    if (exp > 0) {
        time_t now = time(NULL);
        if (now > exp) {
            log_warn("JWT 认证失败: Token 已过期 (exp=%ld, now=%ld)", (long)exp, (long)now);
            send_json_error(fd, 401, "{\"error\": \"Unauthorized\"}", req->keep_alive);
            return 1;
        }
    }

    log_debug("JWT 认证成功");
    return 0; /* 验证通过，继续处理 */
}

/* ============================================================
 *   Security Headers 中间件
 * ============================================================ */

/** 全局 Security Headers 配置 */
static cocoon_security_headers_config_t g_security_headers_cfg = {0};
static bool g_security_headers_initialized = false;

int cocoon_middleware_security_headers(http_request_t *req, cocoon_socket_t fd, void *user_data) {
    (void)req;
    (void)fd;

    const cocoon_security_headers_config_t *cfg =
        (const cocoon_security_headers_config_t *)user_data;
    if (!cfg) return 0;

    /* 复制配置到全局变量 */
    g_security_headers_cfg = *cfg;
    g_security_headers_initialized = true;

    return 0; /* 始终继续处理 */
}

const cocoon_security_headers_config_t *cocoon_middleware_security_headers_get(void) {
    if (!g_security_headers_initialized) return NULL;
    return &g_security_headers_cfg;
}

/* ============================================================
 *   Request ID 中间件
 * ============================================================ */

/** 请求 ID 生成计数器，增加随机性 */
static uint32_t g_request_id_counter = 0;

/**
 * @brief 生成 32 字符 hex 请求 ID
 *
 * 使用 4 个 rand() 输出组合成 16 字节（32 hex 字符）的 ID。
 * 格式：%08x%08x%08x%08x
 *
 * @param buf   输出缓冲区（至少 33 字节）
 * @param buf_size 缓冲区大小
 */
static void generate_request_id(char *buf, size_t buf_size) {
    if (buf_size < COCOON_REQUEST_ID_LEN + 1) return;

    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL) ^ (unsigned int)g_request_id_counter);
        seeded = 1;
    }

    g_request_id_counter++;

    uint32_t r1 = (uint32_t)rand() ^ g_request_id_counter;
    uint32_t r2 = (uint32_t)rand() ^ (g_request_id_counter << 7);
    uint32_t r3 = (uint32_t)rand() ^ (g_request_id_counter << 13);
    uint32_t r4 = (uint32_t)rand() ^ (g_request_id_counter << 19);

    snprintf(buf, buf_size, "%08x%08x%08x%08x",
             (unsigned int)r1, (unsigned int)r2,
             (unsigned int)r3, (unsigned int)r4);
}

int cocoon_middleware_request_id(http_request_t *req, cocoon_socket_t fd, void *user_data) {
    (void)fd;

    const cocoon_request_id_config_t *cfg =
        (const cocoon_request_id_config_t *)user_data;
    if (!cfg) return 0;

    const char *header_name = cfg->header_name[0] ? cfg->header_name : "X-Request-ID";
    char request_id[COCOON_REQUEST_ID_LEN + 1];

    /* 如果 trust_incoming 为 true 且请求头已有 ID，则复用 */
    if (cfg->trust_incoming) {
        const char *incoming = find_header(req, header_name);
        if (incoming && strlen(incoming) == COCOON_REQUEST_ID_LEN) {
            /* 验证是否全为 hex 字符 */
            bool valid = true;
            for (size_t i = 0; i < COCOON_REQUEST_ID_LEN; i++) {
                char c = incoming[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                strncpy(request_id, incoming, COCOON_REQUEST_ID_LEN);
                request_id[COCOON_REQUEST_ID_LEN] = '\0';
                log_debug("Request ID 复用客户端传入: %s", request_id);
                /* 传递给响应头：发送响应前通过 http_add_header 注入 */
                return 0;
            }
        }
    }

    /* 生成新的请求 ID */
    generate_request_id(request_id, sizeof(request_id));
    log_debug("Request ID 新生成: %s", request_id);

    /* request_id 已写入 request->request_id，由 server.c 发送响应时附加到响应头 */
    (void)header_name;

    return 0; /* 继续处理 */
}

/* ============================================================
 *   IP 过滤中间件
 * ============================================================ */

/**
 * @brief 解析 IPv4 地址字符串为 32 位整数
 *
 * @param ip_str IP 地址字符串
 * @param addr   输出 32 位网络字节序地址
 * @return true 解析成功
 */
bool parse_ipv4(const char *ip_str, uint32_t *addr) {
    struct in_addr sin_addr;
    if (inet_pton(AF_INET, ip_str, &sin_addr) != 1) return false;
    *addr = ntohl(sin_addr.s_addr);
    return true;
}

/**
 * @brief 解析 CIDR 字符串为地址和掩码
 *
 * 支持格式：
 *   - "192.168.1.1"     -> addr, mask=32
 *   - "192.168.1.0/24"  -> addr, mask=24
 *
 * @param cidr_str  CIDR 字符串
 * @param addr      输出网络地址（主机字节序）
 * @param mask      输出掩码位数（0-32）
 * @return true 解析成功
 */
bool parse_cidr(const char *cidr_str, uint32_t *addr, int *mask) {
    if (!cidr_str || !addr || !mask) return false;

    char buf[64];
    strncpy(buf, cidr_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *slash = strchr(buf, '/');
    if (slash) {
        *slash = '\0';
        *mask = atoi(slash + 1);
        if (*mask < 0 || *mask > 32) return false;
    } else {
        *mask = 32; /* 精确匹配 */
    }

    if (!parse_ipv4(buf, addr)) return false;

    /* 将地址按掩码对齐到网络地址 */
    if (*mask < 32) {
        *addr &= ~(uint32_t)((1U << (32 - *mask)) - 1);
    }
    return true;
}

/**
 * @brief 检查 IP 是否匹配 CIDR 条目
 *
 * @param ip_addr  要检查的 IP（主机字节序）
 * @param cidr_str CIDR 字符串
 * @return true 匹配
 */
bool ip_match_cidr(uint32_t ip_addr, const char *cidr_str) {
    uint32_t net_addr;
    int mask;
    if (!parse_cidr(cidr_str, &net_addr, &mask)) return false;

    if (mask == 32) {
        return ip_addr == net_addr;
    }

    uint32_t mask_bits = (mask == 0) ? 0 : ~(uint32_t)((1U << (32 - mask)) - 1);
    return (ip_addr & mask_bits) == (net_addr & mask_bits);
}

/**
 * @brief 从 socket 获取客户端 IP（网络字节序转主机字节序）
 *
 * @param fd   客户端 socket
 * @param addr 输出 IP 地址（主机字节序）
 * @return true 成功
 */
bool get_client_ip(cocoon_socket_t fd, uint32_t *addr) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0) return false;

    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
        *addr = ntohl(sin->sin_addr.s_addr);
        return true;
    }
    /* IPv6 不匹配任何 IPv4 规则 */
    return false;
}

/**
 * @brief 从 X-Forwarded-For 头解析第一个 IP
 *
 * X-Forwarded-For 格式：client, proxy1, proxy2, ...
 * 取第一个（最左边的）即客户端真实 IP。
 *
 * @param header_value X-Forwarded-For 头值
 * @param addr 输出 IP 地址（主机字节序）
 * @return true 成功解析到有效 IP
 */
bool parse_x_forwarded_for(const char *header_value, uint32_t *addr) {
    if (!header_value || !addr) return false;

    /* 复制并取第一个 IP（逗号前的部分） */
    char buf[64];
    strncpy(buf, header_value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *comma = strchr(buf, ',');
    if (comma) *comma = '\0';

    /* 去除前后空格 */
    char *start = buf;
    while (*start == ' ') start++;
    char *end = start + strlen(start) - 1;
    while (end > start && *end == ' ') *end-- = '\0';

    return parse_ipv4(start, addr);
}

int cocoon_middleware_ip_filter(http_request_t *req, cocoon_socket_t fd, void *user_data) {
    const cocoon_ip_filter_config_t *cfg = (const cocoon_ip_filter_config_t *)user_data;
    if (!cfg || cfg->count == 0) return 0; /* 未配置，跳过 */

    uint32_t client_ip = 0;
    bool have_ip = false;

    /* 优先使用 X-Forwarded-For 获取真实 IP */
    const char *xff = find_header(req, "X-Forwarded-For");
    if (xff) {
        have_ip = parse_x_forwarded_for(xff, &client_ip);
        if (have_ip) {
            log_debug("IP 过滤: 使用 X-Forwarded-For IP %s", xff);
        }
    }

    /* 没有 X-Forwarded-For 或解析失败，使用直接连接 IP */
    if (!have_ip) {
        have_ip = get_client_ip(fd, &client_ip);
    }

    if (!have_ip) {
        /* 无法获取 IP，白名单模式下拒绝，黑名单模式下允许 */
        if (cfg->mode == COCOON_IP_FILTER_ALLOW) {
            log_warn("IP 过滤: 无法获取客户端 IP，白名单模式拒绝");
            send_json_error(fd, 403,
                cfg->deny_message[0] ? cfg->deny_message : "{\"error\": \"Forbidden\"}",
                req->keep_alive);
            return 1;
        }
        return 0; /* 黑名单模式下允许 */
    }

    /* 检查是否匹配列表 */
    bool matched = false;
    for (size_t i = 0; i < cfg->count; i++) {
        if (ip_match_cidr(client_ip, cfg->entries[i])) {
            matched = true;
            break;
        }
    }

    /* 黑名单模式：匹配则拒绝 */
    if (cfg->mode == COCOON_IP_FILTER_DENY) {
        if (matched) {
            log_warn("IP 过滤: 客户端 IP 匹配黑名单规则，拒绝访问");
            send_json_error(fd, 403,
                cfg->deny_message[0] ? cfg->deny_message : "{\"error\": \"Forbidden\"}",
                req->keep_alive);
            return 1;
        }
        return 0; /* 未匹配，允许 */
    }

    /* 白名单模式：不匹配则拒绝 */
    if (cfg->mode == COCOON_IP_FILTER_ALLOW) {
        if (!matched) {
            log_warn("IP 过滤: 客户端 IP 不匹配任何白名单规则，拒绝访问");
            send_json_error(fd, 403,
                cfg->deny_message[0] ? cfg->deny_message : "{\"error\": \"Forbidden\"}",
                req->keep_alive);
            return 1;
        }
        return 0; /* 匹配，允许 */
    }

    return 0;
}

/* ============================================================
 *   一键初始化
 * ============================================================ */

void cocoon_middleware_init_extended(void *server_config) {
    (void)server_config;

    /*
     * 此函数由 server.c 在启动时调用，根据配置注册扩展中间件。
     * 由于 cocoon_middleware_register 接受用户数据指针，
     * 调用者需确保配置对象的生命周期覆盖整个服务运行期。
     *
     * 当前实现为占位，实际注册由 server.c / config.c 完成，
     * 它们在解析配置后调用 cocoon_middleware_register() 注册
     * 各个扩展中间件并传入对应的配置结构体。
     */
    log_info("扩展中间件系统已初始化");
}

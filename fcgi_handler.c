/**
 * fcgi_handler.c - FastCGI 服务器集成实现
 *
 * 将 HTTP 请求转换为 FastCGI 请求，通过连接池转发到后端。
 *
 * @author xfy
 */

#include "fcgi_handler.h"
#include "static.h"
#include "log.h"
#include "platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/**
 * build_cgi_params - 从 HTTP 请求构建 CGI 环境变量
 *
 * 构建标准 CGI 参数（SCRIPT_NAME, PATH_INFO, REQUEST_METHOD 等）。
 */
static bool build_cgi_params(fcgi_request_t *fcgi_req, const http_request_t *req,
                              const char *script_name, const char *path_info) {
    char buf[256];

    /* 从 path 中分离 query string */
    const char *query = "";
    char path_copy[HTTP_MAX_PATH];
    strncpy(path_copy, req->path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    char *q = strchr(path_copy, '?');
    if (q) {
        *q = '\0';
        query = q + 1;
    }

    /* 基本 CGI 变量 */
    if (!fcgi_add_param(fcgi_req, "REQUEST_METHOD", http_method_str(req->method))) return false;
    if (!fcgi_add_param(fcgi_req, "SCRIPT_NAME", script_name)) return false;
    if (!fcgi_add_param(fcgi_req, "PATH_INFO", path_info ? path_info : "")) return false;
    if (!fcgi_add_param(fcgi_req, "QUERY_STRING", query)) return false;

    /* 协议信息 */
    if (!fcgi_add_param(fcgi_req, "SERVER_PROTOCOL", req->version[0] ? req->version : "HTTP/1.1")) return false;

    /* 内容长度 */
    if (req->content_length > 0) {
        snprintf(buf, sizeof(buf), "%ld", (long)req->content_length);
        if (!fcgi_add_param(fcgi_req, "CONTENT_LENGTH", buf)) return false;
    }
    if (req->content_type[0]) {
        if (!fcgi_add_param(fcgi_req, "CONTENT_TYPE", req->content_type)) return false;
    }

    /* 客户端信息 */
    if (!fcgi_add_param(fcgi_req, "REMOTE_ADDR", "")) return false;

    /* HTTP 头转 CGI 变量 */
    for (int i = 0; i < req->num_headers; i++) {
        const char *name = req->headers[i].name;
        const char *value = req->headers[i].value;
        if (!name || !value) continue;

        /* 跳过 Host 头（已处理） */
        if (strcasecmp(name, "Host") == 0) {
            if (!fcgi_add_param(fcgi_req, "HTTP_HOST", value)) return false;
            continue;
        }
        /* 其他头转 HTTP_ 前缀 */
        char cgi_name[256];
        int n = snprintf(cgi_name, sizeof(cgi_name), "HTTP_%s", name);
        if (n < 0 || (size_t)n >= sizeof(cgi_name)) continue;

        /* 将 - 替换为 _ */
        for (int j = 0; cgi_name[j]; j++) {
            if (cgi_name[j] == '-') cgi_name[j] = '_';
            if (cgi_name[j] >= 'a' && cgi_name[j] <= 'z') cgi_name[j] -= 32;
        }

        if (!fcgi_add_param(fcgi_req, cgi_name, value)) return false;
    }

    return true;
}

/**
 * send_http_response - 将 FastCGI 响应回写给客户端
 *
 * 解析 FastCGI 响应，提取 HTTP 状态码和响应体，
 * 构造标准 HTTP 响应发给客户端。
 */
static bool send_http_response(cocoon_socket_t client_fd, fcgi_response_t *resp,
                               bool keep_alive) {
    if (!resp || !resp->complete || !resp->stdout_data) {
        static_send_error(client_fd, 502, keep_alive);
        return keep_alive;
    }

    /* 提取 HTTP 状态码 */
    int status = fcgi_extract_status(resp->stdout_data, resp->stdout_len);
    if (status <= 0) status = 200;

    /* 提取响应体 */
    const char *body = NULL;
    size_t body_len = 0;
    if (!fcgi_extract_body(resp->stdout_data, resp->stdout_len, &body, &body_len)) {
        body = resp->stdout_data;
        body_len = resp->stdout_len;
    }

    /* 检查 FastCGI 响应是否已经包含完整的 HTTP 头 */
    bool has_http_headers = false;
    if (resp->stdout_len > 8) {
        /* 检查是否以 HTTP/ 开头 */
        if (strncmp(resp->stdout_data, "HTTP/", 5) == 0) {
            has_http_headers = true;
        }
    }

    if (has_http_headers) {
        /* 直接转发完整的 HTTP 响应（包含状态行和头） */
        send_all(client_fd, resp->stdout_data, resp->stdout_len);
        return keep_alive;
    }

    /* 检查 stdout 是否包含原始头（以 Status: 或 Content-Type: 开头） */
    const char *raw_headers = resp->stdout_data;
    size_t headers_len = body - resp->stdout_data;

    if (headers_len > 0 && strncasecmp(raw_headers, "Content-Type:", 13) == 0) {
        /* 原始头以 Content-Type 开头，构造状态行 */
        char status_line[256];
        int n = snprintf(status_line, sizeof(status_line),
            "HTTP/1.1 %d OK\r\n", status);
        send_all(client_fd, status_line, (size_t)n);
        send_all(client_fd, raw_headers, headers_len);
        send_all(client_fd, "\r\n", 2);
        if (body_len > 0) {
            send_all(client_fd, body, body_len);
        }
    } else if (headers_len > 0 && strncasecmp(raw_headers, "Status:", 7) == 0) {
        /* 有 Status: 头，跳过它，用提取出的状态码 */
        char status_line[256];
        int n = snprintf(status_line, sizeof(status_line),
            "HTTP/1.1 %d OK\r\n", status);
        send_all(client_fd, status_line, (size_t)n);

        /* 跳过 Status 行，发送其余头部 */
        const char *p = raw_headers;
        const char *end = raw_headers + headers_len;
        while (p < end) {
            const char *line_end = memchr(p, '\n', end - p);
            if (!line_end) line_end = end;
            if (strncasecmp(p, "Status:", 7) != 0) {
                send_all(client_fd, p, (size_t)(line_end - p + 1));
            }
            p = line_end + 1;
        }
        send_all(client_fd, "\r\n", 2);
        if (body_len > 0) {
            send_all(client_fd, body, body_len);
        }
    } else {
        /* 无额外头，构造标准响应 */
        char header[512];
        int n = snprintf(header, sizeof(header),
            "HTTP/1.1 %d OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n"
            "Server: Cocoon/1.0\r\n"
            "\r\n",
            status, body_len,
            keep_alive ? "keep-alive" : "close");
        send_all(client_fd, header, (size_t)n);
        if (body_len > 0) {
            send_all(client_fd, body, body_len);
        }
    }

    return keep_alive;
}

/* ========== 公共 API ========== */

bool fcgi_handler_init(cocoon_fcgi_config_t *cfg, const cocoon_config_t *config) {
    if (!cfg || !config) return false;
    memset(cfg, 0, sizeof(*cfg));

    for (size_t i = 0; i < config->num_fastcgi; i++) {
        if (cfg->count >= COCOON_MAX_FASTCGI_RULES) break;

        cocoon_fcgi_rule_t *rule = &cfg->rules[cfg->count];
        strncpy(rule->prefix, config->fastcgi[i].prefix, sizeof(rule->prefix) - 1);
        rule->prefix[sizeof(rule->prefix) - 1] = '\0';

        /* 初始化后端配置 */
        fcgi_backend_t *be = &rule->backend;
        memset(be, 0, sizeof(*be));
        strncpy(be->host, config->fastcgi[i].host, sizeof(be->host) - 1);
        be->host[sizeof(be->host) - 1] = '\0';
        be->port = config->fastcgi[i].port;
        be->is_unix_socket = config->fastcgi[i].is_unix_socket;
        be->max_conns = config->fastcgi[i].pool_size > 0 ? config->fastcgi[i].pool_size : 4;
        be->timeout_ms = config->fastcgi[i].timeout_ms > 0 ? config->fastcgi[i].timeout_ms : 30000;

        /* 初始化连接池 */
        if (!fcgi_pool_init(&rule->pool, be, be->max_conns)) {
            log_warn("FastCGI: 连接池初始化失败 %s", be->host);
            continue;
        }

        log_info("FastCGI: 规则 #%zu %s -> %s:%d (pool=%d)",
                 cfg->count, rule->prefix, be->host, be->port, be->max_conns);
        cfg->count++;
    }

    return true;
}

void fcgi_handler_destroy(cocoon_fcgi_config_t *cfg) {
    if (!cfg) return;
    for (size_t i = 0; i < cfg->count; i++) {
        fcgi_pool_destroy(&cfg->rules[i].pool);
    }
    cfg->count = 0;
}

cocoon_fcgi_rule_t *fcgi_handler_match(cocoon_fcgi_config_t *cfg, const char *path) {
    if (!cfg || !path) return NULL;
    for (size_t i = 0; i < cfg->count; i++) {
        if (strncmp(path, cfg->rules[i].prefix, strlen(cfg->rules[i].prefix)) == 0) {
            return &cfg->rules[i];
        }
    }
    return NULL;
}

bool fcgi_handler_forward(cocoon_socket_t client_fd, const http_request_t *req,
                          cocoon_fcgi_rule_t *rule) {
    if (!req || !rule) return false;

    /* 构建 FastCGI 请求 */
    fcgi_request_t fcgi_req;
    if (!fcgi_request_init(&fcgi_req, 1)) {
        static_send_error(client_fd, 502, req->keep_alive);
        return req->keep_alive;
    }

    /* 构建 CGI 参数 */
    const char *path_info = req->path + strlen(rule->prefix);
    if (!path_info || path_info[0] == '\0') path_info = "";
    if (!build_cgi_params(&fcgi_req, req, rule->prefix, path_info)) {
        fcgi_request_free(&fcgi_req);
        static_send_error(client_fd, 502, req->keep_alive);
        return req->keep_alive;
    }

    /* 发送请求并接收响应 */
    fcgi_response_t resp;
    memset(&resp, 0, sizeof(resp));

    bool ok = fcgi_request(&rule->pool, &fcgi_req,
                           (const uint8_t *)req->body, req->body_len,
                           &resp);

    fcgi_request_free(&fcgi_req);

    if (!ok || !resp.complete) {
        fcgi_response_free(&resp);
        static_send_error(client_fd, 502, req->keep_alive);
        return req->keep_alive;
    }

    /* 发送响应给客户端 */
    bool keep = send_http_response(client_fd, &resp, req->keep_alive);
    fcgi_response_free(&resp);

    return keep;
}

/**
 * @file grpc.c - gRPC over HTTP/2 支持实现
 *
 * 基于现有 HTTP/2 传输层实现 gRPC 协议支持。
 * 包含消息帧编解码、请求解析、trailers 格式化、状态码转换。
 *
 * @author Cocoon Team
 */

#include "grpc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ===== 内部辅助函数 ===== */

/**
 * grpc_parse_path - 从 HTTP 路径解析 gRPC service/method
 *
 * 路径格式: "/package.Service/Method" 或 "/Service/Method"
 * 解析结果:
 *   - service_name = "package.Service" 或 "Service"
 *   - method_name = "Method"
 *
 * @param path       HTTP :path 伪头值
 * @param svc_buf    服务名输出缓冲区
 * @param svc_size   服务名缓冲区大小
 * @param method_buf 方法名输出缓冲区
 * @param method_size 方法名缓冲区大小
 * @return 0 成功，-1 失败
 */
static int grpc_parse_path(const char *path,
                           char *svc_buf, size_t svc_size,
                           char *method_buf, size_t method_size) {
    if (!path || path[0] != '/') {
        return -1;
    }

    /* 跳过开头的 '/' */
    const char *p = path + 1;

    /* 查找第二个 '/'（分隔 service 和 method） */
    const char *slash = strrchr(p, '/');
    if (!slash || slash == p) {
        return -1; /* 格式错误：没有 method 部分 */
    }

    /* service_name = path[1..slash-1] */
    size_t svc_len = (size_t)(slash - p);
    if (svc_len >= svc_size) {
        svc_len = svc_size - 1;
    }
    memcpy(svc_buf, p, svc_len);
    svc_buf[svc_len] = '\0';

    /* method_name = slash[1..end] */
    size_t method_len = strlen(slash + 1);
    if (method_len >= method_size) {
        method_len = method_size - 1;
    }
    memcpy(method_buf, slash + 1, method_len);
    method_buf[method_len] = '\0';

    return 0;
}

/**
 * extract_content_type - 从 HTTP 请求中提取 Content-Type 头值
 *
 * @param req HTTP 请求
 * @return Content-Type 头值指针，未找到返回 NULL
 */
static const char *extract_content_type(const http_request_t *req) {
    if (!req) return NULL;

    /* 优先使用已解析的 content_type 字段 */
    if (req->content_type[0] != '\0') {
        return req->content_type;
    }

    /* 否则遍历 headers 数组查找 */
    for (int i = 0; i < req->num_headers; i++) {
        if (strcasecmp(req->headers[i].name, "content-type") == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

/* ===== gRPC 请求检测 ===== */

/**
 * grpc_detect - 检测请求是否为 gRPC 请求
 *
 * 检查 Content-Type 是否以 "application/grpc" 开头。
 * 覆盖:
 *   - application/grpc
 *   - application/grpc+proto
 *   - application/grpc+json
 *   - application/grpc-web (gRPC-Web)
 *   - application/grpc-web+proto
 */
bool grpc_detect(const http_request_t *req) {
    if (!req) return false;

    const char *ct = extract_content_type(req);
    if (!ct) return false;

    /* 检查 "application/grpc" 前缀（大小写不敏感） */
    if (strncasecmp(ct, "application/grpc", 16) == 0) {
        char c = ct[16];
        /* 标准 gRPC: application/grpc, application/grpc+proto, ... */
        if (c == '\0' || c == '+' || c == ';' || c == ' ') {
            return true;
        }
        /* gRPC-Web: application/grpc-web, application/grpc-web+proto */
        if (c == '-' && strncasecmp(ct + 17, "web", 3) == 0) {
            char d = ct[20];
            if (d == '\0' || d == '+' || d == ';' || d == ' ') {
                return true;
            }
        }
    }

    return false;
}

/**
 * grpc_is_grpc_web - 检测请求是否为 gRPC-Web
 *
 * gRPC-Web 使用 application/grpc-web 前缀。
 */
bool grpc_is_grpc_web(const http_request_t *req) {
    if (!req) return false;

    const char *ct = extract_content_type(req);
    if (!ct) return false;

    if (strncasecmp(ct, "application/grpc-web", 20) == 0) {
        char c = ct[20];
        if (c == '\0' || c == '+' || c == ';' || c == ' ') {
            return true;
        }
    }

    return false;
}

/* ===== gRPC 请求解析 ===== */

/**
 * grpc_parse_request - 从 HTTP/2 请求解析 gRPC 上下文
 *
 * 解析路径获取 service/method，并解析消息体中的 gRPC 消息帧。
 * 默认标记为非流式（Unary），流模式由调用方根据 protobuf 定义设置。
 */
int grpc_parse_request(const http_request_t *req, grpc_request_t *grpc_req) {
    if (!req || !grpc_req) return -1;

    /* 清空输出结构 */
    memset(grpc_req, 0, sizeof(grpc_request_t));

    /* 检测 gRPC-Web */
    grpc_req->is_grpc_web = grpc_is_grpc_web(req);

    /* 解析 service/method */
    if (grpc_parse_path(req->path,
                        grpc_req->service_name, sizeof(grpc_req->service_name),
                        grpc_req->method_name, sizeof(grpc_req->method_name)) != 0) {
        /* 路径格式错误，设置默认值 */
        grpc_req->status = GRPC_INTERNAL;
        snprintf(grpc_req->status_message, sizeof(grpc_req->status_message),
                 "Invalid gRPC path format: %.200s", req->path);
        return -1;
    }

    /* 默认流模式：Unary（非流式） */
    grpc_req->client_streaming = false;
    grpc_req->server_streaming = false;
    grpc_req->is_streaming = false;

    /* 解析消息体中的 gRPC 消息帧 */
    if (req->body && req->body_len > 0) {
        int decoded = grpc_decode_message((const uint8_t *)req->body,
                                          req->body_len, &grpc_req->message);
        if (decoded < 0) {
            grpc_req->status = GRPC_INTERNAL;
            snprintf(grpc_req->status_message, sizeof(grpc_req->status_message),
                     "Failed to decode gRPC message frame");
            return -1;
        }
    }

    /* 提取元数据（非 Content-Type 的请求头） */
    grpc_req->metadata_count = 0;
    for (int i = 0; i < req->num_headers && grpc_req->metadata_count < 16; i++) {
        const char *name = req->headers[i].name;
        /* 跳过 HTTP/2 伪头和标准传输头 */
        if (name[0] == ':') continue;
        if (strcasecmp(name, "content-type") == 0) continue;
        if (strcasecmp(name, "content-length") == 0) continue;
        if (strcasecmp(name, "te") == 0) continue;
        if (strcasecmp(name, "host") == 0) continue;

        snprintf(grpc_req->metadata[grpc_req->metadata_count][0], 256, "%s", name);
        snprintf(grpc_req->metadata[grpc_req->metadata_count][1], 256, "%s",
                 req->headers[i].value);
        grpc_req->metadata_count++;
    }

    grpc_req->status = GRPC_OK;
    return 0;
}

/* ===== gRPC 消息帧编解码 ===== */

/**
 * grpc_decode_message - 解码 gRPC 长度前缀消息帧
 *
 * 帧格式: [compressed:1][length:4(BE)][payload:N]
 * payload 动态分配，调用者需通过 grpc_message_free() 释放。
 */
int grpc_decode_message(const uint8_t *buf, size_t len, grpc_message_t *msg) {
    if (!buf || !msg || len == 0) return -1;

    /* 至少需要 5 字节前缀 */
    if (len < 5) return -1;

    /* 解析压缩标志 */
    msg->compressed = buf[0];

    /* 解析长度（big-endian uint32） */
    msg->length = ((uint32_t)buf[1] << 24) |
                  ((uint32_t)buf[2] << 16) |
                  ((uint32_t)buf[3] << 8)  |
                  (uint32_t)buf[4];

    /* 检查数据完整性 */
    if (len < 5 + msg->length) return -1;

    /* 分配 payload 内存 */
    if (msg->length > 0) {
        msg->payload = (uint8_t *)malloc(msg->length);
        if (!msg->payload) return -1;
        memcpy(msg->payload, buf + 5, msg->length);
    } else {
        msg->payload = NULL;
    }

    return (int)(5 + msg->length);
}

/**
 * grpc_encode_message - 编码 gRPC 长度前缀消息帧
 *
 * 帧格式: [compressed:1][length:4(BE)][payload:N]
 */
int grpc_encode_message(const grpc_message_t *msg, uint8_t *buf, size_t buf_size) {
    if (!msg || !buf) return -1;

    /* 检查缓冲区是否足够（5 字节前缀 + payload） */
    if (buf_size < 5 + msg->length) return -1;

    /* 写入压缩标志 */
    buf[0] = msg->compressed;

    /* 写入长度（big-endian uint32） */
    buf[1] = (uint8_t)((msg->length >> 24) & 0xFF);
    buf[2] = (uint8_t)((msg->length >> 16) & 0xFF);
    buf[3] = (uint8_t)((msg->length >> 8) & 0xFF);
    buf[4] = (uint8_t)(msg->length & 0xFF);

    /* 复制 payload */
    if (msg->length > 0 && msg->payload) {
        memcpy(buf + 5, msg->payload, msg->length);
    }

    return (int)(5 + msg->length);
}

/**
 * grpc_message_free - 释放 gRPC 消息中动态分配的资源
 */
void grpc_message_free(grpc_message_t *msg) {
    if (!msg) return;

    if (msg->payload) {
        free(msg->payload);
        msg->payload = NULL;
    }
    msg->length = 0;
    msg->compressed = 0;
}

/* ===== gRPC Trailers ===== */

/**
 * grpc_format_response_trailers - 格式化 gRPC trailers
 *
 * gRPC 使用 HTTP/2 trailing headers 传递最终状态。
 * 格式: grpc-status: N\r\ngrpc-message: text\r\n
 *
 * gRPC-Web 不使用 trailers，而是在响应头中发送 grpc-status。
 */
int grpc_format_response_trailers(const grpc_request_t *grpc_req, char *buf, size_t buf_size) {
    if (!grpc_req || !buf || buf_size == 0) return -1;

    int n = snprintf(buf, buf_size,
                     "grpc-status: %d\r\n"
                     "grpc-message: %s\r\n",
                     (int)grpc_req->status,
                     grpc_req->status_message[0] ? grpc_req->status_message : "");

    if ((size_t)n >= buf_size) return -1;
    return n;
}

/* ===== 状态码转换 ===== */

/**
 * grpc_status_to_http - gRPC 状态码转 HTTP 状态码
 *
 * 映射关系参照 gRPC 规范：
 * https://github.com/grpc/grpc/blob/master/doc/http-grpc-status-mapping.md
 */
int grpc_status_to_http(grpc_status_t status) {
    switch (status) {
        case GRPC_OK:                  return 200;
        case GRPC_CANCELLED:           return 499; /* Client Closed Request */
        case GRPC_UNKNOWN:             return 500; /* Internal Server Error */
        case GRPC_INVALID_ARGUMENT:    return 400; /* Bad Request */
        case GRPC_DEADLINE_EXCEEDED:   return 504; /* Gateway Timeout */
        case GRPC_NOT_FOUND:           return 404; /* Not Found */
        case GRPC_ALREADY_EXISTS:      return 409; /* Conflict */
        case GRPC_PERMISSION_DENIED:   return 403; /* Forbidden */
        case GRPC_RESOURCE_EXHAUSTED:  return 429; /* Too Many Requests */
        case GRPC_FAILED_PRECONDITION: return 400; /* Bad Request */
        case GRPC_ABORTED:             return 409; /* Conflict */
        case GRPC_OUT_OF_RANGE:        return 400; /* Bad Request */
        case GRPC_UNIMPLEMENTED:       return 501; /* Not Implemented */
        case GRPC_INTERNAL:            return 500; /* Internal Server Error */
        case GRPC_UNAVAILABLE:         return 503; /* Service Unavailable */
        case GRPC_DATA_LOSS:           return 500; /* Internal Server Error */
        case GRPC_UNAUTHENTICATED:     return 401; /* Unauthorized */
        default:                       return 500;
    }
}

/**
 * grpc_http_to_status - HTTP 状态码转 gRPC 状态码
 *
 * HTTP → gRPC 反向映射。
 */
grpc_status_t grpc_http_to_status(int http_status) {
    switch (http_status) {
        case 200: return GRPC_OK;
        case 400: return GRPC_INVALID_ARGUMENT;
        case 401: return GRPC_UNAUTHENTICATED;
        case 403: return GRPC_PERMISSION_DENIED;
        case 404: return GRPC_NOT_FOUND;
        case 409: return GRPC_ABORTED;
        case 412: return GRPC_FAILED_PRECONDITION;
        case 429: return GRPC_RESOURCE_EXHAUSTED;
        case 499: return GRPC_CANCELLED;
        case 500: return GRPC_INTERNAL;
        case 501: return GRPC_UNIMPLEMENTED;
        case 503: return GRPC_UNAVAILABLE;
        case 504: return GRPC_DEADLINE_EXCEEDED;
        default:
            if (http_status >= 200 && http_status < 300) return GRPC_OK;
            if (http_status >= 400 && http_status < 500) return GRPC_INVALID_ARGUMENT;
            return GRPC_INTERNAL;
    }
}

/**
 * grpc_status_to_string - gRPC 状态码转可读字符串
 *
 * @return 状态码名称，未知值返回 "UNKNOWN"
 */
const char *grpc_status_to_string(grpc_status_t status) {
    switch (status) {
        case GRPC_OK:                  return "OK";
        case GRPC_CANCELLED:           return "CANCELLED";
        case GRPC_UNKNOWN:             return "UNKNOWN";
        case GRPC_INVALID_ARGUMENT:    return "INVALID_ARGUMENT";
        case GRPC_DEADLINE_EXCEEDED:   return "DEADLINE_EXCEEDED";
        case GRPC_NOT_FOUND:           return "NOT_FOUND";
        case GRPC_ALREADY_EXISTS:      return "ALREADY_EXISTS";
        case GRPC_PERMISSION_DENIED:   return "PERMISSION_DENIED";
        case GRPC_RESOURCE_EXHAUSTED:  return "RESOURCE_EXHAUSTED";
        case GRPC_FAILED_PRECONDITION: return "FAILED_PRECONDITION";
        case GRPC_ABORTED:             return "ABORTED";
        case GRPC_OUT_OF_RANGE:        return "OUT_OF_RANGE";
        case GRPC_UNIMPLEMENTED:       return "UNIMPLEMENTED";
        case GRPC_INTERNAL:            return "INTERNAL";
        case GRPC_UNAVAILABLE:         return "UNAVAILABLE";
        case GRPC_DATA_LOSS:           return "DATA_LOSS";
        case GRPC_UNAUTHENTICATED:     return "UNAUTHENTICATED";
        default:                       return "UNKNOWN";
    }
}

/* ===== gRPC 响应发送 ===== */

/**
 * grpc_send_unary_response - 发送 Unary RPC 响应
 *
 * 将响应 payload 编码为 gRPC 消息帧格式，通过 socket 发送。
 * 编码格式: [flag:1][length:4(BE)][payload]
 */
int grpc_send_unary_response(cocoon_socket_t fd, grpc_request_t *grpc_req,
                             const uint8_t *resp_payload, size_t resp_len) {
    if (fd == COCOON_INVALID_SOCKET || !grpc_req) return -1;

    /* 构建响应消息帧 */
    grpc_message_t resp_msg = {
        .compressed = 0,  /* 默认不压缩 */
        .length = (uint32_t)resp_len,
        .payload = (uint8_t *)(uintptr_t)resp_payload  /* const 转换，编码时不修改 */
    };

    /* 分配编码缓冲区 */
    size_t buf_size = 5 + resp_len;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    if (!buf) return -1;

    int encoded = grpc_encode_message(&resp_msg, buf, buf_size);
    if (encoded < 0) {
        free(buf);
        return -1;
    }

    /* 通过 socket 发送 */
    ssize_t sent = cocoon_socket_send(fd, (const char *)buf, (size_t)encoded);
    free(buf);

    if (sent < 0) return -1;
    return (int)sent;
}

/**
 * grpc_send_trailers - 发送 gRPC trailers
 *
 * 格式化并发送 gRPC trailing headers。
 * 在当前简化实现中，将 trailers 格式化为文本缓冲区返回。
 * 实际 HTTP/2 trailers 发送需由调用方通过 nghttp2 完成。
 */
int grpc_send_trailers(cocoon_socket_t fd, uint32_t stream_id,
                       grpc_status_t status, const char *message) {
    if (fd == COCOON_INVALID_SOCKET) return -1;

    /* 构建临时 grpc_request_t 用于格式化 */
    grpc_request_t tmp_req = {0};
    tmp_req.status = status;
    if (message) {
        snprintf(tmp_req.status_message, sizeof(tmp_req.status_message), "%s", message);
    }

    char trailers_buf[512];
    int n = grpc_format_response_trailers(&tmp_req, trailers_buf, sizeof(trailers_buf));
    if (n < 0) return -1;

    /* 发送 trailers 文本（实际应由 HTTP/2 层包装为 HEADERS 帧） */
    ssize_t sent = cocoon_socket_send(fd, trailers_buf, (size_t)n);
    (void)stream_id; /* 在完整 HTTP/2 实现中用于 nghttp2_submit_trailer */

    if (sent < 0) return -1;
    return (int)sent;
}

/**
 * grpc_request_free - 释放 gRPC 请求资源
 *
 * 释放 grpc_request_t 中所有动态分配的内存。
 * 包括 message.payload 和关联资源。
 */
void grpc_request_free(grpc_request_t *grpc_req) {
    if (!grpc_req) return;

    /* 释放消息 payload */
    grpc_message_free(&grpc_req->message);

    /* 清空其他字段 */
    grpc_req->metadata_count = 0;
}

/**
 * grpc_error_response - 发送 gRPC 错误响应
 *
 * 直接发送 grpc-status trailers，不发送消息体。
 * 适用于请求解析失败、方法未找到、未实现等场景。
 */
void grpc_error_response(cocoon_socket_t fd, uint32_t stream_id,
                         grpc_status_t status, const char *message) {
    if (fd == COCOON_INVALID_SOCKET) return;

    const char *status_msg = message ? message : grpc_status_to_string(status);

    /* 发送错误 trailers */
    grpc_send_trailers(fd, stream_id, status, status_msg);
}

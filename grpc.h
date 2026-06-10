/**
 * @file grpc.h - gRPC over HTTP/2 支持模块
 *
 * 基于现有 HTTP/2 传输层，实现 gRPC 协议支持。
 * 包含 gRPC 状态码、消息帧编解码、四种 RPC 模式、trailers 格式化。
 *
 * gRPC message frame format:
 *   [1 byte: compressed flag] [4 bytes: length (big-endian)] [N bytes: payload]
 *
 * @author Cocoon Team
 */

#ifndef COCOON_GRPC_H
#define COCOON_GRPC_H

#include "http.h"
#include "platform.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== gRPC 状态码 (17 个标准状态码) ===== */
/**
 * grpc_status_t - gRPC 标准状态码
 *
 * 参照 https://grpc.io/docs/guides/status-codes/
 */
typedef enum {
    GRPC_OK = 0,                  /**< 成功 */
    GRPC_CANCELLED = 1,           /**< 操作已取消 */
    GRPC_UNKNOWN = 2,             /**< 未知错误 */
    GRPC_INVALID_ARGUMENT = 3,    /**< 无效参数 */
    GRPC_DEADLINE_EXCEEDED = 4,   /**< 超时 */
    GRPC_NOT_FOUND = 5,           /**< 未找到 */
    GRPC_ALREADY_EXISTS = 6,      /**< 已存在 */
    GRPC_PERMISSION_DENIED = 7,   /**< 权限拒绝 */
    GRPC_RESOURCE_EXHAUSTED = 8,  /**< 资源耗尽 */
    GRPC_FAILED_PRECONDITION = 9, /**< 前置条件失败 */
    GRPC_ABORTED = 10,            /**< 操作已中止 */
    GRPC_OUT_OF_RANGE = 11,       /**< 超出范围 */
    GRPC_UNIMPLEMENTED = 12,      /**< 未实现 */
    GRPC_INTERNAL = 13,           /**< 内部错误 */
    GRPC_UNAVAILABLE = 14,        /**< 服务不可用 */
    GRPC_DATA_LOSS = 15,          /**< 数据丢失 */
    GRPC_UNAUTHENTICATED = 16,    /**< 未认证 */
    GRPC_STATUS_MAX               /**< 状态码数量上限 */
} grpc_status_t;

/* ===== gRPC 消息帧 ===== */
/**
 * grpc_message_t - gRPC 长度前缀消息帧
 *
 * gRPC 消息格式：
 *   [1 byte flag] [4 bytes length (big-endian)] [length bytes payload]
 *   flag: 0x00 = 未压缩, 0x01 = 压缩 (protobuf 数据)
 *
 * payload 为 opaque 字节流，不包含 protobuf 字段级解析。
 */
typedef struct {
    uint8_t compressed;   /**< 压缩标志：0x00=未压缩, 0x01=压缩 */
    uint32_t length;      /**< payload 长度 */
    uint8_t *payload;     /**< 消息数据（动态分配） */
} grpc_message_t;

/* ===== gRPC 请求上下文 ===== */
/**
 * grpc_request_t - gRPC 请求上下文
 *
 * 从 HTTP/2 请求解析得到的 gRPC 调用信息。
 * 包含 service/method 名、消息、元数据、流模式标记。
 */
typedef struct {
    char service_name[256];      /**< 服务名（从路径 /Service/Method 解析） */
    char method_name[256];       /**< 方法名 */
    grpc_message_t message;      /**< 请求消息帧 */
    grpc_status_t status;        /**< 响应状态 */
    char status_message[256];    /**< 状态消息文本 */

    /* 元数据 (key-value pairs) */
    char metadata[16][2][256];   /**< 元数据键值对数组 */
    size_t metadata_count;       /**< 元数据数量 */

    /* RPC 流模式标记 */
    bool is_streaming;           /**< 是否为流式 RPC（任一方向） */
    bool client_streaming;       /**< 客户端流式（Client Streaming / Bidi） */
    bool server_streaming;       /**< 服务端流式（Server Streaming / Bidi） */

    /* gRPC-Web 兼容 */
    bool is_grpc_web;            /**< 是否为 gRPC-Web 请求 */
} grpc_request_t;

/* ===== gRPC 流管理 ===== */
/**
 * grpc_stream_t - gRPC 流状态管理
 *
 * 用于跟踪流式 RPC 的流生命周期。
 */
typedef struct {
    uint32_t stream_id;          /**< HTTP/2 流 ID */
    cocoon_socket_t fd;          /**< 客户端 socket */
    grpc_request_t *req;         /**< 关联的请求上下文 */
    bool half_closed;            /**< 客户端已半关闭（发送 END_STREAM） */
    bool closed;                 /**< 流已完全关闭 */
} grpc_stream_t;

/* ===== API ===== */

/**
 * grpc_detect - 检测请求是否为 gRPC 请求
 *
 * 检查 Content-Type 是否以 "application/grpc" 开头。
 * 支持 application/grpc、application/grpc+proto、application/grpc+json。
 * 也支持 application/grpc-web 变体（设置 is_grpc_web 标记）。
 *
 * @param req HTTP 请求
 * @return true 是 gRPC/gRPC-Web 请求
 */
bool grpc_detect(const http_request_t *req);

/**
 * grpc_is_grpc_web - 检测请求是否为 gRPC-Web
 *
 * @param req HTTP 请求
 * @return true 是 gRPC-Web 请求
 */
bool grpc_is_grpc_web(const http_request_t *req);

/**
 * grpc_parse_request - 从 HTTP/2 请求解析 gRPC 上下文
 *
 * 解析 :path 伪头获取 service/method，提取消息体。
 * 路径格式: "/package.Service/Method" 或 "/Service/Method"
 *
 * @param req      HTTP 请求（已解析）
 * @param grpc_req 输出 gRPC 请求上下文（调用者已 zero-init）
 * @return 0 成功，-1 失败
 */
int grpc_parse_request(const http_request_t *req, grpc_request_t *grpc_req);

/**
 * grpc_decode_message - 解码 gRPC 长度前缀消息帧
 *
 * 从缓冲区解码一个 gRPC 消息帧。
 * 不解析 protobuf 内容，仅提取 opaque payload。
 *
 * @param buf 输入缓冲区
 * @param len 缓冲区长度
 * @param msg 输出消息结构体（payload 动态分配）
 * @return 成功解码的字节数（含 5 字节前缀），< 0 表示错误或数据不完整
 */
int grpc_decode_message(const uint8_t *buf, size_t len, grpc_message_t *msg);

/**
 * grpc_encode_message - 编码 gRPC 长度前缀消息帧
 *
 * @param msg      消息结构体
 * @param buf      输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 编码后的字节数（含 5 字节前缀），< 0 表示缓冲区不足
 */
int grpc_encode_message(const grpc_message_t *msg, uint8_t *buf, size_t buf_size);

/**
 * grpc_message_free - 释放 gRPC 消息中动态分配的资源
 *
 * @param msg 消息结构体
 */
void grpc_message_free(grpc_message_t *msg);

/**
 * grpc_format_response_trailers - 格式化 gRPC trailers（状态）
 *
 * gRPC 使用 HTTP/2 trailers 传递最终状态。
 * 格式: grpc-status: N\r\ngrpc-message: text\r\n
 *
 * @param grpc_req  gRPC 请求（含 status 和 status_message）
 * @param buf       输出缓冲区
 * @param buf_size  缓冲区大小
 * @return 写入的字节数，< 0 表示缓冲区不足
 */
int grpc_format_response_trailers(const grpc_request_t *grpc_req, char *buf, size_t buf_size);

/**
 * grpc_status_to_http - gRPC 状态码转 HTTP 状态码
 *
 * @param status gRPC 状态码
 * @return 对应的 HTTP 状态码
 */
int grpc_status_to_http(grpc_status_t status);

/**
 * grpc_http_to_status - HTTP 状态码转 gRPC 状态码
 *
 * @param http_status HTTP 状态码
 * @return 对应的 gRPC 状态码
 */
grpc_status_t grpc_http_to_status(int http_status);

/**
 * grpc_status_to_string - gRPC 状态码转可读字符串
 *
 * @param status gRPC 状态码
 * @return 状态码名称字符串
 */
const char *grpc_status_to_string(grpc_status_t status);

/**
 * grpc_send_unary_response - 发送 Unary RPC 响应
 *
 * 发送 gRPC 响应消息（DATA 帧）+ trailers。
 * 编码格式: [flag:1][length:4][payload]
 *
 * @param fd           客户端 socket
 * @param grpc_req     请求上下文
 * @param resp_payload 响应 protobuf payload
 * @param resp_len     payload 长度
 * @return 成功发送的字节数，< 0 错误
 */
int grpc_send_unary_response(cocoon_socket_t fd, grpc_request_t *grpc_req,
                             const uint8_t *resp_payload, size_t resp_len);

/**
 * grpc_send_trailers - 发送 gRPC trailers 结束流
 *
 * 发送 HTTP/2 trailing headers 形式的 gRPC 状态。
 *
 * @param fd        客户端 socket
 * @param stream_id HTTP/2 流 ID
 * @param status    gRPC 状态码
 * @param message   状态消息（可为 NULL）
 * @return 0 成功，< 0 错误
 */
int grpc_send_trailers(cocoon_socket_t fd, uint32_t stream_id,
                       grpc_status_t status, const char *message);

/**
 * grpc_request_free - 释放 gRPC 请求资源
 *
 * 释放 grpc_request_t 中所有动态分配的内存（包括 message.payload）。
 *
 * @param grpc_req gRPC 请求上下文
 */
void grpc_request_free(grpc_request_t *grpc_req);

/**
 * grpc_error_response - 发送 gRPC 错误响应
 *
 * 直接发送带有 grpc-status 的错误 trailers，不发送消息体。
 * 用于请求解析失败、方法未找到等场景。
 *
 * @param fd        客户端 socket
 * @param stream_id HTTP/2 流 ID
 * @param status    gRPC 错误状态码
 * @param message   错误消息（可为 NULL）
 */
void grpc_error_response(cocoon_socket_t fd, uint32_t stream_id,
                         grpc_status_t status, const char *message);

#ifdef __cplusplus
}
#endif

#endif /* COCOON_GRPC_H */

/**
 * @file http3.h
 * @brief HTTP/3 (QUIC) 支持模块头文件
 *
 * 实现 HTTP/3 over QUIC 协议支持，包括：
 *   - QUIC 基础传输层（简化版）：UDP socket、连接 ID、流管理
 *   - HTTP/3 帧处理：HEADERS、DATA、SETTINGS、GOAWAY 等
 *   - QPACK 头部压缩：静态表编码/解码（RFC 9204 Appendix A）
 *   - Variable-length integer 编解码
 *   - TLS 1.3 集成接口（条件编译，OpenSSL 3.2+）
 *
 * @author Cocoon Team
 */

#ifndef COCOON_HTTP3_H
#define COCOON_HTTP3_H

#include "http.h"
#include "platform.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== HTTP/3 帧类型 ===== */
#define HTTP3_FRAME_DATA          0x00
#define HTTP3_FRAME_HEADERS       0x01
#define HTTP3_FRAME_CANCEL_PUSH   0x03
#define HTTP3_FRAME_SETTINGS      0x04
#define HTTP3_FRAME_PUSH_PROMISE  0x05
#define HTTP3_FRAME_GOAWAY        0x06
#define HTTP3_FRAME_MAX_PUSH_ID   0x07
#define HTTP3_FRAME_PRIORITY_UPDATE_REQ   0xF0700
#define HTTP3_FRAME_PRIORITY_UPDATE_PUSH  0xF0701

/* ===== HTTP/3 设置参数 ===== */
#define HTTP3_SETTING_MAX_FIELD_SECTION_SIZE  0x06
#define HTTP3_DEFAULT_MAX_FIELD_SECTION_SIZE  (16384)  /**< 16KB */
#define HTTP3_SETTING_QPACK_MAX_TABLE_CAPACITY 0x01
#define HTTP3_SETTING_BLOCKED_STREAMS         0x07

/* ===== QUIC 常量 ===== */
#define QUIC_MAX_DATAGRAM_SIZE    1200
#define QUIC_MAX_STREAMS_PER_CONN 100
#define QUIC_DEFAULT_IDLE_TIMEOUT 30000  /**< 30 秒（毫秒） */
#define QUIC_MAX_CONN_ID_LEN      8
#define HTTP3_MAX_HEADER_ENTRIES  64
#define QPACK_STATIC_TABLE_SIZE   99

/* ===== HTTP/3 错误码 ===== */
typedef enum {
    HTTP3_NO_ERROR = 0x0100,
    HTTP3_GENERAL_PROTOCOL_ERROR = 0x0101,
    HTTP3_INTERNAL_ERROR = 0x0102,
    HTTP3_STREAM_CREATION_ERROR = 0x0103,
    HTTP3_CLOSED_CRITICAL_STREAM = 0x0104,
    HTTP3_FRAME_UNEXPECTED = 0x0105,
    HTTP3_FRAME_ERROR = 0x0106,
    HTTP3_EXCESSIVE_LOAD = 0x0107,
    HTTP3_ID_ERROR = 0x0108,
    HTTP3_SETTINGS_ERROR = 0x0109,
    HTTP3_MISSING_SETTINGS = 0x010A,
    HTTP3_REQUEST_REJECTED = 0x010B,
    HTTP3_REQUEST_CANCELLED = 0x010C,
    HTTP3_REQUEST_INCOMPLETE = 0x010D,
    HTTP3_EARLY_RESPONSE = 0x010E,
    HTTP3_CONNECT_ERROR = 0x010F,
    HTTP3_VERSION_FALLBACK = 0x0110
} http3_error_t;

/* ===== 前向声明 ===== */
typedef struct quic_stream quic_stream_t;
typedef struct quic_connection quic_connection_t;

/**
 * @brief QPACK 静态表条目
 *
 * RFC 9204 Appendix A 定义的静态表。
 * 包含常见 HTTP 字段名和值的预定义索引。
 */
typedef struct {
    const char *name;   /**< 字段名 */
    const char *value;  /**< 字段值（可为空字符串） */
} qpack_static_entry_t;

/**
 * @brief QUIC 流结构
 *
 * 每个 QUIC 流对应一个此结构体。
 * 流 ID 的低 2 位表示类型：
 *   - 0x00: 客户端发起的双向流
 *   - 0x01: 服务器发起的双向流
 *   - 0x02: 客户端发起的单向流
 *   - 0x03: 服务器发起的单向流
 */
struct quic_stream {
    uint64_t stream_id;       /**< 流 ID */
    uint64_t offset;          /**< 当前发送偏移 */
    uint64_t recv_offset;     /**< 当前接收偏移 */
    bool peer_fin;            /**< 对端已发送 FIN */
    bool local_fin;           /**< 本端已发送 FIN */
    bool reset;               /**< 流是否被重置 */
    uint8_t *recv_buf;        /**< 接收缓冲区（动态分配） */
    size_t recv_buf_len;      /**< 接收缓冲区已用长度 */
    size_t recv_buf_cap;      /**< 接收缓冲区容量 */
    quic_connection_t *conn;  /**< 所属连接 */
    quic_stream_t *next;      /**< 链表下一个节点 */
};

/**
 * @brief QUIC 连接结构
 *
 * 简化版 QUIC 连接管理。
 * 不实现完整拥塞控制，依赖内核 UDP 传输。
 */
struct quic_connection {
    uint64_t conn_id;                 /**< 64-bit 连接 ID */
    cocoon_socket_t udp_fd;           /**< 底层 UDP socket */
    struct sockaddr_storage peer_addr; /**< 对端地址 */
    socklen_t peer_addr_len;          /**< 对端地址长度 */
    bool handshake_complete;          /**< TLS 1.3 握手完成标志 */
    void *tls_conn;                   /**< TLS 连接指针（ opaque，避免暴露 tls_conn_t ） */
    uint32_t max_streams_bidi;        /**< 最大双向流数 */
    uint32_t next_stream_id;          /**< 下一个客户端发起的双向流 ID */
    quic_stream_t *streams;           /**< 活跃流链表头 */
    quic_stream_t *streams_tail;      /**< 活跃流链表尾 */
    uint64_t bytes_received;          /**< 接收字节数 */
    uint64_t bytes_sent;              /**< 发送字节数 */
    uint64_t idle_timeout_ms;         /**< 空闲超时（毫秒） */
    uint64_t last_activity;           /**< 最后活动时间戳（毫秒） */
    bool closed;                      /**< 连接已关闭 */
    bool closing;                     /**< 连接正在关闭 */
    quic_connection_t *next;          /**< 全局连接链表下一个节点 */
};

/**
 * @brief HTTP/3 流结构
 *
 * 封装 QUIC 流，添加 HTTP/3 语义状态。
 */
typedef struct {
    quic_stream_t *qstream;       /**< 底层 QUIC 流 */
    bool headers_received;        /**< 已接收 HEADERS 帧 */
    bool data_received;           /**< 已接收 DATA 帧 */
    bool headers_sent;            /**< 已发送 HEADERS 帧 */
    bool trailers_sent;           /**< 已发送 trailers */
    http3_error_t error_code;     /**< 流错误码 */
    bool request_complete;        /**< 请求接收完整 */
    bool response_complete;       /**< 响应发送完整 */
} http3_stream_t;

/**
 * @brief HTTP/3 会话结构
 *
 * 管理 HTTP/3 连接状态，包含 SETTINGS 和 QPACK 上下文。
 */
typedef struct {
    quic_connection_t *conn;                          /**< 底层 QUIC 连接 */
    uint64_t max_field_section_size;                  /**< SETTINGS: 最大字段段大小 */
    uint64_t qpack_encoder_max_capacity;              /**< QPACK 编码器最大容量 */
    uint64_t qpack_decoder_max_capacity;              /**< QPACK 解码器最大容量 */
    http3_stream_t *h3_streams[QUIC_MAX_STREAMS_PER_CONN]; /**< HTTP/3 流数组 */
    uint64_t goaway_stream_id;                        /**< GOAWAY 流 ID */
    bool settings_received;                           /**< 已收到客户端 SETTINGS */
    bool settings_sent;                               /**< 已发送服务端 SETTINGS */
} http3_session_t;

/**
 * @brief QPACK 编码头部字段结果
 */
typedef struct {
    uint8_t *data;        /**< 编码后的数据（动态分配） */
    size_t len;           /**< 编码后长度 */
} qpack_encoded_t;

/**
 * @brief QPACK 解码头部字段结果
 */
typedef struct {
    char name[HTTP_HEADER_NAME_MAX];    /**< 字段名 */
    char value[HTTP_HEADER_VALUE_MAX];  /**< 字段值 */
    bool valid;                         /**< 解码是否成功 */
} qpack_decoded_t;

/* ===== 全局初始化 / 清理 ===== */

/**
 * @brief 全局 HTTP/3 初始化
 *
 * 初始化 QPACK 静态表等全局资源。
 * 应在服务器启动前调用一次。
 *
 * @return true 成功，false 失败
 */
bool http3_init(void);

/**
 * @brief 全局 HTTP/3 清理
 *
 * 释放所有全局资源。
 */
void http3_cleanup(void);

/* ===== 会话管理 ===== */

/**
 * @brief 创建 HTTP/3 会话
 *
 * 为给定的 QUIC 连接创建 HTTP/3 会话，
 * 发送初始 SETTINGS 帧。
 *
 * @param conn QUIC 连接
 * @return HTTP/3 会话，失败返回 NULL
 */
http3_session_t *http3_session_create(quic_connection_t *conn);

/**
 * @brief 销毁 HTTP/3 会话
 *
 * 释放会话及其管理的所有 HTTP/3 流。
 *
 * @param session HTTP/3 会话
 */
void http3_session_destroy(http3_session_t *session);

/* ===== QUIC 连接管理 ===== */

/**
 * @brief 接受新的 QUIC 连接
 *
 * 从 UDP socket 接收数据报，查找或创建 QUIC 连接。
 *
 * @param udp_fd UDP socket 描述符
 * @return 新连接或已有连接，NULL 表示无新连接
 */
quic_connection_t *http3_accept(cocoon_socket_t udp_fd);

/**
 * @brief 创建 QUIC 连接（内部使用/测试用）
 *
 * @param conn_id 连接 ID
 * @param udp_fd UDP socket
 * @param peer_addr 对端地址
 * @return 新连接，失败返回 NULL
 */
quic_connection_t *quic_connection_create(uint64_t conn_id,
    cocoon_socket_t udp_fd, const struct sockaddr_storage *peer_addr);

/**
 * @brief 销毁 QUIC 连接
 *
 * @param conn QUIC 连接
 */
void quic_connection_destroy(quic_connection_t *conn);

/**
 * @brief 获取或创建 QUIC 流
 *
 * @param conn QUIC 连接
 * @param stream_id 流 ID
 * @return 流指针，失败返回 NULL
 */
quic_stream_t *quic_stream_get_or_create(quic_connection_t *conn, uint64_t stream_id);

/**
 * @brief 销毁 QUIC 流
 *
 * @param conn 所属连接
 * @param stream 要销毁的流
 */
void quic_stream_destroy(quic_connection_t *conn, quic_stream_t *stream);

/**
 * @brief 查找 QUIC 流
 *
 * @param conn QUIC 连接
 * @param stream_id 流 ID
 * @return 流指针，未找到返回 NULL
 */
quic_stream_t *quic_stream_find(quic_connection_t *conn, uint64_t stream_id);

/**
 * @brief 向 QUIC 流写入数据
 *
 * @param stream QUIC 流
 * @param data 数据
 * @param len 数据长度
 * @return 0 成功，< 0 错误
 */
int quic_stream_write(quic_stream_t *stream, const uint8_t *data, size_t len);

/**
 * @brief 从 QUIC 流读取数据
 *
 * @param stream QUIC 流
 * @param buf 输出缓冲区
 * @param len 最大读取长度
 * @return 读取字节数，< 0 错误
 */
ssize_t quic_stream_read(quic_stream_t *stream, uint8_t *buf, size_t len);

/**
 * @brief 设置 QUIC 流 FIN 标志
 *
 * @param stream QUIC 流
 */
void quic_stream_set_fin(quic_stream_t *stream);

/* ===== HTTP/3 请求/响应处理 ===== */

/**
 * @brief 从 HTTP/3 流读取请求
 *
 * 解析 HEADERS 帧（QPACK 解码）和 DATA 帧，构建 http_request_t。
 *
 * @param session HTTP/3 会话
 * @param req 输出 HTTP 请求
 * @return 成功返回流 ID（>= 0），< 0 表示无可用请求
 */
int64_t http3_read_request(http3_session_t *session, http_request_t *req);

/**
 * @brief 发送 HTTP/3 响应
 *
 * 发送 HEADERS 帧（QPACK 编码）和 DATA 帧。
 *
 * @param session HTTP/3 会话
 * @param stream_id 流 ID
 * @param resp HTTP 响应
 * @param body 响应体数据
 * @param body_len 响应体长度
 * @return 0 成功，< 0 错误
 */
int http3_send_response(http3_session_t *session, uint64_t stream_id,
                        const http_response_t *resp,
                        const uint8_t *body, size_t body_len);

/**
 * @brief 发送 HTTP/3 错误响应
 *
 * @param session HTTP/3 会话
 * @param stream_id 流 ID
 * @param status_code HTTP 状态码
 * @param message 错误消息
 */
void http3_send_error(http3_session_t *session, uint64_t stream_id,
                      int status_code, const char *message);

/**
 * @brief 关闭 QUIC 连接
 *
 * @param conn QUIC 连接
 * @param error HTTP/3 错误码
 */
void http3_close_connection(quic_connection_t *conn, http3_error_t error);

/**
 * @brief 发送 SETTINGS 帧
 *
 * @param session HTTP/3 会话
 * @return 0 成功，< 0 错误
 */
int http3_send_settings(http3_session_t *session);

/**
 * @brief 处理 UDP 数据报
 *
 * 接收并处理 UDP 数据报，分发给对应的 QUIC 连接。
 *
 * @param udp_fd UDP socket
 * @param buf 数据报内容
 * @param len 数据报长度
 * @param peer_addr 对端地址
 */
void http3_process_datagram(cocoon_socket_t udp_fd,
                            const uint8_t *buf, size_t len,
                            const struct sockaddr_storage *peer_addr);

/**
 * @brief 处理 HTTP/3 控制流
 *
 * 处理 SETTINGS、GOAWAY 等控制帧。
 *
 * @param session HTTP/3 会话
 * @param stream QUIC 控制流
 * @param data 数据
 * @param len 数据长度
 * @return 0 成功，< 0 错误
 */
int http3_handle_control_stream(http3_session_t *session, quic_stream_t *stream,
                                const uint8_t *data, size_t len);

/* ===== Variable-Length Integer 编解码 ===== */

/**
 * @brief 编码 QUIC variable-length integer
 *
 * 2-bit prefix 指示长度：
 *   - 00 = 1 byte (0..63)
 *   - 01 = 2 bytes (0..16383)
 *   - 10 = 4 bytes (0..1073741823)
 *   - 11 = 8 bytes (0..4611686018427387903)
 *
 * @param value 要编码的值
 * @param buf 输出缓冲区（至少 8 字节）
 * @return 编码后的字节数（1, 2, 4, or 8）
 */
size_t http3_encode_varint(uint64_t value, uint8_t *buf);

/**
 * @brief 解码 QUIC variable-length integer
 *
 * @param buf 输入缓冲区
 * @param len 缓冲区长度
 * @param value 输出值
 * @return 解码后的字节数，< 0 表示错误
 */
int http3_decode_varint(const uint8_t *buf, size_t len, uint64_t *value);

/**
 * @brief 计算帧头编码后的大小
 *
 * @param frame_type 帧类型
 * @param length 帧负载长度
 * @return 帧头总字节数
 */
size_t http3_frame_header_size(uint64_t frame_type, uint64_t length);

/* ===== QPACK 编码/解码 ===== */

/**
 * @brief QPACK 编码头部字段
 *
 * 使用静态表索引或字面量编码单个头部字段。
 *
 * @param name 字段名
 * @param value 字段值
 * @param out 输出缓冲区
 * @param out_cap 输出缓冲区容量
 * @return 编码后字节数，< 0 表示错误
 */
int qpack_encode_header(const char *name, const char *value,
                        uint8_t *out, size_t out_cap);

/**
 * @brief QPACK 解码头部字段
 *
 * @param in 输入缓冲区
 * @param in_len 输入长度
 * @param out 输出解码结果
 * @param consumed 消耗字节数
 * @return 0 成功，< 0 错误
 */
int qpack_decode_header(const uint8_t *in, size_t in_len,
                        qpack_decoded_t *out, size_t *consumed);

/**
 * @brief QPACK 编码完整请求头
 *
 * 编码所有请求伪头和常规头。
 *
 * @param req HTTP 请求
 * @param out 输出缓冲区
 * @param out_cap 输出缓冲区容量
 * @return 编码后字节数，< 0 错误
 */
int qpack_encode_request_headers(const http_request_t *req,
                                  uint8_t *out, size_t out_cap);

/**
 * @brief QPACK 解码完整请求头
 *
 * @param in 输入缓冲区
 * @param in_len 输入长度
 * @param req 输出 HTTP 请求
 * @return 0 成功，< 0 错误
 */
int qpack_decode_request_headers(const uint8_t *in, size_t in_len,
                                  http_request_t *req);

/**
 * @brief QPACK 编码响应头
 *
 * @param resp HTTP 响应
 * @param out 输出缓冲区
 * @param out_cap 输出缓冲区容量
 * @return 编码后字节数，< 0 错误
 */
int qpack_encode_response_headers(const http_response_t *resp,
                                   uint8_t *out, size_t out_cap);

/**
 * @brief QPACK 解码响应头
 *
 * @param in 输入缓冲区
 * @param in_len 输入长度
 * @param resp 输出 HTTP 响应
 * @return 0 成功，< 0 错误
 */
int qpack_decode_response_headers(const uint8_t *in, size_t in_len,
                                   http_response_t *resp);

/* ===== 帧处理辅助函数 ===== */

/**
 * @brief 编码 HTTP/3 帧头
 *
 * 格式：[Type: varint][Length: varint]
 *
 * @param frame_type 帧类型
 * @param length 负载长度
 * @param buf 输出缓冲区（至少 16 字节）
 * @return 编码后帧头字节数
 */
size_t http3_encode_frame_header(uint64_t frame_type, uint64_t length,
                                  uint8_t *buf);

/**
 * @brief 解码 HTTP/3 帧头
 *
 * @param buf 输入缓冲区
 * @param len 缓冲区长度
 * @param frame_type 输出帧类型
 * @param length 输出负载长度
 * @return 解码后帧头字节数，< 0 错误
 */
int http3_decode_frame_header(const uint8_t *buf, size_t len,
                               uint64_t *frame_type, uint64_t *length);

/**
 * @brief 编码完整 HTTP/3 帧
 *
 * @param frame_type 帧类型
 * @param payload 负载数据
 * @param payload_len 负载长度
 * @param buf 输出缓冲区
 * @param buf_cap 缓冲区容量
 * @return 编码后总字节数，< 0 错误
 */
int http3_encode_frame(uint64_t frame_type,
                        const uint8_t *payload, size_t payload_len,
                        uint8_t *buf, size_t buf_cap);

/**
 * @brief 解析 HTTP/3 帧（从流缓冲区）
 *
 * 从 QUIC 流接收缓冲区解析完整帧。
 *
 * @param stream QUIC 流
 * @param frame_type 输出帧类型
 * @param payload 输出负载指针（指向流缓冲区内部）
 * @param payload_len 输出负载长度
 * @return 0 成功，1 数据不足，< 0 错误
 */
int http3_parse_frame(quic_stream_t *stream,
                       uint64_t *frame_type,
                       const uint8_t **payload,
                       size_t *payload_len);

/* ===== 全局连接管理 ===== */

/**
 * @brief 查找现有 QUIC 连接
 *
 * 根据连接 ID 在全局链表中查找。
 *
 * @param conn_id 连接 ID
 * @return 连接指针，未找到返回 NULL
 */
quic_connection_t *quic_find_connection(uint64_t conn_id);

/**
 * @brief 移除并清理超时连接
 *
 * 检查全局连接链表，关闭并释放超时的连接。
 *
 * @param timeout_ms 超时毫秒数
 */
void quic_cleanup_timeout_connections(uint64_t timeout_ms);

/**
 * @brief 生成 64-bit 随机连接 ID
 *
 * @return 随机连接 ID
 */
uint64_t quic_generate_conn_id(void);

/**
 * @brief 获取当前时间戳（毫秒）
 *
 * @return 毫秒时间戳
 */
uint64_t quic_current_time_ms(void);

/**
 * @brief 发送 QUIC 数据报
 *
 * 通过 UDP socket 发送数据报给对端。
 *
 * @param conn QUIC 连接
 * @param data 数据
 * @param len 数据长度
 * @return 0 成功，< 0 错误
 */
int quic_send_datagram(quic_connection_t *conn, const uint8_t *data, size_t len);

/* ===== 连接计数 ===== */

/**
 * @brief 获取活跃 QUIC 连接数
 *
 * @return 活跃连接数
 */
size_t quic_get_connection_count(void);

#ifdef __cplusplus
}
#endif

#endif /* COCOON_HTTP3_H */

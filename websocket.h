#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @file websocket.h
 * @brief WebSocket 协议实现
 *
 * 支持 RFC 6455 WebSocket 握手、帧解析与编码。
 * 服务器端实现：不发送掩码（mask=0），接收客户端掩码帧。
 */

/**
 * @brief WebSocket 操作码
 */
typedef enum {
    WS_OP_CONT = 0x0,   /**< 继续帧 */
    WS_OP_TEXT = 0x1,   /**< 文本帧 */
    WS_OP_BINARY = 0x2, /**< 二进制帧 */
    WS_OP_CLOSE = 0x8,  /**< 关闭帧 */
    WS_OP_PING = 0x9,   /**< Ping 帧 */
    WS_OP_PONG = 0xA    /**< Pong 帧 */
} ws_opcode_t;

/**
 * @brief WebSocket 帧结构
 */
typedef struct {
    uint8_t opcode;      /**< 操作码 */
    bool fin;            /**< 是否为最后一帧 */
    bool masked;         /**< 是否掩码 */
    uint64_t payload_len;/**< 负载长度 */
    uint8_t mask_key[4]; /**< 掩码密钥（仅客户端发送时有效） */
    uint8_t *payload;    /**< 负载数据（已解掩码） */
} ws_frame_t;

/**
 * @brief 执行 WebSocket 握手响应
 *
 * 根据 RFC 6455，对 Sec-WebSocket-Key 计算 SHA1 + Base64 响应。
 *
 * @param fd 客户端 socket
 * @param key 客户端发来的 Sec-WebSocket-Key
 * @return 0 成功，-1 失败
 */
int ws_handshake(int fd, const char *key);

/**
 * @brief 解析单个 WebSocket 帧
 *
 * 从数据流中解析一个完整帧。如果数据不完整，返回 -1 且不修改 frame。
 *
 * @param data 输入数据
 * @param len  数据长度
 * @param frame 输出帧结构（调用者需初始化）
 * @param consumed 输出：消耗的字节数
 * @return 0 成功，-1 数据不完整，-2 格式错误
 */
int ws_parse_frame(const uint8_t *data, size_t len, ws_frame_t *frame, size_t *consumed);

/**
 * @brief 释放帧占用的负载内存
 *
 * @param frame 帧指针
 */
void ws_frame_free(ws_frame_t *frame);

/**
 * @brief 发送 WebSocket 帧
 *
 * @param fd 客户端 socket
 * @param opcode 操作码
 * @param payload 负载数据
 * @param len 负载长度
 * @return 0 成功，-1 失败
 */
int ws_send_frame(int fd, uint8_t opcode, const uint8_t *payload, size_t len);

/**
 * @brief 发送文本帧
 *
 * @param fd 客户端 socket
 * @param text 文本内容（UTF-8）
 * @return 0 成功，-1 失败
 */
int ws_send_text(int fd, const char *text);

/**
 * @brief 发送关闭帧
 *
 * @param fd 客户端 socket
 * @param code 关闭码（如 1000）
 * @param reason 关闭原因（可为 NULL）
 * @return 0 成功，-1 失败
 */
int ws_send_close(int fd, uint16_t code, const char *reason);

/**
 * @brief 发送 Ping 帧
 *
 * @param fd 客户端 socket
 * @return 0 成功，-1 失败
 */
int ws_send_ping(int fd);

/**
 * @brief 发送 Pong 帧
 *
 * @param fd 客户端 socket
 * @param payload Ping 的负载（可为 NULL）
 * @param len 负载长度
 * @return 0 成功，-1 失败
 */
int ws_send_pong(int fd, const uint8_t *payload, size_t len);

/**
 * @brief 向所有活跃 WebSocket 连接广播文本消息
 *
 * @param text 文本内容
 * @return 成功发送的连接数
 */
int ws_broadcast(const char *text);

/**
 * @brief 向指定频道的所有连接发送文本消息
 *
 * @param path 频道路径
 * @param text 文本内容
 * @return 成功发送的连接数
 */
int ws_broadcast_to_path(const char *path, const char *text);

/**
 * @brief 获取当前 WebSocket 连接数
 *
 * @return 活跃连接数
 */
int ws_connection_count(void);

/**
 * @brief 处理 WebSocket 连接（主循环）
 *
 * 进入 WebSocket 帧循环，处理文本/二进制/ping/pong/close。
 * 连接自动注册到全局广播表，退出时注销。
 *
 * @param fd 客户端 socket
 * @param timeout_ms 超时毫秒（0 表示默认）
 * @param path WebSocket 握手路径（频道标识，可为 NULL）
 */
void ws_handle_connection(int fd, uint32_t timeout_ms, const char *path);

#endif /* WEBSOCKET_H */

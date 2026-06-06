/**
 * @file websocket.c
 * @brief WebSocket 协议实现（RFC 6455）
 *
 * 支持握手、帧解析、帧编码、连接管理。
 * 新增广播与频道路由系统（2026-06-05）。
 */

#include "websocket.h"
#include "log.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdatomic.h>

/* WebSocket 魔数字符串（GUID） */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* 基础 64 编码表 */
static const char BASE64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * @brief 简单 Base64 编码（内部使用）
 *
 * @param in  输入数据
 * @param in_len 输入长度
 * @param out 输出缓冲区（至少 4/3 * in_len + 4 字节）
 * @return 输出字符串长度
 */
static size_t base64_encode(const uint8_t *in, size_t in_len, char *out) {
    size_t i, j;
    for (i = 0, j = 0; i + 2 < in_len; i += 3, j += 4) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[j]     = BASE64_TABLE[(v >> 18) & 0x3F];
        out[j + 1] = BASE64_TABLE[(v >> 12) & 0x3F];
        out[j + 2] = BASE64_TABLE[(v >> 6)  & 0x3F];
        out[j + 3] = BASE64_TABLE[v & 0x3F];
    }
    if (i < in_len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        out[j]     = BASE64_TABLE[(v >> 18) & 0x3F];
        out[j + 1] = BASE64_TABLE[(v >> 12) & 0x3F];
        out[j + 2] = (i + 1 < in_len) ? BASE64_TABLE[(v >> 6) & 0x3F] : '=';
        out[j + 3] = '=';
        j += 4;
    }
    out[j] = '\0';
    return j;
}

/**
 * @brief 生成 WebSocket 握手响应的 Sec-WebSocket-Accept
 *
 * 对 key + GUID 计算 SHA1，然后 Base64 编码。
 *
 * @param key  客户端 Sec-WebSocket-Key
 * @param accept 输出缓冲区（至少 32 字节）
 * @return 0 成功
 */
static int ws_compute_accept(const char *key, char *accept) {
    char concat[128];
    size_t key_len = strlen(key);
    if (key_len + sizeof(WS_GUID) >= sizeof(concat)) return -1;

    memcpy(concat, key, key_len);
    memcpy(concat + key_len, WS_GUID, sizeof(WS_GUID) - 1);
    concat[key_len + sizeof(WS_GUID) - 1] = '\0';

    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1((unsigned char *)concat, key_len + sizeof(WS_GUID) - 1, digest);

    base64_encode(digest, SHA_DIGEST_LENGTH, accept);
    return 0;
}

/**
 * @brief 发送完整的 HTTP 响应
 *
 * @param fd 客户端 socket
 * @param buf 数据
 * @param len 长度
 * @return 0 成功，-1 失败
 */
static int ws_send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

int ws_handshake(int fd, const char *key) {
    char accept[32];
    if (ws_compute_accept(key, accept) != 0) return -1;

    char response[512];
    int n = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "Server: Cocoon/1.0\r\n"
        "\r\n",
        accept);

    if (n < 0 || (size_t)n >= sizeof(response)) return -1;
    return ws_send_all(fd, response, (size_t)n);
}

int ws_parse_frame(const uint8_t *data, size_t len, ws_frame_t *frame, size_t *consumed) {
    if (len < 2) return -1; /* 至少需要 2 字节 */

    uint8_t b0 = data[0];
    uint8_t b1 = data[1];

    frame->fin = (b0 >> 7) & 1;
    frame->opcode = b0 & 0x0F;
    frame->masked = (b1 >> 7) & 1;
    uint64_t payload_len = b1 & 0x7F;

    size_t header_len = 2;

    if (payload_len == 126) {
        if (len < 4) return -1;
        payload_len = ((uint64_t)data[2] << 8) | data[3];
        header_len = 4;
    } else if (payload_len == 127) {
        if (len < 10) return -1;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | data[2 + i];
        }
        header_len = 10;
    }

    if (frame->masked) {
        if (len < header_len + 4) return -1;
        memcpy(frame->mask_key, data + header_len, 4);
        header_len += 4;
    }

    if (len < header_len + payload_len) return -1;

    frame->payload_len = payload_len;
    if (payload_len > 0) {
        frame->payload = (uint8_t *)malloc(payload_len + 1);
        if (!frame->payload) return -2;
        memcpy(frame->payload, data + header_len, payload_len);
        if (frame->masked) {
            for (uint64_t i = 0; i < payload_len; i++) {
                frame->payload[i] ^= frame->mask_key[i % 4];
            }
        }
        frame->payload[payload_len] = '\0';
    } else {
        frame->payload = NULL;
    }

    *consumed = header_len + payload_len;
    return 0;
}

void ws_frame_free(ws_frame_t *frame) {
    if (frame && frame->payload) {
        free(frame->payload);
        frame->payload = NULL;
    }
}

int ws_send_frame(int fd, uint8_t opcode, const uint8_t *payload, size_t len) {
    uint8_t header[14];
    size_t header_len = 0;

    header[0] = 0x80 | (opcode & 0x0F); /* FIN=1, opcode */

    if (len <= 125) {
        header[1] = (uint8_t)len;
        header_len = 2;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)(len & 0xFF);
        header_len = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (uint8_t)(len >> (56 - i * 8));
        }
        header_len = 10;
    }

    /* 服务器发送不掩码 */
    if (ws_send_all(fd, (char *)header, header_len) != 0) return -1;
    if (len > 0 && ws_send_all(fd, (char *)payload, len) != 0) return -1;
    return 0;
}

int ws_send_text(int fd, const char *text) {
    return ws_send_frame(fd, WS_OP_TEXT, (const uint8_t *)text, strlen(text));
}

int ws_send_close(int fd, uint16_t code, const char *reason) {
    uint8_t payload[128];
    size_t len = 0;
    if (code != 0) {
        payload[0] = (uint8_t)(code >> 8);
        payload[1] = (uint8_t)(code & 0xFF);
        len = 2;
        if (reason) {
            size_t rlen = strlen(reason);
            if (rlen > sizeof(payload) - 3) rlen = sizeof(payload) - 3;
            memcpy(payload + 2, reason, rlen);
            len += rlen;
        }
    }
    return ws_send_frame(fd, WS_OP_CLOSE, payload, len);
}

int ws_send_ping(int fd) {
    return ws_send_frame(fd, WS_OP_PING, NULL, 0);
}

int ws_send_pong(int fd, const uint8_t *payload, size_t len) {
    return ws_send_frame(fd, WS_OP_PONG, payload, len);
}

/* ============================================================
 * WebSocket 连接注册表 — 广播与频道路由
 * ============================================================ */

typedef struct ws_conn_node {
    int fd;                          /**< 客户端 socket */
    char *path;                      /**< 握手路径（频道标识） */
    struct ws_conn_node *next;       /**< 链表下一个节点 */
} ws_conn_node_t;

/** 全局注册表 */
typedef struct {
    ws_conn_node_t *head;            /**< 链表头 */
    pthread_mutex_t mutex;           /**< 保护链表的互斥锁 */
    atomic_int count;                /**< 当前连接数 */
} ws_registry_t;

static ws_registry_t g_ws_registry = {
    .head = NULL,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .count = 0
};

/**
 * @brief 注册 WebSocket 连接
 *
 * @param fd 客户端 socket
 * @param path 握手路径（可为 NULL，使用默认频道）
 */
static void ws_registry_add(int fd, const char *path) {
    ws_conn_node_t *node = (ws_conn_node_t *)calloc(1, sizeof(ws_conn_node_t));
    if (!node) return;
    node->fd = fd;
    node->path = path ? strdup(path) : NULL;

    pthread_mutex_lock(&g_ws_registry.mutex);
    node->next = g_ws_registry.head;
    g_ws_registry.head = node;
    atomic_fetch_add(&g_ws_registry.count, 1);
    pthread_mutex_unlock(&g_ws_registry.mutex);

    log_info("WebSocket 注册 fd=%d 路径=%s，当前连接数: %d",
             fd, path ? path : "(default)", atomic_load(&g_ws_registry.count));
}

/**
 * @brief 注销 WebSocket 连接
 *
 * @param fd 客户端 socket
 */
static void ws_registry_remove(int fd) {
    pthread_mutex_lock(&g_ws_registry.mutex);
    ws_conn_node_t **pp = &g_ws_registry.head;
    while (*pp) {
        ws_conn_node_t *node = *pp;
        if (node->fd == fd) {
            *pp = node->next;
            free(node->path);
            free(node);
            atomic_fetch_sub(&g_ws_registry.count, 1);
            log_info("WebSocket 注销 fd=%d，当前连接数: %d",
                     fd, atomic_load(&g_ws_registry.count));
            break;
        }
        pp = &node->next;
    }
    pthread_mutex_unlock(&g_ws_registry.mutex);
}

/**
 * @brief 向所有活跃 WebSocket 连接广播文本消息
 *
 * @param text 文本内容
 * @return 成功发送的连接数
 */
int ws_broadcast(const char *text) {
    if (!text) return 0;
    int sent = 0;
    pthread_mutex_lock(&g_ws_registry.mutex);
    ws_conn_node_t *node = g_ws_registry.head;
    while (node) {
        if (ws_send_text(node->fd, text) == 0) {
            sent++;
        }
        node = node->next;
    }
    pthread_mutex_unlock(&g_ws_registry.mutex);
    log_debug("WebSocket 广播: %s -> %d/%d 连接", text, sent,
              atomic_load(&g_ws_registry.count));
    return sent;
}

/**
 * @brief 向指定频道的所有连接发送文本消息
 *
 * @param path 频道路径
 * @param text 文本内容
 * @return 成功发送的连接数
 */
int ws_broadcast_to_path(const char *path, const char *text) {
    if (!path || !text) return 0;
    int sent = 0;
    pthread_mutex_lock(&g_ws_registry.mutex);
    ws_conn_node_t *node = g_ws_registry.head;
    while (node) {
        if (node->path && strcmp(node->path, path) == 0) {
            if (ws_send_text(node->fd, text) == 0) {
                sent++;
            }
        }
        node = node->next;
    }
    pthread_mutex_unlock(&g_ws_registry.mutex);
    log_debug("WebSocket 频道广播 [%s]: %s -> %d 连接", path, text, sent);
    return sent;
}

/**
 * @brief 获取当前 WebSocket 连接数
 */
int ws_connection_count(void) {
    return atomic_load(&g_ws_registry.count);
}

/* ============================================================
 * WebSocket 连接处理
 * ============================================================ */

/**
 * @brief 从连接读取数据（简单阻塞读取）
 */
static ssize_t ws_read_data(int fd, uint8_t *buf, size_t max_len) {
    ssize_t n = recv(fd, buf, max_len, 0);
    return n;
}

void ws_handle_connection(int fd, uint32_t timeout_ms, const char *path) {
    uint8_t buf[8192];
    size_t buf_len = 0;
    bool closed = false;

    /* 注册到全局连接表 */
    ws_registry_add(fd, path);

    log_info("WebSocket 连接建立 fd=%d path=%s", fd, path ? path : "(default)");

    while (!closed) {
        /* 等待数据可读或超时 */
        if (timeout_ms > 0) {
            int ret = cocoon_socket_poll_readable(fd, (int)timeout_ms);
            if (ret == 0) {
                log_info("WebSocket fd=%d 空闲超时 (%u ms)，关闭连接", fd, timeout_ms);
                ws_send_close(fd, 1001, "Idle timeout");
                closed = true;
                break;
            }
            if (ret < 0) {
                log_debug("WebSocket fd=%d poll 错误: %s", fd, cocoon_strerror(cocoon_get_last_error()));
                break;
            }
        }

        ssize_t n = ws_read_data(fd, buf + buf_len, sizeof(buf) - buf_len);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                continue;
            }
            log_debug("WebSocket fd=%d 读取结束或错误", fd);
            break;
        }
        buf_len += (size_t)n;

        /* 解析帧 */
        size_t parsed = 0;
        while (parsed < buf_len) {
            ws_frame_t frame = {0};
            size_t consumed = 0;
            int ret = ws_parse_frame(buf + parsed, buf_len - parsed, &frame, &consumed);
            if (ret == -1) {
                break; /* 数据不完整，等待更多 */
            }
            if (ret == -2) {
                log_warn("WebSocket fd=%d 收到畸形帧", fd);
                ws_send_close(fd, 1002, "Protocol error");
                closed = true;
                break;
            }

            parsed += consumed;

            switch (frame.opcode) {
                case WS_OP_TEXT:
                    log_debug("WebSocket fd=%d 收到文本: %s", fd,
                              frame.payload ? (char *)frame.payload : "(empty)");
                    if (frame.fin) {
                        /* 默认行为：echo 回发送者 */
                        ws_send_text(fd, frame.payload ? (char *)frame.payload : "");
                    }
                    break;

                case WS_OP_BINARY:
                    log_debug("WebSocket fd=%d 收到二进制 %llu 字节", fd,
                              (unsigned long long)frame.payload_len);
                    /* 简单 echo 回二进制 */
                    ws_send_frame(fd, WS_OP_BINARY, frame.payload, frame.payload_len);
                    break;

                case WS_OP_CLOSE:
                    log_info("WebSocket fd=%d 收到关闭帧", fd);
                    ws_send_close(fd, 1000, NULL);
                    closed = true;
                    break;

                case WS_OP_PING:
                    log_debug("WebSocket fd=%d 收到 Ping", fd);
                    ws_send_pong(fd, frame.payload, frame.payload_len);
                    break;

                case WS_OP_PONG:
                    log_debug("WebSocket fd=%d 收到 Pong", fd);
                    break;

                case WS_OP_CONT:
                    log_debug("WebSocket fd=%d 收到继续帧", fd);
                    break;

                default:
                    log_warn("WebSocket fd=%d 未知操作码 %d", fd, frame.opcode);
                    break;
            }

            ws_frame_free(&frame);
        }

        /* 移动剩余数据到缓冲区开头 */
        if (parsed > 0 && parsed < buf_len) {
            memmove(buf, buf + parsed, buf_len - parsed);
            buf_len -= parsed;
        } else if (parsed == buf_len) {
            buf_len = 0;
        }
    }

    /* 从全局连接表注销 */
    ws_registry_remove(fd);

    log_info("WebSocket 连接关闭 fd=%d", fd);
}

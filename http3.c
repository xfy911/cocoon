/**
 * @file http3.c
 * @brief HTTP/3 (QUIC) 支持模块实现
 *
 * 包含：
 *   - QUIC Variable-Length Integer 编解码
 *   - HTTP/3 帧处理（HEADERS / DATA / SETTINGS / GOAWAY）
 *   - QPACK 静态表头部压缩（RFC 9204 Appendix A）
 *   - QUIC 流管理（创建、数据读写、FIN 控制）
 *   - QUIC 连接管理（ID 分配、超时、关闭）
 *   - HTTP/3 会话管理
 *   - TLS 1.3 集成接口（条件编译）
 *
 * @author Cocoon Team
 */

#include "http3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* ===== TLS 条件编译 ===== */
#if defined(OPENSSL_HAS_QUIC) && defined(OPENSSL_VERSION_NUMBER)
    #include <openssl/ssl.h>
#endif

/* ===== 全局连接链表 ===== */
static quic_connection_t *g_connections = NULL;
static size_t g_connection_count = 0;

/* ===== QPACK 静态表（RFC 9204 Appendix A） ===== */
static const qpack_static_entry_t s_qpack_static_table[QPACK_STATIC_TABLE_SIZE] = {
    /*  0 */ {":authority", ""},
    /*  1 */ {":path", "/"},
    /*  2 */ {":method", "GET"},
    /*  3 */ {":method", "POST"},
    /*  4 */ {":scheme", "https"},
    /*  5 */ {":scheme", "http"},
    /*  6 */ {":status", "200"},
    /*  7 */ {":status", "204"},
    /*  8 */ {":status", "206"},
    /*  9 */ {":status", "304"},
    /* 10 */ {":status", "400"},
    /* 11 */ {":status", "404"},
    /* 12 */ {":status", "500"},
    /* 13 */ {"accept-charset", ""},
    /* 14 */ {"accept-encoding", "gzip, deflate, br"},
    /* 15 */ {"accept-language", ""},
    /* 16 */ {"accept-ranges", ""},
    /* 17 */ {"accept", ""},
    /* 18 */ {"access-control-allow-origin", ""},
    /* 19 */ {"age", ""},
    /* 20 */ {"allow", ""},
    /* 21 */ {"authorization", ""},
    /* 22 */ {"cache-control", ""},
    /* 23 */ {"content-disposition", ""},
    /* 24 */ {"content-encoding", ""},
    /* 25 */ {"content-language", ""},
    /* 26 */ {"content-length", ""},
    /* 27 */ {"content-location", ""},
    /* 28 */ {"content-range", ""},
    /* 29 */ {"content-type", ""},
    /* 30 */ {"cookie", ""},
    /* 31 */ {"date", ""},
    /* 32 */ {"etag", ""},
    /* 33 */ {"expect", ""},
    /* 34 */ {"expires", ""},
    /* 35 */ {"from", ""},
    /* 36 */ {"host", ""},
    /* 37 */ {"if-match", ""},
    /* 38 */ {"if-modified-since", ""},
    /* 39 */ {"if-none-match", ""},
    /* 40 */ {"if-range", ""},
    /* 41 */ {"if-unmodified-since", ""},
    /* 42 */ {"last-modified", ""},
    /* 43 */ {"link", ""},
    /* 44 */ {"location", ""},
    /* 45 */ {"max-forwards", ""},
    /* 46 */ {"proxy-authenticate", ""},
    /* 47 */ {"proxy-authorization", ""},
    /* 48 */ {"range", ""},
    /* 49 */ {"referer", ""},
    /* 50 */ {"refresh", ""},
    /* 51 */ {"retry-after", ""},
    /* 52 */ {"server", ""},
    /* 53 */ {"set-cookie", ""},
    /* 54 */ {"strict-transport-security", ""},
    /* 55 */ {"transfer-encoding", ""},
    /* 56 */ {"user-agent", ""},
    /* 57 */ {"vary", ""},
    /* 58 */ {"via", ""},
    /* 59 */ {"www-authenticate", ""},
    /* 60 */ {":status", "100"},
    /* 61 */ {":status", "103"},
    /* 62 */ {"accept-encoding", ""},
    /* 63 */ {":path", ""},
    /* 64 */ {"content-type", "application/json"},
    /* 65 */ {"content-type", "text/html"},
    /* 66 */ {"content-type", "text/plain"},
    /* 67 */ {":authority", ""},
    /* 68 */ {":method", "CONNECT"},
    /* 69 */ {":method", "DELETE"},
    /* 70 */ {":method", "HEAD"},
    /* 71 */ {":method", "OPTIONS"},
    /* 72 */ {":method", "PUT"},
    /* 73 */ {":scheme", "https"},
    /* 74 */ {":scheme", "http"},
    /* 75 */ {":status", "100"},
    /* 76 */ {":status", "101"},
    /* 77 */ {":status", "103"},
    /* 78 */ {":status", "201"},
    /* 79 */ {":status", "301"},
    /* 80 */ {":status", "302"},
    /* 81 */ {":status", "304"},
    /* 82 */ {":status", "400"},
    /* 83 */ {":status", "401"},
    /* 84 */ {":status", "403"},
    /* 85 */ {":status", "404"},
    /* 86 */ {":status", "405"},
    /* 87 */ {":status", "406"},
    /* 88 */ {":status", "407"},
    /* 89 */ {":status", "408"},
    /* 90 */ {":status", "409"},
    /* 91 */ {":status", "410"},
    /* 92 */ {":status", "411"},
    /* 93 */ {":status", "412"},
    /* 94 */ {":status", "413"},
    /* 95 */ {":status", "414"},
    /* 96 */ {":status", "415"},
    /* 97 */ {":status", "416"},
    /* 98 */ {":status", "421"},
};

/* ===== Variable-Length Integer 编解码 ===== */

/**
 * @brief 编码 QUIC variable-length integer
 */
size_t http3_encode_varint(uint64_t value, uint8_t *buf) {
    if (!buf) return 0;

    if (value <= 63ULL) {
        buf[0] = (uint8_t)value;
        return 1;
    } else if (value <= 16383ULL) {
        buf[0] = (uint8_t)(((value >> 8) & 0x3F) | 0x40);
        buf[1] = (uint8_t)(value & 0xFF);
        return 2;
    } else if (value <= 1073741823ULL) {
        buf[0] = (uint8_t)(((value >> 24) & 0x3F) | 0x80);
        buf[1] = (uint8_t)((value >> 16) & 0xFF);
        buf[2] = (uint8_t)((value >> 8) & 0xFF);
        buf[3] = (uint8_t)(value & 0xFF);
        return 4;
    } else {
        buf[0] = (uint8_t)(((value >> 56) & 0x3F) | 0xC0);
        buf[1] = (uint8_t)((value >> 48) & 0xFF);
        buf[2] = (uint8_t)((value >> 40) & 0xFF);
        buf[3] = (uint8_t)((value >> 32) & 0xFF);
        buf[4] = (uint8_t)((value >> 24) & 0xFF);
        buf[5] = (uint8_t)((value >> 16) & 0xFF);
        buf[6] = (uint8_t)((value >> 8) & 0xFF);
        buf[7] = (uint8_t)(value & 0xFF);
        return 8;
    }
}

/**
 * @brief 解码 QUIC variable-length integer
 */
int http3_decode_varint(const uint8_t *buf, size_t len, uint64_t *value) {
    if (!buf || !value || len == 0) return -1;

    uint8_t prefix = (buf[0] & 0xC0) >> 6;
    size_t need = 1;
    uint64_t result = 0;

    switch (prefix) {
        case 0:
            need = 1;
            result = buf[0] & 0x3F;
            break;
        case 1:
            need = 2;
            if (len < need) return -1;
            result = (((uint64_t)(buf[0] & 0x3F)) << 8) | buf[1];
            break;
        case 2:
            need = 4;
            if (len < need) return -1;
            result = (((uint64_t)(buf[0] & 0x3F)) << 24) |
                     (((uint64_t)buf[1]) << 16) |
                     (((uint64_t)buf[2]) << 8) |
                     (uint64_t)buf[3];
            break;
        case 3:
            need = 8;
            if (len < need) return -1;
            result = (((uint64_t)(buf[0] & 0x3F)) << 56) |
                     (((uint64_t)buf[1]) << 48) |
                     (((uint64_t)buf[2]) << 40) |
                     (((uint64_t)buf[3]) << 32) |
                     (((uint64_t)buf[4]) << 24) |
                     (((uint64_t)buf[5]) << 16) |
                     (((uint64_t)buf[6]) << 8) |
                     (uint64_t)buf[7];
            break;
    }

    *value = result;
    return (int)need;
}

/**
 * @brief 计算帧头编码后的大小
 */
size_t http3_frame_header_size(uint64_t frame_type, uint64_t length) {
    uint8_t dummy[16];
    return http3_encode_varint(frame_type, dummy) +
           http3_encode_varint(length, dummy + 8);
}

/* ===== HTTP/3 帧处理 ===== */

/**
 * @brief 编码 HTTP/3 帧头
 */
size_t http3_encode_frame_header(uint64_t frame_type, uint64_t length,
                                  uint8_t *buf) {
    if (!buf) return 0;
    size_t n = 0;
    n += http3_encode_varint(frame_type, buf);
    n += http3_encode_varint(length, buf + n);
    return n;
}

/**
 * @brief 解码 HTTP/3 帧头
 */
int http3_decode_frame_header(const uint8_t *buf, size_t len,
                               uint64_t *frame_type, uint64_t *length) {
    if (!buf || !frame_type || !length || len == 0) return -1;

    uint64_t ft = 0, flen = 0;
    int n1 = http3_decode_varint(buf, len, &ft);
    if (n1 < 0) return -1;

    int n2 = http3_decode_varint(buf + n1, len - (size_t)n1, &flen);
    if (n2 < 0) return -1;

    *frame_type = ft;
    *length = flen;
    return n1 + n2;
}

/**
 * @brief 编码完整 HTTP/3 帧
 */
int http3_encode_frame(uint64_t frame_type,
                        const uint8_t *payload, size_t payload_len,
                        uint8_t *buf, size_t buf_cap) {
    if (!buf) return -1;
    size_t header_len = http3_frame_header_size(frame_type, payload_len);
    if (header_len + payload_len > buf_cap) return -1;

    size_t n = 0;
    n += http3_encode_varint(frame_type, buf);
    n += http3_encode_varint(payload_len, buf + n);

    if (payload && payload_len > 0) {
        memcpy(buf + n, payload, payload_len);
        n += payload_len;
    }
    return (int)n;
}

/**
 * @brief 从 QUIC 流接收缓冲区解析完整帧
 */
int http3_parse_frame(quic_stream_t *stream,
                       uint64_t *frame_type,
                       const uint8_t **payload,
                       size_t *payload_len) {
    if (!stream || !frame_type || !payload || !payload_len) return -1;
    if (stream->recv_buf_len == 0) return 1; /* 数据不足 */

    uint64_t ft = 0, flen = 0;
    int header_len = http3_decode_frame_header(stream->recv_buf,
                                                stream->recv_buf_len,
                                                &ft, &flen);
    if (header_len < 0) return 1; /* 数据不足 */

    if ((size_t)header_len + flen > stream->recv_buf_len) return 1; /* 数据不足 */

    *frame_type = ft;
    *payload = stream->recv_buf + header_len;
    *payload_len = (size_t)flen;

    /* 消费已解析的帧 */
    size_t total = (size_t)header_len + (size_t)flen;
    if (total < stream->recv_buf_len) {
        memmove(stream->recv_buf, stream->recv_buf + total,
                stream->recv_buf_len - total);
    }
    stream->recv_buf_len -= total;

    return 0;
}

/* ===== QPACK 编码/解码 ===== */

/**
 * @brief 在静态表中查找完全匹配的条目
 *
 * @param name 字段名
 * @param value 字段值
 * @return 索引（>= 0），未找到返回 -1
 */
static int qpack_find_static_index(const char *name, const char *value) {
    if (!name) return -1;

    for (int i = 0; i < QPACK_STATIC_TABLE_SIZE; i++) {
        if (strcmp(s_qpack_static_table[i].name, name) == 0) {
            if (!value || strcmp(s_qpack_static_table[i].value, value) == 0) {
                return i;
            }
        }
    }
    return -1;
}

/**
 * @brief 在静态表中查找名称匹配的条目
 *
 * @param name 字段名
 * @return 索引（>= 0），未找到返回 -1
 */
static int qpack_find_static_name_index(const char *name) {
    if (!name) return -1;

    for (int i = 0; i < QPACK_STATIC_TABLE_SIZE; i++) {
        if (strcmp(s_qpack_static_table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief 编码字面量字符串（带长度前缀）
 *
 * @param str 字符串
 * @param out 输出缓冲区
 * @param out_cap 缓冲区容量
 * @return 编码后字节数，< 0 错误
 */
static int qpack_encode_literal(const char *str, uint8_t *out, size_t out_cap) {
    if (!str || !out) return -1;
    size_t len = strlen(str);
    uint8_t len_buf[8];
    size_t len_bytes = http3_encode_varint(len, len_buf);
    if (len_bytes + len > out_cap) return -1;

    memcpy(out, len_buf, len_bytes);
    memcpy(out + len_bytes, str, len);
    return (int)(len_bytes + len);
}

/**
 * @brief 解码字面量字符串（带长度前缀）
 *
 * @param in 输入缓冲区
 * @param in_len 输入长度
 * @param out 输出字符串缓冲区
 * @param out_cap 输出缓冲区容量
 * @param consumed 消耗字节数
 * @return 解码后字符串长度，< 0 错误
 */
static int qpack_decode_literal(const uint8_t *in, size_t in_len,
                                 char *out, size_t out_cap,
                                 size_t *consumed) {
    if (!in || !out || !consumed || in_len == 0) return -1;

    uint64_t len = 0;
    int n = http3_decode_varint(in, in_len, &len);
    if (n < 0) return -1;
    if ((size_t)n + len > in_len) return -1;

    size_t copy_len = (len < (uint64_t)out_cap) ? (size_t)len : out_cap - 1;
    memcpy(out, in + n, copy_len);
    out[copy_len] = '\0';
    *consumed = (size_t)n + (size_t)len;
    return (int)copy_len;
}

/**
 * @brief QPACK 编码头部字段
 *
 * 编码策略：
 *   1. 静态表完全匹配 → 索引编码（0b11XXXXXX...）
 *   2. 静态表名称匹配 → 字面量编码，引用名称索引
 *   3. 完全不匹配 → 字面量编码，原始名称+值
 */
int qpack_encode_header(const char *name, const char *value,
                        uint8_t *out, size_t out_cap) {
    if (!name || !value || !out || out_cap == 0) return -1;

    /* 策略1：静态表完全匹配 */
    int idx = qpack_find_static_index(name, value);
    if (idx >= 0) {
        /* Indexed Field Line: 0b1TXXXXXX ... (T=1 for static table) */
        /* QPACK 静态表索引是 0-based */
        uint64_t encoded_idx = (uint64_t)idx;
        if (encoded_idx < 63) {
            if (out_cap < 1) return -1;
            out[0] = (uint8_t)(0xC0 | (encoded_idx & 0x3F));
            return 1;
        } else {
            /* 多字节编码 */
            if (out_cap < 2) return -1;
            out[0] = (uint8_t)(0xC0 | 0x3F); /* 0b111111 = multi-byte marker */
            out[1] = (uint8_t)(encoded_idx - 63);
            return 2;
        }
    }

    /* 策略2：静态表名称匹配，字面量值 */
    int name_idx = qpack_find_static_name_index(name);
    if (name_idx >= 0) {
        /* Literal Field Line with Name Reference: 0b0101XXXX ... */
        /* QPACK 静态表索引是 0-based */
        uint64_t encoded_idx = (uint64_t)name_idx;
        size_t n = 0;
        if (encoded_idx < 15) {
            if (out_cap < 2) return -1;
            out[n++] = (uint8_t)(0x50 | (encoded_idx & 0x0F));
        } else {
            /* 多字节编码 */
            if (out_cap < 3) return -1;
            out[n++] = (uint8_t)(0x5F); /* 0b1111 = multi-byte marker */
            out[n++] = (uint8_t)(encoded_idx - 15);
        }
        int val_n = qpack_encode_literal(value, out + n, out_cap - n);
        if (val_n < 0) return -1;
        return (int)(n + (size_t)val_n);
    }

    /* 策略3：完全字面量编码 */
    /* Literal Field Line with Literal Name: 0b001XXXXX [name] [value] */
    if (out_cap < 1) return -1;
    size_t n = 0;
    out[n++] = 0x20; /* 0b00100000 */
    int name_n = qpack_encode_literal(name, out + n, out_cap - n);
    if (name_n < 0) return -1;
    n += (size_t)name_n;
    int val_n = qpack_encode_literal(value, out + n, out_cap - n);
    if (val_n < 0) return -1;
    return (int)(n + (size_t)val_n);
}

/**
 * @brief QPACK 解码 prefix 整数（RFC 9204 Section 4.1.1）
 *
 * QPACK 使用前缀编码：前 N 位表示值的一部分。
 * 如果值超出前缀位数，则后续字节使用 base-128 延续格式。
 *
 * @param in 输入缓冲区
 * @param in_len 输入长度
 * @param prefix_bits 前缀位数（6 或 4）
 * @param out_value 输出值
 * @return 消耗字节数，< 0 错误
 */
static int qpack_decode_prefix_int(const uint8_t *in, size_t in_len,
                                    int prefix_bits, uint64_t *out_value) {
    if (!in || !out_value || in_len == 0) return -1;

    uint8_t prefix_mask = (uint8_t)((1U << prefix_bits) - 1);
    uint64_t value = (uint64_t)(in[0] & prefix_mask);

    if (value < (uint64_t)prefix_mask) {
        *out_value = value;
        return 1;
    }

    /* Multi-byte: following bytes use base-128 continuation (RFC 9204) */
    size_t pos = 1;
    uint64_t m = 0;
    while (pos < in_len) {
        uint8_t byte = in[pos];
        pos++;
        value += ((uint64_t)(byte & 0x7F)) << m;
        m += 7;
        if ((byte & 0x80) == 0) break;
        if (pos > 8) return -1; /* Too many bytes */
    }

    *out_value = value;
    return (int)pos;
}

/**
 * @brief QPACK 解码头部字段
 */
int qpack_decode_header(const uint8_t *in, size_t in_len,
                        qpack_decoded_t *out, size_t *consumed) {
    if (!in || !out || !consumed || in_len == 0) return -1;

    memset(out, 0, sizeof(*out));
    size_t pos = 0;

    uint8_t first = in[pos];

    /* 判断编码类型 */
    if ((first & 0x80) != 0) {
        /* Indexed Field Line: 1TXXXXXX ... */
        /* T bit: 1=static table, 0=dynamic table */
        bool is_static = (first & 0x40) != 0;
        if (!is_static) {
            /* Dynamic table not implemented */
            return -1;
        }

        uint64_t idx = 0;
        int n = qpack_decode_prefix_int(in + pos, in_len - pos, 6, &idx);
        if (n < 0) return -1;
        pos += (size_t)n;

        /* Static table index is 1-based */
        if (idx >= (uint64_t)QPACK_STATIC_TABLE_SIZE) return -1;
        const qpack_static_entry_t *entry = &s_qpack_static_table[(size_t)idx];
        size_t nlen = strlen(entry->name);
        size_t vlen = strlen(entry->value);
        memcpy(out->name, entry->name, nlen + 1);
        memcpy(out->value, entry->value, vlen + 1);
        out->valid = true;
        *consumed = pos;
        return 0;
    } else if ((first & 0x40) != 0) {
        /* Literal Field Line with Name Reference: 01NTXXXX ... */
        /* N bit = 0x20 (post-base), T bit = 0x10 (1=static, 0=dynamic) */
        bool is_static_name = (first & 0x10) != 0;
        if (!is_static_name) {
            /* Dynamic table not implemented */
            return -1;
        }
        bool value_is_huffman = (first & 0x08) != 0;
        (void)value_is_huffman; /* Huffman decoding not implemented */

        uint64_t idx = 0;
        int n = qpack_decode_prefix_int(in + pos, in_len - pos, 4, &idx);
        if (n < 0) return -1;
        pos += (size_t)n;

        if (idx >= (uint64_t)QPACK_STATIC_TABLE_SIZE) return -1;
        const qpack_static_entry_t *entry = &s_qpack_static_table[(size_t)idx];
        memcpy(out->name, entry->name, strlen(entry->name) + 1);

        /* Decode value literal */
        size_t val_consumed = 0;
        int val_len = qpack_decode_literal(in + pos, in_len - pos,
                                            out->value, sizeof(out->value),
                                            &val_consumed);
        if (val_len < 0) return -1;
        pos += val_consumed;

        out->valid = true;
        *consumed = pos;
        return 0;
    } else if ((first & 0x20) != 0) {
        /* Literal Field Line with Literal Name: 001XXXXX [H|name_len] [name] [value] */
        bool name_is_huffman = (first & 0x10) != 0;
        (void)name_is_huffman;
        pos++;

        /* Decode name */
        size_t name_consumed = 0;
        int name_len = qpack_decode_literal(in + pos, in_len - pos,
                                             out->name, sizeof(out->name),
                                             &name_consumed);
        if (name_len < 0) return -1;
        pos += name_consumed;

        /* Decode value */
        size_t val_consumed = 0;
        int val_len = qpack_decode_literal(in + pos, in_len - pos,
                                            out->value, sizeof(out->value),
                                            &val_consumed);
        if (val_len < 0) return -1;
        pos += val_consumed;

        out->valid = true;
        *consumed = pos;
        return 0;
    } else {
        /* 其他类型（动态表更新等）未实现 */
        return -1;
    }
}

/**
 * @brief QPACK 编码完整请求头
 */
int qpack_encode_request_headers(const http_request_t *req,
                                  uint8_t *out, size_t out_cap) {
    if (!req || !out || out_cap == 0) return -1;

    size_t total = 0;

    /* Required pseudo-headers for request:
     *   :method, :scheme, :authority, :path
     */
    int n;

    /* :method */
    const char *method_str = http_method_str(req->method);
    n = qpack_encode_header(":method", method_str, out + total, out_cap - total);
    if (n < 0) return -1;
    total += (size_t)n;

    /* :scheme (always https for HTTP/3) */
    n = qpack_encode_header(":scheme", "https", out + total, out_cap - total);
    if (n < 0) return -1;
    total += (size_t)n;

    /* :authority (from Host header) */
    const char *authority = "";
    for (int i = 0; i < req->num_headers; i++) {
        if (strcasecmp(req->headers[i].name, "host") == 0) {
            authority = req->headers[i].value;
            break;
        }
    }
    n = qpack_encode_header(":authority", authority, out + total, out_cap - total);
    if (n < 0) return -1;
    total += (size_t)n;

    /* :path */
    n = qpack_encode_header(":path", req->path, out + total, out_cap - total);
    if (n < 0) return -1;
    total += (size_t)n;

    /* Regular headers */
    for (int i = 0; i < req->num_headers; i++) {
        n = qpack_encode_header(req->headers[i].name, req->headers[i].value,
                                out + total, out_cap - total);
        if (n < 0) return -1;
        total += (size_t)n;
    }

    return (int)total;
}

/**
 * @brief QPACK 解码完整请求头
 */
int qpack_decode_request_headers(const uint8_t *in, size_t in_len,
                                  http_request_t *req) {
    if (!in || !req) return -1;

    memset(req, 0, sizeof(*req));
    req->method = HTTP_UNKNOWN;
    req->content_length = -1;

    size_t pos = 0;
    while (pos < in_len) {
        qpack_decoded_t decoded;
        size_t consumed = 0;
        if (qpack_decode_header(in + pos, in_len - pos, &decoded, &consumed) != 0) {
            return -1;
        }
        pos += consumed;

        if (decoded.valid) {
            if (strcmp(decoded.name, ":method") == 0) {
                if (strcmp(decoded.value, "GET") == 0) req->method = HTTP_GET;
                else if (strcmp(decoded.value, "HEAD") == 0) req->method = HTTP_HEAD;
                else if (strcmp(decoded.value, "POST") == 0) req->method = HTTP_POST;
                else if (strcmp(decoded.value, "PUT") == 0) req->method = HTTP_PUT;
                else if (strcmp(decoded.value, "DELETE") == 0) req->method = HTTP_DELETE;
                else if (strcmp(decoded.value, "OPTIONS") == 0) req->method = HTTP_OPTIONS;
                else req->method = HTTP_UNKNOWN;
            } else if (strcmp(decoded.name, ":path") == 0) {
                size_t plen = strlen(decoded.value);
                if (plen > sizeof(req->path) - 1) plen = sizeof(req->path) - 1;
                memcpy(req->path, decoded.value, plen);
                req->path[plen] = '\0';
            } else if (strcmp(decoded.name, ":authority") == 0) {
                if (req->num_headers < HTTP_MAX_HEADERS) {
                    memcpy(req->headers[req->num_headers].name, "Host", 5);
                    size_t vcopy = strlen(decoded.value);
                    if (vcopy > sizeof(req->headers[0].value) - 1)
                        vcopy = sizeof(req->headers[0].value) - 1;
                    memcpy(req->headers[req->num_headers].value, decoded.value, vcopy);
                    req->headers[req->num_headers].value[vcopy] = '\0';
                    req->num_headers++;
                }
            } else if (decoded.name[0] != ':') {
                /* Regular header (skip pseudo-headers starting with ":") */
                if (req->num_headers < HTTP_MAX_HEADERS) {
                    size_t ncopy = strlen(decoded.name);
                    if (ncopy > sizeof(req->headers[0].name) - 1)
                        ncopy = sizeof(req->headers[0].name) - 1;
                    memcpy(req->headers[req->num_headers].name, decoded.name, ncopy);
                    req->headers[req->num_headers].name[ncopy] = '\0';
                    size_t vcopy = strlen(decoded.value);
                    if (vcopy > sizeof(req->headers[0].value) - 1)
                        vcopy = sizeof(req->headers[0].value) - 1;
                    memcpy(req->headers[req->num_headers].value, decoded.value, vcopy);
                    req->headers[req->num_headers].value[vcopy] = '\0';
                    if (strcmp(decoded.name, "content-length") == 0) {
                        req->content_length = atoll(decoded.value);
                    }
                    req->num_headers++;
                }
            }
        }
    }

    return 0;
}

/**
 * @brief QPACK 编码响应头
 */
int qpack_encode_response_headers(const http_response_t *resp,
                                   uint8_t *out, size_t out_cap) {
    if (!resp || !out || out_cap == 0) return -1;

    size_t total = 0;
    int n;
    char status_str[16];

    /* :status pseudo-header */
    snprintf(status_str, sizeof(status_str), "%d", resp->status_code);
    n = qpack_encode_header(":status", status_str, out + total, out_cap - total);
    if (n < 0) return -1;
    total += (size_t)n;

    /* Content-Type */
    if (resp->content_type && resp->content_type[0]) {
        n = qpack_encode_header("content-type", resp->content_type,
                                out + total, out_cap - total);
        if (n < 0) return -1;
        total += (size_t)n;
    }

    /* Content-Length */
    if (resp->content_length >= 0) {
        char clen_str[32];
        snprintf(clen_str, sizeof(clen_str), "%ld", (long)resp->content_length);
        n = qpack_encode_header("content-length", clen_str,
                                out + total, out_cap - total);
        if (n < 0) return -1;
        total += (size_t)n;
    }

    /* Server */
    n = qpack_encode_header("server", "Cocoon/1.0 (HTTP/3)",
                            out + total, out_cap - total);
    if (n < 0) return -1;
    total += (size_t)n;

    return (int)total;
}

/**
 * @brief QPACK 解码响应头
 */
int qpack_decode_response_headers(const uint8_t *in, size_t in_len,
                                   http_response_t *resp) {
    if (!in || !resp) return -1;

    memset(resp, 0, sizeof(*resp));
    resp->status_code = 0;
    resp->content_length = -1;

    size_t pos = 0;
    while (pos < in_len) {
        qpack_decoded_t decoded;
        size_t consumed = 0;
        if (qpack_decode_header(in + pos, in_len - pos, &decoded, &consumed) != 0) {
            return -1;
        }
        pos += consumed;

        if (decoded.valid) {
            if (strcmp(decoded.name, ":status") == 0) {
                resp->status_code = atoi(decoded.value);
            } else if (strcmp(decoded.name, "content-type") == 0) {
                resp->content_type = strdup(decoded.value);
            } else if (strcmp(decoded.name, "content-length") == 0) {
                resp->content_length = atoll(decoded.value);
            }
        }
    }

    return 0;
}


/* ===== QUIC 连接管理 ===== */

/**
 * @brief 获取当前时间戳（毫秒）
 */
uint64_t quic_current_time_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (uint64_t)(time(NULL) * 1000ULL);
    }
    return (uint64_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

/**
 * @brief 生成 64-bit 随机连接 ID
 */
uint64_t quic_generate_conn_id(void) {
    /* 简单的伪随机生成器，使用当前时间作为种子 */
    static unsigned int seed = 0;
    if (seed == 0) {
        seed = (unsigned int)time(NULL);
    }
    uint64_t id = 0;
    id |= ((uint64_t)rand_r(&seed) << 48) & 0xFFFF000000000000ULL;
    id |= ((uint64_t)rand_r(&seed) << 32) & 0x0000FFFF00000000ULL;
    id |= ((uint64_t)rand_r(&seed) << 16) & 0x00000000FFFF0000ULL;
    id |= ((uint64_t)rand_r(&seed))       & 0x000000000000FFFFULL;
    return id;
}

/**
 * @brief 创建 QUIC 连接
 */
quic_connection_t *quic_connection_create(uint64_t conn_id,
    cocoon_socket_t udp_fd, const struct sockaddr_storage *peer_addr) {

    quic_connection_t *conn = (quic_connection_t *)calloc(1, sizeof(quic_connection_t));
    if (!conn) return NULL;

    conn->conn_id = conn_id;
    conn->udp_fd = udp_fd;
    conn->handshake_complete = false;
    conn->tls_conn = NULL;
    conn->max_streams_bidi = QUIC_MAX_STREAMS_PER_CONN;
    conn->next_stream_id = 0; /* 客户端 bidirectional 从 0 开始 */
    conn->streams = NULL;
    conn->streams_tail = NULL;
    conn->bytes_received = 0;
    conn->bytes_sent = 0;
    conn->idle_timeout_ms = QUIC_DEFAULT_IDLE_TIMEOUT;
    conn->last_activity = quic_current_time_ms();
    conn->closed = false;
    conn->closing = false;
    conn->next = NULL;

    if (peer_addr) {
        memcpy(&conn->peer_addr, peer_addr, sizeof(*peer_addr));
        conn->peer_addr_len = sizeof(*peer_addr);
    }

    /* 插入全局链表头部 */
    conn->next = g_connections;
    g_connections = conn;
    g_connection_count++;

    return conn;
}

/**
 * @brief 销毁 QUIC 连接
 */
void quic_connection_destroy(quic_connection_t *conn) {
    if (!conn) return;

    /* 释放所有流 */
    quic_stream_t *stream = conn->streams;
    while (stream) {
        quic_stream_t *next = stream->next;
        if (stream->recv_buf) {
            free(stream->recv_buf);
        }
        free(stream);
        stream = next;
    }
    conn->streams = NULL;
    conn->streams_tail = NULL;

    /* 释放 TLS 连接 */
    if (conn->tls_conn) {
#if defined(OPENSSL_HAS_QUIC) && defined(OPENSSL_VERSION_NUMBER)
        /* 使用 OpenSSL QUIC-TLS 接口 */
        SSL_free((SSL *)conn->tls_conn);
#else
        free(conn->tls_conn);
#endif
        conn->tls_conn = NULL;
    }

    /* 从全局链表中移除 */
    quic_connection_t **pp = &g_connections;
    while (*pp) {
        if (*pp == conn) {
            *pp = conn->next;
            g_connection_count--;
            break;
        }
        pp = &(*pp)->next;
    }

    free(conn);
}

/**
 * @brief 在全局链表中查找 QUIC 连接
 */
quic_connection_t *quic_find_connection(uint64_t conn_id) {
    quic_connection_t *conn = g_connections;
    while (conn) {
        if (conn->conn_id == conn_id && !conn->closed) {
            return conn;
        }
        conn = conn->next;
    }
    return NULL;
}

/**
 * @brief 移除并清理超时连接
 */
void quic_cleanup_timeout_connections(uint64_t timeout_ms) {
    uint64_t now = quic_current_time_ms();
    quic_connection_t *conn = g_connections;
    quic_connection_t *prev = NULL;

    while (conn) {
        quic_connection_t *next = conn->next;
        if (!conn->closed && (now - conn->last_activity) > timeout_ms) {
            /* 超时，移除连接 */
            if (prev) {
                prev->next = next;
            } else {
                g_connections = next;
            }
            g_connection_count--;

            /* 清理连接 */
            quic_stream_t *stream = conn->streams;
            while (stream) {
                quic_stream_t *snext = stream->next;
                if (stream->recv_buf) free(stream->recv_buf);
                free(stream);
                stream = snext;
            }
            if (conn->tls_conn) {
#if defined(OPENSSL_HAS_QUIC) && defined(OPENSSL_VERSION_NUMBER)
                SSL_free((SSL *)conn->tls_conn);
#else
                free(conn->tls_conn);
#endif
            }
            free(conn);
        } else {
            prev = conn;
        }
        conn = next;
    }
}

/**
 * @brief 获取活跃 QUIC 连接数
 */
size_t quic_get_connection_count(void) {
    return g_connection_count;
}

/**
 * @brief 发送 QUIC 数据报
 */
int quic_send_datagram(quic_connection_t *conn, const uint8_t *data, size_t len) {
    if (!conn || !data || len == 0) return -1;
    if (conn->udp_fd == COCOON_INVALID_SOCKET) return -1;

    ssize_t sent = sendto(conn->udp_fd, (const char *)data, len, 0,
                          (const struct sockaddr *)&conn->peer_addr,
                          conn->peer_addr_len);
    if (sent < 0) return -1;

    conn->bytes_sent += (uint64_t)sent;
    conn->last_activity = quic_current_time_ms();
    return 0;
}

/* ===== QUIC 流管理 ===== */

/**
 * @brief 获取或创建 QUIC 流
 */
quic_stream_t *quic_stream_get_or_create(quic_connection_t *conn, uint64_t stream_id) {
    if (!conn) return NULL;

    /* 查找现有流 */
    quic_stream_t *stream = conn->streams;
    while (stream) {
        if (stream->stream_id == stream_id) {
            return stream;
        }
        stream = stream->next;
    }

    /* 创建新流 */
    stream = (quic_stream_t *)calloc(1, sizeof(quic_stream_t));
    if (!stream) return NULL;

    stream->stream_id = stream_id;
    stream->offset = 0;
    stream->recv_offset = 0;
    stream->peer_fin = false;
    stream->local_fin = false;
    stream->reset = false;
    stream->recv_buf = NULL;
    stream->recv_buf_len = 0;
    stream->recv_buf_cap = 0;
    stream->conn = conn;
    stream->next = NULL;

    /* 插入链表尾部 */
    if (conn->streams_tail) {
        conn->streams_tail->next = stream;
    } else {
        conn->streams = stream;
    }
    conn->streams_tail = stream;

    return stream;
}

/**
 * @brief 销毁 QUIC 流
 */
void quic_stream_destroy(quic_connection_t *conn, quic_stream_t *stream) {
    if (!conn || !stream) return;

    /* 从链表中移除 */
    quic_stream_t **pp = &conn->streams;
    while (*pp) {
        if (*pp == stream) {
            *pp = stream->next;
            if (conn->streams_tail == stream) {
                conn->streams_tail = (*pp == NULL) ? NULL : conn->streams;
                /* Fix tail pointer */
                if (conn->streams_tail == NULL && conn->streams != NULL) {
                    quic_stream_t *s = conn->streams;
                    while (s->next) s = s->next;
                    conn->streams_tail = s;
                }
            }
            break;
        }
        pp = &(*pp)->next;
    }

    if (stream->recv_buf) {
        free(stream->recv_buf);
    }
    free(stream);
}

/**
 * @brief 查找 QUIC 流
 */
quic_stream_t *quic_stream_find(quic_connection_t *conn, uint64_t stream_id) {
    if (!conn) return NULL;

    quic_stream_t *stream = conn->streams;
    while (stream) {
        if (stream->stream_id == stream_id) {
            return stream;
        }
        stream = stream->next;
    }
    return NULL;
}

/**
 * @brief 向 QUIC 流写入数据（追加到流的接收缓冲区）
 */
int quic_stream_write(quic_stream_t *stream, const uint8_t *data, size_t len) {
    if (!stream || !data) return -1;

    /* 确保缓冲区容量足够 */
    size_t need = stream->recv_buf_len + len;
    if (need > stream->recv_buf_cap) {
        size_t new_cap = stream->recv_buf_cap * 2;
        if (new_cap == 0) new_cap = 4096;
        while (new_cap < need) new_cap *= 2;

        uint8_t *new_buf = (uint8_t *)realloc(stream->recv_buf, new_cap);
        if (!new_buf) return -1;

        stream->recv_buf = new_buf;
        stream->recv_buf_cap = new_cap;
    }

    memcpy(stream->recv_buf + stream->recv_buf_len, data, len);
    stream->recv_buf_len += len;
    stream->recv_offset += len;

    if (stream->conn) {
        stream->conn->last_activity = quic_current_time_ms();
    }

    return 0;
}

/**
 * @brief 从 QUIC 流读取数据
 */
ssize_t quic_stream_read(quic_stream_t *stream, uint8_t *buf, size_t len) {
    if (!stream || !buf) return -1;

    size_t to_read = stream->recv_buf_len < len ? stream->recv_buf_len : len;
    if (to_read == 0) return 0;

    memcpy(buf, stream->recv_buf, to_read);

    /* 消费已读取的数据 */
    if (to_read < stream->recv_buf_len) {
        memmove(stream->recv_buf, stream->recv_buf + to_read,
                stream->recv_buf_len - to_read);
    }
    stream->recv_buf_len -= to_read;

    return (ssize_t)to_read;
}

/**
 * @brief 设置 QUIC 流 FIN 标志
 */
void quic_stream_set_fin(quic_stream_t *stream) {
    if (!stream) return;
    stream->local_fin = true;
}

/* ===== HTTP/3 会话管理 ===== */

/**
 * @brief 全局 HTTP/3 初始化
 */
bool http3_init(void) {
    /* 重置全局状态 */
    g_connections = NULL;
    g_connection_count = 0;

    /* 初始化随机种子 */
    srand((unsigned int)time(NULL));

    return true;
}

/**
 * @brief 全局 HTTP/3 清理
 */
void http3_cleanup(void) {
    /* 销毁所有连接 */
    while (g_connections) {
        quic_connection_t *conn = g_connections;
        g_connections = conn->next;

        quic_stream_t *stream = conn->streams;
        while (stream) {
            quic_stream_t *next = stream->next;
            if (stream->recv_buf) free(stream->recv_buf);
            free(stream);
            stream = next;
        }
        if (conn->tls_conn) {
#if defined(OPENSSL_HAS_QUIC) && defined(OPENSSL_VERSION_NUMBER)
            SSL_free((SSL *)conn->tls_conn);
#else
            free(conn->tls_conn);
#endif
        }
        free(conn);
    }
    g_connection_count = 0;
}

/**
 * @brief 创建 HTTP/3 会话
 */
http3_session_t *http3_session_create(quic_connection_t *conn) {
    if (!conn) return NULL;

    http3_session_t *session = (http3_session_t *)calloc(1, sizeof(http3_session_t));
    if (!session) return NULL;

    session->conn = conn;
    session->max_field_section_size = HTTP3_DEFAULT_MAX_FIELD_SECTION_SIZE;
    session->qpack_encoder_max_capacity = 0;  /* 不使用动态表 */
    session->qpack_decoder_max_capacity = 0;
    session->goaway_stream_id = UINT64_MAX;
    session->settings_received = false;
    session->settings_sent = false;

    /* 发送服务端 SETTINGS */
    http3_send_settings(session);

    return session;
}

/**
 * @brief 销毁 HTTP/3 会话
 */
void http3_session_destroy(http3_session_t *session) {
    if (!session) return;

    /* 释放所有 HTTP/3 流 */
    for (int i = 0; i < QUIC_MAX_STREAMS_PER_CONN; i++) {
        if (session->h3_streams[i]) {
            /* 不销毁底层 QUIC 流，只释放 HTTP/3 包装 */
            free(session->h3_streams[i]);
            session->h3_streams[i] = NULL;
        }
    }

    free(session);
}

/**
 * @brief 发送 SETTINGS 帧
 */
int http3_send_settings(http3_session_t *session) {
    if (!session || !session->conn) return -1;

    uint8_t payload[64];
    size_t pos = 0;

    /* 编码 SETTINGS 参数（键值对 varint） */
    pos += http3_encode_varint(HTTP3_SETTING_MAX_FIELD_SECTION_SIZE,
                                payload + pos);
    pos += http3_encode_varint(session->max_field_section_size,
                                payload + pos);
    pos += http3_encode_varint(HTTP3_SETTING_QPACK_MAX_TABLE_CAPACITY,
                                payload + pos);
    pos += http3_encode_varint(0, payload + pos);  /* 0 capacity = no dynamic table */

    /* 编码完整帧 */
    uint8_t frame[128];
    int frame_len = http3_encode_frame(HTTP3_FRAME_SETTINGS, payload, pos,
                                        frame, sizeof(frame));
    if (frame_len < 0) return -1;

    /* 通过流 ID 2（服务器控制流）发送 */
    /* 简化：直接发送数据报 */
    if (quic_send_datagram(session->conn, frame, (size_t)frame_len) != 0) {
        return -1;
    }

    session->settings_sent = true;
    return 0;
}

/**
 * @brief 处理 HTTP/3 控制流
 */
int http3_handle_control_stream(http3_session_t *session, quic_stream_t *stream,
                                const uint8_t *data, size_t len) {
    if (!session || !stream || !data || len == 0) return -1;

    /* 将数据追加到流的接收缓冲区 */
    if (quic_stream_write(stream, data, len) != 0) {
        return -1;
    }

    /* 尝试解析帧 */
    uint64_t frame_type = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    while (http3_parse_frame(stream, &frame_type, &payload, &payload_len) == 0) {
        switch (frame_type) {
            case HTTP3_FRAME_SETTINGS: {
                /* 解析 SETTINGS 帧 */
                size_t pos = 0;
                while (pos + 2 <= payload_len) {
                    uint64_t setting_id = 0, setting_value = 0;
                    int n1 = http3_decode_varint(payload + pos, payload_len - pos,
                                                  &setting_id);
                    if (n1 < 0) break;
                    pos += (size_t)n1;

                    int n2 = http3_decode_varint(payload + pos, payload_len - pos,
                                                  &setting_value);
                    if (n2 < 0) break;
                    pos += (size_t)n2;

                    if (setting_id == HTTP3_SETTING_MAX_FIELD_SECTION_SIZE) {
                        session->max_field_section_size = setting_value;
                    }
                }
                session->settings_received = true;
                break;
            }
            case HTTP3_FRAME_GOAWAY: {
                /* 解析 GOAWAY 帧 */
                uint64_t stream_id = 0;
                if (payload_len >= 1) {
                    http3_decode_varint(payload, payload_len, &stream_id);
                    session->goaway_stream_id = stream_id;
                    session->conn->closing = true;
                }
                break;
            }
            default:
                /* 忽略未知帧 */
                break;
        }
    }

    return 0;
}

/* ===== HTTP/3 请求/响应处理 ===== */

/**
 * @brief 查找或创建 HTTP/3 流
 */
static http3_stream_t *h3_stream_get_or_create(http3_session_t *session,
                                                uint64_t stream_id) {
    if (!session) return NULL;

    /* 查找流在数组中的索引 */
    size_t idx = (size_t)(stream_id / 4);
    if (idx >= QUIC_MAX_STREAMS_PER_CONN) return NULL;

    if (session->h3_streams[idx]) {
        return session->h3_streams[idx];
    }

    /* 创建新的 HTTP/3 流 */
    http3_stream_t *h3s = (http3_stream_t *)calloc(1, sizeof(http3_stream_t));
    if (!h3s) return NULL;

    /* 获取或创建底层 QUIC 流 */
    h3s->qstream = quic_stream_get_or_create(session->conn, stream_id);
    if (!h3s->qstream) {
        free(h3s);
        return NULL;
    }

    h3s->headers_received = false;
    h3s->data_received = false;
    h3s->headers_sent = false;
    h3s->trailers_sent = false;
    h3s->error_code = HTTP3_NO_ERROR;
    h3s->request_complete = false;
    h3s->response_complete = false;

    session->h3_streams[idx] = h3s;
    return h3s;
}

/**
 * @brief 从 HTTP/3 流读取请求
 */
int64_t http3_read_request(http3_session_t *session, http_request_t *req) {
    if (!session || !req || !session->conn) return -1;

    /* 遍历所有 HTTP/3 流，寻找有完整请求的 */
    for (int i = 0; i < QUIC_MAX_STREAMS_PER_CONN; i++) {
        http3_stream_t *h3s = session->h3_streams[i];
        if (!h3s || !h3s->qstream) continue;

        quic_stream_t *qstream = h3s->qstream;
        if (qstream->recv_buf_len == 0) continue;

        /* 尝试解析帧 */
        uint64_t frame_type = 0;
        const uint8_t *payload = NULL;
        size_t payload_len = 0;

        /* 保存缓冲区状态，以便回滚 */
        size_t saved_len = qstream->recv_buf_len;
        uint8_t *saved_buf = NULL;
        if (saved_len > 0) {
            saved_buf = (uint8_t *)malloc(saved_len);
            if (saved_buf) memcpy(saved_buf, qstream->recv_buf, saved_len);
        }

        int parse_result = http3_parse_frame(qstream, &frame_type, &payload, &payload_len);

        if (parse_result != 0 || frame_type != HTTP3_FRAME_HEADERS) {
            /* 恢复缓冲区 */
            if (saved_buf) {
                if (qstream->recv_buf_cap < saved_len) {
                    uint8_t *new_buf = (uint8_t *)realloc(qstream->recv_buf, saved_len);
                    if (new_buf) {
                        qstream->recv_buf = new_buf;
                        qstream->recv_buf_cap = saved_len;
                    }
                }
                if (qstream->recv_buf) {
                    memcpy(qstream->recv_buf, saved_buf, saved_len);
                    qstream->recv_buf_len = saved_len;
                }
                free(saved_buf);
            }
            continue;
        }

        free(saved_buf);

        /* 解码 HEADERS */
        if (qpack_decode_request_headers(payload, payload_len, req) != 0) {
            continue;
        }

        h3s->headers_received = true;

        /* 检查是否有 DATA 帧跟随 */
        if (qstream->recv_buf_len > 0) {
            size_t saved_len2 = qstream->recv_buf_len;
            uint8_t *saved_buf2 = (uint8_t *)malloc(saved_len2);
            if (saved_buf2) memcpy(saved_buf2, qstream->recv_buf, saved_len2);

            uint64_t data_frame_type = 0;
            const uint8_t *data_payload = NULL;
            size_t data_payload_len = 0;

            if (http3_parse_frame(qstream, &data_frame_type,
                                  &data_payload, &data_payload_len) == 0 &&
                data_frame_type == HTTP3_FRAME_DATA) {
                /* 有 DATA 帧 */
                if (data_payload_len > 0) {
                    req->body = (char *)malloc(data_payload_len + 1);
                    if (req->body) {
                        memcpy(req->body, data_payload, data_payload_len);
                        req->body[data_payload_len] = '\0';
                        req->body_len = data_payload_len;
                    }
                }
                h3s->data_received = true;
            } else {
                /* 恢复缓冲区 */
                if (saved_buf2) {
                    if (qstream->recv_buf_cap < saved_len2) {
                        uint8_t *nb = realloc(qstream->recv_buf, saved_len2);
                        if (nb) {
                            qstream->recv_buf = nb;
                            qstream->recv_buf_cap = saved_len2;
                        }
                    }
                    if (qstream->recv_buf) {
                        memcpy(qstream->recv_buf, saved_buf2, saved_len2);
                        qstream->recv_buf_len = saved_len2;
                    }
                }
            }
            free(saved_buf2);
        }

        /* 如果 HEADERS 帧已收到，检查 FIN */
        if (qstream->peer_fin || !h3s->data_received) {
            h3s->request_complete = true;
        }

        return (int64_t)qstream->stream_id;
    }

    return -1; /* 没有完整请求 */
}

/**
 * @brief 发送 HTTP/3 响应
 */
int http3_send_response(http3_session_t *session, uint64_t stream_id,
                        const http_response_t *resp,
                        const uint8_t *body, size_t body_len) {
    if (!session || !session->conn) return -1;

    http3_stream_t *h3s = h3_stream_get_or_create(session, stream_id);
    if (!h3s) return -1;

    /* 编码响应头 */
    uint8_t headers_buf[4096];
    int headers_len = qpack_encode_response_headers(resp, headers_buf,
                                                      sizeof(headers_buf));
    if (headers_len < 0) return -1;

    /* 编码 HEADERS 帧 */
    uint8_t headers_frame[8192];
    int headers_frame_len = http3_encode_frame(HTTP3_FRAME_HEADERS,
                                                headers_buf, (size_t)headers_len,
                                                headers_frame, sizeof(headers_frame));
    if (headers_frame_len < 0) return -1;

    /* 发送 HEADERS 帧 */
    if (quic_send_datagram(session->conn, headers_frame,
                           (size_t)headers_frame_len) != 0) {
        return -1;
    }

    h3s->headers_sent = true;

    /* 发送 DATA 帧（如果有 body） */
    if (body && body_len > 0) {
        uint8_t data_frame_buf[16384];
        int data_frame_len = http3_encode_frame(HTTP3_FRAME_DATA,
                                                  body, body_len,
                                                  data_frame_buf,
                                                  sizeof(data_frame_buf));
        if (data_frame_len < 0) return -1;

        if (quic_send_datagram(session->conn, data_frame_buf,
                               (size_t)data_frame_len) != 0) {
            return -1;
        }
    }

    h3s->response_complete = true;
    return 0;
}

/**
 * @brief 发送 HTTP/3 错误响应
 */
void http3_send_error(http3_session_t *session, uint64_t stream_id,
                      int status_code, const char *message) {
    if (!session) return;

    http_response_t resp = {0};
    resp.status_code = status_code;
    resp.content_type = "text/html; charset=utf-8";

    /* 构造简单错误页面 */
    char body[1024];
    int body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><title>%d Error</title></head>"
        "<body><h1>%d %s</h1><p>%s</p></body></html>",
        status_code, status_code,
        status_code == 404 ? "Not Found" :
        status_code == 500 ? "Internal Server Error" :
        status_code == 400 ? "Bad Request" :
        status_code == 405 ? "Method Not Allowed" : "Error",
        message ? message : "");

    resp.content_length = body_len;

    http3_send_response(session, stream_id, &resp,
                        (const uint8_t *)body, (size_t)body_len);
}

/**
 * @brief 关闭 QUIC 连接
 */
void http3_close_connection(quic_connection_t *conn, http3_error_t error) {
    if (!conn || conn->closed) return;

    (void)error; /* 错误码可用于发送 CONNECTION_CLOSE 帧 */

    conn->closed = true;
    conn->closing = true;

    /* 发送 GOAWAY 帧 */
    if (conn->udp_fd != COCOON_INVALID_SOCKET) {
        uint8_t goaway_payload[16];
        size_t pos = 0;
        pos += http3_encode_varint(UINT64_MAX, goaway_payload + pos);

        uint8_t frame[32];
        int frame_len = http3_encode_frame(HTTP3_FRAME_GOAWAY,
                                            goaway_payload, pos,
                                            frame, sizeof(frame));
        if (frame_len > 0) {
            quic_send_datagram(conn, frame, (size_t)frame_len);
        }
    }
}

/* ===== UDP 服务器循环 ===== */

/**
 * @brief 处理 UDP 数据报
 *
 * 简化版处理：不实现完整 QUIC 数据报解析，
 * 而是将数据直接分发给对应连接的流。
 */
void http3_process_datagram(cocoon_socket_t udp_fd,
                            const uint8_t *buf, size_t len,
                            const struct sockaddr_storage *peer_addr) {
    if (!buf || len == 0 || !peer_addr) return;

    /* 简化处理：假设数据报前 8 字节是连接 ID */
    uint64_t conn_id = 0;
    if (len >= 8) {
        conn_id = ((uint64_t)buf[0] << 56) |
                  ((uint64_t)buf[1] << 48) |
                  ((uint64_t)buf[2] << 40) |
                  ((uint64_t)buf[3] << 32) |
                  ((uint64_t)buf[4] << 24) |
                  ((uint64_t)buf[5] << 16) |
                  ((uint64_t)buf[6] << 8) |
                  (uint64_t)buf[7];
    }

    /* 查找或创建连接 */
    quic_connection_t *conn = quic_find_connection(conn_id);
    if (!conn) {
        conn = quic_connection_create(conn_id, udp_fd, peer_addr);
        if (!conn) return;
    }

    /* 更新活动时间 */
    conn->last_activity = quic_current_time_ms();
    conn->bytes_received += len;

    /* 简化：假设剩余数据包含 [stream_id:8][data...]
     * 在实际完整实现中，这里需要解析 QUIC 数据包头、包号、帧等
     */
    if (len > 16) {
        uint64_t stream_id = ((uint64_t)buf[8] << 56) |
                             ((uint64_t)buf[9] << 48) |
                             ((uint64_t)buf[10] << 40) |
                             ((uint64_t)buf[11] << 32) |
                             ((uint64_t)buf[12] << 24) |
                             ((uint64_t)buf[13] << 16) |
                             ((uint64_t)buf[14] << 8) |
                             (uint64_t)buf[15];

        quic_stream_t *stream = quic_stream_get_or_create(conn, stream_id);
        if (stream) {
            quic_stream_write(stream, buf + 16, len - 16);
        }
    }
}

/**
 * @brief 接受新的 QUIC 连接
 *
 * 从 UDP socket 接收数据报并处理。
 */
quic_connection_t *http3_accept(cocoon_socket_t udp_fd) {
    if (udp_fd == COCOON_INVALID_SOCKET) return NULL;

    uint8_t buf[QUIC_MAX_DATAGRAM_SIZE];
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);

    ssize_t n = recvfrom(udp_fd, (char *)buf, sizeof(buf), 0,
                         (struct sockaddr *)&peer_addr, &peer_addr_len);
    if (n <= 0) return NULL;

    http3_process_datagram(udp_fd, buf, (size_t)n, &peer_addr);

    /* 返回新创建或找到的连接 */
    if (n >= 8) {
        uint64_t conn_id = ((uint64_t)buf[0] << 56) |
                           ((uint64_t)buf[1] << 48) |
                           ((uint64_t)buf[2] << 40) |
                           ((uint64_t)buf[3] << 32) |
                           ((uint64_t)buf[4] << 24) |
                           ((uint64_t)buf[5] << 16) |
                           ((uint64_t)buf[6] << 8) |
                           (uint64_t)buf[7];
        return quic_find_connection(conn_id);
    }

    return NULL;
}

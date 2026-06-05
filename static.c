
/**
 * static.c - 闈欐€佽祫婧愭湇鍔″疄鐜?
 *
 * 鎻愪緵鏂囦欢鏈嶅姟銆佺洰褰曞垪琛ㄣ€侀敊璇搷搴斿姛鑳姐€?
 * 鍒╃敤 coco 鐨?I/O API 瀹炵幇闈為樆濉炴枃浠朵紶杈撱€?
 *
 * @author xfy
 */

#include "static.h"
#include "cocoon.h"
#include "platform.h"
#include "../coco/include/coco.h"
#include "tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <zlib.h>
#include <brotli/encode.h>

/**
 * is_compressible_mime - 鍒ゆ柇 MIME 绫诲瀷鏄惁閫傚悎鍘嬬缉
 *
 * 鏂囨湰绫诲瀷閫氬父鏈夊緢楂樼殑鍘嬬缉鐜囷紝浜岃繘鍒剁被鍨嬶紙鍥剧墖銆佽棰戙€侀煶棰戯級
 * 鏈韩宸茬粡鍘嬬缉杩囷紝鍐嶅帇缂╂氮璐规椂闂翠笖鏁堟灉宸€?
 *
 * @param mime_type MIME 绫诲瀷瀛楃涓?
 * @return true 閫傚悎鍘嬬缉
 */
static bool is_compressible_mime(const char *mime_type) {
    if (!mime_type) return false;
    return (
        strstr(mime_type, "text/") != NULL ||
        strstr(mime_type, "application/javascript") != NULL ||
        strstr(mime_type, "application/json") != NULL ||
        strstr(mime_type, "application/xml") != NULL ||
        strstr(mime_type, "application/manifest") != NULL ||
        strstr(mime_type, "image/svg") != NULL
    );
}

/**
 * gzip_compress - 浣跨敤 zlib 鍘嬬缉鏁版嵁涓?gzip 鏍煎紡
 *
 * 浣跨敤 deflateInit2 鐨?windowBits = 15 + 16 鏉ョ敓鎴愭爣鍑?gzip 澶淬€?
 *
 * @param src 鍘熷鏁版嵁
 * @param src_len 鍘熷鏁版嵁闀垮害
 * @param dst 杈撳嚭缂撳啿鍖猴紙璋冪敤鑰呭垎閰嶏紝寤鸿澶у皬涓?src_len锛?
 * @param dst_cap 杈撳嚭缂撳啿鍖哄閲?
 * @return 鍘嬬缉鍚庨暱搴︼紝0 琛ㄧず涓嶉渶瑕佸帇缂╋紙鍘嬬缉鍚庢洿澶э級锛?1 琛ㄧず閿欒
 */
static ssize_t gzip_compress(const char *src, size_t src_len,
                             char *dst, size_t dst_cap) {
    z_stream strm = {0};
    /* 15 + 16 = gzip 鏍煎紡 */
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return -1;
    }

    strm.avail_in = (uInt)src_len;
    strm.next_in = (Bytef *)src;
    strm.avail_out = (uInt)dst_cap;
    strm.next_out = (Bytef *)dst;

    if (deflate(&strm, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&strm);
        return -1;
    }

    size_t compressed_len = dst_cap - strm.avail_out;
    deflateEnd(&strm);

    /* 濡傛灉鍘嬬缉鍚庢洿澶ф垨宸笉澶氾紝灏变笉鍘嬬缉浜?*/
    if (compressed_len >= src_len * 0.95) {
        return 0;
    }
    return (ssize_t)compressed_len;
}

/**
 * brotli_compress - 浣跨敤 Brotli 鍘嬬缉鏁版嵁
 *
 * 浣跨敤 Brotli 缂栫爜鍣ㄨ繘琛岄珮璐ㄩ噺鍘嬬缉銆?
 *
 * @param src 鍘熷鏁版嵁
 * @param src_len 鍘熷鏁版嵁闀垮害
 * @param dst 杈撳嚭缂撳啿鍖猴紙璋冪敤鑰呭垎閰嶏紝寤鸿澶у皬涓?src_len锛?
 * @param dst_cap 杈撳嚭缂撳啿鍖哄閲?
 * @return 鍘嬬缉鍚庨暱搴︼紝0 琛ㄧず涓嶉渶瑕佸帇缂╋紙鍘嬬缉鍚庢洿澶э級锛?1 琛ㄧず閿欒
 */
static ssize_t brotli_compress(const char *src, size_t src_len,
                                 char *dst, size_t dst_cap) {
    size_t encoded_size = dst_cap;
    BROTLI_BOOL ok = BrotliEncoderCompress(
        BROTLI_DEFAULT_QUALITY,      /* 榛樿璐ㄩ噺 11 */
        BROTLI_DEFAULT_WINDOW,       /* 榛樿绐楀彛澶у皬 22 */
        BROTLI_MODE_GENERIC,         /* 閫氱敤妯″紡 */
        src_len,
        (const uint8_t *)src,
        &encoded_size,
        (uint8_t *)dst
    );
    if (!ok) return -1;

    /* 濡傛灉鍘嬬缉鍚庢洿澶ф垨宸笉澶氾紝灏变笉鍘嬬缉浜?*/
    if (encoded_size >= src_len * 0.95) {
        return 0;
    }
    return (ssize_t)encoded_size;
}

/**
 * format_http_time - 灏嗘椂闂存埑鏍煎紡鍖栦负 HTTP 鏃ユ湡瀛楃涓?
 *
 * HTTP 鏃ユ湡鏍煎紡: "Wed, 21 Oct 2015 07:28:00 GMT"
 *
 * @param t 鏃堕棿鎴筹紙绉掞級
 * @param buf 杈撳嚭缂撳啿鍖?
 * @param buf_size 缂撳啿鍖哄ぇ灏?
 */
static void format_http_time(time_t t, char *buf, size_t buf_size) {
    struct tm *gmt = gmtime(&t);
    if (gmt) {
        strftime(buf, buf_size, "%a, %d %b %Y %H:%M:%S GMT", gmt);
    } else {
        buf[0] = '\0';
    }
}

/**
 * generate_etag - 鍩轰簬鏂囦欢鍏冩暟鎹敓鎴?ETag
 *
 * 鏍煎紡: "澶у皬-淇敼鏃堕棿鍗佸叚杩涘埗"
 * 绀轰緥: "1024-647a3b2f"
 *
 * @param st 鏂囦欢鐘舵€佺粨鏋勪綋
 * @param buf 杈撳嚭缂撳啿鍖?
 * @param buf_size 缂撳啿鍖哄ぇ灏?
 */
static void generate_etag(const cocoon_stat_t *st, char *buf, size_t buf_size) {
    snprintf(buf, buf_size, "\"%lx-%lx\"", (unsigned long)st->st_size, (unsigned long)st->st_mtime);
}

/**
 * match_etag - 姣旇緝 ETag 鍊兼槸鍚﹀尮閰?
 *
 * 鏀寔 W/ 寮卞尮閰嶅墠缂€鍜?* 閫氶厤绗︺€?
 *
 * @param etag 鏈嶅姟鍣?ETag
 * @param if_none_match 瀹㈡埛绔?If-None-Match 鍊?
 * @return true 鍖归厤
 */
static bool match_etag(const char *etag, const char *if_none_match) {
    if (!etag || !if_none_match) return false;
    /* 閫氶厤绗﹀尮閰?*/
    if (strcmp(if_none_match, "*") == 0) return true;
    /* 鍘婚櫎 W/ 鍓嶇紑姣旇緝 */
    const char *client = if_none_match;
    if (strncmp(client, "W/", 2) == 0) client += 2;
    return strcmp(client, etag) == 0;
}

/**
 * parse_http_time - 瑙ｆ瀽 HTTP 鏃ユ湡瀛楃涓蹭负鏃堕棿鎴?
 *
 * 鏀寔 RFC 1123 / RFC 850 / ANSI C 鏍煎紡銆?
 *
 * @param str HTTP 鏃ユ湡瀛楃涓?
 * @return 鏃堕棿鎴筹紝瑙ｆ瀽澶辫触杩斿洖 -1
 */
static time_t parse_http_time(const char *str) {
    struct tm tm = {0};
    if (strptime(str, "%a, %d %b %Y %H:%M:%S GMT", &tm) != NULL ||
        strptime(str, "%A, %d-%b-%y %H:%M:%S GMT", &tm) != NULL ||
        strptime(str, "%a %b %d %H:%M:%S %Y", &tm) != NULL) {
        return timegm(&tm);
    }
    return -1;
}

/**
 * safe_path_join - 瀹夊叏璺緞鎷兼帴
 *
 * 闃叉璺緞閬嶅巻鏀诲嚮锛岀姝㈣秴鍑烘牴鐩綍鐨勮闂€?
 *
 * @param dst 杈撳嚭缂撳啿鍖?
 * @param dst_size 缂撳啿鍖哄ぇ灏?
 * @param root 鏍圭洰褰?
 * @param path 璇锋眰璺緞
 * @return true 璺緞瀹夊叏锛宖alse 瀛樺湪璺緞閬嶅巻椋庨櫓
 */
static bool safe_path_join(char *dst, size_t dst_size,
                           const char *root, const char *path) {
    if (!dst || !root || !path || dst_size == 0) return false;

    /* 鍏堣鑼冨寲鏍圭洰褰?*/
    char root_normalized[4096];
    if (!cocoon_realpath(root, root_normalized, sizeof(root_normalized))) {
        snprintf(root_normalized, sizeof(root_normalized), "%s", root);
    }
    size_t root_len = strlen(root_normalized);

    /* 鎷兼帴璺緞 */
    int n = snprintf(dst, dst_size, "%s%s", root_normalized, path);
    if (n < 0 || (size_t)n >= dst_size) return false;

    /* 妫€鏌ヨ矾寰勯亶鍘?*/
    if (strstr(path, "..") != NULL) {
        /* 浣跨敤 realpath 杩涗竴姝ラ獙璇?*/
        char resolved[4096];
        if (realpath(dst, resolved)) {
            if (strncmp(resolved, root_normalized, root_len) != 0) {
                return false;
            }
            snprintf(dst, dst_size, "%s", resolved);
            return true;
        }
        return false;
    }

    return true;
}

/**
 * send_all - 纭繚缂撳啿鍖哄叏閮ㄥ彂閫?
 *
 * 浣跨敤 write 寰幆鍙戦€侊紝鐩村埌鍏ㄩ儴鏁版嵁鍙戦€佸畬姣曟垨閬囧埌涓嶅彲鎭㈠閿欒銆?
 *
 * @param fd socket 鏂囦欢鎻忚堪绗?
 * @param buf 鏁版嵁缂撳啿鍖?
 * @param len 鏁版嵁闀垮害
 * @return 0 鎴愬姛锛?1 澶辫触
 */
int send_all(int fd, const char *buf, size_t len) {
    if (tls_has_connection(fd)) {
        return tls_write(fd, buf, len) == (ssize_t)len ? 0 : -1;
    }

    size_t sent = 0;
    while (sent < len) {
        ssize_t n;
        if (tls_has_connection(fd)) {
            n = tls_write(fd, buf + sent, len - sent);
        } else if (coco_sched_get_current() != NULL) {
            int ret = coco_write(fd, buf + sent, len - sent);
            if (ret < 0) {
                if (ret == COCO_ERROR_WOULD_BLOCK) {
                    continue;
                }
                return -1;
            }
            n = ret;
        } else {
            n = cocoon_socket_send(fd, buf + sent, len - sent);
        }
        if (n < 0) {
            int err = cocoon_get_last_error();
            if (err == EAGAIN || err == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/**
 * static_send_error - 鍙戦€?HTTP 閿欒鍝嶅簲
 *
 * 鐢熸垚绠€娲佺殑閿欒椤甸潰锛屽寘鍚姸鎬佺爜鍜岀姸鎬佹枃鏈€?
 *
 * @param fd 瀹㈡埛绔?socket
 * @param status_code HTTP 鐘舵€佺爜
 * @param keep_alive 鏄惁淇濇寔杩炴帴
 * @return COCOON_OK 鎴愬姛
 */
int static_send_error(int fd, int status_code, bool keep_alive) {
    const char *status_text = "Unknown Error";
    switch (status_code) {
        case 400: status_text = "Bad Request"; break;
        case 403: status_text = "Forbidden"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 416: status_text = "Range Not Satisfiable"; break;
        case 500: status_text = "Internal Server Error"; break;
    }

    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html>\n"
        "<html><head><title>%d %s</title></head>\n"
        "<body><h1>%d %s</h1>\n"
        "<p>Cocoon Server</p></body></html>\n",
        status_code, status_text, status_code, status_text);

    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "Server: Cocoon/1.0\r\n"
        "\r\n",
        status_code, status_text, body_len,
        keep_alive ? "keep-alive" : "close");

    send_all(fd, header, (size_t)header_len);
    send_all(fd, body, (size_t)body_len);
    return COCOON_OK;
}

/**
 * static_serve_file - 鏈嶅姟鍗曚釜闈欐€佹枃浠?
 *
 * 鎵撳紑鏂囦欢锛岃绠楀唴瀹归暱搴︼紝澶勭悊 Range 璇锋眰锛?
 * 浼樺厛浣跨敤 sendfile 闆舵嫹璐濆彂閫侊紝鍥為€€鍒?read/write 寰幆銆?
 *
 * @param fd 瀹㈡埛绔?socket
 * @param req HTTP 璇锋眰
 * @param root_dir 闈欐€佽祫婧愭牴鐩綍
 * @return COCOON_OK 鎴愬姛锛岃礋鍊奸敊璇爜
 */
int static_serve_file(int fd, const http_request_t *req, const char *root_dir, bool gzip_enabled, bool brotli_enabled) {
    char real_path[4096];
    if (!safe_path_join(real_path, sizeof(real_path), root_dir, req->path)) {
        return static_send_error(fd, 403, req->keep_alive);
    }

    /* 妫€鏌ユ枃浠舵槸鍚﹀瓨鍦ㄤ笖鍙 */
    cocoon_stat_t st;
    if (cocoon_file_stat(real_path, &st) != 0) {
        return static_send_error(fd, 404, req->keep_alive);
    }
    if (!cocoon_stat_isreg(&st)) {
        return static_send_error(fd, 403, req->keep_alive);
    }

    /* 鎵撳紑鏂囦欢 */
    cocoon_file_t file_fd = cocoon_file_open(real_path);
    if (file_fd < 0) {
        return static_send_error(fd, 403, req->keep_alive);
    }

    /* 鐢熸垚 ETag 鍜?Last-Modified */
    char etag[64];
    char last_modified[64];
    generate_etag(&st, etag, sizeof(etag));
    format_http_time(cocoon_stat_mtime(&st), last_modified, sizeof(last_modified));

    /* 妫€鏌ョ紦瀛樺崗鍟?*/
    if (req->has_if_none_match && match_etag(etag, req->if_none_match)) {
        cocoon_file_close(file_fd);
        http_response_t resp = {
            .status_code = 304,
            .status_text = "Not Modified",
            .content_type = http_mime_type(real_path),
            .content_length = 0,
            .keep_alive = req->keep_alive,
            .etag = etag,
            .last_modified = last_modified
        };
        char header_buf[1024];
        int header_len = http_format_response_header(header_buf, sizeof(header_buf), &resp);
        if (header_len > 0) send_all(fd, header_buf, (size_t)header_len);
        return COCOON_OK;
    }
    if (req->has_if_modified_since) {
        time_t client_time = parse_http_time(req->if_modified_since);
        if (client_time >= 0 && cocoon_stat_mtime(&st) <= client_time) {
            cocoon_file_close(file_fd);
            http_response_t resp = {
                .status_code = 304,
                .status_text = "Not Modified",
                .content_type = http_mime_type(real_path),
                .content_length = 0,
                .keep_alive = req->keep_alive,
                .etag = etag,
                .last_modified = last_modified
            };
            char header_buf[1024];
            int header_len = http_format_response_header(header_buf, sizeof(header_buf), &resp);
            if (header_len > 0) send_all(fd, header_buf, (size_t)header_len);
            return COCOON_OK;
        }
    }

    /* 鍒ゆ柇鍘嬬缉鏂瑰紡锛氫紭鍏?brotli锛屽洖閫€ gzip */
    int64_t file_size = cocoon_stat_size(&st);
    bool use_gzip = false;
    bool use_brotli = false;
    char *compress_buf = NULL;
    ssize_t compress_len = 0;

    if (!req->has_range && req->method != HTTP_HEAD) {
        const char *mime = http_mime_type(real_path);
        if (is_compressible_mime(mime) && file_size > 256) {
            /* 璇诲彇鏂囦欢鍐呭鍒板唴瀛?*/
            char *file_buf = (char *)malloc((size_t)file_size);
            if (file_buf) {
                ssize_t read_total = 0;
                while (read_total < file_size) {
                    ssize_t n = cocoon_file_read(file_fd, file_buf + read_total, (size_t)(file_size - read_total));
                    if (n <= 0) break;
                    read_total += n;
                }
                if (read_total == file_size) {
                    compress_buf = (char *)malloc((size_t)file_size);
                    if (compress_buf) {
                        /* 浼樺厛 brotli */
                        if (brotli_enabled && req->accept_brotli) {
                            compress_len = brotli_compress(file_buf, (size_t)file_size, compress_buf, (size_t)file_size);
                            if (compress_len > 0) use_brotli = true;
                        }
                        /* 鍥為€€ gzip */
                        if (!use_brotli && gzip_enabled && req->accept_gzip) {
                            compress_len = gzip_compress(file_buf, (size_t)file_size, compress_buf, (size_t)file_size);
                            if (compress_len > 0) use_gzip = true;
                        }
                    }
                }
                free(file_buf);
            }
        }
    }

    /* 璁＄畻鍙戦€佽寖鍥?*/
    int64_t send_start = 0;
    int64_t send_end = file_size - 1;
    int status_code = 200;

    if (!use_gzip && !use_brotli && req->has_range) {
        send_start = req->range_start;
        if (req->range_end >= 0 && req->range_end < file_size) {
            send_end = req->range_end;
        }
        if (send_start >= file_size || send_start > send_end) {
            cocoon_file_close(file_fd);
            free(compress_buf);
            return static_send_error(fd, 416, req->keep_alive);
        }
        status_code = 206;
    }

    int64_t send_length = (use_gzip || use_brotli) ? compress_len : (send_end - send_start + 1);

    /* 鏋勫缓鍝嶅簲澶?*/
    http_response_t resp = {
        .status_code = status_code,
        .status_text = status_code == 206 ? "Partial Content" : "OK",
        .content_type = http_mime_type(real_path),
        .content_length = send_length,
        .keep_alive = req->keep_alive,
        .has_range = !use_gzip && !use_brotli && req->has_range,
        .range_start = send_start,
        .range_end = send_end,
        .total_length = file_size,
        .etag = etag,
        .last_modified = last_modified,
        .content_encoding = use_brotli ? "br" : (use_gzip ? "gzip" : NULL)
    };

    char header_buf[1024];
    int header_len = http_format_response_header(header_buf, sizeof(header_buf), &resp);
    if (header_len < 0) {
        cocoon_file_close(file_fd);
        free(compress_buf);
        return static_send_error(fd, 500, req->keep_alive);
    }

    /* 鍙戦€佸搷搴斿ご */
    if (send_all(fd, header_buf, (size_t)header_len) != 0) {
        cocoon_file_close(file_fd);
        free(compress_buf);
        return COCOON_ERROR;
    }

    /* 鍙戦€佹枃浠跺唴瀹?*/
    if (req->method == HTTP_HEAD) {
        /* HEAD 璇锋眰涓嶅彂閫?body */
        cocoon_file_close(file_fd);
        free(compress_buf);
        return COCOON_OK;
    }

    if (use_gzip || use_brotli || tls_has_connection(fd)) {
        /* 发送压缩后的数据，或 TLS 模式下的文件内容 */
        if (use_gzip || use_brotli) {
            send_all(fd, compress_buf, (size_t)compress_len);
            free(compress_buf);
        } else {
            /* 定位到起始位置（文件可能已被压缩读取提前读至末尾） */
            cocoon_file_seek(file_fd, send_start, SEEK_SET);
            /* TLS 模式：不能使用 sendfile，需读取文件后发送 */
            char file_buf[65536];
            ssize_t remaining = send_length;
            while (remaining > 0) {
                size_t to_read = (size_t)remaining < sizeof(file_buf) ? (size_t)remaining : sizeof(file_buf);
                ssize_t n = cocoon_file_read(file_fd, file_buf, to_read);
                if (n <= 0) break;
                if (send_all(fd, file_buf, (size_t)n) != 0) break;
                remaining -= n;
            }
        }
        cocoon_file_close(file_fd);
    } else {
        /* 浣跨敤璺ㄥ钩鍙版枃浠跺彂閫侊紙Linux sendfile / Windows read+send锛?*/
        ssize_t sent = cocoon_file_send(fd, file_fd, send_start, (size_t)send_length);
        cocoon_file_close(file_fd);
        if (sent < 0) {
            return COCOON_ERROR;
        }
    }
    return COCOON_OK;
}

/**
 * html_escape - HTML 鐗规畩瀛楃杞箟
 *
 * 灏?&, <, >, " 杞箟涓哄搴旂殑 HTML 瀹炰綋锛岄槻姝?XSS銆?
 *
 * @param src 鍘熷瀛楃涓?
 * @param dst 杈撳嚭缂撳啿鍖?
 * @param dst_size 缂撳啿鍖哄ぇ灏?
 */
static void html_escape(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
        switch (src[i]) {
            case '&':
                if (j + 5 < dst_size) {
                    memcpy(dst + j, "&amp;", 5);
                    j += 5;
                }
                break;
            case '<':
                if (j + 4 < dst_size) {
                    memcpy(dst + j, "&lt;", 4);
                    j += 4;
                }
                break;
            case '>':
                if (j + 4 < dst_size) {
                    memcpy(dst + j, "&gt;", 4);
                    j += 4;
                }
                break;
            case '"':
                if (j + 6 < dst_size) {
                    memcpy(dst + j, "&quot;", 6);
                    j += 6;
                }
                break;
            default:
                dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

/**
 * static_serve_directory - 鐢熸垚鐩綍娴忚椤甸潰
 *
 * 璇诲彇鐩綍椤癸紝鐢熸垚缇庤鐨?HTML 鐩綍鍒楄〃锛屾敮鎸佹帓搴忋€?
 *
 * @param fd 瀹㈡埛绔?socket
 * @param req HTTP 璇锋眰
 * @param root_dir 闈欐€佽祫婧愭牴鐩綍
 * @param real_path 鏂囦欢绯荤粺涓婄殑鐪熷疄璺緞
 * @return COCOON_OK 鎴愬姛锛岃礋鍊奸敊璇爜
 */
int static_serve_directory(int fd, const http_request_t *req,
                           const char *root_dir, const char *real_path) {
    (void)root_dir;    /* 鏈洿鎺ヤ娇鐢紝real_path 宸查€氳繃 safe_path_join 澶勭悊 */
    
    /* 妫€鏌ョ洰褰曟槸鍚﹀彲璁块棶 */
    cocoon_stat_t st;
    if (cocoon_file_stat(real_path, &st) != 0 || !cocoon_stat_isdir(&st)) {
        return static_send_error(fd, 404, req->keep_alive);
    }

    cocoon_dir_iter_t iter;
    if (cocoon_dir_open(&iter, real_path) != 0) {
        return static_send_error(fd, 403, req->keep_alive);
    }

    /* 鍏堟敹闆嗘墍鏈夌洰褰曢」 */
    char *entries[4096];
    int num_entries = 0;
    while (cocoon_dir_next(&iter) == 0 && num_entries < 4096) {
        if (iter.name[0] == '.') continue; /* 闅愯棌鏂囦欢 */
        entries[num_entries] = strdup(iter.name);
        num_entries++;
    }
    cocoon_dir_close(&iter);

    /* 鏋勫缓 HTML */
    char html[65536];
    int n = snprintf(html, sizeof(html),
        "<!DOCTYPE html>\n"
        "<html><head>\n"
        "<meta charset=\"utf-8\">\n"
        "<title>Index of %s</title>\n"
        "<style>"
        "body{font-family:system-ui,-apple-system,sans-serif;max-width:800px;margin:40px auto;padding:0 20px}"
        "h1{border-bottom:1px solid #ddd;padding-bottom:10px}"
        "table{width:100%%;border-collapse:collapse}"
        "th{text-align:left;padding:8px;border-bottom:2px solid #ddd}"
        "td{padding:8px;border-bottom:1px solid #eee}"
        "a{text-decoration:none;color:#0066cc}"
        "a:hover{text-decoration:underline}"
        "</style>\n"
        "</head><body>\n"
        "<h1>Index of %s</h1>\n"
        "<table>\n"
        "<tr><th>Name</th><th>Size</th><th>Modified</th></tr>\n",
        req->path, req->path);

    /* 娣诲姞杩斿洖涓婄骇閾炬帴 */
    if (strcmp(req->path, "/") != 0) {
        n += snprintf(html + n, sizeof(html) - n,
            "<tr><td><a href=\"../\">../</a></td><td>-</td><td>-</td></tr>\n");
    }

    /* 娣诲姞鐩綍椤?*/
    for (int i = 0; i < num_entries; i++) {
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", real_path, entries[i]);

        cocoon_stat_t entry_st;
        char size_str[32] = "-";
        char mtime_str[32] = "-";
        bool entry_is_dir = false;

        if (cocoon_file_stat(full_path, &entry_st) == 0) {
            entry_is_dir = cocoon_stat_isdir(&entry_st);
            /* 鏍煎紡鍖栨枃浠跺ぇ灏?*/
            if (entry_is_dir) {
                strncpy(size_str, "-", sizeof(size_str));
            } else {
                int64_t sz = cocoon_stat_size(&entry_st);
                if (sz < 1024) {
                    snprintf(size_str, sizeof(size_str), "%lld B", (long long)sz);
                } else if (sz < 1024 * 1024) {
                    snprintf(size_str, sizeof(size_str), "%.1f KB", sz / 1024.0);
                } else if (sz < 1024LL * 1024 * 1024) {
                    snprintf(size_str, sizeof(size_str), "%.1f MB", sz / (1024.0 * 1024));
                } else {
                    snprintf(size_str, sizeof(size_str), "%.1f GB", sz / (1024.0 * 1024 * 1024));
                }
            }

            /* 鏍煎紡鍖栦慨鏀规椂闂?*/
            time_t mtime = cocoon_stat_mtime(&entry_st);
            struct tm *tm_info = localtime(&mtime);
            if (tm_info) {
                strftime(mtime_str, sizeof(mtime_str), "%Y-%m-%d %H:%M", tm_info);
            }
        }

        /* HTML 杞箟鏂囦欢鍚?*/
        char escaped_name[512];
        html_escape(entries[i], escaped_name, sizeof(escaped_name));

        n += snprintf(html + n, sizeof(html) - n,
            "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n",
            escaped_name,
            entry_is_dir ? "/" : "",
            escaped_name,
            entry_is_dir ? "/" : "",
            size_str, mtime_str);

        free(entries[i]);
    }

    n += snprintf(html + n, sizeof(html) - n,
        "</table>\n"
        "<hr>\n"
        "<p><em>Cocoon Server</em></p>\n"
        "</body></html>\n");

    /* 鍙戦€佸搷搴?*/
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "Server: Cocoon/1.0\r\n"
        "\r\n",
        n, req->keep_alive ? "keep-alive" : "close");

    send_all(fd, header, (size_t)header_len);
    send_all(fd, html, (size_t)n);
    return COCOON_OK;
}

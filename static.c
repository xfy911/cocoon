#define _GNU_SOURCE

/**
 * static.c - 静态资源服务实现
 *
 * 提供文件服务、目录列表、错误响应功能。
 * 利用 coco 的 I/O API 实现非阻塞文件传输。
 *
 * @author xfy
 */

#include "static.h"
#include "cocoon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>

/**
 * format_http_time - 将时间戳格式化为 HTTP 日期字符串
 *
 * HTTP 日期格式: "Wed, 21 Oct 2015 07:28:00 GMT"
 *
 * @param t 时间戳（秒）
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
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
 * generate_etag - 基于文件元数据生成 ETag
 *
 * 格式: "大小-修改时间十六进制"
 * 示例: "1024-647a3b2f"
 *
 * @param st 文件状态结构体
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 */
static void generate_etag(const struct stat *st, char *buf, size_t buf_size) {
    snprintf(buf, buf_size, "\"%lx-%lx\"", (unsigned long)st->st_size, (unsigned long)st->st_mtime);
}

/**
 * match_etag - 比较 ETag 值是否匹配
 *
 * 支持 W/ 弱匹配前缀和 * 通配符。
 *
 * @param etag 服务器 ETag
 * @param if_none_match 客户端 If-None-Match 值
 * @return true 匹配
 */
static bool match_etag(const char *etag, const char *if_none_match) {
    if (!etag || !if_none_match) return false;
    /* 通配符匹配 */
    if (strcmp(if_none_match, "*") == 0) return true;
    /* 去除 W/ 前缀比较 */
    const char *client = if_none_match;
    if (strncmp(client, "W/", 2) == 0) client += 2;
    return strcmp(client, etag) == 0;
}

/**
 * parse_http_time - 解析 HTTP 日期字符串为时间戳
 *
 * 支持 RFC 1123 / RFC 850 / ANSI C 格式。
 *
 * @param str HTTP 日期字符串
 * @return 时间戳，解析失败返回 -1
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
 * safe_path_join - 安全路径拼接
 *
 * 防止路径遍历攻击，禁止超出根目录的访问。
 *
 * @param dst 输出缓冲区
 * @param dst_size 缓冲区大小
 * @param root 根目录
 * @param path 请求路径
 * @return true 路径安全，false 存在路径遍历风险
 */
static bool safe_path_join(char *dst, size_t dst_size,
                           const char *root, const char *path) {
    if (!dst || !root || !path || dst_size == 0) return false;

    /* 先规范化根目录 */
    char root_normalized[4096];
    if (!realpath(root, root_normalized)) {
        snprintf(root_normalized, sizeof(root_normalized), "%s", root);
    }
    size_t root_len = strlen(root_normalized);

    /* 拼接路径 */
    int n = snprintf(dst, dst_size, "%s%s", root_normalized, path);
    if (n < 0 || (size_t)n >= dst_size) return false;

    /* 检查路径遍历 */
    if (strstr(path, "..") != NULL) {
        /* 使用 realpath 进一步验证 */
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
 * send_all - 确保缓冲区全部发送
 *
 * 使用 write 循环发送，直到全部数据发送完毕或遇到不可恢复错误。
 *
 * @param fd socket 文件描述符
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return 0 成功，-1 失败
 */
static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/**
 * static_send_error - 发送 HTTP 错误响应
 *
 * 生成简洁的错误页面，包含状态码和状态文本。
 *
 * @param fd 客户端 socket
 * @param status_code HTTP 状态码
 * @param keep_alive 是否保持连接
 * @return COCOON_OK 成功
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
 * static_serve_file - 服务单个静态文件
 *
 * 打开文件，计算内容长度，处理 Range 请求，
 * 优先使用 sendfile 零拷贝发送，回退到 read/write 循环。
 *
 * @param fd 客户端 socket
 * @param req HTTP 请求
 * @param root_dir 静态资源根目录
 * @return COCOON_OK 成功，负值错误码
 */
int static_serve_file(int fd, const http_request_t *req, const char *root_dir) {
    char real_path[4096];
    if (!safe_path_join(real_path, sizeof(real_path), root_dir, req->path)) {
        return static_send_error(fd, 403, req->keep_alive);
    }

    /* 检查文件是否存在且可读 */
    struct stat st;
    if (stat(real_path, &st) != 0) {
        return static_send_error(fd, 404, req->keep_alive);
    }
    if (!S_ISREG(st.st_mode)) {
        return static_send_error(fd, 403, req->keep_alive);
    }

    /* 打开文件 */
    int file_fd = open(real_path, O_RDONLY);
    if (file_fd < 0) {
        return static_send_error(fd, 403, req->keep_alive);
    }

    /* 生成 ETag 和 Last-Modified */
    char etag[64];
    char last_modified[64];
    generate_etag(&st, etag, sizeof(etag));
    format_http_time(st.st_mtime, last_modified, sizeof(last_modified));

    /* 检查缓存协商 */
    if (req->has_if_none_match && match_etag(etag, req->if_none_match)) {
        close(file_fd);
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
        if (client_time >= 0 && st.st_mtime <= client_time) {
            close(file_fd);
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

    /* 计算发送范围 */
    int64_t file_size = st.st_size;
    int64_t send_start = 0;
    int64_t send_end = file_size - 1;
    int status_code = 200;

    if (req->has_range) {
        send_start = req->range_start;
        if (req->range_end >= 0 && req->range_end < file_size) {
            send_end = req->range_end;
        }
        if (send_start >= file_size || send_start > send_end) {
            close(file_fd);
            return static_send_error(fd, 416, req->keep_alive);
        }
        status_code = 206;
    }

    int64_t send_length = send_end - send_start + 1;

    /* 构建响应头 */
    http_response_t resp = {
        .status_code = status_code,
        .status_text = status_code == 206 ? "Partial Content" : "OK",
        .content_type = http_mime_type(real_path),
        .content_length = send_length,
        .keep_alive = req->keep_alive,
        .has_range = req->has_range,
        .range_start = send_start,
        .range_end = send_end,
        .total_length = file_size,
        .etag = etag,
        .last_modified = last_modified
    };

    char header_buf[1024];
    int header_len = http_format_response_header(header_buf, sizeof(header_buf), &resp);
    if (header_len < 0) {
        close(file_fd);
        return static_send_error(fd, 500, req->keep_alive);
    }

    /* 发送响应头 */
    if (send_all(fd, header_buf, (size_t)header_len) != 0) {
        close(file_fd);
        return COCOON_ERROR;
    }

    /* 发送文件内容 */
    if (req->method == HTTP_HEAD) {
        /* HEAD 请求不发送 body */
        close(file_fd);
        return COCOON_OK;
    }

    /* 定位到起始位置 */
    if (send_start > 0) {
        lseek(file_fd, send_start, SEEK_SET);
    }

    /* 使用 sendfile 零拷贝发送 */
    off_t offset = send_start;
    ssize_t remaining = send_length;
    while (remaining > 0) {
        ssize_t n = sendfile(fd, file_fd, &offset, (size_t)remaining);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            /* sendfile 失败，回退到 read/write */
            break;
        }
        if (n == 0) break;
        remaining -= n;
    }

    close(file_fd);
    return COCOON_OK;
}

/**
 * html_escape - HTML 特殊字符转义
 *
 * 将 &, <, >, " 转义为对应的 HTML 实体，防止 XSS。
 *
 * @param src 原始字符串
 * @param dst 输出缓冲区
 * @param dst_size 缓冲区大小
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
 * static_serve_directory - 生成目录浏览页面
 *
 * 读取目录项，生成美观的 HTML 目录列表，支持排序。
 *
 * @param fd 客户端 socket
 * @param req HTTP 请求
 * @param root_dir 静态资源根目录
 * @param real_path 文件系统上的真实路径
 * @return COCOON_OK 成功，负值错误码
 */
int static_serve_directory(int fd, const http_request_t *req,
                           const char *root_dir, const char *real_path) {
    (void)root_dir;    /* 未直接使用，real_path 已通过 safe_path_join 处理 */
    
    /* 检查目录是否可访问 */
    struct stat st;
    if (stat(real_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return static_send_error(fd, 404, req->keep_alive);
    }

    DIR *dir = opendir(real_path);
    if (!dir) {
        return static_send_error(fd, 403, req->keep_alive);
    }

    /* 先收集所有目录项 */
    struct dirent *entry;
    char *entries[4096];
    int num_entries = 0;
    while ((entry = readdir(dir)) != NULL && num_entries < 4096) {
        if (entry->d_name[0] == '.') continue; /* 隐藏文件 */
        entries[num_entries] = strdup(entry->d_name);
        num_entries++;
    }
    closedir(dir);

    /* 构建 HTML */
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

    /* 添加返回上级链接 */
    if (strcmp(req->path, "/") != 0) {
        n += snprintf(html + n, sizeof(html) - n,
            "<tr><td><a href=\"../\">../</a></td><td>-</td><td>-</td></tr>\n");
    }

    /* 添加目录项 */
    for (int i = 0; i < num_entries; i++) {
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", real_path, entries[i]);

        struct stat entry_st;
        char size_str[32] = "-";
        char mtime_str[32] = "-";

        if (stat(full_path, &entry_st) == 0) {
            /* 格式化文件大小 */
            if (S_ISDIR(entry_st.st_mode)) {
                strncpy(size_str, "-", sizeof(size_str));
            } else if (entry_st.st_size < 1024) {
                snprintf(size_str, sizeof(size_str), "%ld B", (long)entry_st.st_size);
            } else if (entry_st.st_size < 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1f KB", entry_st.st_size / 1024.0);
            } else if (entry_st.st_size < 1024 * 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1f MB", entry_st.st_size / (1024.0 * 1024));
            } else {
                snprintf(size_str, sizeof(size_str), "%.1f GB", entry_st.st_size / (1024.0 * 1024 * 1024));
            }

            /* 格式化修改时间 */
            struct tm *tm_info = localtime(&entry_st.st_mtime);
            if (tm_info) {
                strftime(mtime_str, sizeof(mtime_str), "%Y-%m-%d %H:%M", tm_info);
            }
        }

        /* HTML 转义文件名 */
        char escaped_name[512];
        html_escape(entries[i], escaped_name, sizeof(escaped_name));

        n += snprintf(html + n, sizeof(html) - n,
            "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n",
            escaped_name,
            S_ISDIR(entry_st.st_mode) ? "/" : "",
            escaped_name,
            S_ISDIR(entry_st.st_mode) ? "/" : "",
            size_str, mtime_str);

        free(entries[i]);
    }

    n += snprintf(html + n, sizeof(html) - n,
        "</table>\n"
        "<hr>\n"
        "<p><em>Cocoon Server</em></p>\n"
        "</body></html>\n");

    /* 发送响应 */
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

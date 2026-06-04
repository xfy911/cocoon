/**
 * access_log.c - 访问日志模块实现
 *
 * 使用简化 Nginx combined 格式：
 *   %h %l %u %t "%r" %s %b "%{Referer}i" "%{User-Agent}i"
 *
 * 线程安全：使用 pthread_mutex_t 保护文件写入。
 *
 * @author xfy
 */

#include "access_log.h"
#include "http.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>

/* === 内部状态 === */
static FILE *g_log_file = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_enabled = false;

/**
 * format_log_time - 格式化当前时间为日志时间格式
 *
 * Nginx 格式: [05/Jun/2026:00:00:00 +0800]
 *
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 */
static void format_log_time(char *buf, size_t buf_size) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) {
        buf[0] = '\0';
        return;
    }

    /* 计算时区偏移 */
    long tz_offset = 0;
#if defined(__linux__) || defined(__unix__)
    tz_offset = tm->tm_gmtoff;
#else
    /* 通用回退 */
    struct tm gmt = {0};
    gmtime_r(&now, &gmt);
    time_t local = mktime(tm);
    time_t gmt_t = mktime(&gmt);
    tz_offset = (long)difftime(local, gmt_t);
#endif

    int tz_hours = (int)(tz_offset / 3600);
    int tz_mins = (int)((tz_offset % 3600) / 60);
    if (tz_mins < 0) tz_mins = -tz_mins;

    const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    snprintf(buf, buf_size, "[%02d/%s/%04d:%02d:%02d:%02d %c%02d%02d]",
             tm->tm_mday, months[tm->tm_mon], tm->tm_year + 1900,
             tm->tm_hour, tm->tm_min, tm->tm_sec,
             tz_offset >= 0 ? '+' : '-', tz_hours, tz_mins);
}

/**
 * get_client_ip - 从 sockaddr 提取客户端 IP 字符串
 *
 * @param addr sockaddr 结构体
 * @param addr_len 地址长度
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 */
static void get_client_ip(const struct sockaddr *addr, socklen_t addr_len,
                          char *buf, size_t buf_size) {
    (void)addr_len;
    buf[0] = '\0';

    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &sin->sin_addr, buf, (socklen_t)buf_size);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, buf, (socklen_t)buf_size);
    } else {
        snprintf(buf, buf_size, "unknown");
    }
}

/**
 * get_header - 从请求头数组中查找指定头
 *
 * @param req HTTP 请求
 * @param name 头名称（小写）
 * @return 头值，未找到返回 "-"
 */
static const char *get_header(const http_request_t *req, const char *name) {
    for (int i = 0; i < req->num_headers; i++) {
        if (strcmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return "-";
}

/* === 公共 API === */

int access_log_init(const char *path) {
    pthread_mutex_lock(&g_log_mutex);

    if (g_log_file && g_log_file != stdout) {
        fclose(g_log_file);
    }
    g_log_file = NULL;
    g_enabled = false;

    if (!path || strcmp(path, "-") == 0 || strcmp(path, "stdout") == 0) {
        g_log_file = stdout;
        g_enabled = true;
        log_info("访问日志输出到 stdout");
    } else {
        g_log_file = fopen(path, "a");
        if (!g_log_file) {
            log_error("无法打开访问日志文件: %s", path);
            pthread_mutex_unlock(&g_log_mutex);
            return -1;
        }
        g_enabled = true;
        log_info("访问日志: %s", path);
    }

    pthread_mutex_unlock(&g_log_mutex);
    return 0;
}

void access_log_close(void) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file && g_log_file != stdout) {
        fclose(g_log_file);
    }
    g_log_file = NULL;
    g_enabled = false;
    pthread_mutex_unlock(&g_log_mutex);
}

bool access_log_is_enabled(void) {
    return g_enabled;
}

void access_log_write(const struct sockaddr *client_addr, socklen_t addr_len,
                      const http_request_t *req, int status_code, int64_t response_bytes) {
    if (!g_enabled || !req) return;

    char ip[64];
    get_client_ip(client_addr, addr_len, ip, sizeof(ip));

    char time_str[64];
    format_log_time(time_str, sizeof(time_str));

    const char *method = http_method_str(req->method);
    const char *path = req->path;
    const char *version = req->version[0] ? req->version : "HTTP/1.1";

    const char *referer = get_header(req, "referer");
    const char *user_agent = get_header(req, "user-agent");

    /* 响应字节 */
    char bytes_str[32];
    if (response_bytes < 0) {
        snprintf(bytes_str, sizeof(bytes_str), "-");
    } else {
        snprintf(bytes_str, sizeof(bytes_str), "%ld", (long)response_bytes);
    }

    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file) {
        fprintf(g_log_file, "%s - - %s \"%s %s %s\" %d %s \"%s\" \"%s\"\n",
                ip[0] ? ip : "-",
                time_str,
                method, path, version,
                status_code,
                bytes_str,
                referer,
                user_agent);
        fflush(g_log_file);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

/**
 * log.c - 简单日志系统实现
 *
 * @author xfy
 */

#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

static log_level_t g_level = LOG_LEVEL_INFO;
static const char *g_prefix = "[Cocoon]";

/**
 * level_str - 将日志级别转换为字符串
 *
 * @param level 日志级别
 * @return 级别名称（ERROR/WARN/INFO/DEBUG/UNKNOWN）
 */
static const char *level_str(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        default:              return "UNKNOWN";
    }
}

/**
 * log_set_level - 设置全局日志级别
 *
 * 低于此级别的日志消息将被忽略。
 *
 * @param level 日志级别
 */
void log_set_level(log_level_t level) {
    g_level = level;
}

/**
 * log_set_prefix - 设置日志前缀
 *
 * @param prefix 前缀字符串，设为 NULL 则无前缀
 */
void log_set_prefix(const char *prefix) {
    g_prefix = prefix;
}

/**
 * log_get_level - 获取当前日志级别
 *
 * @return 当前日志级别
 */
log_level_t log_get_level(void) {
    return g_level;
}

/**
 * log_output - 日志输出核心函数
 *
 * 格式化时间戳、前缀、级别和消息，输出到 stderr。
 * 若日志级别低于全局级别则直接返回。
 *
 * @param level 日志级别
 * @param fmt 格式字符串
 * @param args 可变参数列表
 */
static void log_output(log_level_t level, const char *fmt, va_list args) {
    if (level > g_level) return;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    if (g_prefix) {
        fprintf(stderr, "%s %s [%s] ", time_buf, g_prefix, level_str(level));
    } else {
        fprintf(stderr, "%s [%s] ", time_buf, level_str(level));
    }
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
}

/**
 * log_error - 输出 ERROR 级别日志
 * @param fmt 格式字符串
 */
void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_output(LOG_LEVEL_ERROR, fmt, args);
    va_end(args);
}

/**
 * log_warn - 输出 WARN 级别日志
 * @param fmt 格式字符串
 */
void log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_output(LOG_LEVEL_WARN, fmt, args);
    va_end(args);
}

/**
 * log_info - 输出 INFO 级别日志
 * @param fmt 格式字符串
 */
void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_output(LOG_LEVEL_INFO, fmt, args);
    va_end(args);
}

/**
 * log_debug - 输出 DEBUG 级别日志
 * @param fmt 格式字符串
 */
void log_debug(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_output(LOG_LEVEL_DEBUG, fmt, args);
    va_end(args);
}

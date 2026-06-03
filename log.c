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

static const char *level_str(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        default:              return "UNKNOWN";
    }
}

void log_set_level(log_level_t level) {
    g_level = level;
}

void log_set_prefix(const char *prefix) {
    g_prefix = prefix;
}

log_level_t log_get_level(void) {
    return g_level;
}

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

void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_output(LOG_LEVEL_ERROR, fmt, args);
    va_end(args);
}

void log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_output(LOG_LEVEL_WARN, fmt, args);
    va_end(args);
}

void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_output(LOG_LEVEL_INFO, fmt, args);
    va_end(args);
}

void log_debug(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_output(LOG_LEVEL_DEBUG, fmt, args);
    va_end(args);
}

/**
 * log.h - 简单日志系统
 *
 * 支持分级日志输出：ERROR、WARN、INFO、DEBUG。
 * 线程安全（使用 printf，底层为线程安全）。
 *
 * @author xfy
 */

#ifndef COCOON_LOG_H
#define COCOON_LOG_H

#include <stdbool.h>

/* === 日志级别 === */
typedef enum {
    LOG_LEVEL_ERROR = 0,   /**< 错误，程序可能无法继续 */
    LOG_LEVEL_WARN  = 1,   /**< 警告，程序可继续但需注意 */
    LOG_LEVEL_INFO  = 2,   /**< 信息，正常运行输出 */
    LOG_LEVEL_DEBUG = 3,   /**< 调试，详细运行状态 */
} log_level_t;

/**
 * log_set_level - 设置全局日志级别
 *
 * 低于此级别的日志将被忽略。
 *
 * @param level 最低输出级别
 */
void log_set_level(log_level_t level);

/**
 * log_set_prefix - 设置日志前缀（默认 [Cocoon]）
 *
 * @param prefix 前缀字符串（如 "[Cocoon]"），NULL 表示无前缀
 */
void log_set_prefix(const char *prefix);

/**
 * log_get_level - 获取当前日志级别
 * @return 当前日志级别
 */
log_level_t log_get_level(void);

/**
 * log_error / log_warn / log_info / log_debug - 分级日志输出
 *
 * 使用 printf 风格格式化字符串，自动追加换行。
 */
void log_error(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_debug(const char *fmt, ...);

#endif /* COCOON_LOG_H */

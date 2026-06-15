/**
 * cache.h - 内存响应缓存模块
 *
 * 基于 LRU + TTL 策略的轻量级内存缓存，用于缓存小文件响应。
 * 避免重复磁盘 I/O，提升静态文件服务性能。
 *
 * 设计要点：
 *   - 哈希表 + 双向链表实现 O(1) 的 get/put
 *   - 单条缓存上限：避免大文件撑爆内存
 *   - 总容量上限：LRU 淘汰最久未使用的条目
 *   - TTL 过期：基于 expires_at 时间戳
 *   - 文件 mtime 校验：文件修改后缓存自动失效
 *   - 线程安全：pthread_mutex 保护
 *
 * @author xfy
 */

#ifndef COCOON_CACHE_H
#define COCOON_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* 前向声明，避免暴露内部结构 */
typedef struct cocoon_cache cocoon_cache_t;

/**
 * cocoon_cache_entry_t - 缓存条目
 *
 * 存储完整的 HTTP 响应（头 + 体），用于直接发送给客户端。
 */
typedef struct {
    char *key;              /**< 缓存键（文件绝对路径） */
    char *header;           /**< 格式化后的 HTTP 响应头 */
    size_t header_len;      /**< 响应头长度 */
    char *body;             /**< 响应体（文件内容） */
    size_t body_len;        /**< 响应体长度 */
    time_t mtime;           /**< 文件修改时间（用于失效校验） */
    time_t expires_at;      /**< TTL 过期时间 */
} cocoon_cache_entry_t;

/**
 * cocoon_cache_stats_t - 缓存统计
 */
typedef struct {
    size_t entries;         /**< 当前条目数 */
    size_t total_size;      /**< 当前总字节数 */
    size_t hits;            /**< 命中次数 */
    size_t misses;          /**< 未命中次数 */
    size_t evictions;       /**< 淘汰次数 */
    size_t expirations;     /**< TTL 过期次数 */
} cocoon_cache_stats_t;

/**
 * cache_create - 创建缓存实例
 *
 * @param max_size 最大总容量（字节），0 表示默认 64MB
 * @param ttl_seconds TTL 秒数，0 表示默认 60
 * @param max_entry_size 单条最大大小（字节），0 表示默认 1MB
 * @return 缓存实例，失败返回 NULL
 */
cocoon_cache_t *cache_create(size_t max_size, uint32_t ttl_seconds, size_t max_entry_size);

/**
 * cache_destroy - 销毁缓存实例
 *
 * 释放所有缓存条目和内部结构。
 *
 * @param cache 缓存实例
 */
void cache_destroy(cocoon_cache_t *cache);

/**
 * cache_get - 获取缓存条目
 *
 * 查找指定 key 的缓存。如果条目存在但 mtime 不匹配或已过期，
 * 则视为未命中并删除该条目。
 *
 * @param cache 缓存实例
 * @param key 缓存键（文件绝对路径）
 * @param mtime 当前文件修改时间（用于失效校验）
 * @return 缓存条目指针（引用，不拷贝），未命中返回 NULL
 */
const cocoon_cache_entry_t *cache_get(cocoon_cache_t *cache, const char *key, time_t mtime);

/**
 * cache_put - 存入缓存
 *
 * 将响应存入缓存。如果缓存已满，按 LRU 淘汰旧条目。
 * 如果条目大小超过 max_entry_size，则忽略（不存入）。
 *
 * @param cache 缓存实例
 * @param key 缓存键
 * @param header 响应头（会被拷贝）
 * @param header_len 响应头长度
 * @param body 响应体（会被拷贝）
 * @param body_len 响应体长度
 * @param mtime 文件修改时间
 */
void cache_put(cocoon_cache_t *cache, const char *key,
               const char *header, size_t header_len,
               const char *body, size_t body_len,
               time_t mtime);

/**
 * cache_stats - 获取缓存统计
 *
 * @param cache 缓存实例
 * @param stats 输出统计结构
 */
void cache_stats(cocoon_cache_t *cache, cocoon_cache_stats_t *stats);

/**
 * cache_clear - 清空缓存
 *
 * 删除所有缓存条目，重置统计。
 *
 * @param cache 缓存实例
 */
void cache_clear(cocoon_cache_t *cache);

/**
 * cache_is_eligible - 判断文件是否适合缓存
 *
 * @param cache 缓存实例
 * @param file_size 文件大小（字节）
 * @return true 适合缓存
 */
bool cache_is_eligible(const cocoon_cache_t *cache, int64_t file_size);

#endif /* COCOON_CACHE_H */

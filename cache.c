/**
 * cache.c - 内存响应缓存实现
 *
 * LRU + TTL 缓存，哈希表 + 双向链表。
 * 线程安全：所有操作受 pthread_mutex 保护。
 *
 * @author xfy
 */

#include "cache.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define CACHE_DEFAULT_MAX_SIZE       (64 * 1024 * 1024)   /**< 默认 64MB */
#define CACHE_DEFAULT_TTL_SECONDS    60                   /**< 默认 60 秒 */
#define CACHE_DEFAULT_MAX_ENTRY_SIZE (1024 * 1024)        /**< 默认 1MB */
#define CACHE_DEFAULT_BUCKET_COUNT   512                  /**< 哈希桶数 */

/**
 * cache_node_t - 内部链表节点
 *
 * 双向链表实现 LRU：头节点为最近使用，尾节点为最久未使用。
 */
typedef struct cache_node {
    char *key;                      /**< 键（独立拷贝） */
    char *header;                   /**< 响应头 */
    size_t header_len;              /**< 响应头长度 */
    char *body;                     /**< 响应体 */
    size_t body_len;                /**< 响应体长度 */
    time_t mtime;                   /**< 文件修改时间 */
    time_t expires_at;              /**< TTL 过期时间 */
    struct cache_node *next;        /**< LRU 链表：前驱（更近） */
    struct cache_node *prev;        /**< LRU 链表：后继（更久） */
    struct cache_node *hash_next;   /**< 哈希链表 */
} cache_node_t;

struct cocoon_cache {
    cache_node_t *lru_head;         /**< LRU 头（最近使用） */
    cache_node_t *lru_tail;         /**< LRU 尾（最久未使用） */
    cache_node_t **buckets;         /**< 哈希桶数组 */
    size_t bucket_count;            /**< 桶数量 */
    size_t max_size;                /**< 最大总容量 */
    size_t max_entry_size;          /**< 单条最大大小 */
    uint32_t ttl_seconds;           /**< TTL 秒数 */
    size_t total_size;              /**< 当前总大小 */
    size_t hits;                    /**< 命中次数 */
    size_t misses;                  /**< 未命中次数 */
    size_t evictions;               /**< 淘汰次数 */
    size_t expirations;             /**< 过期次数 */
    pthread_mutex_t lock;           /**< 线程锁 */
};

/* === 内部辅助函数 === */

/**
 * hash_fn - djb2 哈希
 */
static size_t hash_fn(const char *str) {
    size_t hash = 5381;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

/**
 * lru_move_to_head - 将节点移到 LRU 链表头部（最近使用）
 */
static void lru_move_to_head(cocoon_cache_t *cache, cache_node_t *node) {
    if (cache->lru_head == node) return;

    /* 从原位置移除 */
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (cache->lru_tail == node) cache->lru_tail = node->prev;

    /* 插入头部 */
    node->next = cache->lru_head;
    node->prev = NULL;
    if (cache->lru_head) cache->lru_head->prev = node;
    cache->lru_head = node;
    if (!cache->lru_tail) cache->lru_tail = node;
}

/**
 * lru_remove - 从 LRU 链表中移除节点
 */
static void lru_remove(cocoon_cache_t *cache, cache_node_t *node) {
    if (node->prev) node->prev->next = node->next;
    else cache->lru_head = node->next;

    if (node->next) node->next->prev = node->prev;
    else cache->lru_tail = node->prev;

    node->prev = NULL;
    node->next = NULL;
}

/**
 * hash_remove - 从哈希表中移除节点
 */
static void hash_remove(cocoon_cache_t *cache, const char *key) {
    size_t idx = hash_fn(key) % cache->bucket_count;
    cache_node_t **pp = &cache->buckets[idx];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            *pp = (*pp)->hash_next;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

/**
 * hash_insert - 插入到哈希表
 */
static void hash_insert(cocoon_cache_t *cache, cache_node_t *node) {
    size_t idx = hash_fn(node->key) % cache->bucket_count;
    node->hash_next = cache->buckets[idx];
    cache->buckets[idx] = node;
}

/**
 * hash_find - 在哈希表中查找节点
 */
static cache_node_t *hash_find(cocoon_cache_t *cache, const char *key) {
    size_t idx = hash_fn(key) % cache->bucket_count;
    cache_node_t *node = cache->buckets[idx];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            return node;
        }
        node = node->hash_next;
    }
    return NULL;
}

/**
 * node_free - 释放单个节点
 */
static void node_free(cache_node_t *node) {
    if (!node) return;
    free(node->key);
    free(node->header);
    free(node->body);
    free(node);
}

/**
 * evict_one - 淘汰最久未使用的条目
 */
static void evict_one(cocoon_cache_t *cache) {
    cache_node_t *victim = cache->lru_tail;
    if (!victim) return;

    lru_remove(cache, victim);
    hash_remove(cache, victim->key);

    size_t entry_size = victim->header_len + victim->body_len;
    cache->total_size -= entry_size;
    cache->evictions++;

    log_debug("[Cache] LRU 淘汰: %s (大小 %zu 字节)", victim->key, entry_size);
    node_free(victim);
}

/**
 * make_room - 为新条目腾出空间
 */
static void make_room(cocoon_cache_t *cache, size_t needed) {
    while (cache->total_size + needed > cache->max_size && cache->lru_tail) {
        evict_one(cache);
    }
}

/* === 公共 API === */

cocoon_cache_t *cache_create(size_t max_size, uint32_t ttl_seconds, size_t max_entry_size) {
    cocoon_cache_t *cache = (cocoon_cache_t *)calloc(1, sizeof(cocoon_cache_t));
    if (!cache) {
        log_error("[Cache] 分配缓存结构失败");
        return NULL;
    }

    cache->max_size = max_size > 0 ? max_size : CACHE_DEFAULT_MAX_SIZE;
    cache->ttl_seconds = ttl_seconds > 0 ? ttl_seconds : CACHE_DEFAULT_TTL_SECONDS;
    cache->max_entry_size = max_entry_size > 0 ? max_entry_size : CACHE_DEFAULT_MAX_ENTRY_SIZE;
    cache->bucket_count = CACHE_DEFAULT_BUCKET_COUNT;
    cache->buckets = (cache_node_t **)calloc(cache->bucket_count, sizeof(cache_node_t *));
    if (!cache->buckets) {
        log_error("[Cache] 分配哈希桶失败");
        free(cache);
        return NULL;
    }

    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        log_error("[Cache] 初始化互斥锁失败");
        free(cache->buckets);
        free(cache);
        return NULL;
    }

    log_info("[Cache] 已创建: 最大容量 %zu 字节, TTL %u 秒, 单条上限 %zu 字节",
             cache->max_size, cache->ttl_seconds, cache->max_entry_size);
    return cache;
}

void cache_destroy(cocoon_cache_t *cache) {
    if (!cache) return;

    pthread_mutex_lock(&cache->lock);

    cache_node_t *node = cache->lru_head;
    while (node) {
        cache_node_t *next = node->next;
        node_free(node);
        node = next;
    }

    free(cache->buckets);
    pthread_mutex_unlock(&cache->lock);
    pthread_mutex_destroy(&cache->lock);
    free(cache);

    log_info("[Cache] 已销毁");
}

const cocoon_cache_entry_t *cache_get(cocoon_cache_t *cache, const char *key, time_t mtime) {
    if (!cache || !key) return NULL;

    pthread_mutex_lock(&cache->lock);

    cache_node_t *node = hash_find(cache, key);
    if (!node) {
        cache->misses++;
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    /* 检查 TTL */
    time_t now = time(NULL);
    if (now > node->expires_at) {
        cache->expirations++;
        cache->misses++;
        lru_remove(cache, node);
        hash_remove(cache, key);
        cache->total_size -= (node->header_len + node->body_len);
        node_free(node);
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    /* 检查文件是否被修改 */
    if (node->mtime != mtime) {
        cache->misses++;
        lru_remove(cache, node);
        hash_remove(cache, key);
        cache->total_size -= (node->header_len + node->body_len);
        node_free(node);
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }

    /* 命中：移到 LRU 头部 */
    lru_move_to_head(cache, node);
    cache->hits++;

    /* 构造返回结构（栈上临时，调用者需立即使用） */
    static __thread cocoon_cache_entry_t entry;
    entry.key = node->key;
    entry.header = node->header;
    entry.header_len = node->header_len;
    entry.body = node->body;
    entry.body_len = node->body_len;
    entry.mtime = node->mtime;
    entry.expires_at = node->expires_at;

    pthread_mutex_unlock(&cache->lock);
    return &entry;
}

void cache_put(cocoon_cache_t *cache, const char *key,
               const char *header, size_t header_len,
               const char *body, size_t body_len,
               time_t mtime) {
    if (!cache || !key || !header || !body) return;

    size_t entry_size = header_len + body_len;

    /* 超过单条上限，跳过 */
    if (entry_size > cache->max_entry_size) {
        log_debug("[Cache] 条目 %s 大小 %zu 超过上限 %zu，不缓存", key, entry_size, cache->max_entry_size);
        return;
    }

    pthread_mutex_lock(&cache->lock);

    /* 如果已存在，先删除旧条目 */
    cache_node_t *existing = hash_find(cache, key);
    if (existing) {
        lru_remove(cache, existing);
        hash_remove(cache, key);
        cache->total_size -= (existing->header_len + existing->body_len);
        node_free(existing);
    }

    /* 腾出空间 */
    make_room(cache, entry_size);

    /* 创建新节点 */
    cache_node_t *node = (cache_node_t *)calloc(1, sizeof(cache_node_t));
    if (!node) {
        pthread_mutex_unlock(&cache->lock);
        log_error("[Cache] 分配节点失败");
        return;
    }

    node->key = strdup(key);
    node->header = (char *)malloc(header_len);
    node->body = (char *)malloc(body_len);
    if (!node->key || !node->header || !node->body) {
        node_free(node);
        pthread_mutex_unlock(&cache->lock);
        log_error("[Cache] 拷贝数据失败");
        return;
    }

    memcpy(node->header, header, header_len);
    node->header_len = header_len;
    memcpy(node->body, body, body_len);
    node->body_len = body_len;
    node->mtime = mtime;
    node->expires_at = time(NULL) + cache->ttl_seconds;

    /* 插入 LRU 头部和哈希表 */
    node->next = cache->lru_head;
    if (cache->lru_head) cache->lru_head->prev = node;
    cache->lru_head = node;
    if (!cache->lru_tail) cache->lru_tail = node;

    hash_insert(cache, node);
    cache->total_size += entry_size;

    pthread_mutex_unlock(&cache->lock);

    log_debug("[Cache] 已缓存: %s (头 %zu + 体 %zu = %zu 字节, 总计 %zu 字节)",
              key, header_len, body_len, entry_size, cache->total_size);
}

void cache_stats(cocoon_cache_t *cache, cocoon_cache_stats_t *stats) {
    if (!cache || !stats) return;

    pthread_mutex_lock(&cache->lock);
    stats->entries = 0;
    cache_node_t *node = cache->lru_head;
    while (node) {
        stats->entries++;
        node = node->next;
    }
    stats->total_size = cache->total_size;
    stats->hits = cache->hits;
    stats->misses = cache->misses;
    stats->evictions = cache->evictions;
    stats->expirations = cache->expirations;
    pthread_mutex_unlock(&cache->lock);
}

void cache_clear(cocoon_cache_t *cache) {
    if (!cache) return;

    pthread_mutex_lock(&cache->lock);

    cache_node_t *node = cache->lru_head;
    while (node) {
        cache_node_t *next = node->next;
        node_free(node);
        node = next;
    }
    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    memset(cache->buckets, 0, cache->bucket_count * sizeof(cache_node_t *));
    cache->total_size = 0;
    cache->hits = 0;
    cache->misses = 0;
    cache->evictions = 0;
    cache->expirations = 0;

    pthread_mutex_unlock(&cache->lock);

    log_info("[Cache] 已清空");
}

/* 缓存辅助函数：判断文件是否适合缓存 */
bool cache_is_eligible(const cocoon_cache_t *cache, int64_t file_size) {
    if (!cache || file_size <= 0) return false;
    return cache->max_entry_size > 0 && (size_t)file_size <= cache->max_entry_size;
}

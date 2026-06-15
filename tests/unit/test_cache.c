/**
 * test_cache.c - 内存缓存单元测试
 *
 * 测试 LRU + TTL 缓存的 get/put、LRU 淘汰、TTL 失效、统计等功能。
 */

#include "cache.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

static int g_tests = 0;
static int g_passed = 0;

#define TEST_ASSERT(cond) do { \
    g_tests++; \
    if (cond) { g_passed++; } else { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while(0)

#define TEST_ASSERT_EQUAL(expected, actual) do { \
    g_tests++; \
    if ((expected) == (actual)) { g_passed++; } else { \
        fprintf(stderr, "FAIL: %s:%d: expected %lld, got %lld\n", __FILE__, __LINE__, (long long)(expected), (long long)(actual)); \
    } \
} while(0)

static void test_cache_create_destroy(void) {
    cocoon_cache_t *cache = cache_create(1024, 60, 512);
    TEST_ASSERT(cache != NULL);
    if (cache) {
        cache_destroy(cache);
    }
}

static void test_cache_put_get(void) {
    cocoon_cache_t *cache = cache_create(1024, 60, 512);
    TEST_ASSERT(cache != NULL);
    if (!cache) return;

    const char *key = "/test/file.txt";
    const char *header = "HTTP/1.1 200 OK\r\n";
    const char *body = "hello world";

    cache_put(cache, key, header, strlen(header), body, strlen(body), 12345);

    const cocoon_cache_entry_t *entry = cache_get(cache, key, 12345);
    TEST_ASSERT(entry != NULL);
    if (entry) {
        TEST_ASSERT_EQUAL(strlen(header), entry->header_len);
        TEST_ASSERT_EQUAL(strlen(body), entry->body_len);
        TEST_ASSERT(memcmp(entry->header, header, strlen(header)) == 0);
        TEST_ASSERT(memcmp(entry->body, body, strlen(body)) == 0);
    }

    cache_destroy(cache);
}

static void test_cache_mtime_invalidation(void) {
    cocoon_cache_t *cache = cache_create(1024, 60, 512);
    TEST_ASSERT(cache != NULL);
    if (!cache) return;

    const char *key = "/test/file.txt";
    cache_put(cache, key, "H", 1, "B", 1, 100);

    /* mtime 不匹配，缓存应失效 */
    const cocoon_cache_entry_t *entry = cache_get(cache, key, 200);
    TEST_ASSERT(entry == NULL);

    cache_destroy(cache);
}

static void test_cache_lru_eviction(void) {
    /* 最大 18 字节（3条刚好满），单条 32 字节，存 4 条应该淘汰最早的 */
    cocoon_cache_t *cache = cache_create(18, 60, 32);
    TEST_ASSERT(cache != NULL);
    if (!cache) return;

    cache_put(cache, "/a", "H", 1, "bodyA", 5, 1);
    cache_put(cache, "/b", "H", 1, "bodyB", 5, 1);
    cache_put(cache, "/c", "H", 1, "bodyC", 5, 1);

    /* 访问 /a，提升其优先级 */
    cache_get(cache, "/a", 1);

    /* 再存 /d，应该淘汰 /b（最久未访问） */
    cache_put(cache, "/d", "H", 1, "bodyD", 5, 1);

    TEST_ASSERT(cache_get(cache, "/a", 1) != NULL);  /* /a 被访问过，保留 */
    TEST_ASSERT(cache_get(cache, "/b", 1) == NULL);  /* /b 最久未访问，被淘汰 */
    TEST_ASSERT(cache_get(cache, "/c", 1) != NULL);
    TEST_ASSERT(cache_get(cache, "/d", 1) != NULL);

    cache_destroy(cache);
}

static void test_cache_ttl_expiration(void) {
    /* TTL 1 秒 */
    cocoon_cache_t *cache = cache_create(1024, 1, 512);
    TEST_ASSERT(cache != NULL);
    if (!cache) return;

    cache_put(cache, "/ttl", "H", 1, "body", 4, 1);
    TEST_ASSERT(cache_get(cache, "/ttl", 1) != NULL);

    /* 等待 2 秒 */
    sleep(2);

    TEST_ASSERT(cache_get(cache, "/ttl", 1) == NULL);

    cache_destroy(cache);
}

static void test_cache_stats(void) {
    cocoon_cache_t *cache = cache_create(1024, 60, 512);
    TEST_ASSERT(cache != NULL);
    if (!cache) return;

    /* 先清除统计（cache_create 后可能有其他操作） */
    cache_put(cache, "/s1", "H", 1, "B", 1, 1);
    cache_put(cache, "/s2", "H", 1, "B", 1, 1);

    /* 两次命中 */
    cache_get(cache, "/s1", 1);
    cache_get(cache, "/s1", 1);

    /* 一次 miss（mtime 不同） */
    cache_get(cache, "/s2", 2);

    cocoon_cache_stats_t stats;
    cache_stats(cache, &stats);
    TEST_ASSERT_EQUAL(2, stats.hits);
    TEST_ASSERT_EQUAL(1, stats.misses);
    TEST_ASSERT_EQUAL(0, stats.evictions);

    cache_destroy(cache);
}

static void test_cache_clear(void) {
    cocoon_cache_t *cache = cache_create(1024, 60, 512);
    TEST_ASSERT(cache != NULL);
    if (!cache) return;

    cache_put(cache, "/c1", "H", 1, "B", 1, 1);
    cache_put(cache, "/c2", "H", 1, "B", 1, 1);

    TEST_ASSERT(cache_get(cache, "/c1", 1) != NULL);
    TEST_ASSERT(cache_get(cache, "/c2", 1) != NULL);

    cache_clear(cache);

    TEST_ASSERT(cache_get(cache, "/c1", 1) == NULL);
    TEST_ASSERT(cache_get(cache, "/c2", 1) == NULL);
    cache_clear(cache);
    cocoon_cache_stats_t stats;
    cache_stats(cache, &stats);
    TEST_ASSERT_EQUAL(0, stats.total_size);
    TEST_ASSERT_EQUAL(0, stats.entries);

    cache_destroy(cache);
}

/* Unity 框架要求的 setUp/tearDown 钩子 */
void setUp(void) {}
void tearDown(void) {}

int main(void) {
    test_cache_create_destroy();
    test_cache_put_get();
    test_cache_mtime_invalidation();
    test_cache_lru_eviction();
    test_cache_ttl_expiration();
    test_cache_stats();
    test_cache_clear();

    printf("Cache tests: %d/%d passed\n", g_passed, g_tests);
    return (g_passed == g_tests) ? 0 : 1;
}

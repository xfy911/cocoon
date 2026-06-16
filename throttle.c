/**
 * throttle.c - Token Bucket 带宽限速实现
 *
 * 支持 per-connection 和全局总限速，通过 fd 注册机制自动应用。
 *
 * @author xfy
 */

#include "throttle.h"
#include "platform.h"
#include <pthread.h>
#include <string.h>

/* 最大支持的 fd 数（Linux 默认进程限制通常是 1024-65536） */
#define MAX_FD 65536

/* fd -> throttle 映射表，受互斥锁保护 */
static cocoon_throttle_t *g_fd_throttle[MAX_FD];
static pthread_mutex_t g_throttle_mutex = PTHREAD_MUTEX_INITIALIZER;

void throttle_init(cocoon_throttle_t *t, uint64_t rate_bytes_per_sec, uint64_t burst_bytes) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->rate_bytes_per_sec = rate_bytes_per_sec;
    t->burst_bytes = burst_bytes > 0 ? burst_bytes : rate_bytes_per_sec;
    t->tokens = t->burst_bytes; /* 初始满桶 */
    clock_gettime(CLOCK_MONOTONIC, &t->last_update);
}

uint64_t throttle_consume(cocoon_throttle_t *t, size_t bytes) {
    if (!t || t->rate_bytes_per_sec == 0) return 0;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* 计算时间差（微秒） */
    uint64_t elapsed_us = (uint64_t)(now.tv_sec - t->last_update.tv_sec) * 1000000
                        + (uint64_t)(now.tv_nsec - t->last_update.tv_nsec) / 1000;

    /* 补充令牌：rate * elapsed_us / 1_000_000 */
    uint64_t new_tokens = t->rate_bytes_per_sec * elapsed_us / 1000000;
    if (new_tokens > 0) {
        t->tokens += new_tokens;
        if (t->tokens > t->burst_bytes) {
            t->tokens = t->burst_bytes;
        }
        t->last_update = now;
    }

    /* 尝试消费 */
    if (t->tokens >= bytes) {
        t->tokens -= bytes;
        return 0; /* 无需等待 */
    }

    /* 令牌不足，计算需要等待的时间 */
    uint64_t need = bytes - t->tokens;
    uint64_t wait_us = need * 1000000 / t->rate_bytes_per_sec;
    /* 最小等待 1ms，避免忙等 */
    if (wait_us < 1000) wait_us = 1000;
    return wait_us;
}

void throttle_set_fd(int fd, cocoon_throttle_t *t) {
    if (fd >= 0 && fd < MAX_FD) {
        pthread_mutex_lock(&g_throttle_mutex);
        g_fd_throttle[fd] = t;
        pthread_mutex_unlock(&g_throttle_mutex);
    }
}

void throttle_clear_fd(int fd) {
    if (fd >= 0 && fd < MAX_FD) {
        pthread_mutex_lock(&g_throttle_mutex);
        g_fd_throttle[fd] = NULL;
        pthread_mutex_unlock(&g_throttle_mutex);
    }
}

cocoon_throttle_t *throttle_lookup(int fd) {
    if (fd < 0 || fd >= MAX_FD) return NULL;
    pthread_mutex_lock(&g_throttle_mutex);
    cocoon_throttle_t *t = g_fd_throttle[fd];
    pthread_mutex_unlock(&g_throttle_mutex);
    return t;
}

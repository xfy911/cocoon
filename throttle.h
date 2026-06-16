/**
 * throttle.h - 带宽限速模块
 *
 * Token Bucket 算法实现，支持 per-connection 和全局总限速。
 * 通过 fd 注册/查询机制，在 send_all 和 cocoon_file_send 中自动应用限速。
 *
 * @author xfy
 */

#ifndef COCOON_THROTTLE_H
#define COCOON_THROTTLE_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/**
 * cocoon_throttle_t - Token Bucket 限速器
 *
 * 基于令牌桶算法，控制每秒发送的字节数。
 * rate: 每秒补充的令牌数（bytes/sec）
 * burst: 桶的最大容量（bytes），允许突发传输
 */
typedef struct {
    uint64_t rate_bytes_per_sec;  /**< 每秒速率（bytes/sec） */
    uint64_t burst_bytes;         /**< 突发容量（bytes） */
    uint64_t tokens;              /**< 当前桶中令牌数 */
    struct timespec last_update;  /**< 上次更新时间 */
} cocoon_throttle_t;

/**
 * throttle_init - 初始化限速器
 *
 * @param t 限速器指针
 * @param rate_bytes_per_sec 每秒速率（0 表示不限制）
 * @param burst_bytes 突发容量（0 表示使用 rate 作为 burst）
 */
void throttle_init(cocoon_throttle_t *t, uint64_t rate_bytes_per_sec, uint64_t burst_bytes);

/**
 * throttle_consume - 从桶中消费 N 字节
 *
 * 根据当前时间和上次更新时间计算应补充的令牌数，
 * 然后尝试消费指定字节数。如果令牌不足，返回需要等待的微秒数。
 *
 * @param t 限速器指针
 * @param bytes 需要消费的字节数
 * @return 需要等待的微秒数（0 表示无需等待，可直接发送）
 */
uint64_t throttle_consume(cocoon_throttle_t *t, size_t bytes);

/**
 * throttle_set_fd - 为 fd 注册限速器
 *
 * 在 send_all / cocoon_file_send 中会自动查找并应用限速。
 *
 * @param fd socket 文件描述符
 * @param t 限速器指针（NULL 表示清除）
 */
void throttle_set_fd(int fd, cocoon_throttle_t *t);

/**
 * throttle_clear_fd - 清除 fd 的限速器
 *
 * @param fd socket 文件描述符
 */
void throttle_clear_fd(int fd);

/**
 * throttle_lookup - 查询 fd 的限速器
 *
 * @param fd socket 文件描述符
 * @return 限速器指针，NULL 表示无限制
 */
cocoon_throttle_t *throttle_lookup(int fd);

#endif /* COCOON_THROTTLE_H */

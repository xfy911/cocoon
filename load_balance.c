/**
 * @file load_balance.c - 分布式负载均衡模块实现
 * @brief 实现多种负载均衡算法：一致性哈希、最少连接、加权响应时间、随机
 *
 * Phase 4 第二项：多种负载均衡算法
 *
 * @author xfy
 */

#include "load_balance.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ===== 内部辅助函数声明 ===== */

/**
 * @brief 获取健康的后端数量
 */
static size_t count_healthy_backends(cocoon_proxy_rule_t *rule);

/**
 * @brief 最少连接算法实现
 */
static int select_least_connections(cocoon_load_balancer_t *lb,
                                    cocoon_proxy_rule_t *rule);

/**
 * @brief 加权响应时间 (EWMA) 算法实现
 */
static int select_weighted_response(cocoon_load_balancer_t *lb,
                                    cocoon_proxy_rule_t *rule);

/**
 * @brief 一致性哈希算法实现
 */
static int select_consistent_hash(cocoon_load_balancer_t *lb,
                                  cocoon_proxy_rule_t *rule,
                                  const char *hash_key);

/**
 * @brief 随机算法实现
 */
static int select_random(cocoon_proxy_rule_t *rule);

/**
 * @brief 虚拟节点比较函数（用于 qsort）
 */
static int node_compare(const void *a, const void *b);

/**
 * @brief 为单个后端生成虚拟节点
 */
static void generate_virtual_nodes(cocoon_hash_ring_node_t *nodes,
                                   size_t *count,
                                   size_t backend_idx,
                                   const char *host,
                                   uint16_t port,
                                   size_t replicas);

/* ===== 公共 API 实现 ===== */

void lb_init(cocoon_load_balancer_t *lb, cocoon_lb_algorithm_t algo) {
    if (!lb) return;

    memset(lb, 0, sizeof(*lb));
    lb->algorithm = algo;
    lb->alpha = 80; /**< 默认 EWMA 平滑因子 0.8（以百分制表示） */
    pthread_mutex_init(&lb->mutex, NULL);

    /* 初始化哈希环 */
    lb->hash_ring.node_count = 0;
    lb->hash_ring.initialized = false;

    /* 初始化所有后端统计 */
    for (size_t i = 0; i < COCOON_MAX_PROXY_BACKENDS; i++) {
        lb->stats[i].active_connections = 0;
        lb->stats[i].total_requests = 0;
        lb->stats[i].total_failures = 0;
        lb->stats[i].total_response_time_us = 0;
        lb->stats[i].last_response_time_us = 0;
        lb->stats[i].ewma_response_time_us = 0;
    }
}

void lb_destroy(cocoon_load_balancer_t *lb) {
    if (!lb) return;
    pthread_mutex_destroy(&lb->mutex);
    memset(lb, 0, sizeof(*lb));
}

int lb_select_backend(cocoon_load_balancer_t *lb, cocoon_proxy_rule_t *rule,
                      const char *hash_key) {
    if (!lb || !rule || rule->backend_count == 0) {
        return -1;
    }

    switch (lb->algorithm) {
    case COCOON_LB_ROUND_ROBIN:
        /**
         * 加权轮询使用 proxy.c 中已有的平滑加权轮询算法。
         * 返回 -1 表示调用者应使用 select_backend_sww()。
         */
        return -1;

    case COCOON_LB_LEAST_CONNECTIONS:
        return select_least_connections(lb, rule);

    case COCOON_LB_WEIGHTED_RESPONSE:
        return select_weighted_response(lb, rule);

    case COCOON_LB_CONSISTENT_HASH:
        return select_consistent_hash(lb, rule, hash_key);

    case COCOON_LB_RANDOM:
        return select_random(rule);

    default:
        return -1;
    }
}

void lb_update_stats_request_start(cocoon_load_balancer_t *lb, size_t backend_idx) {
    if (!lb || backend_idx >= COCOON_MAX_PROXY_BACKENDS) return;

    pthread_mutex_lock(&lb->mutex);
    lb->stats[backend_idx].active_connections++;
    lb->stats[backend_idx].total_requests++;
    pthread_mutex_unlock(&lb->mutex);
}

void lb_update_stats_request_end(cocoon_load_balancer_t *lb, size_t backend_idx,
                                 bool success, uint64_t response_time_us) {
    if (!lb || backend_idx >= COCOON_MAX_PROXY_BACKENDS) return;

    pthread_mutex_lock(&lb->mutex);

    cocoon_backend_stats_t *s = &lb->stats[backend_idx];

    /* 减少活跃连接数 */
    if (s->active_connections > 0) {
        s->active_connections--;
    }

    s->total_response_time_us += response_time_us;
    s->last_response_time_us = response_time_us;

    if (!success) {
        s->total_failures++;
    }

    /**
     * EWMA 更新公式：
     *   new_ewma = (alpha * current_rtt + (100 - alpha) * old_ewma) / 100
     *
     * alpha = 80 表示新值占 80%，旧值占 20%，对近期响应更敏感。
     * 首次更新（old_ewma == 0）时直接使用当前值。
     */
    uint32_t a = lb->alpha;
    if (s->ewma_response_time_us == 0) {
        s->ewma_response_time_us = response_time_us;
    } else {
        s->ewma_response_time_us = (a * response_time_us +
                                    (100 - a) * s->ewma_response_time_us) / 100;
    }

    pthread_mutex_unlock(&lb->mutex);
}

void lb_build_hash_ring(cocoon_hash_ring_t *ring, cocoon_proxy_rule_t *rule) {
    if (!ring || !rule) return;

    ring->node_count = 0;
    ring->initialized = false;

    if (rule->backend_count == 0) return;

    /**
     * 为每个后端生成 COCOON_HASH_RING_SIZE 个虚拟节点。
     * 虚拟节点数量 = 后端数 × 每个后端的副本数
     */
    for (size_t i = 0; i < rule->backend_count; i++) {
        cocoon_proxy_backend_t *be = &rule->backends[i];

        /* 跳过重和不健康的后端 */
        if (!be->healthy) continue;

        generate_virtual_nodes(ring->nodes, &ring->node_count,
                               i, be->target_host, be->target_port,
                               COCOON_HASH_RING_SIZE);
    }

    if (ring->node_count == 0) return;

    /**
     * 按哈希值升序排序，二分查找依赖有序数组。
     */
    qsort(ring->nodes, ring->node_count,
          sizeof(cocoon_hash_ring_node_t), node_compare);

    ring->initialized = true;
}

uint32_t lb_hash_key(const char *key, size_t len) {
    if (!key || len == 0) return 0;

    /**
     * MurmurHash3 x86 32-bit 实现
     *
     * 参考原始 MurmurHash3 算法参数：
     *   c1 = 0xcc9e2d51  （第一次混合常数）
     *   c2 = 0x1b873593  （第二次混合常数）
     *   r1 = 15          （第一次右移位数）
     *   r2 = 13          （第二次右移位数）
     *   m  = 5           （累乘常数）
     *   n  = 0xe6546b64  （累加常数）
     *
     * 此实现为纯软件、零外部依赖版本。
     */
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;
    const uint32_t r1 = 15;
    const uint32_t r2 = 13;
    const uint32_t m  = 5;
    const uint32_t n  = 0xe6546b64;

    uint32_t hash = 0; /**< seed = 0，保证相同输入总有相同输出 */
    const size_t nblocks = len / 4;
    const uint32_t *blocks = (const uint32_t *)(const void *)key;

    /* === 主体：4-byte 块处理 === */
    for (size_t i = 0; i < nblocks; i++) {
        uint32_t k = blocks[i];

        /* 第一次混合 */
        k *= c1;
        k = (k << r1) | (k >> (32 - r1)); /**< ROTL32(k, r1) */
        k *= c2;

        /* 哈希混合 */
        hash ^= k;
        hash = ((hash << r2) | (hash >> (32 - r2))); /**< ROTL32(hash, r2) */
        hash = hash * m + n;
    }

    /* === 尾部：不足 4 字节的部分 === */
    const uint8_t *tail = (const uint8_t *)(key + nblocks * 4);
    uint32_t k1 = 0;

    switch (len & 3) { /**< len % 4 */
    case 3:
        k1 ^= (uint32_t)tail[2] << 16;
        /* fall through */
    case 2:
        k1 ^= (uint32_t)tail[1] << 8;
        /* fall through */
    case 1:
        k1 ^= (uint32_t)tail[0];
        k1 *= c1;
        k1 = (k1 << r1) | (k1 >> (32 - r1));
        k1 *= c2;
        hash ^= k1;
        break;
    default:
        break;
    }

    /* === 最终化（finalization） === */
    hash ^= (uint32_t)len;
    hash ^= hash >> 16;
    hash *= 0x85ebca6b;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35;
    hash ^= hash >> 16;

    return hash;
}

size_t lb_pick_from_ring(cocoon_hash_ring_t *ring, uint32_t hash) {
    if (!ring || ring->node_count == 0) return 0;

    /**
     * 二分查找：定位第一个 node_hash >= hash 的虚拟节点
     *
     * 如果 hash 大于环中所有节点的哈希值，环绕到第一个节点
     *（取模运算自然处理此情况）。
     */
    size_t lo = 0;
    size_t hi = ring->node_count;

    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ring->nodes[mid].node_hash < hash) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return ring->nodes[lo % ring->node_count].backend_index;
}

const char *lb_algorithm_name(cocoon_lb_algorithm_t algo) {
    switch (algo) {
    case COCOON_LB_ROUND_ROBIN:       return "round_robin";
    case COCOON_LB_LEAST_CONNECTIONS: return "least_connections";
    case COCOON_LB_WEIGHTED_RESPONSE: return "weighted_response";
    case COCOON_LB_CONSISTENT_HASH:   return "consistent_hash";
    case COCOON_LB_RANDOM:            return "random";
    default:                          return "unknown";
    }
}

/* ===== 内部辅助函数实现 ===== */

/**
 * @brief 统计健康后端数量
 */
static size_t count_healthy_backends(cocoon_proxy_rule_t *rule) {
    size_t count = 0;
    for (size_t i = 0; i < rule->backend_count; i++) {
        if (rule->backends[i].healthy) {
            count++;
        }
    }
    return count;
}

/**
 * @brief 最少连接算法
 *
 * 遍历所有健康后端，选择 active_connections 最小者。
 * 如果多个后端连接数相同，选择第一个（保持确定性）。
 */
static int select_least_connections(cocoon_load_balancer_t *lb,
                                    cocoon_proxy_rule_t *rule) {
    int best_idx = -1;
    uint32_t min_conns = UINT32_MAX;

    for (size_t i = 0; i < rule->backend_count; i++) {
        if (!rule->backends[i].healthy) continue;

        uint32_t conns = lb->stats[i].active_connections;
        if (conns < min_conns) {
            min_conns = conns;
            best_idx = (int)i;
        }
    }

    return best_idx;
}

/**
 * @brief 加权响应时间算法 (EWMA)
 *
 * 选择 ewma_response_time_us / weight 比值最小的健康后端。
 * 后端的 weight 越高，可以承受更高的 EWMA 仍被选中。
 * 如果后端没有 EWMA 记录（首次请求），视为 0 优先选择。
 */
static int select_weighted_response(cocoon_load_balancer_t *lb,
                                    cocoon_proxy_rule_t *rule) {
    int best_idx = -1;
    double best_score = 1e308; /**< 初始化为极大值 */

    for (size_t i = 0; i < rule->backend_count; i++) {
        if (!rule->backends[i].healthy) continue;

        uint32_t weight = rule->backends[i].weight;
        if (weight == 0) weight = 1; /**< 防止除零 */

        uint64_t ewma = lb->stats[i].ewma_response_time_us;

        /**
         * 评分 = EWMA / weight
         * weight 越大，相同 EWMA 下评分越低（越优先）。
         */
        double score = (double)ewma / (double)weight;

        if (score < best_score) {
            best_score = score;
            best_idx = (int)i;
        }
    }

    return best_idx;
}

/**
 * @brief 一致性哈希算法
 *
 * 如果提供了 hash_key，使用 MurmurHash3 哈希后从环中选取；
 * 如果 hash_key 为 NULL 或哈希环未初始化，回退到随机选择。
 */
static int select_consistent_hash(cocoon_load_balancer_t *lb,
                                  cocoon_proxy_rule_t *rule,
                                  const char *hash_key) {
    /**
     * 如果哈希环未初始化，尝试构建。
     */
    if (!lb->hash_ring.initialized) {
        lb_build_hash_ring(&lb->hash_ring, rule);
    }

    if (!lb->hash_ring.initialized || lb->hash_ring.node_count == 0) {
        return select_random(rule);
    }

    /**
     * 如果没有提供 hash_key，回退到随机选择。
     * 这样保证在无 key 场景下仍有可用后端。
     */
    if (!hash_key || hash_key[0] == '\0') {
        return select_random(rule);
    }

    uint32_t h = lb_hash_key(hash_key, strlen(hash_key));
    size_t idx = lb_pick_from_ring(&lb->hash_ring, h);

    /**
     * 确保选中的后端是健康的。如果该后端不健康，
     * 回退到随机选择（简单处理，生产环境可改为查找下一个节点）。
     */
    if (idx < rule->backend_count && rule->backends[idx].healthy) {
        return (int)idx;
    }

    return select_random(rule);
}

/**
 * @brief 随机算法
 *
 * 在健康后端中均匀随机选择一个。
 * 使用标准库的 rand()，调用者应事先 srand()。
 */
static int select_random(cocoon_proxy_rule_t *rule) {
    size_t healthy_count = count_healthy_backends(rule);
    if (healthy_count == 0) return -1;

    /**
     * 生成 [0, healthy_count) 范围内的随机数，
     * 然后映射到第 n 个健康后端。
     */
    size_t pick = (size_t)rand() % healthy_count;

    size_t count = 0;
    for (size_t i = 0; i < rule->backend_count; i++) {
        if (rule->backends[i].healthy) {
            if (count == pick) {
                return (int)i;
            }
            count++;
        }
    }

    return -1; /**< 不应到达此处 */
}

/**
 * @brief 虚拟节点比较函数（qsort 回调）
 *
 * 按 node_hash 升序排列。
 */
static int node_compare(const void *a, const void *b) {
    const cocoon_hash_ring_node_t *na = (const cocoon_hash_ring_node_t *)a;
    const cocoon_hash_ring_node_t *nb = (const cocoon_hash_ring_node_t *)b;

    if (na->node_hash < nb->node_hash) return -1;
    if (na->node_hash > nb->node_hash) return 1;
    return 0;
}

/**
 * @brief 为单个后端生成虚拟节点
 *
 * 每个虚拟节点的键格式为 "<host>:<port>-<replica_idx>"，
 * 通过 MurmurHash3 生成唯一哈希值。
 */
static void generate_virtual_nodes(cocoon_hash_ring_node_t *nodes,
                                   size_t *count,
                                   size_t backend_idx,
                                   const char *host,
                                   uint16_t port,
                                   size_t replicas) {
    for (size_t i = 0; i < replicas; i++) {
        if (*count >= COCOON_HASH_RING_MAX_NODES) break;

        /**
         * 虚拟节点键："hostname:port-replica_index"
         * 格式保证不同后端、不同副本产生不同的哈希值。
         */
        char key[512];
        int n = snprintf(key, sizeof(key), "%s:%u-%zu",
                         host, (unsigned int)port, i);
        if (n < 0 || (size_t)n >= sizeof(key)) continue;

        nodes[*count].node_hash = lb_hash_key(key, (size_t)n);
        nodes[*count].backend_index = backend_idx;
        (*count)++;
    }
}

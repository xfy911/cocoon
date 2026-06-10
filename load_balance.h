/**
 * @file load_balance.h - 分布式负载均衡模块头文件
 * @brief 支持多种负载均衡算法：一致性哈希、最少连接、加权响应时间、随机
 *
 * Phase 4 第二项：多种负载均衡算法
 * 设计为独立模块，可集成到 proxy.c 替换现有轮询逻辑。
 *
 * @author xfy
 */

#ifndef COCOON_LOAD_BALANCE_H
#define COCOON_LOAD_BALANCE_H

#include "proxy.h"
#include "healthcheck.h"
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 负载均衡算法枚举 ===== */
/**
 * @enum cocoon_lb_algorithm_t
 * @brief 支持的负载均衡算法
 */
typedef enum {
    COCOON_LB_ROUND_ROBIN,        /**< 加权轮询（已有平滑加权轮询在 proxy.c 中） */
    COCOON_LB_LEAST_CONNECTIONS,  /**< 最少连接 */
    COCOON_LB_WEIGHTED_RESPONSE,  /**< 加权响应时间 (EWMA) */
    COCOON_LB_CONSISTENT_HASH,    /**< 一致性哈希 */
    COCOON_LB_RANDOM              /**< 随机 */
} cocoon_lb_algorithm_t;

/* ===== 后端状态跟踪 ===== */
/**
 * @struct cocoon_backend_stats_t
 * @brief 后端实时统计信息
 *
 * 记录每个后端的连接数、请求数、响应时间等关键指标，
 * 为负载均衡算法提供决策依据。
 */
typedef struct {
    uint32_t active_connections;      /**< 当前活跃连接数 */
    uint32_t total_requests;          /**< 总请求数 */
    uint32_t total_failures;          /**< 总失败数 */
    uint64_t total_response_time_us;  /**< 总响应时间（微秒） */
    uint64_t last_response_time_us;   /**< 上次响应时间 */
    uint64_t ewma_response_time_us;   /**< 指数加权移动平均响应时间 */
} cocoon_backend_stats_t;

/* ===== 一致性哈希环 ===== */
/**
 * @def COCOON_HASH_RING_SIZE
 * @brief 每个后端生成的虚拟节点数量
 */
#define COCOON_HASH_RING_SIZE 512

/**
 * @def COCOON_HASH_RING_MAX_NODES
 * @brief 哈希环最大节点总数
 */
#define COCOON_HASH_RING_MAX_NODES (COCOON_HASH_RING_SIZE * COCOON_MAX_PROXY_BACKENDS)

/**
 * @struct cocoon_hash_ring_node_t
 * @brief 哈希环上的单个虚拟节点
 */
typedef struct {
    uint32_t node_hash;     /**< 虚拟节点哈希值 */
    size_t   backend_index; /**< 指向的后端索引 */
} cocoon_hash_ring_node_t;

/**
 * @struct cocoon_hash_ring_t
 * @brief 一致性哈希环
 *
 * 虚拟节点按哈希值升序排列，支持二分查找。
 */
typedef struct {
    cocoon_hash_ring_node_t nodes[COCOON_HASH_RING_MAX_NODES]; /**< 排序后的虚拟节点数组 */
    size_t node_count;   /**< 实际节点数量 */
    bool   initialized;  /**< 是否已初始化 */
} cocoon_hash_ring_t;

/* ===== 负载均衡器 ===== */
/**
 * @struct cocoon_load_balancer_t
 * @brief 负载均衡器主结构体
 *
 * 包含算法选择、后端统计、哈希环和线程安全锁。
 */
typedef struct {
    cocoon_lb_algorithm_t algorithm;                       /**< 当前使用的算法 */
    cocoon_backend_stats_t stats[COCOON_MAX_PROXY_BACKENDS]; /**< 各后端统计 */
    cocoon_hash_ring_t hash_ring;                          /**< 一致性哈希环 */
    uint32_t alpha;     /**< EWMA 平滑因子（默认 80，即 0.8） */
    pthread_mutex_t mutex; /**< 统计更新锁（线程安全） */
} cocoon_load_balancer_t;

/* ===== API ===== */

/**
 * @brief 初始化负载均衡器
 *
 * 设置算法类型，初始化统计数组和互斥锁。
 * 如果算法为 CONSISTENT_HASH，不会自动构建哈希环，
 * 需要显式调用 lb_build_hash_ring()。
 *
 * @param lb   负载均衡器指针
 * @param algo 负载均衡算法
 */
void lb_init(cocoon_load_balancer_t *lb, cocoon_lb_algorithm_t algo);

/**
 * @brief 销毁负载均衡器
 *
 * 销毁互斥锁，清空所有状态。
 *
 * @param lb 负载均衡器指针
 */
void lb_destroy(cocoon_load_balancer_t *lb);

/**
 * @brief 根据算法选择后端索引
 *
 * 根据配置的负载均衡算法，从规则的健康后端中选择一个。
 * 对于 CONSISTENT_HASH 算法，需要提供 hash_key；
 * 如果 hash_key 为 NULL，则回退到随机选择。
 *
 * @param lb       负载均衡器
 * @param rule     代理规则（包含后端列表）
 * @param hash_key 一致性哈希的键（如请求路径或客户端 IP），可为 NULL
 * @return 选择的后端索引，无可用后端返回 -1
 */
int lb_select_backend(cocoon_load_balancer_t *lb, cocoon_proxy_rule_t *rule,
                      const char *hash_key);

/**
 * @brief 请求开始时更新统计
 *
 * 增加指定后端的活跃连接数和总请求数。
 * 线程安全：内部加锁。
 *
 * @param lb          负载均衡器
 * @param backend_idx 后端索引
 */
void lb_update_stats_request_start(cocoon_load_balancer_t *lb, size_t backend_idx);

/**
 * @brief 请求结束时更新统计
 *
 * 减少活跃连接数，更新 EWMA 响应时间，记录成功/失败。
 * 线程安全：内部加锁。
 *
 * @param lb              负载均衡器
 * @param backend_idx     后端索引
 * @param success         请求是否成功
 * @param response_time_us 响应时间（微秒）
 */
void lb_update_stats_request_end(cocoon_load_balancer_t *lb, size_t backend_idx,
                                 bool success, uint64_t response_time_us);

/**
 * @brief 构建一致性哈希环
 *
 * 为规则中的所有健康后端生成虚拟节点并排序。
 * 应在后端配置变更或健康状态变化时重新构建。
 *
 * @param ring 哈希环
 * @param rule 代理规则
 */
void lb_build_hash_ring(cocoon_hash_ring_t *ring, cocoon_proxy_rule_t *rule);

/**
 * @brief MurmurHash3 x86 32-bit 哈希函数
 *
 * 纯软件实现，不依赖外部库。
 * 用于一致性哈希的键哈希。
 *
 * @param key 输入键
 * @param len 键长度
 * @return 32-bit 哈希值
 */
uint32_t lb_hash_key(const char *key, size_t len);

/**
 * @brief 从哈希环中选取后端
 *
 * 使用二分查找定位最近的虚拟节点，支持哈希环环绕。
 *
 * @param ring 已构建的哈希环
 * @param hash 要查找的哈希值
 * @return 选中的后端索引
 */
size_t lb_pick_from_ring(cocoon_hash_ring_t *ring, uint32_t hash);

/**
 * @brief 获取算法名称字符串
 *
 * @param algo 负载均衡算法
 * @return 算法名称（静态字符串）
 */
const char *lb_algorithm_name(cocoon_lb_algorithm_t algo);

#ifdef __cplusplus
}
#endif

#endif /* COCOON_LOAD_BALANCE_H */

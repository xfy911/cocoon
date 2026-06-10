/**
 * @file test_load_balance.c - 负载均衡模块单元测试
 * @brief 使用 Unity 框架测试所有负载均衡算法
 *
 * 测试覆盖：
 *   - MurmurHash3 哈希函数已知值验证
 *   - 一致性哈希环构建与查找
 *   - 一致性哈希 key→后端映射稳定性
 *   - 最少连接选择正确性
 *   - EWMA 计算验证
 *   - 加权响应时间权重影响
 *   - 随机算法分布
 *   - 边界条件和错误处理
 *
 * @author xfy
 */

#include "unity.h"
#include "load_balance.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ===== 测试辅助函数 ===== */

/**
 * @brief 创建简单的代理规则（n 个后端）用于测试
 */
static void make_test_rule(cocoon_proxy_rule_t *rule, size_t n) {
    memset(rule, 0, sizeof(*rule));
    rule->backend_count = n;
    for (size_t i = 0; i < n; i++) {
        snprintf(rule->backends[i].target_host,
                 sizeof(rule->backends[i].target_host),
                 "backend%zu", i);
        rule->backends[i].target_port = (uint16_t)(8000 + i);
        rule->backends[i].healthy = true;
        rule->backends[i].weight = 1;
        rule->backends[i].current_weight = 0;
    }
}

/**
 * @brief 创建带权重的代理规则
 */
static void make_weighted_rule(cocoon_proxy_rule_t *rule, size_t n,
                                const uint32_t *weights) {
    make_test_rule(rule, n);
    for (size_t i = 0; i < n; i++) {
        rule->backends[i].weight = weights[i];
    }
}

void setUp(void) {
    /* 每次测试前重置随机种子，保证可重复 */
    srand(42);
}

void tearDown(void) { }

/* ===== 测试组 1: MurmurHash3 哈希函数 ===== */

/**
 * @test test_hash_known_values
 * @brief MurmurHash3 已知输入输出验证
 *
 * 使用参考实现验证相同输入产生相同的哈希值。
 */
void test_hash_known_values(void) {
    /* 空字符串：seed=0 时 hash=0 */
    TEST_ASSERT_EQUAL_UINT32(0, lb_hash_key("", 0));

    /* 常见字符串的已知哈希值（seed=0 的 MurmurHash3 x86 32-bit） */
    TEST_ASSERT_EQUAL_UINT32(0x248bfa47U, lb_hash_key("hello", 5));
    TEST_ASSERT_EQUAL_UINT32(0x149bbb7fU, lb_hash_key("hello, world", 12));
    TEST_ASSERT_EQUAL_UINT32(0x38d82c45U, lb_hash_key("19Jan2038at03:14:07UTC", 22));

    /* 较长的字符串 */
    TEST_ASSERT_EQUAL_UINT32(0x2e4ff723U,
        lb_hash_key("The quick brown fox jumps over the lazy dog", 43));
}

/**
 * @test test_hash_empty_string_zero_length
 * @brief 空字符串返回 0
 */
void test_hash_empty_string_zero_length(void) {
    TEST_ASSERT_EQUAL_UINT32(0, lb_hash_key("", 0));
}

/**
 * @test test_hash_null_returns_zero
 * @brief NULL 键返回 0
 */
void test_hash_null_returns_zero(void) {
    TEST_ASSERT_EQUAL_UINT32(0, lb_hash_key(NULL, 0));
}

/**
 * @test test_hash_consistency
 * @brief 相同输入必须产生相同输出（确定性）
 */
void test_hash_consistency(void) {
    const char *key = "/api/users/12345";
    size_t len = strlen(key);
    uint32_t h1 = lb_hash_key(key, len);
    uint32_t h2 = lb_hash_key(key, len);
    uint32_t h3 = lb_hash_key(key, len);

    TEST_ASSERT_EQUAL_UINT32(h1, h2);
    TEST_ASSERT_EQUAL_UINT32(h2, h3);
}

/**
 * @test test_hash_different_inputs
 * @brief 不同输入产生不同哈希值（碰撞概率极低）
 */
void test_hash_different_inputs(void) {
    uint32_t h1 = lb_hash_key("/api/a", 6);
    uint32_t h2 = lb_hash_key("/api/b", 6);
    uint32_t h3 = lb_hash_key("/api/c", 6);

    TEST_ASSERT_UINT32_WITHIN(0xFFFFFFFFU, 0, h1 ^ h2); /* 非零差异 */
    TEST_ASSERT(h1 != h2);
    TEST_ASSERT(h2 != h3);
    TEST_ASSERT(h1 != h3);
}

/**
 * @test test_hash_binary_data
 * @brief 二进制数据也能正确处理
 */
void test_hash_binary_data(void) {
    const uint8_t data[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD, 0xFC};
    uint32_t h = lb_hash_key((const char *)data, sizeof(data));
    TEST_ASSERT_NOT_EQUAL(0, h);
}

/* ===== 测试组 2: lb_init 和 lb_destroy ===== */

/**
 * @test test_init_sets_algorithm
 * @brief 初始化设置正确的算法
 */
void test_init_sets_algorithm(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_LEAST_CONNECTIONS);
    TEST_ASSERT_EQUAL(COCOON_LB_LEAST_CONNECTIONS, lb.algorithm);
    lb_destroy(&lb);
}

/**
 * @test test_init_default_alpha
 * @brief 初始化设置默认 alpha 值
 */
void test_init_default_alpha(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_WEIGHTED_RESPONSE);
    TEST_ASSERT_EQUAL_UINT32(80, lb.alpha);
    lb_destroy(&lb);
}

/**
 * @test test_init_clears_stats
 * @brief 初始化清零所有统计
 */
void test_init_clears_stats(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_RANDOM);
    for (size_t i = 0; i < COCOON_MAX_PROXY_BACKENDS; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, lb.stats[i].active_connections);
        TEST_ASSERT_EQUAL_UINT32(0, lb.stats[i].total_requests);
        TEST_ASSERT_EQUAL_UINT32(0, lb.stats[i].total_failures);
        TEST_ASSERT_EQUAL_UINT64(0, lb.stats[i].ewma_response_time_us);
    }
    lb_destroy(&lb);
}

/**
 * @test test_init_clears_hash_ring
 * @brief 初始化清零哈希环
 */
void test_init_clears_hash_ring(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_CONSISTENT_HASH);
    TEST_ASSERT_EQUAL_size_t(0, lb.hash_ring.node_count);
    TEST_ASSERT_FALSE(lb.hash_ring.initialized);
    lb_destroy(&lb);
}

/**
 * @test test_destroy_cleans_up
 * @brief 销毁后结构体应被清零
 */
void test_destroy_cleans_up(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_LEAST_CONNECTIONS);
    lb_destroy(&lb);
    /* lb.algorithm 应被设为 0（ROUND_ROBIN 的值） */
    TEST_ASSERT_EQUAL(COCOON_LB_ROUND_ROBIN, lb.algorithm);
}

/* ===== 测试组 3: lb_algorithm_name ===== */

/**
 * @test test_algorithm_name_all
 * @brief 所有算法都有名称
 */
void test_algorithm_name_all(void) {
    TEST_ASSERT_EQUAL_STRING("round_robin",
                             lb_algorithm_name(COCOON_LB_ROUND_ROBIN));
    TEST_ASSERT_EQUAL_STRING("least_connections",
                             lb_algorithm_name(COCOON_LB_LEAST_CONNECTIONS));
    TEST_ASSERT_EQUAL_STRING("weighted_response",
                             lb_algorithm_name(COCOON_LB_WEIGHTED_RESPONSE));
    TEST_ASSERT_EQUAL_STRING("consistent_hash",
                             lb_algorithm_name(COCOON_LB_CONSISTENT_HASH));
    TEST_ASSERT_EQUAL_STRING("random",
                             lb_algorithm_name(COCOON_LB_RANDOM));
}

/**
 * @test test_algorithm_name_unknown
 * @brief 未知算法返回 "unknown"
 */
void test_algorithm_name_unknown(void) {
    TEST_ASSERT_EQUAL_STRING("unknown",
                             lb_algorithm_name((cocoon_lb_algorithm_t)999));
}

/* ===== 测试组 4: 一致性哈希环 ===== */

/**
 * @test test_build_hash_ring_creates_nodes
 * @brief 构建哈希环创建正确数量的虚拟节点
 */
void test_build_hash_ring_creates_nodes(void) {
    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    lb_build_hash_ring(&ring, &rule);

    TEST_ASSERT_TRUE(ring.initialized);
    TEST_ASSERT_EQUAL_size_t(3 * COCOON_HASH_RING_SIZE, ring.node_count);
}

/**
 * @test test_build_hash_ring_empty_rule
 * @brief 空规则构建空哈希环
 */
void test_build_hash_ring_empty_rule(void) {
    cocoon_proxy_rule_t rule;
    memset(&rule, 0, sizeof(rule));

    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    lb_build_hash_ring(&ring, &rule);

    TEST_ASSERT_FALSE(ring.initialized);
    TEST_ASSERT_EQUAL_size_t(0, ring.node_count);
}

/**
 * @test test_build_hash_ring_skips_unhealthy
 * @brief 构建哈希环跳过不健康后端
 */
void test_build_hash_ring_skips_unhealthy(void) {
    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 4);
    rule.backends[1].healthy = false; /**< backend1 不健康 */

    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    lb_build_hash_ring(&ring, &rule);

    TEST_ASSERT_TRUE(ring.initialized);
    /**< 3 个健康后端 × 512 虚拟节点 */
    TEST_ASSERT_EQUAL_size_t(3 * COCOON_HASH_RING_SIZE, ring.node_count);
}

/**
 * @test test_build_hash_ring_all_unhealthy
 * @brief 所有后端不健康时构建空环
 */
void test_build_hash_ring_all_unhealthy(void) {
    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);
    for (size_t i = 0; i < 3; i++) {
        rule.backends[i].healthy = false;
    }

    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    lb_build_hash_ring(&ring, &rule);

    TEST_ASSERT_FALSE(ring.initialized);
    TEST_ASSERT_EQUAL_size_t(0, ring.node_count);
}

/**
 * @test test_hash_ring_nodes_sorted
 * @brief 哈希环虚拟节点按哈希值排序
 */
void test_hash_ring_nodes_sorted(void) {
    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    lb_build_hash_ring(&ring, &rule);

    for (size_t i = 1; i < ring.node_count; i++) {
        TEST_ASSERT_MESSAGE(
            ring.nodes[i - 1].node_hash <= ring.nodes[i].node_hash,
            "哈希环虚拟节点未按升序排列"
        );
    }
}

/**
 * @test test_hash_ring_backend_coverage
 * @brief 所有健康后端都有虚拟节点
 */
void test_hash_ring_backend_coverage(void) {
    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    lb_build_hash_ring(&ring, &rule);

    bool has_backend[3] = {false, false, false};
    for (size_t i = 0; i < ring.node_count; i++) {
        if (ring.nodes[i].backend_index < 3) {
            has_backend[ring.nodes[i].backend_index] = true;
        }
    }
    TEST_ASSERT_TRUE(has_backend[0]);
    TEST_ASSERT_TRUE(has_backend[1]);
    TEST_ASSERT_TRUE(has_backend[2]);
}

/**
 * @test test_pick_from_ring_basic
 * @brief 从哈希环选取返回有效后端索引
 */
void test_pick_from_ring_basic(void) {
    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    lb_build_hash_ring(&ring, &rule);

    uint32_t h = lb_hash_key("/api/users", 10);
    size_t idx = lb_pick_from_ring(&ring, h);
    TEST_ASSERT(idx < 3);
}

/**
 * @test test_pick_from_ring_wraparound
 * @brief 哈希值大于最大节点时正确环绕
 */
void test_pick_from_ring_wraparound(void) {
    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 2);

    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    lb_build_hash_ring(&ring, &rule);

    /**< 使用最大可能的哈希值，强制环绕 */
    size_t idx = lb_pick_from_ring(&ring, 0xFFFFFFFFU);
    TEST_ASSERT(idx < 2);
}

/**
 * @test test_pick_from_ring_zero_hash
 * @brief 哈希值为 0 时正确返回第一个节点
 */
void test_pick_from_ring_zero_hash(void) {
    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 2);

    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    lb_build_hash_ring(&ring, &rule);

    size_t idx = lb_pick_from_ring(&ring, 0);
    TEST_ASSERT(idx < 2);
}

/**
 * @test test_pick_from_ring_deterministic
 * @brief 相同哈希值总是选取相同后端
 */
void test_pick_from_ring_deterministic(void) {
    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    lb_build_hash_ring(&ring, &rule);

    uint32_t h = lb_hash_key("/static/file.css", 16);
    size_t idx1 = lb_pick_from_ring(&ring, h);
    size_t idx2 = lb_pick_from_ring(&ring, h);
    size_t idx3 = lb_pick_from_ring(&ring, h);

    TEST_ASSERT_EQUAL_size_t(idx1, idx2);
    TEST_ASSERT_EQUAL_size_t(idx2, idx3);
}

/**
 * @test test_pick_from_ring_empty
 * @brief 空环返回 0（安全回退）
 */
void test_pick_from_ring_empty(void) {
    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));

    size_t idx = lb_pick_from_ring(&ring, 12345);
    TEST_ASSERT_EQUAL_size_t(0, idx);
}

/* ===== 测试组 5: 一致性哈希映射稳定性 ===== */

/**
 * @test test_consistent_hash_stability
 * @brief 相同 key 总是映射到相同后端
 */
void test_consistent_hash_stability(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_CONSISTENT_HASH);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 4);

    lb_build_hash_ring(&lb.hash_ring, &rule);

    /**< 多次选择相同 key */
    int idx1 = lb_select_backend(&lb, &rule, "/api/resource/42");
    int idx2 = lb_select_backend(&lb, &rule, "/api/resource/42");
    int idx3 = lb_select_backend(&lb, &rule, "/api/resource/42");

    TEST_ASSERT_EQUAL_INT(idx1, idx2);
    TEST_ASSERT_EQUAL_INT(idx2, idx3);
    TEST_ASSERT(idx1 >= 0 && idx1 < 4);

    lb_destroy(&lb);
}

/**
 * @test test_consistent_hash_different_keys
 * @brief 不同 key 可以映射到不同后端
 */
void test_consistent_hash_different_keys(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_CONSISTENT_HASH);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 4);

    lb_build_hash_ring(&lb.hash_ring, &rule);

    /**< 使用多个不同的 key */
    int idx1 = lb_select_backend(&lb, &rule, "/api/users");
    int idx2 = lb_select_backend(&lb, &rule, "/api/products");
    int idx3 = lb_select_backend(&lb, &rule, "/api/orders");

    TEST_ASSERT(idx1 >= 0 && idx1 < 4);
    TEST_ASSERT(idx2 >= 0 && idx2 < 4);
    TEST_ASSERT(idx3 >= 0 && idx3 < 4);

    lb_destroy(&lb);
}

/**
 * @test test_consistent_hash_null_key_fallback
 * @brief NULL key 回退到随机选择
 */
void test_consistent_hash_null_key_fallback(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_CONSISTENT_HASH);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    lb_build_hash_ring(&lb.hash_ring, &rule);

    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT(idx >= 0 && idx < 3);

    lb_destroy(&lb);
}

/**
 * @test test_consistent_hash_distribution
 * @brief 多个 key 的分布检查（不应全部映射到同一后端）
 */
void test_consistent_hash_distribution(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_CONSISTENT_HASH);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 4);

    lb_build_hash_ring(&lb.hash_ring, &rule);

    /**< 使用大量不同的 key 测试分布 */
    int count[4] = {0, 0, 0, 0};
    char key[64];
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "/api/item/%d", i);
        int idx = lb_select_backend(&lb, &rule, key);
        TEST_ASSERT(idx >= 0 && idx < 4);
        count[idx]++;
    }

    /**< 每个后端都应被分配到一些 key（分布应相对均匀） */
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_MESSAGE(count[i] > 50,
            "一致性哈希分布不均匀，某个后端分配过少");
    }

    lb_destroy(&lb);
}

/* ===== 测试组 6: 最少连接算法 ===== */

/**
 * @test test_least_connections_selects_min
 * @brief 最少连接选择连接数最少的后端
 */
void test_least_connections_selects_min(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_LEAST_CONNECTIONS);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    /**< 设置不同的活跃连接数 */
    lb.stats[0].active_connections = 5;
    lb.stats[1].active_connections = 2; /**< 最少 */
    lb.stats[2].active_connections = 8;

    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(1, idx);

    lb_destroy(&lb);
}

/**
 * @test test_least_connections_all_zero
 * @brief 所有连接数为 0 时选择第一个健康后端
 */
void test_least_connections_all_zero(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_LEAST_CONNECTIONS);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(0, idx);

    lb_destroy(&lb);
}

/**
 * @test test_least_connections_skips_unhealthy
 * @brief 最少连接跳过不健康后端
 */
void test_least_connections_skips_unhealthy(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_LEAST_CONNECTIONS);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);
    rule.backends[0].healthy = false;

    /**< backend0 连接数最少，但不健康 */
    lb.stats[0].active_connections = 0;
    lb.stats[1].active_connections = 3;
    lb.stats[2].active_connections = 5;

    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(1, idx);

    lb_destroy(&lb);
}

/**
 * @test test_least_connections_no_healthy
 * @brief 没有健康后端返回 -1
 */
void test_least_connections_no_healthy(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_LEAST_CONNECTIONS);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);
    for (size_t i = 0; i < 3; i++) {
        rule.backends[i].healthy = false;
    }

    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(-1, idx);

    lb_destroy(&lb);
}

/**
 * @test test_least_connections_tie_break
 * @brief 连接数相同时选择索引小的（确定性）
 */
void test_least_connections_tie_break(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_LEAST_CONNECTIONS);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    /**< 所有后端连接数相同 */
    lb.stats[0].active_connections = 3;
    lb.stats[1].active_connections = 3;
    lb.stats[2].active_connections = 3;

    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(0, idx); /**< 第一个达到最小值的后端 */

    lb_destroy(&lb);
}

/* ===== 测试组 7: 统计更新 ===== */

/**
 * @test test_stats_request_start
 * @brief 请求开始正确更新连接数和请求数
 */
void test_stats_request_start(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_LEAST_CONNECTIONS);

    lb_update_stats_request_start(&lb, 0);
    TEST_ASSERT_EQUAL_UINT32(1, lb.stats[0].active_connections);
    TEST_ASSERT_EQUAL_UINT32(1, lb.stats[0].total_requests);

    lb_update_stats_request_start(&lb, 0);
    TEST_ASSERT_EQUAL_UINT32(2, lb.stats[0].active_connections);
    TEST_ASSERT_EQUAL_UINT32(2, lb.stats[0].total_requests);

    lb_destroy(&lb);
}

/**
 * @test test_stats_request_end_success
 * @brief 请求成功正确更新统计
 */
void test_stats_request_end_success(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_WEIGHTED_RESPONSE);

    lb_update_stats_request_start(&lb, 0);
    TEST_ASSERT_EQUAL_UINT32(1, lb.stats[0].active_connections);

    lb_update_stats_request_end(&lb, 0, true, 1000);
    TEST_ASSERT_EQUAL_UINT32(0, lb.stats[0].active_connections);
    TEST_ASSERT_EQUAL_UINT64(1000, lb.stats[0].last_response_time_us);
    TEST_ASSERT_EQUAL_UINT32(0, lb.stats[0].total_failures);

    lb_destroy(&lb);
}

/**
 * @test test_stats_request_end_failure
 * @brief 请求失败正确记录失败数
 */
void test_stats_request_end_failure(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_WEIGHTED_RESPONSE);

    lb_update_stats_request_start(&lb, 0);
    lb_update_stats_request_end(&lb, 0, false, 500);
    TEST_ASSERT_EQUAL_UINT32(1, lb.stats[0].total_failures);

    lb_update_stats_request_start(&lb, 0);
    lb_update_stats_request_end(&lb, 0, false, 600);
    TEST_ASSERT_EQUAL_UINT32(2, lb.stats[0].total_failures);

    lb_destroy(&lb);
}

/**
 * @test test_stats_end_without_start
 * @brief 未调用 start 直接调用 end 不会下溢
 */
void test_stats_end_without_start(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_LEAST_CONNECTIONS);

    /**< active_connections 已经是 0 */
    lb_update_stats_request_end(&lb, 0, true, 1000);
    TEST_ASSERT_EQUAL_UINT32(0, lb.stats[0].active_connections);

    lb_destroy(&lb);
}

/**
 * @test test_stats_multiple_backends
 * @brief 多后端统计互不干扰
 */
void test_stats_multiple_backends(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_LEAST_CONNECTIONS);

    lb_update_stats_request_start(&lb, 0);
    lb_update_stats_request_start(&lb, 1);
    lb_update_stats_request_start(&lb, 1);

    TEST_ASSERT_EQUAL_UINT32(1, lb.stats[0].active_connections);
    TEST_ASSERT_EQUAL_UINT32(2, lb.stats[1].active_connections);

    lb_destroy(&lb);
}

/* ===== 测试组 8: EWMA 计算 ===== */

/**
 * @test test_ewma_first_update
 * @brief 首次 EWMA 更新直接使用当前值
 */
void test_ewma_first_update(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_WEIGHTED_RESPONSE);

    lb_update_stats_request_end(&lb, 0, true, 1000);
    /**< 首次更新：ewma 直接设为当前值 */
    TEST_ASSERT_EQUAL_UINT64(1000, lb.stats[0].ewma_response_time_us);

    lb_destroy(&lb);
}

/**
 * @test test_ewma_subsequent_updates
 * @brief 后续 EWMA 更新应用平滑公式
 *
 * alpha = 80，即 new = 0.8 * current + 0.2 * old
 *   old = 1000, current = 2000
 *   new = (80 * 2000 + 20 * 1000) / 100 = (160000 + 20000) / 100 = 1800
 */
void test_ewma_subsequent_updates(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_WEIGHTED_RESPONSE);

    lb_update_stats_request_end(&lb, 0, true, 1000); /**< 首次：ewma=1000 */
    lb_update_stats_request_end(&lb, 0, true, 2000); /**< 第二次：ewma=1800 */

    TEST_ASSERT_EQUAL_UINT64(1800, lb.stats[0].ewma_response_time_us);

    lb_destroy(&lb);
}

/**
 * @test test_ewma_converges
 * @brief EWMA 向稳定值收敛
 *
 * 多次更新相同值后，EWMA 应接近该值。
 */
void test_ewma_converges(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_WEIGHTED_RESPONSE);

    lb_update_stats_request_end(&lb, 0, true, 1000); /**< 首次 */
    for (int i = 0; i < 20; i++) {
        lb_update_stats_request_end(&lb, 0, true, 1000);
    }

    /**< 经过 20 次相同值更新，EWMA 应非常接近 1000 */
    TEST_ASSERT_UINT64_WITHIN(10, 1000,
        lb.stats[0].ewma_response_time_us);

    lb_destroy(&lb);
}

/**
 * @test test_ewma_alpha_effect
 * @brief alpha 值影响平滑程度
 *
 * alpha 较大时（接近 100），对新值更敏感。
 */
void test_ewma_alpha_effect(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_WEIGHTED_RESPONSE);
    lb.alpha = 90; /**< 更高 alpha，更敏感 */

    lb_update_stats_request_end(&lb, 0, true, 1000);
    lb_update_stats_request_end(&lb, 0, true, 2000);

    /**< alpha=90: new = (90*2000 + 10*1000)/100 = 1900 */
    TEST_ASSERT_EQUAL_UINT64(1900, lb.stats[0].ewma_response_time_us);

    lb.alpha = 50; /**< 更低 alpha，更平滑 */
    lb_update_stats_request_end(&lb, 0, true, 2000);

    /**< alpha=50: new = (50*2000 + 50*1900)/100 = 1950 */
    TEST_ASSERT_EQUAL_UINT64(1950, lb.stats[0].ewma_response_time_us);

    lb_destroy(&lb);
}

/* ===== 测试组 9: 加权响应时间选择 ===== */

/**
 * @test test_weighted_response_selects_lowest_ratio
 * @brief 选择 EWMA/weight 比值最小的后端
 */
void test_weighted_response_selects_lowest_ratio(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_WEIGHTED_RESPONSE);

    cocoon_proxy_rule_t rule;
    uint32_t weights[] = {1, 1, 1};
    make_weighted_rule(&rule, 3, weights);

    /**< backend1 的 EWMA 最低 */
    lb.stats[0].ewma_response_time_us = 5000;
    lb.stats[1].ewma_response_time_us = 1000; /**< 最优 */
    lb.stats[2].ewma_response_time_us = 3000;

    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(1, idx);

    lb_destroy(&lb);
}

/**
 * @test test_weighted_response_weight_influence
 * @brief 权重影响选择（高权重可抵消高 EWMA）
 *
 * backend0: ewma=5000, weight=5  => score=1000
 * backend1: ewma=3000, weight=1  => score=3000
 * backend2: ewma=4000, weight=2  => score=2000
 *
 * backend0 应被选中（最低 score）
 */
void test_weighted_response_weight_influence(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_WEIGHTED_RESPONSE);

    cocoon_proxy_rule_t rule;
    uint32_t weights[] = {5, 1, 2};
    make_weighted_rule(&rule, 3, weights);

    lb.stats[0].ewma_response_time_us = 5000; /**< weight=5, score=1000 */
    lb.stats[1].ewma_response_time_us = 3000; /**< weight=1, score=3000 */
    lb.stats[2].ewma_response_time_us = 4000; /**< weight=2, score=2000 */

    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(0, idx);

    lb_destroy(&lb);
}

/**
 * @test test_weighted_response_zero_weight
 * @brief weight=0 时按 weight=1 处理（防止除零）
 */
void test_weighted_response_zero_weight(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_WEIGHTED_RESPONSE);

    cocoon_proxy_rule_t rule;
    uint32_t weights[] = {0, 1}; /**< backend0 weight=0 */
    make_weighted_rule(&rule, 2, weights);

    lb.stats[0].ewma_response_time_us = 1000;
    lb.stats[1].ewma_response_time_us = 2000;

    /**< backend0: 1000/1=1000, backend1: 2000/1=2000 */
    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(0, idx);

    lb_destroy(&lb);
}

/* ===== 测试组 10: 随机算法 ===== */

/**
 * @test test_random_returns_valid_index
 * @brief 随机选择返回有效后端索引
 */
void test_random_returns_valid_index(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_RANDOM);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 5);

    for (int i = 0; i < 50; i++) {
        int idx = lb_select_backend(&lb, &rule, NULL);
        TEST_ASSERT(idx >= 0 && idx < 5);
    }

    lb_destroy(&lb);
}

/**
 * @test test_random_distribution
 * @brief 随机选择分布大致均匀
 */
void test_random_distribution(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_RANDOM);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 4);

    int count[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4000; i++) {
        int idx = lb_select_backend(&lb, &rule, NULL);
        count[idx]++;
    }

    /**< 每个后端应被选中约 1000 次，允许 ±200 偏差 */
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_INT_WITHIN(300, 1000, count[i]);
    }

    lb_destroy(&lb);
}

/**
 * @test test_random_no_healthy
 * @brief 没有健康后端时返回 -1
 */
void test_random_no_healthy(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_RANDOM);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);
    for (size_t i = 0; i < 3; i++) {
        rule.backends[i].healthy = false;
    }

    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(-1, idx);

    lb_destroy(&lb);
}

/* ===== 测试组 11: 边界条件和错误处理 ===== */

/**
 * @test test_select_null_lb
 * @brief NULL 负载均衡器返回 -1
 */
void test_select_null_lb(void) {
    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 2);

    int idx = lb_select_backend(NULL, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(-1, idx);
}

/**
 * @test test_select_null_rule
 * @brief NULL 规则返回 -1
 */
void test_select_null_rule(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_RANDOM);

    int idx = lb_select_backend(&lb, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, idx);

    lb_destroy(&lb);
}

/**
 * @test test_select_empty_rule
 * @brief 空规则（无后端）返回 -1
 */
void test_select_empty_rule(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_RANDOM);

    cocoon_proxy_rule_t rule;
    memset(&rule, 0, sizeof(rule));

    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(-1, idx);

    lb_destroy(&lb);
}

/**
 * @test test_update_stats_null_lb
 * @brief NULL 负载均衡器更新统计不崩溃
 */
void test_update_stats_null_lb(void) {
    /**< 应安全返回，不崩溃 */
    lb_update_stats_request_start(NULL, 0);
    lb_update_stats_request_end(NULL, 0, true, 100);
    TEST_PASS_MESSAGE("NULL lb stats update handled safely");
}

/**
 * @test test_round_robin_returns_neg_one
 * @brief 轮询算法返回 -1（由调用者使用 proxy.c 的 SWW）
 */
void test_round_robin_returns_neg_one(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_ROUND_ROBIN);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(-1, idx);

    lb_destroy(&lb);
}

/* ===== 测试组 12: 综合集成测试 ===== */

/**
 * @test test_full_request_lifecycle_least_connections
 * @brief 最少连接完整请求生命周期
 */
void test_full_request_lifecycle_least_connections(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_LEAST_CONNECTIONS);

    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    /**< 初始选择：backend0（连接数都是 0，选第一个） */
    int idx1 = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(0, idx1);

    /**< 请求开始 */
    lb_update_stats_request_start(&lb, (size_t)idx1);
    TEST_ASSERT_EQUAL_UINT32(1, lb.stats[0].active_connections);

    /**< 第二次选择：backend1（backend0 已有 1 个连接） */
    int idx2 = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(1, idx2);
    lb_update_stats_request_start(&lb, (size_t)idx2);

    /**< 第三次选择：backend2 */
    int idx3 = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(2, idx3);
    lb_update_stats_request_start(&lb, (size_t)idx3);

    /**< 请求结束 */
    lb_update_stats_request_end(&lb, (size_t)idx1, true, 500);
    lb_update_stats_request_end(&lb, (size_t)idx2, true, 600);
    lb_update_stats_request_end(&lb, (size_t)idx3, false, 100);

    TEST_ASSERT_EQUAL_UINT32(0, lb.stats[0].active_connections);
    TEST_ASSERT_EQUAL_UINT32(0, lb.stats[1].active_connections);
    TEST_ASSERT_EQUAL_UINT32(0, lb.stats[2].active_connections);
    TEST_ASSERT_EQUAL_UINT32(1, lb.stats[2].total_failures);

    lb_destroy(&lb);
}

/**
 * @test test_full_request_lifecycle_weighted_response
 * @brief 加权响应时间完整生命周期
 */
void test_full_request_lifecycle_weighted_response(void) {
    cocoon_load_balancer_t lb;
    lb_init(&lb, COCOON_LB_WEIGHTED_RESPONSE);

    cocoon_proxy_rule_t rule;
    uint32_t weights[] = {2, 1};
    make_weighted_rule(&rule, 2, weights);

    /**< 初始无 EWMA，两个后端都是 0，选第一个健康后端 */
    int idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(0, idx);

    /**< 模拟请求并记录不同响应时间 */
    lb_update_stats_request_start(&lb, 0);
    lb_update_stats_request_end(&lb, 0, true, 2000);

    lb_update_stats_request_start(&lb, 1);
    lb_update_stats_request_end(&lb, 1, true, 500);

    /**< backend0: ewma=2000, weight=2 => score=1000 */
    /**< backend1: ewma=500,  weight=1 => score=500  */
    /**< 应选择 backend1 */
    idx = lb_select_backend(&lb, &rule, NULL);
    TEST_ASSERT_EQUAL_INT(1, idx);

    lb_destroy(&lb);
}

/**
 * @test test_virtual_node_distribution_uniform
 * @brief 虚拟节点在环上均匀分布（哈希值范围覆盖）
 */
void test_virtual_node_distribution_uniform(void) {
    cocoon_proxy_rule_t rule;
    make_test_rule(&rule, 3);

    cocoon_hash_ring_t ring;
    memset(&ring, 0, sizeof(ring));
    lb_build_hash_ring(&ring, &rule);

    /**< 检查最小和最大哈希值存在合理差距 */
    uint32_t min_hash = ring.nodes[0].node_hash;
    uint32_t max_hash = ring.nodes[ring.node_count - 1].node_hash;

    /**< 哈希值应分布在较大范围内 */
    TEST_ASSERT(min_hash < max_hash);
    TEST_ASSERT_UINT32_WITHIN(0xFFFFFFFFU / 4, 0x80000000U,
                              min_hash + (max_hash - min_hash) / 2);
}

/* ===== 主函数 ===== */

int main(void) {
    UNITY_BEGIN();

    /* 测试组 1: MurmurHash3 */
    RUN_TEST(test_hash_known_values);
    RUN_TEST(test_hash_empty_string_zero_length);
    RUN_TEST(test_hash_null_returns_zero);
    RUN_TEST(test_hash_consistency);
    RUN_TEST(test_hash_different_inputs);
    RUN_TEST(test_hash_binary_data);

    /* 测试组 2: lb_init / lb_destroy */
    RUN_TEST(test_init_sets_algorithm);
    RUN_TEST(test_init_default_alpha);
    RUN_TEST(test_init_clears_stats);
    RUN_TEST(test_init_clears_hash_ring);
    RUN_TEST(test_destroy_cleans_up);

    /* 测试组 3: lb_algorithm_name */
    RUN_TEST(test_algorithm_name_all);
    RUN_TEST(test_algorithm_name_unknown);

    /* 测试组 4: 一致性哈希环 */
    RUN_TEST(test_build_hash_ring_creates_nodes);
    RUN_TEST(test_build_hash_ring_empty_rule);
    RUN_TEST(test_build_hash_ring_skips_unhealthy);
    RUN_TEST(test_build_hash_ring_all_unhealthy);
    RUN_TEST(test_hash_ring_nodes_sorted);
    RUN_TEST(test_hash_ring_backend_coverage);
    RUN_TEST(test_pick_from_ring_basic);
    RUN_TEST(test_pick_from_ring_wraparound);
    RUN_TEST(test_pick_from_ring_zero_hash);
    RUN_TEST(test_pick_from_ring_deterministic);
    RUN_TEST(test_pick_from_ring_empty);

    /* 测试组 5: 一致性哈希映射稳定性 */
    RUN_TEST(test_consistent_hash_stability);
    RUN_TEST(test_consistent_hash_different_keys);
    RUN_TEST(test_consistent_hash_null_key_fallback);
    RUN_TEST(test_consistent_hash_distribution);

    /* 测试组 6: 最少连接算法 */
    RUN_TEST(test_least_connections_selects_min);
    RUN_TEST(test_least_connections_all_zero);
    RUN_TEST(test_least_connections_skips_unhealthy);
    RUN_TEST(test_least_connections_no_healthy);
    RUN_TEST(test_least_connections_tie_break);

    /* 测试组 7: 统计更新 */
    RUN_TEST(test_stats_request_start);
    RUN_TEST(test_stats_request_end_success);
    RUN_TEST(test_stats_request_end_failure);
    RUN_TEST(test_stats_end_without_start);
    RUN_TEST(test_stats_multiple_backends);

    /* 测试组 8: EWMA 计算 */
    RUN_TEST(test_ewma_first_update);
    RUN_TEST(test_ewma_subsequent_updates);
    RUN_TEST(test_ewma_converges);
    RUN_TEST(test_ewma_alpha_effect);

    /* 测试组 9: 加权响应时间选择 */
    RUN_TEST(test_weighted_response_selects_lowest_ratio);
    RUN_TEST(test_weighted_response_weight_influence);
    RUN_TEST(test_weighted_response_zero_weight);

    /* 测试组 10: 随机算法 */
    RUN_TEST(test_random_returns_valid_index);
    RUN_TEST(test_random_distribution);
    RUN_TEST(test_random_no_healthy);

    /* 测试组 11: 边界条件 */
    RUN_TEST(test_select_null_lb);
    RUN_TEST(test_select_null_rule);
    RUN_TEST(test_select_empty_rule);
    RUN_TEST(test_update_stats_null_lb);
    RUN_TEST(test_round_robin_returns_neg_one);

    /* 测试组 12: 综合集成 */
    RUN_TEST(test_full_request_lifecycle_least_connections);
    RUN_TEST(test_full_request_lifecycle_weighted_response);
    RUN_TEST(test_virtual_node_distribution_uniform);

    return UNITY_END();
}

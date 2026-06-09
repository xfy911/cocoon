#include "unity.h"
#include "proxy.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/* 白盒测试：包含 proxy.c 以访问静态函数 */
#define UNIT_TEST
#include "proxy.c"
#undef UNIT_TEST

/* 绕过 gethostbyname 相关函数，避免测试中进行真实 DNS 查询 */
/* proxy_connect_backend 不会被调用，所以无需 mock */

void setUp(void) { }
void tearDown(void) { }

/* ===== proxy_init ===== */

void test_init_zeros(void) {
    cocoon_proxy_config_t cfg;
    proxy_init(&cfg);
    TEST_ASSERT_EQUAL(0, cfg.count);
    for (size_t i = 0; i < sizeof(cfg.rules) / sizeof(cfg.rules[0]); i++) {
        TEST_ASSERT_EQUAL(0, cfg.rules[i].backend_count);
    }
}

/* ===== proxy_add_rule ===== */

void test_add_rule_basic(void) {
    cocoon_proxy_config_t cfg;
    proxy_init(&cfg);

    bool ok = proxy_add_rule(&cfg, "/api/", "http://localhost:3000", 4, 1, NULL);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, cfg.count);
    TEST_ASSERT_EQUAL_STRING("/api/", cfg.rules[0].path_prefix);
    TEST_ASSERT_EQUAL(1, cfg.rules[0].backend_count);
    TEST_ASSERT_EQUAL_STRING("localhost", cfg.rules[0].backends[0].target_host);
    TEST_ASSERT_EQUAL(3000, cfg.rules[0].backends[0].target_port);
    TEST_ASSERT_FALSE(cfg.rules[0].backends[0].target_https);
}

void test_add_rule_append_backend(void) {
    cocoon_proxy_config_t cfg;
    proxy_init(&cfg);

    proxy_add_rule(&cfg, "/api/", "http://localhost:3000", 4, 1, NULL);
    bool ok = proxy_add_rule(&cfg, "/api/", "http://localhost:3001", 4, 2, NULL);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, cfg.count);      /* 仍是1条规则 */
    TEST_ASSERT_EQUAL(2, cfg.rules[0].backend_count); /* 2个后端 */
    TEST_ASSERT_EQUAL(3000, cfg.rules[0].backends[0].target_port);
    TEST_ASSERT_EQUAL(3001, cfg.rules[0].backends[1].target_port);
    TEST_ASSERT_EQUAL(2, cfg.rules[0].backends[1].weight);
}

void test_add_rule_null_prefix(void) {
    cocoon_proxy_config_t cfg;
    proxy_init(&cfg);
    TEST_ASSERT_FALSE(proxy_add_rule(&cfg, NULL, "http://localhost:3000", 4, 1, NULL));
    TEST_ASSERT_FALSE(proxy_add_rule(&cfg, "", "http://localhost:3000", 4, 1, NULL));
}

void test_add_rule_null_target(void) {
    cocoon_proxy_config_t cfg;
    proxy_init(&cfg);
    TEST_ASSERT_FALSE(proxy_add_rule(&cfg, "/api/", NULL, 4, 1, NULL));
    TEST_ASSERT_FALSE(proxy_add_rule(&cfg, "/api/", "", 4, 1, NULL));
}

/* ===== proxy_match ===== */

void test_match_exact(void) {
    cocoon_proxy_config_t cfg;
    proxy_init(&cfg);
    proxy_add_rule(&cfg, "/api/", "http://localhost:3000", 4, 1, NULL);

    cocoon_proxy_rule_t *rule = proxy_match(&cfg, "/api/users");
    TEST_ASSERT_NOT_NULL(rule);
    TEST_ASSERT_EQUAL_STRING("/api/", rule->path_prefix);
}

void test_match_no_match(void) {
    cocoon_proxy_config_t cfg;
    proxy_init(&cfg);
    proxy_add_rule(&cfg, "/api/", "http://localhost:3000", 4, 1, NULL);

    TEST_ASSERT_NULL(proxy_match(&cfg, "/static/file.css"));
    TEST_ASSERT_NULL(proxy_match(&cfg, "/"));
}

void test_match_multiple_rules(void) {
    cocoon_proxy_config_t cfg;
    proxy_init(&cfg);
    proxy_add_rule(&cfg, "/api/", "http://localhost:3000", 4, 1, NULL);
    proxy_add_rule(&cfg, "/static/", "http://localhost:4000", 4, 1, NULL);

    TEST_ASSERT_EQUAL_STRING("/api/", proxy_match(&cfg, "/api/v1")->path_prefix);
    TEST_ASSERT_EQUAL_STRING("/static/", proxy_match(&cfg, "/static/img.jpg")->path_prefix);
}

void test_match_null(void) {
    TEST_ASSERT_NULL(proxy_match(NULL, "/api/"));
    TEST_ASSERT_NULL(proxy_match(NULL, NULL));
}

/* ===== select_backend_sww ===== */

void test_sww_single(void) {
    cocoon_proxy_rule_t rule = {0};
    rule.backend_count = 1;
    rule.backends[0].healthy = true;
    rule.backends[0].weight = 1;
    rule.backends[0].current_weight = 0;

    cocoon_proxy_backend_t *b = select_backend_sww(&rule);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL(1, b->weight);
    /* 1 + 1 - 1 = 1 */
    TEST_ASSERT_EQUAL(0, b->current_weight);
}

void test_sww_equal_weights(void) {
    cocoon_proxy_rule_t rule = {0};
    rule.backend_count = 2;
    rule.backends[0].healthy = true; rule.backends[0].weight = 1; rule.backends[0].current_weight = 0;
    rule.backends[1].healthy = true; rule.backends[1].weight = 1; rule.backends[1].current_weight = 0;

    /* 第1轮：cw=[1,1]，选第1个（或第2个，取决于实现），但 Nginx 算法是选最大 */
    /* current_weight += weight → [1,1], total=2, 选第一个(1), 然后 -=2 → [-1,1] */
    cocoon_proxy_backend_t *b1 = select_backend_sww(&rule);
    TEST_ASSERT_NOT_NULL(b1);

    /* 第2轮：cw=[0,2], total=2, 选 backends[1] (2>0), 然后 -=2 → [0,0] */
    cocoon_proxy_backend_t *b2 = select_backend_sww(&rule);
    TEST_ASSERT_NOT_NULL(b2);

    /* 两轮应该分别选到不同后端（等权重） */
    TEST_ASSERT_NOT_EQUAL(b1, b2);

    /* 第3轮：cw=[1,1], 回到和第1轮相同状态 */
    cocoon_proxy_backend_t *b3 = select_backend_sww(&rule);
    TEST_ASSERT_EQUAL_PTR(b1, b3);
}

void test_sww_unequal_weights(void) {
    cocoon_proxy_rule_t rule = {0};
    rule.backend_count = 2;
    rule.backends[0].healthy = true; rule.backends[0].weight = 3; rule.backends[0].current_weight = 0;
    rule.backends[1].healthy = true; rule.backends[1].weight = 1; rule.backends[1].current_weight = 0;

    /* 记录 10 次选择，验证 3:1 分布 */
    int count[2] = {0, 0};
    for (int i = 0; i < 8; i++) {  /* 8次 = 2个周期（total=4） */
        cocoon_proxy_backend_t *b = select_backend_sww(&rule);
        TEST_ASSERT_NOT_NULL(b);
        if (b == &rule.backends[0]) count[0]++;
        else count[1]++;
    }
    /* 3:1 权重，8次选择应该 backends[0] 出现 6 次，backends[1] 出现 2 次 */
    TEST_ASSERT_EQUAL(6, count[0]);
    TEST_ASSERT_EQUAL(2, count[1]);
}

void test_sww_skips_unhealthy(void) {
    cocoon_proxy_rule_t rule = {0};
    rule.backend_count = 2;
    rule.backends[0].healthy = false; rule.backends[0].weight = 1; rule.backends[0].current_weight = 0;
    rule.backends[1].healthy = true;  rule.backends[1].weight = 1; rule.backends[1].current_weight = 0;

    cocoon_proxy_backend_t *b = select_backend_sww(&rule);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_PTR(&rule.backends[1], b);
}

void test_sww_all_unhealthy(void) {
    cocoon_proxy_rule_t rule = {0};
    rule.backend_count = 2;
    rule.backends[0].healthy = false; rule.backends[0].weight = 1;
    rule.backends[1].healthy = false; rule.backends[1].weight = 1;

    TEST_ASSERT_NULL(select_backend_sww(&rule));
}

/* ===== select_backend_sww_all ===== */

void test_sww_all_includes_unhealthy(void) {
    cocoon_proxy_rule_t rule = {0};
    rule.backend_count = 2;
    rule.backends[0].healthy = false; rule.backends[0].weight = 1; rule.backends[0].current_weight = 0;
    rule.backends[1].healthy = false;  rule.backends[1].weight = 1; rule.backends[1].current_weight = 0;

    /* sww_all 应该包含不健康后端 */
    cocoon_proxy_backend_t *b = select_backend_sww_all(&rule);
    TEST_ASSERT_NOT_NULL(b);
}

/* ===== parse_backend_url ===== */

void test_parse_url_http(void) {
    cocoon_proxy_backend_t backend = {0};
    parse_backend_url("http://localhost:3000/api", &backend, 4);
    TEST_ASSERT_FALSE(backend.target_https);
    TEST_ASSERT_EQUAL(3000, backend.target_port);
    TEST_ASSERT_EQUAL_STRING("localhost", backend.target_host);
    TEST_ASSERT_EQUAL_STRING("/api", backend.target_path);
}

void test_parse_url_https(void) {
    cocoon_proxy_backend_t backend = {0};
    parse_backend_url("https://example.com:443/", &backend, 4);
    TEST_ASSERT_TRUE(backend.target_https);
    TEST_ASSERT_EQUAL(443, backend.target_port);
    TEST_ASSERT_EQUAL_STRING("example.com", backend.target_host);
    TEST_ASSERT_EQUAL_STRING("/", backend.target_path);
}

void test_parse_url_no_scheme(void) {
    cocoon_proxy_backend_t backend = {0};
    parse_backend_url("127.0.0.1:8080", &backend, 4);
    TEST_ASSERT_FALSE(backend.target_https);
    TEST_ASSERT_EQUAL(8080, backend.target_port);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", backend.target_host);
    TEST_ASSERT_EQUAL_STRING("", backend.target_path);
}

void test_parse_url_no_port(void) {
    cocoon_proxy_backend_t backend = {0};
    parse_backend_url("http://localhost", &backend, 4);
    TEST_ASSERT_FALSE(backend.target_https);
    TEST_ASSERT_EQUAL(80, backend.target_port);
    TEST_ASSERT_EQUAL_STRING("localhost", backend.target_host);
}

void test_parse_url_https_no_port(void) {
    cocoon_proxy_backend_t backend = {0};
    parse_backend_url("https://secure.example.com", &backend, 4);
    TEST_ASSERT_TRUE(backend.target_https);
    TEST_ASSERT_EQUAL(443, backend.target_port);
    TEST_ASSERT_EQUAL_STRING("secure.example.com", backend.target_host);
}

void test_parse_url_with_path(void) {
    cocoon_proxy_backend_t backend = {0};
    parse_backend_url("http://api.internal:5000/v2/users", &backend, 4);
    TEST_ASSERT_FALSE(backend.target_https);
    TEST_ASSERT_EQUAL(5000, backend.target_port);
    TEST_ASSERT_EQUAL_STRING("api.internal", backend.target_host);
    TEST_ASSERT_EQUAL_STRING("/v2/users", backend.target_path);
}

/* ===== proxy_build_forwarded_path ===== */

void test_build_path_no_target_path(void) {
    cocoon_proxy_rule_t rule = {0};
    strncpy(rule.path_prefix, "/api/", sizeof(rule.path_prefix) - 1);
    cocoon_proxy_backend_t backend = {0};
    backend.target_path[0] = '\0';

    char out[256];
    proxy_build_forwarded_path(&rule, &backend, "/api/users/123", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("users/123", out);
}

void test_build_path_with_target_path(void) {
    cocoon_proxy_rule_t rule = {0};
    strncpy(rule.path_prefix, "/api/", sizeof(rule.path_prefix) - 1);
    cocoon_proxy_backend_t backend = {0};
    strncpy(backend.target_path, "/v1", sizeof(backend.target_path) - 1);

    char out[256];
    proxy_build_forwarded_path(&rule, &backend, "/api/users", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("/v1users", out);
}

void test_build_path_empty_remaining(void) {
    cocoon_proxy_rule_t rule = {0};
    strncpy(rule.path_prefix, "/api", sizeof(rule.path_prefix) - 1);
    cocoon_proxy_backend_t backend = {0};
    backend.target_path[0] = '\0';

    char out[256];
    proxy_build_forwarded_path(&rule, &backend, "/api", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("/", out);
}

/* ===== proxy_update_health ===== */

void test_update_health_success(void) {
    cocoon_proxy_backend_t backend = {0};
    backend.healthy = false;
    backend.success_count = 0;

    /* 第1次成功 — 仍不健康 */
    proxy_update_health(&backend, true);
    TEST_ASSERT_FALSE(backend.healthy);
    TEST_ASSERT_EQUAL(1, backend.success_count);

    /* 第2次成功 — 达到阈值 */
    proxy_update_health(&backend, true);
    TEST_ASSERT_TRUE(backend.healthy);
    TEST_ASSERT_EQUAL(2, backend.success_count);
}

void test_update_health_failure_threshold(void) {
    cocoon_proxy_backend_t backend = {0};
    backend.healthy = true;
    backend.fail_count = 0;

    /* 连续3次失败 */
    proxy_update_health(&backend, false);
    TEST_ASSERT_TRUE(backend.healthy); /* 第1次 */
    proxy_update_health(&backend, false);
    TEST_ASSERT_TRUE(backend.healthy); /* 第2次 */
    proxy_update_health(&backend, false);
    TEST_ASSERT_FALSE(backend.healthy); /* 第3次，标记不健康 */
}

void test_update_health_reset_counts(void) {
    cocoon_proxy_backend_t backend = {0};
    backend.healthy = true;

    /* 失败1次 */
    proxy_update_health(&backend, false);
    TEST_ASSERT_EQUAL(1, backend.fail_count);

    /* 成功1次 — fail_count 重置 */
    proxy_update_health(&backend, true);
    TEST_ASSERT_EQUAL(0, backend.fail_count);
    TEST_ASSERT_EQUAL(1, backend.success_count);
}

void test_update_health_stays_healthy_after_single_failure(void) {
    cocoon_proxy_backend_t backend = {0};
    backend.healthy = true;

    proxy_update_health(&backend, false);
    TEST_ASSERT_TRUE(backend.healthy);
    TEST_ASSERT_EQUAL(1, backend.fail_count);
}

/* ===== proxy_pool_init ===== */

void test_pool_init_default_size(void) {
    cocoon_proxy_backend_t backend = {0};
    proxy_pool_init(&backend, 4);
    TEST_ASSERT_EQUAL(4, backend.pool.max_size);
    proxy_pool_destroy(&backend);
}

void test_pool_init_capped(void) {
    cocoon_proxy_backend_t backend = {0};
    proxy_pool_init(&backend, 100);  /* 超过上限 16 */
    TEST_ASSERT_EQUAL(COCOON_POOL_MAX_CAPACITY, backend.pool.max_size);
    proxy_pool_destroy(&backend);
}

void test_pool_init_zero_uses_default(void) {
    cocoon_proxy_backend_t backend = {0};
    proxy_pool_init(&backend, 0);
    TEST_ASSERT_EQUAL(COCOON_POOL_DEFAULT_SIZE, backend.pool.max_size);
    proxy_pool_destroy(&backend);
}

/* ===== 主函数 ===== */

int main(void) {
    UNITY_BEGIN();

    /* proxy_init */
    RUN_TEST(test_init_zeros);

    /* proxy_add_rule */
    RUN_TEST(test_add_rule_basic);
    RUN_TEST(test_add_rule_append_backend);
    RUN_TEST(test_add_rule_null_prefix);
    RUN_TEST(test_add_rule_null_target);

    /* proxy_match */
    RUN_TEST(test_match_exact);
    RUN_TEST(test_match_no_match);
    RUN_TEST(test_match_multiple_rules);
    RUN_TEST(test_match_null);

    /* select_backend_sww */
    RUN_TEST(test_sww_single);
    RUN_TEST(test_sww_equal_weights);
    RUN_TEST(test_sww_unequal_weights);
    RUN_TEST(test_sww_skips_unhealthy);
    RUN_TEST(test_sww_all_unhealthy);

    /* select_backend_sww_all */
    RUN_TEST(test_sww_all_includes_unhealthy);

    /* parse_backend_url */
    RUN_TEST(test_parse_url_http);
    RUN_TEST(test_parse_url_https);
    RUN_TEST(test_parse_url_no_scheme);
    RUN_TEST(test_parse_url_no_port);
    RUN_TEST(test_parse_url_https_no_port);
    RUN_TEST(test_parse_url_with_path);

    /* proxy_build_forwarded_path */
    RUN_TEST(test_build_path_no_target_path);
    RUN_TEST(test_build_path_with_target_path);
    RUN_TEST(test_build_path_empty_remaining);

    /* proxy_update_health */
    RUN_TEST(test_update_health_success);
    RUN_TEST(test_update_health_failure_threshold);
    RUN_TEST(test_update_health_reset_counts);
    RUN_TEST(test_update_health_stays_healthy_after_single_failure);

    /* proxy_pool_init */
    RUN_TEST(test_pool_init_default_size);
    RUN_TEST(test_pool_init_capped);
    RUN_TEST(test_pool_init_zero_uses_default);

    return UNITY_END();
}

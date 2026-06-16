#include "unity.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* 辅助：写临时配置文件，返回路径 */
static const char *write_temp_config(const char *content) {
    static char path[256];
    static int counter = 0;
    snprintf(path, sizeof(path), "/tmp/cocoon_test_config_%d_%d.json",
             (int)getpid(), counter++);
    FILE *fp = fopen(path, "w");
    if (fp) {
        fwrite(content, 1, strlen(content), fp);
        fclose(fp);
    }
    return path;
}

static void cleanup(const char *path) {
    remove(path);
}

/* ===== config_load_from_file ===== */

void test_load_valid_config(void) {
    const char *p = write_temp_config(
        "{\n"
        "  \"root_dir\": \"/var/www\",\n"
        "  \"port\": 8080,\n"
        "  \"threaded\": true,\n"
        "  \"num_workers\": 8,\n"
        "  \"max_connections\": 5000,\n"
        "  \"timeout_ms\": 60000,\n"
        "  \"log_level\": \"debug\",\n"
        "  \"gzip_enabled\": false\n"
        "}\n"
    );
    cocoon_config_t cfg = {0};
    TEST_ASSERT_TRUE(config_load_from_file(p, &cfg));
    TEST_ASSERT_EQUAL_STRING("/var/www", cfg.root_dir);
    TEST_ASSERT_EQUAL(8080, cfg.port);
    TEST_ASSERT_TRUE(cfg.threaded);
    TEST_ASSERT_EQUAL(8, cfg.num_workers);
    TEST_ASSERT_EQUAL(5000, cfg.max_connections);
    TEST_ASSERT_EQUAL(60000, cfg.timeout_ms);
    TEST_ASSERT_EQUAL(LOG_LEVEL_DEBUG, cfg.log_level);
    TEST_ASSERT_FALSE(cfg.gzip_enabled);
    free((void *)cfg.root_dir);
    cleanup(p);
}

void test_load_valid_minimal(void) {
    const char *p = write_temp_config("{\"port\": 3000}");
    cocoon_config_t cfg = {0};
    TEST_ASSERT_TRUE(config_load_from_file(p, &cfg));
    TEST_ASSERT_EQUAL(3000, cfg.port);
    free((void *)cfg.root_dir);
    cleanup(p);
}

void test_load_valid_with_comments(void) {
    const char *p = write_temp_config(
        "{\n"
        "  // this is a comment\n"
        "  \"port\": 9090\n"
        "}\n"
    );
    cocoon_config_t cfg = {0};
    TEST_ASSERT_TRUE(config_load_from_file(p, &cfg));
    TEST_ASSERT_EQUAL(9090, cfg.port);
    free((void *)cfg.root_dir);
    cleanup(p);
}

void test_load_invalid_json(void) {
    const char *p = write_temp_config("not json");
    cocoon_config_t cfg = {0};
    TEST_ASSERT_FALSE(config_load_from_file(p, &cfg));
    cleanup(p);
}

void test_load_missing_file(void) {
    cocoon_config_t cfg = {0};
    TEST_ASSERT_FALSE(config_load_from_file(
        "/tmp/cocoon_nonexist_12345.json", &cfg));
}

void test_load_empty_file(void) {
    const char *p = write_temp_config("");
    cocoon_config_t cfg = {0};
    TEST_ASSERT_FALSE(config_load_from_file(p, &cfg));
    cleanup(p);
}

void test_load_null_args(void) {
    TEST_ASSERT_FALSE(config_load_from_file(NULL, NULL));
    TEST_ASSERT_FALSE(config_load_from_file("/tmp/x", NULL));
}

/* ===== config_validate ===== */

void test_validate_null_config(void) {
    char err[256];
    TEST_ASSERT_FALSE(config_validate(NULL, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "NULL"));
}

void test_validate_valid_default(void) {
    cocoon_config_t cfg = {0};
    cfg.port = 8080;
    cfg.log_level = LOG_LEVEL_INFO;
    TEST_ASSERT_TRUE(config_validate(&cfg, NULL, 0));
}

void test_validate_port_zero(void) {
    cocoon_config_t cfg = {.port = 0, .log_level = LOG_LEVEL_INFO};
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "port"));
}

void test_validate_port_max(void) {
    cocoon_config_t cfg = {.port = 65535, .log_level = LOG_LEVEL_INFO};
    TEST_ASSERT_TRUE(config_validate(&cfg, NULL, 0));
}

void test_validate_workers_too_high(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .num_workers = 1025};
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "workers"));
}

void test_validate_max_conn_too_high(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .max_connections = 100001};
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "connections"));
}

void test_validate_timeout_too_short(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .timeout_ms = 50};
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "timeout"));
}

void test_validate_timeout_too_long(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .timeout_ms = 3600001};
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "timeout"));
}

void test_validate_log_level_invalid(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = 99};
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "log_level"));
}

void test_validate_empty_root_dir(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .root_dir = ""};
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "root_dir"));
}

void test_validate_tls_mismatch_cert_only(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .tls_cert = "/cert.pem"};
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "TLS"));
}

void test_validate_tls_mismatch_key_only(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .tls_key = "/key.pem"};
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "TLS"));
}

void test_validate_tls_both_set(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .tls_cert = "/cert.pem", .tls_key = "/key.pem"};
    TEST_ASSERT_TRUE(config_validate(&cfg, NULL, 0));
}

void test_validate_auth_mismatch_user_only(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .auth_user = "admin"};
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "Auth"));
}

void test_validate_proxy_empty_prefix(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .num_proxies = 1};
    strcpy(cfg.proxies[0].prefix, "");
    strcpy(cfg.proxies[0].target, "http://localhost:3000");
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "proxy"));
}

void test_validate_proxy_empty_target(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .num_proxies = 1};
    strcpy(cfg.proxies[0].prefix, "/api");
    strcpy(cfg.proxies[0].target, "");
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "proxy"));
}

void test_validate_proxy_pool_too_large(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .num_proxies = 1};
    strcpy(cfg.proxies[0].prefix, "/api");
    strcpy(cfg.proxies[0].target, "http://localhost:3000");
    cfg.proxies[0].pool_size = 17;
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "pool_size"));
}

void test_validate_vhost_empty_name(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .num_vhosts = 1};
    strcpy(cfg.vhosts[0].server_name, "");
    strcpy(cfg.vhosts[0].root_dir, "/var/www");
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "vhost"));
}

void test_validate_fastcgi_empty_prefix(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .num_fastcgi = 1};
    strcpy(cfg.fastcgi[0].prefix, "");
    strcpy(cfg.fastcgi[0].host, "127.0.0.1");
    cfg.fastcgi[0].port = 9000;
    cfg.fastcgi[0].pool_size = 4;
    cfg.fastcgi[0].timeout_ms = 30000;
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "fastcgi"));
}

void test_validate_fastcgi_invalid_port(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .num_fastcgi = 1};
    strcpy(cfg.fastcgi[0].prefix, "/php");
    strcpy(cfg.fastcgi[0].host, "127.0.0.1");
    cfg.fastcgi[0].port = 0;
    cfg.fastcgi[0].pool_size = 4;
    cfg.fastcgi[0].timeout_ms = 30000;
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "fastcgi"));
}

void test_validate_fastcgi_pool_too_large(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .num_fastcgi = 1};
    strcpy(cfg.fastcgi[0].prefix, "/php");
    strcpy(cfg.fastcgi[0].host, "127.0.0.1");
    cfg.fastcgi[0].port = 9000;
    cfg.fastcgi[0].pool_size = 17;
    cfg.fastcgi[0].timeout_ms = 30000;
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "fastcgi"));
}

void test_validate_rate_limit_too_high(void) {
    cocoon_config_t cfg = {.port = 8080, .log_level = LOG_LEVEL_INFO, .rate_limit = 100001};
    char err[256];
    TEST_ASSERT_FALSE(config_validate(&cfg, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "rate_limit"));
}

void test_validate_no_err_buf(void) {
    cocoon_config_t cfg = {.port = 0, .log_level = LOG_LEVEL_INFO};
    TEST_ASSERT_FALSE(config_validate(&cfg, NULL, 0));
}


void test_merge_override_all(void) {
    cocoon_config_t base = {
        .root_dir = strdup("/old"),
        .port = 8080,
        .threaded = false,
        .num_workers = 2,
        .max_connections = 100,
        .timeout_ms = 30000,
        .log_level = LOG_LEVEL_INFO,
        .gzip_enabled = true,
        .brotli_enabled = true
    };
    cocoon_config_t cmdline = {
        .root_dir = strdup("/new"),
        .port = 9090,
        .threaded = true,
        .num_workers = 4,
        .max_connections = 200,
        .timeout_ms = 60000,
        .log_level = LOG_LEVEL_DEBUG,
        .gzip_enabled = false,
        .brotli_enabled = false
    };
    config_merge(&base, &cmdline,
                 true, true, true, true, true, true, true, true, true, true, true, true,
                 false, false,
                 false, false, false, false, false, false, false, false, false, false);
    TEST_ASSERT_EQUAL_STRING("/new", base.root_dir);
    TEST_ASSERT_EQUAL(9090, base.port);
    TEST_ASSERT_TRUE(base.threaded);
    TEST_ASSERT_EQUAL(4, base.num_workers);
    TEST_ASSERT_EQUAL(200, base.max_connections);
    TEST_ASSERT_EQUAL(60000, base.timeout_ms);
    TEST_ASSERT_EQUAL(LOG_LEVEL_DEBUG, base.log_level);
    TEST_ASSERT_FALSE(base.gzip_enabled);
    TEST_ASSERT_FALSE(base.brotli_enabled);
    free((void *)base.root_dir);
    free((void *)cmdline.root_dir);
}

void test_merge_no_override(void) {
    cocoon_config_t base = {
        .root_dir = strdup("/old"),
        .port = 8080,
        .threaded = false,
        .num_workers = 2,
        .log_level = LOG_LEVEL_INFO
    };
    cocoon_config_t cmdline = {
        .root_dir = "/new",
        .port = 9090,
        .num_workers = 4,
        .log_level = LOG_LEVEL_DEBUG
    };
    config_merge(&base, &cmdline,
                 false, false, false, false, false, false, false, false, false, false, false, false,
                 false, false,
                 false, false, false, false, false, false, false, false, false, false);
    TEST_ASSERT_EQUAL_STRING("/old", base.root_dir);
    TEST_ASSERT_EQUAL(8080, base.port);
    TEST_ASSERT_FALSE(base.threaded);
    TEST_ASSERT_EQUAL(2, base.num_workers);
    TEST_ASSERT_EQUAL(LOG_LEVEL_INFO, base.log_level);
    free((void *)base.root_dir);
}

void test_merge_partial_override(void) {
    cocoon_config_t base = {
        .root_dir = strdup("/old"),
        .port = 8080,
        .num_workers = 2,
        .log_level = LOG_LEVEL_INFO
    };
    cocoon_config_t cmdline = {
        .root_dir = "/new",
        .port = 9090,
        .num_workers = 4,
        .log_level = LOG_LEVEL_DEBUG
    };
    config_merge(&base, &cmdline,
                 true, false, false, true, false, false, false, false, false, false, false, false,
                 false, false,
                 false, false, false, false, false, false, false, false, false, false);
    TEST_ASSERT_EQUAL_STRING("/new", base.root_dir);   /* overridden */
    TEST_ASSERT_EQUAL(8080, base.port);                /* not overridden */
    TEST_ASSERT_EQUAL(2, base.num_workers);            /* not overridden */
    TEST_ASSERT_EQUAL(LOG_LEVEL_INFO, base.log_level);   /* not overridden */
    free((void *)base.root_dir);
}

void test_load_invalid_field_type(void) {
    /* port 为字符串而非数字，应解析失败或忽略 */
    const char *p = write_temp_config("{\"port\": \"not_a_number\"}");
    cocoon_config_t cfg = {0};
    /* 极简解析器可能忽略非法类型，不 crash 即可 */
    config_load_from_file(p, &cfg);
    /* port 保持默认值 0 */
    TEST_ASSERT_EQUAL(0, cfg.port);
    free((void *)cfg.root_dir);
    cleanup(p);
}

void test_load_log_level_invalid(void) {
    const char *p = write_temp_config("{\"log_level\": \"verbose\"}");
    cocoon_config_t cfg = {0};
    config_load_from_file(p, &cfg);
    /* 非法 log_level 保持默认值 INFO */
    TEST_ASSERT_EQUAL(LOG_LEVEL_INFO, cfg.log_level);
    free((void *)cfg.root_dir);
    cleanup(p);
}

void test_load_nested_ignored(void) {
    /* 极简解析器遇到嵌套对象会失败，但至少不 crash */
    const char *p = write_temp_config(
        "{\n"
        "  \"port\": 7777,\n"
        "  \"nested\": {\"a\": 1}\n"
        "}\n"
    );
    cocoon_config_t cfg = {0};
    /* 当前极简解析器不支持嵌套对象，返回 false，但不应 segfault */
    config_load_from_file(p, &cfg);
    TEST_ASSERT_TRUE(1); /* 走到这里说明没 crash */
    free((void *)cfg.root_dir);
    cleanup(p);
}

void test_load_array_ignored(void) {
    /* 极简解析器遇到数组会失败，但至少不 crash */
    const char *p = write_temp_config(
        "{\n"
        "  \"port\": 7777,\n"
        "  \"arr\": [1, 2, 3]\n"
        "}\n"
    );
    cocoon_config_t cfg = {0};
    config_load_from_file(p, &cfg);
    TEST_ASSERT_TRUE(1); /* 走到这里说明没 crash */
    free((void *)cfg.root_dir);
    cleanup(p);
}

void test_merge_cmdline_null_root_dir(void) {
    /* cmdline root_dir 为 NULL，不应覆盖 base */
    cocoon_config_t base = {.root_dir = strdup("/old"), .port = 8080};
    cocoon_config_t cmdline = {.root_dir = NULL, .port = 9090};
    config_merge(&base, &cmdline, true, true, false, false, false, false, false, false, false, false, false, false,
                 false, false,
                 false, false, false, false, false, false, false, false, false, false);
    TEST_ASSERT_EQUAL_STRING("/old", base.root_dir); /* NULL 不覆盖 */
    TEST_ASSERT_EQUAL(9090, base.port);               /* port 覆盖 */
    free((void *)base.root_dir);
}

void test_merge_null_safety(void) {
    /* 不应 crash */
    config_merge(NULL, NULL, true, true, true, true, true, true, true, true, true, true, true, true,
                 false, false,
                 true, true, true, true, true, true, true, true, false, false);
    TEST_ASSERT_TRUE(1);
}

void test_load_gzip_enabled(void) {
    /* gzip_enabled: true */
    const char *p1 = write_temp_config("{\"gzip_enabled\": true}");
    cocoon_config_t cfg1 = {0};
    TEST_ASSERT_TRUE(config_load_from_file(p1, &cfg1));
    TEST_ASSERT_TRUE(cfg1.gzip_enabled);
    free((void *)cfg1.root_dir);
    cleanup(p1);

    /* gzip_enabled: false */
    const char *p2 = write_temp_config("{\"gzip_enabled\": false}");
    cocoon_config_t cfg2 = {0};
    TEST_ASSERT_TRUE(config_load_from_file(p2, &cfg2));
    TEST_ASSERT_FALSE(cfg2.gzip_enabled);
    free((void *)cfg2.root_dir);
    cleanup(p2);

    /* 默认值应为 true（在 main 中设置，但这里 0 初始化后为 false） */
    const char *p3 = write_temp_config("{\"port\": 3000}");
    cocoon_config_t cfg3 = {.gzip_enabled = true};
    TEST_ASSERT_TRUE(config_load_from_file(p3, &cfg3));
    TEST_ASSERT_TRUE(cfg3.gzip_enabled); /* 未指定时保持原值 */
    free((void *)cfg3.root_dir);
    cleanup(p3);
}

void test_load_brotli_enabled(void) {
    /* brotli_enabled: true */
    const char *p1 = write_temp_config("{\"brotli_enabled\": true}");
    cocoon_config_t cfg1 = {0};
    TEST_ASSERT_TRUE(config_load_from_file(p1, &cfg1));
    TEST_ASSERT_TRUE(cfg1.brotli_enabled);
    free((void *)cfg1.root_dir);
    cleanup(p1);

    /* brotli_enabled: false */
    const char *p2 = write_temp_config("{\"brotli_enabled\": false}");
    cocoon_config_t cfg2 = {0};
    TEST_ASSERT_TRUE(config_load_from_file(p2, &cfg2));
    TEST_ASSERT_FALSE(cfg2.brotli_enabled);
    free((void *)cfg2.root_dir);
    cleanup(p2);

    /* 默认值应为 true */
    const char *p3 = write_temp_config("{\"port\": 3000}");
    cocoon_config_t cfg3 = {.brotli_enabled = true};
    TEST_ASSERT_TRUE(config_load_from_file(p3, &cfg3));
    TEST_ASSERT_TRUE(cfg3.brotli_enabled);
    free((void *)cfg3.root_dir);
    cleanup(p3);
}

void test_load_plugins_string(void) {
    const char *p = write_temp_config(
        "{\n"
        "  \"port\": 3000,\n"
        "  \"plugins\": \"plugins/hello.so\"\n"
        "}\n"
    );
    cocoon_config_t cfg = {0};
    TEST_ASSERT_TRUE(config_load_from_file(p, &cfg));
    TEST_ASSERT_EQUAL(1, cfg.num_plugins);
    TEST_ASSERT_EQUAL_STRING("plugins/hello.so", cfg.plugins[0]);
    free((void *)cfg.root_dir);
    for (size_t i = 0; i < cfg.num_plugins; i++) free((void *)cfg.plugins[i]);
    cleanup(p);
}

void test_load_plugins_array(void) {
    const char *p = write_temp_config(
        "{\n"
        "  \"port\": 3000,\n"
        "  \"plugins\": [\"plugins/a.so\"]\n"
        "}\n"
    );
    cocoon_config_t cfg = {0};
    TEST_ASSERT_TRUE(config_load_from_file(p, &cfg));
    TEST_ASSERT_EQUAL(1, cfg.num_plugins);
    TEST_ASSERT_EQUAL_STRING("plugins/a.so", cfg.plugins[0]);
    free((void *)cfg.root_dir);
    for (size_t i = 0; i < cfg.num_plugins; i++) free((void *)cfg.plugins[i]);
    cleanup(p);
}

void test_load_plugins_array_multiple(void) {
    const char *p = write_temp_config(
        "{\n"
        "  \"port\": 3000,\n"
        "  \"plugins\": [\"plugins/a.so\", \"plugins/b.so\"]\n"
        "}\n"
    );
    cocoon_config_t cfg = {0};
    TEST_ASSERT_TRUE(config_load_from_file(p, &cfg));
    TEST_ASSERT_EQUAL(2, cfg.num_plugins);
    TEST_ASSERT_EQUAL_STRING("plugins/a.so", cfg.plugins[0]);
    TEST_ASSERT_EQUAL_STRING("plugins/b.so", cfg.plugins[1]);
    free((void *)cfg.root_dir);
    for (size_t i = 0; i < cfg.num_plugins; i++) free((void *)cfg.plugins[i]);
    cleanup(p);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_load_valid_config);
    RUN_TEST(test_load_valid_minimal);
    RUN_TEST(test_load_valid_with_comments);
    RUN_TEST(test_load_invalid_json);
    RUN_TEST(test_load_missing_file);
    RUN_TEST(test_load_empty_file);
    RUN_TEST(test_load_null_args);
    RUN_TEST(test_load_invalid_field_type);
    RUN_TEST(test_load_log_level_invalid);
    RUN_TEST(test_load_nested_ignored);
    RUN_TEST(test_load_array_ignored);

    RUN_TEST(test_merge_override_all);
    RUN_TEST(test_merge_no_override);
    RUN_TEST(test_merge_partial_override);
    RUN_TEST(test_merge_null_safety);
    RUN_TEST(test_merge_cmdline_null_root_dir);

    RUN_TEST(test_load_gzip_enabled);
    RUN_TEST(test_load_brotli_enabled);
    RUN_TEST(test_load_plugins_string);
    RUN_TEST(test_load_plugins_array);
    RUN_TEST(test_load_plugins_array_multiple);

    /* config_validate */
    RUN_TEST(test_validate_null_config);
    RUN_TEST(test_validate_valid_default);
    RUN_TEST(test_validate_port_zero);
    RUN_TEST(test_validate_port_max);
    RUN_TEST(test_validate_workers_too_high);
    RUN_TEST(test_validate_max_conn_too_high);
    RUN_TEST(test_validate_timeout_too_short);
    RUN_TEST(test_validate_timeout_too_long);
    RUN_TEST(test_validate_log_level_invalid);
    RUN_TEST(test_validate_empty_root_dir);
    RUN_TEST(test_validate_tls_mismatch_cert_only);
    RUN_TEST(test_validate_tls_mismatch_key_only);
    RUN_TEST(test_validate_tls_both_set);
    RUN_TEST(test_validate_auth_mismatch_user_only);
    RUN_TEST(test_validate_proxy_empty_prefix);
    RUN_TEST(test_validate_proxy_empty_target);
    RUN_TEST(test_validate_proxy_pool_too_large);
    RUN_TEST(test_validate_vhost_empty_name);
    RUN_TEST(test_validate_fastcgi_empty_prefix);
    RUN_TEST(test_validate_fastcgi_invalid_port);
    RUN_TEST(test_validate_fastcgi_pool_too_large);
    RUN_TEST(test_validate_rate_limit_too_high);
    RUN_TEST(test_validate_no_err_buf);
    return UNITY_END();
}

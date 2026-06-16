/**
 * test_acme_config.c - ACME 配置集成单元测试
 *
 * 测试 ACME 配置字段解析、校验、合并和证书过期检查。
 *
 * @author xfy
 */

#include "unity.h"
#include "acme.h"
#include "config.h"
#include "cocoon.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void setUp(void) {
    log_set_level(LOG_LEVEL_ERROR);
}

void tearDown(void) {
}

/**
 * test_acme_config_parse - 测试 ACME 配置 JSON 解析
 */
static void test_acme_config_parse(void) {
    cocoon_config_t config = {0};
    config.port = 8080;
    config.log_level = LOG_LEVEL_INFO;
    config.gzip_enabled = true;
    config.brotli_enabled = true;

    const char *json = "{"
        "\"root_dir\":\"/var/www\","
        "\"acme\":{"
        "  \"enabled\":true,"
        "  \"directory_url\":\"https://acme-staging-v02.api.letsencrypt.org/directory\","
        "  \"email\":\"admin@example.com\","
        "  \"domains\":[\"example.com\",\"www.example.com\"],"
        "  \"cert_path\":\"/etc/cocoon/cert.pem\","
        "  \"key_path\":\"/etc/cocoon/key.pem\","
        "  \"renew_days\":14"
        "}"
        "}";

    FILE *fp = fopen("/tmp/test_acme_config.json", "w");
    TEST_ASSERT_NOT_NULL(fp);
    fwrite(json, 1, strlen(json), fp);
    fclose(fp);

    bool ok = config_load_from_file("/tmp/test_acme_config.json", &config);
    TEST_ASSERT_TRUE(ok);

    TEST_ASSERT_TRUE(config.acme_enabled == true);
    TEST_ASSERT_EQUAL_STRING("https://acme-staging-v02.api.letsencrypt.org/directory", config.acme_directory_url);
    TEST_ASSERT_EQUAL_STRING("admin@example.com", config.acme_email);
    TEST_ASSERT_EQUAL_size_t(2, config.acme_num_domains);
    TEST_ASSERT_EQUAL_STRING("example.com", config.acme_domains[0]);
    TEST_ASSERT_EQUAL_STRING("www.example.com", config.acme_domains[1]);
    TEST_ASSERT_EQUAL_STRING("/etc/cocoon/cert.pem", config.acme_cert_path);
    TEST_ASSERT_EQUAL_STRING("/etc/cocoon/key.pem", config.acme_key_path);
    TEST_ASSERT_EQUAL_UINT(14, config.acme_renew_days);

    /* 校验 */
    char err_buf[256] = {0};
    ok = config_validate(&config, err_buf, sizeof(err_buf));
    TEST_ASSERT_TRUE(ok);

    free((void*)config.root_dir);
}

/**
 * test_acme_config_validation - 测试 ACME 配置校验
 */
static void test_acme_config_validation(void) {
    cocoon_config_t config = {0};
    config.port = 8080;
    config.log_level = LOG_LEVEL_INFO;
    config.gzip_enabled = true;
    config.brotli_enabled = true;
    config.root_dir = strdup("/var/www");
    config.acme_enabled = true;

    char err_buf[256] = {0};
    bool ok;

    /* 缺少邮箱 */
    ok = config_validate(&config, err_buf, sizeof(err_buf));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_NOT_NULL(strstr(err_buf, "邮箱"));

    /* 缺少域名 */
    strcpy(config.acme_email, "admin@example.com");
    ok = config_validate(&config, err_buf, sizeof(err_buf));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_NOT_NULL(strstr(err_buf, "域名"));

    /* 缺少证书路径 */
    strcpy(config.acme_domains[0], "example.com");
    config.acme_num_domains = 1;
    ok = config_validate(&config, err_buf, sizeof(err_buf));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_NOT_NULL(strstr(err_buf, "证书"));

    /* 缺少私钥路径 */
    strcpy(config.acme_cert_path, "/etc/cocoon/cert.pem");
    ok = config_validate(&config, err_buf, sizeof(err_buf));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_NOT_NULL(strstr(err_buf, "私钥"));

    /* renew_days 越界 */
    strcpy(config.acme_key_path, "/etc/cocoon/key.pem");
    config.acme_renew_days = 0;
    ok = config_validate(&config, err_buf, sizeof(err_buf));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_NOT_NULL(strstr(err_buf, "renew_days"));

    config.acme_renew_days = 91;
    ok = config_validate(&config, err_buf, sizeof(err_buf));
    TEST_ASSERT_FALSE(ok);

    /* 完整配置通过校验 */
    config.acme_renew_days = 30;
    ok = config_validate(&config, err_buf, sizeof(err_buf));
    TEST_ASSERT_TRUE(ok);

    free((void*)config.root_dir);
}

/**
 * test_acme_config_defaults - 测试 ACME 配置默认值
 */
static void test_acme_config_defaults(void) {
    cocoon_config_t config = {0};
    config.port = 8080;
    config.log_level = LOG_LEVEL_INFO;
    config.gzip_enabled = true;
    config.brotli_enabled = true;
    config.root_dir = strdup("/var/www");

    char err_buf[256] = {0};
    bool ok = config_validate(&config, err_buf, sizeof(err_buf));
    /* 未启用 ACME，不应校验 ACME 字段 */
    TEST_ASSERT_TRUE(ok);

    free((void*)config.root_dir);
}

/**
 * test_acme_cert_expiry - 测试证书过期检查
 */
static void test_acme_cert_expiry(void) {
    /* 不存在的证书 */
    int days = acme_cert_days_until_expiry("/nonexistent/cert.pem");
    TEST_ASSERT_TRUE(days < 0);

    /* 生成一个自签名证书用于测试 */
    const char *cert_path = "/tmp/test_acme_cert.pem";
    const char *key_path = "/tmp/test_acme_key.pem";

    FILE *fp = fopen(cert_path, "w");
    if (!fp) {
        /* 无法创建测试证书，跳过 */
        return;
    }
    fclose(fp);

    /* 空文件应该返回 -1 */
    days = acme_cert_days_until_expiry(cert_path);
    TEST_ASSERT_TRUE(days < 0);

    /* 使用 OpenSSL 生成一个有效期 90 天的自签名证书 */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
             "-days 90 -nodes -subj '/CN=test.example.com' 2>/dev/null",
             key_path, cert_path);
    int ret = system(cmd);
    if (ret != 0) {
        /* openssl 不可用，跳过 */
        remove(cert_path);
        return;
    }

    days = acme_cert_days_until_expiry(cert_path);
    /* 应该接近 90 天 */
    TEST_ASSERT_TRUE(days >= 85 && days <= 90);

    remove(cert_path);
    remove(key_path);
}

/**
 * test_acme_save_certificate - 测试证书保存
 */
static void test_acme_save_certificate(void) {
    const char *cert_pem = "-----BEGIN CERTIFICATE-----\n"
                           "MIIBkTCB+wIJAKHBfpE";
    const char *key_pem = "-----BEGIN PRIVATE KEY-----\n"
                          "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg";

    const char *cert_path = "/tmp/test_save_cert.pem";
    const char *key_path = "/tmp/test_save_key.pem";

    int ret = acme_save_certificate(cert_pem, key_pem, cert_path, key_path);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* 验证文件存在且内容正确 */
    FILE *fp = fopen(cert_path, "r");
    TEST_ASSERT_NOT_NULL(fp);
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT(0, strncmp(buf, cert_pem, strlen(cert_pem)));

    fp = fopen(key_path, "r");
    TEST_ASSERT_NOT_NULL(fp);
    n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT(0, strncmp(buf, key_pem, strlen(key_pem)));

    remove(cert_path);
    remove(key_path);
}

/**
 * test_acme_config_merge - 测试 ACME 配置合并
 */
static void test_acme_config_merge(void) {
    cocoon_config_t base = {0};
    base.port = 8080;
    base.log_level = LOG_LEVEL_INFO;
    base.gzip_enabled = true;
    base.brotli_enabled = true;
    base.root_dir = strdup("/var/www");

    cocoon_config_t cmdline = {0};
    cmdline.acme_enabled = true;

    config_merge(&base, &cmdline,
                 false, false, false, false, false, false,
                 false, false, false, false, false,
                 false, false, false, false,
                 false,
                 false, false,
                 false,
                 false, false, false, false,
                 true); /* has_acme_enabled */

    TEST_ASSERT_TRUE(base.acme_enabled == true);

    free((void*)base.root_dir);
}

/**
 * main - 测试入口
 */
int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_acme_config_parse);
    RUN_TEST(test_acme_config_validation);
    RUN_TEST(test_acme_config_defaults);
    RUN_TEST(test_acme_cert_expiry);
    RUN_TEST(test_acme_save_certificate);
    RUN_TEST(test_acme_config_merge);

    return UNITY_END();
}

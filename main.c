/**
 * main.c - Cocoon 入口
 *
 * 解析命令行参数和配置文件，初始化并启动服务器。
 *
 * @author xfy
 */

#include "cocoon.h"
#include "server.h"
#include "config.h"
#include "platform.h"
#include "access_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * g_ctx - 全局服务器上下文（用于信号处理）
 */
static server_context_t *g_ctx = NULL;

/**
 * signal_handler - 优雅关闭信号处理
 *
 * 捕获 SIGINT/SIGTERM，触发服务器停止。
 *
 * @param sig 信号编号
 */
static void signal_handler(int sig) {
    (void)sig;
    if (g_ctx) {
        printf("\n[Cocoon] 收到关闭信号，正在优雅停止...\n");
        server_stop(g_ctx);
    }
}

/**
 * print_usage - 打印使用说明
 *
 * @param prog 程序名
 */
static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -c <file>  JSON 配置文件路径\n");
    printf("  -r <dir>    静态资源根目录（必填，或配置文件指定）\n");
    printf("  -p <port>   监听端口（默认 8080）\n");
    printf("  -t          启用多线程调度\n");
    printf("  -w <num>    工作线程数（默认自动检测 CPU 核心）\n");
    printf("  -m <num>    最大并发连接数（默认无限制）\n");
    printf("  -o <ms>     连接空闲超时毫秒（默认 30000）\n");
    printf("  -l <level>  日志级别: error, warn, info, debug（默认 info）\n");
    printf("  -v          详细日志输出（等同于 -l debug）\n");
    printf("  --cert <path>  TLS 证书路径（启用 HTTPS）\n");
    printf("  --key <path>   TLS 私钥路径\n");
    printf("  --tls          显式启用 TLS（需同时指定 --cert 和 --key）\n");
    printf("  --no-gzip   禁用 gzip 压缩\n");
    printf("  --access-log <path> 访问日志文件路径（- 表示 stdout）\n");
    printf("  --cors       启用 CORS 支持\n");
    printf("  --auth-user <user>  Basic Auth 用户名\n");
    printf("  --auth-pass <pass>  Basic Auth 密码\n");
    printf("  --rate-limit <n>    每秒最大请求数（限流）\n");
    printf("  -h          显示此帮助\n");
    printf("\nExample:\n");
    printf("  %s -c cocoon.json\n", prog);
    printf("  %s -r ./www -p 8080\n", prog);
    printf("  %s -c cocoon.json -p 9090  # 命令行覆盖配置文件的端口\n", prog);
}

/**
 * parse_args - 解析命令行参数
 *
 * 支持配置文件 + 命令行覆盖的混合模式。
 *
 * @param argc 参数个数
 * @param argv 参数数组
 * @param config 输出配置结构体
 * @return true 成功，false 参数错误
 */
static bool parse_args(int argc, char *argv[], cocoon_config_t *config) {
    /* 默认值 */
    config->root_dir = NULL;
    config->port = 8080;
    config->threaded = false;
    config->num_workers = 0;
    config->max_connections = 0;
    config->timeout_ms = 0;
    config->log_level = LOG_LEVEL_INFO;
    config->gzip_enabled = true;
    config->brotli_enabled = true;

    config->tls_cert = NULL;
    config->tls_key = NULL;
    config->tls_enabled = false;
    config->access_log_path = NULL;

    config->cors_enabled = false;
    config->auth_user = NULL;
    config->auth_pass = NULL;
    config->rate_limit = 0;

    bool has_root_dir = false;
    bool has_port = false;
    bool has_workers = false;
    bool has_max_conn = false;
    bool has_timeout = false;
    bool has_log_level = false;
    bool has_gzip_enabled = false;
    bool has_brotli_enabled = false;
    bool has_tls_cert = false;
    bool has_tls_key = false;
    bool has_tls_enabled = false;
    bool has_access_log = false;
    bool has_cors_enabled = false;
    bool has_auth_user = false;
    bool has_auth_pass = false;
    bool has_rate_limit = false;
    const char *config_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            if (++i >= argc) return false;
            config_file = argv[i];
        } else if (strcmp(argv[i], "-r") == 0) {
            if (++i >= argc) return false;
            config->root_dir = strdup(argv[i]);
            has_root_dir = true;
        } else if (strcmp(argv[i], "-p") == 0) {
            if (++i >= argc) return false;
            config->port = (uint16_t)atoi(argv[i]);
            if (config->port == 0) config->port = 8080;
            has_port = true;
        } else if (strcmp(argv[i], "-t") == 0) {
            config->threaded = true;
        } else if (strcmp(argv[i], "-w") == 0) {
            if (++i >= argc) return false;
            config->num_workers = (uint32_t)atoi(argv[i]);
            has_workers = true;
        } else if (strcmp(argv[i], "-m") == 0) {
            if (++i >= argc) return false;
            config->max_connections = (uint32_t)atoi(argv[i]);
            has_max_conn = true;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) return false;
            config->timeout_ms = (uint32_t)atoi(argv[i]);
            has_timeout = true;
        } else if (strcmp(argv[i], "-l") == 0) {
            if (++i >= argc) return false;
            if (strcmp(argv[i], "error") == 0) config->log_level = LOG_LEVEL_ERROR;
            else if (strcmp(argv[i], "warn") == 0) config->log_level = LOG_LEVEL_WARN;
            else if (strcmp(argv[i], "info") == 0) config->log_level = LOG_LEVEL_INFO;
            else if (strcmp(argv[i], "debug") == 0) config->log_level = LOG_LEVEL_DEBUG;
            else {
                fprintf(stderr, "Unknown log level: %s\n", argv[i]);
                return false;
            }
            has_log_level = true;
        } else if (strcmp(argv[i], "-v") == 0) {
            config->log_level = LOG_LEVEL_DEBUG;
            has_log_level = true;
        } else if (strcmp(argv[i], "--no-gzip") == 0) {
            config->gzip_enabled = false;
            has_gzip_enabled = true;
        } else if (strcmp(argv[i], "--no-brotli") == 0) {
            config->brotli_enabled = false;
            has_brotli_enabled = true;
        } else if (strcmp(argv[i], "--cert") == 0) {
            if (++i >= argc) return false;
            config->tls_cert = strdup(argv[i]);
            has_tls_cert = true;
        } else if (strcmp(argv[i], "--key") == 0) {
            if (++i >= argc) return false;
            config->tls_key = strdup(argv[i]);
            has_tls_key = true;
        } else if (strcmp(argv[i], "--tls") == 0) {
            config->tls_enabled = true;
            has_tls_enabled = true;
        } else if (strcmp(argv[i], "--access-log") == 0) {
            if (++i >= argc) return false;
            config->access_log_path = strdup(argv[i]);
            has_access_log = true;
        } else if (strcmp(argv[i], "--cors") == 0) {
            config->cors_enabled = true;
            has_cors_enabled = true;
        } else if (strcmp(argv[i], "--auth-user") == 0) {
            if (++i >= argc) return false;
            config->auth_user = strdup(argv[i]);
            has_auth_user = true;
        } else if (strcmp(argv[i], "--auth-pass") == 0) {
            if (++i >= argc) return false;
            config->auth_pass = strdup(argv[i]);
            has_auth_pass = true;
        } else if (strcmp(argv[i], "--rate-limit") == 0) {
            if (++i >= argc) return false;
            config->rate_limit = (uint32_t)atoi(argv[i]);
            has_rate_limit = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return false;
        }
    }

    /* 如果有配置文件，先加载 */
    if (config_file) {
        if (!config_load_from_file(config_file, config)) {
            fprintf(stderr, "Error: 无法加载配置文件: %s\n", config_file);
            return false;
        }
        /* 用命令行参数覆盖配置文件 */
        config_merge(config, config, has_root_dir, has_port, has_workers,
                     has_max_conn, has_timeout, has_log_level,
                     has_gzip_enabled, has_brotli_enabled,
                     has_tls_cert, has_tls_key, has_tls_enabled,
                     has_access_log,
                     has_cors_enabled, has_auth_user, has_auth_pass,
                     has_rate_limit);
    }

    if (!config->root_dir) {
        fprintf(stderr, "Error: 必须指定静态资源根目录（-r <dir> 或配置文件）\n");
        return false;
    }

    return true;
}

/**
 * main - 程序入口
 *
 * 解析参数，注册信号，创建并启动服务器。
 *
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 0 成功，1 失败
 */
int main(int argc, char *argv[]) {
    cocoon_config_t config = {0};

    if (!parse_args(argc, argv, &config)) {
        print_usage(argv[0]);
        return 1;
    }

    /* 初始化 socket 子系统（Windows 下 WSAStartup） */
    if (cocoon_socket_init() != 0) {
        fprintf(stderr, "[Cocoon] socket 子系统初始化失败\n");
        return 1;
    }

    /* 注册信号处理 */
    cocoon_signal_setup(signal_handler);

    /* 设置日志级别 */
    log_set_level(config.log_level);

    /* 初始化访问日志 */
    if (config.access_log_path) {
        access_log_init(config.access_log_path);
    }

    /* 创建服务器 */
    g_ctx = server_create(&config);
    if (!g_ctx) {
        fprintf(stderr, "[Cocoon] 创建服务器失败\n");
        return 1;
    }

    /* 启动（阻塞） */
    int ret = server_start(g_ctx);

    /* 清理 */
    server_destroy(g_ctx);
    g_ctx = NULL;

    /* 释放 socket 子系统（Windows 下 WSACleanup） */
    cocoon_socket_cleanup();

    /* 关闭访问日志 */
    access_log_close();

    /* 释放配置文件分配的内存 */
    if (config.root_dir) free((void *)config.root_dir);
    if (config.tls_cert) free((void *)config.tls_cert);
    if (config.tls_key) free((void *)config.tls_key);
    if (config.access_log_path) free((void *)config.access_log_path);
    if (config.auth_user) free((void *)config.auth_user);
    if (config.auth_pass) free((void *)config.auth_pass);

    return ret == COCOON_OK ? 0 : 1;
}

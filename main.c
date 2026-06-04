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
    printf("  --no-gzip   禁用 gzip 压缩\n");
    printf("  --no-brotli 禁用 brotli 压缩\n");
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

    bool has_root_dir = false;
    bool has_port = false;
    bool has_workers = false;
    bool has_max_conn = false;
    bool has_timeout = false;
    bool has_log_level = false;
    bool has_gzip_enabled = false;
    bool has_brotli_enabled = false;
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
                     has_max_conn, has_timeout, has_log_level, has_gzip_enabled, has_brotli_enabled);
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
    cocoon_config_t config;

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

    /* 释放配置文件分配的 root_dir */
    if (config.root_dir) {
        free((void *)config.root_dir);
        config.root_dir = NULL;
    }

    return ret == COCOON_OK ? 0 : 1;
}

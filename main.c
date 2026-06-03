/**
 * main.c - Cocoon 入口
 *
 * 解析命令行参数，初始化并启动服务器。
 *
 * @author xfy
 */

#include "cocoon.h"
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

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
    printf("  -r <dir>    静态资源根目录（必填）\n");
    printf("  -p <port>   监听端口（默认 8080）\n");
    printf("  -t          启用多线程调度\n");
    printf("  -w <num>    工作线程数（默认自动检测 CPU 核心）\n");
    printf("  -v          详细日志输出\n");
    printf("  -h          显示此帮助\n");
    printf("\nExample:\n");
    printf("  %s -r ./www -p 8080\n", prog);
    printf("  %s -r ./www -p 8080 -t -w 8\n", prog);
}

/**
 * parse_args - 解析命令行参数
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
    config->verbose = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            if (++i >= argc) return false;
            config->root_dir = argv[i];
        } else if (strcmp(argv[i], "-p") == 0) {
            if (++i >= argc) return false;
            config->port = (uint16_t)atoi(argv[i]);
            if (config->port == 0) config->port = 8080;
        } else if (strcmp(argv[i], "-t") == 0) {
            config->threaded = true;
        } else if (strcmp(argv[i], "-w") == 0) {
            if (++i >= argc) return false;
            config->num_workers = (uint32_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            config->verbose = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return false;
        }
    }

    if (!config->root_dir) {
        fprintf(stderr, "Error: 必须指定静态资源根目录（-r <dir>）\n");
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

    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

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

    return ret == COCOON_OK ? 0 : 1;
}

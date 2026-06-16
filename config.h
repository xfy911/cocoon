/**
 * config.h - JSON 配置文件解析模块
 *
 * 轻量级配置加载，支持从 JSON 文件读取服务器配置。
 * 保持零依赖，只解析 cocoon 需要的字段。
 *
 * @author xfy
 */

#ifndef COCOON_CONFIG_H
#define COCOON_CONFIG_H

#include "cocoon.h"
#include <stdbool.h>

/**
 * config_load_from_file - 从 JSON 配置文件加载配置
 *
 * 解析 JSON 文件，填充 cocoon_config_t 结构体。
 * 只解析存在的字段，不存在的保持默认值。
 *
 * @param path JSON 文件路径
 * @param config 输出配置结构体
 * @return true 成功，false 失败（文件不存在或格式错误）
 */
bool config_load_from_file(const char *path, cocoon_config_t *config);

/**
 * config_validate - 校验配置是否合法
 *
 * 在热重载前调用，确保新配置不会导致服务异常。
 * 校验失败时返回 false 并写入错误信息到 err_buf。
 *
 * @param config 待校验的配置
 * @param err_buf 错误信息缓冲区（可为 NULL）
 * @param err_size 缓冲区大小
 * @return true 合法，false 不合法
 */
bool config_validate(const cocoon_config_t *config, char *err_buf, size_t err_size);

/**
 * config_merge - 用命令行配置覆盖文件配置
 *
 * 优先级：命令行 > 配置文件 > 硬编码默认值
 * 命令行参数中显式指定的值（非默认值）会覆盖配置文件。
 *
 * @param base 从文件加载的基础配置
 * @param cmdline 命令行解析出的配置
 * @param has_root_dir 命令行是否指定了 -r
 * @param has_port 命令行是否指定了 -p
 * @param has_workers 命令行是否指定了 -w
 * @param has_max_conn 命令行是否指定了 -m
 * @param has_timeout 命令行是否指定了 -o
 * @param has_log_level 命令行是否指定了 -l
 */
void config_merge(cocoon_config_t *base, const cocoon_config_t *cmdline,
                  bool has_root_dir, bool has_port, bool has_workers,
                  bool has_max_conn, bool has_timeout, bool has_log_level,
                  bool has_gzip_enabled, bool has_brotli_enabled,
                  bool has_tls_cert, bool has_tls_key, bool has_tls_enabled,
                  bool has_access_log,
                  bool has_cors_enabled, bool has_auth_user, bool has_auth_pass,
                  bool has_rate_limit,
                  bool has_plugins,
                  bool has_cache_enabled, bool has_cache_max_size,
                  bool has_cache_ttl_seconds, bool has_cache_max_entry_size,
                  bool has_acme_enabled);

#endif /* COCOON_CONFIG_H */

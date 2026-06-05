/**
 * plugin.h - 插件系统接口
 *
 * 提供动态加载 .so/.dll 扩展的能力，插件可以注册中间件、
 * 拦截请求或扩展服务器功能。
 *
 * 插件接口：
 *   - cocoon_plugin_init()   初始化，返回 0 表示成功
 *   - cocoon_plugin_shutdown()  关闭，清理资源
 *   - cocoon_plugin_version()   返回版本字符串
 *
 * 插件通过调用 cocoon_middleware_register() 注册中间件来参与请求处理。
 *
 * @author xfy
 */

#ifndef COCOON_PLUGIN_H
#define COCOON_PLUGIN_H

#include "middleware.h"
#include <stdbool.h>
#include <stddef.h>

/* === 插件接口（插件需要实现） === */
/**
 * cocoon_plugin_init_func_t - 插件初始化函数签名
 *
 * 插件入口。在此函数中调用 cocoon_middleware_register() 注册中间件，
 * 或执行其他初始化工作。
 *
 * @return 0 成功，非 0 失败（服务器会拒绝加载该插件）
 */
typedef int (*cocoon_plugin_init_func_t)(void);

/**
 * cocoon_plugin_shutdown_func_t - 插件关闭函数签名
 *
 * 清理插件分配的资源，注销中间件等。
 */
typedef void (*cocoon_plugin_shutdown_func_t)(void);

/**
 * cocoon_plugin_version_func_t - 插件版本函数签名
 *
 * @return 版本字符串，如 "1.0.0"
 */
typedef const char *(*cocoon_plugin_version_func_t)(void);

/* === 插件加载器 API === */

#define MAX_PLUGINS 8

/**
 * cocoon_plugin_load - 加载一个插件
 *
 * 使用 dlopen/dlsym 加载动态库，调用初始化函数。
 * 如果插件没有实现 cocoon_plugin_init，则仅加载不执行初始化。
 *
 * @param path 插件文件路径（.so / .dll）
 * @return 0 成功，-1 失败
 */
int cocoon_plugin_load(const char *path);

/**
 * cocoon_plugin_unload_all - 卸载所有已加载的插件
 *
 * 按加载逆序调用 shutdown 函数，然后 dlclose。
 */
void cocoon_plugin_unload_all(void);

/**
 * cocoon_plugin_count - 获取当前已加载的插件数量
 */
size_t cocoon_plugin_count(void);

/**
 * cocoon_plugin_get_version - 获取指定索引的插件版本
 *
 * @param index 插件索引（0 ~ count-1）
 * @return 版本字符串，或 "unknown"
 */
const char *cocoon_plugin_get_version(size_t index);

/**
 * cocoon_plugin_get_path - 获取指定索引的插件路径
 */
const char *cocoon_plugin_get_path(size_t index);

#endif /* COCOON_PLUGIN_H */

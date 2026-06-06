/**
 * plugin.c - 插件系统实现
 *
 * 使用 dlopen / dlsym 实现动态加载。
 * 保持线程安全：加载/卸载在主线程完成。
 *
 * @author xfy
 */

#include "plugin.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* dlopen 需要 RTLD_NOW 和 RTLD_LOCAL */
#include <dlfcn.h>

/* === 插件条目 === */
typedef struct {
    void                       *handle;         /**< dlopen 句柄 */
    char                        path[256];      /**< 插件路径 */
    cocoon_plugin_shutdown_func_t shutdown;   /**< 关闭函数 */
    cocoon_plugin_version_func_t   version;    /**< 版本函数 */
} plugin_entry_t;

static plugin_entry_t g_plugins[MAX_PLUGINS];
static size_t g_plugin_count = 0;
/* 存储已加载的插件路径，用于热重载 */
static char g_plugin_paths[MAX_PLUGINS][256];
static size_t g_plugin_path_count = 0;

/**
 * plugin_store_path - 保存插件路径到重载列表
 */
static void plugin_store_path(const char *path) {
    if (g_plugin_path_count < MAX_PLUGINS) {
        strncpy(g_plugin_paths[g_plugin_path_count], path, sizeof(g_plugin_paths[0]) - 1);
        g_plugin_paths[g_plugin_path_count][sizeof(g_plugin_paths[0]) - 1] = '\0';
        g_plugin_path_count++;
    }
}

/**
 * cocoon_plugin_load - 加载一个插件
 */
int cocoon_plugin_load(const char *path) {
    if (!path || !path[0]) {
        log_error("plugin: 空路径");
        return -1;
    }
    if (g_plugin_count >= MAX_PLUGINS) {
        log_error("plugin: 注册表已满（最多 %d 个）", MAX_PLUGINS);
        return -1;
    }

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        log_error("plugin: 无法加载 %s: %s", path, dlerror());
        return -1;
    }

    /* 查找符号 */
    cocoon_plugin_init_func_t init = (cocoon_plugin_init_func_t)dlsym(handle, "cocoon_plugin_init");
    cocoon_plugin_shutdown_func_t shutdown = (cocoon_plugin_shutdown_func_t)dlsym(handle, "cocoon_plugin_shutdown");
    cocoon_plugin_version_func_t version = (cocoon_plugin_version_func_t)dlsym(handle, "cocoon_plugin_version");

    const char *ver_str = version ? version() : "unknown";

    /* 如果存在 init，则调用 */
    if (init) {
        int ret = init();
        if (ret != 0) {
            log_error("plugin: %s 初始化失败（返回 %d），已卸载", path, ret);
            dlclose(handle);
            return -1;
        }
    } else {
        log_warn("plugin: %s 缺少 cocoon_plugin_init，仅加载", path);
    }

    /* 保存到注册表 */
    plugin_entry_t *entry = &g_plugins[g_plugin_count];
    entry->handle = handle;
    entry->shutdown = shutdown;
    entry->version = version;
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
    g_plugin_count++;

    /* 同时保存路径用于重载 */
    plugin_store_path(path);

    log_info("plugin: 已加载 %s (v%s, #%zu)", path, ver_str, g_plugin_count);
    return 0;
}

/**
 * cocoon_plugin_unload_all - 卸载所有插件
 */
void cocoon_plugin_unload_all(void) {
    /* 逆序卸载 */
    for (size_t i = g_plugin_count; i > 0; i--) {
        plugin_entry_t *entry = &g_plugins[i - 1];
        if (entry->shutdown) {
            entry->shutdown();
        }
        dlclose(entry->handle);
        log_info("plugin: 已卸载 %s", entry->path);
    }
    g_plugin_count = 0;
}

/**
 * cocoon_plugin_reload - 热重载所有插件
 *
 * 先卸载所有插件，再按存储路径重新加载。
 */
int cocoon_plugin_reload(void) {
    if (g_plugin_path_count == 0) {
        log_warn("plugin: 没有存储的插件路径，无法重载");
        return 0; /* 没有插件也算成功 */
    }

    log_info("plugin: 开始热重载 %zu 个插件...", g_plugin_path_count);
    cocoon_plugin_unload_all();
    /* 保留路径，重新加载 */
    size_t success = 0;
    for (size_t i = 0; i < g_plugin_path_count; i++) {
        if (cocoon_plugin_load(g_plugin_paths[i]) == 0) {
            success++;
        } else {
            log_error("plugin: 重载失败 %s", g_plugin_paths[i]);
        }
    }

    log_info("plugin: 热重载完成，%zu/%zu 成功", success, g_plugin_path_count);
    return (success == g_plugin_path_count) ? 0 : -1;
}

/**
 * cocoon_plugin_count - 已加载插件数量
 */
size_t cocoon_plugin_count(void) {
    return g_plugin_count;
}

/**
 * cocoon_plugin_get_version - 获取插件版本
 */
const char *cocoon_plugin_get_version(size_t index) {
    if (index >= g_plugin_count) return "unknown";
    if (g_plugins[index].version) {
        return g_plugins[index].version();
    }
    return "unknown";
}

/**
 * cocoon_plugin_get_path - 获取插件路径
 */
const char *cocoon_plugin_get_path(size_t index) {
    if (index >= g_plugin_count) return NULL;
    return g_plugins[index].path;
}

/**
 * hello.c - 示例插件
 *
 * 演示如何注册自定义中间件。
 * 该插件注册一个中间件，为所有响应添加自定义响应头 X-Hello-Plugin。
 */

#include "../middleware.h"
#include "../http.h"
#include "../platform.h"
#include <stdio.h>
#include <string.h>

/**
 * hello_middleware - 示例中间件
 *
 * 在请求中注入自定义头，实际演示中不修改响应，仅记录日志。
 */
static int hello_middleware(http_request_t *req, cocoon_socket_t fd, void *user_data) {
    (void)req;
    (void)fd;
    (void)user_data;
    /* 仅返回 0 继续后续处理，不短路 */
    return 0;
}

/**
 * cocoon_plugin_init - 插件初始化入口
 */
int cocoon_plugin_init(void) {
    /* 注册中间件 */
    if (cocoon_middleware_register("hello", hello_middleware, NULL) != 0) {
        return -1;
    }
    return 0;
}

/**
 * cocoon_plugin_shutdown - 插件关闭
 */
void cocoon_plugin_shutdown(void) {
    cocoon_middleware_unregister("hello");
}

/**
 * cocoon_plugin_version - 插件版本
 */
const char *cocoon_plugin_version(void) {
    return "1.0.0";
}

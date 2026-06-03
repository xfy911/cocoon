/**
 * static.h - 静态资源服务模块接口
 *
 * @author xfy
 */

#ifndef COCOON_STATIC_H
#define COCOON_STATIC_H

#include "http.h"
#include <stdbool.h>

/**
 * send_all - 确保缓冲区全部发送
 *
 * 使用 write 循环发送，直到全部数据发送完毕或遇到不可恢复错误。
 * 内部辅助函数，也可被其他模块调用。
 *
 * @param fd socket 文件描述符
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return 0 成功，-1 失败
 */
int send_all(int fd, const char *buf, size_t len);

/**
 * static_serve_file - 服务单个静态文件
 *
 * 打开文件，根据请求判断是否需要发送部分内容（Range），
 * 然后通过 sendfile 或循环读取发送文件内容。
 *
 * @param fd 客户端 socket 文件描述符
 * @param req HTTP 请求
 * @param root_dir 静态资源根目录
 * @return COCOON_OK 成功，负值错误码
 */
int static_serve_file(int fd, const http_request_t *req, const char *root_dir);

/**
 * static_serve_directory - 生成目录列表 HTML
 *
 * 当请求路径是目录且不含默认 index.html 时，生成美观的目录浏览页面。
 *
 * @param fd 客户端 socket 文件描述符
 * @param req HTTP 请求
 * @param root_dir 静态资源根目录
 * @param real_path 文件系统上的真实路径
 * @return COCOON_OK 成功，负值错误码
 */
int static_serve_directory(int fd, const http_request_t *req,
                           const char *root_dir, const char *real_path);

/**
 * static_send_error - 发送 HTTP 错误响应
 *
 * @param fd 客户端 socket
 * @param status_code HTTP 状态码（如 404、403）
 * @param keep_alive 是否保持连接
 * @return COCOON_OK 成功
 */
int static_send_error(int fd, int status_code, bool keep_alive);

#endif /* COCOON_STATIC_H */

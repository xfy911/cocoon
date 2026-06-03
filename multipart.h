/**
 * multipart.h - multipart/form-data 解析模块
 *
 * 轻量级 multipart 解析器，支持文件上传场景。
 * 零外部依赖，只处理 Cocoon 需要的字段。
 *
 * @author xfy
 */

#ifndef COCOON_MULTIPART_H
#define COCOON_MULTIPART_H

#include <stddef.h>
#include <stdbool.h>

/**
 * multipart_part_t - 单个 multipart 部分
 */
typedef struct {
    char *name;         /**< 字段名 */
    char *filename;     /**< 文件名（文件上传时） */
    char *content_type; /**< 部分的 Content-Type */
    char *data;         /**< 部分数据 */
    size_t data_len;    /**< 数据长度 */
} multipart_part_t;

/**
 * multipart_parse - 解析 multipart/form-data 请求体
 *
 * @param body 请求体数据
 * @param body_len 请求体长度
 * @param boundary 边界字符串（不含前导 --）
 * @param parts 输出部分数组（需调用者 free）
 * @param num_parts 输出部分数量
 * @return 0 成功，-1 错误
 */
int multipart_parse(const char *body, size_t body_len, const char *boundary,
                    multipart_part_t **parts, int *num_parts);

/**
 * multipart_parts_free - 释放 multipart 解析结果
 *
 * @param parts 部分数组
 * @param num_parts 部分数量
 */
void multipart_parts_free(multipart_part_t *parts, int num_parts);

/**
 * multipart_extract_boundary - 从 Content-Type 提取 boundary
 *
 * @param content_type Content-Type 头值
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return true 成功提取
 */
bool multipart_extract_boundary(const char *content_type, char *buf, size_t buf_size);

#endif /* COCOON_MULTIPART_H */

/**
 * multipart.c - multipart/form-data 解析实现
 *
 * 轻量级解析器，支持文件上传。
 *
 * @author xfy
 */

#include "multipart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * multipart_extract_boundary - 从 Content-Type 提取 boundary
 */
bool multipart_extract_boundary(const char *content_type, char *buf, size_t buf_size) {
    if (!content_type || !buf || buf_size == 0) return false;

    const char *p = strstr(content_type, "boundary=");
    if (!p) return false;

    p += 9; /* skip "boundary=" */

    /* 跳过可能的引号 */
    size_t len = 0;
    if (*p == '"') {
        p++;
        while (p[len] && p[len] != '"') len++;
    } else {
        while (p[len] && p[len] != ';' && p[len] != ' ' && p[len] != '\r' && p[len] != '\n') len++;
    }

    if (len == 0 || len >= buf_size) return false;

    memcpy(buf, p, len);
    buf[len] = '\0';
    return true;
}

/**
 * find_line_end - 查找行尾（\r\n 或 \n）
 *
 * @param data 数据指针
 * @param len 剩余长度
 * @return 行尾后的位置偏移，未找到返回 len
 */
static size_t find_line_end(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') return i + 1;
    }
    return len;
}

/**
 * parse_content_disposition - 解析 Content-Disposition 头
 *
 * @param line 头行内容
 * @param name 输出 name 缓冲区
 * @param name_size name 缓冲区大小
 * @param filename 输出 filename 缓冲区
 * @param filename_size filename 缓冲区大小
 */
static void parse_content_disposition(const char *line,
                                      char *name, size_t name_size,
                                      char *filename, size_t filename_size) {
    const char *p = strstr(line, "name=");
    if (p) {
        p += 5;
        if (*p == '"') {
            p++;
            size_t i = 0;
            while (*p && *p != '"' && i < name_size - 1) {
                name[i++] = *p++;
            }
            name[i] = '\0';
        } else {
            size_t i = 0;
            while (*p && *p != ';' && *p != ' ' && *p != '\r' && *p != '\n' && i < name_size - 1) {
                name[i++] = *p++;
            }
            name[i] = '\0';
        }
    }

    p = strstr(line, "filename=");
    if (p) {
        p += 9;
        if (*p == '"') {
            p++;
            size_t i = 0;
            while (*p && *p != '"' && i < filename_size - 1) {
                filename[i++] = *p++;
            }
            filename[i] = '\0';
        } else {
            size_t i = 0;
            while (*p && *p != ';' && *p != ' ' && *p != '\r' && *p != '\n' && i < filename_size - 1) {
                filename[i++] = *p++;
            }
            filename[i] = '\0';
        }
    }
}

/**
 * multipart_parse - 解析 multipart/form-data 请求体
 */
int multipart_parse(const char *body, size_t body_len, const char *boundary,
                    multipart_part_t **parts, int *num_parts) {
    if (!body || body_len == 0 || !boundary || !parts || !num_parts) return -1;

    *parts = NULL;
    *num_parts = 0;

    /* 构建边界分隔符：--boundary\r\n */
    char delim[256];
    int delim_len = snprintf(delim, sizeof(delim), "--%s", boundary);
    if (delim_len < 0 || (size_t)delim_len >= sizeof(delim)) return -1;

    size_t b_len = (size_t)delim_len;

    /* 第一遍：统计部分数量 */
    int count = 0;
    const char *p = body;
    size_t remain = body_len;

    /* 跳过 preamble（边界前的内容） */
    while (remain >= b_len) {
        if (strncmp(p, delim, b_len) == 0) break;
        p++;
        remain--;
    }

    while (remain >= b_len) {
        if (strncmp(p, delim, b_len) == 0) {
            count++;
            p += b_len;
            remain -= b_len;
            /* 跳过 -- 结束标记 */
            if (remain >= 2 && strncmp(p, "--", 2) == 0) break;
            /* 跳过 \r\n 或 \n */
            if (remain >= 2 && p[0] == '\r' && p[1] == '\n') {
                p += 2;
                remain -= 2;
            } else if (remain >= 1 && p[0] == '\n') {
                p += 1;
                remain -= 1;
            }
        } else {
            p++;
            remain--;
        }
    }

    if (count == 0) return -1;

    /* 分配数组 */
    *parts = (multipart_part_t *)calloc((size_t)count, sizeof(multipart_part_t));
    if (!*parts) return -1;

    /* 第二遍：提取每个部分 */
    int idx = 0;
    p = body;
    remain = body_len;

    while (remain >= b_len && idx < count) {
        /* 查找下一个边界 */
        if (strncmp(p, delim, b_len) != 0) {
            p++;
            remain--;
            continue;
        }

        p += b_len;
        remain -= b_len;

        /* 检查结束标记 */
        if (remain >= 2 && strncmp(p, "--", 2) == 0) break;

        /* 跳过分隔线后的换行 */
        if (remain >= 2 && p[0] == '\r' && p[1] == '\n') {
            p += 2;
            remain -= 2;
        } else if (remain >= 1 && p[0] == '\n') {
            p += 1;
            remain -= 1;
        }

        /* 解析头部 */
        char name[256] = {0};
        char filename[256] = {0};
        char content_type[128] = {0};

        while (remain > 0) {
            size_t line_end = find_line_end(p, remain);
            size_t line_len = line_end;

            /* 空行表示头部结束 */
            if (line_len == 2 && p[0] == '\r' && p[1] == '\n') {
                p += 2;
                remain -= 2;
                break;
            }
            if (line_len == 1 && p[0] == '\n') {
                p += 1;
                remain -= 1;
                break;
            }

            /* 解析头部 */
            if (strncasecmp(p, "Content-Disposition:", 20) == 0) {
                parse_content_disposition(p, name, sizeof(name), filename, sizeof(filename));
            } else if (strncasecmp(p, "Content-Type:", 13) == 0) {
                const char *vp = p + 13;
                while (*vp == ' ' || *vp == '\t') vp++;
                size_t ct_len = line_len - (size_t)(vp - p);
                /* 去掉尾部 \r\n */
                if (ct_len >= 2 && vp[ct_len - 2] == '\r' && vp[ct_len - 1] == '\n') ct_len -= 2;
                else if (ct_len >= 1 && vp[ct_len - 1] == '\n') ct_len -= 1;
                if (ct_len > 0 && ct_len < sizeof(content_type)) {
                    memcpy(content_type, vp, ct_len);
                    content_type[ct_len] = '\0';
                }
            }

            p += line_end;
            remain -= line_end;
        }

        /* 查找数据结束位置（下一个边界） */
        const char *data_start = p;
        size_t data_remain = remain;
        size_t data_len = 0;

        while (data_remain >= b_len) {
            if (strncmp(p, delim, b_len) == 0) break;
            p++;
            data_remain--;
            data_len++;
        }

        /* 更新 remain 以反映已消费的数据 */
        remain = data_remain;

        /* 去掉数据尾部的 \r\n（如果存在） */
        if (data_len >= 2 && data_start[data_len - 2] == '\r' && data_start[data_len - 1] == '\n') {
            data_len -= 2;
        } else if (data_len >= 1 && data_start[data_len - 1] == '\n') {
            data_len -= 1;
        }

        /* 保存部分 */
        multipart_part_t *part = &(*parts)[idx];
        if (name[0]) {
            part->name = strdup(name);
        }
        if (filename[0]) {
            part->filename = strdup(filename);
        }
        if (content_type[0]) {
            part->content_type = strdup(content_type);
        }
        if (data_len > 0) {
            part->data = (char *)malloc(data_len + 1);
            if (part->data) {
                memcpy(part->data, data_start, data_len);
                part->data[data_len] = '\0';
                part->data_len = data_len;
            }
        }

        idx++;
    }

    *num_parts = idx;
    return 0;
}

/**
 * multipart_parts_free - 释放 multipart 解析结果
 */
void multipart_parts_free(multipart_part_t *parts, int num_parts) {
    if (!parts) return;
    for (int i = 0; i < num_parts; i++) {
        free(parts[i].name);
        free(parts[i].filename);
        free(parts[i].content_type);
        free(parts[i].data);
    }
    free(parts);
}



/**
 * config.c - JSON 配置文件解析实现
 *
 * 极简 JSON 解析器，只处理 cocoon 配置所需的字段：
 * - 字符串（root_dir, log_level）
 * - 整数（port, num_workers, max_connections, timeout_ms）
 * - 布尔值（threaded）
 *
 * 保持零依赖，不引入外部 JSON 库。
 *
 * @author xfy
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* === 内部：极简 JSON token 类型 === */
typedef enum {
    TOKEN_EOF,
    TOKEN_LBRACE,      /* { */
    TOKEN_RBRACE,      /* } */
    TOKEN_STRING,      /* "..." */
    TOKEN_NUMBER,      /* 123 */
    TOKEN_TRUE,        /* true */
    TOKEN_FALSE,       /* false */
    TOKEN_LBRACKET,  /* [ */
    TOKEN_RBRACKET,    /* ] */
    TOKEN_COMMA,       /* , */
    TOKEN_COLON,       /* : */
    TOKEN_INVALID      /* 错误 */
} token_type_t;

typedef struct {
    token_type_t type;
    const char *start;
    size_t len;
    int line;
} token_t;

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
    int line;
} parser_t;

/* === 内部：parser 辅助函数 === */

/**
 * parser_init - 初始化 JSON 解析器
 *
 * @param p 解析器状态结构
 * @param src JSON 源字符串
 * @param len 源字符串长度
 */
static void parser_init(parser_t *p, const char *src, size_t len) {
    p->src = src;
    p->pos = 0;
    p->len = len;
    p->line = 1;
}

/**
 * parser_skip_ws - 跳过空白字符和注释
 *
 * 支持空格、制表符、换行，以及 // 行注释。
 *
 * @param p 解析器状态结构
 */
static void parser_skip_ws(parser_t *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            p->pos++;
        } else if (c == '\n') {
            p->pos++;
            p->line++;
        } else if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '/') {
            /* 跳过 // 注释 */
            p->pos += 2;
            while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
        } else {
            break;
        }
    }
}

/**
 * parser_next_token - 读取下一个 JSON token
 *
 * 支持的 token 类型：字符串、数字、true、false、
 * 以及各种分隔符（{ } [ ] , :）。
 *
 * @param p 解析器状态结构
 * @return 下一个 token
 */
static token_t parser_next_token(parser_t *p) {
    parser_skip_ws(p);
    token_t t = {TOKEN_INVALID, NULL, 0, p->line};

    if (p->pos >= p->len) {
        t.type = TOKEN_EOF;
        return t;
    }

    const char *start = p->src + p->pos;
    char c = start[0];

    switch (c) {
        case '{': p->pos++; t.type = TOKEN_LBRACE; return t;
        case '}': p->pos++; t.type = TOKEN_RBRACE; return t;
        case '[': p->pos++; t.type = TOKEN_LBRACKET; return t;
        case ']': p->pos++; t.type = TOKEN_RBRACKET; return t;
        case ',': p->pos++; t.type = TOKEN_COMMA; return t;
        case ':': p->pos++; t.type = TOKEN_COLON; return t;
        case '"': {
            /* 字符串 */
            p->pos++; /* skip " */
            t.start = p->src + p->pos;
            while (p->pos < p->len && p->src[p->pos] != '"') {
                if (p->src[p->pos] == '\\' && p->pos + 1 < p->len) {
                    p->pos += 2; /* skip escaped */
                } else {
                    p->pos++;
                }
            }
            t.len = (size_t)(p->src + p->pos - t.start);
            if (p->pos < p->len) p->pos++; /* skip closing " */
            t.type = TOKEN_STRING;
            return t;
        }
        default: {
            if (isdigit(c) || c == '-') {
                /* 数字 */
                t.start = start;
                p->pos++;
                while (p->pos < p->len && (isdigit(p->src[p->pos]) || p->src[p->pos] == '.')) {
                    p->pos++;
                }
                t.len = (size_t)(p->src + p->pos - t.start);
                t.type = TOKEN_NUMBER;
                return t;
            } else if (strncmp(start, "true", 4) == 0) {
                p->pos += 4;
                t.type = TOKEN_TRUE;
                return t;
            } else if (strncmp(start, "false", 5) == 0) {
                p->pos += 5;
                t.type = TOKEN_FALSE;
                return t;
            }
            /* 未知 token */
            p->pos++;
            t.type = TOKEN_INVALID;
            return t;
        }
    }
}

/**
 * token_expect - 期望下一个 token 为指定类型
 *
 * @param p 解析器状态结构
 * @param expected 期望的 token 类型
 * @return true 匹配，false 不匹配
 */
static bool token_expect(parser_t *p, token_type_t expected) {
    token_t t = parser_next_token(p);
    return t.type == expected;
}

/**
 * token_str_dup - 将字符串 token 复制为 C 字符串
 *
 * 处理 JSON 字符串中的转义序列（\n \t \r \\ \"）。
 *
 * @param t 字符串 token
 * @return 新分配的 C 字符串，调用者负责释放；失败返回 NULL
 */
static char *token_str_dup(const token_t *t) {
    char *buf = (char *)malloc(t->len + 1);
    if (!buf) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < t->len; i++) {
        if (t->start[i] == '\\' && i + 1 < t->len) {
            char next = t->start[i + 1];
            switch (next) {
                case 'n': buf[j++] = '\n'; i++; break;
                case 't': buf[j++] = '\t'; i++; break;
                case 'r': buf[j++] = '\r'; i++; break;
                case '\\': buf[j++] = '\\'; i++; break;
                case '"': buf[j++] = '"'; i++; break;
                default: buf[j++] = t->start[i]; break;
            }
        } else {
            buf[j++] = t->start[i];
        }
    }
    buf[j] = '\0';
    return buf;
}

/**
 * token_to_long - 将数字 token 转换为长整型
 *
 * @param t 数字 token
 * @return 转换后的数值
 */
static long token_to_long(const token_t *t) {
    char buf[32] = {0};
    size_t n = t->len < 31 ? t->len : 31;
    memcpy(buf, t->start, n);
    return strtol(buf, NULL, 10);
}

/**
 * str_to_log_level - 将字符串转换为日志级别
 *
 * @param str 日志级别字符串（error/warn/info/debug）
 * @return 对应的日志级别枚举值，无效时返回 LOG_LEVEL_INFO
 */
static log_level_t str_to_log_level(const char *str) {
    if (strcmp(str, "error") == 0) return LOG_LEVEL_ERROR;
    if (strcmp(str, "warn") == 0) return LOG_LEVEL_WARN;
    if (strcmp(str, "info") == 0) return LOG_LEVEL_INFO;
    if (strcmp(str, "debug") == 0) return LOG_LEVEL_DEBUG;
    return LOG_LEVEL_INFO; /* 默认 */
}

/* === 公共 API === */

/**
 * config_load_from_file - 从 JSON 配置文件加载配置
 *
 * 解析 cocoon.json 格式，支持以下字段：
 *   root_dir, port, threaded, num_workers, max_connections, timeout_ms,
 *   log_level, gzip_enabled, brotli_enabled, tls_cert, tls_key, tls_enabled,
 *   access_log, cors_enabled, auth_user, auth_pass, rate_limit,
 *   plugins（字符串或数组）, proxies（对象数组）
 *
 * 未知字段将被静默忽略，便于向后兼容。
 *
 * @param path 配置文件路径
 * @param config 输出配置结构体
 * @return true 成功，false 失败（会输出错误信息到 stderr）
 */
bool config_load_from_file(const char *path, cocoon_config_t *config) {
    if (!path || !config) return false;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "[Config] 无法打开配置文件: %s (%s)\n", path, strerror(errno));
        return false;
    }

    /* 读取文件内容 */
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0 || size > 65536) { /* 限制 64KB */
        fclose(fp);
        fprintf(stderr, "[Config] 配置文件过大或为空\n");
        return false;
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return false;
    }

    size_t read = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[read] = '\0';

    parser_t p;
    parser_init(&p, buf, read);

    /* 期望 { */
    if (!token_expect(&p, TOKEN_LBRACE)) {
        fprintf(stderr, "[Config] 配置文件格式错误: 期望 '{'\n");
        free(buf);
        return false;
    }

    /* 解析键值对 */
    while (1) {
        token_t key = parser_next_token(&p);
        if (key.type == TOKEN_RBRACE) break; /* 空对象 {} */
        if (key.type != TOKEN_STRING) {
            fprintf(stderr, "[Config] 第 %d 行: 期望字符串键\n", key.line);
            free(buf);
            return false;
        }

        if (!token_expect(&p, TOKEN_COLON)) {
            fprintf(stderr, "[Config] 第 %d 行: 键后缺少 ':'\n", key.line);
            free(buf);
            return false;
        }

        token_t val = parser_next_token(&p);
        char *key_str = token_str_dup(&key);

        if (strcmp(key_str, "root_dir") == 0 && val.type == TOKEN_STRING) {
            char *v = token_str_dup(&val);
            if (v) {
                free((void *)config->root_dir); /* 释放旧的 */
                config->root_dir = v;
            }
        } else if (strcmp(key_str, "port") == 0 && val.type == TOKEN_NUMBER) {
            long v = token_to_long(&val);
            if (v > 0 && v < 65536) config->port = (uint16_t)v;
        } else if (strcmp(key_str, "threaded") == 0) {
            if (val.type == TOKEN_TRUE) config->threaded = true;
            else if (val.type == TOKEN_FALSE) config->threaded = false;
        } else if (strcmp(key_str, "num_workers") == 0 && val.type == TOKEN_NUMBER) {
            long v = token_to_long(&val);
            if (v > 0 && v < 1024) config->num_workers = (uint32_t)v;
        } else if (strcmp(key_str, "max_connections") == 0 && val.type == TOKEN_NUMBER) {
            long v = token_to_long(&val);
            if (v >= 0 && v < 1000000) config->max_connections = (uint32_t)v;
        } else if (strcmp(key_str, "timeout_ms") == 0 && val.type == TOKEN_NUMBER) {
            long v = token_to_long(&val);
            if (v >= 0 && v < 3600000) config->timeout_ms = (uint32_t)v;
        } else if (strcmp(key_str, "log_level") == 0 && val.type == TOKEN_STRING) {
            char *v = token_str_dup(&val);
            if (v) {
                config->log_level = str_to_log_level(v);
                free(v);
            }
        } else if (strcmp(key_str, "gzip_enabled") == 0) {
            if (val.type == TOKEN_TRUE) config->gzip_enabled = true;
            else if (val.type == TOKEN_FALSE) config->gzip_enabled = false;
        } else if (strcmp(key_str, "brotli_enabled") == 0) {
            if (val.type == TOKEN_TRUE) config->brotli_enabled = true;
            else if (val.type == TOKEN_FALSE) config->brotli_enabled = false;
        } else if (strcmp(key_str, "tls_cert") == 0 && val.type == TOKEN_STRING) {
            char *v = token_str_dup(&val);
            if (v) {
                free((void *)config->tls_cert);
                config->tls_cert = v;
            }
        } else if (strcmp(key_str, "tls_key") == 0 && val.type == TOKEN_STRING) {
            char *v = token_str_dup(&val);
            if (v) {
                free((void *)config->tls_key);
                config->tls_key = v;
            }
        } else if (strcmp(key_str, "tls_enabled") == 0) {
            if (val.type == TOKEN_TRUE) config->tls_enabled = true;
            else if (val.type == TOKEN_FALSE) config->tls_enabled = false;
        } else if (strcmp(key_str, "access_log") == 0 && val.type == TOKEN_STRING) {
            char *v = token_str_dup(&val);
            if (v) {
                free((void *)config->access_log_path);
                config->access_log_path = v;
            }
        } else if (strcmp(key_str, "cors_enabled") == 0) {
            if (val.type == TOKEN_TRUE) config->cors_enabled = true;
            else if (val.type == TOKEN_FALSE) config->cors_enabled = false;
        } else if (strcmp(key_str, "auth_user") == 0 && val.type == TOKEN_STRING) {
            char *v = token_str_dup(&val);
            if (v) {
                free((void *)config->auth_user);
                config->auth_user = v;
            }
        } else if (strcmp(key_str, "auth_pass") == 0 && val.type == TOKEN_STRING) {
            char *v = token_str_dup(&val);
            if (v) {
                free((void *)config->auth_pass);
                config->auth_pass = v;
            }
        } else if (strcmp(key_str, "rate_limit") == 0 && val.type == TOKEN_NUMBER) {
            long v = token_to_long(&val);
            if (v >= 0 && v < 1000000) config->rate_limit = (uint32_t)v;
        } else if (strcmp(key_str, "plugins") == 0) {
            if (val.type == TOKEN_STRING) {
                char *v = token_str_dup(&val);
                if (v && config->num_plugins < COCOON_MAX_PLUGINS) {
                    config->plugins[config->num_plugins++] = v;
                }
            } else if (val.type == TOKEN_LBRACKET) {
                /* 解析字符串数组 */
                while (1) {
                    token_t item = parser_next_token(&p);
                    if (item.type == TOKEN_RBRACKET) break;
                    if (item.type == TOKEN_STRING) {
                        char *v = token_str_dup(&item);
                        if (v && config->num_plugins < COCOON_MAX_PLUGINS) {
                            config->plugins[config->num_plugins++] = v;
                        }
                    } else {
                        fprintf(stderr, "[Config] 第 %d 行: plugins 数组期望字符串项\n", item.line);
                    }
                    token_t sep = parser_next_token(&p);
                    if (sep.type == TOKEN_RBRACKET) break;
                    if (sep.type != TOKEN_COMMA) {
                        fprintf(stderr, "[Config] 第 %d 行: plugins 数组期望 ',' 或 ']'\n", sep.line);
                        break;
                    }
                }
            } else {
                fprintf(stderr, "[Config] 第 %d 行: plugins 期望字符串或数组\n", val.line);
            }
        } else if (strcmp(key_str, "proxies") == 0) {
            if (val.type == TOKEN_LBRACKET) {
                /* 解析对象数组 */
                while (1) {
                    token_t item = parser_next_token(&p);
                    if (item.type == TOKEN_RBRACKET) break;
                    if (item.type == TOKEN_LBRACE) {
                        char prefix[256] = {0};
                        char target[256] = {0};
                        /* 解析对象内的键值对 */
                        while (1) {
                            token_t pkey = parser_next_token(&p);
                            if (pkey.type == TOKEN_RBRACE) break;
                            if (pkey.type != TOKEN_STRING) {
                                fprintf(stderr, "[Config] 第 %d 行: proxy 对象期望字符串键\n", pkey.line);
                                break;
                            }
                            if (!token_expect(&p, TOKEN_COLON)) break;
                            token_t pval = parser_next_token(&p);
                            char *pk = token_str_dup(&pkey);
                            if (strcmp(pk, "prefix") == 0 && pval.type == TOKEN_STRING) {
                                char *v = token_str_dup(&pval);
                                if (v) { strncpy(prefix, v, sizeof(prefix)-1); free(v); }
                            } else if (strcmp(pk, "target") == 0 && pval.type == TOKEN_STRING) {
                                char *v = token_str_dup(&pval);
                                if (v) { strncpy(target, v, sizeof(target)-1); free(v); }
                            } else if (strcmp(pk, "pool_size") == 0 && pval.type == TOKEN_NUMBER) {
                                char *v = token_str_dup(&pval);
                                if (v) { config->proxies[config->num_proxies].pool_size = (uint32_t)atoi(v); free(v); }
                            }
                            free(pk);
                            token_t psep = parser_next_token(&p);
                            if (psep.type == TOKEN_RBRACE) break;
                            if (psep.type != TOKEN_COMMA) {
                                fprintf(stderr, "[Config] 第 %d 行: proxy 对象期望 ',' 或 '}'\n", psep.line);
                                break;
                            }
                        }
                        if (prefix[0] && target[0] && config->num_proxies < COCOON_MAX_PROXY_RULES) {
                            size_t prefix_len = strlen(prefix);
                            if (prefix_len >= sizeof(config->proxies[0].prefix)) prefix_len = sizeof(config->proxies[0].prefix) - 1;
                            memcpy(config->proxies[config->num_proxies].prefix, prefix, prefix_len);
                            config->proxies[config->num_proxies].prefix[prefix_len] = '\0';
                            size_t target_len = strlen(target);
                            if (target_len >= sizeof(config->proxies[0].target)) target_len = sizeof(config->proxies[0].target) - 1;
                            memcpy(config->proxies[config->num_proxies].target, target, target_len);
                            config->proxies[config->num_proxies].target[target_len] = '\0';
                            if (config->proxies[config->num_proxies].pool_size == 0) {
                                config->proxies[config->num_proxies].pool_size = 4; /* 默认连接池大小 */
                            }
                            config->num_proxies++;
                        }
                    } else {
                        fprintf(stderr, "[Config] 第 %d 行: proxies 数组期望对象项\n", item.line);
                    }
                    token_t sep = parser_next_token(&p);
                    if (sep.type == TOKEN_RBRACKET) break;
                    if (sep.type != TOKEN_COMMA) {
                        fprintf(stderr, "[Config] 第 %d 行: proxies 数组期望 ',' 或 ']'\n", sep.line);
                        break;
                    }
                }
            } else {
                fprintf(stderr, "[Config] 第 %d 行: proxies 期望数组\n", val.line);
            }
        } /* 其他字段：忽略（未来扩展预留） */

        free(key_str);

        /* 检查逗号或右括号 */
        token_t sep = parser_next_token(&p);
        if (sep.type == TOKEN_RBRACE) break;
        if (sep.type != TOKEN_COMMA) {
            fprintf(stderr, "[Config] 第 %d 行: 期望 ',' 或 '}'\n", sep.line);
            free(buf);
            return false;
        }
    }

    free(buf);
    return true;
}

/**
 * config_merge - 将命令行参数合并到基础配置
 *
 * 命令行显式指定的值覆盖配置文件中的值。
 * 对于字符串字段，会释放旧值并复制新值。
 *
 * @param base 基础配置（通常来自配置文件）
 * @param cmdline 命令行配置
 * @param has_root_dir  是否显式指定 root_dir
 * @param has_port      是否显式指定 port
 * @param has_workers   是否显式指定 num_workers
 * @param has_max_conn  是否显式指定 max_connections
 * @param has_timeout   是否显式指定 timeout_ms
 * @param has_log_level 是否显式指定 log_level
 * @param has_gzip_enabled    是否显式指定 gzip_enabled
 * @param has_brotli_enabled  是否显式指定 brotli_enabled
 * @param has_tls_cert  是否显式指定 tls_cert
 * @param has_tls_key   是否显式指定 tls_key
 * @param has_tls_enabled 是否显式指定 tls_enabled
 * @param has_access_log 是否显式指定 access_log
 * @param has_cors_enabled 是否显式指定 cors_enabled
 * @param has_auth_user 是否显式指定 auth_user
 * @param has_auth_pass 是否显式指定 auth_pass
 * @param has_rate_limit 是否显式指定 rate_limit
 * @param has_plugins   是否显式指定 plugins
 */
void config_merge(cocoon_config_t *base, const cocoon_config_t *cmdline,
                  bool has_root_dir, bool has_port, bool has_workers,
                  bool has_max_conn, bool has_timeout, bool has_log_level,
                  bool has_gzip_enabled, bool has_brotli_enabled,
                  bool has_tls_cert, bool has_tls_key, bool has_tls_enabled,
                  bool has_access_log,
                  bool has_cors_enabled, bool has_auth_user, bool has_auth_pass,
                  bool has_rate_limit,
                  bool has_plugins) {
    if (!base || !cmdline) return;

    /* 命令行显式指定的值覆盖配置文件 */
    if (has_root_dir && cmdline->root_dir) {
        free((void *)base->root_dir);
        base->root_dir = strdup(cmdline->root_dir);
    }
    if (has_port) base->port = cmdline->port;
    if (has_workers) base->num_workers = cmdline->num_workers;
    if (has_max_conn) base->max_connections = cmdline->max_connections;
    if (has_timeout) base->timeout_ms = cmdline->timeout_ms;
    if (has_log_level) base->log_level = cmdline->log_level;
    if (has_gzip_enabled) base->gzip_enabled = cmdline->gzip_enabled;
    if (has_brotli_enabled) base->brotli_enabled = cmdline->brotli_enabled;
    if (has_tls_cert && cmdline->tls_cert) {
        free((void *)base->tls_cert);
        base->tls_cert = strdup(cmdline->tls_cert);
    }
    if (has_tls_key && cmdline->tls_key) {
        free((void *)base->tls_key);
        base->tls_key = strdup(cmdline->tls_key);
    }
    if (has_tls_enabled) base->tls_enabled = cmdline->tls_enabled;
    if (has_access_log && cmdline->access_log_path) {
        free((void *)base->access_log_path);
        base->access_log_path = strdup(cmdline->access_log_path);
    }
    if (has_cors_enabled) base->cors_enabled = cmdline->cors_enabled;
    if (has_auth_user && cmdline->auth_user) {
        free((void *)base->auth_user);
        base->auth_user = strdup(cmdline->auth_user);
    }
    if (has_auth_pass && cmdline->auth_pass) {
        free((void *)base->auth_pass);
        base->auth_pass = strdup(cmdline->auth_pass);
    }
    if (has_rate_limit) base->rate_limit = cmdline->rate_limit;
    if (has_plugins) {
        for (size_t i = 0; i < cmdline->num_plugins && i < COCOON_MAX_PLUGINS; i++) {
            if (base->num_plugins < COCOON_MAX_PLUGINS) {
                base->plugins[base->num_plugins++] = strdup(cmdline->plugins[i]);
            }
        }
    }
    /* threaded 是 flag 参数，命令行指定了就用命令行的 */
    if (cmdline->threaded) base->threaded = true;
}

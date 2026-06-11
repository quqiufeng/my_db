#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 * 简易 JSON 构造器
 *
 * 用于构建 JSON 字符串，避免 sprintf 的缓冲区溢出问题。
 * 用法：
 *   json_t j;
 *   json_init(&j, buf, sizeof(buf));
 *   json_object(&j);
 *   json_string(&j, "id", "node1");
 *   json_int(&j, "epoch", 3);
 *   json_object_end(&j);
 *   printf("%s", j.buf);
 * =================================================================== */

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
    int    item_count;   /* 当前层级已写入的键值对数量 */
    int    depth;
} json_t;

void json_init(json_t *j, char *buf, size_t size) {
    j->buf = buf;
    j->size = size;
    j->pos = 0;
    j->item_count = 0;
    j->depth = 0;
    buf[0] = '\0';
}

static void json_append(json_t *j, const char *s) {
    size_t len = strlen(s);
    if (j->pos + len < j->size) {
        memcpy(j->buf + j->pos, s, len);
        j->pos += len;
        j->buf[j->pos] = '\0';
    }
}

void json_object(json_t *j) {
    if (j->item_count > 0) json_append(j, ",");
    json_append(j, "{");
    j->depth++;
    j->item_count = 0;
}

void json_object_end(json_t *j) {
    j->depth--;
    j->item_count = 0;
    json_append(j, "}");
}

void json_array(json_t *j) {
    if (j->item_count > 0) json_append(j, ",");
    json_append(j, "[");
    j->depth++;
    j->item_count = 0;
}

void json_array_end(json_t *j) {
    j->depth--;
    json_append(j, "]");
}

void json_key(json_t *j, const char *key) {
    if (j->item_count > 0) json_append(j, ",");
    char buf[512];
    snprintf(buf, sizeof(buf), "\"%s\":", key);
    json_append(j, buf);
    j->item_count++;
}

void json_string(json_t *j, const char *key, const char *val) {
    if (j->item_count > 0) json_append(j, ",");
    char buf[2048];
    snprintf(buf, sizeof(buf), "\"%s\":\"%s\"", key, val ? val : "");
    json_append(j, buf);
    j->item_count++;
}

void json_int(json_t *j, const char *key, int val) {
    if (j->item_count > 0) json_append(j, ",");
    char buf[256];
    snprintf(buf, sizeof(buf), "\"%s\":%d", key, val);
    json_append(j, buf);
    j->item_count++;
}

void json_double(json_t *j, const char *key, double val) {
    if (j->item_count > 0) json_append(j, ",");
    char buf[256];
    snprintf(buf, sizeof(buf), "\"%s\":%.2f", key, val);
    json_append(j, buf);
    j->item_count++;
}

void json_long(json_t *j, const char *key, long val) {
    if (j->item_count > 0) json_append(j, ",");
    char buf[256];
    snprintf(buf, sizeof(buf), "\"%s\":%ld", key, val);
    json_append(j, buf);
    j->item_count++;
}

/* ===================================================================
 * 简易 JSON 解析器（基于 sscanf 的封装）
 * =================================================================== */

/**
 * json_get_string - 从 JSON 中提取字符串字段
 * @json: JSON 字符串
 * @key:  字段名
 * @val:  输出缓冲区
 * @size: 缓冲区大小
 * 返回 0 成功，-1 未找到
 */
int json_get_string(const char *json, const char *key, char *val, size_t size) {
    char pattern[512];
    snprintf(pattern, sizeof(pattern),
             "\"%s\":\"%%%zu[^\"]\"", key, size - 1);
    return (sscanf(json, pattern, val) == 1) ? 0 : -1;
}

/**
 * json_get_int - 从 JSON 中提取整数字段
 */
int json_get_int(const char *json, const char *key, int *val) {
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "\"%s\":%%d", key);
    return (sscanf(json, pattern, val) == 1) ? 0 : -1;
}

/**
 * json_get_double - 从 JSON 中提取浮点数字段
 */
int json_get_double(const char *json, const char *key, double *val) {
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "\"%s\":%%lf", key);
    return (sscanf(json, pattern, val) == 1) ? 0 : -1;
}

/**
 * json_get_long - 从 JSON 中提取长整数字段
 */
int json_get_long(const char *json, const char *key, long *val) {
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "\"%s\":%%ld", key);
    return (sscanf(json, pattern, val) == 1) ? 0 : -1;
}

#ifndef JSON_H
#define JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
void json_escape(const char *in, char *out, size_t out_size);

#endif

/* ===================================================================
 * 简易 JSON 构造器
 * =================================================================== */
typedef struct {
    char   *buf;
    size_t  size;
    size_t  pos;
    int     item_count;
    int     depth;
} json_t;

void json_init(json_t *j, char *buf, size_t size);
void json_object(json_t *j);
void json_object_end(json_t *j);
void json_array(json_t *j);
void json_array_end(json_t *j);
void json_key(json_t *j, const char *key);
void json_string(json_t *j, const char *key, const char *val);
void json_int(json_t *j, const char *key, int val);
void json_double(json_t *j, const char *key, double val);
void json_long(json_t *j, const char *key, long val);

/* ===================================================================
 * 简易 JSON 解析器
 * =================================================================== */
int json_get_string(const char *json, const char *key, char *val, size_t size);
int json_get_int(const char *json, const char *key, int *val);
int json_get_double(const char *json, const char *key, double *val);
int json_get_long(const char *json, const char *key, long *val);

#ifdef __cplusplus
}
void json_escape(const char *in, char *out, size_t out_size);

#endif

void json_escape(const char *in, char *out, size_t out_size);

#endif /* JSON_H */

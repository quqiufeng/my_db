#include "cache_http_server.h"
#include "cache_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>

#define HTTP_SERVER_BACKLOG 32
#define HTTP_SERVER_BUF_SIZE 65536
#define HTTP_MAX_PATH 2048
#define HTTP_MAX_BODY (1024 * 1024)  // 1MB max body

static volatile int g_http_server_running = 0;
static int g_http_server_fd = -1;

// ====== HTTP 请求解析 ======

typedef enum {
    HTTP_GET,
    HTTP_PUT,
    HTTP_POST,
    HTTP_DELETE,
    HTTP_UNKNOWN
} http_method_t;

typedef struct {
    http_method_t method;
    char path[HTTP_MAX_PATH];
    char query[HTTP_MAX_PATH];
    size_t content_length;
    char* body;
    size_t body_len;
} http_request_t;

typedef struct {
    int status_code;
    const char* content_type;
    char* body;
    size_t body_len;
} http_response_t;

static http_method_t parse_method(const char* method_str) {
    if (strcmp(method_str, "GET") == 0) return HTTP_GET;
    if (strcmp(method_str, "PUT") == 0) return HTTP_PUT;
    if (strcmp(method_str, "POST") == 0) return HTTP_POST;
    if (strcmp(method_str, "DELETE") == 0) return HTTP_DELETE;
    return HTTP_UNKNOWN;
}

static int parse_request(const char* buf, size_t len, http_request_t* req) {
    memset(req, 0, sizeof(*req));
    req->content_length = 0;
    
    // 找第一行的结束
    const char* line_end = strstr(buf, "\r\n");
    if (!line_end) return -1;
    
    // 解析请求行: METHOD PATH HTTP/1.1
    char method_str[16] = {0};
    char path_raw[HTTP_MAX_PATH] = {0};
    
    if (sscanf(buf, "%15s %2047s HTTP/", method_str, path_raw) != 2) {
        return -1;
    }
    
    req->method = parse_method(method_str);
    if (req->method == HTTP_UNKNOWN) return -1;
    
    // 分离 path 和 query
    char* q = strchr(path_raw, '?');
    if (q) {
        *q = '\0';
        strncpy(req->path, path_raw, HTTP_MAX_PATH - 1);
        strncpy(req->query, q + 1, HTTP_MAX_PATH - 1);
    } else {
        strncpy(req->path, path_raw, HTTP_MAX_PATH - 1);
    }
    
    // 解析 headers
    const char* headers_start = line_end + 2;
    const char* body_start = strstr(headers_start, "\r\n\r\n");
    if (!body_start) return -1;
    body_start += 4;
    
    // 查找 Content-Length
    const char* cl_header = strcasestr(headers_start, "Content-Length:");
    if (cl_header && cl_header < body_start) {
        req->content_length = (size_t)atoll(cl_header + 15);
    }
    
    // body
    size_t header_len = body_start - buf;
    if (len > header_len) {
        req->body_len = len - header_len;
        if (req->body_len > req->content_length) {
            req->body_len = req->content_length;
        }
        req->body = (char*)body_start;
    }
    
    return 0;
}

// URL decode
static int url_decode(const char* src, char* dst, size_t dst_size) {
    size_t i = 0, j = 0;
    while (src[i] && j < dst_size - 1) {
        if (src[i] == '%' && src[i+1] && src[i+2] &&
            isxdigit((unsigned char)src[i+1]) && isxdigit((unsigned char)src[i+2])) {
            char hex[3] = {src[i+1], src[i+2], '\0'};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
    return (int)j;
}

// ====== JSON 辅助函数 ======

static void json_escape(const char* src, char* dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
        switch (src[i]) {
            case '"':  if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = '"'; } break;
            case '\\': if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = '\\'; } break;
            case '\b': if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = 'b'; } break;
            case '\f': if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = 'f'; } break;
            case '\n': if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = 'n'; } break;
            case '\r': if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = 'r'; } break;
            case '\t': if (j + 2 < dst_size) { dst[j++] = '\\'; dst[j++] = 't'; } break;
            default:   dst[j++] = src[i]; break;
        }
    }
    dst[j] = '\0';
}

static void send_json_response(int fd, int status_code, const char* json_body) {
    const char* status_text = "OK";
    switch (status_code) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 400: status_text = "Bad Request"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 413: status_text = "Payload Too Large"; break;
        case 500: status_text = "Internal Server Error"; break;
    }
    
    size_t body_len = json_body ? strlen(json_body) : 0;
    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: keep-alive\r\n"
             "\r\n",
             status_code, status_text, body_len);
    
    send(fd, header, strlen(header), MSG_NOSIGNAL);
    if (body_len > 0) {
        send(fd, json_body, body_len, MSG_NOSIGNAL);
    }
}

static void send_error(int fd, int status_code, const char* message) {
    char json[512];
    char escaped[256];
    json_escape(message, escaped, sizeof(escaped));
    snprintf(json, sizeof(json), "{\"status\":\"error\",\"error\":\"%s\"}", escaped);
    send_json_response(fd, status_code, json);
}

static void send_success(int fd, const char* data_json) {
    if (data_json) {
        char json[HTTP_SERVER_BUF_SIZE];
        snprintf(json, sizeof(json), "{\"status\":\"ok\",\"data\":%s}", data_json);
        send_json_response(fd, 200, json);
    } else {
        send_json_response(fd, 200, "{\"status\":\"ok\"}");
    }
}

// ====== 简单的 JSON 解析辅助 ======

static const char* json_find_string(const char* json, const char* key, char* out, size_t out_size) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return NULL;
    
    p = strchr(p + strlen(search), '"');
    if (!p) return NULL;
    p++;
    
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case '"': case '\\': case '/': out[i++] = *p; break;
                case 'b': out[i++] = '\b'; break;
                case 'f': out[i++] = '\f'; break;
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return out;
}

static int json_find_int(const char* json, const char* key, int64_t* out) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return -1;
    
    p = strchr(p + strlen(search), ':');
    if (!p) return -1;
    p++;
    while (*p && isspace(*p)) p++;
    
    *out = atoll(p);
    return 0;
}

// ====== 路由处理 ======

static void handle_health(int fd, cache_t* cache) {
    (void)cache;
    send_success(fd, "{\"status\":\"healthy\"}");
}

static void handle_stats(int fd, cache_t* cache) {
    char json[512];
    snprintf(json, sizeof(json),
             "{\"entries\":%zu,\"memory_used\":%zu,\"memory_max\":%zu}",
             cache_count(cache),
             cache_memory_used(cache),
             cache_memory_max(cache));
    send_success(fd, json);
}

static void handle_cache_get(int fd, cache_t* cache, const char* key) {
    char decoded_key[CACHE_MAX_KEY_LEN];
    url_decode(key, decoded_key, sizeof(decoded_key));
    
    const char* value = cache_get(cache, decoded_key);
    if (!value) {
        send_error(fd, 404, "key not found");
        return;
    }
    
    char escaped[HTTP_SERVER_BUF_SIZE];
    json_escape(value, escaped, sizeof(escaped));
    char json[HTTP_SERVER_BUF_SIZE + 64];
    snprintf(json, sizeof(json), "{\"key\":\"%s\",\"value\":\"%s\"}", decoded_key, escaped);
    send_success(fd, json);
}

static void handle_cache_put(int fd, cache_t* cache, const char* key, const char* body) {
    char decoded_key[CACHE_MAX_KEY_LEN];
    url_decode(key, decoded_key, sizeof(decoded_key));
    
    if (!body || body[0] == '\0') {
        send_error(fd, 400, "missing request body");
        return;
    }
    
    // 解析 JSON body: { "value": "...", "ttl_ms": 1234 }
    char value[CACHE_MAX_VALUE_LEN];
    if (!json_find_string(body, "value", value, sizeof(value))) {
        send_error(fd, 400, "missing 'value' field in request body");
        return;
    }
    
    int64_t ttl_ms = 0;
    json_find_int(body, "ttl_ms", &ttl_ms);
    
    int ret = cache_set(cache, decoded_key, value, (uint64_t)ttl_ms);
    if (ret == CACHE_OK) {
        send_success(fd, NULL);
    } else if (ret == CACHE_ERR_NOMEM) {
        send_error(fd, 413, "memory limit exceeded");
    } else {
        send_error(fd, 500, "set failed");
    }
}

static void handle_cache_delete(int fd, cache_t* cache, const char* key) {
    char decoded_key[CACHE_MAX_KEY_LEN];
    url_decode(key, decoded_key, sizeof(decoded_key));
    
    int ret = cache_del(cache, decoded_key);
    if (ret == CACHE_OK) {
        send_success(fd, NULL);
    } else {
        send_error(fd, 404, "key not found");
    }
}

static void handle_cache_exists(int fd, cache_t* cache, const char* key) {
    char decoded_key[CACHE_MAX_KEY_LEN];
    url_decode(key, decoded_key, sizeof(decoded_key));
    
    int exists = cache_exists(cache, decoded_key);
    char json[64];
    snprintf(json, sizeof(json), "{\"exists\":%s}", exists ? "true" : "false");
    send_success(fd, json);
}

static void handle_search(int fd, cache_t* cache, const char* query) {
    // 解析 query string: pattern=...&type=prefix|regex|fuzzy|tag&max_results=...
    char pattern[1024] = {0};
    char type[32] = "prefix";
    int max_results = 100;
    
    // 简单的 query string 解析
    char query_copy[HTTP_MAX_PATH];
    strncpy(query_copy, query, sizeof(query_copy) - 1);
    query_copy[sizeof(query_copy) - 1] = '\0';
    
    char* saveptr = NULL;
    char* param = strtok_r(query_copy, "&", &saveptr);
    while (param) {
        char* eq = strchr(param, '=');
        if (eq) {
            *eq = '\0';
            if (strcmp(param, "pattern") == 0 || strcmp(param, "q") == 0) {
                url_decode(eq + 1, pattern, sizeof(pattern));
            } else if (strcmp(param, "type") == 0) {
                url_decode(eq + 1, type, sizeof(type));
            } else if (strcmp(param, "max_results") == 0) {
                max_results = atoi(eq + 1);
            }
        }
        param = strtok_r(NULL, "&", &saveptr);
    }
    
    if (pattern[0] == '\0') {
        send_error(fd, 400, "missing 'pattern' parameter");
        return;
    }
    
    if (max_results <= 0 || max_results > 10000) {
        max_results = 100;
    }
    
    cache_search_options_t opts = cache_search_options_default();
    opts.max_results = max_results;
    
    cache_result_t* results = NULL;
    size_t count = 0;
    int ret = -1;
    
    if (strcasecmp(type, "prefix") == 0) {
        ret = cache_search_prefix(cache, pattern, &opts, &results, &count);
    } else if (strcasecmp(type, "regex") == 0) {
        ret = cache_search_regex(cache, pattern, &opts, &results, &count);
    } else if (strcasecmp(type, "fuzzy") == 0) {
        ret = cache_search_fuzzy(cache, pattern, &opts, &results, &count);
    } else if (strcasecmp(type, "tag") == 0) {
        ret = cache_search_tag(cache, pattern, &opts, &results, &count);
    } else {
        send_error(fd, 400, "invalid search type");
        return;
    }
    
    if (ret != CACHE_OK) {
        send_error(fd, 500, "search failed");
        return;
    }
    
    // 构建 JSON 结果
    char* json = malloc(HTTP_SERVER_BUF_SIZE);
    if (!json) {
        cache_results_free(results);
        send_error(fd, 500, "out of memory");
        return;
    }
    
    size_t pos = 0;
    pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos,
                    "{\"count\":%zu,\"results\":[", count);
    
    for (size_t i = 0; i < count && pos < HTTP_SERVER_BUF_SIZE - 256; i++) {
        char key_escaped[1024];
        char value_escaped[CACHE_MAX_VALUE_LEN];
        json_escape(results[i].key, key_escaped, sizeof(key_escaped));
        json_escape(results[i].value, value_escaped, sizeof(value_escaped));
        
        if (i > 0) pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos, ",");
        pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos,
                        "{\"key\":\"%s\",\"value\":\"%s\",\"score\":%.4f}",
                        key_escaped, value_escaped, results[i].score);
    }
    
    pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos, "]}");
    
    send_success(fd, json);
    free(json);
    cache_results_free(results);
}

static void handle_batch(int fd, cache_t* cache, const char* body) {
    if (!body || body[0] == '\0') {
        send_error(fd, 400, "missing request body");
        return;
    }
    
    // 解析 JSON 数组: [{"key":"...","value":"...","ttl_ms":1234}, ...]
    // 简单实现：统计 { 的数量作为 batch size
    size_t item_count = 0;
    for (const char* p = body; *p; p++) {
        if (*p == '{') item_count++;
    }
    
    if (item_count == 0) {
        send_error(fd, 400, "empty batch");
        return;
    }
    
    cache_batch_item_t* items = calloc(item_count, sizeof(cache_batch_item_t));
    if (!items) {
        send_error(fd, 500, "out of memory");
        return;
    }
    
    size_t parsed = 0;
    const char* p = body;
    while (*p && parsed < item_count) {
        p = strchr(p, '{');
        if (!p) break;
        p++;
        
        // 找到匹配的 }
        const char* end = p;
        int depth = 1;
        while (*end && depth > 0) {
            if (*end == '{') depth++;
            else if (*end == '}') depth--;
            end++;
        }
        if (depth != 0) break;
        
        size_t obj_len = end - p - 1;
        char* obj = malloc(obj_len + 1);
        if (!obj) break;
        strncpy(obj, p, obj_len);
        obj[obj_len] = '\0';
        
        char key[CACHE_MAX_KEY_LEN];
        char value[CACHE_MAX_VALUE_LEN];
        int64_t ttl_ms = 0;
        
        if (json_find_string(obj, "key", key, sizeof(key)) &&
            json_find_string(obj, "value", value, sizeof(value))) {
            items[parsed].key = strdup(key);
            items[parsed].value = strdup(value);
            json_find_int(obj, "ttl_ms", &ttl_ms);
            items[parsed].ttl_ms = (uint64_t)ttl_ms;
            parsed++;
        }
        
        free(obj);
        p = end;
    }
    
    if (parsed == 0) {
        free(items);
        send_error(fd, 400, "no valid items in batch");
        return;
    }
    
    int ret = cache_batch_set(cache, items, parsed);
    
    // 清理
    for (size_t i = 0; i < parsed; i++) {
        free((void*)items[i].key);
        free((void*)items[i].value);
    }
    free(items);
    
    if (ret == CACHE_OK) {
        char json[64];
        snprintf(json, sizeof(json), "{\"count\":%zu}", parsed);
        send_success(fd, json);
    } else {
        send_error(fd, 500, "batch set failed");
    }
}

static void handle_namespaces(int fd, cache_t* cache) {
    // 使用迭代器收集所有 namespace
    cache_iter_t* iter = cache_iter_create(cache);
    if (!iter) {
        send_error(fd, 500, "failed to create iterator");
        return;
    }
    
    // 简单实现：收集所有唯一的 namespace 前缀
    char** ns_list = NULL;
    size_t ns_count = 0;
    size_t ns_capacity = 16;
    
    ns_list = malloc(ns_capacity * sizeof(char*));
    if (!ns_list) {
        cache_iter_destroy(iter);
        send_error(fd, 500, "out of memory");
        return;
    }
    
    const char* key = NULL;
    const char* value = NULL;
    
    while (cache_iter_next(iter, &key, &value) == 1) {
        // 查找 namespace 分隔符 ':''
        const char* colon = strchr(key, ':');
        if (colon) {
            size_t ns_len = colon - key;
            int found = 0;
            for (size_t i = 0; i < ns_count; i++) {
                if (strlen(ns_list[i]) == ns_len && strncmp(ns_list[i], key, ns_len) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (ns_count >= ns_capacity) {
                    ns_capacity *= 2;
                    char** new_list = realloc(ns_list, ns_capacity * sizeof(char*));
                    if (!new_list) break;
                    ns_list = new_list;
                }
                ns_list[ns_count] = malloc(ns_len + 1);
                if (ns_list[ns_count]) {
                    strncpy(ns_list[ns_count], key, ns_len);
                    ns_list[ns_count][ns_len] = '\0';
                    ns_count++;
                }
            }
        }
    }
    
    cache_iter_destroy(iter);
    
    // 构建 JSON
    char* json = malloc(HTTP_SERVER_BUF_SIZE);
    if (!json) {
        for (size_t i = 0; i < ns_count; i++) free(ns_list[i]);
        free(ns_list);
        send_error(fd, 500, "out of memory");
        return;
    }
    
    size_t pos = 0;
    pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos,
                    "{\"count\":%zu,\"namespaces\":[", ns_count);
    
    for (size_t i = 0; i < ns_count; i++) {
        if (i > 0) pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos, ",");
        pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos, "\"%s\"", ns_list[i]);
        free(ns_list[i]);
    }
    free(ns_list);
    
    pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos, "]}");
    
    send_success(fd, json);
    free(json);
}

static void handle_namespace_get(int fd, cache_t* cache, const char* ns) {
    char decoded_ns[CACHE_MAX_KEY_LEN];
    url_decode(ns, decoded_ns, sizeof(decoded_ns));
    
    cache_iter_t* iter = cache_iter_create(cache);
    if (!iter) {
        send_error(fd, 500, "failed to create iterator");
        return;
    }
    
    // 构建 namespace 前缀
    size_t ns_len = strlen(decoded_ns);
    char ns_prefix[CACHE_MAX_KEY_LEN + 2];
    snprintf(ns_prefix, sizeof(ns_prefix), "%s:", decoded_ns);
    
    char* json = malloc(HTTP_SERVER_BUF_SIZE);
    if (!json) {
        cache_iter_destroy(iter);
        send_error(fd, 500, "out of memory");
        return;
    }
    
    size_t pos = 0;
    pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos, "{\"namespace\":\"%s\",\"keys\":[", decoded_ns);
    
    const char* key = NULL;
    const char* value = NULL;
    int count = 0;
    
    while (cache_iter_next(iter, &key, &value) == 1 && pos < HTTP_SERVER_BUF_SIZE - 512) {
        if (strncmp(key, ns_prefix, ns_len + 1) == 0) {
            char key_escaped[1024];
            char value_escaped[CACHE_MAX_VALUE_LEN];
            json_escape(key, key_escaped, sizeof(key_escaped));
            json_escape(value, value_escaped, sizeof(value_escaped));
            
            if (count > 0) pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos, ",");
            pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos,
                            "{\"key\":\"%s\",\"value\":\"%s\"}",
                            key_escaped, value_escaped);
            count++;
        }
    }
    
    cache_iter_destroy(iter);
    
    pos += snprintf(json + pos, HTTP_SERVER_BUF_SIZE - pos, "],\"count\":%d}", count);
    
    send_success(fd, json);
    free(json);
}

static void handle_sync(int fd, cache_t* cache) {
    int ret = cache_sync(cache);
    if (ret == CACHE_OK) {
        send_success(fd, NULL);
    } else {
        send_error(fd, 500, "sync failed");
    }
}

// ====== 主路由 ======

static void route_request(int fd, http_request_t* req, cache_t* cache) {
    const char* path = req->path;
    
    // Health check
    if (strcmp(path, "/health") == 0 || strcmp(path, "/api/v1/health") == 0) {
        if (req->method == HTTP_GET) {
            handle_health(fd, cache);
        } else {
            send_error(fd, 405, "method not allowed");
        }
        return;
    }
    
    // Stats
    if (strcmp(path, "/stats") == 0 || strcmp(path, "/api/v1/stats") == 0) {
        if (req->method == HTTP_GET) {
            handle_stats(fd, cache);
        } else {
            send_error(fd, 405, "method not allowed");
        }
        return;
    }
    
    // Search
    if (strcmp(path, "/search") == 0 || strcmp(path, "/api/v1/search") == 0) {
        if (req->method == HTTP_GET && req->query[0]) {
            handle_search(fd, cache, req->query);
        } else {
            send_error(fd, 400, "missing query parameters");
        }
        return;
    }
    
    // Namespaces
    if (strcmp(path, "/namespaces") == 0 || strcmp(path, "/api/v1/namespaces") == 0) {
        if (req->method == HTTP_GET) {
            handle_namespaces(fd, cache);
        } else {
            send_error(fd, 405, "method not allowed");
        }
        return;
    }
    
    // Namespace keys
    if (strncmp(path, "/namespace/", 11) == 0 || strncmp(path, "/api/v1/namespace/", 18) == 0) {
        const char* ns = (strncmp(path, "/api/v1/", 8) == 0) ? path + 18 : path + 11;
        if (req->method == HTTP_GET) {
            handle_namespace_get(fd, cache, ns);
        } else {
            send_error(fd, 405, "method not allowed");
        }
        return;
    }
    
    // Batch
    if (strcmp(path, "/batch") == 0 || strcmp(path, "/api/v1/batch") == 0) {
        if (req->method == HTTP_POST) {
            handle_batch(fd, cache, req->body);
        } else {
            send_error(fd, 405, "method not allowed");
        }
        return;
    }
    
    // Sync
    if (strcmp(path, "/sync") == 0 || strcmp(path, "/api/v1/sync") == 0) {
        if (req->method == HTTP_POST) {
            handle_sync(fd, cache);
        } else {
            send_error(fd, 405, "method not allowed");
        }
        return;
    }
    
    // Cache key operations: /cache/:key 或 /api/v1/cache/:key
    const char* cache_prefix = "/cache/";
    const char* cache_prefix_v1 = "/api/v1/cache/";
    const char* key = NULL;
    
    if (strncmp(path, cache_prefix_v1, strlen(cache_prefix_v1)) == 0) {
        key = path + strlen(cache_prefix_v1);
    } else if (strncmp(path, cache_prefix, strlen(cache_prefix)) == 0) {
        key = path + strlen(cache_prefix);
    }
    
    if (key && key[0]) {
        // 检查是否存在子路径如 /exists
        size_t key_len = strlen(key);
        if (key_len > 7 && strcmp(key + key_len - 7, "/exists") == 0) {
            char real_key[CACHE_MAX_KEY_LEN];
            strncpy(real_key, key, key_len - 7);
            real_key[key_len - 7] = '\0';
            if (req->method == HTTP_GET) {
                handle_cache_exists(fd, cache, real_key);
            } else {
                send_error(fd, 405, "method not allowed");
            }
            return;
        }
        
        switch (req->method) {
            case HTTP_GET:
                handle_cache_get(fd, cache, key);
                break;
            case HTTP_PUT:
                handle_cache_put(fd, cache, key, req->body);
                break;
            case HTTP_DELETE:
                handle_cache_delete(fd, cache, key);
                break;
            default:
                send_error(fd, 405, "method not allowed");
                break;
        }
        return;
    }
    
    // 404
    send_error(fd, 404, "not found");
}

// ====== 客户端连接处理 ======

static void handle_http_client(int fd, cache_t* cache) {
    char buf[HTTP_SERVER_BUF_SIZE];
    
    while (g_http_server_running) {
        // 读取请求
        ssize_t total = 0;
        ssize_t n;
        
        // 先读取头部（批量读取，避免逐字节系统调用）
        while (total < sizeof(buf) - 1) {
            n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
            if (n <= 0) return;
            total += n;
            buf[total] = '\0';  // 确保每次读取后 null-terminate
            
            // 检查是否读取到完整的 HTTP 头部
            if (total >= 4 && strstr(buf, "\r\n\r\n")) {
                break;
            }
        }
        
        if (total >= sizeof(buf) - 1) {
            send_error(fd, 413, "request too large");
            return;
        }
        
        buf[total] = '\0';  // 确保 null-terminate
        
        http_request_t req;
        if (parse_request(buf, total, &req) < 0) {
            send_error(fd, 400, "bad request");
            return;
        }
        
        // 如果还有 body 没读完，继续读取
        if (req.content_length > 0 && req.content_length <= HTTP_MAX_BODY) {
            size_t body_already_read = 0;
            if (req.body && req.body >= buf) {
                body_already_read = total - (size_t)(req.body - buf);
                if (body_already_read > total) body_already_read = 0;  // sanity check
            }
            
            if (body_already_read < req.content_length) {
                size_t remaining = req.content_length - body_already_read;
                
                if (total + remaining >= sizeof(buf)) {
                    send_error(fd, 413, "body too large");
                    return;
                }
                
                size_t received = 0;
                while (received < remaining) {
                    n = recv(fd, buf + total + received, remaining - received, 0);
                    if (n <= 0) return;
                    received += n;
                }
                total += received;
                buf[total] = '\0';
                
                // 重新解析，body 指针可能已失效
                if (parse_request(buf, total, &req) < 0) {
                    send_error(fd, 400, "bad request");
                    return;
                }
            }
        } else if (req.content_length > HTTP_MAX_BODY) {
            send_error(fd, 413, "body too large");
            return;
        }
        
        // 路由请求
        route_request(fd, &req, cache);
        
        // 检查是否是 keep-alive
        if (strcasestr(buf, "Connection: close")) {
            return;
        }
    }
}

// ====== 服务器主循环 ======

int cache_http_server_run(const cache_http_server_config_t* config) {
    if (!config) return -1;
    
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config->port);
    addr.sin_addr.s_addr = inet_addr(config->host);
    
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    
    if (listen(fd, HTTP_SERVER_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    
    printf("[CACHE-HTTP] Listening on http://%s:%d (db: %s)\n",
           config->host, config->port, config->db_dir);
    
    g_http_server_fd = fd;
    g_http_server_running = 1;
    
    cache_t* cache = cache_open(config->db_dir, config->max_memory);
    if (!cache) {
        fprintf(stderr, "[CACHE-HTTP] Failed to open cache at %s\n", config->db_dir);
        close(fd);
        g_http_server_fd = -1;
        return -1;
    }
    
    printf("[CACHE-HTTP] Cache opened, %zu entries\n", cache_count(cache));
    
    while (g_http_server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (g_http_server_running) perror("accept");
            break;
        }
        
        handle_http_client(client_fd, cache);
        close(client_fd);
    }
    
    printf("[CACHE-HTTP] Shutting down...\n");
    
    cache_close(cache);
    close(fd);
    g_http_server_fd = -1;
    g_http_server_running = 0;
    
    return 0;
}

void cache_http_server_stop(void) {
    g_http_server_running = 0;
    if (g_http_server_fd >= 0) {
        close(g_http_server_fd);
        g_http_server_fd = -1;
    }
}

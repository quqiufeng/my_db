#ifndef CACHE_HTTP_SERVER_H
#define CACHE_HTTP_SERVER_H

#include "cache.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ====== HTTP 服务器配置 ======
typedef struct {
    const char* host;           // 监听地址（默认 "0.0.0.0"）
    int port;                   // 监听端口（默认 8080）
    const char* db_dir;         // cache 数据目录
    size_t max_memory;          // 最大内存（默认 100MB）
    int max_clients;            // 最大客户端数（默认 100）
} cache_http_server_config_t;

static inline cache_http_server_config_t cache_http_server_config_default(void) {
    return (cache_http_server_config_t){
        .host = "0.0.0.0",
        .port = 8080,
        .db_dir = "./cache_http_data",
        .max_memory = 100 * 1024 * 1024,
        .max_clients = 100
    };
}

// ====== 服务器生命周期 ======
// 创建并启动 HTTP 服务器（阻塞，直到 cache_http_server_stop 被调用）
int cache_http_server_run(const cache_http_server_config_t* config);

// 请求停止服务器（从信号处理函数或其他线程调用）
void cache_http_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* CACHE_HTTP_SERVER_H */

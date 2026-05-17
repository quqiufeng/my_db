#ifndef CACHE_SERVER_H
#define CACHE_SERVER_H

#include "cache.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ====== 服务器配置 ======
typedef struct {
    const char* host;           // 监听地址（默认 "0.0.0.0"）
    int port;                   // 监听端口（默认 7777）
    const char* db_dir;         // cache 数据目录
    size_t max_memory;          // 最大内存（默认 100MB）
    int max_clients;            // 最大客户端数（默认 100）
} cache_server_config_t;

static inline cache_server_config_t cache_server_config_default(void) {
    return (cache_server_config_t){
        .host = "0.0.0.0",
        .port = 7777,
        .db_dir = "./cache_server_data",
        .max_memory = 100 * 1024 * 1024,
        .max_clients = 100
    };
}

// ====== 服务器生命周期 ======
// 创建并启动服务器（阻塞，直到 cache_server_stop 被调用）
int cache_server_run(const cache_server_config_t* config);

// 请求停止服务器（从信号处理函数或其他线程调用）
void cache_server_stop(void);

// ====== 客户端 API ======
typedef struct cache_client cache_client_t;

// 连接到远程 cache 服务器
cache_client_t* cache_client_connect(const char* host, int port);
void cache_client_disconnect(cache_client_t* client);

// 远程操作
int cache_client_ping(cache_client_t* client);
int cache_client_set(cache_client_t* client, const char* key, const char* value, uint64_t ttl_ms);
char* cache_client_get(cache_client_t* client, const char* key);  // 返回 malloc 的字符串，调用者 free
int cache_client_del(cache_client_t* client, const char* key);
int cache_client_exists(cache_client_t* client, const char* key);
int cache_client_count(cache_client_t* client);

#ifdef __cplusplus
}
#endif

#endif /* CACHE_SERVER_H */

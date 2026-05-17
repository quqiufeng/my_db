#include "cache_server.h"
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

#define CACHE_SERVER_BACKLOG 32
#define CACHE_SERVER_BUF_SIZE 65536

static volatile int g_server_running = 0;
static int g_server_fd = -1;

// ====== 协议辅助函数 ======

static void send_ok(int fd) {
    const char* msg = "+OK\r\n";
    send(fd, msg, strlen(msg), MSG_NOSIGNAL);
}

static void send_pong(int fd) {
    const char* msg = "+PONG\r\n";
    send(fd, msg, strlen(msg), MSG_NOSIGNAL);
}

static void send_err(int fd, const char* msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "-ERR %s\r\n", msg);
    send(fd, buf, strlen(buf), MSG_NOSIGNAL);
}

static void send_int(int fd, int64_t val) {
    char buf[64];
    snprintf(buf, sizeof(buf), ":%ld\r\n", (long)val);
    send(fd, buf, strlen(buf), MSG_NOSIGNAL);
}

static void send_bulk(int fd, const char* data, size_t len) {
    if (!data) {
        send(fd, "$-1\r\n", 5, MSG_NOSIGNAL);
        return;
    }
    char header[64];
    snprintf(header, sizeof(header), "$%zu\r\n", len);
    send(fd, header, strlen(header), MSG_NOSIGNAL);
    send(fd, data, len, MSG_NOSIGNAL);
    send(fd, "\r\n", 2, MSG_NOSIGNAL);
}

// ====== 命令解析与执行 ======

static void handle_client(int fd, cache_t* cache);

static int parse_command(char* buf, char** argv, int max_argv) {
    int argc = 0;
    char* p = buf;
    
    while (*p && argc < max_argv) {
        // 跳过空白
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (!*p) break;
        
        // 处理引号字符串
        if (*p == '"') {
            p++;
            argv[argc] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            argv[argc] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            if (*p) *p++ = '\0';
        }
        argc++;
    }
    
    return argc;
}

static void execute_command(int fd, cache_t* cache, int argc, char** argv) {
    if (argc == 0) {
        send_err(fd, "empty command");
        return;
    }
    
    const char* cmd = argv[0];
    
    if (strcasecmp(cmd, "PING") == 0) {
        send_pong(fd);
    }
    else if (strcasecmp(cmd, "SET") == 0) {
        if (argc < 3) {
            send_err(fd, "usage: SET key value [ttl_ms]");
            return;
        }
        const char* key = argv[1];
        const char* value = argv[2];
        uint64_t ttl = 0;
        if (argc >= 4) ttl = (uint64_t)atoll(argv[3]);
        
        int ret = cache_set(cache, key, value, ttl);
        if (ret == CACHE_OK) send_ok(fd);
        else send_err(fd, "set failed");
    }
    else if (strcasecmp(cmd, "GET") == 0) {
        if (argc < 2) {
            send_err(fd, "usage: GET key");
            return;
        }
        const char* value = cache_get(cache, argv[1]);
        if (value) send_bulk(fd, value, strlen(value));
        else send_bulk(fd, NULL, 0);
    }
    else if (strcasecmp(cmd, "DEL") == 0) {
        if (argc < 2) {
            send_err(fd, "usage: DEL key");
            return;
        }
        int ret = cache_del(cache, argv[1]);
        send_int(fd, ret == CACHE_OK ? 1 : 0);
    }
    else if (strcasecmp(cmd, "EXISTS") == 0) {
        if (argc < 2) {
            send_err(fd, "usage: EXISTS key");
            return;
        }
        send_int(fd, cache_exists(cache, argv[1]));
    }
    else if (strcasecmp(cmd, "COUNT") == 0) {
        send_int(fd, (int64_t)cache_count(cache));
    }
    else if (strcasecmp(cmd, "SEARCH") == 0) {
        if (argc < 2) {
            send_err(fd, "usage: SEARCH prefix|regex|fuzzy|tag query [max_results]");
            return;
        }
        const char* type = argv[1];
        const char* query = argc >= 3 ? argv[2] : "";
        int max_results = 100;
        if (argc >= 4) max_results = atoi(argv[3]);
        
        cache_search_options_t opts = cache_search_options_default();
        opts.max_results = max_results;
        
        cache_result_t* results = NULL;
        size_t count = 0;
        int ret = -1;
        
        if (strcasecmp(type, "prefix") == 0) {
            ret = cache_search_prefix(cache, query, &opts, &results, &count);
        } else if (strcasecmp(type, "regex") == 0) {
            ret = cache_search_regex(cache, query, &opts, &results, &count);
        } else if (strcasecmp(type, "fuzzy") == 0) {
            ret = cache_search_fuzzy(cache, query, &opts, &results, &count);
        } else if (strcasecmp(type, "tag") == 0) {
            ret = cache_search_tag(cache, query, &opts, &results, &count);
        } else {
            send_err(fd, "unknown search type");
            return;
        }
        
        if (ret != CACHE_OK) {
            send_err(fd, "search failed");
            return;
        }
        
        // 发送结果数量
        char header[64];
        snprintf(header, sizeof(header), "*OK %zu\r\n", count);
        send(fd, header, strlen(header), MSG_NOSIGNAL);
        
        // 发送每个结果: key score value
        for (size_t i = 0; i < count; i++) {
            char score_buf[32];
            snprintf(score_buf, sizeof(score_buf), "%.3f", results[i].score);
            send_bulk(fd, results[i].key, strlen(results[i].key));
            send_bulk(fd, score_buf, strlen(score_buf));
            send_bulk(fd, results[i].value, strlen(results[i].value));
        }
        
        cache_results_free(results);
    }
    else if (strcasecmp(cmd, "STATS") == 0) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "entries: %zu\ndeleted: %zu\nmemory_used: %zu\nmemory_max: %zu",
                 cache_count(cache),
                 0,  // deleted_count 是内部字段
                 cache_memory_used(cache),
                 cache_memory_max(cache));
        send_bulk(fd, buf, strlen(buf));
    }
    else if (strcasecmp(cmd, "SYNC") == 0) {
        int ret = cache_sync(cache);
        if (ret == CACHE_OK) send_ok(fd);
        else send_err(fd, "sync failed");
    }
    else if (strcasecmp(cmd, "QUIT") == 0) {
        send_ok(fd);
    }
    else {
        send_err(fd, "unknown command");
    }
}

static void handle_client(int fd, cache_t* cache) {
    char buf[CACHE_SERVER_BUF_SIZE];
    
    while (g_server_running) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        
        buf[n] = '\0';
        
        // 处理可能的多条命令
        char* p = buf;
        while (*p) {
            // 找行尾
            char* end = strstr(p, "\r\n");
            if (!end) break;
            
            // 命令长度限制：单行最大 4096 字节
            size_t line_len = end - p;
            if (line_len > 4096) {
                send_err(fd, "command too long");
                return;
            }
            
            *end = '\0';
            
            char* argv[16];
            int argc = parse_command(p, argv, 16);
            
            if (argc > 0) {
                execute_command(fd, cache, argc, argv);
                
                if (strcasecmp(argv[0], "QUIT") == 0) {
                    return;
                }
            }
            
            p = end + 2;
        }
    }
}

// ====== 服务器主循环 ======

int cache_server_run(const cache_server_config_t* config) {
    if (!config) return -1;
    
    // 创建 socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    
    // 允许地址复用
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }
    
    // 绑定
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
    
    // 监听
    if (listen(fd, CACHE_SERVER_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    
    printf("[CACHE-SERVER] Listening on %s:%d (db: %s)\n",
           config->host, config->port, config->db_dir);
    
    g_server_fd = fd;
    g_server_running = 1;
    
    // 打开 cache
    cache_t* cache = cache_open(config->db_dir, config->max_memory);
    if (!cache) {
        fprintf(stderr, "[CACHE-SERVER] Failed to open cache at %s\n", config->db_dir);
        close(fd);
        g_server_fd = -1;
        return -1;
    }
    
    printf("[CACHE-SERVER] Cache opened, %zu entries\n", cache_count(cache));
    
    // 主循环：accept 连接
    while (g_server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (g_server_running) perror("accept");
            break;
        }
        
        printf("[CACHE-SERVER] Client connected from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        handle_client(client_fd, cache);
        
        close(client_fd);
        printf("[CACHE-SERVER] Client disconnected\n");
    }
    
    printf("[CACHE-SERVER] Shutting down...\n");
    
    cache_close(cache);
    close(fd);
    g_server_fd = -1;
    g_server_running = 0;
    
    return 0;
}

void cache_server_stop(void) {
    g_server_running = 0;
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}

// ====== 客户端实现 ======

struct cache_client {
    int fd;
    char buf[CACHE_SERVER_BUF_SIZE];
};

cache_client_t* cache_client_connect(const char* host, int port) {
    if (!host || port <= 0) return NULL;
    
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }
    
    cache_client_t* client = malloc(sizeof(cache_client_t));
    if (!client) {
        close(fd);
        return NULL;
    }
    
    client->fd = fd;
    memset(client->buf, 0, sizeof(client->buf));
    return client;
}

void cache_client_disconnect(cache_client_t* client) {
    if (!client) return;
    if (client->fd >= 0) {
        send(client->fd, "QUIT\r\n", 6, MSG_NOSIGNAL);
        close(client->fd);
    }
    free(client);
}

// 发送命令并读取响应
static int send_command(cache_client_t* client, const char* cmd) {
    if (!client || client->fd < 0) return -1;
    
    size_t len = strlen(cmd);
    if (send(client->fd, cmd, len, MSG_NOSIGNAL) != (ssize_t)len) {
        return -1;
    }
    
    return 0;
}

static char* read_response(cache_client_t* client) {
    if (!client || client->fd < 0) return NULL;
    
    // 简单实现：读取一行
    size_t pos = 0;
    while (pos < sizeof(client->buf) - 1) {
        ssize_t n = recv(client->fd, client->buf + pos, 1, 0);
        if (n <= 0) return NULL;
        
        if (pos >= 1 && client->buf[pos-1] == '\r' && client->buf[pos] == '\n') {
            client->buf[pos-1] = '\0';
            return strdup(client->buf);
        }
        pos++;
    }
    
    return NULL;
}

static char* read_bulk_string(cache_client_t* client) {
    char* line = read_response(client);
    if (!line) return NULL;
    
    if (line[0] != '$') {
        free(line);
        return NULL;
    }
    
    size_t len = (size_t)atoi(line + 1);
    free(line);
    
    if (len == (size_t)-1) return NULL;  // null bulk string
    
    char* data = malloc(len + 1);
    if (!data) return NULL;
    
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(client->fd, data + received, len - received, 0);
        if (n <= 0) {
            free(data);
            return NULL;
        }
        received += n;
    }
    data[len] = '\0';
    
    // 消耗 \r\n
    char crlf[2];
    recv(client->fd, crlf, 2, 0);
    
    return data;
}

int cache_client_ping(cache_client_t* client) {
    if (send_command(client, "PING\r\n") < 0) return -1;
    
    char* resp = read_response(client);
    if (!resp) return -1;
    
    int ok = (strcmp(resp, "+PONG") == 0);
    free(resp);
    return ok ? 0 : -1;
}

int cache_client_set(cache_client_t* client, const char* key, const char* value, uint64_t ttl_ms) {
    char cmd[CACHE_SERVER_BUF_SIZE];
    if (ttl_ms > 0) {
        snprintf(cmd, sizeof(cmd), "SET \"%s\" \"%s\" %llu\r\n", key, value, (unsigned long long)ttl_ms);
    } else {
        snprintf(cmd, sizeof(cmd), "SET \"%s\" \"%s\"\r\n", key, value);
    }
    
    if (send_command(client, cmd) < 0) return -1;
    
    char* resp = read_response(client);
    if (!resp) return -1;
    
    int ok = (strcmp(resp, "+OK") == 0);
    free(resp);
    return ok ? 0 : -1;
}

char* cache_client_get(cache_client_t* client, const char* key) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "GET \"%s\"\r\n", key);
    
    if (send_command(client, cmd) < 0) return NULL;
    
    return read_bulk_string(client);
}

int cache_client_del(cache_client_t* client, const char* key) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "DEL \"%s\"\r\n", key);
    
    if (send_command(client, cmd) < 0) return -1;
    
    char* resp = read_response(client);
    if (!resp) return -1;
    
    int ok = (resp[0] == ':' && atoi(resp + 1) > 0);
    free(resp);
    return ok ? 0 : -1;
}

int cache_client_exists(cache_client_t* client, const char* key) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "EXISTS \"%s\"\r\n", key);
    
    if (send_command(client, cmd) < 0) return -1;
    
    char* resp = read_response(client);
    if (!resp) return -1;
    
    int exists = (resp[0] == ':' && atoi(resp + 1) > 0);
    free(resp);
    return exists;
}

int cache_client_count(cache_client_t* client) {
    if (send_command(client, "COUNT\r\n") < 0) return -1;
    
    char* resp = read_response(client);
    if (!resp) return -1;
    
    int count = (resp[0] == ':') ? atoi(resp + 1) : -1;
    free(resp);
    return count;
}

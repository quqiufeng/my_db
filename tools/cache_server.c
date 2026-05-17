#include "cache_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

static void signal_handler(int sig) {
    (void)sig;
    printf("\n[CACHE-SERVER] Received signal, shutting down...\n");
    cache_server_stop();
}

int main(int argc, char* argv[]) {
    cache_server_config_t config = cache_server_config_default();
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            config.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            config.host = argv[++i];
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            config.db_dir = argv[++i];
        } else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
            config.max_memory = (size_t)atoll(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --host HOST      Listen address (default: 0.0.0.0)\n");
            printf("  --port PORT      Listen port (default: 7777)\n");
            printf("  --db DIR         Database directory (default: ./cache_server_data)\n");
            printf("  --memory BYTES   Max memory (default: 104857600)\n");
            printf("  --help           Show this help\n");
            return 0;
        }
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("=== my_db Cache Server ===\n");
    
    int ret = cache_server_run(&config);
    
    printf("[CACHE-SERVER] Exited with code %d\n", ret);
    return ret;
}

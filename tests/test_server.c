#include "cache_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define TEST_HOST "127.0.0.1"
#define TEST_PORT 17778
#define TEST_DB "/tmp/test_cache_server"

int main() {
    printf("=== TCP Cache Server Test ===\n\n");
    
    // 清理测试目录
    system("rm -rf " TEST_DB);
    
    // Fork 服务器进程
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    
    if (pid == 0) {
        // 子进程：启动服务器
        cache_server_config_t config = cache_server_config_default();
        config.host = TEST_HOST;
        config.port = TEST_PORT;
        config.db_dir = TEST_DB;
        
        // 关闭 stdout/stderr
        int ret = cache_server_run(&config);
        exit(ret);
    }
    
    // 父进程：等待服务器启动
    usleep(500000);  // 500ms
    
    // 连接客户端
    printf("Connecting to %s:%d...\n", TEST_HOST, TEST_PORT);
    cache_client_t* client = cache_client_connect(TEST_HOST, TEST_PORT);
    if (!client) {
        printf("FAILED: Cannot connect to server\n");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }
    printf("Connected.\n\n");
    
    int passed = 0;
    int total = 0;
    
    // Test 1: PING
    total++;
    printf("Test 1: PING... ");
    if (cache_client_ping(client) == 0) {
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL\n");
    }
    
    // Test 2: SET
    total++;
    printf("Test 2: SET... ");
    if (cache_client_set(client, "hello", "world", 0) == 0) {
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL\n");
    }
    
    // Test 3: GET
    total++;
    printf("Test 3: GET... ");
    char* value = cache_client_get(client, "hello");
    if (value && strcmp(value, "world") == 0) {
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL\n");
    }
    free(value);
    
    // Test 4: EXISTS
    total++;
    printf("Test 4: EXISTS... ");
    if (cache_client_exists(client, "hello") == 1) {
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL\n");
    }
    
    // Test 5: COUNT
    total++;
    printf("Test 5: COUNT... ");
    int count = cache_client_count(client);
    if (count == 1) {
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL\n");
    }
    
    // Test 6: DEL
    total++;
    printf("Test 6: DEL... ");
    if (cache_client_del(client, "hello") == 0) {
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL\n");
    }
    
    // Test 7: GET after DEL
    total++;
    printf("Test 7: GET after DEL... ");
    value = cache_client_get(client, "hello");
    if (value == NULL) {
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL\n");
        free(value);
    }
    
    // Test 8: Multiple SET
    total++;
    printf("Test 8: Multiple SET... ");
    cache_client_set(client, "key1", "value1", 0);
    cache_client_set(client, "key2", "value2", 0);
    cache_client_set(client, "key3", "value3", 0);
    count = cache_client_count(client);
    if (count == 3) {
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL\n");
    }
    
    // 断开连接
    cache_client_disconnect(client);
    
    // 停止服务器
    printf("\nStopping server...\n");
    kill(pid, SIGTERM);
    usleep(200000);
    kill(pid, SIGKILL);  // 强制终止
    waitpid(pid, NULL, 0);
    
    // 清理
    system("rm -rf " TEST_DB);
    
    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);
    
    return (passed == total) ? 0 : 1;
}

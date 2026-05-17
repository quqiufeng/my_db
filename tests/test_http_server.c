#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <assert.h>

#define TEST_PORT 18080
#define TEST_DB_DIR "./test_http_cache"
#define TEST_HOST "127.0.0.1"

// 简单 HTTP 客户端
static int http_request(const char* method, const char* path, const char* body,
                        char* response, size_t response_size) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    addr.sin_addr.s_addr = inet_addr(TEST_HOST);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    char request[4096];
    if (body && body[0]) {
        snprintf(request, sizeof(request),
                 "%s %s HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "%s",
                 method, path, TEST_HOST, TEST_PORT, strlen(body), body);
    } else {
        snprintf(request, sizeof(request),
                 "%s %s HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 method, path, TEST_HOST, TEST_PORT);
    }
    
    if (send(fd, request, strlen(request), 0) < 0) {
        close(fd);
        return -1;
    }
    
    // 读取响应
    size_t total = 0;
    ssize_t n;
    while ((n = recv(fd, response + total, response_size - total - 1, 0)) > 0) {
        total += n;
    }
    response[total] = '\0';
    
    close(fd);
    return (int)total;
}

// 从 HTTP 响应中提取 body
static const char* get_body(const char* response) {
    const char* body = strstr(response, "\r\n\r\n");
    return body ? body + 4 : response;
}

// 启动 HTTP 服务器进程
static pid_t start_server(void) {
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程：启动服务器，关闭 stdout/stderr 避免输出混合
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        setsid();  // 创建新会话，避免被父进程信号影响
        execl("./tools/cache_http_server", "cache_http_server",
              "--host", TEST_HOST,
              "--port", "18080",
              "--db", TEST_DB_DIR,
              NULL);
        _exit(1);
    }
    // 等待服务器启动
    usleep(500000);  // 500ms
    return pid;
}

static void stop_server(pid_t pid) {
    kill(pid, SIGTERM);
    usleep(100000);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

int main(void) {
    printf("=== HTTP Server Tests ===\n");
    
    // 清理旧数据
    system("rm -rf " TEST_DB_DIR);
    
    pid_t server_pid = start_server();
    if (server_pid < 0) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }
    
    char resp[4096];
    int ret;
    int passed = 0, failed = 0;
    
    // Test 1: Health check
    printf("Test 1: GET /health ... ");
    ret = http_request("GET", "/health", NULL, resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "200") && strstr(get_body(resp), "\"status\":\"ok\"")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 2: GET /stats (empty cache)
    printf("Test 2: GET /stats (empty) ... ");
    ret = http_request("GET", "/stats", NULL, resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "200") && strstr(get_body(resp), "\"entries\":0")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 3: PUT /cache/test_key
    printf("Test 3: PUT /cache/test_key ... ");
    ret = http_request("PUT", "/cache/test_key",
                       "{\"value\":\"hello world\",\"ttl_ms\":60000}",
                       resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "200") && strstr(get_body(resp), "\"status\":\"ok\"")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 4: GET /cache/test_key
    printf("Test 4: GET /cache/test_key ... ");
    ret = http_request("GET", "/cache/test_key", NULL, resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "200") && strstr(get_body(resp), "\"value\":\"hello world\"")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 5: GET /cache/nonexistent
    printf("Test 5: GET /cache/nonexistent ... ");
    ret = http_request("GET", "/cache/nonexistent", NULL, resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "404")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 6: GET /cache/test_key/exists
    printf("Test 6: GET /cache/test_key/exists ... ");
    ret = http_request("GET", "/cache/test_key/exists", NULL, resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "200") && strstr(get_body(resp), "\"exists\":true")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 7: DELETE /cache/test_key
    printf("Test 7: DELETE /cache/test_key ... ");
    ret = http_request("DELETE", "/cache/test_key", NULL, resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "200")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 8: Verify deletion
    printf("Test 8: Verify deletion ... ");
    ret = http_request("GET", "/cache/test_key", NULL, resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "404")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 9: POST /batch
    printf("Test 9: POST /batch ... ");
    ret = http_request("POST", "/batch",
                       "[{\"key\":\"batch1\",\"value\":\"v1\"},{\"key\":\"batch2\",\"value\":\"v2\"}]",
                       resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "200") && strstr(get_body(resp), "\"count\":2")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 10: GET /search?pattern=batch&type=prefix
    printf("Test 10: GET /search ... ");
    ret = http_request("GET", "/search?pattern=batch&type=prefix", NULL, resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "200") && strstr(get_body(resp), "\"count\":2")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 11: POST /sync
    printf("Test 11: POST /sync ... ");
    ret = http_request("POST", "/sync", NULL, resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "200")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 12: Namespace operations
    printf("Test 12: Namespace operations ... ");
    http_request("PUT", "/cache/ns:test:key1", "{\"value\":\"ns1\"}", resp, sizeof(resp));
    http_request("PUT", "/cache/ns:test:key2", "{\"value\":\"ns2\"}", resp, sizeof(resp));
    
    ret = http_request("GET", "/namespaces", NULL, resp, sizeof(resp));
    int ns_ok = (ret > 0 && strstr(resp, "200") && strstr(get_body(resp), "\"ns\""));
    
    ret = http_request("GET", "/namespace/ns:test", NULL, resp, sizeof(resp));
    int ns_keys_ok = (ret > 0 && strstr(resp, "200") && strstr(get_body(resp), "\"count\":2"));
    
    if (ns_ok && ns_keys_ok) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED (ns=%d, keys=%d)\n", ns_ok, ns_keys_ok);
        failed++;
    }
    
    // Test 13: Method not allowed
    printf("Test 13: Method not allowed ... ");
    ret = http_request("POST", "/health", NULL, resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "405")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    // Test 14: 404 not found
    printf("Test 14: 404 not found ... ");
    ret = http_request("GET", "/nonexistent", NULL, resp, sizeof(resp));
    if (ret > 0 && strstr(resp, "404")) {
        printf("PASSED\n");
        passed++;
    } else {
        printf("FAILED\n");
        failed++;
    }
    
    stop_server(server_pid);
    
    // 清理
    system("rm -rf " TEST_DB_DIR);
    
    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}

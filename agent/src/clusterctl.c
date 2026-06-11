/* ===================================================================
 * clusterctl - Master-side AI cluster control tool
 *
 * 用法:
 *   clusterctl --connect <ip:port> <command> [args]
 *
 * 命令:
 *   query                   查询集群状态（JSON 输出）
 *   exec <cmd>              在所有节点执行 shell 命令
 *   status                  查询节点身份+实时状态
 *
 * 选项:
 *   --connect <addr>        要连接的 Agent 地址（默认 127.0.0.1:9527）
 *   --timeout <sec>         超时秒数（默认 10）
 *   --help                  显示帮助
 *
 * 输出: JSON
 *   查询类: {"status":"ok","type":"...", ...}
 *   执行类: {"status":"ok","task_id":"...","results":[...]}
 * 退出码:
 *   0 = 成功
 *   1 = 参数错误
 *   2 = 连接失败
 *   3 = 执行失败 / 部分失败
 * =================================================================== */
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <poll.h>

/* ===================================================================
 * 配置
 * =================================================================== */
typedef struct {
    char     connect_addr[256];   /* 目标 Agent 地址 */
    int      timeout_sec;         /* 超时秒数 */
    char     command[256];        /* 子命令: query, exec, status */
    char     exec_cmd[4096];      /* exec 的参数 */
} ctl_config_t;

/* ===================================================================
 * 打印帮助
 * =================================================================== */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --connect <ip:port> <command> [args]\n"
        "\n"
        "Commands:\n"
        "  query                Query cluster status (JSON output)\n"
        "  exec <cmd>           Execute shell command on all nodes\n"
        "  status               Query node identity + real-time status\n"
        "\n"
        "Options:\n"
        "  --connect <addr>     Target agent address (default: 127.0.0.1:9527)\n"
        "  --timeout <sec>      Timeout in seconds (default: 10)\n"
        "  --help               Show this help\n"
        "\n"
        "Exit codes:\n"
        "  0 = success\n"
        "  1 = parameter error\n"
        "  2 = connection failed\n"
        "  3 = execution failed / partial failure\n"
        "\n"
        "Examples:\n"
        "  %s --connect 192.168.1.10:9527 query\n"
        "  %s --connect 192.168.1.10:9527 exec \"uptime\"\n"
        "  %s --connect 192.168.1.10:9527 status\n",
        prog, prog, prog, prog);
}

/* ===================================================================
 * 当前时间戳（毫秒）
 * =================================================================== */
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ===================================================================
 * JSON 字符串转义（简易版）
 * =================================================================== */
static void json_escape(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    for (const char *p = in; *p && j + 6 < out_size; p++) {
        switch (*p) {
        case '"':  out[j++] = '\\'; out[j++] = '"';  break;
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
        case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
        case '\t': out[j++] = '\\'; out[j++] = 't';  break;
        default:   out[j++] = *p; break;
        }
    }
    out[j] = '\0';
}

/* ===================================================================
 * tcp_connect - 连接到指定地址
 * 返回 fd，-1 失败
 * =================================================================== */
static int tcp_connect(const char *addr) {
    char ip[64] = {0};
    int  port = 9527;

    /* 解析 ip:port */
    const char *colon = strchr(addr, ':');
    if (colon) {
        size_t len = colon - addr;
        if (len >= sizeof(ip)) len = sizeof(ip) - 1;
        memcpy(ip, addr, len);
        ip[len] = '\0';
        port = atoi(colon + 1);
    } else {
        snprintf(ip, sizeof(ip), "%s", addr);
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) <= 0) {
        /* 尝试解析主机名 */
        struct hostent *h = gethostbyname(ip);
        if (!h) {
            fprintf(stderr, "{\"status\":\"error\",\"error\":\"resolve failed: %s\"}\n", ip);
            return -1;
        }
        memcpy(&sa.sin_addr, h->h_addr_list[0], h->h_length);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* ===================================================================
 * recv_message_timeout - 带超时的 recv_message
 * =================================================================== */
static int recv_message_timeout(int fd, uint8_t *type, uint8_t *flags,
                                char *payload, uint32_t *payload_len,
                                int timeout_ms) {
    struct pollfd pfd;
    pfd.fd     = fd;
    pfd.events = POLLIN;

    long deadline = now_ms() + timeout_ms;
    while (1) {
        int remaining = (int)(deadline - now_ms());
        if (remaining <= 0) return -1;

        int ret = poll(&pfd, 1, remaining);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) return -1;  /* 超时 */

        return recv_message(fd, type, flags, payload, payload_len);
    }
}

/* ===================================================================
 * do_query - 发送 MSG_MASTER_QUERY，接收 MSG_MASTER_STATUS
 * =================================================================== */
static int do_query(int fd, int timeout_sec) {
    /* 发送查询 */
    if (send_message(fd, MSG_MASTER_QUERY, PROTO_FLAG_REQUEST, "{}", 2) < 0) {
        printf("{\"status\":\"error\",\"error\":\"send failed\"}\n");
        return 3;
    }

    /* 接收响应 */
    uint8_t  rtype, rflags;
    uint32_t rlen;
    char     payload[PROTO_MAX_PAYLOAD];

    int ret = recv_message_timeout(fd, &rtype, &rflags, payload, &rlen,
                                    timeout_sec * 1000);
    if (ret < 0) {
        printf("{\"status\":\"error\",\"error\":\"timeout or disconnect\"}\n");
        return 3;
    }

    /* 输出原始 JSON */
    payload[rlen] = '\0';
    printf("%s\n", payload);
    return 0;
}

/* ===================================================================
 * do_exec - 发送 MSG_MASTER_CMD (exec)，接收 MSG_MASTER_RESULT
 * =================================================================== */
static int do_exec(int fd, const char *cmd, int timeout_sec) {
    char escaped_cmd[8192];
    json_escape(cmd, escaped_cmd, sizeof(escaped_cmd));

    char json[16384];
    snprintf(json, sizeof(json),
             "{\"type\":\"master_cmd\",\"action\":\"exec\","
             "\"cmd\":\"%s\",\"target\":\"all\",\"timeout\":%d}",
             escaped_cmd, timeout_sec);

    if (send_message(fd, MSG_MASTER_CMD, PROTO_FLAG_REQUEST, json, strlen(json)) < 0) {
        printf("{\"status\":\"error\",\"error\":\"send failed\"}\n");
        return 3;
    }

    /* 接收响应 */
    uint8_t  rtype, rflags;
    uint32_t rlen;
    char     payload[PROTO_MAX_PAYLOAD];

    int ret = recv_message_timeout(fd, &rtype, &rflags, payload, &rlen,
                                    (timeout_sec + 5) * 1000);
    if (ret < 0) {
        printf("{\"status\":\"error\",\"error\":\"timeout or disconnect\"}\n");
        return 3;
    }

    payload[rlen] = '\0';
    printf("%s\n", payload);
    return 0;
}

/* ===================================================================
 * do_status - 先 hello，再 query
 * =================================================================== */
static int do_status(int fd, int timeout_sec) {
    /* 发送 MSG_MASTER_HELLO 声明身份 */
    char hello[128];
    snprintf(hello, sizeof(hello), "{\"client\":\"clusterctl\",\"version\":1}");
    send_message(fd, MSG_MASTER_HELLO, PROTO_FLAG_REQUEST, hello, strlen(hello));

    /* 等待短暂时间接收可能的消息 */
    uint8_t  rtype, rflags;
    uint32_t rlen;
    char     payload[PROTO_MAX_PAYLOAD];

    int ret = recv_message_timeout(fd, &rtype, &rflags, payload, &rlen, 2000);

    /* 如果收到了 MSG_MASTER_LEADER，打印 Leader 信息 */
    if (ret == 0 && rtype == MSG_MASTER_LEADER) {
        payload[rlen] = '\0';
        printf("{\"status\":\"ok\",\"type\":\"master_leader\",\"detail\":%s}\n", payload);
        return 0;
    }

    /* 否则执行 query */
    return do_query(fd, timeout_sec);
}

/* ===================================================================
 * main
 * =================================================================== */
int main(int argc, char **argv) {
    ctl_config_t config;
    memset(&config, 0, sizeof(config));
    snprintf(config.connect_addr, sizeof(config.connect_addr), "127.0.0.1:9527");
    config.timeout_sec = 10;

    /* ========= 解析参数 ========= */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            snprintf(config.connect_addr, sizeof(config.connect_addr), "%s", argv[++i]);
            i++;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            config.timeout_sec = atoi(argv[++i]);
            i++;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "{\"status\":\"error\",\"error\":\"Unknown option: %s\"}\n", argv[i]);
            return 1;
        } else {
            break;  /* 子命令开始 */
        }
    }

    if (i >= argc) {
        fprintf(stderr, "{\"status\":\"error\",\"error\":\"missing command\"}\n");
        print_usage(argv[0]);
        return 1;
    }

    snprintf(config.command, sizeof(config.command), "%s", argv[i]);
    if (strcmp(config.command, "exec") == 0) {
        if (i + 1 >= argc) {
            fprintf(stderr, "{\"status\":\"error\",\"error\":\"exec requires a command argument\"}\n");
            return 1;
        }
        snprintf(config.exec_cmd, sizeof(config.exec_cmd), "%s", argv[i + 1]);
        /* 支持多单词命令，拼接剩余参数 */
        for (int j = i + 2; j < argc; j++) {
            size_t cur = strlen(config.exec_cmd);
            snprintf(config.exec_cmd + cur, sizeof(config.exec_cmd) - cur, " %s", argv[j]);
        }
    }

    /* ========= 连接 ========= */
    int fd = tcp_connect(config.connect_addr);
    if (fd < 0) {
        printf("{\"status\":\"error\",\"error\":\"connect to %s failed: %s\"}\n",
               config.connect_addr, strerror(errno));
        return 2;
    }

    /* ========= 执行命令 ========= */
    int ret = 0;
    if (strcmp(config.command, "query") == 0) {
        ret = do_query(fd, config.timeout_sec);
    } else if (strcmp(config.command, "status") == 0) {
        ret = do_status(fd, config.timeout_sec);
    } else if (strcmp(config.command, "exec") == 0) {
        ret = do_exec(fd, config.exec_cmd, config.timeout_sec);
    } else {
        printf("{\"status\":\"error\",\"error\":\"unknown command: %s\"}\n", config.command);
        close(fd);
        return 1;
    }

    close(fd);
    return ret;
}

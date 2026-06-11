#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <stdarg.h>

/* ===================================================================
 * 全局单例
 * =================================================================== */
agent_state_t g_agent;

/* ===================================================================
 * 工具函数
 * =================================================================== */
long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void agent_log(const char *level, const char *fmt, ...) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char buf[64];
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&ts.tv_sec));

    fprintf(stderr, "[%s.%03ld] [%s] ", buf, ts.tv_nsec / 1000000, level);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void agent_init(agent_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->listen_fd  = -1;
    state->epoll_fd   = -1;
    state->running    = 1;

    /* 默认配置 */
    strncpy(state->config.bind_addr, "0.0.0.0", sizeof(state->config.bind_addr) - 1);
    state->config.port     = 9527;
    state->config.daemonize = 1;
    strncpy(state->config.data_dir, "/tmp/agent", sizeof(state->config.data_dir) - 1);

    /* 上下文初始化 */
    state->ctx.epoch        = 0;
    state->ctx.cluster_size = 1;
    state->ctx.uptime_sec   = 0;
    state->start_time       = now_ms();

    /* 选举初始化 */
    election_init(&state->election, "", 0, 0);

    /* 随机数种子 */
    srand((unsigned int)(now_ms() ^ getpid()));
}

int daemonize_process(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);  /* 父进程退出 */

    /* 子进程继续 */
    if (setsid() < 0) return -1;

    /* 第二次 fork，完全脱离控制终端 */
    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    /* 关闭标准 I/O */
    close(0);
    close(1);
    close(2);

    /* 重定向到 /dev/null */
    open("/dev/null", O_RDONLY);  /* stdin */
    open("/dev/null", O_WRONLY);  /* stdout */
    open("/dev/null", O_WRONLY);  /* stderr */

    return 0;
}

int parse_ip_list(agent_state_t *state, const char *ip_list, int default_port) {
    return peer_create_list(state, ip_list, default_port);
}

/* ===================================================================
 * 打印帮助信息
 * =================================================================== */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -ip <list>       Comma-separated peer IP[:port] list\n"
        "  --port <port>    Listen port (default: 9527)\n"
        "  --id <name>      Node ID (default: auto)\n"
        "  --bind <addr>    Bind address (default: 0.0.0.0)\n"
        "  --datadir <dir>  Data directory (default: /tmp/agent)\n"
        "  --idx <n>        Node index in peer list (0-based)\n"
        "  --cluster-size <n>  Total nodes in cluster (default: peer_count+1)\n"
        "  --foreground     Run in foreground (don't daemonize)\n"
        "  -h               Show this help\n",
        prog);
}

/* ===================================================================
 * 解析命令行参数
 * =================================================================== */
static int parse_args(agent_state_t *state, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "-ip") == 0 && i + 1 < argc) {
            strncpy(state->config.ip_list, argv[++i], sizeof(state->config.ip_list) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            state->config.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            strncpy(state->config.node_id, argv[++i], sizeof(state->config.node_id) - 1);
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            strncpy(state->config.bind_addr, argv[++i], sizeof(state->config.bind_addr) - 1);
        } else if (strcmp(argv[i], "--datadir") == 0 && i + 1 < argc) {
            strncpy(state->config.data_dir, argv[++i], sizeof(state->config.data_dir) - 1);
        } else if (strcmp(argv[i], "--cluster-size") == 0 && i + 1 < argc) {
            state->config.cluster_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--idx") == 0 && i + 1 < argc) {
            state->config.node_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--foreground") == 0) {

            state->config.daemonize = 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }

    /* 如果没指定 --id，自动生成 */
    if (state->config.node_id[0] == '\0') {
        char hostname[64] = "unknown";
        gethostname(hostname, sizeof(hostname));
        snprintf(state->config.node_id, sizeof(state->config.node_id),
                 "%s:%d", hostname, state->config.port);
    }

    return 0;
}

/* ===================================================================
 * update_session_ctx - 将配置和运行时信息写入会话上下文
 * =================================================================== */
static void update_session_ctx(agent_state_t *state) {
    snprintf(state->ctx.node_id, sizeof(state->ctx.node_id), "%s", state->config.node_id);
    snprintf(state->ctx.self_addr, sizeof(state->ctx.self_addr),
             "%s:%d", state->config.bind_addr, state->config.port);

    /* 如果没有 Leader，role 默认为 follower（等待选举） */
    if (state->ctx.role[0] == '\0')
        strncpy(state->ctx.role, "follower", sizeof(state->ctx.role) - 1);

    state->ctx.uptime_sec = (now_ms() - state->start_time) / 1000;
    state->ctx.client_count = state->master_count;
}

/* ===================================================================
 * main - 入口
 * =================================================================== */
int main(int argc, char **argv) {
    /* 初始化 */
    agent_init(&g_agent);

    /* 解析参数 */
    if (parse_args(&g_agent, argc, argv) < 0)
        return 1;

    /* 更新会话上下文 */
    update_session_ctx(&g_agent);
    election_init(&g_agent.election, g_agent.config.node_id, g_agent.config.node_index, 0);
    election_init_timer(&g_agent.election, now_ms());

    g_agent.election.my_index = g_agent.config.node_index;
    /* 后台运行 */
    if (g_agent.config.daemonize) {
        if (daemonize_process() < 0) {
            fprintf(stderr, "daemonize failed\n");
            return 1;
        }
    }

    /* 解析 IP 列表 */
    if (g_agent.config.ip_list[0] != '\0') {
        int n = parse_ip_list(&g_agent, g_agent.config.ip_list, g_agent.config.port);
        if (n < 0) {
            agent_log("ERROR", "failed to parse IP list");
            return 1;
        }
        agent_log("INFO", "configured %d peers", n);
    }

    /* 设置集群总大小，供 election_tick 中的多数票判断使用 */
    /* 必须在 parse_ip_list 之后，因为 peer_create_list 会设置 cluster_size = peer_count + 1 */
    if (g_agent.config.cluster_size > 0)
        g_agent.ctx.cluster_size = g_agent.config.cluster_size;
    else
        g_agent.ctx.cluster_size = g_agent.peer_count + 1;

    /* 初始化服务器 */
    if (server_init(&g_agent) < 0)
        return 1;

    /* 初始化 epoll */
    if (epoll_init(&g_agent) < 0)
        return 1;

    /* 连接对端 */
    if (g_agent.peer_count > 0) {
        peer_connect_all(&g_agent);
    }

    /* 进入主事件循环 */
    agent_log("INFO", "agent %s started on port %d",
              g_agent.config.node_id, g_agent.config.port);
    server_event_loop(&g_agent);

    /* 清理 */
    agent_log("INFO", "agent shutting down");
    for (int i = 0; i < g_agent.peer_count; i++) {
        if (g_agent.peers[i].fd >= 0)
            close(g_agent.peers[i].fd);
    }
    close(g_agent.listen_fd);
    close(g_agent.epoll_fd);
    free(g_agent.peers);

    return 0;
}

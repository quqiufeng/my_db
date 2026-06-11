#ifndef AGENT_H
#define AGENT_H

#include "protocol.h"
#include "peer.h"
#include "election.h"
#include "task.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * 全局配置
 * =================================================================== */
typedef struct {
    char     ip_list[4096];        /* -ip 参数：逗号分隔的 IP[:port] 列表 */
    int      port;                 /* --port  监听端口（默认 9527） */
    char     node_id[64];          /* --id   节点 ID */
    char     data_dir[256];        /* --datadir 数据目录 */
    int              node_index;        /* --idx 在节点列表中的位置 */
    int              cluster_size;      /* --cluster-size 集群节点总数（含自己） */
    char     bind_addr[64];        /* 绑定地址（默认 0.0.0.0） */
    int      daemonize;            /* 是否后台运行 */
} agent_config_t;

/* ===================================================================
 * Agent 全局状态
 * =================================================================== */
typedef struct agent_state {
    agent_config_t  config;

    /* 监听 socket */
    int             listen_fd;
    int             epoll_fd;

    /* 本机会话上下文 */
    session_ctx_t   ctx;

    /* 对端节点列表 */
    peer_t         *peers;
    int             peer_count;
    int             peer_capacity;

    /* 选举状态 */
    election_t      election;

    /* 运行标志 */
    int             running;
    long            start_time;     /* 启动时间戳（monotonic） */

    /* Master 连接列表 */
    int             master_fds[16];
    int             master_count;
} agent_state_t;

/* 全局单例，在 main.c 中定义 */
extern agent_state_t g_agent;

/* ===================================================================
 * 工具函数
 * =================================================================== */

/**
 * agent_init - 初始化全局状态
 */
void agent_init(agent_state_t *state);

/**
 * agent_log - 日志输出
 * @level: "DEBUG"/"INFO"/"WARN"/"ERROR"
 */
void agent_log(const char *level, const char *fmt, ...);

/**
 * now_ms - 获取当前单调时钟（毫秒），用于超时判断
 */
long now_ms(void);

/**
 * parse_ip_list - 解析 -ip 参数，填充 peers 列表
 * @state: agent 状态
 * @ip_list: 逗号分隔的 IP[:port] 字符串
 * @default_port: 默认端口
 * 返回 peer 数量
 */
int parse_ip_list(agent_state_t *state, const char *ip_list, int default_port);

/**
 * daemonize - 后台运行（fork + setsid）
 * 返回 0 成功（父进程退出，子进程返回）
 */
int daemonize_process(void);

/**
 * server_init - 创建 TCP 监听 socket
 * 返回 listen_fd，-1 失败
 */
int server_init(agent_state_t *state);

/**
 * epoll_init - 初始化 epoll 并将 listen_fd 加入监听
 */
int epoll_init(agent_state_t *state);

/**
 * server_event_loop - 主事件循环
 */
int server_event_loop(agent_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_H */

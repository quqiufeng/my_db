#ifndef PEER_H
#define PEER_H

#include "protocol.h"
#include <sys/epoll.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * 对端节点状态
 * =================================================================== */
typedef enum {
    PEER_DISCONNECTED = 0,   /* 未连接 */
    PEER_CONNECTING,         /* 连接中 */
    PEER_CONNECTED,          /* 已连接 */
    PEER_OFFLINE,            /* 曾经连接过，现超时离线 */
} peer_state_t;

/* ===================================================================
 * 对端节点
 * =================================================================== */
typedef struct peer {
    int             fd;             /* TCP socket，-1 表示未连接 */
    peer_state_t    state;          /* 连接状态 */
    char            addr[128];       /* "192.168.1.10:9527" */
    char            ip[64];         /* IP 地址 */
    int             port;           /* 端口 */

    /* 对方的会话上下文（从 MSG_MASTER_STATUS 或心跳中获得） */
    session_ctx_t   remote_ctx;

    /* 心跳计时 */
    long            last_heartbeat; /* 最后一次收到心跳的时间戳（ms） */
    int             heartbeat_miss; /* 连续丢失心跳次数 */

    /* 重连 */
    long            next_reconnect; /* 下次重连时间戳（ms） */

    /* epoll event */
    struct epoll_event event;
} peer_t;

/* ===================================================================
 * 对端管理函数
 * =================================================================== */

/**
 * peer_create_list - 从 IP 列表创建对端节点数组
 * @state:  agent 状态
 * @ip_list: 逗号分隔的 IP[:port] 列表
 * @default_port: 默认端口
 * 返回节点数量
 */
int peer_create_list(void *state, const char *ip_list, int default_port);

/**
 * peer_connect_all - 尝试连接所有处于 DISCONNECTED 的节点
 * 返回成功连接的个数
 */
int peer_connect_all(void *state);

/**
 * peer_find_by_fd - 通过 socket fd 查找对端
 */
peer_t *peer_find_by_fd(void *state, int fd);

/**
 * peer_find_by_addr - 通过地址查找对端
 */
peer_t *peer_find_by_addr(void *state, const char *addr);

/**
 * peer_mark_offline - 标记节点为离线
 */
void peer_mark_offline(void *state, peer_t *peer);

/**
 * peer_check_timeout - 遍历检查所有节点的心跳超时
 * @state:  agent 状态
 * @now:    当前时间（ms）
 * @timeout_ms: 超时阈值（ms）
 * 返回超时的节点数
 */
int peer_check_timeout(void *state, long now, int timeout_ms);

/**
 * peer_reconnect - 重连所有处于 OFFLINE 且到达重连时间的节点
 */
void peer_reconnect(void *state, long now);

#ifdef __cplusplus
}
#endif

#endif /* PEER_H */

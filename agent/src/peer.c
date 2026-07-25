#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ===================================================================
 * peer_create_list - 从 IP 列表创建对端节点数组
 * =================================================================== */
int peer_create_list(void *vstate, const char *ip_list, int default_port) {
    agent_state_t *state = (agent_state_t *)vstate;
    char buf[4096];
    char *saveptr, *token;

    strncpy(buf, ip_list, sizeof(buf) - 1);

    /* 统计节点数 */
    int count = 0;
    for (const char *p = ip_list; *p; p++)
        if (*p == ',') count++;
    count++;  /* 逗号数 + 1 */

    state->peer_count    = 0;
    state->peer_capacity = count;
    state->peers         = calloc(count, sizeof(peer_t));
    if (!state->peers) {
        agent_log("ERROR", "calloc(%d peers) failed", count);
        return -1;
    }

    /* 更新集群规模（仅在未通过 --cluster-size 显式设置时）*/
    if (state->ctx.cluster_size <= 1)
        state->ctx.cluster_size = count + 1;

    token = strtok_r(buf, ",", &saveptr);
    while (token && state->peer_count < count) {
        /* 去除前后空格 */
        while (*token == ' ') token++;

        peer_t *peer = &state->peers[state->peer_count];
        memset(peer, 0, sizeof(*peer));

        /* 检查是否有端口号 IP:PORT */
        char *colon = strchr(token, ':');
        if (colon) {
            *colon = '\0';
            strncpy(peer->ip, token, sizeof(peer->ip) - 1);
            peer->port = atoi(colon + 1);
        } else {
            strncpy(peer->ip, token, sizeof(peer->ip) - 1);
            peer->port = default_port;
        }

        snprintf(peer->addr, sizeof(peer->addr), "%s:%d", peer->ip, peer->port);
        peer->fd      = -1;
        peer->state   = PEER_DISCONNECTED;
        peer->heartbeat_miss = 0;
        peer->last_heartbeat = 0;
        peer->next_reconnect = 0;

        agent_log("DEBUG", "peer[%d]: %s", state->peer_count, peer->addr);
        state->peer_count++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    return state->peer_count;
}

/* ===================================================================
 * peer_connect - 连接单个对端
 * =================================================================== */
static int peer_connect_one(agent_state_t *state, peer_t *peer) {
    struct sockaddr_in addr;
    int fd, ret;

    if (peer->fd >= 0) {
        close(peer->fd);
        peer->fd = -1;
    }

    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(peer->port);
    if (inet_pton(AF_INET, peer->ip, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        agent_log("WARN", "connect to %s failed: %s", peer->addr, strerror(errno));
        close(fd);
        return -1;
    }

    peer->fd    = fd;
    peer->state = PEER_CONNECTING;

    /* 加入 epoll，监听可写事件（connect 完成） */
    struct epoll_event ev;
    ev.events  = EPOLLOUT | EPOLLET;
    ev.data.fd = fd;
    if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        agent_log("WARN", "epoll_ctl ADD for peer %s failed: %s",
                  peer->addr, strerror(errno));
        close(fd);
        peer->fd = -1;
        peer->state = PEER_DISCONNECTED;
        return -1;
    }

    agent_log("INFO", "connecting to %s (fd=%d)", peer->addr, fd);
    return 0;
}

/* ===================================================================
 * peer_connect_all - 连接所有未连接的节点
 * =================================================================== */
int peer_connect_all(void *vstate) {
    agent_state_t *state = (agent_state_t *)vstate;
    int connected = 0;

    for (int i = 0; i < state->peer_count; i++) {
        if (state->peers[i].state == PEER_DISCONNECTED) {
            if (peer_connect_one(state, &state->peers[i]) == 0)
                connected++;
        }
    }

    return connected;
}

/* ===================================================================
 * peer_find_by_fd - 通过 fd 查找对端
 * =================================================================== */
peer_t *peer_find_by_fd(void *vstate, int fd) {
    agent_state_t *state = (agent_state_t *)vstate;
    for (int i = 0; i < state->peer_count; i++) {
        if (state->peers[i].fd == fd)
            return &state->peers[i];
    }
    return NULL;
}

/* ===================================================================
 * peer_find_by_addr - 通过地址查找对端
 * =================================================================== */
peer_t *peer_find_by_addr(void *vstate, const char *addr) {
    agent_state_t *state = (agent_state_t *)vstate;
    for (int i = 0; i < state->peer_count; i++) {
        if (strcmp(state->peers[i].addr, addr) == 0)
            return &state->peers[i];
    }
    return NULL;
}

/* ===================================================================
 * peer_mark_offline - 标记节点离线
 * =================================================================== */
void peer_mark_offline(void *vstate, peer_t *peer) {
    agent_state_t *state = (agent_state_t *)vstate;
    (void)state;

    if (peer->state == PEER_OFFLINE || peer->state == PEER_DISCONNECTED)
        return;

    agent_log("WARN", "peer %s offline", peer->addr);
    close(peer->fd);
    peer->fd    = -1;
    peer->state = PEER_OFFLINE;
    peer->heartbeat_miss = 0;
    peer->next_reconnect = now_ms() + 1000;  /* 1s 后重连 */
}

/* ===================================================================
 * peer_check_timeout - 检查心跳超时
 * =================================================================== */
int peer_check_timeout(void *vstate, long now, int timeout_ms) {
    agent_state_t *state = (agent_state_t *)vstate;
    int timeout_count = 0;

    for (int i = 0; i < state->peer_count; i++) {
        peer_t *peer = &state->peers[i];
        if (peer->state == PEER_CONNECTED && peer->last_heartbeat > 0) {
            if (now - peer->last_heartbeat > timeout_ms) {
                peer->heartbeat_miss++;
                if (peer->heartbeat_miss >= 3) {
                    agent_log("WARN", "peer %s heartbeat timeout (miss=%d)",
                              peer->addr, peer->heartbeat_miss);
                }
                timeout_count++;
            }
        }
    }

    return timeout_count;
}

/* ===================================================================
 * peer_reconnect - 重连离线节点
 * =================================================================== */
void peer_reconnect(void *vstate, long now) {
    agent_state_t *state = (agent_state_t *)vstate;

    for (int i = 0; i < state->peer_count; i++) {
        peer_t *peer = &state->peers[i];
        if (peer->state == PEER_OFFLINE && now >= peer->next_reconnect) {
            agent_log("INFO", "reconnecting to %s", peer->addr);
            peer_connect_one(state, peer);
        }
    }
}

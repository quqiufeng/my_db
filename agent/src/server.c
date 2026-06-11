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
#include <netdb.h>

/* ===================================================================
 * Socket 工具
 * =================================================================== */

static int set_reuseaddr(int fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

/* ===================================================================
 * server_init - 创建监听 socket
 * =================================================================== */
int server_init(agent_state_t *state) {
    struct sockaddr_in addr;
    int fd, ret;

    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        agent_log("ERROR", "socket() failed: %s", strerror(errno));
        return -1;
    }

    set_reuseaddr(fd);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(state->config.port);
    if (inet_pton(AF_INET, state->config.bind_addr, &addr.sin_addr) <= 0)
        addr.sin_addr.s_addr = INADDR_ANY;

    ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0) {
        agent_log("ERROR", "bind(%s:%d) failed: %s",
                  state->config.bind_addr, state->config.port, strerror(errno));
        close(fd);
        return -1;
    }

    ret = listen(fd, 128);
    if (ret < 0) {
        agent_log("ERROR", "listen() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    state->listen_fd = fd;
    agent_log("INFO", "listening on %s:%d", state->config.bind_addr, state->config.port);
    return fd;
}

/* ===================================================================
 * epoll 初始化
 * =================================================================== */
int epoll_init(agent_state_t *state) {
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        agent_log("ERROR", "epoll_create1() failed: %s", strerror(errno));
        return -1;
    }
    state->epoll_fd = epfd;

    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = state->listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, state->listen_fd, &ev) < 0) {
        agent_log("ERROR", "epoll_ctl ADD listen_fd failed: %s", strerror(errno));
        close(epfd);
        return -1;
    }

    return epfd;
}

/* ===================================================================
 * 辅助：从 peer 列表中找到下一个在线的对手
 * =================================================================== */
static int find_next_online_opponent(agent_state_t *state, int start_idx) {
    for (int i = start_idx; i < state->peer_count; i++) {
        if (state->peers[i].state == PEER_CONNECTED)
            return i;
    }
    return -1;  /* 没有更多在线对手 */
}

/* ===================================================================
 * accept_connection - 接受新连接
 * =================================================================== */
static int accept_connection(agent_state_t *state) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int connfd;

    while (1) {
        connfd = accept4(state->listen_fd, (struct sockaddr*)&addr,
                         &addrlen, SOCK_NONBLOCK);
        if (connfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            agent_log("ERROR", "accept() failed: %s", strerror(errno));
            return -1;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        agent_log("INFO", "accept connection from %s:%d", ip, ntohs(addr.sin_port));

        /* try to match inbound connection to a peer by IP */
        {
            int matched = 0;
            for (int i = 0; i < state->peer_count && !matched; i++) {
                if (strcmp(state->peers[i].ip, ip) != 0) continue;
                if (state->peers[i].state != PEER_DISCONNECTED)
                    continue;  /* already have a working connection */
                if (state->peers[i].fd >= 0) {
                    epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, state->peers[i].fd, NULL);
                    close(state->peers[i].fd);
                }
                state->peers[i].fd = connfd;
                state->peers[i].state = PEER_CONNECTED;
                state->peers[i].heartbeat_miss = 0;
                state->peers[i].last_heartbeat = now_ms();
                agent_log("INFO", "peer %s connected (inbound, fd=%d)",
                          state->peers[i].addr, connfd);
                matched = 1;
            }
            if (!matched)
                agent_log("DEBUG", "inbound from %s (unmatched, fd=%d)", ip, connfd);
        }

        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = connfd;
        if (epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, connfd, &ev) < 0) {
            agent_log("ERROR", "epoll_ctl ADD conn failed: %s", strerror(errno));
            close(connfd);
            continue;
        }
    }
    return 0;
}

/* ===================================================================
 * 消息处理函数
 * =================================================================== */

/* Master 连接声明 */
static int on_master_hello(agent_state_t *state, int fd,
                           const char *payload, uint32_t len) {
    (void)payload; (void)len;
    agent_log("INFO", "master connected from fd=%d", fd);
    if (state->master_count < 16)
        state->master_fds[state->master_count++] = fd;

    /* 如果已经是 Leader，立即报备 */
    if (strcmp(state->ctx.role, "leader") == 0) {
        char report[2048];
        snprintf(report, sizeof(report),
            "{\"type\":\"master_leader\",\"leader\":\"%s\",\"epoch\":%d,"
            "\"cluster_size\":%d}",
            state->ctx.self_addr, state->ctx.epoch, state->ctx.cluster_size);
        send_message(fd, MSG_MASTER_LEADER, PROTO_FLAG_RESPONSE, report, strlen(report));
    }
    return 0;
}

/* Master 查询身份+状态 */
static int on_master_query(agent_state_t *state, int fd,
                           const char *payload, uint32_t len) {
    (void)payload; (void)len;
    char json[2048];
    session_ctx_to_json(&state->ctx, json, sizeof(json));
    send_message(fd, MSG_MASTER_STATUS, PROTO_FLAG_RESPONSE, json, strlen(json));
    return 0;
}


/* 收到 VoteReq：Candidate 请求投票 */
static int on_election_vote_req(agent_state_t *state, int fd,
                                const char *payload, uint32_t len) {
    (void)len;
    int req_term = 0;
    char candidate_addr[128] = {0};
    sscanf(payload, "{\"term\":%d,\"candidate\":\"%127[^\"]\"}", &req_term, candidate_addr);
    if (req_term < state->election.term) {
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"term\":%d,\"vote_granted\":0}", state->election.term);
        send_message(fd, MSG_ELECTION_VOTE_RESP, PROTO_FLAG_RESPONSE, resp, strlen(resp));
        return 0;
    }
    if (state->election.role == ELECTION_LEADER && req_term == state->election.term) {
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"term\":%d,\"vote_granted\":0}", state->election.term);
        send_message(fd, MSG_ELECTION_VOTE_RESP, PROTO_FLAG_RESPONSE, resp, strlen(resp));
        return 0;
    }
    if (req_term > state->election.term)
        election_become_follower(&state->election, req_term, now_ms());
    if (state->election.voted_for == -1) {
        state->election.voted_for = -2;
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"term\":%d,\"vote_granted\":1}", req_term);
        send_message(fd, MSG_ELECTION_VOTE_RESP, PROTO_FLAG_RESPONSE, resp, strlen(resp));
        agent_log("DEBUG", "vote granted for term %d candidate %s", req_term, candidate_addr);
    } else {
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"term\":%d,\"vote_granted\":0}", state->election.term);
        send_message(fd, MSG_ELECTION_VOTE_RESP, PROTO_FLAG_RESPONSE, resp, strlen(resp));
    }
    return 0;
}

/* 收到 VoteResp：投票响应 */
static int on_election_vote_resp(agent_state_t *state, int fd,
                                 const char *payload, uint32_t len) {
    (void)fd; (void)len;
    int resp_term = 0, vote_granted = 0;
    sscanf(payload, "{\"term\":%d,\"vote_granted\":%d}", &resp_term, &vote_granted);
    if (resp_term != state->election.term) return 0;
    if (state->election.role != ELECTION_CANDIDATE) return 0;
    if (vote_granted) {
        state->election.votes_received++;
        agent_log("DEBUG", "got vote, now %d/%d", 
                  state->election.votes_received, state->election.total_voters);
    }
    return 0;
}

/* 收到 COORD：有人宣布自己是 Leader */
static int on_coord(agent_state_t *state, int fd,
                    const char *payload, uint32_t len) {
    (void)fd; (void)len;
    char leader_addr[128] = {0};
    int  epoch = 0;
    sscanf(payload, "{%*[^:]:\"%127[^\"]\",%*[^:]:%d}", leader_addr, &epoch);
    if (epoch < state->election.term) return 0;
    agent_log("INFO", "LEADER ELECTED: %s epoch=%d", leader_addr, epoch);
    election_become_follower(&state->election, epoch, now_ms());
    snprintf(state->election.leader_addr, sizeof(state->election.leader_addr), "%s", leader_addr);
    state->election.leader_term = epoch;
    snprintf(state->ctx.leader_addr, sizeof(state->ctx.leader_addr), "%s", leader_addr);
    state->ctx.epoch = epoch;
    if (strcmp(leader_addr, state->ctx.self_addr) == 0)
        strncpy(state->ctx.role, "leader", sizeof(state->ctx.role) - 1);
    else
        strncpy(state->ctx.role, "follower", sizeof(state->ctx.role) - 1);
    return 0;
}

/* 收到 Leader 心跳 */
static int on_heartbeat(agent_state_t *state, int fd,
                        const char *payload, uint32_t len) {
    (void)len;
    int hb_term = 0;
    char leader_addr[128] = {0};
    sscanf(payload, "{\"term\":%d,\"leader\":\"%127[^\"]\"}", &hb_term, leader_addr);
    if (hb_term < state->election.term) return 0;
    if (hb_term > state->election.term || state->election.role == ELECTION_CANDIDATE)
        election_become_follower(&state->election, hb_term, now_ms());
    snprintf(state->election.leader_addr, sizeof(state->election.leader_addr), "%s", leader_addr);
    state->election.leader_term = hb_term;
    snprintf(state->ctx.leader_addr, sizeof(state->ctx.leader_addr), "%s", leader_addr);
    state->ctx.epoch = hb_term;
    strncpy(state->ctx.role, "follower", sizeof(state->ctx.role) - 1);
    election_reset_timer(&state->election, now_ms());
    peer_t *peer = peer_find_by_fd(state, fd);
    if (peer) {
        peer->last_heartbeat = now_ms();
        peer->heartbeat_miss = 0;
    }
    send_message(fd, MSG_HEARTBEAT_ACK, PROTO_FLAG_RESPONSE, "{}", 2);
    return 0;
}

static int on_heartbeat_ack(agent_state_t *state, int fd,
                            const char *payload, uint32_t len) {
    (void)state; (void)fd; (void)payload; (void)len;
    return 0;
}

static int on_ok(agent_state_t *state, int fd,
                 const char *payload, uint32_t len) {
    (void)state; (void)fd; (void)payload; (void)len;
    return 0;
}

/* ========== Task dispatch ========== */
static int on_task_dispatch(agent_state_t *state, int fd,
                            const char *payload, uint32_t len) {
    (void)fd;
    agent_log("DEBUG", "task dispatch: %.*s", (int)len, payload);
    char resp[2048];
    snprintf(resp, sizeof(resp),
        "{\"status\":\"ok\",\"node\":\"%s\",\"msg\":\"simulated\"}",
        state->ctx.self_addr);
    send_message(fd, MSG_TASK_RESULT, PROTO_FLAG_RESPONSE, resp, strlen(resp));
    return 0;
}

static int on_task_result(agent_state_t *state, int fd,
                          const char *payload, uint32_t len) {
    agent_log("DEBUG", "task result from fd=%d: %.*s", fd, (int)len, payload);
    return 0;
}

static int on_status_report(agent_state_t *state, int fd,
                            const char *payload, uint32_t len) {
    peer_t *peer = peer_find_by_fd(state, fd);
    if (peer) {
        json_to_session_ctx(payload, &peer->remote_ctx);
        peer->last_heartbeat = now_ms();
        peer->heartbeat_miss = 0;
    }
    return 0;
}

/* ========== Master command channel ========== */
static int on_master_cmd(agent_state_t *state, int fd,
                         const char *payload, uint32_t len) {
    if (strcmp(state->ctx.role, "leader") != 0) {
        char resp[256];
        snprintf(resp, sizeof(resp),
            "{\"error\":\"not_leader\",\"leader\":\"%s\"}",
            state->ctx.leader_addr);
        send_message(fd, MSG_MASTER_RESULT, PROTO_FLAG_RESPONSE, resp, strlen(resp));
        return 0;
    }
    agent_log("DEBUG", "master cmd: %.*s", (int)len, payload);
    char action[32] = {0};
    sscanf(payload, "{\"action\":\"%31[^\"]\"", action);

    if (strcmp(action, "exec") == 0) {
        char cmd[4096] = {0};
        int tmo = 30;
        sscanf(payload,
            "{\"action\":\"%*[^\"]\",\"cmd\":\"%4095[^\"]\",\"timeout\":%d}",
            cmd, &tmo);
        char resp[4096];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"task_id\":\"t_%ld\",\"results\":["
            "{\"node\":\"%s\",\"exit\":0,\"stdout\":\"(sim) %s\"}]}",
            now_ms(), state->ctx.self_addr, cmd);
        send_message(fd, MSG_MASTER_RESULT, PROTO_FLAG_RESPONSE, resp, strlen(resp));
    } else if (strcmp(action, "query") == 0) {
        char json[2048];
        session_ctx_to_json(&state->ctx, json, sizeof(json));
        send_message(fd, MSG_MASTER_RESULT, PROTO_FLAG_RESPONSE, json, strlen(json));
    } else {
        char resp[256];
        snprintf(resp, sizeof(resp), "{\"error\":\"unknown_action\",\"action\":\"%s\"}", action);
        send_message(fd, MSG_MASTER_RESULT, PROTO_FLAG_RESPONSE, resp, strlen(resp));
    }
    return 0;
}

static int on_master_status(agent_state_t *state, int fd,
                            const char *payload, uint32_t len) {
    (void)payload; (void)len;
    char json[2048];
    session_ctx_to_json(&state->ctx, json, sizeof(json));
    send_message(fd, MSG_MASTER_STATUS, PROTO_FLAG_RESPONSE, json, strlen(json));
    return 0;
}

/* ===================================================================
 * handle_message - 消息分发
 * =================================================================== */


static int handle_message(agent_state_t *state, int fd,
                          uint8_t type, uint8_t flags,
                          char *payload, uint32_t len) {
    (void)flags;
    switch (type) {
    case MSG_MASTER_HELLO:  return on_master_hello(state, fd, payload, len);
    case MSG_MASTER_QUERY:  return on_master_query(state, fd, payload, len);
    case MSG_MASTER_CMD:    return on_master_cmd(state, fd, payload, len);
    case MSG_MASTER_STATUS: return on_master_status(state, fd, payload, len);
    case MSG_ELECTION_VOTE_REQ:  return on_election_vote_req(state, fd, payload, len);
    case MSG_ELECTION_VOTE_RESP: return on_election_vote_resp(state, fd, payload, len);
    case MSG_COORD:         return on_coord(state, fd, payload, len);
    case MSG_OK:            return on_ok(state, fd, payload, len);
    case MSG_HEARTBEAT:     return on_heartbeat(state, fd, payload, len);
    case MSG_HEARTBEAT_ACK: return on_heartbeat_ack(state, fd, payload, len);
    case MSG_TASK_DISPATCH: return on_task_dispatch(state, fd, payload, len);
    case MSG_TASK_RESULT:   return on_task_result(state, fd, payload, len);
    case MSG_STATUS_REPORT: return on_status_report(state, fd, payload, len);
    default:
        agent_log("WARN", "unknown message type: 0x%02X", type);
        return 0;
    }
}

/* ===================================================================
 * handle_peer_connect - 处理异步连接完成
 * =================================================================== */
static int handle_peer_connect(agent_state_t *state, int fd) {
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
        peer_t *peer = peer_find_by_fd(state, fd);
        if (peer) {
            agent_log("WARN", "connect to %s failed: %s", peer->addr, strerror(err));
            close(fd);
            peer->fd = -1;
            peer->state = PEER_DISCONNECTED;
            peer->next_reconnect = now_ms() + 5000;
        }
        return -1;
    }

    /* 连接成功 */
    peer_t *peer = peer_find_by_fd(state, fd);
    if (!peer) {
        for (int i = 0; i < state->peer_count; i++) {
            if (state->peers[i].state == PEER_CONNECTING && state->peers[i].fd == fd) {
                peer = &state->peers[i];
                break;
            }
        }
    }
    if (!peer) {
        agent_log("WARN", "connect fd=%d: no matching peer", fd);
        close(fd);
        return -1;
    }
    if (peer->state == PEER_CONNECTED) {
        close(fd);
        return 0;
    }
    peer->state = PEER_CONNECTED;
    peer->heartbeat_miss = 0;
    peer->last_heartbeat = now_ms();
    agent_log("INFO", "connected to %s (fd=%d)", peer->addr, fd);

    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(state->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        agent_log("ERROR", "epoll_ctl MOD fd=%d failed: %s", fd, strerror(errno));
        close(fd);
        return -1;
    }

    return 0;

}

/* ===================================================================
 * handle_read - 处理可读事件
 * =================================================================== */

/* ===================================================================
 * handle_read - 处理可读事件
 * =================================================================== */
static int handle_read(agent_state_t *state, int fd) {
    if (fd == state->listen_fd)
        return accept_connection(state);

    uint8_t  type, flags;
    uint32_t payload_len;
    char     payload[PROTO_MAX_PAYLOAD];

    int ret = recv_message(fd, &type, &flags, payload, &payload_len);
    if (ret < 0) {
        /* 连接断开 */
        peer_t *peer = peer_find_by_fd(state, fd);
        if (peer) {
            agent_log("INFO", "peer %s disconnected", peer->addr);
            peer_mark_offline(state, peer);
        } else {
            agent_log("INFO", "fd=%d disconnected", fd);
            for (int i = 0; i < state->master_count; i++) {
                if (state->master_fds[i] == fd) {
                    state->master_fds[i] = state->master_fds[--state->master_count];
                    break;
                }
            }
        }
        epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        return 0;
    }

    return handle_message(state, fd, type, flags, payload, payload_len);
}

/* ===================================================================
 * election_tick - 顺序 PK 选举状态机
 * =================================================================== */
/* ===================================================================
 * election_tick - 顺序 PK 选举状态机
 * =================================================================== */
static void election_tick(agent_state_t *state, long now) {
    /* ========= Follower: election timeout -> candidate ========= */
    if (state->election.role == ELECTION_FOLLOWER) {
        if (state->election.leader_addr[0] != '\0')
            return;
        if (election_check_timeout(&state->election, now)) {
            int total = state->ctx.cluster_size;
            agent_log("INFO", "election timeout, becoming candidate for term %d (total=%d)",
                      state->election.term + 1, total);
            election_become_candidate(&state->election, now, total);
            state->election.leader_term = -1;
            char req[256];
            snprintf(req, sizeof(req),
                "{\"term\":%d,\"candidate\":\"%s\"}",
                state->election.term, state->ctx.self_addr);
            for (int i = 0; i < state->peer_count; i++) {
                if (state->peers[i].state == PEER_CONNECTED)
                    send_message(state->peers[i].fd, MSG_ELECTION_VOTE_REQ,
                                 PROTO_FLAG_REQUEST, req, strlen(req));
            }
        }
    }
    /* ========= Candidate: check majority or timeout ========= */
    if (state->election.role == ELECTION_CANDIDATE) {
        if (election_is_majority(state->election.votes_received, state->election.total_voters)) {
            agent_log("INFO", "I WON the election! term=%d votes=%d/%d",
                      state->election.term,
                      state->election.votes_received,
                      state->election.total_voters);
            election_become_leader(&state->election);
            state->election.leader_term = state->election.term;
            snprintf(state->election.leader_addr, sizeof(state->election.leader_addr),
                     "%s", state->ctx.self_addr);
            strncpy(state->ctx.role, "leader", sizeof(state->ctx.role) - 1);
            state->ctx.epoch = state->election.term;
            /* broadcast COORD */
            char coord[256];
            snprintf(coord, sizeof(coord),
                "{\"leader\":\"%s\",\"epoch\":%d}",
                state->ctx.self_addr, state->election.term);
            for (int i = 0; i < state->peer_count; i++) {
                if (state->peers[i].state == PEER_CONNECTED)
                    send_message(state->peers[i].fd, MSG_COORD,
                                 PROTO_FLAG_REQUEST, coord, strlen(coord));
            }
            /* Master report */
            for (int i = 0; i < state->master_count; i++) {
                char report[2048];
                snprintf(report, sizeof(report),
                    "{\"type\":\"master_leader\",\"leader\":\"%s\",\"epoch\":%d,"
                    "\"cluster_size\":%d}",
                    state->ctx.self_addr, state->ctx.epoch, state->ctx.cluster_size);
                send_message(state->master_fds[i], MSG_MASTER_LEADER,
                             PROTO_FLAG_RESPONSE, report, strlen(report));
            }
            return;
        }
        if (now >= state->election.vote_deadline) {
            agent_log("WARN", "vote timeout, votes=%d/%d, restarting election",
                      state->election.votes_received,
                      state->election.total_voters);
            int total = state->ctx.cluster_size;
            election_become_candidate(&state->election, now, total);
            char req[256];
            snprintf(req, sizeof(req),
                "{\"term\":%d,\"candidate\":\"%s\"}",
                state->election.term, state->ctx.self_addr);
            for (int i = 0; i < state->peer_count; i++) {
                if (state->peers[i].state == PEER_CONNECTED)
                    send_message(state->peers[i].fd, MSG_ELECTION_VOTE_REQ,
                                 PROTO_FLAG_REQUEST, req, strlen(req));
            }
        }
    }
    /* ========= Leader: send heartbeat ========= */
    if (state->election.role == ELECTION_LEADER) {
        if (now - state->election.last_heartbeat >= state->election.heartbeat_interval) {
            state->election.last_heartbeat = now;
            char hb[256];
            snprintf(hb, sizeof(hb),
                "{\"term\":%d,\"leader\":\"%s\"}",
                state->election.term, state->ctx.self_addr);
            for (int i = 0; i < state->peer_count; i++) {
                if (state->peers[i].state == PEER_CONNECTED)
                    send_message(state->peers[i].fd, MSG_HEARTBEAT,
                                 PROTO_FLAG_REQUEST, hb, strlen(hb));
            }
        }
    }
}

static void heartbeat_send(agent_state_t *state, long now) {
    (void)state; (void)now;
    static long last_send = 0;
    if (now - last_send < 3000) return;
    last_send = now;

    if (strcmp(state->ctx.role, "follower") != 0) return;
    if (state->election.leader_addr[0] == '\0') return;

    peer_t *leader = peer_find_by_addr(state, state->election.leader_addr);
    if (!leader || leader->state != PEER_CONNECTED) return;

    char json[2048];
    session_ctx_to_json(&state->ctx, json, sizeof(json));
    send_message(leader->fd, MSG_HEARTBEAT, PROTO_FLAG_REQUEST, json, strlen(json));
}

/* ===================================================================
 * server_event_loop - 主事件循环
 * =================================================================== */
int server_event_loop(agent_state_t *state) {
    struct epoll_event events[256];
    int nfds;

    agent_log("INFO", "entering event loop");

    while (state->running) {
        nfds = epoll_wait(state->epoll_fd, events, 256, 100);

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (events[i].events & EPOLLOUT)
                handle_peer_connect(state, fd);

            if (events[i].events & EPOLLIN)
                handle_read(state, fd);

            /* drain any remaining data before processing error/hup */
            if (events[i].events & EPOLLIN) {
                uint8_t _tmp[256];
                int _n;
                while ((_n = read(fd, _tmp, sizeof(_tmp))) > 0) {}
                if (_n < 0 && errno != EAGAIN) {
                    /* read error, will be handled by EPOLLERR below */
                }
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                peer_t *peer = peer_find_by_fd(state, fd);
                if (peer) {
                    agent_log("WARN", "peer %s error/hup", peer->addr);
                    peer_mark_offline(state, peer);
                }
                epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
            }
        }

        /* 定时任务 */
        long now = now_ms();
        peer_check_timeout(state, now, 9000);
        peer_reconnect(state, now);
        election_tick(state, now);
        heartbeat_send(state, now);
    }

    return 0;
}

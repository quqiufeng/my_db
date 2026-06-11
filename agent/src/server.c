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
    ev.events  = EPOLLIN | EPOLLET;
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

        struct epoll_event ev;
        ev.events  = EPOLLIN | EPOLLET;
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

/* 收到 PK 挑战：我是被挑战方 */
static int on_pk_challenge(agent_state_t *state, int fd,
                           const char *payload, uint32_t len) {
    (void)len;
    int candidate_idx = -1, opponent_idx = -1, candidate_val = -1, epoch = 0;
    sscanf(payload,
        "%*[^{]{%*[^:]:%d,%*[^:]:%d,%*[^:]:%d,%*[^:]:%d",
        &candidate_idx, &opponent_idx, &candidate_val, &epoch);

    if (epoch != state->election.epoch) {
        agent_log("WARN", "pk_challenge ignored: epoch mismatch (%d vs %d)",
                  epoch, state->election.epoch);
        return 0;
    }

    /* 我是不是本轮对手？ */
    if (opponent_idx != state->election.my_index) {
        agent_log("WARN", "pk_challenge not for me (opponent=%d, I'm %d)",
                  opponent_idx, state->election.my_index);
        return 0;
    }

    agent_log("DEBUG", "pk_challenge: cand=%d val=%d vs me(%d)", 
              candidate_idx, candidate_val, state->election.my_index);

    /* 记录擂主信息 */
    state->election.candidate_idx = candidate_idx;
    state->election.candidate_val = candidate_val;
    state->election.opponent_idx  = opponent_idx;

    /* 生成我的随机数并回复 */
    int my_val = election_generate_val(&state->election);
    char reply[256];
    snprintf(reply, sizeof(reply),
        "{\"candidate_idx\":%d,\"opponent_idx\":%d,\"val\":%d,\"epoch\":%d}",
        candidate_idx, state->election.my_index, my_val, state->election.epoch);
    send_message(fd, MSG_PK_RESULT, PROTO_FLAG_RESPONSE, reply, strlen(reply));

    return 0;
}

/* 收到 PK 结果：我是挑战方 */
static int on_pk_result(agent_state_t *state, int fd,
                        const char *payload, uint32_t len) {
    (void)fd; (void)len;
    int candidate_idx = -1, opponent_idx = -1, opponent_val = -1, epoch = 0;
    sscanf(payload,
        "%*[^{]{%*[^:]:%d,%*[^:]:%d,%*[^:]:%d,%*[^:]:%d",
        &candidate_idx, &opponent_idx, &opponent_val, &epoch);

    if (epoch != state->election.epoch) {
        agent_log("WARN", "pk_result ignored: epoch mismatch");
        return 0;
    }

    /* 我必须是擂主 */
    if (candidate_idx != state->election.my_index)
        return 0;

    agent_log("DEBUG", "pk_result: me(candidate=%d val=%d) vs opponent=%d val=%d",
              candidate_idx, state->election.candidate_val,
              opponent_idx, opponent_val);

    /* 判断胜负 */
    int winner, loser, done;
    election_handle_result(&state->election, now_ms(), opponent_val,
                           &winner, &loser, &done);

    char winner_addr[128] = {0};
    char loser_addr[128]  = {0};

    if (winner == state->election.my_index) {
        /* 我赢了，找下一个对手 */
        snprintf(winner_addr, sizeof(winner_addr), "%s", state->ctx.self_addr);
        snprintf(loser_addr, sizeof(loser_addr), "%s",
                 state->peers[loser].addr);

        int next = find_next_online_opponent(state, opponent_idx + 1);
        if (next < 0) {
            /* 没有更多对手了！我当选 Leader */
            agent_log("INFO", "I WON the election! No more opponents.");
            election_finish(&state->election, state->ctx.self_addr);
            strncpy(state->ctx.role, "leader", sizeof(state->ctx.role) - 1);
            state->ctx.epoch = state->election.epoch;

            /* 广播 MSG_COORD 给所有人 */
            char coord[256];
            snprintf(coord, sizeof(coord),
                "{\"leader\":\"%s\",\"epoch\":%d}",
                state->ctx.self_addr, state->election.epoch);
            for (int i = 0; i < state->peer_count; i++) {
                if (state->peers[i].state == PEER_CONNECTED)
                    send_message(state->peers[i].fd, MSG_COORD,
                                 PROTO_FLAG_REQUEST, coord, strlen(coord));
            }

            /* 向 Master 报备 */
            for (int i = 0; i < state->master_count; i++) {
                char report[2048];
                snprintf(report, sizeof(report),
                    "{\"type\":\"master_leader\",\"leader\":\"%s\",\"epoch\":%d,"
                    "\"cluster_size\":%d}",
                    state->ctx.self_addr, state->ctx.epoch, state->ctx.cluster_size);
                send_message(state->master_fds[i], MSG_MASTER_LEADER,
                             PROTO_FLAG_RESPONSE, report, strlen(report));
            }
        } else {
            /* 挑战下一个对手 */
            int my_val = election_generate_val(&state->election);
            char challenge[256];
            snprintf(challenge, sizeof(challenge),
                "{\"candidate_idx\":%d,\"opponent_idx\":%d,"
                "\"val\":%d,\"epoch\":%d}",
                state->election.my_index, next,
                my_val, state->election.epoch);
            send_message(state->peers[next].fd, MSG_PK_CHALLENGE,
                         PROTO_FLAG_REQUEST, challenge, strlen(challenge));

            state->election.opponent_idx = next;
            state->election.waiting_for  = state->peers[next].fd;
            state->election.deadline     = now_ms() + state->election.pk_timeout_ms;
            agent_log("DEBUG", "advance: I(winner) challenge peer[%d]", next);
        }
    } else {
        /* 对手赢了，由对手继续挑战下一个 */
        snprintf(loser_addr, sizeof(loser_addr), "%s", state->ctx.self_addr);
        snprintf(winner_addr, sizeof(winner_addr), "%s",
                 state->peers[winner].addr);

        /* 跳过败者，从胜者下一个开始找 */
        int next = find_next_online_opponent(state, winner + 1);
        if (next < 0) {
            /* 对手直接获胜 */
            char coord[256];
            snprintf(coord, sizeof(coord),
                "{\"leader\":\"%s\",\"epoch\":%d}",
                state->peers[winner].addr, state->election.epoch);
            for (int i = 0; i < state->peer_count; i++) {
                if (state->peers[i].state == PEER_CONNECTED)
                    send_message(state->peers[i].fd, MSG_COORD,
                                 PROTO_FLAG_REQUEST, coord, strlen(coord));
            }
            /* 更新我自己的状态 */
            state->election.phase = ELECTION_DONE;
            snprintf(state->election.leader_addr, sizeof(state->election.leader_addr), "%s", state->peers[winner].addr);
            snprintf(state->ctx.leader_addr, sizeof(state->ctx.leader_addr), "%s", state->peers[winner].addr);
            state->ctx.epoch = state->election.epoch;
            strncpy(state->ctx.role, "follower", sizeof(state->ctx.role) - 1);
        } else {
            /* 通知胜者去挑战下一个 */
            char handover[256];
            snprintf(handover, sizeof(handover),
                "{\"winner_idx\":%d,\"next_opponent_idx\":%d,\"epoch\":%d}",
                winner, next, state->election.epoch);
            send_message(state->peers[winner].fd, MSG_PK_CHALLENGE,
                         PROTO_FLAG_REQUEST, handover, strlen(handover));
        }
    }

    return 0;
}

/* 收到 COORD：有人宣布自己是 Leader */
static int on_coord(agent_state_t *state, int fd,
                    const char *payload, uint32_t len) {
    (void)fd; (void)len;
    char leader_addr[64] = {0};
    int  epoch = 0;
    sscanf(payload, "%*[^{]{%*[^:]:\"%63[^\"]\",%*[^:]:%d", leader_addr, &epoch);

    if (epoch < state->election.epoch) return 0;  /* 旧消息 */

    agent_log("INFO", "LEADER ELECTED: %s epoch=%d", leader_addr, epoch);

    state->election.phase = ELECTION_DONE;
    snprintf(state->election.leader_addr, sizeof(state->election.leader_addr), "%s", leader_addr);
    state->election.leader_epoch = epoch;
    snprintf(state->ctx.leader_addr, sizeof(state->ctx.leader_addr), "%s", leader_addr);
    state->ctx.epoch = epoch;

    if (strcmp(leader_addr, state->ctx.self_addr) == 0) {
        strncpy(state->ctx.role, "leader", sizeof(state->ctx.role) - 1);
    } else {
        strncpy(state->ctx.role, "follower", sizeof(state->ctx.role) - 1);
    }

    return 0;
}

/* 心跳 */
static int on_heartbeat(agent_state_t *state, int fd,
                        const char *payload, uint32_t len) {
    (void)len;
    session_ctx_t remote;
    if (json_to_session_ctx(payload, &remote) == 0) {
        peer_t *peer = peer_find_by_addr(state, remote.self_addr);
        if (peer) {
            peer->remote_ctx = remote;
            peer->last_heartbeat = now_ms();
            peer->heartbeat_miss = 0;
        }
    }
    send_message(fd, MSG_HEARTBEAT_ACK, PROTO_FLAG_RESPONSE, "{}", 2);
    return 0;
}

static int on_heartbeat_ack(agent_state_t *state, int fd,
                            const char *payload, uint32_t len) {
    (void)state; (void)fd; (void)payload; (void)len;
    return 0;  /* 暂不处理 */
}

static int on_ok(agent_state_t *state, int fd,
                 const char *payload, uint32_t len) {
    (void)state; (void)fd; (void)payload; (void)len;
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
    case MSG_PK_CHALLENGE:  return on_pk_challenge(state, fd, payload, len);
    case MSG_PK_RESULT:     return on_pk_result(state, fd, payload, len);
    case MSG_COORD:         return on_coord(state, fd, payload, len);
    case MSG_OK:            return on_ok(state, fd, payload, len);
    case MSG_HEARTBEAT:     return on_heartbeat(state, fd, payload, len);
    case MSG_HEARTBEAT_ACK: return on_heartbeat_ack(state, fd, payload, len);
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
    if (peer) {
        peer->state = PEER_CONNECTED;
        peer->heartbeat_miss = 0;
        peer->last_heartbeat = now_ms();
        agent_log("INFO", "connected to %s (fd=%d)", peer->addr, fd);

        /* 改为监听可读事件 */
        struct epoll_event ev;
        ev.events  = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        epoll_ctl(state->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }

    return 0;
}

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
 * election_sequential_pk - 顺序 PK 选举状态机
 * =================================================================== */
/* ===================================================================
 * election_sequential_pk - 顺序 PK 选举状态机
 * =================================================================== */
static void election_sequential_pk(agent_state_t *state, long now) {
    static long self_promotion_start[256];
    static int  self_promotion_ok = 0;
    (void)self_promotion_start;

    /* ========= 首次或重选 ========= */
    if (state->election.phase == ELECTION_IDLE &&
        state->peer_count > 0) {

        int has_leader = (state->election.leader_addr[0] != '\0');
        int leader_alive = 0;

        /* 如果有 Leader，检查它是否还活着 */
        if (has_leader) {
            for (int i = 0; i < state->peer_count; i++) {
                if (strcmp(state->peers[i].addr, state->election.leader_addr) == 0 &&
                    state->peers[i].state == PEER_CONNECTED &&
                    state->peers[i].heartbeat_miss < 3) {
                    leader_alive = 1;
                    break;
                }
            }
        }

        /* Leader 活着，什么都不做 */
        if (has_leader && leader_alive)
            return;

        /* Leader 死了或没有 Leader */
        if (has_leader && !leader_alive)
            agent_log("WARN", "leader lost, starting election");

        /* 节点 0：立即发起选举 */
        if (state->election.my_index == 0) {
            if (!has_leader || !leader_alive) {
                agent_log("INFO", "I'm peer[0], starting sequential election");
                election_start(&state->election, now);
                election_generate_val(&state->election);

                int first = find_next_online_opponent(state, 1);
                if (first >= 0) {
                    char challenge[256];
                    snprintf(challenge, sizeof(challenge),
                        "{\"candidate_idx\":0,\"opponent_idx\":%d,"
                        "\"val\":%d,\"epoch\":%d}",
                        first, state->election.candidate_val,
                        state->election.epoch);
                    send_message(state->peers[first].fd, MSG_PK_CHALLENGE,
                                 PROTO_FLAG_REQUEST, challenge, strlen(challenge));
                    state->election.opponent_idx = first;
                    state->election.waiting_for  = state->peers[first].fd;
                    state->election.deadline     = now + state->election.pk_timeout_ms;
                    agent_log("INFO", "election: I(peer[0]) challenge peer[%d]", first);
                } else {
                    agent_log("INFO", "no online peers, I'm leader by default");
                    election_finish(&state->election, state->ctx.self_addr);
                    strncpy(state->ctx.role, "leader", sizeof(state->ctx.role) - 1);
                    state->ctx.epoch = state->election.epoch;
                }
            }
        }
        /* 非 0 节点：记录等待开始时间，但不阻塞 */
        else if (!has_leader) {
            /* 如果还没记录过等待开始时间，记下 */
            if (!self_promotion_ok) {
                /* 每个节点只需要检查自己是不是该自升了 */
                self_promotion_ok = 1;
            }
        }
        return;
    }

    /* ========= PK 超时处理：挑战者跳过不响应的对手 ========= */
    if (state->election.phase == ELECTION_RUNNING &&
        state->election.waiting_for >= 0 &&
        now >= state->election.deadline) {

        int opponent = state->election.opponent_idx;
        agent_log("WARN", "PK timeout waiting for peer[%d], skipping", opponent);

        if (state->election.my_index == state->election.candidate_idx) {
            int next = find_next_online_opponent(state, opponent + 1);
            if (next >= 0) {
                int my_val = election_generate_val(&state->election);
                char challenge[256];
                snprintf(challenge, sizeof(challenge),
                    "{\"candidate_idx\":%d,\"opponent_idx\":%d,"
                    "\"val\":%d,\"epoch\":%d}",
                    state->election.my_index, next,
                    my_val, state->election.epoch);
                send_message(state->peers[next].fd, MSG_PK_CHALLENGE,
                             PROTO_FLAG_REQUEST, challenge, strlen(challenge));
                state->election.opponent_idx = next;
                state->election.waiting_for  = state->peers[next].fd;
                state->election.deadline     = now + state->election.pk_timeout_ms;
            } else {
                /* 所有对手都跳过了，我赢 */
                agent_log("INFO", "all opponents skipped, I'm leader!");
                election_finish(&state->election, state->ctx.self_addr);
                strncpy(state->ctx.role, "leader", sizeof(state->ctx.role) - 1);
                state->ctx.epoch = state->election.epoch;

                char coord[256];
                snprintf(coord, sizeof(coord),
                    "{\"leader\":\"%s\",\"epoch\":%d}",
                    state->ctx.self_addr, state->election.epoch);
                for (int i = 0; i < state->peer_count; i++) {
                    if (state->peers[i].state == PEER_CONNECTED)
                        send_message(state->peers[i].fd, MSG_COORD,
                                     PROTO_FLAG_REQUEST, coord, strlen(coord));
                }
            }
        }
    }

    /* ========= 自升兜底：非 0 节点长时间等不到挑战 ========= */
    /* 只有完全空闲（无 Leader、无正在进行的选举）时才触发 */
    if (state->election.phase == ELECTION_IDLE &&
        state->election.leader_addr[0] == '\0' &&
        state->election.my_index > 0 &&
        state->peer_count > 0) {

        /* 找到第一个比我小的在线节点 */
        int smaller_online = 0;
        for (int i = 0; i < state->election.my_index && i < state->peer_count; i++) {
            if (state->peers[i].state == PEER_CONNECTED) {
                smaller_online = 1;
                break;
            }
        }

        /* 如果前面有节点在线，他们应该会发起选举，我不用管 */
        if (smaller_online)
            return;

        /* 前面没有在线节点，我应该自升为候选 */
        /* 但给一个初始延迟，避免启动瞬间就自升 */
        static long promote_check_time = 0;
        if (promote_check_time == 0)
            promote_check_time = now;

        int wait_min = 5 * (state->election.my_index + 1);  /* peer[1]=10min, peer[2]=15min... */
        if (now - promote_check_time > wait_min * 60 * 1000) {
            agent_log("INFO", "self-promote after timeout: I(peer[%d]) take over!",
                      state->election.my_index);
            election_start(&state->election, now);
            election_generate_val(&state->election);

            int next = find_next_online_opponent(state, state->election.my_index + 1);
            if (next >= 0) {
                char challenge[256];
                snprintf(challenge, sizeof(challenge),
                    "{\"candidate_idx\":%d,\"opponent_idx\":%d,"
                    "\"val\":%d,\"epoch\":%d}",
                    state->election.my_index, next,
                    state->election.candidate_val, state->election.epoch);
                send_message(state->peers[next].fd, MSG_PK_CHALLENGE,
                             PROTO_FLAG_REQUEST, challenge, strlen(challenge));
                state->election.opponent_idx = next;
                state->election.waiting_for  = state->peers[next].fd;
                state->election.deadline     = now + state->election.pk_timeout_ms;
            } else {
                agent_log("INFO", "no online peers after me, I'm leader!");
                election_finish(&state->election, state->ctx.self_addr);
                strncpy(state->ctx.role, "leader", sizeof(state->ctx.role) - 1);
                state->ctx.epoch = state->election.epoch;
            }
            promote_check_time = 0;  /* 重置，防止重复触发 */
        }
    }
}

/* ===================================================================
 * heartbeat_send - Follower 向 Leader 发心跳
 * =================================================================== */
static void heartbeat_send(agent_state_t *state, long now) {
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
        election_sequential_pk(state, now);
        heartbeat_send(state, now);
    }

    return 0;
}

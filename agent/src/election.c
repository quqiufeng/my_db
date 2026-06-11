#include "election.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* 选举超时范围（毫秒）*/
#define ELECTION_TIMEOUT_MIN  1500
#define ELECTION_TIMEOUT_MAX  3000
#define HEARTBEAT_INTERVAL    1000

/* 生成 [min, max] 区间内的随机数 */
static int rand_range(int min, int max) {
    if (max <= min) return min;
    return min + rand() % (max - min + 1);
}

void election_init(election_t *e, const char *node_id, int my_index, int term) {
    memset(e, 0, sizeof(*e));
    e->role              = ELECTION_FOLLOWER;
    e->term              = term;
    e->voted_for         = -1;
    e->my_index          = my_index;
    e->leader_addr[0]    = '\0';
    e->leader_term       = -1;
    e->election_deadline = 0;
    e->heartbeat_interval = HEARTBEAT_INTERVAL;
    strncpy(e->my_id, node_id, sizeof(e->my_id) - 1);
}

/* 首次启动时调用，初始化 election_deadline */
void election_init_timer(election_t *e, long now_ms) {
    election_reset_timer(e, now_ms);
}

void election_reset_timer(election_t *e, long now_ms) {
    int timeout = rand_range(ELECTION_TIMEOUT_MIN, ELECTION_TIMEOUT_MAX);
    e->election_base_ms = timeout;
    e->election_deadline = now_ms + timeout;
}

void election_become_follower(election_t *e, int term, long now_ms) {
    e->role      = ELECTION_FOLLOWER;
    e->term      = term;
    e->voted_for = -1;
    e->votes_received = 0;
    e->leader_addr[0] = '\0';
    election_reset_timer(e, now_ms);
}

void election_become_candidate(election_t *e, long now_ms, int total) {
    e->role           = ELECTION_CANDIDATE;
    e->term++;   /* 递增任期 */
    e->voted_for      = e->my_index;  /* 投给自己 */
    e->votes_received = 1;             /* 自己的票 */
    e->total_voters   = total;
    e->vote_deadline  = now_ms + 3000;  /* 最多等 3 秒收票 */
    e->leader_addr[0] = '\0';
    election_reset_timer(e, now_ms);
}

void election_become_leader(election_t *e) {
    e->role         = ELECTION_LEADER;
    e->leader_term  = e->term;
    e->voted_for    = -1;
    e->votes_received = 0;
    e->last_heartbeat = 0;  /* 触发立即发心跳 */
}

int election_check_timeout(const election_t *e, long now_ms) {
    if (e->election_deadline == 0) return 0;
    return (now_ms >= e->election_deadline) ? 1 : 0;
}

int election_is_majority(int votes, int total) {
    /* 多数 = floor(n/2) + 1，对包含自己的集群 */
    return votes >= (total / 2) + 1;
}

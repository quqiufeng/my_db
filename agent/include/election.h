#ifndef ELECTION_H
#define ELECTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * Raft 角色
 * =================================================================== */
typedef enum {
    ELECTION_FOLLOWER  = 0,
    ELECTION_CANDIDATE = 1,
    ELECTION_LEADER    = 2,
} election_role_t;

/* ===================================================================
 * 选举状态 — Raft 风格
 * =================================================================== */
typedef struct {
    election_role_t role;              /* 当前角色 */
    int             term;              /* 当前任期（每次选举递增） */
    int             voted_for;         /* 本任期投票给了谁的 my_index，-1 = 未投 */
    char            my_id[64];         /* 节点 ID */
    int             my_index;          /* 本节点在 peer 列表中的索引 */

    /* 当前 Leader */
    char            leader_addr[128];  /* 当前认可的 Leader 地址 */
    int             leader_term;       /* Leader 所在任期 */

    /* 选举定时器 */
    long            election_deadline; /* 下次选举超时时间戳（ms） */
    int             election_base_ms;  /* 超时基值（ms），启动时随机化 */

    /* Candidate 投票统计 */
    int             votes_received;    /* 已收到赞成票数 */
    int             total_voters;      /* 集群总节点数（含自己） */
    long            vote_deadline;     /* 投票收集截止时间 */

    /* 心跳 */
    long            last_heartbeat;    /* 最后收到/发送心跳的时间（ms） */
    int             heartbeat_interval;/* 心跳间隔（ms），默认 1000 */
} election_t;

/* ===================================================================
 * 选举函数
 * =================================================================== */

/**
 * election_init - 初始化选举状态
 */
void election_init(election_t *e, const char *node_id, int my_index, int term);
void election_init_timer(election_t *e, long now_ms);

/**
 * election_reset_timer - 重置选举超时定时器（随机化）
 */
void election_reset_timer(election_t *e, long now_ms);

/**
 * election_become_follower - 成为 Follower
 */
void election_become_follower(election_t *e, int term, long now_ms);

/**
 * election_become_candidate - 成为 Candidate，发起选举
 */
void election_become_candidate(election_t *e, long now_ms, int total);

/**
 * election_become_leader - 赢得选举，成为 Leader
 */
void election_become_leader(election_t *e);

/**
 * election_check_timeout - 检查选举超时是否触发
 * @return 1=超时触发，0=未触发
 */
int  election_check_timeout(const election_t *e, long now_ms);

/**
 * election_is_majority - 判断是否达到多数票
 */
int  election_is_majority(int votes, int total);

#ifdef __cplusplus
}
#endif

#endif /* ELECTION_H */

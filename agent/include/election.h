#ifndef ELECTION_H
#define ELECTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * 选举阶段
 * =================================================================== */
typedef enum {
    ELECTION_IDLE       = 0,   /* 未在选举，有正常 Leader */
    ELECTION_WAITING,          /* 等待被挑战 */
    ELECTION_RUNNING,          /* 选举进行中 */
    ELECTION_DONE,             /* 选举完成 */
} election_phase_t;

/* ===================================================================
 * 选举状态 — 顺序 PK 擂台赛
 *
 * 按 -ip 列表顺序，第一个节点发起，依次 PK：
 *   节点[0] PK 节点[1] → 胜者 PK 节点[2] → 胜者 PK 节点[3] → ... → 最后胜者 = Leader
 * =================================================================== */
typedef struct {
    election_phase_t phase;       /* 当前阶段 */

    int              epoch;       /* 纪元号，每次选举递增 */
    char             my_id[64];   /* 本节点 ID */
    int              my_index;    /* 本节点在 peer 列表中的索引 */

    /* PK 状态 */
    int              candidate_idx;  /* 当前擂主在 peer 列表中的索引 */
    int              candidate_val;  /* 当前擂主的随机数 */
    int              opponent_idx;   /* 当前挑战对象的索引 */
    int              waiting_for;    /* 正在等待响应的 fd，-1 表示不等待 */

    /* 定时器 */
    long             deadline;       /* 当前步骤截止时间（ms） */
    int              pk_timeout_ms;  /* PK 响应超时（默认 5000ms） */
    int              wait_timeout_ms;/* 等待被挑战超时（按 index 动态计算） */
    long             wait_start;     /* 开始等待的时间戳 */

    /* 选举结果 */
    char             leader_addr[128]; /* 最终 Leader 地址 */
    int              leader_epoch;    /* Leader 所在纪元 */
} election_t;

/* ===================================================================
 * 选举函数
 * =================================================================== */

void election_init(election_t *e, const char *node_id, int my_index, int epoch);
void election_start(election_t *e, long now_ms);
int  election_generate_val(election_t *e);
int  election_my_turn(const election_t *e, int my_index);
int  election_is_my_battle(const election_t *e, int my_index);
int  election_handle_result(election_t *e, long now_ms, int opponent_val,
                            int *winner, int *loser, int *done);
int  election_advance(election_t *e, long now_ms, int winner_idx);
void election_finish(election_t *e, const char *leader_addr);
int  election_check_timeout(const election_t *e, long now_ms);

#ifdef __cplusplus
}
#endif

#endif /* ELECTION_H */

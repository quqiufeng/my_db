#include "election.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void election_init(election_t *e, const char *node_id, int my_index, int epoch) {
    memset(e, 0, sizeof(*e));
    e->phase           = ELECTION_IDLE;
    e->epoch           = epoch;
    e->my_index        = my_index;
    e->pk_timeout_ms   = 5000;
    e->wait_timeout_ms = 0;  /* 按 index 动态计算 */
    e->candidate_idx   = -1;
    e->opponent_idx    = -1;
    e->waiting_for     = -1;
    e->candidate_val   = -1;
    e->wait_start      = 0;
    strncpy(e->my_id, node_id, sizeof(e->my_id) - 1);
}

void election_start(election_t *e, long now_ms) {
    e->phase         = ELECTION_RUNNING;
    e->epoch++;
    e->candidate_idx = -1;   /* 调用者设置 */
    e->opponent_idx  = -1;
    e->candidate_val = -1;
    e->waiting_for   = -1;
    e->deadline      = now_ms + e->pk_timeout_ms;
}

int election_generate_val(election_t *e) {
    int val = rand() % 11;
    if (e->candidate_idx == e->my_index)
        e->candidate_val = val;
    return val;
}

int election_my_turn(const election_t *e, int my_index) {
    return (e->candidate_idx == my_index && e->opponent_idx >= 0);
}

int election_is_my_battle(const election_t *e, int my_index) {
    return (e->candidate_idx == my_index || e->opponent_idx == my_index);
}

int election_handle_result(election_t *e, long now_ms, int opponent_val,
                           int *winner, int *loser, int *done) {
    (void)now_ms;
    *winner = -1;
    *loser  = -1;
    *done   = 0;

    if (e->candidate_val >= opponent_val) {
        *winner = e->candidate_idx;
        *loser  = e->opponent_idx;
    } else {
        *winner = e->opponent_idx;
        *loser  = e->candidate_idx;
    }
    return 0;
}

int election_advance(election_t *e, long now_ms, int winner_idx) {
    e->candidate_idx = winner_idx;
    e->opponent_idx++;
    e->waiting_for = -1;
    e->deadline    = now_ms + e->pk_timeout_ms;
    return 1;
}

void election_finish(election_t *e, const char *leader_addr) {
    strncpy(e->leader_addr, leader_addr, sizeof(e->leader_addr) - 1);
    e->leader_epoch = e->epoch;
    e->phase        = ELECTION_DONE;
}

int election_check_timeout(const election_t *e, long now_ms) {
    if (e->phase != ELECTION_RUNNING)
        return 0;
    return (now_ms >= e->deadline) ? 1 : 0;
}

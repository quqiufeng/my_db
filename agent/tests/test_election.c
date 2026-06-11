#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/election.h"

static int tests_pass = 0, tests_fail = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { fprintf(stderr, "FAIL: %s (%s)\n", name, #expr); tests_fail++; } \
    else { tests_pass++; } \
} while(0)

int main(void) {
    srand(time(NULL));
    election_t e;

    /* init */
    election_init(&e, "test_node", 0, 0);
    TEST("init role", e.role == ELECTION_FOLLOWER);
    TEST("init term", e.term == 0);
    TEST("init voted_for", e.voted_for == -1);

    /* become candidate */
    election_become_candidate(&e, 1000, 3);
    TEST("candidate role", e.role == ELECTION_CANDIDATE);
    TEST("candidate term", e.term == 1);
    TEST("candidate voted_for", e.voted_for == 0);
    TEST("candidate votes", e.votes_received == 1);
    TEST("candidate total", e.total_voters == 3);

    /* majority check */
    TEST("majority 0/3", election_is_majority(0, 3) == 0);
    TEST("majority 1/3", election_is_majority(1, 3) == 0);
    TEST("majority 2/3", election_is_majority(2, 3) == 1);
    TEST("majority 3/3", election_is_majority(3, 3) == 1);
    TEST("majority 2/5", election_is_majority(2, 5) == 0);
    TEST("majority 3/5", election_is_majority(3, 5) == 1);

    /* become leader */
    election_become_leader(&e);
    TEST("leader role", e.role == ELECTION_LEADER);
    TEST("leader term", e.leader_term == 1);
    TEST("leader voted_for", e.voted_for == -1);

    /* become follower (higher term) */
    election_become_follower(&e, 5, 5000);
    TEST("follower role", e.role == ELECTION_FOLLOWER);
    TEST("follower term", e.term == 5);
    TEST("follower voted_for", e.voted_for == -1);

    /* election timeout */
    TEST("no timeout yet", election_check_timeout(&e, 4999) == 0);
    /* deadline was set to 5000 + random(1500-3000) = 6500-8000 */
    /* so at 6500 it should fire */
    TEST("timeout at 8001", election_check_timeout(&e, 8001) == 1);
    

    /* reset timer */
    election_reset_timer(&e, 10000);
    TEST("timer reset", e.election_deadline >= 11500 && e.election_deadline <= 13000);

    printf("Results: %d passed, %d failed\n", tests_pass, tests_fail);
    return tests_fail > 0 ? 1 : 0;
}

#define _GNU_SOURCE
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct session {
    cache_t*    cache;
    char*       session_id;
    char*       project_ns;
    char*       model;
    char*       ns;
    int         next_turn;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

session_t* session_create(cache_t* cache, const char* session_id,
                          const char* project_ns, const char* model) {
    if (!cache || !session_id || !project_ns) return NULL;

    session_t* s = calloc(1, sizeof(session_t));
    if (!s) return NULL;

    s->cache = cache;
    s->session_id = strdup(session_id);
    s->project_ns = strdup(project_ns);
    s->model = model ? strdup(model) : strdup("default");

    size_t len = strlen(session_id) + 16;
    s->ns = malloc(len);
    if (!s->ns) {
        free(s);
        return NULL;
    }
    snprintf(s->ns, len, "/session/%s/", session_id);
    s->next_turn = 1;

    session_set_meta(s);
    return s;
}

void session_free(session_t* s) {
    if (!s) return;
    free(s->session_id);
    free(s->project_ns);
    free(s->model);
    free(s->ns);
    free(s);
}

const char* session_id(const session_t* s)    { return s ? s->session_id : NULL; }
const char* session_project(const session_t* s){ return s ? s->project_ns : NULL; }
const char* session_model(const session_t* s) { return s ? s->model : NULL; }
const char* session_ns(const session_t* s)    { return s ? s->ns : NULL; }

char* session_key(session_t* s, const char* subkey) {
    if (!s || !subkey) return NULL;
    size_t len = strlen(s->ns) + strlen(subkey) + 1;
    char* key = malloc(len);
    if (!key) return NULL;
    snprintf(key, len, "%s%s", s->ns, subkey);
    return key;
}

int session_set(session_t* s, const char* subkey, const char* value, uint64_t ttl_ms) {
    char* key = session_key(s, subkey);
    if (!key) return -1;
    int rc = cache_set(s->cache, key, value, ttl_ms);
    free(key);
    return rc;
}

const char* session_get(session_t* s, const char* subkey) {
    char* key = session_key(s, subkey);
    if (!key) return NULL;
    const char* val = cache_get(s->cache, key);
    free(key);
    return val;
}

int session_del(session_t* s, const char* subkey) {
    char* key = session_key(s, subkey);
    if (!key) return -1;
    int rc = cache_del(s->cache, key);
    free(key);
    return rc;
}

int session_set_meta(session_t* s) {
    if (!s) return -1;
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"created_at\":%llu,\"updated_at\":%llu,\"project\":\"%s\",\"model\":\"%s\"}",
        (unsigned long long)now_ms(),
        (unsigned long long)now_ms(),
        s->project_ns,
        s->model);
    return session_set(s, "meta", buf, 0);
}

int session_set_task(session_t* s, const char* task) {
    if (!s || !task) return -1;
    char buf[4096];
    snprintf(buf, sizeof(buf),
        "{\"t\":\"task\",\"c\":\"%s\",\"i\":5,\"ts\":%llu}",
        task, (unsigned long long)now_ms());
    return session_set(s, "task/current", buf, 0);
}

int session_start_turn(session_t* s, int turn_id, const char* role, const char* content) {
    if (!s || !role || !content) return -1;
    char subkey[64];
    snprintf(subkey, sizeof(subkey), "turns/%08d", turn_id);
    /* Very basic JSON escaping not handled here; Lua layer should pass already-safe content */
    char buf[8192];
    snprintf(buf, sizeof(buf),
        "{\"t\":\"turn\",\"role\":\"%s\",\"c\":\"%s\",\"ts\":%llu}",
        role, content, (unsigned long long)now_ms());
    return session_set(s, subkey, buf, 24 * 3600 * 1000);
}

int session_next_turn_id(session_t* s) {
    if (!s) return -1;
    return s->next_turn++;
}

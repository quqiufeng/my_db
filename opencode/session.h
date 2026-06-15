#ifndef OPCODE_SESSION_H
#define OPCODE_SESSION_H

#include <stddef.h>
#include "cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Session handle (opaque) */
typedef struct session session_t;

/* Create/destroy */
session_t* session_create(cache_t* cache, const char* session_id,
                          const char* project_ns, const char* model);
void       session_free(session_t* s);

const char* session_id(const session_t* s);
const char* session_project(const session_t* s);
const char* session_model(const session_t* s);
const char* session_ns(const session_t* s);      /* /session/{id}/ */

/* Raw KV access under session namespace */
int         session_set(session_t* s, const char* subkey, const char* value, uint64_t ttl_ms);
const char* session_get(session_t* s, const char* subkey);  /* pointer into cache, do not free */
int         session_del(session_t* s, const char* subkey);

/* Helpers for common session data */
int session_set_meta(session_t* s);
int session_set_task(session_t* s, const char* task);
int session_start_turn(session_t* s, int turn_id, const char* role, const char* content);
int session_next_turn_id(session_t* s);

/* Build absolute key from subkey (caller frees) */
char* session_key(session_t* s, const char* subkey);

#ifdef __cplusplus
}
#endif

#endif /* OPCODE_SESSION_H */

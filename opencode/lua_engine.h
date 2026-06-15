#ifndef OPCODE_LUA_ENGINE_H
#define OPCODE_LUA_ENGINE_H

#include <lua.h>
#include "cache.h"

#include "llm_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LuaJIT engine handle (opaque) */
typedef struct lua_engine lua_engine_t;

/* Create/destroy. Optionally pass an LLM config; set to NULL to skip LLM setup. */
lua_engine_t* lua_engine_create(cache_t* cache, const llm_config_t* llm_config);
void          lua_engine_free(lua_engine_t* e);

/* Access underlying Lua state for custom registration */
lua_State* lua_engine_state(lua_engine_t* e);

/* Load and run a Lua file */
int lua_engine_dofile(lua_engine_t* e, const char* path);

/* Run a Lua string */
int lua_engine_dostring(lua_engine_t* e, const char* code);

/* Convenience: call a global Lua function that returns a string.
 * Returns malloc'd string (caller frees), or NULL on error. */
char* lua_engine_call_s(lua_engine_t* e, const char* func,
                        const char** args, size_t nargs);

#ifdef __cplusplus
}
#endif

#endif /* OPCODE_LUA_ENGINE_H */

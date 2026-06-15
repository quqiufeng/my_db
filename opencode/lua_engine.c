#define _GNU_SOURCE
#include "lua_engine.h"

#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lua_engine {
    lua_State*     L;
    cache_t*       cache;
    llm_client_t*  llm;
};

/* ---------- helpers ---------- */

static cache_t* get_cache(lua_State* L) {
    lua_getglobal(L, "__cache");
    cache_t* c = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return c;
}

static llm_client_t* get_llm(lua_State* L) {
    lua_getglobal(L, "__llm");
    llm_client_t* c = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return c;
}

static int l_cache_get(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    cache_t* c = get_cache(L);
    const char* val = cache_get(c, key);
    if (val) lua_pushstring(L, val);
    else     lua_pushnil(L);
    return 1;
}

static int l_cache_set(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    const char* value = luaL_checkstring(L, 2);
    uint64_t ttl_ms = luaL_optinteger(L, 3, 0);
    cache_t* c = get_cache(L);
    int rc = cache_set(c, key, value, ttl_ms);
    lua_pushboolean(L, rc == CACHE_OK);
    return 1;
}

static void push_result_table(lua_State* L, cache_result_t* results, size_t count) {
    lua_createtable(L, (int)count, 0);
    for (size_t i = 0; i < count; i++) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, results[i].key);
        lua_setfield(L, -2, "key");
        lua_pushstring(L, results[i].value);
        lua_setfield(L, -2, "value");
        lua_pushnumber(L, results[i].score);
        lua_setfield(L, -2, "score");
        lua_rawseti(L, -2, (int)(i + 1));
    }
}

static int l_cache_search_prefix(lua_State* L) {
    const char* prefix = luaL_checkstring(L, 1);
    int max_results = luaL_optinteger(L, 2, 100);
    cache_t* c = get_cache(L);

    cache_search_options_t opts = cache_search_options_default();
    opts.max_results = max_results;

    cache_result_t* results = NULL;
    size_t count = 0;
    if (cache_search_prefix(c, prefix, &opts, &results, &count) != CACHE_OK) {
        lua_createtable(L, 0, 0);
        return 1;
    }
    push_result_table(L, results, count);
    cache_results_free(results);
    return 1;
}

static int l_cache_search_tag(lua_State* L) {
    const char* tag = luaL_checkstring(L, 1);
    int max_results = luaL_optinteger(L, 2, 100);
    cache_t* c = get_cache(L);

    cache_search_options_t opts = cache_search_options_default();
    opts.max_results = max_results;

    cache_result_t* results = NULL;
    size_t count = 0;
    if (cache_search_tag(c, tag, &opts, &results, &count) != CACHE_OK) {
        lua_createtable(L, 0, 0);
        return 1;
    }
    push_result_table(L, results, count);
    cache_results_free(results);
    return 1;
}

static int l_source_read(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int line_start = luaL_optinteger(L, 2, 1);
    int line_end = luaL_optinteger(L, 3, 0);

    FILE* f = fopen(path, "r");
    if (!f) {
        lua_pushnil(L);
        lua_pushstring(L, "file not found");
        return 2;
    }

    char* buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    char line[4096];
    int ln = 0;

    while (fgets(line, sizeof(line), f)) {
        ln++;
        if (ln < line_start) continue;
        if (line_end > 0 && ln > line_end) break;

        size_t llen = strlen(line);
        if (len + llen + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            while (len + llen + 1 > cap) cap *= 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, line, llen);
        len += llen;
        buf[len] = '\0';
    }
    fclose(f);

    if (buf) {
        lua_pushlstring(L, buf, len);
        free(buf);
        return 1;
    }
    lua_pushstring(L, "");
    return 1;
}

static int l_log_info(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    fprintf(stderr, "[LUA] %s\n", msg);
    return 0;
}

static int l_llm_complete(lua_State* L) {
    llm_client_t* llm = get_llm(L);
    if (!llm) {
        lua_pushnil(L);
        lua_pushstring(L, "llm not configured");
        return 2;
    }
    const char* system = luaL_checkstring(L, 1);
    const char* prompt = luaL_checkstring(L, 2);
    const char* tools = lua_isstring(L, 3) ? lua_tostring(L, 3) : NULL;

    char* resp = llm_complete(llm, system, prompt, tools);
    if (resp) {
        lua_pushstring(L, resp);
        free(resp);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "llm request failed");
    return 2;
}

static int l_llm_protocol(lua_State* L) {
    llm_client_t* llm = get_llm(L);
    int p = llm ? llm_client_protocol(llm) : LLM_PROTOCOL_OPENAI;
    lua_pushstring(L, p == LLM_PROTOCOL_ANTHROPIC ? "anthropic" : "openai");
    return 1;
}

static int l_llm_complete_messages(lua_State* L) {
    llm_client_t* llm = get_llm(L);
    if (!llm) {
        lua_pushnil(L);
        lua_pushstring(L, "llm not configured");
        return 2;
    }
    const char* system = luaL_checkstring(L, 1);
    const char* messages = luaL_checkstring(L, 2);
    const char* tools = lua_isstring(L, 3) ? lua_tostring(L, 3) : NULL;

    char* resp = llm_complete_messages(llm, system, messages, tools);
    if (resp) {
        lua_pushstring(L, resp);
        free(resp);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "llm request failed");
    return 2;
}

static int l_llm_complete_raw(lua_State* L) {
    llm_client_t* llm = get_llm(L);
    if (!llm) {
        lua_pushnil(L);
        lua_pushstring(L, "llm not configured");
        return 2;
    }
    const char* body = luaL_checkstring(L, 1);
    char* resp = llm_complete_raw(llm, body);
    if (resp) {
        lua_pushstring(L, resp);
        free(resp);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "llm request failed");
    return 2;
}

static int l_get_model(lua_State* L) {
    llm_client_t* llm = get_llm(L);
    if (llm && llm_client_model(llm)) {
        lua_pushstring(L, llm_client_model(llm));
    } else {
        lua_pushstring(L, "unknown");
    }
    return 1;
}

static const luaL_Reg opencode_lib[] = {
    {"cache_get",          l_cache_get},
    {"cache_set",          l_cache_set},
    {"cache_search_prefix", l_cache_search_prefix},
    {"cache_search_tag",   l_cache_search_tag},
    {"source_read",        l_source_read},
    {"log_info",           l_log_info},
    {"llm_complete",       l_llm_complete},
    {"llm_complete_messages", l_llm_complete_messages},
    {"llm_complete_raw",   l_llm_complete_raw},
    {"llm_protocol",       l_llm_protocol},
    {"get_model",          l_get_model},
    {NULL, NULL}
};

/* ---------- lifecycle ---------- */

lua_engine_t* lua_engine_create(cache_t* cache, const llm_config_t* llm_config) {
    lua_State* L = luaL_newstate();
    if (!L) return NULL;
    luaL_openlibs(L);

    lua_engine_t* e = malloc(sizeof(lua_engine_t));
    if (!e) {
        lua_close(L);
        return NULL;
    }
    e->L = L;
    e->cache = cache;
    e->llm = llm_config ? llm_client_create(llm_config) : NULL;

    /* Store cache/llm pointers as hidden globals for bindings */
    lua_pushlightuserdata(L, cache);
    lua_setglobal(L, "__cache");
    lua_pushlightuserdata(L, e->llm);
    lua_setglobal(L, "__llm");

    /* Register opencode C bindings */
    luaL_newlib(L, opencode_lib);
    lua_setglobal(L, "opencode");

    /* Set up package.cpath so require("cjson") finds /usr/local/lualib/cjson.so */
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "cpath");
    const char* old_cpath = lua_tostring(L, -1);
    char cpath[2048];
    if (old_cpath && *old_cpath) {
        snprintf(cpath, sizeof(cpath), "/usr/local/lualib/?.so;%s", old_cpath);
    } else {
        snprintf(cpath, sizeof(cpath), "/usr/local/lualib/?.so");
    }
    lua_pushstring(L, cpath);
    lua_setfield(L, -3, "cpath");
    lua_pop(L, 2);

    return e;
}

void lua_engine_free(lua_engine_t* e) {
    if (!e) return;
    if (e->L) lua_close(e->L);
    if (e->llm) llm_client_free(e->llm);
    free(e);
}

lua_State* lua_engine_state(lua_engine_t* e) {
    return e ? e->L : NULL;
}

int lua_engine_dofile(lua_engine_t* e, const char* path) {
    if (!e || !path) return -1;
    return luaL_dofile(e->L, path);
}

int lua_engine_dostring(lua_engine_t* e, const char* code) {
    if (!e || !code) return -1;
    return luaL_dostring(e->L, code);
}

char* lua_engine_call_s(lua_engine_t* e, const char* func,
                        const char** args, size_t nargs) {
    if (!e || !func) return NULL;
    lua_State* L = e->L;

    lua_getglobal(L, func);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return NULL;
    }
    for (size_t i = 0; i < nargs; i++) {
        lua_pushstring(L, args[i]);
    }
    if (lua_pcall(L, (int)nargs, 1, 0) != LUA_OK) {
        fprintf(stderr, "lua call error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return NULL;
    }
    const char* s = lua_tostring(L, -1);
    char* out = s ? strdup(s) : NULL;
    lua_pop(L, 1);
    return out;
}

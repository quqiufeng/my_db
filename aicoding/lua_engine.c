#define _GNU_SOURCE
#include "lua_engine.h"

#include "http_async.h"
#include "llm_client.h"
#include "session.h"
#include "vector_engine.h"
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

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

static int l_vector_search(lua_State* L) {
    const char* cache_dir = luaL_checkstring(L, 1);
    const char* query = luaL_checkstring(L, 2);
    const char* ns = luaL_optstring(L, 3, NULL);
    int top_k = luaL_optinteger(L, 4, 10);
    if (top_k <= 0 || top_k > 100) top_k = 10;

    vector_engine_t* engine = vector_engine_open(cache_dir, "jina");
    if (!engine) {
        lua_pushnil(L);
        lua_pushstring(L, vector_engine_error());
        return 2;
    }

    vector_result_t* results = calloc(top_k, sizeof(vector_result_t));
    if (!results) {
        vector_engine_close(engine);
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    int n = ns
        ? vector_engine_search_ns(engine, query, top_k, ns, results)
        : vector_engine_search(engine, query, top_k, results);

    if (n < 0) {
        free(results);
        vector_engine_close(engine);
        lua_pushnil(L);
        lua_pushstring(L, vector_engine_error());
        return 2;
    }

    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_createtable(L, 0, 9);
        lua_pushstring(L, results[i].namespace);
        lua_setfield(L, -2, "namespace");
        lua_pushstring(L, results[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, results[i].file);
        lua_setfield(L, -2, "file");
        lua_pushstring(L, results[i].kind);
        lua_setfield(L, -2, "kind");
        lua_pushstring(L, results[i].language);
        lua_setfield(L, -2, "language");
        lua_pushstring(L, results[i].signature);
        lua_setfield(L, -2, "signature");
        lua_pushstring(L, results[i].content);
        lua_setfield(L, -2, "content");
        lua_pushinteger(L, results[i].line_start);
        lua_setfield(L, -2, "line_start");
        lua_pushinteger(L, results[i].line_end);
        lua_setfield(L, -2, "line_end");
        lua_pushnumber(L, results[i].score);
        lua_setfield(L, -2, "score");
        lua_rawseti(L, -2, i + 1);
    }

    free(results);
    vector_engine_close(engine);
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

static int log_level_from_env(void) {
    static int level = -1;
    if (level >= 0) return level;
    const char* env = getenv("OPENCODE_LOG_LEVEL");
    if (!env) {
        level = 2; /* default: info */
    } else if (strcasecmp(env, "debug") == 0) {
        level = 1;
    } else if (strcasecmp(env, "info") == 0) {
        level = 2;
    } else if (strcasecmp(env, "warn") == 0) {
        level = 3;
    } else if (strcasecmp(env, "error") == 0) {
        level = 4;
    } else if (strcasecmp(env, "none") == 0 || strcasecmp(env, "off") == 0) {
        level = 5;
    } else {
        level = 2;
    }
    return level;
}

static void log_at_level(const char* level_tag, const char* msg) {
    fprintf(stderr, "[LUA] [%s] %s\n", level_tag, msg);
}

static int l_log_debug(lua_State* L) {
    if (log_level_from_env() <= 1) {
        log_at_level("DEBUG", luaL_checkstring(L, 1));
    }
    return 0;
}

static int l_log_warn(lua_State* L) {
    if (log_level_from_env() <= 3) {
        log_at_level("WARN", luaL_checkstring(L, 1));
    }
    return 0;
}

static int l_log_error(lua_State* L) {
    if (log_level_from_env() <= 4) {
        log_at_level("ERROR", luaL_checkstring(L, 1));
    }
    return 0;
}

static int l_set_clipboard(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    const char* cmd = NULL;
    if (access("/usr/bin/wl-copy", X_OK) == 0 || access("/usr/local/bin/wl-copy", X_OK) == 0) {
        cmd = "wl-copy";
    } else if (access("/usr/bin/xclip", X_OK) == 0 || access("/usr/local/bin/xclip", X_OK) == 0) {
        cmd = "xclip -selection clipboard";
    } else if (access("/usr/bin/xsel", X_OK) == 0 || access("/usr/local/bin/xsel", X_OK) == 0) {
        cmd = "xsel --clipboard --input";
    }
    if (!cmd) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no clipboard utility found");
        return 2;
    }
    FILE* pipe = popen(cmd, "w");
    if (!pipe) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "failed to open clipboard pipe");
        return 2;
    }
    fputs(text, pipe);
    int rc = pclose(pipe);
    if (rc != 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "clipboard command failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
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

static int l_http_request(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    const char* method = luaL_checkstring(L, 2);
    const char* headers = luaL_optstring(L, 3, NULL);
    const char* body = luaL_optstring(L, 4, NULL);
    http_async_req_t* req = http_async_request(url, method, headers, body);
    if (!req) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlightuserdata(L, req);
    return 1;
}

static int l_http_poll(lua_State* L) {
    http_async_req_t* req = lua_touserdata(L, 1);
    int s = http_async_poll(req);
    lua_pushinteger(L, s);
    return 1;
}

static int l_http_response(lua_State* L) {
    http_async_req_t* req = lua_touserdata(L, 1);
    const char* r = http_async_response(req);
    if (r) lua_pushstring(L, r);
    else lua_pushnil(L);
    return 1;
}

static int l_http_free(lua_State* L) {
    http_async_req_t* req = lua_touserdata(L, 1);
    http_async_free(req);
    return 0;
}

static int l_get_lua_state(lua_State* L) {
    lua_pushlightuserdata(L, L);
    return 1;
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

/* Streaming LLM call: llm_complete_raw_stream(body_json, callback).
 * callback(delta_text) is invoked for each SSE text chunk. Returns
 * true on success, false + error on failure. */
static int g_stream_ref = LUA_NOREF;

static int stream_lua_cb(const char* delta, void* userdata) {
    lua_State* L = (lua_State*)userdata;
    if (g_stream_ref == LUA_NOREF) return 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_stream_ref);
    lua_pushstring(L, delta);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        fprintf(stderr, "[lua] stream callback error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return 0;
}

static int l_llm_complete_raw_stream(lua_State* L) {
    llm_client_t* llm = get_llm(L);
    if (!llm) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "llm not configured");
        return 2;
    }
    const char* body = luaL_checkstring(L, 1);
    if (!lua_isfunction(L, 2)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "callback required");
        return 2;
    }
    g_stream_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int rc = llm_complete_raw_stream(llm, body, stream_lua_cb, L);
    luaL_unref(L, LUA_REGISTRYINDEX, g_stream_ref);
    g_stream_ref = LUA_NOREF;
    if (rc == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    lua_pushstring(L, "llm request failed");
    return 2;
}

static int l_get_model(lua_State* L) {    llm_client_t* llm = get_llm(L);
    if (llm && llm_client_model(llm)) {
        lua_pushstring(L, llm_client_model(llm));
    } else {
        lua_pushstring(L, "unknown");
    }
    return 1;
}

// ACP send: write a JSON-RPC notification to stdout (for --acp mode)
static int l_acp_send(lua_State* L) {
    const char* method = luaL_checkstring(L, 1);
    const char* params_json = luaL_checkstring(L, 2);
    fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}\n", method, params_json);
    fflush(stdout);
    return 0;
}

// ACP request: write a JSON-RPC request to stdout (engine-initiated), then
// block reading the matching response line from stdin. Returns the raw
// response line to Lua (or nil on EOF). Engine requests use a separate id
// range (>= 10000) so they never collide with client-initiated request ids.
static int l_acp_request(lua_State* L) {
    const char* method = luaL_checkstring(L, 1);
    const char* params_json = luaL_checkstring(L, 2);
    static int engine_req_id = 10000;
    int id = ++engine_req_id;
    fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}\n",
            id, method, params_json);
    fflush(stdout);
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "\"id\":%d", id);
    char line[65536];
    while (fgets(line, sizeof(line), stdin)) {
        if (strstr(line, id_str)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) { line[--n] = '\0'; }
            lua_pushlstring(L, line, n);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

static int l_set_llm(lua_State* L);

static const luaL_Reg opencode_lib[] = {
    {"cache_get",          l_cache_get},
    {"cache_set",          l_cache_set},
    {"cache_search_prefix", l_cache_search_prefix},
    {"cache_search_tag",   l_cache_search_tag},
    {"vector_search",      l_vector_search},
    {"source_read",        l_source_read},
    {"log_info",           l_log_info},
    {"log_debug",          l_log_debug},
    {"log_warn",           l_log_warn},
    {"log_error",          l_log_error},
    {"set_clipboard",      l_set_clipboard},
    {"llm_complete",       l_llm_complete},
    {"llm_complete_messages", l_llm_complete_messages},
    {"llm_complete_raw",   l_llm_complete_raw},
    {"llm_complete_raw_stream", l_llm_complete_raw_stream},
    {"llm_protocol",       l_llm_protocol},
    {"get_model",          l_get_model},
    {"http_request",        l_http_request},
    {"http_poll",           l_http_poll},
    {"http_response",       l_http_response},
    {"http_free",           l_http_free},
    {"get_lua_state",       l_get_lua_state},
    {"acp_send",            l_acp_send},
    {"acp_request",         l_acp_request},
    {"set_llm",             l_set_llm},
    {NULL, NULL}
};

/* ---------- lifecycle ---------- */

// Rebuild the LLM client from the current environment (e.g. after
// select_model() switched models at runtime). Returns boolean success.
static int l_set_llm(lua_State* L) {
    llm_config_t cfg;
    llm_config_from_env(&cfg);
    if (!cfg.base_url[0] || !cfg.api_key[0] || !cfg.model[0]) {
        llm_config_free_fields(&cfg);
        lua_pushboolean(L, 0);
        return 1;
    }
    llm_client_t* llm = llm_client_create(&cfg);
    llm_config_free_fields(&cfg);
    if (!llm) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_getglobal(L, "__engine");
    lua_engine_t* e = lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (e) {
        lua_engine_set_llm(e, llm);
    } else {
        llm_client_free(llm);
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

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
    lua_pushlightuserdata(L, e);
    lua_setglobal(L, "__engine");
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

void lua_engine_set_llm(lua_engine_t* e, llm_client_t* llm) {
    if (!e) return;
    if (e->llm) llm_client_free(e->llm);
    e->llm = llm;
    lua_pushlightuserdata(e->L, llm);
    lua_setglobal(e->L, "__llm");
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

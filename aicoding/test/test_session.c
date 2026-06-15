#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cache.h"
#include "session.h"
#include "lua_engine.h"

int main(int argc, char** argv) {
    const char* db_dir = "/tmp/opencode_test_cache";
    if (argc > 1) db_dir = argv[1];

    cache_t* cache = cache_open(db_dir, 50 * 1024 * 1024);
    if (!cache) {
        fprintf(stderr, "failed to open cache at %s\n", db_dir);
        return 1;
    }

    session_t* s = session_create(cache, "test_session", "/code/mydb", "gpt-4o-mini");
    if (!s) {
        fprintf(stderr, "failed to create session\n");
        cache_close(cache);
        return 1;
    }

    session_set_task(s, "Add TypeScript plugin support to code_indexer");

    llm_config_t llm_cfg = {0};
    const char* env_url = getenv("OPENAI_BASE_URL");
    const char* env_key = getenv("OPENAI_API_KEY");
    const char* env_model = getenv("OPENAI_MODEL");
    if (env_url && env_key && env_model) {
        llm_cfg.base_url = (char*)env_url;
        llm_cfg.api_key = (char*)env_key;
        llm_cfg.model = (char*)env_model;
        llm_cfg.temperature = 0.7;
    }

    lua_engine_t* L = lua_engine_create(cache, (llm_cfg.base_url ? &llm_cfg : NULL));
    if (!L) {
        fprintf(stderr, "failed to create lua engine\n");
        session_free(s);
        cache_close(cache);
        return 1;
    }

    /* Set Lua package.path so require() finds prompts/default and tools/default */
    const char* base = "/opt/my_db/aicoding";
    if (argc > 2) base = argv[2];
    char path_cmd[2048];
    snprintf(path_cmd, sizeof(path_cmd),
        "package.path = '%s/?.lua;%s/?/init.lua;' .. package.path",
        base, base);
    if (lua_engine_dostring(L, path_cmd) != 0) {
        fprintf(stderr, "failed to set package.path\n");
        lua_engine_free(L);
        session_free(s);
        cache_close(cache);
        return 1;
    }

    if (lua_engine_dofile(L, "/opt/my_db/aicoding/main.lua") != 0) {
        fprintf(stderr, "failed to load main.lua\n");
        lua_engine_free(L);
        session_free(s);
        cache_close(cache);
        return 1;
    }

    const char* args[] = {
        session_id(s),
        session_project(s),
        "How do I register a new plugin in code_indexer?"
    };
    char* prompt = lua_engine_call_s(L, "build_prompt", args, 3);
    if (prompt) {
        printf("=== PROMPT ===\n%s\n", prompt);
        free(prompt);
    } else {
        fprintf(stderr, "build_prompt returned nil\n");
    }

    lua_engine_free(L);
    session_free(s);
    cache_close(cache);
    return 0;
}

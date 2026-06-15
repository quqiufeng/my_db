#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cache.h"
#include "session.h"
#include "lua_engine.h"
#include "llm_client.h"

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '\0') return s;
    char* e = s + strlen(s) - 1;
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r')) e--;
    e[1] = '\0';
    return s;
}

static int load_env_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char* p = trim(line);
        if (*p == '#' || *p == '\0') continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = trim(p);
        char* val = trim(eq + 1);
        if (strlen(key) > 0) setenv(key, val, 1);
    }
    fclose(f);
    return 0;
}

static void configure_llm(llm_config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    const char* proto = getenv("LLM_PROTOCOL");
    cfg->protocol = (proto && strcasecmp(proto, "anthropic") == 0) ? LLM_PROTOCOL_ANTHROPIC : LLM_PROTOCOL_OPENAI;

    if (cfg->protocol == LLM_PROTOCOL_ANTHROPIC) {
        cfg->base_url = getenv("ANTHROPIC_BASE_URL");
        cfg->api_key = getenv("ANTHROPIC_API_KEY");
        cfg->model = getenv("ANTHROPIC_MODEL");
    } else {
        cfg->base_url = getenv("OPENAI_BASE_URL");
        cfg->api_key = getenv("OPENAI_API_KEY");
        cfg->model = getenv("OPENAI_MODEL");
    }
    cfg->user_agent = getenv("LLM_USER_AGENT");
    cfg->extra_header = getenv("LLM_EXTRA_HEADER");
    const char* temp = getenv("LLM_TEMPERATURE");
    cfg->temperature = temp ? atof(temp) : 0.7;
    cfg->max_tokens = 4096;
}

static void print_help(void) {
    printf("opencode CLI\n");
    printf("Usage: opencode_cli [options]\n");
    printf("  --session ID       Session ID (default: default)\n");
    printf("  --project NS       Project namespace (default: /code/current)\n");
    printf("  --cache DIR        KV Cache directory (default: ./opencode_cache)\n");
    printf("  --env FILE         Load env file (default: ./.env)\n");
    printf("  --yes, --non-interactive  Auto-allow all permission prompts\n");
    printf("  -h, --help         Show this help\n");
}

int main(int argc, char** argv) {
    const char* session_id = "default";
    const char* project_ns = "/code/current";
    const char* cache_dir = "./opencode_cache";
    const char* env_file = "./.env";
    int non_interactive = 0;
    int allow_all = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) session_id = argv[++i];
        else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) project_ns = argv[++i];
        else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) cache_dir = argv[++i];
        else if (strcmp(argv[i], "--env") == 0 && i + 1 < argc) env_file = argv[++i];
        else if (strcmp(argv[i], "--yes") == 0) allow_all = 1;
        else if (strcmp(argv[i], "--non-interactive") == 0) non_interactive = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
    }

    load_env_file(env_file);
    if (non_interactive) setenv("OPENCODE_NON_INTERACTIVE", "1", 1);
    if (allow_all) setenv("OPENCODE_ALLOW_ALL", "1", 1);

    llm_config_t cfg;
    configure_llm(&cfg);

    if (!cfg.base_url || !cfg.api_key || !cfg.model) {
        fprintf(stderr, "LLM not configured. Set OPENAI_BASE_URL, OPENAI_API_KEY, OPENAI_MODEL\n");
        fprintf(stderr, "or ANTHROPIC_BASE_URL, ANTHROPIC_API_KEY, ANTHROPIC_MODEL in %s\n", env_file);
        return 1;
    }

    cache_t* cache = cache_open(cache_dir, 100 * 1024 * 1024);
    if (!cache) {
        fprintf(stderr, "failed to open cache at %s\n", cache_dir);
        return 1;
    }

    session_t* s = session_create(cache, session_id, project_ns, cfg.model);
    if (!s) {
        fprintf(stderr, "failed to create session\n");
        cache_close(cache);
        return 1;
    }

    lua_engine_t* L = lua_engine_create(cache, &cfg);
    if (!L) {
        fprintf(stderr, "failed to create lua engine\n");
        session_free(s);
        cache_close(cache);
        return 1;
    }

    /* Set Lua package.path so require() finds prompts/default and tools/default */
    char path_cmd[2048];
    snprintf(path_cmd, sizeof(path_cmd),
        "package.path = '%s/?.lua;%s/?/init.lua;' .. package.path",
        "/opt/my_db/opencode", "/opt/my_db/opencode");
    if (lua_engine_dostring(L, path_cmd) != 0) {
        fprintf(stderr, "failed to set package.path\n");
        goto cleanup;
    }

    if (lua_engine_dofile(L, "/opt/my_db/opencode/main.lua") != 0) {
        fprintf(stderr, "failed to load main.lua\n");
        goto cleanup;
    }

    /* Set project root in Lua */
    char proj_cmd[2048];
    snprintf(proj_cmd, sizeof(proj_cmd),
        "if type(set_project_root) == 'function' then set_project_root('%s') end",
        project_ns);
    lua_engine_dostring(L, proj_cmd);

    printf("opencode CLI ready. Session: %s, Project: %s, Model: %s\n",
           session_id, project_ns, cfg.model);

    int use_gui = getenv("OPENCODE_GUI") != NULL;
    if (use_gui) {
        printf("Starting GUI...\n");
        const char* gui_args[] = { session_id, project_ns };
        char* result = lua_engine_call_s(L, "run_gui", gui_args, 2);
        if (result) {
            printf("GUI exited: %s\n", result);
            free(result);
        } else {
            printf("GUI exited without result\n");
        }
    } else {
        printf("Type your message below. Use /quit to exit, /task <desc> to set task.\n\n");

        char input[4096];
        while (1) {
            printf("> ");
            fflush(stdout);
            if (!fgets(input, sizeof(input), stdin)) break;

            char* line = trim(input);
            if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) break;
            if (strncmp(line, "/task ", 6) == 0) {
                session_set_task(s, trim(line + 6));
                printf("Task updated.\n");
                continue;
            }
            if (*line == '\0') continue;

            int turn_id = session_next_turn_id(s);
            session_start_turn(s, turn_id, "user", line);

            const char* args[] = { session_id, project_ns, line };
            char* response = lua_engine_call_s(L, "chat_once", args, 3);

            if (response) {
                printf("%s\n\n", response);
                session_start_turn(s, turn_id, "assistant", response);
                const char* summary_args[] = { session_id, "summary" };
                lua_engine_call_s(L, "generate_summary", summary_args, 2);
                free(response);
            } else {
                printf("(no response)\n\n");
            }
        }
    }

cleanup:
    lua_engine_free(L);
    session_free(s);
    cache_close(cache);
    return 0;
}

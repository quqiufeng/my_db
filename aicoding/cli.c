#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <time.h>
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

static const char* get_home_dir(void) {
    const char* home = getenv("HOME");
    if (home) return home;
    struct passwd* pw = getpwuid(getuid());
    if (pw) return pw->pw_dir;
    return ".";
}

// Model selection moved to Lua: select_model(name_or_nil) in main.lua

static void ensure_dir(const char* path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static const char* project_basename(const char* project_ns) {
    if (!project_ns || *project_ns == '\0') return "default";
    if (strcmp(project_ns, ".") == 0) return "default";
    const char* last = strrchr(project_ns, '/');
    if (last && last[1] != '\0') return last + 1;
    return project_ns;
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

        /* DeepSeek uses OpenAI-compatible protocol but has its own env vars as aliases. */
        if (!cfg->base_url) cfg->base_url = getenv("DEEPSEEK_BASE_URL");
        if (!cfg->api_key) cfg->api_key = getenv("DEEPSEEK_API_KEY");
        if (!cfg->model) cfg->model = getenv("DEEPSEEK_MODEL");
    }
    cfg->user_agent = getenv("LLM_USER_AGENT");
    cfg->extra_header = getenv("LLM_EXTRA_HEADER");
    const char* temp = getenv("LLM_TEMPERATURE");
    cfg->temperature = temp ? atof(temp) : 0.7;
    cfg->max_tokens = 4096;
}

static int is_configured(llm_config_t* cfg) {
    return cfg->base_url && cfg->api_key && cfg->model;
}

static void print_config_help(const char* env_file) {
    fprintf(stderr, "LLM not configured. Set one of the following groups in %s, ~/.aicoding/.env, or environment variables:\n", env_file);
    fprintf(stderr, "  OpenAI-compatible: OPENAI_BASE_URL, OPENAI_API_KEY, OPENAI_MODEL\n");
    fprintf(stderr, "  DeepSeek (OpenAI-compatible): DEEPSEEK_BASE_URL, DEEPSEEK_API_KEY, DEEPSEEK_MODEL\n");
    fprintf(stderr, "  Anthropic: ANTHROPIC_BASE_URL, ANTHROPIC_API_KEY, ANTHROPIC_MODEL\n");
}

static void print_help(void) {
    printf("aicoding CLI\n");
    printf("Usage: aicoding [options]\n");
    printf("  --acp              ACP server mode (JSON-RPC over stdio)\n");
    printf("  --session ID       Session ID (default: default)\n");
    printf("  --project NS       Project directory (default: current dir)\n");
    printf("  --cache DIR        KV Cache directory (default: <project_root>/.opencode)\n");
    printf("  --env FILE         Load env file (default: ./.env, fallback: ~/.aicoding/.env)\n");
    printf("  --model NAME       Use model from models.json (skips interactive picker)\n");
    printf("  --yes, --non-interactive  Auto-allow all permission prompts\n");
    printf("  --test-compress    Run compression test and exit\n");
    printf("\nREPL commands:\n");
    printf("  /task <desc>       Set current task description\n");
    printf("  /lua <code>        Execute Lua in the agent runtime\n");
    printf("  /plan <desc>       Run autonomous plan agent\n");
    printf("  /build [goal]      Run autonomous build-fix agent\n");
    printf("  /quit, /exit       Exit\n");
}

int main(int argc, char** argv) {
    const char* session_id = "default";
    const char* project_ns = ".";
    const char* cache_dir = NULL;
    const char* env_file = "./.env";
    const char* model_name = NULL;
    int non_interactive = 0;
    int allow_all = 0;
    int test_compress = 0;

    int acp_mode = 0;
    cache_t* cache = NULL;
    session_t* s = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--acp") == 0) acp_mode = 1;
        else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) session_id = argv[++i];
        else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) project_ns = argv[++i];
        else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) cache_dir = argv[++i];
        else if (strcmp(argv[i], "--env") == 0 && i + 1 < argc) env_file = argv[++i];
        else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) model_name = argv[++i];
        else if (strcmp(argv[i], "--yes") == 0) allow_all = 1;
        else if (strcmp(argv[i], "--test-compress") == 0) {
            test_compress = 1;
            non_interactive = 1;
        }
        else if (strcmp(argv[i], "--non-interactive") == 0) non_interactive = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
    }

    /* Load env files. System-wide /etc/aicoding/.env is loaded first
     * (lowest priority), then global ~/.aicoding/.env, then project
     * ./.env or --env FILE (highest priority). This way all users
     * (including root via sudo) share the same config.
     */
    load_env_file("/etc/aicoding/.env");
    char global_env[1024];
    snprintf(global_env, sizeof(global_env), "%s/.aicoding/.env", get_home_dir());
    load_env_file(global_env);
    load_env_file(env_file);

    if (non_interactive || acp_mode) setenv("OPENCODE_NON_INTERACTIVE", "1", 1);
    if (allow_all) setenv("OPENCODE_ALLOW_ALL", "1", 1);

    /* Compute default cache directory: <project_root>/.opencode/ */
    char default_cache_dir[2048];
    if (!cache_dir) {
        const char* root = project_ns;
        char abs_path[2048];
        if (project_ns[0] == '.' || project_ns[0] != '/') {
            char cwd[2048];
            if (getcwd(cwd, sizeof(cwd))) {
                if (strcmp(project_ns, ".") == 0)
                    snprintf(abs_path, sizeof(abs_path), "%s", cwd);
                else
                    snprintf(abs_path, sizeof(abs_path), "%s/%s", cwd, project_ns);
                root = abs_path;
            }
        }
        snprintf(default_cache_dir, sizeof(default_cache_dir), "%s/.opencode", root);
        ensure_dir(default_cache_dir);
        cache_dir = default_cache_dir;
    }

    cache = cache_open(cache_dir, 100 * 1024 * 1024);
    if (!cache) {
        fprintf(stderr, "failed to open cache at %s\n", cache_dir);
        return 1;
    }

    /* Create Lua engine WITHOUT LLM config — model selection happens in Lua first */
    lua_engine_t* L = lua_engine_create(cache, NULL);
    if (!L) {
        fprintf(stderr, "failed to create lua engine\n");
        cache_close(cache);
        return 1;
    }

    /* Set Lua package.path so require() finds prompts/default and tools/default */
    char path_cmd[2048];
    snprintf(path_cmd, sizeof(path_cmd),
        "package.path = '%s/?.lua;%s/?/init.lua;' .. package.path",
        "/opt/my_db/aicoding", "/opt/my_db/aicoding");
    lua_engine_dostring(L, path_cmd);

    if (lua_engine_dofile(L, "/opt/my_db/aicoding/main.lua") != 0) {
        fprintf(stderr, "failed to load main.lua\n");
        goto cleanup;
    }

    /* Select model via Lua (cjson). Sets env vars like LLM_PROTOCOL, OPENAI_MODEL. */
    const char* lua_args[] = { model_name ? model_name : "" };
    char* selected = lua_engine_call_s(L, "select_model", lua_args, 1);
    if (selected) {
        fprintf(stderr, "[model] %s\n", selected);
        free(selected);
    }

    /* Now configure LLM from env vars (set by select_model) */
    llm_config_t cfg;
    configure_llm(&cfg);
    if (!is_configured(&cfg)) {
        print_config_help(env_file);
        goto cleanup;
    }

    /* Create LLM client and inject into Lua engine */
    llm_client_t* llm = llm_client_create(&cfg);
    lua_engine_set_llm(L, llm);

    s = session_create(cache, session_id, project_ns, cfg.model);
    if (!s) {
        fprintf(stderr, "failed to create session\n");
        goto cleanup;
    }

    if (test_compress) {
        if (lua_engine_dostring(L, "local compress = require('compress'); local ok, info = compress.wrap('test system prompt with context window and KV Cache and tool call', 1); print('compressed:', ok); if info then print('saved:', info.saved_tokens, 'ratio:', info.ratio) end") != 0) {
            fprintf(stderr, "test_compress error: %s\n", lua_tostring(lua_engine_state(L), -1));
        }
        goto cleanup;
    }

    /* Set project root in Lua */
    char proj_cmd[2048];
    snprintf(proj_cmd, sizeof(proj_cmd),
        "if type(set_project_root) == 'function' then set_project_root('%s') end",
        project_ns);
    lua_engine_dostring(L, proj_cmd);

    // ===== ACP mode: JSON-RPC over stdio =====
    if (acp_mode) {
        fprintf(stderr, "[acp] ACP server mode. Session: %s, Project: %s, Model: %s\n",
                session_id, project_ns, cfg.model);
        fprintf(stderr, "[acp] Reading JSON-RPC from stdin...\n");

        char line[65536];
        while (fgets(line, sizeof(line), stdin)) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (line[0] == '\0') continue;

            // All JSON parsing happens in Lua via cjson
            const char* args[] = { line };
            char* resp = lua_engine_call_s(L, "acp_dispatch", args, 1);
            if (resp) {
                printf("%s\n", resp);
                fflush(stdout);
                free(resp);
            }
            // If resp is NULL, it's a notification (session/cancel), no response
        }
        goto cleanup;
    }

    printf("aicoding CLI ready. Session: %s, Project: %s, Model: %s\n",
           session_id, project_ns, cfg.model);

    /* REPL mode (default for interactive runs without --acp) */
    printf("Type your message below. Use /quit to exit, /task <desc> to set task, /plan <desc> to run plan agent, /build [goal] to run build-fix agent.\n\n");

    char input[4096];
    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(input, sizeof(input), stdin)) break;

        char* line = trim(input);
        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) break;

        int turn_id = session_next_turn_id(s);
        session_start_turn(s, turn_id, "user", line);

        if (strncmp(line, "/task ", 6) == 0) {
            session_set_task(s, trim(line + 6));
            printf("Task updated.\n");
            continue;
        }
        if (strncmp(line, "/plan ", 6) == 0) {
            const char* task = trim(line + 6);
            const char* args[] = { session_id, project_ns, task };
            char* response = lua_engine_call_s(L, "run_plan_agent", args, 3);
            if (response) {
                printf("%s\n\n", response);
                session_start_turn(s, turn_id, "assistant", response);
                free(response);
            } else {
                printf("(plan agent failed)\n\n");
            }
            continue;
        }
        if (strncmp(line, "/build", 6) == 0) {
            const char* goal = line + 6;
            while (*goal == ' ') goal++;
            if (*goal == '\0') goal = "make the project compile";
            const char* args[] = { session_id, project_ns, goal };
            char* response = lua_engine_call_s(L, "run_build_agent", args, 3);
            if (response) {
                printf("%s\n\n", response);
                session_start_turn(s, turn_id, "assistant", response);
                free(response);
            } else {
                printf("(build agent failed)\n\n");
            }
            continue;
        }
        if (strncmp(line, "/lua ", 5) == 0) {
            const char* code = line + 5;
            if (lua_engine_dostring(L, code) != 0) {
                printf("Lua error: %s\n", lua_tostring(lua_engine_state(L), -1));
                lua_pop(lua_engine_state(L), 1);
            }
            continue;
        }
        if (*line == '\0') continue;

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

cleanup:
    lua_engine_free(L);  /* frees llm client internally */
    if (s) session_free(s);
    if (cache) cache_close(cache);
    return 0;
}

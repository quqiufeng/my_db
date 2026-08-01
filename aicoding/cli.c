#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
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

/* Run Lua unit tests from tests/unit/test_*.lua via the testkit module.
 * Returns number of failures (exit code). */
static int run_lua_tests(lua_engine_t* L, const char* dir, const char* filter) {
    DIR* d = opendir(dir);
    if (!d) {
        fprintf(stderr, "[test] cannot open test dir %s\n", dir);
        return 2;
    }
    char names[256][2048];
    int n = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) && n < 256) {
        if (strncmp(ent->d_name, "test_", 5) != 0) continue;
        const char* dot = strrchr(ent->d_name, '.');
        if (!dot || strcmp(dot, ".lua") != 0) continue;
        if (filter && filter[0] && strstr(ent->d_name, filter) == NULL) continue;
        snprintf(names[n++], sizeof(names[0]), "%s/%s", dir, ent->d_name);
    }
    closedir(d);

    /* sort names */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(names[j], names[i]) < 0) {
                char tmp[2048];
                strcpy(tmp, names[i]);
                strcpy(names[i], names[j]);
                strcpy(names[j], tmp);
            }
        }
    }

    int total_passed = 0, total_failed = 0;
    for (int i = 0; i < n; i++) {
        const char* base = strrchr(names[i], '/');
        base = base ? base + 1 : names[i];
        fprintf(stderr, "[test] %s ... ", base);
        if (lua_engine_dofile(L, names[i]) != 0) {
            fprintf(stderr, "LOAD ERROR: %s\n", lua_tostring(lua_engine_state(L), -1));
            lua_pop(lua_engine_state(L), 1);
            total_failed++;
            continue;
        }
        lua_engine_dostring(L, "return _testkit.result()");
        lua_State* ls = lua_engine_state(L);
        /* result() returns (passed, failed, failures): pushed in that order,
         * so -3 is passed, -2 is failed, -1 is the failures table. */
        int passed = (int)lua_tonumber(ls, -3);
        int failed = (int)lua_tonumber(ls, -2);
        lua_pop(ls, 3);
        total_passed += passed;
        total_failed += failed;
        fprintf(stderr, "%d passed, %d failed\n", passed, failed);
        lua_engine_dostring(L, "_testkit.reset()");
    }
    fprintf(stderr, "[test] TOTAL: %d passed, %d failed across %d files\n",
            total_passed, total_failed, n);
    return total_failed > 0 ? 1 : 0;
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

static int load_env_file(const char* path, int force) {
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
        if (strlen(key) > 0) setenv(key, val, force);
    }
    fclose(f);
    return 0;
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
    printf("  serve --port N     Standalone TCP bridge (aicoding serve)\n");
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
    const char* test_filter = NULL;
    int test_mode = 0;
    int test_exit_code = 0;
    cache_t* cache = NULL;
    session_t* s = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--acp") == 0) acp_mode = 1;
        else if (strcmp(argv[i], "serve") == 0) {
            /* serve: standalone TCP bridge; delegates to serve_main(). */
            extern int serve_main(int argc, char** argv);
            return serve_main(argc, argv);
        }
        else if (strcmp(argv[i], "--test") == 0) {
            test_mode = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') test_filter = argv[++i];
        }
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
     * (including root via sudo) share the same config. System and global
     * files act as fallbacks only: variables already set in the caller's
     * environment are not overwritten. An explicit --env FILE is loaded
     * last and wins over everything.
     */
    load_env_file("/etc/aicoding/.env", 0);
    char global_env[1024];
    snprintf(global_env, sizeof(global_env), "%s/.aicoding/.env", get_home_dir());
    load_env_file(global_env, 0);
    load_env_file(env_file, 1);

    if (non_interactive) setenv("OPENCODE_NON_INTERACTIVE", "1", 1);
    if (acp_mode) setenv("OPENCODE_ACP", "1", 1);
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
        if (test_mode) {
            /* unit tests must not touch the project cache */
            snprintf(default_cache_dir, sizeof(default_cache_dir), "/tmp/aicoding_test_cache");
            ensure_dir(default_cache_dir);
            cache_dir = default_cache_dir;
        }
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

    /* Unit test mode: run tests/unit/test_*.lua and exit. */
    if (test_mode) {
        /* Isolate tests from the user's real config (permissions read
         * ~/.config/opencode/config.json via $HOME). */
        setenv("HOME", "/tmp/aicoding_test_home", 1);
        /* Like ACP mode: select_model() picks the first model without an
         * interactive picker, while permissions keep their default "ask"
         * semantics (NON_INTERACTIVE would flip the catch-all to deny). */
        setenv("OPENCODE_ACP", "1", 1);
        char test_path[2048];
        snprintf(test_path, sizeof(test_path),
            "package.path = '/opt/my_db/aicoding/tests/unit/?.lua;' .. package.path");
        lua_engine_dostring(L, test_path);
        test_exit_code = run_lua_tests(L, "/opt/my_db/aicoding/tests/unit", test_filter);
        goto cleanup;
    }

    /* Select model via Lua (cjson). Sets env vars like LLM_PROTOCOL, OPENAI_MODEL. */
    const char* lua_args[] = { model_name ? model_name : "" };
    char* selected = lua_engine_call_s(L, "select_model", lua_args, 1);
    if (selected) {
        const char* base = getenv("OPENAI_BASE_URL");
        if (!base) base = getenv("ANTHROPIC_BASE_URL");
        if (base) {
            fprintf(stderr, "[model] %s -> %s\n", selected, base);
        } else {
            fprintf(stderr, "[model] %s\n", selected);
        }
        free(selected);
    }

    /* Now configure LLM from env vars (set by select_model) */
    llm_config_t cfg;
    llm_config_from_env(&cfg);
    if (!is_configured(&cfg)) {
        print_config_help(env_file);
        goto cleanup;
    }

    /* Create LLM client and inject into Lua engine */
    llm_client_t* llm = llm_client_create(&cfg);
    if (!llm) {
        fprintf(stderr, "failed to create llm client\n");
        goto cleanup;
    }
    lua_engine_set_llm(L, llm);

    s = session_create(cache, session_id, project_ns, cfg.model);
    if (!s) {
        fprintf(stderr, "failed to create session\n");
        goto cleanup;
    }
    if (acp_mode) {
        fprintf(stderr, "[acp] ACP server mode. Session: %s, Project: %s, Model: %s\n",
                session_id, project_ns, cfg.model);
    }
    /* cfg fields are only referenced above (create/session_create copy them). */
    llm_config_free_fields(&cfg);

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
    return test_exit_code;
}

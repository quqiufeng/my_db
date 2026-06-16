#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
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

#define MAX_MODELS 32
#define MODEL_NAME_LEN 64
#define MODEL_PROVIDER_LEN 16
#define MODEL_ID_LEN 64

typedef struct {
    char name[MODEL_NAME_LEN];
    char provider[MODEL_PROVIDER_LEN];
    char model[MODEL_ID_LEN];
} model_entry_t;

static int models_count = 0;
static model_entry_t models[MAX_MODELS];

static int parse_json_models(const char* text) {
    models_count = 0;
    if (!text || !*text) return 0;
    const char* p = text;
    while (*p && models_count < MAX_MODELS) {
        p = strchr(p, '{');
        if (!p) break;
        p++;
        char name[MODEL_NAME_LEN] = {0};
        char provider[MODEL_PROVIDER_LEN] = {0};
        char model[MODEL_ID_LEN] = {0};
        while (*p && *p != '}') {
            char key[64] = {0};
            char val[128] = {0};
            while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
            if (*p == '}') break;
            const char* q = p;
            while (*q && *q != '"') q++;
            if (*q != '"') break;
            q++;
            const char* kstart = q;
            while (*q && *q != '"') q++;
            if (*q != '"') break;
            size_t klen = q - kstart;
            if (klen >= sizeof(key)) klen = sizeof(key) - 1;
            strncpy(key, kstart, klen);
            q++;
            while (*q && *q != ':') q++;
            if (*q != ':') break;
            q++;
            while (*q && (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t')) q++;
            if (*q != '"') break;
            q++;
            const char* vstart = q;
            while (*q && *q != '"') q++;
            if (*q != '"') break;
            size_t vlen = q - vstart;
            if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
            strncpy(val, vstart, vlen);
            q++;
            p = q;
            if (strcmp(key, "name") == 0) strncpy(name, val, sizeof(name) - 1);
            else if (strcmp(key, "provider") == 0) strncpy(provider, val, sizeof(provider) - 1);
            else if (strcmp(key, "model") == 0) strncpy(model, val, sizeof(model) - 1);
        }
        if (name[0] && provider[0] && model[0]) {
            strncpy(models[models_count].name, name, sizeof(models[models_count].name) - 1);
            strncpy(models[models_count].provider, provider, sizeof(models[models_count].provider) - 1);
            strncpy(models[models_count].model, model, sizeof(models[models_count].model) - 1);
            models_count++;
        }
        while (*p && *p != '}' && *p != '{') p++;
    }
    return models_count;
}

static int load_models_json(void) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/.aicoding/models.json", get_home_dir());
    FILE* f = fopen(path, "r");
    if (!f) {
        f = fopen("/opt/my_db/aicoding/models.json", "r");
    }
    if (!f) return 0;
    char text[8192];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = '\0';
    fclose(f);
    return parse_json_models(text);
}

static int find_model_index(const char* name) {
    for (int i = 0; i < models_count; i++) {
        if (strcmp(models[i].name, name) == 0) return i;
        if (strcmp(models[i].model, name) == 0) return i;
    }
    return -1;
}

static void apply_model_config(int idx) {
    if (idx < 0 || idx >= models_count) return;
    const model_entry_t* m = &models[idx];

    if (strcmp(m->provider, "anthropic") == 0) {
        setenv("LLM_PROTOCOL", "anthropic", 1);
        /* Use ANTHROPIC_* vars if they exist, otherwise warn. */
        setenv("ANTHROPIC_MODEL", m->model, 1);
    } else if (strcmp(m->provider, "deepseek") == 0) {
        /* DeepSeek uses OpenAI-compatible protocol. Copy DEEPSEEK_* into
         * OPENAI_* so configure_llm picks them up regardless of which
         * env vars happen to be in .env. */
        setenv("LLM_PROTOCOL", "openai", 1);
        const char* ds_url = getenv("DEEPSEEK_BASE_URL");
        if (ds_url) setenv("OPENAI_BASE_URL", ds_url, 1);
        const char* ds_key = getenv("DEEPSEEK_API_KEY");
        if (ds_key) setenv("OPENAI_API_KEY", ds_key, 1);
        setenv("OPENAI_MODEL", m->model, 1);
    } else {
        /* Default OpenAI-compatible (Kimi, etc.) */
        setenv("LLM_PROTOCOL", "openai", 1);
        setenv("OPENAI_MODEL", m->model, 1);
    }
}

static int select_model_interactive(const char* default_model) {
    if (models_count <= 1) return 0;
    if (default_model) {
        int idx = find_model_index(default_model);
        if (idx >= 0) {
            apply_model_config(idx);
            return idx;
        }
        fprintf(stderr, "Model '%s' not found in models.json.\n", default_model);
    }
    fprintf(stderr, "\nAvailable models:\n");
    for (int i = 0; i < models_count; i++) {
        fprintf(stderr, "  %d. %s (%s)\n", i + 1, models[i].name, models[i].provider);
    }
    fprintf(stderr, "Select model (1-%d, or press Enter for %s): ", models_count, models[0].name);
    fflush(stderr);
    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    char* line = trim(buf);
    if (*line == '\0') {
        apply_model_config(0);
        return 0;
    }
    int choice = atoi(line);
    if (choice < 1 || choice > models_count) {
        int idx = find_model_index(line);
        if (idx >= 0) {
            apply_model_config(idx);
            return idx;
        }
        fprintf(stderr, "Invalid selection. Using default model: %s\n", models[0].name);
        apply_model_config(0);
        return 0;
    }
    apply_model_config(choice - 1);
    return choice - 1;
}

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
    printf("  --session ID       Session ID (default: default)\n");
    printf("  --project NS       Project namespace (default: /code/current)\n");
    printf("  --cache DIR        KV Cache directory (default: ~/aicoding/<project_basename>)\n");
    printf("  --env FILE         Load env file (default: ./.env, fallback: ~/.aicoding/.env)\n");
    printf("  --model NAME       Use model from models.json (skips interactive picker)\n");
    printf("  --yes, --non-interactive  Auto-allow all permission prompts\n");
    printf("  --gui-test-script FILE  Load Lua script to drive GUI and exit\n");
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
    const char* project_ns = "/code/current";
    const char* cache_dir = NULL;
    const char* env_file = "./.env";
    const char* model_name = NULL;
    int non_interactive = 0;
    int allow_all = 0;
    int test_compress = 0;
    const char* gui_test_script = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) session_id = argv[++i];
        else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) project_ns = argv[++i];
        else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) cache_dir = argv[++i];
        else if (strcmp(argv[i], "--env") == 0 && i + 1 < argc) env_file = argv[++i];
        else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) model_name = argv[++i];
        else if (strcmp(argv[i], "--yes") == 0) allow_all = 1;
        else if (strcmp(argv[i], "--gui-test-script") == 0 && i + 1 < argc) {
            gui_test_script = argv[++i];
            non_interactive = 1;
        }
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

    /* Load env files. Global ~/.aicoding/.env is always loaded first
     * (lower priority), then project ./.env or --env FILE overwrites
     * (higher priority). This way a project can override specific vars
     * while the global file provides defaults for all providers
     * (e.g. both OPENAI_* and DEEPSEEK_* keys coexist).
     */
    char global_env[1024];
    snprintf(global_env, sizeof(global_env), "%s/.aicoding/.env", get_home_dir());
    load_env_file(global_env);
    load_env_file(env_file);

    /* Load model list and apply selection before configuring LLM. */
    load_models_json();
    if (models_count > 1 && !non_interactive) {
        select_model_interactive(model_name);
    } else if (model_name) {
        int idx = find_model_index(model_name);
        if (idx >= 0) {
            apply_model_config(idx);
        } else {
            fprintf(stderr, "Unknown model: %s\n", model_name);
            return 1;
        }
    } else if (models_count == 1) {
        apply_model_config(0);
    }

    if (non_interactive) setenv("OPENCODE_NON_INTERACTIVE", "1", 1);
    if (allow_all) setenv("OPENCODE_ALLOW_ALL", "1", 1);
    if (gui_test_script) setenv("OPENCODE_GUI_TEST_SCRIPT", gui_test_script, 1);

    llm_config_t cfg;
    configure_llm(&cfg);

    if (!is_configured(&cfg)) {
        print_config_help(env_file);
        return 1;
    }

    /* Compute default cache directory: ~/aicoding/<project_basename> */
    char default_cache_dir[1024];
    if (!cache_dir) {
        const char* home = get_home_dir();
        const char* base = project_basename(project_ns);
        snprintf(default_cache_dir, sizeof(default_cache_dir), "%s/aicoding/%s", home, base);
        ensure_dir(default_cache_dir);
        cache_dir = default_cache_dir;
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
        "/opt/my_db/aicoding", "/opt/my_db/aicoding");
    if (lua_engine_dostring(L, path_cmd) != 0) {
        fprintf(stderr, "failed to set package.path\n");
        goto cleanup;
    }

    if (lua_engine_dofile(L, "/opt/my_db/aicoding/main.lua") != 0) {
        fprintf(stderr, "failed to load main.lua\n");
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

    printf("aicoding CLI ready. Session: %s, Project: %s, Model: %s\n",
           session_id, project_ns, cfg.model);

    /* Default to GUI for interactive runs. Disable via OPENCODE_GUI=0. */
    int use_gui = 1;
    const char* gui_env = getenv("OPENCODE_GUI");
    if (gui_env && strcmp(gui_env, "0") == 0) use_gui = 0;
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
    }

cleanup:
    lua_engine_free(L);
    session_free(s);
    cache_close(cache);
    return 0;
}

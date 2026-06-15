# opencode CLI

A lightweight CLI replica of opencode, built on the my_db KV Cache + C/LuaJIT stack.

## Quick start

```bash
cd /opt/my_db/opencode
make
./opencode_cli --project /path/to/your/repo
```

Configuration is read from `./.env`:

```bash
LLM_PROTOCOL=openai
OPENAI_API_KEY=sk-...
OPENAI_BASE_URL=https://api.kimi.com/coding/v1
OPENAI_MODEL=kimi-latest
LLM_USER_AGENT=opencode/1.17.6 ai-sdk/provider-utils/4.0.27 runtime/bun/1.3.14
LLM_EXTRA_HEADER=x-opencode-version: 1.17.6
LLM_TEMPERATURE=1.0
```

## CLI options

```bash
./opencode_cli --session mysession --project /path/to/repo --cache ./opencode_cache --env ./.env
```

## REPL commands

- Type any coding task or question.
- `/task <description>` — set the current task.
- `/quit` or `/exit` — leave.

## Permissions

The CLI supports opencode-compatible permission rules. Rules are evaluated in order and the **first match wins**, so place the most specific rules before a catch-all default.

### Config locations

1. Project config: `.opencode/config.json` or `.opencode/opencode.jsonc`
2. Global config: `~/.config/opencode/config.json`

### Rule format (v2)

```json
{
  "permissions": [
    { "action": "apply_edit", "resource": "*", "effect": "deny" },
    { "action": "git", "resource": "*", "effect": "allow" },
    { "action": "read", "resource": "*", "effect": "allow" },
    { "action": "*", "resource": "*", "effect": "ask" }
  ]
}
```

- `action`: tool category (`apply_edit`, `file_create`, `file_delete`, `bash`, `git`, `read`, `edit`, `write`, `delete`, `*`)
- `resource`: file path or command pattern; `*` matches anything
- `effect`: `allow`, `deny`, or `ask`

### Legacy format

```json
{
  "permission": {
    "bash": "ask",
    "git status": "allow",
    "edit": "deny"
  }
}
```

### Interactive prompts

When a rule has `effect: "ask"`, the CLI prompts on `/dev/tty` so that `stdin` remains free for the chat stream:

```
Allow apply_edit '/path/to/file'? [y/N/a(ll)/d(eny)]:
```

- `y`/`yes` — allow once
- `a`/`all` — allow for the rest of the session
- `d`/`deny` — deny and stop asking for this action
- anything else — deny once

### Non-interactive modes

| Flag | Behavior |
|------|----------|
| `--yes` | Auto-allow all permission checks (useful in scripts) |
| `--non-interactive` | Treat all `ask` rules as `deny`; no prompts |

## Tools

| Tool | Purpose |
|------|---------|
| `kv_search` | Search indexed code/history in KV Cache |
| `kv_get` | Read an exact KV key |
| `kv_context` | Get symbol context (callers/callees) |
| `source_read` | Read file lines |
| `apply_edit` | Precise text replacement in existing files |
| `file_create` | Create a new file |
| `file_delete` | Delete a file |
| `file_list` | List directory contents |
| `bash` | Run shell commands |
| `git` | Run git commands in the project |
| `diff` | Show git diff |
| `code_index` | Index a repo into KV Cache |

All relative file paths are resolved against the project root passed via `--project`.

## GUI mode (experimental)

A GPU-capable GUI is available via Rust + egui/eframe, exposed as `libopencode_gui.so` and driven from LuaJIT through FFI.

### Build the GUI library

```bash
cd /opt/my_db/opencode/gui
cargo build --release
cp target/release/libopencode_gui.so ../libopencode_gui.so
```

### Run GUI mode

```bash
cd /opt/my_db/opencode
OPENCODE_GUI=1 ./opencode_cli --project /path/to/your/repo
```

### GUI C ABI

Exported by `libopencode_gui.so`:

```c
void* gui_app_create(const char* config_json);
void  gui_app_free(void* app);
void  gui_on_user_message(void* app, void (*cb)(const char* session_id, const char* text, void* userdata), void* userdata);
void  gui_on_tool_call(void* app, char* (*cb)(const char* session_id, const char* tool_json, void* userdata), void* userdata);
int   gui_run(void* app);
void  gui_stream_delta(void* app, const char* session_id, const char* delta);
void  gui_append_message(void* app, const char* session_id, const char* role, const char* text);
void  gui_tool_output(void* app, const char* session_id, const char* tool_id, const char* output);
```

### Lua FFI binding

`gui.lua` wraps the C ABI. Example:

```lua
local gui = require("gui")
local app = gui.create({ title = "opencode" })

gui.on_user_message(app, function(session_id, text)
    gui.append_message(app, session_id, "user", text)
    -- call LLM / tools here
    gui.append_message(app, session_id, "assistant", "reply")
end)

gui.run(app)
gui.free(app)
```

### Notes

- The current GUI uses **egui** because it compiles reliably without system GUI dev packages. The long-term target is **GPUI + gpui-component** for a richer editor/dock experience; the C ABI and Lua FFI layer are designed to survive that migration unchanged.
- GUI mode requires a display server (X11/Wayland on Linux).
- Non-GUI REPL mode is unchanged and remains the default.

## Architecture

- **C kernel**: KV Cache (`libmydb.so`), session namespace, HTTP client (libcurl), LuaJIT host.
- **LuaJIT layer**: prompt assembly, tool definitions, tool dispatch, LLM request body assembly.
- **JSON**: handled by `/usr/local/lualib/cjson.so` inside LuaJIT.

## Key files

| File | Purpose |
|------|---------|
| `cli.c` | Interactive REPL, env loading |
| `session.h/c` | Session namespace on KV Cache |
| `lua_engine.h/c` | LuaJIT embedding + C bindings |
| `llm_client.h/c` | OpenAI/Anthropic HTTP client |
| `prompts/default.lua` | System prompt + context assembly |
| `tools/default.lua` | Tool schemas + dispatch |
| `main.lua` | Runtime: register tools, chat loop |

## Build

```bash
make clean && make
```

Produces `libopencode_agent.a` and `opencode_cli`.

## Notes

- The `.env` file contains secrets and is gitignored.
- Long tool outputs are truncated to avoid model token limits.
- UI layer will be added later once the core tool loop is stable.

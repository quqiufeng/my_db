# opencode CLI

A lightweight CLI replica of opencode, built on the my_db KV Cache + C/LuaJIT stack.

## Quick start

```bash
cd /opt/my_db/opencode
make
./opencode_cli
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

## Commands inside the REPL

- Type any question or coding task.
- `/task <description>` — set the current task.
- `/quit` or `/exit` — leave.
- `goodbye` — say goodbye and exit the REPL politely.

## Goodbye Function

```python
def goodbye():
    """Print a farewell message and exit the REPL."""
    print("Goodbye!")
    exit(0)
```

A simple function that prints `Goodbye!` and exits the REPL cleanly.

## Architecture

- **C kernel**: KV Cache (`libmydb.so`), session namespace, HTTP client (libcurl), LuaJIT host.
- **LuaJIT layer**: prompt assembly, tool definitions, tool dispatch, LLM request body assembly.
- **Tools**: `bash`, `source_read`, `kv_search`, `kv_get`, `kv_context`, `code_index`.
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

## Goodbye

```
goodbye()
```

A built-in function that prints `Goodbye!` and exits the REPL cleanly.

Prints `Goodbye!` and exits the REPL cleanly.

## Goodbye Function

```c
void goodbye(void) {
    printf("Goodbye!\n");
    exit(0);
}
```

A simple C function that prints a farewell message and exits cleanly.

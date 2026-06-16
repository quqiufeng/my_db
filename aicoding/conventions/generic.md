# Generic Project Conventions

This is the default workflow for projects that do not match a specific type.

## Workflow

When asked to implement or fix something:

1. **Explore first**: use `glob` and `kv_search` to find relevant files. The code index is AST-aware where supported (e.g. TypeScript via tree-sitter), so `kv_search` with `search_type=semantic` understands functions, classes, and symbols, not just text.
2. **Read before editing**: always `read` the current content before using `edit`.
3. **Make minimal changes**: match existing style, naming, and file organization.
4. **Verify**: run `build` or the project's standard test command after changes.
5. **Summarize**: store key facts, file paths, and decisions in KV Cache with `kv_set`.

## Checklist

Before finishing, verify:

- [ ] Changes follow the existing project style.
- [ ] Build/test passes.
- [ ] No unintended files were modified.
- [ ] Key facts are stored in KV Cache for future sessions.

## Common mistakes

- Editing a file without reading it first.
- Using a broad `bash` command when `edit`/`write`/`apply_patch` would be safer.
- Forgetting to run the project's build/test command.
- Repeating the same failed tool call instead of inspecting state.

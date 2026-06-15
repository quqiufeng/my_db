# TypeScript/Node.js Indexer Plugin for coding.md

C implementation using tree-sitter. No Node.js / npm dependency.

## Dependencies

- tree-sitter core static library: `/opt/tree-sitter/libtree-sitter.a`
- tree-sitter-typescript parsers: `/opt/tree-sitter-typescript`
- jansson: JSON output library (`libjansson-dev`)

## Build

```bash
cd /opt/my_db/plugins/typescript-indexer
make
```

The Makefile statically links `libtree-sitter.a`, so the resulting binary only needs `libjansson.so.4` and `libc` at runtime:

```bash
ldd bin/typescript-indexer
# linux-vdso.so.1
# libjansson.so.4 => /lib/x86_64-linux-gnu/libjansson.so.4
# libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6
```

## Usage

```bash
./bin/typescript-indexer --file /opt/opencode/packages/core/src/session/message.ts --project /opt/opencode
```

Output is JSON Lines to stdout.

## Integration with code_indexer

The plugin is registered via `plugin.json`. `code_indexer` will:

1. Match files by extension (`.ts`, `.tsx`, `.js`, `.jsx`)
2. Spawn the plugin binary with `--file` and `--project`
3. Read JSON Lines output
4. Fall back to ctags if the plugin fails

## Integration with analyze_repo.sh

For Node.js / TypeScript projects, pass the plugin registry to `analyze_repo.sh`:

```bash
./analyze_repo.sh /opt/opencode /code/opencode \
  --name opencode \
  --plugins /opt/my_db/plugins/typescript-indexer/plugin.json \
  --jobs 4
```

Or use the convenience wrapper from the repo root:

```bash
./analyze_nodejs_repo.sh /opt/opencode /code/opencode
```

The wrapper auto-detects namespace, compiles the plugin if needed, and passes all common Node.js excludes (e.g. `node_modules,dist,build,.next`).

## Output Records

- `chunk`: extracted symbol (function, class, interface, type alias, enum, variable, namespace)
- `call_edge`: function call relationship
- `import_edge`: import/export relationship
- `metadata`: file-level stats

## Recent Optimizations

- Fixed `export const` incorrectly marked as non-export.
- Reduced noise from anonymous inline lambdas.
- Deduplicated arrow functions that were also emitted as variable chunks.
- Static-linked tree-sitter core to avoid runtime `libtree-sitter.so` dependency.

## Development

```bash
# Rebuild after source changes
make clean && make

# Test on a single file
./bin/typescript-indexer --file test.ts --project .

# Verify runtime dependencies
ldd bin/typescript-indexer
```

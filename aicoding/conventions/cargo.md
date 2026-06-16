# Rust / Cargo Project Conventions

These conventions apply to Rust projects using Cargo.

## Workflow

When modifying a Cargo project:

1. **Find relevant crates/modules**: use `glob`/`grep`/`kv_search`.
2. **Read before editing**: always `read` current code before using `edit`.
3. **Make changes**: follow existing Rust idioms and project naming conventions.
4. **Check types**: run `cargo check` frequently.
5. **Add/update tests**: tests live in `tests/` or inline `#[cfg(test)]` modules.
6. **Run tests**: run `cargo test`.
7. **Lint**: run `cargo clippy` if available.
8. **Summarize**: store key facts in KV Cache.

## Checklist

Before finishing, verify:

- [ ] `cargo check` passes.
- [ ] `cargo test` passes.
- [ ] `cargo clippy` has no new warnings (if used).
- [ ] Tests added/updated for changed behavior.
- [ ] Key facts stored in KV Cache.

## Common mistakes

- Forgetting to run `cargo check` after small edits.
- Adding dependencies without checking if they are already in `Cargo.toml`.
- Ignoring `clippy` warnings that indicate real issues.
- Modifying public API without updating examples or tests.

## Useful commands

- `cargo check` — fast type/compile check.
- `cargo build` — full build.
- `cargo test` — run tests.
- `cargo clippy` — lint.
- `cargo fmt` — format code.

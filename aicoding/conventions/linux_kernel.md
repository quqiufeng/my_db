# Linux Kernel Development Conventions

These conventions apply to the Linux kernel source tree.

## Workflow

When modifying the kernel:

1. **Identify the subsystem**: use `kv_search` (semantic symbol search where indexed) or `grep` to find relevant code.
2. **Find a template**: locate a similar existing implementation to copy structure from.
3. **Read Kconfig/Makefile**: understand how the subsystem is built and configured.
4. **Implement**: follow `Documentation/process/coding-style.rst`.
5. **Add exports if public**: new public functions need `EXPORT_SYMBOL()` or `EXPORT_SYMBOL_GPL()`.
6. **Style check**: run `scripts/checkpatch.pl --strict` on modified files.
7. **Build**: run `make` or `make -j$(nproc)`.
8. **Summarize**: store file paths, Kconfig changes, and design decisions in KV Cache.

## Checklist

Before finishing, verify:

- [ ] Code follows kernel coding style.
- [ ] `scripts/checkpatch.pl --strict` reports no new errors.
- [ ] Kconfig/Makefile updated if a driver or config option was added.
- [ ] New public functions have `EXPORT_SYMBOL*` where appropriate.
- [ ] Build succeeds (`make` or targeted build).
- [ ] Key facts stored in KV Cache.

## Common mistakes

- Forgetting to update `Kconfig` when adding a driver or feature.
- Using raw `printk` instead of `pr_*` / `dev_*` macros.
- Adding a public function without `EXPORT_SYMBOL` or header declaration.
- Modifying `.config` directly instead of using `make menuconfig` / `make oldconfig`.
- Editing without reading the surrounding code first.

## Useful commands

- `make -j$(nproc)` — full build.
- `make defconfig` — reset to default configuration.
- `make oldconfig` — update config after Kconfig changes.
- `scripts/checkpatch.pl --strict -f path/to/file.c` — style check a file.
- `git grep -n "symbol_name"` — find symbol usage across the tree.

# CMake Project Conventions

These conventions apply to C/C++ projects using CMake.

## Workflow

When modifying a CMake project:

1. **Find relevant sources**: use `glob`/`grep`/`kv_search`.
2. **Read before editing**: always `read` current code before using `edit`.
3. **Make changes**: match existing C/C++ style.
4. **Update build files**: if you add/remove source files, update `CMakeLists.txt`.
5. **Configure**: run `cmake -B build` if not already configured.
6. **Build**: run `cmake --build build`.
7. **Test**: run `ctest --test-dir build` if tests exist.
8. **Summarize**: store key facts in KV Cache.

## Checklist

Before finishing, verify:

- [ ] `CMakeLists.txt` updated if source files changed.
- [ ] Build succeeds (`cmake --build build`).
- [ ] Tests pass (`ctest` or project test command).
- [ ] No generated build artifacts committed unless intended.
- [ ] Key facts stored in KV Cache.

## Common mistakes

- Adding a `.cpp`/`.c` file without listing it in `CMakeLists.txt`.
- Running `cmake` from the source root instead of a build directory.
- Forgetting to reconfigure after changing CMake options.
- Mixing build systems (e.g. Makefile and CMake).

## Useful commands

- `cmake -B build` — configure in `build/`.
- `cmake --build build` — build.
- `cmake --build build --target test` — run tests.
- `ctest --test-dir build` — run ctest suite.

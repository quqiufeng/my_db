# Node.js / npm Project Conventions

These conventions apply to JavaScript/TypeScript projects using npm/yarn/pnpm.

## Workflow

When modifying an npm project:

1. **Find relevant files**: use `glob`/`grep`/`kv_search` for `src/`, `lib/`, `test/`.
2. **Read before editing**: always `read` current code before using `edit`.
3. **Make changes**: match existing style (ESLint/Prettier config if present).
4. **Install dependencies**: if you add imports, run the package manager install command.
5. **Add/update tests**: tests usually live in `test/`, `tests/`, `__tests__/`, or `*.test.*`.
6. **Run tests**: run the `test` script from `package.json`.
7. **Lint**: run the `lint` script if available.
8. **Summarize**: store key facts in KV Cache.

## Checklist

Before finishing, verify:

- [ ] Tests pass (`npm test` or equivalent).
- [ ] Lint passes (`npm run lint` or equivalent).
- [ ] No new uncommitted `package-lock.json`/`yarn.lock` changes unless intended.
- [ ] Public API changes documented or typed.
- [ ] Key facts stored in KV Cache.

## Common mistakes

- Adding a dependency without updating lockfile.
- Forgetting to run tests after a type change.
- Editing generated files in `dist/` or `node_modules/`.
- Ignoring existing ESLint/Prettier rules.

## Useful commands

- `npm test` — run tests.
- `npm run lint` — run linter.
- `npm run build` — build if a build script exists.
- `npm install <pkg>` — add dependency.

# Go Project Conventions

These conventions apply to Go projects using modules.

## Workflow

When modifying a Go project:

1. **Find relevant packages**: use `glob`/`grep`/`kv_search`.
2. **Read before editing**: always `read` current code before using `edit`.
3. **Make changes**: follow Go conventions and project style.
4. **Format**: Go code should be formatted with `gofmt`.
5. **Add/update tests**: tests are in `*_test.go` files.
6. **Run tests**: run `go test ./...`.
7. **Build**: run `go build ./...`.
8. **Summarize**: store key facts in KV Cache.

## Checklist

Before finishing, verify:

- [ ] `gofmt` formatted.
- [ ] `go test ./...` passes.
- [ ] `go build ./...` passes.
- [ ] `go mod tidy` run if dependencies changed.
- [ ] Key facts stored in KV Cache.

## Common mistakes

- Forgetting `go mod tidy` after adding/removing imports.
- Ignoring `go vet` warnings.
- Breaking public API without updating callers.
- Mixing error handling styles.

## Useful commands

- `go test ./...` — run all tests.
- `go build ./...` — build all packages.
- `gofmt -w .` — format code.
- `go vet ./...` — static analysis.
- `go mod tidy` — clean up module dependencies.

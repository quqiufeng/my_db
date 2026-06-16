# Python Project Conventions

These conventions apply to Python projects.

## Workflow

When modifying a Python project:

1. **Find relevant modules**: use `glob`/`grep`/`kv_search`.
2. **Read before editing**: always `read` current code before using `edit`.
3. **Make changes**: follow PEP 8 and project-specific style.
4. **Add/update tests**: tests usually live in `tests/` or `*_test.py`.
5. **Run tests**: run `pytest`, `python -m pytest`, or the project test command.
6. **Type check**: run `mypy` if the project uses type hints.
7. **Lint**: run `ruff`, `flake8`, or `pylint` if configured.
8. **Summarize**: store key facts in KV Cache.

## Checklist

Before finishing, verify:

- [ ] Tests pass.
- [ ] Lint/type checks pass (if configured).
- [ ] New dependencies added to `pyproject.toml`/`setup.py`/`requirements.txt`.
- [ ] Public API changes have docstrings.
- [ ] Key facts stored in KV Cache.

## Common mistakes

- Editing without checking indentation (spaces vs tabs).
- Forgetting to update `requirements.txt` or `pyproject.toml`.
- Running tests from the wrong directory.
- Breaking import paths by moving files without updating `__init__.py`.

## Useful commands

- `pytest` — run tests.
- `python -m pytest tests/` — run specific test directory.
- `ruff check .` — lint (if ruff is used).
- `mypy .` — type check (if configured).

#!/usr/bin/env bash
# Run the aicoding regression suite.
# Usage: ./tests/run_regression.sh [aicoding_dir]
# Requires: OPENCODE_ALLOW_ALL=1, OPENAI_API_KEY or ANTHROPIC_API_KEY, GUI display.

set -e

BASE_DIR="${1:-$(dirname "$0")/..}"
BASE_DIR="$(cd "$BASE_DIR" && pwd)"
cd "$BASE_DIR"

export LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:"$BASE_DIR"
export OPENCODE_GUI=1
export OPENCODE_ALLOW_ALL=1

AICODING="$BASE_DIR/aicoding"

failures=0

run_test() {
    local name="$1"
    local project="$2"
    local script="$3"
    echo "=== $name ==="
    if "$AICODING" --project "$project" --gui-test-script "$script" > /tmp/aicoding_regress_$name.log 2>&1; then
        echo "PASSED"
    else
        echo "FAILED (see /tmp/aicoding_regress_$name.log)"
        failures=$((failures + 1))
    fi
}

echo "Running C unit tests..."
if make test > /tmp/aicoding_regress_ctest.log 2>&1; then
    echo "C tests PASSED"
else
    echo "C tests FAILED (see /tmp/aicoding_regress_ctest.log)"
    failures=$((failures + 1))
fi

# Ensure libaicoding_gui.so is present.
if [ ! -f "$BASE_DIR/libaicoding_gui.so" ] && [ -f "$BASE_DIR/gui_gpui/target/release/libopencode_gui.so" ]; then
    cp "$BASE_DIR/gui_gpui/target/release/libopencode_gui.so" "$BASE_DIR/libaicoding_gui.so"
fi

# Prepare fresh midtest project.
rm -rf /tmp/midtest_project
mkdir -p /tmp/midtest_project
cat > /tmp/midtest_project/README.md <<'EOF'
# Mid Test
EOF
cat > /tmp/midtest_project/lib.lua <<'EOF'
-- lib
EOF
cd /tmp/midtest_project
git init -q
git config user.email "test@test"
git config user.name "Test"
git add .
git commit -q -m init
cd "$BASE_DIR"

# Prepare fresh writedit project.
rm -rf /tmp/writedit_project
mkdir -p /tmp/writedit_project/src
cat > /tmp/writedit_project/main.lua <<'EOF'
print("old")
EOF
cd /tmp/writedit_project
git init -q
git config user.email "test@test"
git config user.name "Test"
git add .
git commit -q -m init
cd "$BASE_DIR"

run_test "gui_hi"        /tmp/writedit_project tests/gui_hi.lua
run_test "gui_markdown"  /tmp/writedit_project tests/gui_markdown.lua
run_test "gui_write_edit" /tmp/writedit_project tests/gui_write_edit.lua
run_test "gui_mid_project" /tmp/midtest_project tests/gui_mid_project.lua
run_test "gui_build"     /tmp/writedit_project tests/gui_build.lua
run_test "gui_build_agent" /tmp/writedit_project tests/gui_build_agent.lua
run_test "gui_plan"      /tmp/writedit_project tests/gui_plan.lua
run_test "gui_web_fetch" /tmp/writedit_project tests/gui_web_fetch.lua

echo ""
if [ "$failures" -eq 0 ]; then
    echo "All regression tests PASSED."
    exit 0
else
    echo "$failures regression test(s) FAILED."
    exit 1
fi

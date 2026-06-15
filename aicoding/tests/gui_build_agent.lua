local function log(msg)
    print("[TEST] " .. tostring(msg))
end

-- Prepare a broken C project in /tmp.
local project_dir = "/tmp/broken_project"
os.execute("rm -rf " .. project_dir)
os.execute("mkdir -p " .. project_dir)

local main_c = [[
#include <stdio.h>

int main() {
    printf("Hello, World!\n")
    return 0;
}
]]
local makefile = "all: hello\nhello: main.c\n\tgcc -o hello main.c\n"

local f = io.open(project_dir .. "/main.c", "w")
f:write(main_c)
f:close()
local m = io.open(project_dir .. "/Makefile", "w")
m:write(makefile)
m:close()

log("prepared broken project in " .. project_dir)

-- Switch tools project root temporarily.
local tools = require("tools.default")
tools.set_project_root(project_dir)

log("running build agent")
local result = _G.run_build_agent(session_id, project_ns, "make the project compile")
log("agent result: " .. result:gsub("\n", " "):sub(1, 300))

if not result:find("Build succeeded") then
    log("FAILED: build agent did not succeed")
    error("build agent failed")
end

local exe = io.open(project_dir .. "/hello", "r")
if not exe then
    log("FAILED: compiled binary not found")
    error("binary missing")
end
exe:close()

log("PASSED")
os.exit(0)

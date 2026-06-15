local function log(msg)
    print("[TEST] " .. tostring(msg))
end

local cjson = require("cjson")
local tools = require("tools.default")

-- Prepare a simple project for the plan agent.
local project_dir = "/tmp/plantest_project"
os.execute("rm -rf " .. project_dir)
os.execute("mkdir -p " .. project_dir)

local f = io.open(project_dir .. "/README.md", "w")
f:write("# Plan Test\n")
f:close()

local lib = io.open(project_dir .. "/lib.lua", "w")
lib:write("module Plan\n")
lib:close()

local mf = io.open(project_dir .. "/Makefile", "w")
mf:write("all: check\ncheck:\n\t@echo 'ok'\n")
mf:close()

tools.set_project_root(project_dir)
log("prepared project in " .. project_dir)

log("running plan agent")
local result = _G.run_plan_agent(session_id, project_ns, "add a Lua function greet(name) to lib.lua that returns 'Hello, ' .. name, then verify the project builds")
log("agent result: " .. result:gsub("\n", " "):sub(1, 400))

if not result:find("Plan completed") and not result:find("success") then
    log("FAILED: plan agent did not report success")
    error("plan agent failed")
end

local f2 = io.open(project_dir .. "/lib.lua", "r")
if not f2 then
    log("FAILED: lib.lua not found")
    error("lib.lua missing")
end
local content = f2:read("*a")
f2:close()

if not content:find("function greet") then
    log("FAILED: greet function not found in lib.lua")
    error("greet function missing")
end

log("PASSED")
os.exit(0)

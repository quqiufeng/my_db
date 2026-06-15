local function log(msg)
    print("[TEST] " .. tostring(msg))
end

local cjson = require("cjson")
local tools = require("tools.default")
tools.set_project_root("/opt/my_db/aicoding")

log("auto-detecting and running build")

local result = tools.dispatch({
    name = "build",
    arguments = cjson.encode({ workdir = "/opt/my_db/aicoding" })
})

log("result: " .. cjson.encode(result):sub(1, 500))

if not result.ok then
    log("FAILED: build did not succeed")
    error("build failed")
end

if not (result.output and result.output:find("make")) then
    log("FAILED: expected make output")
    error("unexpected build output")
end

log("PASSED")
os.exit(0)

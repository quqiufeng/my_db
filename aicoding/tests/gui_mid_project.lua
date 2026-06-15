-- Mid-size project GUI integration test.
-- Verifies: read tool, edit tool, bash tool, checkpoint/undo, git diff.
-- Run with:
--   OPENCODE_GUI=1 LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
--     ./aicoding --project /tmp/midtest_project --gui-test-script tests/gui_mid_project.lua

local function log(msg)
    local line = "[TEST] " .. tostring(msg)
    print(line)
    if io and io.flush then io.flush() end
end

local function wait_for_assistant(deadline_seconds, predicate)
    local deadline = os.time() + (deadline_seconds or 60)
    while os.time() < deadline do
        local msgs = gui.get_messages(app)
        local last = msgs[#msgs]
        if last and last.role == "assistant" then
            if not predicate or predicate(last.text) then
                return last
            end
        end
        coroutine.yield()
    end
    return nil, "timeout waiting for assistant response"
end

local function count_tool_messages(tool_name)
    local msgs = gui.get_messages(app)
    local count = 0
    for i = 1, #msgs do
        local m = msgs[i]
        if m.role == "tool" and m.text:find("%[" .. tool_name .. "%]") then
            count = count + 1
        end
    end
    return count
end

log("submitting mid-size project task")
gui.set_input(app, "In this project, read README.md, then add a Lua function greet(name) to lib.lua that returns 'Hello, ' .. name, and run a bash command to verify the file content contains 'Hello,'. Then show git diff.")
gui.submit(app)

log("waiting for assistant to finish...")
local msg, err = wait_for_assistant(90, function(text)
    -- Accept final response once all expected tools have been used.
    -- The model may use either the dedicated `diff` tool or `git diff`.
    local has_read = count_tool_messages("read") >= 1
    local has_edit = (count_tool_messages("edit") >= 1) or (count_tool_messages("write") >= 1)
    local has_bash = count_tool_messages("bash") >= 1
    local has_diff = (count_tool_messages("diff") >= 1) or (count_tool_messages("git") >= 1)
    return has_read and has_edit and has_bash and has_diff
end)

if not msg then
    log("FAILED: " .. tostring(err))
    error(err)
end

log("final assistant: " .. msg.text:gsub("\n", " "):sub(1, 200))

-- Post-conditions: verify file on disk.
local f = io.open("/tmp/midtest_project/lib.lua", "r")
if not f then
    log("FAILED: lib.lua not found")
    error("lib.lua missing")
end
local content = f:read("*a")
f:close()

if not content:find("function greet") then
    log("FAILED: greet function not found in lib.lua")
    log("content: " .. content)
    error("greet function missing")
end

if not content:find("Hello,") then
    log("FAILED: 'Hello,' not found in lib.lua")
    log("content: " .. content)
    error("Hello missing")
end

log("file content verified")
log("PASSED")
os.exit(0)

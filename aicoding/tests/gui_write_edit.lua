-- UI integration test focused on write (create file) and edit (modify file).
-- Run with:
--   OPENCODE_GUI=1 OPENCODE_ALLOW_ALL=1 \
--   LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
--     ./aicoding --project /tmp/writedit_project \
--                --gui-test-script tests/gui_write_edit.lua

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

log("submitting write+edit task")
gui.set_input(app, "Create src/utils.lua with a function add(a, b) that returns a + b. Then edit main.lua to change 'print(\"old\")' to 'print(\"new\")'. Finally run bash to verify both files contain the expected content and show git diff.")
gui.submit(app)

log("waiting for assistant to finish...")
local msg, err = wait_for_assistant(120, function(text)
    local has_write = count_tool_messages("write") >= 1
    local has_edit = count_tool_messages("edit") >= 1
    local has_bash = count_tool_messages("bash") >= 1
    return has_write and has_edit and has_bash
end)

if not msg then
    log("FAILED: " .. tostring(err))
    error(err)
end

log("final assistant: " .. msg.text:gsub("\n", " "):sub(1, 200))

-- Post-conditions: verify files on disk.
local utils = io.open("/tmp/writedit_project/src/utils.lua", "r")
if not utils then
    log("FAILED: src/utils.lua not created")
    error("src/utils.lua missing")
end
local utils_content = utils:read("*a")
utils:close()
if not utils_content:find("function add") then
    log("FAILED: add function not found in src/utils.lua")
    log("content: " .. utils_content)
    error("add function missing")
end

local main = io.open("/tmp/writedit_project/main.lua", "r")
if not main then
    log("FAILED: main.lua not found")
    error("main.lua missing")
end
local main_content = main:read("*a")
main:close()
if main_content:find('print%("old"%)') then
    log("FAILED: old print still present in main.lua")
    log("content: " .. main_content)
    error("old print still present")
end
if not main_content:find('print%("new"%)') then
    log("FAILED: new print not found in main.lua")
    log("content: " .. main_content)
    error("new print missing")
end

log("file contents verified")
log("PASSED")
os.exit(0)

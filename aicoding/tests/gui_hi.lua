-- Automated GUI test: send "hi" and assert the assistant replies with a greeting.
-- Run with:
--   OPENCODE_GUI=1 LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
--     ./aicoding --project /code/current --gui-test-script tests/gui_hi.lua

local function log(msg)
    local line = "[TEST] " .. tostring(msg)
    print(line)
    if io and io.flush then io.flush() end
    if opencode and opencode.log_info then opencode.log_info(line) end
end

local function wait_for_assistant(deadline_seconds, predicate)
    local deadline = os.time() + (deadline_seconds or 30)
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

log("submitting: hi")
gui.set_input(app, "hi")
gui.submit(app)

local msg, err = wait_for_assistant(30, function(text)
    local t = text:lower()
    return t:find("hello") or t:find("hi") or t:find("help")
end)

if not msg then
    log("FAILED: " .. tostring(err))
    error(err)
end

log("assistant: " .. msg.text:gsub("\n", " "))
log("PASSED")
os.exit(0)

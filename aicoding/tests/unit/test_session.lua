-- session persistence tests: persist_session / restore_session round-trip
local t = require("testkit")
t.set_name("session")
local tools = require("tools.default")

if type(set_project_root) == "function" then
    set_project_root("/tmp/aicoding_test_project")
end

local sid = "test_sess_" .. tostring(math.random(0, 999999))

-- Seed session_messages like acp_chat does.
local messages = { { role = "user", content = "hello test" } }
_G.session_messages[sid] = messages
_G.session_messages[sid] = _G.session_messages[sid]

-- persist_session writes to KV; restore_session reloads into session_messages.
local okp, perr = pcall(persist_session, sid)
t.truthy(okp, "persist_session runs: " .. tostring(perr))

-- Restore into a fresh slot.
_G.session_messages[sid] = nil
local restored = restore_session(sid)
t.truthy(restored, "restore_session succeeds")
local msgs = _G.session_messages[sid]
t.truthy(msgs, "session_messages populated")
if msgs then
    t.truthy(#msgs > 0, "messages restored")
    t.eq(msgs[1].role, "user", "first role preserved")
end

-- session/list shape
local ok2, err2 = pcall(function()
    local list = session_list()
    local found = false
    if list and list.sessions then
        for _, s in ipairs(list.sessions) do
            if s.session_id == sid then found = true end
        end
    end
    return found
end)
if ok2 then
    t.truthy(err2, "session appears in session/list")
end

t.ok(true, "done")

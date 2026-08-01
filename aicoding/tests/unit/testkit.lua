-- Minimal test kit for engine unit tests (aicoding --test).
-- Each test file does `local t = require("testkit")` and runs assertions
-- at file scope; testkit accumulates results. The engine reads
-- _testkit.result() after each file.
local M = {
    passed = 0,
    failed = 0,
    failures = {},
    current = "unknown",
}

function M.set_name(name)
    M.current = name
end

local function record(ok, msg)
    if ok then
        M.passed = M.passed + 1
    else
        M.failed = M.failed + 1
        table.insert(M.failures, M.current .. ": " .. tostring(msg))
        io.stderr:write(string.format("FAIL [%s] %s\n", M.current, tostring(msg)))
    end
end

function M.ok(cond, msg)
    record(cond and true, msg or "ok")
end

function M.eq(a, b, msg)
    record(a == b, string.format("%s (got %s, want %s)", msg or "eq", tostring(a), tostring(b)))
end

function M.truthy(a, msg)
    record(a and true, string.format("%s (got falsy: %s)", msg or "truthy", tostring(a)))
end

function M.nil_(a, msg)
    record(a == nil, string.format("%s (got %s)", msg or "nil", tostring(a)))
end

function M.contains(haystack, needle, msg)
    record(type(haystack) == "string" and haystack:find(needle, 1, true) ~= nil,
        string.format("%s (needle %q not found in %q)", msg or "contains", tostring(needle), tostring(haystack)))
end

function M.result()
    return M.passed, M.failed, M.failures
end

function M.reset()
    M.passed = 0
    M.failed = 0
    M.failures = {}
end

_G._testkit = M
return M

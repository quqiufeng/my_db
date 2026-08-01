-- KV cache tool tests (kv_set / kv_get / kv_search round-trip)
local t = require("testkit")
t.set_name("kv")
local tools = require("tools.default")

-- Test mode uses /tmp/aicoding_test_cache; project root is the cwd.
if type(set_project_root) == "function" then
    set_project_root("/tmp/aicoding_test_project")
end

local ns = "/test/unit/" .. tostring(math.random(0, 999999))
local key = ns .. "/fact1"

-- kv_set then kv_get round-trip.
local r1 = tools.dispatch({ name = "kv_set", arguments = { key = "fact1", value = "hello world", namespace = ns } })
t.truthy(r1 and r1.ok, "kv_set ok: " .. tostring(r1 and r1.error))

local r2 = tools.dispatch({ name = "kv_get", arguments = { key = "fact1", namespace = ns } })
t.truthy(r2 and r2.ok, "kv_get ok")
t.eq(r2.value, "hello world", "kv_get returns stored value")

-- TTL: 1-second TTL expires.
local ttl_key = ns .. "/short"
tools.dispatch({ name = "kv_set", arguments = { key = "short", value = "x", namespace = ns, ttl_seconds = 1 } })
local r3 = tools.dispatch({ name = "kv_get", arguments = { key = "short", namespace = ns } })
t.eq(r3.value, "x", "value present before TTL")
os.execute("sleep 2")
local r4 = tools.dispatch({ name = "kv_get", arguments = { key = "short", namespace = ns } })
t.eq(r4.value, nil, "value expired after TTL")

-- kv_search finds the stored fact.
local r5 = tools.dispatch({ name = "kv_search", arguments = { query = "hello world", namespace = ns } })
t.truthy(r5 and r5.ok, "kv_search ok")
t.truthy(type(r5.results) == "table" and #r5.results > 0, "search returns results")

-- Cleanup
tools.dispatch({ name = "kv_set", arguments = { key = "fact1", namespace = ns, value = "", ttl_seconds = 0 } })
t.ok(true, "done")

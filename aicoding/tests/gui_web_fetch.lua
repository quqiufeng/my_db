local function log(msg)
    print("[TEST] " .. tostring(msg))
end

local cjson = require("cjson")
local tools = require("tools.default")

log("fetching example.com")
local result = tools.dispatch({
    name = "web_fetch",
    arguments = cjson.encode({
        url = "https://example.com",
        format = "text",
        max_length = 2000,
    })
})

log("result keys: " .. cjson.encode({ok = result.ok, url = result.url, format = result.format, truncated = result.truncated}))

if not result.ok then
    log("FAILED: web_fetch returned error: " .. tostring(result.error))
    error("web_fetch failed")
end

if not (result.content and result.content:find("Example Domain")) then
    log("FAILED: expected 'Example Domain' in content")
    error("content mismatch")
end

log("PASSED")
os.exit(0)

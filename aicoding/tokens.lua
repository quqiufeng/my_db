-- tokens.lua - unified token estimation for aicoding
--
-- Provides a single, conservative token estimator used by context.lua,
-- compress.lua, summarize.lua, and any other module that needs to reason
-- about prompt size or cost.

local M = {}

-- Default ratios. These are intentionally conservative: we slightly
-- over-count to avoid accidentally exceeding the context window.
M.RATIOS = {
    ascii = 0.25,      -- English / code / ASCII punctuation
    cjk   = 2.0,       -- Chinese, Japanese, Korean
    other = 1.0,       -- other non-ASCII bytes
}

-- Estimate token count for a single string.
function M.estimate(text)
    if text == nil then return 0 end
    local s = tostring(text)
    local len = #s
    local non_ascii = 0
    for i = 1, len do
        if s:byte(i) > 127 then non_ascii = non_ascii + 1 end
    end
    local ascii = len - non_ascii
    return math.floor(
        ascii * M.RATIOS.ascii +
        non_ascii * M.RATIOS.other +
        0.5
    )
end

-- Estimate tokens for a chat messages array.
function M.estimate_messages(messages)
    local total = 0
    for _, m in ipairs(messages or {}) do
        total = total + M.estimate(m.role or "")
        total = total + M.estimate(m.content or "")
        if m.name then
            total = total + M.estimate(m.name)
        end
        if m.tool_calls then
            total = total + M.estimate(M.encode_json(m.tool_calls))
        end
        if m.tool_call_id then
            total = total + M.estimate(m.tool_call_id)
        end
    end
    return total
end

-- Best-effort JSON encoding for token counting (does not throw).
function M.encode_json(val)
    local cjson = require("cjson")
    local ok, s = pcall(cjson.encode, val)
    if ok then return s end
    return ""
end

-- Convenience: format a token count with friendly units.
function M.format(n)
    if n < 1000 then return tostring(n) end
    return string.format("%.1fk", n / 1000)
end

return M

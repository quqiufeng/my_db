-- json.lua - safe JSON wrapper around cjson
--
-- Centralize encode/decode so every module handles errors consistently.

local cjson = require("cjson")

local M = {}

-- Decode JSON safely. Returns (value, nil) on success, (nil, err) on failure.
function M.decode(s)
    if s == nil or s == "" then
        return nil, "empty input"
    end
    local ok, val = pcall(cjson.decode, s)
    if not ok then
        return nil, tostring(val)
    end
    return val, nil
end

-- Decode JSON, returning nil on failure (convenience for boolean checks).
function M.decode_or_nil(s)
    local val, _ = M.decode(s)
    return val
end

-- Encode JSON safely. Returns (string, nil) on success, (nil, err) on failure.
function M.encode(val)
    local ok, s = pcall(cjson.encode, val)
    if not ok then
        return nil, tostring(s)
    end
    return s, nil
end

-- Decode or return a default value.
function M.decode_or(s, default)
    local val, err = M.decode(s)
    if val == nil then
        return default, err
    end
    return val, nil
end

return M

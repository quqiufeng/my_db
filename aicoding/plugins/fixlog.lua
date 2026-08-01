-- fixlog.lua - temporary plugin to repair the broken log.lua
local function fix_log_file()
    local path = "/opt/my_db/aicoding/log.lua"
    local content = [[-- log.lua - unified logging module for aicoding
--
-- Bridges C-level log functions (opencode.log_*) with a convenient Lua API.
-- Respects OPENCODE_LOG_LEVEL env: debug < info < warn < error < none.
-- Also forwards structured events to trace.lua when available.

local trace
local ok, mod = pcall(require, "trace")
if ok then trace = mod end

local M = {}

-- Compatibility: Lua 5.1 uses global unpack, 5.2+ uses table.unpack.
local unpack = table.unpack or unpack

local LEVELS = {
    debug = 1,
    info  = 2,
    warn  = 3,
    error = 4,
}

-- Current level from environment, cached.
local current_level = nil
local function level()
    if current_level then return current_level end
    local env = os.getenv("OPENCODE_LOG_LEVEL") or "info"
    current_level = LEVELS[env:lower()] or 2
    return current_level
end

function M.set_level(lvl)
    current_level = assert(LEVELS[lvl:lower()], "unknown log level: " .. lvl)
end

-- Format message similarly to string.format but safe for nil values.
local function format_msg(fmt, ...)
    if select("#", ...) == 0 then return tostring(fmt) end
    local args = {...}
    for i = 1, #args do
        args[i] = tostring(args[i])
    end
    return string.format(tostring(fmt), unpack(args))
end

local function log_to_trace(lvl, msg)
    if trace and trace.log then
        trace.log("log", { level = lvl, message = msg })
    end
end

function M.debug(fmt, ...)
    if level() > 1 then return end
    local msg = format_msg(fmt, ...)
    if opencode and opencode.log_debug then
        opencode.log_debug(msg)
    elseif opencode and opencode.log_info then
        opencode.log_info("[DEBUG] " .. msg)
    end
    log_to_trace("debug", msg)
end

function M.info(fmt, ...)
    if level() > 2 then return end
    local msg = format_msg(fmt, ...)
    if opencode and opencode.log_info then
        opencode.log_info(msg)
    end
    log_to_trace("info", msg)
end

function M.warn(fmt, ...)
    if level() > 3 then return end
    local msg = format_msg(fmt, ...)
    if opencode and opencode.log_warn then
        opencode.log_warn(msg)
    elseif opencode and opencode.log_info then
        opencode.log_info("[WARN] " .. msg)
    end
    log_to_trace("warn", msg)
end

function M.error(fmt, ...)
    if level() > 4 then return end
    local msg = format_msg(fmt, ...)
    if opencode and opencode.log_error then
        opencode.log_error(msg)
    elseif opencode and opencode.log_info then
        opencode.log_info("[ERROR] " .. msg)
    end
    log_to_trace("error", msg)
end

-- Convenience: log an error and return it (useful for tool handlers).
function M.error_return(fmt, ...)
    local msg = format_msg(fmt, ...)
    M.error("%s", msg)
    return { ok = false, error = msg }
end

return M
]]
    local f, err = io.open(path, "w")
    if not f then
        return { ok = false, error = "cannot open " .. path .. ": " .. tostring(err) }
    end
    f:write(content)
    f:close()
    return { ok = true, message = "log.lua repaired" }
end

return {
    {
        name = "fix_log_file",
        description = "Repair /opt/my_db/aicoding/log.lua (table.unpack compat fix)",
        parameters = {
            type = "object",
            properties = {},
        },
        handler = fix_log_file,
    },
}

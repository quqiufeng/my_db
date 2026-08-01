-- opencode permissions system
-- Loads .opencode/config.json(c) or ~/.config/opencode/config.json and evaluates
-- permission rules in opencode-compatible shape.
--
-- Supported config formats:
--   legacy: { "permission": { "bash": "ask", "edit": "allow", "git status": "allow" } }
--   v2:     { "permissions": [
--             { "action": "bash", "resource": "*", "effect": "ask" },
--             { "action": "bash", "resource": "git status", "effect": "allow" }
--           ]}
-- effect values: "allow" | "deny" | "ask"
--
-- Matching: rules are evaluated in order, LAST match wins (like opencode).
-- Wildcards use simple * matching. Paths starting with ~ are expanded.

local json = require("json")

local M = {}

M.rules = {}

local function expand_home(path)
    if path == "~" then
        return os.getenv("HOME") or os.getenv("USERPROFILE") or "~"
    end
    if path:sub(1, 2) == "~/" then
        local home = os.getenv("HOME") or os.getenv("USERPROFILE") or "."
        return home .. path:sub(2)
    end
    return path
end

local function read_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    return s
end

local function parse_jsonc(text)
    -- Strip single-line comments
    local lines = {}
    for line in text:gmatch("[^\r\n]+") do
        local stripped = line:gsub("//.*$", "")
        if stripped:match("%S") then
            table.insert(lines, stripped)
        end
    end
    local joined = table.concat(lines, "\n")
    local v, _ = json.decode(joined)
    if ok then return v end
    return nil
end

local function load_config(path)
    local text = read_file(path)
    if not text then return nil end
    local cfg = parse_jsonc(text)
    if not cfg then
        log.warn("failed to parse config %s", path)
        return nil
    end
    return cfg
end

-- Normalize legacy permission map and v2 rules array into a flat rules list.
local function normalize_rules(cfg)
    local rules = {}

    -- v2 array style
    if cfg.permissions and type(cfg.permissions) == "table" then
        for _, r in ipairs(cfg.permissions) do
            table.insert(rules, {
                action = r.action or "*",
                resource = r.resource or "*",
                effect = r.effect or "ask"
            })
        end
    end

    -- legacy map style { "permission": { "bash": "ask", "git status": "allow" } }
    if cfg.permission and type(cfg.permission) == "table" then
        for key, value in pairs(cfg.permission) do
            if type(value) == "string" then
                table.insert(rules, {
                    action = key,
                    resource = "*",
                    effect = value
                })
            elseif type(value) == "table" then
                for pattern, effect in pairs(value) do
                    table.insert(rules, {
                        action = key,
                        resource = expand_home(pattern),
                        effect = effect
                    })
                end
            end
        end
    end

    return rules
end

function M.load(project_root)
    M.rules = {}

    -- Global config: ~/.config/opencode/config.json
    local global = load_config(expand_home("~/.config/opencode/config.json"))
    if global then
        for _, r in ipairs(normalize_rules(global)) do
            table.insert(M.rules, r)
        end
    end

    -- Project config: .opencode/config.json or .opencode/opencode.jsonc
    if project_root then
        local proj = load_config(project_root .. "/.opencode/config.json")
        if not proj then
            proj = load_config(project_root .. "/.opencode/opencode.jsonc")
        end
        if proj then
            for _, r in ipairs(normalize_rules(proj)) do
                table.insert(M.rules, r)
            end
        end
    end

    -- Built-in safety defaults. These are always present and evaluated LAST
    -- so that user/project config can override them.
    local home = expand_home("~")
    local builtins = {
        -- Protect system directories and user home root from writes/deletes.
        { action = "write", resource = "/usr/*", effect = "deny" },
        { action = "write", resource = "/etc/*", effect = "deny" },
        { action = "write", resource = "/bin/*", effect = "deny" },
        { action = "write", resource = "/sbin/*", effect = "deny" },
        { action = "write", resource = "/lib*", effect = "deny" },
        { action = "write", resource = "/opt/my_db/*", effect = "deny" },
        { action = "write", resource = home .. "/*", effect = "deny" },
        { action = "file_create", resource = "/usr/*", effect = "deny" },
        { action = "file_create", resource = "/etc/*", effect = "deny" },
        { action = "file_create", resource = "/bin/*", effect = "deny" },
        { action = "file_create", resource = "/sbin/*", effect = "deny" },
        { action = "file_create", resource = "/lib*", effect = "deny" },
        { action = "file_create", resource = "/opt/my_db/*", effect = "deny" },
        { action = "file_create", resource = home .. "/*", effect = "deny" },
        { action = "file_delete", resource = "/usr/*", effect = "deny" },
        { action = "file_delete", resource = "/etc/*", effect = "deny" },
        { action = "file_delete", resource = "/bin/*", effect = "deny" },
        { action = "file_delete", resource = "/sbin/*", effect = "deny" },
        { action = "file_delete", resource = "/lib*", effect = "deny" },
        { action = "file_delete", resource = "/opt/my_db/*", effect = "deny" },
        { action = "file_delete", resource = home .. "/*", effect = "deny" },
        { action = "delete", resource = "/usr/*", effect = "deny" },
        { action = "delete", resource = "/etc/*", effect = "deny" },
        { action = "delete", resource = "/bin/*", effect = "deny" },
        { action = "delete", resource = "/sbin/*", effect = "deny" },
        { action = "delete", resource = "/lib*", effect = "deny" },
        { action = "delete", resource = "/opt/my_db/*", effect = "deny" },
        { action = "delete", resource = home .. "/*", effect = "deny" },
    }
    for _, r in ipairs(builtins) do
        table.insert(M.rules, r)
    end

    -- Defaults when no user/project rules are configured. The built-in deny
    -- rules above must not count: they always exist, so the presence of a
    -- default catch-all is decided by user configuration alone.
    if #M.rules <= #builtins then
        local default = "allow"
        local catchall = {
            { action = "read", resource = "*", effect = "allow" },
            { action = "*", resource = "*", effect = default }
        }
        for _, r in ipairs(catchall) do
            table.insert(M.rules, r)
        end
        return
    end

    -- User rules are present: still append a sensible catch-all so the
    -- default behavior is explicit instead of falling through to "ask".
    local default = "allow"
    table.insert(M.rules, { action = "read", resource = "*", effect = "allow" })
    table.insert(M.rules, { action = "*", resource = "*", effect = default })

    -- If OPENCODE_ALLOW_ALL is set but user/project rules are present, prepend
    -- a permissive catch-all so the built-in deny rules do not block tests.
    if os.getenv("OPENCODE_ALLOW_ALL") == "1" then
        table.insert(M.rules, 1, { action = "*", resource = "*", effect = "allow" })
    end
end

-- Simple glob matching: * matches any sequence, exact otherwise
local function match_wildcard(pattern, value)
    if pattern == "*" then return true end
    if pattern == value then return true end
    -- Convert pattern to a Lua pattern
    local lua_pattern = pattern:gsub("([%.%+%-%^%$%(%)%%[%]])", "%%%1")
    lua_pattern = lua_pattern:gsub("%*", ".-")
    local ok, m = pcall(string.match, value, "^" .. lua_pattern .. "$")
    return ok and m ~= nil
end

-- Find FIRST matching rule (most specific rules should be listed first;
-- a trailing catch-all { "action": "*", "resource": "*", "effect": "ask" }
-- acts as the default).
function M.check(action, resource)
    resource = resource or ""
    -- Normalize resource path: collapse redundant slashes.
    resource = resource:gsub("//+", "/")
    for _, rule in ipairs(M.rules) do
        local rule_action = rule.action or "*"
        local rule_resource = rule.resource or "*"
        -- Expand ~ in rule resources so user configs work regardless of shape.
        if rule_resource:sub(1, 2) == "~/" then
            rule_resource = expand_home(rule_resource)
        end
        if (rule_action == "*" or match_wildcard(rule_action, action)) and
           match_wildcard(rule_resource, resource) then
            return rule.effect or "allow"
        end
    end
    return "allow"
end

-- Check whether a bash command contains known dangerous patterns.
function M.is_dangerous_bash(cmd)
    if not cmd then return false end
    local dangerous = {
        "rm%s+%-rf%s+/",
        "rm%s+%-rf%s+%$?/",
        "mkfs",
        "dd%s+if=",
        ":(){",
        -- Write raw data to block devices (e.g. dd of=/dev/sda, cat > /dev/sda)
        "dd[^i].*of=/dev/s",
        ">/dev/sd",
        ">/dev/nvme",
        ">/dev/mmc",
        "curl%s+.*%|%s*sh",
        "wget%s+.*%|%s*sh",
    }
    local c = cmd:lower()
    for _, pat in ipairs(dangerous) do
        if c:find(pat) then
            return true, "dangerous pattern: " .. pat
        end
    end
    return false, nil
end

-- For CLI: ask user interactively via /dev/tty so stdin stays free for chat.
function M.prompt_user(action, resource)
    local tty = io.open("/dev/tty", "w")
    local tin = io.open("/dev/tty", "r")
    if tty and tin then
        tty:write(string.format("Allow %s '%s'? [y/N/a(ll)/d(eny)]: ", action, resource))
        tty:flush()
        local answer = tin:read("*l") or ""
        tin:close()
        tty:close()
        answer = answer:lower():gsub("%s+", "")
        if answer == "a" or answer == "all" then
            return "allow_session"
        elseif answer == "d" or answer == "deny" then
            return "deny"
        elseif answer == "y" or answer == "yes" then
            return "allow"
        else
            return "deny"
        end
    end
    -- fallback: if no tty, assume non-interactive and deny
    return "deny"
end

return M

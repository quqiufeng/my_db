-- shell.lua - POSIX shell quoting and safe command helpers
--
-- Centralize all os.execute / io.popen quoting so no module has to
-- re-implement (and possibly get wrong) shell escaping.

local M = {}

-- Quote a string for use inside a POSIX single-quoted shell argument.
function M.quote(s)
    if s == nil then return "''" end
    s = tostring(s)
    -- Single-quote the whole string, and escape any embedded single quotes
    -- by exiting the quotes, inserting an escaped quote, then re-entering.
    return "'" .. s:gsub("'", "'\"'\"'") .. "'"
end

-- Quote a list of arguments, returning a single shell-escaped string.
function M.quote_args(args)
    local out = {}
    for _, a in ipairs(args or {}) do
        table.insert(out, M.quote(a))
    end
    return table.concat(out, " ")
end

-- Run a shell command safely with arguments passed as a list.
-- Returns stdout string and the exit status object from io.popen.
function M.run(cmd, args)
    local full = cmd
    if args and #args > 0 then
        full = cmd .. " " .. M.quote_args(args)
    end
    local f = io.popen(full .. " 2>&1")
    if not f then return nil, "failed to run: " .. full end
    local out = f:read("*a") or ""
    local ok = f:close()
    return out, ok
end

-- mkdir -p with safe quoting.
function M.mkdir_p(path)
    return os.execute("mkdir -p " .. M.quote(path))
end

-- ls -1 in a directory, returning a list of filenames.
function M.ls(dir)
    local p = io.popen("ls -1 " .. M.quote(dir) .. " 2>/dev/null")
    if not p then return {} end
    local out = p:read("*a") or ""
    p:close()
    local files = {}
    for line in out:gmatch("[^\r\n]+") do
        table.insert(files, line)
    end
    return files
end

return M

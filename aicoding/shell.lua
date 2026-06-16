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

-- mkdir -p with safe quoting. Returns true if the directory exists (or was created).
-- Errors are silently ignored.
function M.mkdir_p(path)
    if not path or path == "" then return false end
    -- Try mkdir -p directly; suppress stderr to avoid noise on read-only dirs.
    os.execute("mkdir -p " .. M.quote(path) .. " 2>/dev/null")
    -- Verify the directory was actually created.
    return os.execute("test -d " .. M.quote(path) .. " 2>/dev/null") == 0
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

-- Return a writable runtime directory for a project.
-- Tries project_root/.opencode/<subdir> first; if not writable,
-- falls back to ~/.aicoding/runtime/<project_basename>/<subdir>.
-- Automatically creates the directory if it doesn't exist.
function M.runtime_dir(project_root, subdir, project_basename)
    project_root = project_root or "."
    subdir = subdir or ""
    project_basename = project_basename or "default"

    -- Try project-local .opencode first
    local local_dir = project_root .. "/.opencode" .. (subdir ~= "" and "/" .. subdir or "")
    if M.mkdir_p(local_dir) then
        return local_dir
    end

    -- Fallback to home runtime directory
    local home = os.getenv("HOME") or "/tmp"
    local fallback = home .. "/.aicoding/runtime/" .. project_basename .. (subdir ~= "" and "/" .. subdir or "")
    M.mkdir_p(fallback)
    return fallback
end

return M

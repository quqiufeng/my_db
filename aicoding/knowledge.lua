-- knowledge.lua - project-level memory shared across sessions
--
-- Files under PROJECT_ROOT/.opencode/knowledge/ are treated as long-term
-- project memory. They are loaded into the system prompt and synced to KV
-- Cache so semantic search can recall them.

local cjson = require("cjson")
local memory = require("memory")

local M = {}

local MAX_FILE_SIZE = 8192
local MAX_PROMPT_SIZE = 8192

-- Shell-quote a path for use with os.execute.
local function shell_quote(s)
    return "'" .. tostring(s):gsub("'", "'\"'\"'") .. "'"
end

function M.root(project_root)
    return (project_root or ".") .. "/.opencode/knowledge"
end

function M.project_ns(project_root)
    local name = (project_root or "."):match("([^/]+)$") or "project"
    return "/project/" .. name
end

-- Ensure knowledge directory exists.
function M.ensure_dir(project_root)
    local root = M.root(project_root)
    os.execute("mkdir -p " .. shell_quote(root))
    return root
end

-- List knowledge files (markdown and plain text).
function M.list(project_root)
    local root = M.root(project_root)
    local p = io.popen("ls -1 " .. shell_quote(root) .. " 2>/dev/null")
    if not p then return {} end
    local out = p:read("*a") or ""
    p:close()
    local files = {}
    for name in out:gmatch("[^\r\n]+") do
        if name:match("%.md$") or name:match("%.txt$") then
            table.insert(files, name)
        end
    end
    table.sort(files)
    return files
end

-- Read a knowledge file.
function M.read(project_root, name)
    local path = M.root(project_root) .. "/" .. name
    local f = io.open(path, "r")
    if not f then return nil, "not found: " .. path end
    local content = f:read("*a") or ""
    f:close()
    return content
end

-- Write a knowledge file and sync to KV Cache.
function M.write(project_root, name, content)
    M.ensure_dir(project_root)
    local path = M.root(project_root) .. "/" .. name
    if not name:match("%.md$") and not name:match("%.txt$") then
        name = name .. ".md"
        path = M.root(project_root) .. "/" .. name
    end
    local f = io.open(path, "w")
    if not f then return false, "cannot write " .. path end
    f:write(content)
    f:close()
    M.sync_file_to_kv(project_root, name)
    return true, path
end

-- Sync one file to KV Cache.
function M.sync_file_to_kv(project_root, name)
    local content = M.read(project_root, name)
    if not content then return false end
    local key = M.project_ns(project_root) .. "/knowledge/" .. name
    memory.write(key, content, 0)
    return true
end

-- Sync all knowledge files to KV Cache.
function M.sync_to_kv(project_root)
    local count = 0
    for _, name in ipairs(M.list(project_root)) do
        if M.sync_file_to_kv(project_root, name) then
            count = count + 1
        end
    end
    return count
end

-- Build a prompt fragment from all knowledge files, truncated to fit.
function M.prompt_fragment(project_root)
    local files = M.list(project_root)
    if #files == 0 then return nil end

    local parts = {}
    for _, name in ipairs(files) do
        local content = M.read(project_root, name)
        if content and content:match("%S") then
            if #content > MAX_FILE_SIZE then
                content = content:sub(1, MAX_FILE_SIZE) .. "\n[truncated]"
            end
            table.insert(parts, string.format("## %s\n%s", name, content))
        end
    end

    local fragment = table.concat(parts, "\n\n")
    if #fragment > MAX_PROMPT_SIZE then
        fragment = fragment:sub(1, MAX_PROMPT_SIZE) .. "\n\n[Project knowledge truncated. Use knowledge_read/knowledge_search for full content.]"
    end
    return fragment
end

-- Search project knowledge via KV Cache (semantic over project namespace).
function M.search(project_root, query, top_k)
    local ns = M.project_ns(project_root)
    local results, err = memory.search(query, {
        namespace = ns,
        search_type = "semantic",
        top_k = top_k or 5
    })
    if not results then
        return nil, err
    end
    return results
end

return M

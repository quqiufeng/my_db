-- opencode tool definitions and dispatch in Lua
-- Tools are stored as KV records under /agent/default/tools/{name}
-- so that the C kernel and prompt builder can discover them dynamically.

local cjson = require("cjson")
local permissions = require("permissions")

local M = {}

-- Helper: convert internal param spec to OpenAI JSON Schema properties + required array
local function schema_from_params(params)
    local props = {}
    local required = {}
    for name, p in pairs(params) do
        props[name] = {}
        for k, v in pairs(p) do
            if k ~= "required" then
                props[name][k] = v
            end
        end
        if p.required then
            table.insert(required, name)
        end
    end
    return {
        type = "object",
        properties = props,
        required = required
    }
end

-- Helper: remove empty required array to keep strict validators happy
local function finalize_schema(schema)
    if schema.required and #schema.required == 0 then
        schema.required = nil
    end
    return schema
end

local project_root = "."

function M.set_project_root(path)
    project_root = path or "."
    permissions.load(project_root)
end

function M.get_project_root()
    return project_root
end

-- Resolve a possibly-relative path against project root
local function resolve_path(path)
    if not path then return nil end
    if path:sub(1, 1) == "/" then
        return path
    end
    if project_root == "." then
        return path
    end
    if project_root:sub(-1) == "/" then
        return project_root .. path
    end
    return project_root .. "/" .. path
end

-- Read whole file
local function read_file(path)
    local f = io.open(path, "r")
    if not f then return nil, "cannot open " .. path end
    local content = f:read("*a")
    f:close()
    return content
end

-- Write whole file
local function write_file(path, content)
    local f, err = io.open(path, "w")
    if not f then return false, err end
    f:write(content)
    f:close()
    return true
end

-- Truncate long outputs to avoid exceeding model context window
local function truncate(s, max_len)
    max_len = max_len or 4000
    if not s then return s end
    if #s <= max_len then return s end
    return s:sub(1, max_len) .. "\n... [truncated " .. tostring(#s - max_len) .. " chars]"
end

-- Permission check wrapper
local function permit(action, resource)
    local effect = permissions.check(action, resource)
    if effect == "deny" then
        return false, "denied by permission rule"
    end
    if effect == "ask" then
        local answer = permissions.prompt_user(action, resource)
        if answer == "deny" then
            return false, "denied by user"
        elseif answer == "allow_session" then
            table.insert(permissions.rules, { action = action, resource = "*", effect = "allow" })
            return true, nil
        elseif answer ~= "allow" then
            return false, "denied by user"
        end
    end
    return true, nil
end

M.tools = {
    {
        name = "kv_search",
        description = "Search the KV Cache for relevant code, facts, or history.",
        parameters = {
            query = { type = "string", required = true },
            namespace = { type = "string", required = false },
            search_type = { type = "string", enum = {"semantic", "prefix", "regex", "tag"}, required = false }
        },
        handler = function(args)
            local q = args.query or ""
            local ns = args.namespace or ""
            local prefix = ns
            if prefix ~= "" and not prefix:match("/$") then prefix = prefix .. "/" end
            local results = opencode.cache_search_prefix(prefix, 10)
            local out = {}
            for _, r in ipairs(results) do
                table.insert(out, { key = r.key, value = r.value, score = r.score })
            end
            return { ok = true, results = out }
        end
    },
    {
        name = "kv_get",
        description = "Read an exact key from the KV Cache.",
        parameters = {
            key = { type = "string", required = true }
        },
        handler = function(args)
            local val = opencode.cache_get(args.key)
            return { ok = val ~= nil, value = val }
        end
    },
    {
        name = "kv_context",
        description = "Get full context for a symbol (callers/callees) from a repo namespace.",
        parameters = {
            symbol = { type = "string", required = true },
            repo = { type = "string", required = true }
        },
        handler = function(args)
            local cmd = string.format("cd /opt/my_db && ./tools/cache_query %s --repo %s --type context 2>&1",
                                        cjson.encode(args.symbol), cjson.encode(args.repo))
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            local ok = f:close()
            return { ok = ok, output = truncate(out) }
        end
    },
    {
        name = "source_read",
        description = "Read a range of lines from a source file.",
        parameters = {
            path = { type = "string", required = true },
            line_start = { type = "integer", required = false },
            line_end = { type = "integer", required = false }
        },
        handler = function(args)
            local path = resolve_path(args.path)
            local content, err = opencode.source_read(path, args.line_start or 1, args.line_end or 0)
            if not content then
                return { ok = false, error = err }
            end
            return { ok = true, content = content }
        end
    },
    {
        name = "apply_edit",
        description = "Apply a precise text replacement in a file. old_string must match exactly.",
        parameters = {
            path = { type = "string", required = true },
            old_string = { type = "string", required = true },
            new_string = { type = "string", required = true }
        },
        handler = function(args)
            local ok, err = permit("apply_edit", resolve_path(args.path))
            if not ok then return { ok = false, error = err } end
            local path = resolve_path(args.path)
            local content, rerr = read_file(path)
            if not content then
                return { ok = false, error = rerr }
            end
            local old = args.old_string
            local idx = content:find(old, 1, true)
            if not idx then
                return { ok = false, error = "old_string not found in file" }
            end
            -- Reject ambiguous matches
            local second = content:find(old, idx + #old, true)
            if second then
                return { ok = false, error = "old_string matches multiple locations; use a more unique block" }
            end
            local new_content = content:sub(1, idx - 1) .. args.new_string .. content:sub(idx + #old)
            local wok, werr = write_file(path, new_content)
            if not wok then
                return { ok = false, error = tostring(werr) }
            end
            return { ok = true, replaced = old, with = args.new_string }
        end
    },
    {
        name = "file_create",
        description = "Create a new file with the given content. Fails if file already exists.",
        parameters = {
            path = { type = "string", required = true },
            content = { type = "string", required = true }
        },
        handler = function(args)
            local ok, err = permit("file_create", resolve_path(args.path))
            if not ok then return { ok = false, error = err } end
            local path = resolve_path(args.path)
            local f = io.open(path, "r")
            if f then
                f:close()
                return { ok = false, error = "file already exists: " .. path }
            end
            local cok, cerr = write_file(path, args.content)
            if not cok then
                return { ok = false, error = tostring(cerr) }
            end
            return { ok = true, created = path }
        end
    },
    {
        name = "file_delete",
        description = "Delete a file.",
        parameters = {
            path = { type = "string", required = true }
        },
        handler = function(args)
            local ok, err = permit("file_delete", resolve_path(args.path))
            if not ok then return { ok = false, error = err } end
            local path = resolve_path(args.path)
            local dok, derr = os.remove(path)
            if not dok then
                return { ok = false, error = tostring(derr) }
            end
            return { ok = true, deleted = path }
        end
    },
    {
        name = "file_list",
        description = "List files in a directory (non-recursive).",
        parameters = {
            path = { type = "string", required = true }
        },
        handler = function(args)
            local path = resolve_path(args.path)
            local cmd = string.format("ls -la %s 2>&1", cjson.encode(path))
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            local ok = f:close()
            return { ok = ok, output = truncate(out) }
        end
    },
    {
        name = "bash",
        description = "Run a shell command and return stdout/stderr.",
        parameters = {
            command = { type = "string", required = true },
            timeout = { type = "integer", required = false }
        },
        handler = function(args)
            local ok, err = permit("bash", args.command)
            if not ok then return { ok = false, error = err } end
            local cmd = args.command
            local timeout = args.timeout or 30
            local f = io.popen(string.format("timeout %d bash -c %s 2>&1",
                                             timeout, cjson.encode(cmd)))
            local out = f:read("*a") or ""
            local bok = f:close()
            return { ok = bok, output = truncate(out) }
        end
    },
    {
        name = "git",
        description = "Run a git command in the current project. Use for status, diff, add, commit, log, branch, etc.",
        parameters = {
            command = { type = "string", required = true },
            repo_path = { type = "string", required = false }
        },
        handler = function(args)
            local ok, err = permit("git", args.command)
            if not ok then return { ok = false, error = err } end
            local repo = resolve_path(args.repo_path) or "."
            local cmd = string.format("cd %s && git %s 2>&1", cjson.encode(repo), args.command)
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            local gok = f:close()
            return { ok = gok, output = truncate(out) }
        end
    },
    {
        name = "diff",
        description = "Show unified diff of current working tree changes (git diff) or between two files.",
        parameters = {
            repo_path = { type = "string", required = false },
            file_path = { type = "string", required = false }
        },
        handler = function(args)
            local repo = resolve_path(args.repo_path) or "."
            local file = args.file_path or ""
            local cmd
            if file ~= "" then
                cmd = string.format("cd %s && git diff -- %s 2>&1", cjson.encode(repo), cjson.encode(file))
            else
                cmd = string.format("cd %s && git diff 2>&1", cjson.encode(repo))
            end
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            local ok = f:close()
            return { ok = ok, output = truncate(out) }
        end
    },
    {
        name = "code_index",
        description = "Index a code repository into KV Cache using the coding.md toolchain.",
        parameters = {
            source = { type = "string", required = true },
            namespace = { type = "string", required = true },
            language = { type = "string", required = false }
        },
        handler = function(args)
            local src = args.source
            local ns = args.namespace
            local lang = args.language or "auto"
            local script = "analyze_repo.sh"
            if lang == "nodejs" or lang == "typescript" then
                script = "analyze_nodejs_repo.sh"
            end
            local cmd = string.format("cd /opt/my_db && ./%s %s %s 2>&1",
                                        script, cjson.encode(src), cjson.encode(ns))
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            local ok = f:close()
            return { ok = ok, output = truncate(out, 8000) }
        end
    }
}

-- Publish tool schemas to KV Cache ------------------------------------------

function M.register_tools(agent_ns)
    agent_ns = agent_ns or "/agent/default"
    if not agent_ns:match("/$") then agent_ns = agent_ns .. "/" end
    for _, t in ipairs(M.tools) do
        local schema = {
            t = "tool",
            name = t.name,
            description = t.description,
            parameters = finalize_schema(schema_from_params(t.parameters))
        }
        local key = agent_ns .. "tools/" .. t.name
        opencode.cache_set(key, cjson.encode(schema), 0)
    end
end

-- Build OpenAI-compatible tools array for LLM request
function M.build_tools_for_request()
    local out = {}
    for _, t in ipairs(M.tools) do
        table.insert(out, {
            type = "function",
            ["function"] = {
                name = t.name,
                description = t.description,
                parameters = finalize_schema(schema_from_params(t.parameters))
            }
        })
    end
    return out
end

-- Dispatch a tool call -------------------------------------------------------

function M.dispatch(tool_call)
    local name = tool_call.name
    local args = tool_call.arguments or tool_call.parameters or {}
    if type(args) == "string" then
        args = cjson.decode(args)
    end
    for _, t in ipairs(M.tools) do
        if t.name == name then
            local ok, res = pcall(t.handler, args)
            if not ok then
                return { ok = false, error = tostring(res) }
            end
            return res
        end
    end
    return { ok = false, error = "unknown tool: " .. tostring(name) }
end

return M

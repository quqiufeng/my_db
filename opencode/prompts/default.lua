-- opencode prompt assembly in Lua
-- C kernel exposes: opencode.cache_get, opencode.cache_set,
--                   opencode.cache_search_prefix, opencode.cache_search_tag,
--                   opencode.source_read, opencode.log_info
-- cjson is available via /usr/local/lualib/cjson.so

local cjson = require("cjson")

local M = {}

M.MAX_FACTS = 20
M.MAX_SUMMARIES = 5
M.MAX_SNIPPETS = 5

local function parse_json(s)
    if not s then return nil end
    local ok, v = pcall(cjson.decode, s)
    if ok then return v end
    return nil
end

local function json_escape(s)
    -- cjson handles escaping; we just encode the string
    return cjson.encode(s)
end

local function sort_by_importance_desc(a, b)
    local ia = (a.value and parse_json(a.value).i) or 0
    local ib = (b.value and parse_json(b.value).i) or 0
    return ia > ib
end

-- Read a project instruction file if it exists.
local function read_instruction_file(project_root, name)
    if not project_root then return nil end
    local candidates = {
        project_root .. "/" .. name,
        project_root .. "/.opencode/" .. name,
    }
    for _, path in ipairs(candidates) do
        local f = io.open(path, "r")
        if f then
            local content = f:read("*a")
            f:close()
            if content and content:match("%S") then
                return content, path
            end
        end
    end
    return nil
end

-- Build just the system/instruction part of the prompt (no user query).
function M.build_system_prompt(session_id, project_ns, project_root)
    project_root = project_root or "."
    local session_prefix = "/session/" .. session_id .. "/"
    local parts = {}

    local function push(fmt, ...)
        table.insert(parts, string.format(fmt, ...))
    end

    push("# System")
    push("You are an expert coding assistant running inside opencode CLI.")
    push("You have access to a local knowledge base (KV Cache) and code editing tools.")
    push("Current project root: %s", project_root)
    push("All relative file paths in tool calls are resolved against this project root.")
    push("Follow these rules to work efficiently:")
    push("- ALWAYS prefer `kv_search` over reading full files. Search first, read only the relevant snippets.")
    push("- Use `kv_context(symbol, repo)` when analyzing a specific function/class to get callers/callees.")
    push("- Use `read(path, offset, limit)` only when you need implementation details.")
    push("- Use `kv_get(key)` for exact keys you already know.")
    push("- Use `code_index(source, namespace)` to index a new repo or dependency before searching it.")
    push("- Use `edit(path, old_string, new_string)` to modify existing files. old_string must match exactly.")
    push("- Use `write(path, content)` to create or overwrite files; `apply_patch(patch)` for diff-based edits.")
    push("- Use `glob(pattern)` and `grep(pattern)` to explore the project.")
    push("- Use `bash(command)` for shell commands, `git(command)` for git operations, `diff(repo_path, file_path)` to review changes.")
    push("- KV Cache holds permanent memory: project architecture, indexed code, conversation facts, and errors.")
    push("- Third-party code is pre-indexed; do not ask the user to read it raw. Search `/code/` namespace instead.")
    push("- After making edits, consider running `diff` or `git status` to verify changes.")
    push("- Be concise. Only load information relevant to the current task.")

    -- Project instructions from AGENTS.md / instructions.md / claude.md
    local instruction_files = {"AGENTS.md", "instructions.md", "claude.md"}
    local found_any = false
    for _, name in ipairs(instruction_files) do
        local content, path = read_instruction_file(project_root, name)
        if content then
            if not found_any then
                push("\n# Project Instructions")
                found_any = true
            end
            push("\n## %s\n%s", path, content)
        end
    end

    -- Available tools
    local tools = opencode.cache_search_prefix("/agent/default/tools/", 32)
    if tools and #tools > 0 then
        push("\n## Available Tools")
        for _, t in ipairs(tools) do
            local tool = parse_json(t.value)
            if tool then
                push("- `%s`: %s", tool.name, tool.description or "")
                if tool.parameters then
                    push("  parameters: %s", cjson.encode(tool.parameters))
                end
            end
        end
    end

    -- Current task
    local task_json = opencode.cache_get(session_prefix .. "task/current")
    local task = task_json and parse_json(task_json)
    if task and task.c then
        push("\n## Current Task\n%s", task.c)
    end

    -- Key facts
    local facts = opencode.cache_search_prefix(session_prefix .. "facts/", M.MAX_FACTS)
    if facts and #facts > 0 then
        table.sort(facts, sort_by_importance_desc)
        push("\n## Key Facts")
        for i = 1, math.min(#facts, M.MAX_FACTS) do
            local f = parse_json(facts[i].value)
            if f and f.c then
                push("- %s", f.c)
            end
        end
    end

    -- Recent summaries
    local summaries = opencode.cache_search_prefix(session_prefix .. "summaries/", M.MAX_SUMMARIES)
    if summaries and #summaries > 0 then
        push("\n## Recent Conversation")
        for i = math.max(1, #summaries - M.MAX_SUMMARIES + 1), #summaries do
            local sm = parse_json(summaries[i].value)
            if sm and sm.c then
                push("- %s", sm.c)
            end
        end
    end

    return table.concat(parts, "\n")
end

M.build_prompt = nil  -- placeholder to be replaced below

-- Assemble a prompt for the given session and user query.
-- Returns the full prompt string.
function M.build_prompt(session_id, project_ns, user_query, project_root)
    local sys = M.build_system_prompt(session_id, project_ns, project_root or ".")
    return sys .. "\n\n## User\n" .. (user_query or "") .. "\n\n## Assistant\n"
end

-- Extract key facts from a turn and store them.
function M.save_facts(session_id, facts_array)
    local session_prefix = "/session/" .. session_id .. "/"
    local ts = tostring(os.time() * 1000)
    for i, f in ipairs(facts_array) do
        local key = session_prefix .. "facts/" .. ts .. "_" .. tostring(i)
        local value = {
            t = "fact",
            c = f.fact,
            i = f.importance or 3,
            tags = f.tags or {},
            ts = tonumber(ts)
        }
        opencode.cache_set(key, cjson.encode(value), 0)
    end
end

-- Store a turn summary.
function M.save_summary(session_id, turn_id, summary)
    local key = string.format("/session/%s/summaries/%08d", session_id, turn_id)
    local value = {
        t = "summary",
        c = summary,
        i = 4,
        ts = os.time() * 1000
    }
    opencode.cache_set(key, cjson.encode(value), 0)
end

return M

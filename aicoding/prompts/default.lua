-- opencode prompt assembly in Lua
-- C kernel exposes: opencode.cache_get, opencode.cache_set,
--                   opencode.cache_search_prefix, opencode.cache_search_tag,
--                   opencode.source_read, opencode.log_info
-- cjson is available via /usr/local/lualib/cjson.so

local cjson = require("cjson")
local compress = require("compress")

local M = {}

M.MAX_FACTS = 10
M.MAX_SUMMARIES = 3

local function parse_json(s)
    if not s then return nil end
    local ok, v = json.decode(s)
    if ok then return v end
    return nil
end

-- Read a project instruction file if it exists. Large files are truncated to avoid blowing up the prompt.
local MAX_INSTRUCTION_SIZE = 4096
local function read_instruction_file(project_root, name)
    if not project_root then return nil end
    local candidates = {
        project_root .. "/.opencode/" .. name,
        project_root .. "/" .. name,
    }
    for _, path in ipairs(candidates) do
        local f = io.open(path, "r")
        if f then
            local content = f:read("*a")
            f:close()
            if content and content:match("%S") then
                if #content > MAX_INSTRUCTION_SIZE then
                    content = content:sub(1, MAX_INSTRUCTION_SIZE) .. "\n\n[Instruction truncated from " .. tostring(#content) .. " chars to " .. tostring(MAX_INSTRUCTION_SIZE) .. " chars. Store details in KV Cache if needed.]"
                end
                return content, path
            end
        end
    end
    return nil
end

-- Build just the system/instruction part of the prompt (no user query).
function M.build_system_prompt(session_id, project_ns, project_root)
    project_root = project_root or "."
    local session_prefix = "/agent/" .. session_id .. "/"
    local parts = {}

    local function push(fmt, ...)
        table.insert(parts, string.format(fmt, ...))
    end

    push("# System")
    push("You are an expert coding assistant in opencode CLI.")
    push("Project root: %s | Session: %s", project_root, session_id)
    push("")
    push("## Memory-first context (IMPORTANT)")
    push("This agent has a FIXED-SIZE context window. Old messages are archived to KV Cache; nothing is lost.")
    push("You MUST use KV Cache as your long-term memory. Do not rely on the prompt for anything beyond the current few turns.")
    push("- WRITE facts/plans/errors with `kv_set(key, value, namespace='/agent/{session}/facts')`.")
    push("- RECALL them with `kv_search(query, namespace='/agent/{session}/facts')` or `kv_get(key)`.")
    push("- Search code with `kv_search(query, namespace='/code/local/{repo}', search_type='semantic')`.")
    push("- Get symbol context with `kv_context(symbol, repo)`.")
    push("- Read exact source with `read(path, offset, limit)`.")
    push("After reading code, store the minimum fact needed to avoid re-reading.")
    push("")
    push("## Tool capabilities")
    push("Memory/code: kv_search, kv_get, kv_set, kv_context, code_index, read, knowledge_read, knowledge_write, knowledge_search, trace_query.")
    push("Edit: edit, write, apply_patch, file_delete.")
    push("Explore: glob, grep.")
    push("Execute: bash, git, diff.")
    push("Extend: plugin_create(name, code), plugin_load(name) — add new tools at runtime without restart.")
    push("")
    push("## Plugin development")
    push("If a tool is missing, create a Lua plugin that returns a list of `{name, description, parameters, handler}` tables. Save it with plugin_create, then plugin_load. The tools become available immediately in the same conversation.")
    push("")
    push("## Workflow")
    push("1. Search first (`kv_search`), read second (`read`). Do not repeat the same search.")
    push("2. For specific functions, use `kv_context(symbol, repo)` to see callers/callees.")
    push("3. Index unknown repos with `code_index(source, namespace)` before searching.")
    push("4. Edit with exact `old_string`; verify with `diff` or `git status`.")
    push("5. Summarize progress periodically into `/agent/{session}/facts` and project knowledge with `knowledge_write`.")
    push("6. Be concise. Only load information relevant to the current task.")

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

    -- Project-level knowledge (shared across sessions)
    local knowledge = require("knowledge")
    local kfrag = knowledge.prompt_fragment(project_root)
    if kfrag and kfrag:match("%S") then
        push("\n# Project Knowledge")
        push(kfrag)
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
        table.sort(facts, function(a, b)
            local ia = (a.value and parse_json(a.value).i) or 0
            local ib = (b.value and parse_json(b.value).i) or 0
            return ia > ib
        end)
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
        push("\n## Recent Conversation Summary")
        for i = math.max(1, #summaries - M.MAX_SUMMARIES + 1), #summaries do
            local sm = parse_json(summaries[i].value)
            if sm and sm.c then
                push("- %s", sm.c)
            end
        end
    end

    local raw = table.concat(parts, "\n")

    -- Optional UTEL-style fixed-dictionary compression for very long system prompts.
    -- Controlled by OPENCODE_COMPRESS_PROMPT: 1=force, 0=disable, unset=auto (threshold 8k).
    local compress_env = os.getenv("OPENCODE_COMPRESS_PROMPT")
    local threshold = 8192
    if compress_env == "1" then
        threshold = 1
    elseif compress_env == "0" then
        return raw
    end
    local wrapped, stats = compress.wrap(raw, threshold)
    return wrapped
end

-- Extract key facts from a turn and store them.
function M.save_facts(session_id, facts_array)
    local session_prefix = "/agent/" .. session_id .. "/"
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
    local key = string.format("/agent/%s/summaries/%08d", session_id, turn_id)
    local value = {
        t = "summary",
        c = summary,
        i = 4,
        ts = os.time() * 1000
    }
    opencode.cache_set(key, cjson.encode(value), 0)
end

-- Full prompt for simple callers (e.g. C unit tests).
function M.build_prompt(session_id, project_ns, user_query, project_root)
    local system = M.build_system_prompt(session_id, project_ns, project_root)
    return system .. "\n\n# User\n" .. tostring(user_query or "")
end

return M

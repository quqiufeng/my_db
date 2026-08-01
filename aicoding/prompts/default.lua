-- opencode prompt assembly in Lua
-- C kernel exposes: opencode.cache_get, opencode.cache_set,
--                   opencode.cache_search_prefix, opencode.cache_search_tag,
--                   opencode.source_read, opencode.log_info
-- cjson is available via /usr/local/lualib/cjson.so

local cjson = require("cjson")
local json = require("json")
local compress = require("compress")
local conventions = require("conventions")

local M = {}

M.MAX_FACTS = 10
M.MAX_SUMMARIES = 3

local function parse_json(s)
    if not s then return nil end
    local v, _ = json.decode(s)
    return v
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
    local memory = require("memory")
    -- Memory is scoped to the project directory (all sessions share facts).
    local proj = memory.encode_project(project_root)
    local facts_ns = proj and ("/project/" .. proj .. "/facts")
                   or ("/agent/" .. session_id .. "/facts")
    local proj_prefix = proj and ("/project/" .. proj .. "/") or ("/agent/" .. session_id .. "/")
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
    push("- WRITE facts/plans/errors with `kv_set(key, value, namespace='%s')`.", facts_ns)
    push("- RECALL them with `kv_search(query, namespace='%s')` or `kv_get(key)`.", facts_ns)
    push("- Search code with `kv_search(query, namespace='/code/local/{repo}', search_type='semantic')`.")
    push("- Get symbol context with `kv_context(symbol, repo)`.")
    push("- Read exact source with `read(path, offset, limit)`.")
    push("After reading code, store the minimum fact needed to avoid re-reading.")
    push("")
    push("## Tool capabilities")
    push("Memory/code: kv_search, kv_get, kv_set, kv_context, code_index, read, knowledge_read, knowledge_write, knowledge_search, trace_query.")
    push("Edit: edit, write, apply_patch, file_delete.")
    push("Explore: glob, grep.")
    push("Execute: bash, git, diff, build.")
    push("Fetch: web_fetch(url) — fetch external docs/APIs/RFCs and summarize.")
    push("Undo/checkpoint: checkpoint_list, undo_last, rollback_to(checkpoint_id) — list checkpoints, undo the last destructive operation, or restore the whole project to a checkpoint.")
    push("Extend: plugin_create(name, code), plugin_load(name), plugin_list() — add new tools at runtime without restart.")
    push("")
    push("## Tool usage rules")
    push("- Use `build` to compile/test projects. It auto-detects cargo/npm/make/cmake/go/python and reports concise errors.")
    push("- Use `web_fetch` when the user references an external URL, RFC, API doc, or latest release notes.")
    push("- Use `plugin_create` + `plugin_load` when the existing tools cannot express an operation (e.g. domain-specific search). Prefer this over brittle `bash` scripts.")
    push("- Use `checkpoint_list`, `undo_last`, or `rollback_to` to recover from mistakes. A checkpoint is created automatically before every destructive edit.")
    push("- Use `trace_query` to inspect your own recent tool calls and outcomes when debugging. Structured trace events are written to `.opencode/traces/{session}.jsonl`.")
    push("- Use `kv_search` to recall earlier facts, summaries, or archived messages when you need context from previous turns.")
    push("")
    push("## Plugin development")
    push("If a tool is missing, create a Lua plugin that returns a list of `{name, description, parameters, handler}` tables. Save it with plugin_create, then plugin_load. The tools become available immediately in the same conversation. Use `plugin_list` to see already-loaded plugins.")
    push("")
    push("## Workflow")
    push("1. Search first (`kv_search`), read second (`read`). Do not repeat the same search.")
    push("2. For specific functions, use `kv_context(symbol, repo)` to see callers/callees.")
    push("3. Index unknown repos with `code_index(source, namespace)` before searching.")
    push("4. Edit with exact `old_string`; verify with `diff` or `git status`.")
    push("5. After editing, run `build` (or the project-specific test command) to verify.")
    push("6. Summarize progress periodically into `%s` and project knowledge with `knowledge_write`.", facts_ns)
    push("7. Be concise. Only load information relevant to the current task.")
    push("")
    push("## Editing rules")
    push("- Always `read(path)` before editing an existing file. Your `old_string` must match the current file content exactly.")
    push("- Use `edit` for small localized changes in existing files.")
    push("- Use `write` for new files or when completely replacing an existing file.")
    push("- Use `apply_patch` for multi-file changes or repetitive mechanical edits across many files.")
    push("- Never use `bash` with sed/awk/perl to edit files when `edit`/`write`/`apply_patch` can do the job.")
    push("- Store every important conclusion as a fact with `kv_set` so you do not need to re-read the same code.")
    push("")
    push("## Error handling and stopping")
    push("- If a tool returns `ok=false`, do not guess. Read the error, inspect state with `read`, `trace_query`, or `kv_search`, then choose a different approach.")
    push("- Do not repeat the exact same failed tool call more than once.")
    push("- When stuck after 2-3 attempts, summarize the blocker to the user and ask for clarification.")
    push("- Stop when the request is fulfilled and verified. End with a concise summary: what changed, which files, and how it was verified.")
    push("")
    push("## Safety and permissions")
    push("- Some paths are write-protected by default (system dirs and home root). If a write is denied, ask the user to confirm or run with explicit permissions.")
    push("- Obviously dangerous shell commands are blocked by default.")
    push("- Prefer targeted file edits over broad shell commands when the same result can be achieved with `edit`/`write`.")
    push("")
    push("## Project-specific conventions")
    push("The section below is auto-detected from the project type. It describes the recommended workflow, checklist, and common mistakes for this kind of project. Follow it unless the user gives conflicting instructions.")

    -- Project-type specific conventions (workflow, checklist, common mistakes)
    local conv_text, conv_type = conventions.load(project_root)
    if conv_text and conv_text:match("%S") then
        push("")
        push(conv_text)
    end

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
    local task_json = opencode.cache_get(proj_prefix .. "task/current")
    local task = task_json and parse_json(task_json)
    if type(task) == "table" and task.c then
        push("\n## Current Task\n%s", task.c)
    end

    -- Key facts
    local facts = opencode.cache_search_prefix(proj_prefix .. "facts/", M.MAX_FACTS)
    if facts and #facts > 0 then
        table.sort(facts, function(a, b)
            local va = a.value and parse_json(a.value)
            local vb = b.value and parse_json(b.value)
            local ia = (type(va) == "table" and va.i) or 0
            local ib = (type(vb) == "table" and vb.i) or 0
            return ia > ib
        end)
        push("\n## Key Facts")
        for i = 1, math.min(#facts, M.MAX_FACTS) do
            local f = parse_json(facts[i].value)
            -- Facts may be structured ({c, i, tags}) from auto-summary or a
            -- bare scalar written via kv_set. Guard against non-tables.
            if type(f) == "table" and f.c then
                push("- %s", f.c)
            elseif type(f) == "string" and f ~= "" then
                push("- %s", f)
            elseif type(f) == "number" or type(f) == "boolean" then
                push("- %s", tostring(f))
            end
        end
    end

    -- Recent summaries
    local summaries = opencode.cache_search_prefix(proj_prefix .. "summaries/", M.MAX_SUMMARIES)
    if summaries and #summaries > 0 then
        push("\n## Recent Conversation Summary")
        for i = math.max(1, #summaries - M.MAX_SUMMARIES + 1), #summaries do
            local sm = parse_json(summaries[i].value)
            if type(sm) == "table" and sm.c then
                push("- %s", sm.c)
            elseif type(sm) == "string" and sm ~= "" then
                push("- %s", sm)
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

-- Extract key facts from a turn and store them (project-scoped).
function M.save_facts(session_id, facts_array)
    local memory = require("memory")
    local proj = memory.project
    local prefix = proj and ("/project/" .. proj .. "/facts/") or ("/agent/" .. session_id .. "/facts/")
    local ts = tostring(os.time() * 1000)
    for i, f in ipairs(facts_array) do
        local key = prefix .. ts .. "_" .. tostring(i)
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

-- Store a turn summary (project-scoped).
function M.save_summary(session_id, turn_id, summary)
    local memory = require("memory")
    local proj = memory.project
    local key = proj
        and string.format("/project/%s/summaries/%08d", proj, turn_id)
        or string.format("/agent/%s/summaries/%08d", session_id, turn_id)
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

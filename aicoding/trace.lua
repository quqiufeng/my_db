-- trace.lua - structured agent trace logging and query
--
-- Writes a JSONL trace file under PROJECT_ROOT/.opencode/traces/{session}.jsonl.
-- Each line is a JSON object with: ts_ms, session, event, data.
-- The trace can be queried by event type, tool name, or time range.

local cjson = require("cjson")

local M = {}

M.session_id = "default"
M.project_root = "."

function M.init(session_id, project_root)
    M.session_id = session_id or "default"
    M.project_root = project_root or "."
end

local function shell_quote(s)
    return "'" .. tostring(s):gsub("'", "'\"'\"'") .. "'"
end

function M.trace_dir()
    return M.project_root .. "/.opencode/traces"
end

function M.trace_path()
    return M.trace_dir() .. "/" .. M.session_id .. ".jsonl"
end

function M.ensure_dir()
    os.execute("mkdir -p " .. shell_quote(M.trace_dir()))
end

-- Append a structured event to the trace file.
function M.log(event, data)
    M.ensure_dir()
    local entry = {
        ts_ms = math.floor((os.time() * 1000) + (os.clock() * 1000 % 1000)),
        session = M.session_id,
        event = event,
        data = data or {}
    }
    local line = cjson.encode(entry) .. "\n"
    local f = io.open(M.trace_path(), "a")
    if f then
        f:write(line)
        f:close()
    end
end

-- Read all trace lines from the session file.
function M.read_all()
    local path = M.trace_path()
    local f = io.open(path, "r")
    if not f then return {} end
    local lines = {}
    for line in f:lines() do
        local ok, entry = pcall(cjson.decode, line)
        if ok and type(entry) == "table" then
            table.insert(lines, entry)
        end
    end
    f:close()
    return lines
end

-- Query trace events.
-- opts:
--   event:   filter by event type string
--   tool:    filter tool_call/tool_result by tool name (checks data.name)
--   since:   minimum ts_ms
--   before:  maximum ts_ms
--   limit:   maximum results
--   reverse: return newest first
function M.query(opts)
    opts = opts or {}
    local entries = M.read_all()
    local out = {}
    for _, e in ipairs(entries) do
        if opts.event and e.event ~= opts.event then goto continue end
        if opts.since and (e.ts_ms or 0) < opts.since then goto continue end
        if opts.before and (e.ts_ms or 0) > opts.before then goto continue end
        if opts.tool and (e.data.name or "") ~= opts.tool then goto continue end
        table.insert(out, e)
        ::continue::
    end
    if opts.reverse then
        local rev = {}
        for i = #out, 1, -1 do
            table.insert(rev, out[i])
        end
        out = rev
    end
    if opts.limit and opts.limit > 0 then
        while #out > opts.limit do table.remove(out) end
    end
    return out
end

-- Convenience wrappers for common events.
function M.agent_start(task)
    M.log("agent_start", { task = task })
end

function M.llm_request(body_preview)
    M.log("llm_request", { body_preview = body_preview })
end

function M.llm_response(response_preview)
    M.log("llm_response", { response_preview = response_preview })
end

function M.tool_call(name, arguments)
    M.log("tool_call", { name = name, arguments = arguments })
end

function M.tool_result(name, result)
    M.log("tool_result", { name = name, result = result })
end

function M.context_archive(count)
    M.log("context_archive", { archived_count = count })
end

function M.error(msg, context)
    M.log("error", { message = msg, context = context })
end

return M

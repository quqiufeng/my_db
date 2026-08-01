-- subagent.lua - minimal sub-agent support: the `task` tool spawns a
-- child agent turn with an isolated context and returns its final text.
--
-- A subagent gets a compact system prompt (no full conversation history),
-- the delegated task, and calls the LLM once (non-streaming) to produce a
-- self-contained result. This mirrors opencode's subagent concept at a
-- fraction of the machinery; parallel execution can be layered on later
-- once the engine supports concurrent turns.

local json = require("json")
local log = require("log")

local M = {}

-- Compose a compact system prompt for a subagent turn.
local function subagent_system(project_root, mode)
    mode = mode or "general"
    local parts = {}
    table.insert(parts, "You are a focused sub-agent working inside the project at " .. tostring(project_root) .. ".")
    table.insert(parts, "You do NOT have the main conversation history. Complete the delegated task using the")
    table.insert(parts, "available tools (read, glob, grep, bash, git, kv_search, web_fetch, etc.).")
    table.insert(parts, "Return a concise, self-contained answer that the caller can use directly.")
    table.insert(parts, "If you need to inspect code, read the relevant files before answering.")
    table.insert(parts, "Do not ask the user questions; act autonomously.")
    if mode == "explore" then
        table.insert(parts, "You are an EXPLORE agent: find files, search code, and answer codebase questions quickly.")
        table.insert(parts, "Prefer glob/grep/kv_search over running builds. Report findings, not actions.")
    elseif mode == "plan" then
        table.insert(parts, "You are a PLAN agent: produce a step-by-step implementation plan.")
        table.insert(parts, "List concrete steps with the files/functions involved. Do not edit files.")
    end
    return table.concat(parts, "\n")
end

-- Run one subagent turn. Returns (ok, result_text) or (nil, error).
function M.run_task(task_text, opts)
    opts = opts or {}
    local project_root = opts.project_root or "."
    local mode = opts.mode or "general"

    local protocol = opencode.llm_protocol()
    local system = subagent_system(project_root, mode)

    local request_body = {}
    request_body.model = (opencode.get_model and opencode.get_model()) or "kimi-latest"
    request_body.temperature = 0.3
    request_body.messages = {
        { role = "system", content = system },
        { role = "user", content = tostring(task_text) },
    }
    local tools = require("tools.default")
    request_body.tools = tools.build_tools_for_request()

    local body_json = json.encode(request_body)
    if not body_json then return nil, "(encode error)" end

    local response = opencode.llm_complete_raw(body_json)
    if not response then return nil, "(no response)" end

    local ok, resp = pcall(json.decode, response)
    if not ok then return nil, "bad json: " .. tostring(resp) end
    if resp.error then
        return nil, "api error: " .. tostring(resp.error.message or json.encode(resp.error))
    end
    local choice = resp.choices and resp.choices[1]
    if not choice then return nil, "no choices" end
    local msg = choice.message
    local text = msg.content or ""
    local tool_calls = msg.tool_calls

    -- If the subagent issued tool calls, execute them and finish one more
    -- turn (bounded) to produce a final answer.
    if tool_calls and #tool_calls > 0 then
        local messages = request_body.messages
        table.insert(messages, {
            role = "assistant",
            content = text ~= "" and text or json.null,
            tool_calls = tool_calls,
        })
        for _, tc in ipairs(tool_calls) do
            local ok_exec, result = pcall(function()
                return tools.dispatch({ name = tc["function"].name, arguments = tc["function"].arguments and json.decode(tc["function"].arguments) or {} })
            end)
            table.insert(messages, {
                role = "tool",
                tool_call_id = tc.id,
                content = ok_exec and json.encode(result) or json.encode({ ok = false, error = tostring(result) }),
            })
        end
        request_body.messages = messages
        local body2 = json.encode(request_body)
        local resp2 = opencode.llm_complete_raw(body2)
        if resp2 then
            local ok2, dec2 = pcall(json.decode, resp2)
            if ok2 and dec2 and dec2.choices and dec2.choices[1] then
                text = dec2.choices[1].message.content or text
            end
        end
    end

    return true, text
end

return M

-- summarize.lua - automatic conversation summary + fact extraction
--
-- Called at the end of a turn/session. It can use a small LLM if available,
-- otherwise falls back to fast local heuristics. Extracted facts and summaries
-- are stored in KV Cache so future sessions can recall them.

local cjson = require("cjson")
local prompt = require("prompts.default")
local tokens = require("tokens")

local M = {}

-- Truncate long messages for transcript cost/speed.
local MAX_MSG_LEN = 1500

local function truncate(s, n)
    n = n or MAX_MSG_LEN
    s = tostring(s or "")
    if #s <= n then return s end
    return s:sub(1, n) .. "\n[...truncated]"
end

-- Estimate transcript token cost using shared estimator.
function M.estimate_transcript_tokens(messages)
    return tokens.estimate_messages(messages)
end

-- Build a compact transcript from the messages table.
function M.build_transcript(messages)
    local lines = {}
    for _, m in ipairs(messages or {}) do
        local role = m.role or "unknown"
        if role == "tool" then
            local parsed, _ = json.decode(m.content or "")
            if ok and type(parsed) == "table" then
                local name = parsed.tool or "tool"
                local status = parsed.ok and "OK" or "ERROR"
                table.insert(lines, string.format("[%s] %s: %s", role, name, status))
            else
                table.insert(lines, string.format("[%s] %s", role, truncate(m.content)))
            end
        elseif role == "assistant" and m.tool_calls then
            local names = {}
            for _, tc in ipairs(m.tool_calls or {}) do
                table.insert(names, tc["function"] and tc["function"].name or tc.name or "?")
            end
            local text = m.content or ""
            if #text > 0 then
                table.insert(lines, string.format("[%s] %s [tools: %s]", role, truncate(text), table.concat(names, ", ")))
            else
                table.insert(lines, string.format("[%s] [tools: %s]", role, table.concat(names, ", ")))
            end
        else
            local content = m.content or ""
            table.insert(lines, string.format("[%s] %s", role, truncate(content)))
        end
    end
    return table.concat(lines, "\n")
end

-- Heuristic summary + facts when no LLM is available.
function M.heuristic_summary(session_id, messages, reason)
    local transcript = M.build_transcript(messages)
    local first_user = ""
    local last_assistant = ""
    local tools_used = {}
    local errors = {}
    local explicit_facts = {}

    for _, m in ipairs(messages or {}) do
        if m.role == "user" and first_user == "" then
            first_user = tostring(m.content or ""):sub(1, 200)
        elseif m.role == "assistant" and m.content then
            last_assistant = tostring(m.content):sub(1, 400)
        elseif m.role == "tool" then
            local parsed, _ = json.decode(m.content or "")
            if ok and type(parsed) == "table" then
                if parsed.ok == false and parsed.error then
                    table.insert(errors, tostring(parsed.error):sub(1, 200))
                end
                if parsed.tool == "kv_set" and parsed.ok and parsed.key then
                    table.insert(explicit_facts, {
                        fact = string.format("Stored %s = %s", parsed.key, tostring(parsed.value or ""):sub(1, 200)),
                        importance = 3,
                        tags = {"memory"}
                    })
                end
            end
        elseif m.role == "assistant" and m.tool_calls then
            for _, tc in ipairs(m.tool_calls) do
                local name = tc["function"] and tc["function"].name or tc.name or "?"
                tools_used[name] = true
            end
        end
    end

    local tool_list = {}
    for name, _ in pairs(tools_used) do table.insert(tool_list, name) end
    table.sort(tool_list)

    local parts = {}
    table.insert(parts, string.format("Task: %s", first_user))
    if #tool_list > 0 then
        table.insert(parts, string.format("Tools used: %s", table.concat(tool_list, ", ")))
    end
    if last_assistant ~= "" then
        table.insert(parts, string.format("Outcome: %s", last_assistant))
    end
    if #errors > 0 then
        table.insert(parts, string.format("Errors: %s", table.concat(errors, "; ")))
    end
    if reason and reason ~= "" then
        table.insert(parts, string.format("End reason: %s", reason))
    end

    local summary = table.concat(parts, "\n")

    -- Build a few heuristic facts.
    local facts = {}
    if first_user ~= "" then
        table.insert(facts, { fact = "User asked: " .. first_user, importance = 2, tags = {"task"} })
    end
    if last_assistant ~= "" then
        table.insert(facts, { fact = "Assistant concluded: " .. last_assistant:sub(1, 300), importance = 3, tags = {"outcome"} })
    end
    for _, e in ipairs(errors) do
        table.insert(facts, { fact = "Error encountered: " .. e, importance = 4, tags = {"error"} })
    end
    for _, f in ipairs(explicit_facts) do
        table.insert(facts, f)
    end

    return summary, facts
end

-- Use a cheap LLM call to summarize and extract facts.
function M.llm_summary(session_id, messages, project_root)
    if not (os.getenv("OPENAI_API_KEY") or os.getenv("ANTHROPIC_API_KEY")) then
        return nil, "no API key configured"
    end
    if os.getenv("OPENCODE_AUTO_SUMMARIZE") == "0" then
        return nil, "OPENCODE_AUTO_SUMMARIZE=0"
    end

    local transcript = M.build_transcript(messages)
    if #transcript < 100 then
        return nil, "transcript too short"
    end

    local system = [[You are a summarization assistant. Read the conversation transcript and produce a concise JSON object with exactly this shape:
{
  "summary": "One-paragraph summary of what was asked, attempted, and the outcome.",
  "facts": [
    {"fact": "concrete fact or decision", "importance": 1-5, "tags": ["tag"]}
  ]
}
Focus on facts useful for future coding sessions: file paths, design decisions, errors, fixes, and TODOs.]]

    local user = "Transcript:\n" .. transcript .. "\n\nRespond ONLY with the JSON object."
    local body = {
        model = (opencode.get_model and opencode.get_model()) or "gpt-4o-mini",
        messages = {
            { role = "system", content = system },
            { role = "user", content = user }
        },
        temperature = 0.2,
        response_format = { type = "json_object" }
    }

    local ok, body_json = pcall(cjson.encode, body)
    if not ok then return nil, "json encode failed" end

    local resp = opencode.llm_complete_raw(body_json)
    if not resp or resp == "" then
        return nil, "llm request failed"
    end

    -- Try to extract JSON from markdown code block if needed.
    local json_text = resp
    local block = resp:match("```json\n(.-)\n```")
    if block then json_text = block end

    local parsed, _ = json.decode(json_text)
    if not parsed or type(parsed) ~= "table" then
        return nil, "llm response parse failed"
    end

    local facts = {}
    if type(parsed.facts) == "table" then
        for _, f in ipairs(parsed.facts) do
            if type(f) == "table" and f.fact then
                table.insert(facts, {
                    fact = tostring(f.fact),
                    importance = tonumber(f.importance) or 3,
                    tags = type(f.tags) == "table" and f.tags or {}
                })
            end
        end
    end

    return tostring(parsed.summary or "(no summary)"), facts
end

-- Main entry: save summary and facts for a session.
function M.extract(session_id, messages, project_root, reason)
    local summary, facts

    -- Prefer LLM summary if key is available and user did not disable it.
    local llm_summary, llm_facts, llm_err = M.llm_summary(session_id, messages, project_root)
    if llm_summary then
        summary = llm_summary
        facts = llm_facts or {}
    else
        summary, facts = M.heuristic_summary(session_id, messages, reason)
    end

    if not summary or summary == "" then
        return false, "empty summary"
    end

    local turn_id = os.time()
    prompt.save_summary(session_id, turn_id, summary)
    if facts and #facts > 0 then
        prompt.save_facts(session_id, facts)
    end

    log.info("[summarize] saved summary for session %s (turn %d, %d facts)", session_id, turn_id, #facts)
    return true, { summary = summary, facts = facts }
end

return M

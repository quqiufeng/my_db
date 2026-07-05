-- opencode main Lua runtime: prompt + tool registration + chat loop helpers

-- Global project root (set from C CLI via tools module)
local project_root = "."

-- Keep conversation history per session for end-of-turn summarization.
_G.session_messages = {}

function set_project_root(path)
    tools.set_project_root(path)
    project_root = tools.get_project_root()
    -- Sync memory session root if OPENCODE_SESSION was provided
    if os.getenv("OPENCODE_SESSION") then
        tools.set_session_id(os.getenv("OPENCODE_SESSION"))
    end
end

function get_project_root()
    project_root = tools.get_project_root()
    return project_root
end

local cjson  = require("cjson")
local json   = require("json")
local log    = require("log")
local prompt = require("prompts.default")
local tools  = require("tools.default")
local context = require("context")
local async_http = require("async_http")
local trace = require("trace")

-- Register tools into KV Cache so C kernel/prompt builder can discover them.
tools.register_tools("/agent/default")

function set_project_root(path)
    tools.set_project_root(path)
end

-- (GUI coroutine scheduler removed — UI handled by Zed Agent Panel via ACP)

-- Convert messages table to JSON array string for C API.
local function messages_to_json(messages)
    return json.encode(messages)
end

-- Extract assistant content, reasoning_content and tool_calls from OpenAI response.
local function parse_openai_response(resp_json)
    local resp, decode_err = json.decode(resp_json)
    if not resp then
        return nil, nil, "invalid json: " .. tostring(decode_err)
    end
    if resp.error then
        local msg = resp.error.message or resp.error.type or json.encode(resp.error)
        return nil, nil, "api error: " .. msg
    end
    local choice = resp.choices and resp.choices[1]
    if not choice then return nil, nil, "no choices in response" end
    local msg = choice.message
    local content = msg.content or ""
    local reasoning = msg.reasoning_content or ""
    local tool_calls = {}
    if msg.tool_calls then
        for _, tc in ipairs(msg.tool_calls) do
            if tc.type == "function" then
                table.insert(tool_calls, {
                    id = tc.id,
                    name = tc["function"].name,
                    arguments = json.decode(tc["function"].arguments)
                })
            end
        end
    end
    return content, reasoning, nil, tool_calls
end

-- Extract assistant content, reasoning_content and tool_uses from Anthropic response.
local function parse_anthropic_response(resp_json)
    local resp, decode_err = json.decode(resp_json)
    if not resp then
        return nil, nil, "invalid json: " .. tostring(decode_err)
    end
    if resp.error then
        local msg = resp.error.message or resp.error.type or json.encode(resp.error)
        return nil, nil, "api error: " .. msg
    end
    local content_parts = {}
    local reasoning_parts = {}
    local tool_calls = {}
    for _, block in ipairs(resp.content or {}) do
        if block.type == "text" then
            table.insert(content_parts, block.text)
        elseif block.type == "thinking" then
            table.insert(reasoning_parts, block.thinking)
        elseif block.type == "tool_use" then
            table.insert(tool_calls, {
                id = block.id,
                name = block.name,
                arguments = block.input
            })
        end
    end
    return table.concat(content_parts, ""), table.concat(reasoning_parts, ""), nil, tool_calls
end

-- Global entry points called from C ------------------------------------------

function build_prompt(session_id, project_ns, user_query)
    return prompt.build_prompt(session_id, project_ns, user_query, tools.get_project_root())
end


-- Append an assistant message (with optional reasoning and tool_calls) to messages.
local function append_assistant(messages, content, reasoning, tool_calls)
    local msg = { role = "assistant" }
    if content and content ~= "" then
        msg.content = content
    else
        msg.content = cjson.null
    end
    if reasoning and reasoning ~= "" then
        msg.reasoning_content = reasoning
    end
    if tool_calls and #tool_calls > 0 then
        msg.tool_calls = {}
        for _, tc in ipairs(tool_calls) do
            table.insert(msg.tool_calls, {
                id = tc.id,
                type = "function",
                ["function"] = {
                    name = tc.name,
                    arguments = json.encode(tc.arguments)
                }
            })
        end
    end
    table.insert(messages, msg)
end

-- Execute tool_calls and append tool result messages.
local function execute_tools(messages, tool_calls)
    for _, tc in ipairs(tool_calls) do
        log.info("executing tool: %s", tc.name)
        trace.tool_call(tc.name, tc.arguments)
        local result = tools.dispatch(tc)
        log.debug("tool result: %s", json.encode(result):sub(1, 300))
        trace.tool_result(tc.name, result)
        table.insert(messages, {
            role = "tool",
            tool_call_id = tc.id,
            content = json.encode(result)
        })
    end
end

function build_system_prompt(session_id, project_ns)
    return prompt.build_system_prompt(session_id, project_ns, tools.get_project_root())
end

-- Run the autonomous build-fix agent and return a human-readable summary.
-- Run the autonomous plan-and-execute agent and return a human-readable summary.
function run_plan_agent(session_id, project_ns, task_description)
    local plan_agent = require("agents.plan")
    local result = plan_agent.run(task_description or "complete the task", {
        session_id = session_id,
        project_ns = project_ns,
        verify_build = true,
        max_steps = 8,
    })
    if result.ok then
        return result.summary or "Plan completed."
    else
        return "Plan failed: " .. tostring(result.error or "unknown error") .. "\nCompleted steps:\n" .. json.encode(result.steps or {})
    end
end

function run_build_agent(session_id, project_ns, goal)
    local build_agent = require("agents.build")
    local result = build_agent.run(goal or "make the project compile", {
        session_id = session_id,
        project_ns = project_ns,
        workdir = tools.get_project_root(),
        max_attempts = 3,
    })
    if result.ok then
        return "Build succeeded after " .. tostring(result.attempts) .. " attempt(s).\nCommand: " .. tostring(result.command or "") .. "\nOutput:\n" .. tostring(result.output or "")
    else
        return "Build failed: " .. tostring(result.error or "unknown error")
    end
end

function chat_once(session_id, project_ns, user_query)
    local protocol = opencode.llm_protocol()
    local system = prompt.build_system_prompt(session_id, project_ns, tools.get_project_root())

    -- Start conversation with user message
    local messages = {
        { role = "user", content = user_query }
    }
    _G.session_messages[session_id] = messages
    trace.init(session_id, tools.get_project_root())
    trace.agent_start(user_query)

            local max_iterations = 8
    for i = 1, max_iterations do
        log.info("llm iteration %d", i)

        -- Apply sliding-window + LRU compression before sending
        local request_messages, archived = context.build_messages(session_id, system, messages)
        if archived and #archived > 0 then
            log.info("archived %d old messages to KV Cache", #archived)
            trace.context_archive(#archived)
        end

        -- Build full request body in Lua for easy debugging
        local request_body = {
            model = opencode.get_model and opencode.get_model() or "kimi-latest",
            messages = {},
            temperature = 1.0
        }
        if protocol ~= "anthropic" then
            request_body.messages = request_messages
            request_body.tools = tools.build_tools_for_request()
        else
            -- Anthropic: system is top-level, messages is just conversation
            request_body.system = system
            request_body.messages = {}
            for _, m in ipairs(request_messages) do
                if m.role ~= "system" then
                    table.insert(request_body.messages, m)
                end
            end
            request_body.tools = tools.build_tools_for_request()
        end

        local body_json = json.encode(request_body)
        if not body_json then
            return "(encode error)"
        end
        log.debug("request body (first 500): %s", body_json:sub(1, 500))
        trace.llm_request(body_json:sub(1, 500))

        local response = opencode.llm_complete_raw(body_json)
        if not response then
            log.warn("llm_complete_raw returned nil")
            trace.error("llm_complete_raw returned nil")
            return nil
        end
        log.debug("llm response: %s", response:sub(1, 200))
        trace.llm_response(response:sub(1, 500))

        local content, reasoning, err, tool_calls
        if protocol == "anthropic" then
            content, reasoning, err, tool_calls = parse_anthropic_response(response)
        else
            content, reasoning, err, tool_calls = parse_openai_response(response)
        end
        if err then
            log.error("parse error: %s", err)
            trace.error("response parse error", { error = err })
            return content or "(parse error)"
        end

        if not tool_calls or #tool_calls == 0 then
            return content or ""
        end

        -- Append assistant message with tool_calls and execute tools
        append_assistant(messages, content, reasoning, tool_calls)
        execute_tools(messages, tool_calls)
    end

    return "(too many tool iterations)"
end

function generate_summary(session_id, reason)
    local messages = _G.session_messages and _G.session_messages[session_id]
    if not messages or #messages == 0 then
        log.warn("generate_summary: no messages for session %s", session_id)
        return
    end
    local summarize = require("summarize")
    summarize.extract(session_id, messages, tools.get_project_root(), reason)
end

function handle_tool_call(tool_call_json)
    local call, decode_err = json.decode(tool_call_json)
    if not call then
        log.error("handle_tool_call: invalid JSON: %s", decode_err)
        trace.error("handle_tool_call invalid JSON", { error = decode_err })
        return json.encode({ ok = false, error = "invalid tool call JSON: " .. tostring(decode_err) })
    end
    trace.tool_call(call.name, call.arguments)
    local result = tools.dispatch(call)
    trace.tool_result(call.name, result)
    return json.encode(result)
end

function save_turn_summary(session_id, turn_id, summary)
    prompt.save_summary(session_id, turn_id, summary)
end

function save_facts(session_id, facts_json)
    local facts, decode_err = json.decode(facts_json)
    if not facts then
        log.error("save_facts: invalid JSON: %s", decode_err)
        trace.error("save_facts invalid JSON", { error = decode_err })
        return
    end
    trace.context_archive(#facts)
    prompt.save_facts(session_id, facts)
end

function run_gui(session_id, project_ns)
    -- GUI mode removed: UI is now handled by Zed's Agent Panel via ACP.
    -- Use `aicoding --acp` for Zed integration, or REPL mode (no --acp) for CLI.
    return "GUI mode removed. Use --acp for Zed integration."
end

-- ACP chat loop: like chat_once but streams ACP JSON-RPC notifications to stdout
-- Called from C when --acp flag is used (Zed integration)
function acp_chat(session_id, project_ns, user_query)
    local protocol = opencode.llm_protocol()
    local system = prompt.build_system_prompt(session_id, project_ns, tools.get_project_root())

    local messages = { { role = "user", content = user_query } }
    local max_iterations = 8
    local function acp(method, params) opencode.acp_send(method, cjson.encode(params)) end

    for i = 1, max_iterations do
        local request_messages, archived = context.build_messages(session_id, system, messages)

        local request_body = {}
        request_body.model = opencode.get_model and opencode.get_model() or "kimi-latest"
        request_body.messages = {}
        request_body.temperature = 1.0
        request_body.tools = tools.build_tools_for_request()

        if protocol ~= "anthropic" then
            for _, m in ipairs(request_messages) do request_body.messages[#request_body.messages+1] = m end
            request_body.tools = tools.build_tools_for_request()
        else
            for _, m in ipairs(request_messages) do
                if m.role ~= "system" then request_body.messages[#request_body.messages+1] = m end
            end
            request_body.system = system
            request_body.tools = tools.build_tools_for_request()
        end

        local body_json = cjson.encode(request_body)
        if not body_json then return "(encode error)" end

        local response = opencode.llm_complete_raw(body_json)
        if not response then return "(no response)" end

        local content, reasoning, err, tool_calls
        if protocol == "anthropic" then
            content, reasoning, err, tool_calls = parse_anthropic_response(response)
        else
            content, reasoning, err, tool_calls = parse_openai_response(response)
        end
        if err then return content or "(parse error)" end

        -- Stream text content via ACP
        if content and content ~= "" then
            acp("session/update", {
                sessionId = session_id,
                update = { sessionUpdate = "content", content = { type = "text", text = content } }
            })
        end

        if not tool_calls or #tool_calls == 0 then
            -- Store summary
            pcall(generate_summary, session_id, "end_turn")
            return ""
        end

        append_assistant(messages, content, reasoning, tool_calls)
        for _, tc in ipairs(tool_calls) do
            -- Report tool call via ACP
            acp("session/update", {
                sessionId = session_id,
                update = {
                    sessionUpdate = "tool_call",
                    toolCallId = tc.id,
                    title = tc.name,
                    kind = tc.name == "bash" and "execute" or tc.name == "read" and "read" or tc.name == "edit" and "edit" or "other",
                    status = "in_progress",
                    rawInput = tc.arguments
                }
            })

            local result = tools.dispatch(tc)
            local result_json = cjson.encode(result)

            -- Report tool result via ACP
            acp("session/update", {
                sessionId = session_id,
                update = {
                    sessionUpdate = "tool_call_update",
                    toolCallId = tc.id,
                    status = "completed",
                    rawOutput = result
                }
            })

            table.insert(messages, {
                role = "tool",
                tool_call_id = tc.id,
                content = result_json
            })
        end
    end

    pcall(generate_summary, session_id, "max_iterations")
    return "(too many tool iterations)"
end

function acp_run_plan(session_id, project_ns, task_description)
    local result = run_plan_agent(session_id, project_ns, task_description)
    return result or "(plan failed)"
end

function acp_run_build(session_id, project_ns, goal)
    local result = run_build_agent(session_id, project_ns, goal or "make the project compile")
    return result or "(build failed)"
end

-- ACP session tracking
local acp_sessions = {}

-- ACP JSON-RPC dispatcher: parses request with cjson, returns response string
-- Called from C cli.c --acp mode for each stdin line.
function acp_dispatch(request_json)
    local ok, req = pcall(cjson.decode, request_json)
    if not ok or type(req) ~= "table" then return nil end

    local id = req.id
    local method = req.method or ""
    local params = req.params or {}

    local function respond(result)
        return cjson.encode({ jsonrpc = "2.0", id = id, result = result })
    end
    local function respond_error(code, msg)
        return cjson.encode({ jsonrpc = "2.0", id = id, error = { code = code, message = msg } })
    end

    if method == "initialize" then
        return respond({
            protocolVersion = 1,
            agentInfo = { name = "aicoding", title = "aicoding Agent", version = "0.2.0" },
            agentCapabilities = {
                loadSession = false,
                promptCapabilities = { image = false, audio = false, embeddedContext = false },
                mcpCapabilities = { http = false, sse = false },
                sessionCapabilities = { list = true, close = true },
                auth = {}
            },
            authMethods = {}
        })

    elseif method == "session/new" then
        local new_sid = "sess_" .. tostring(math.floor(os.time() * 1000)) .. "_" .. tostring(math.random(0, 99999))
        local cwd = params.cwd or project_root
        acp_sessions[new_sid] = {
            cwd = cwd,
            created_at = os.time(),
            updated_at = os.time(),
            mode = "build"
        }
        return respond({ sessionId = new_sid, configOptions = cjson.null, modes = cjson.null })

    elseif method == "session/prompt" then
        local prompt_sid = params.sessionId or ""
        local user_text = ""
        if type(params.prompt) == "table" and #params.prompt > 0 then
            local first = params.prompt[1]
            user_text = first.text or (first.content and first.content.text) or ""
        end
        -- Track session activity
        if acp_sessions[prompt_sid] then
            acp_sessions[prompt_sid].updated_at = os.time()
        end
        local sess_project = (acp_sessions[prompt_sid] and acp_sessions[prompt_sid].cwd) or project_root
        local ok, err = pcall(acp_chat, prompt_sid, sess_project, user_text)
        if not ok then log.warn("acp_chat error: " .. tostring(err)) end
        return respond({ stopReason = "end_turn" })

    elseif method == "session/set_mode" then
        local prompt_sid = params.sessionId or ""
        local mode_id = params.modeId or "build"
        if acp_sessions[prompt_sid] then
            acp_sessions[prompt_sid].mode = mode_id
        end
        return respond({ status = "ok" })

    elseif method == "session/close" then
        local prompt_sid = params.sessionId or ""
        acp_sessions[prompt_sid] = nil
        return respond({})

    elseif method == "session/list" then
        local list = {}
        for sid, s in pairs(acp_sessions) do
            table.insert(list, {
                sessionId = sid,
                cwd = s.cwd,
                createdAt = s.created_at,
                updatedAt = s.updated_at,
                mode = s.mode
            })
        end
        table.sort(list, function(a, b) return a.createdAt > b.createdAt end)
        return respond({ sessions = list })

    elseif method == "session/cancel" then
        return nil  -- notification, no response

    else
        return respond_error(-32601, "Method not found: " .. method)
    end
end

log.info("opencode Lua runtime loaded")

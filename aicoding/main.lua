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

-- Pending coroutines resumed by the GUI tick callback.
local pending_coroutines = {}

function add_pending_coroutine(co)
    table.insert(pending_coroutines, co)
end

-- Called by the Rust GUI timer ~10Hz to resume sleeping Lua coroutines.
function gui_on_copy(text)
    if opencode.set_clipboard then
        local ok, err = opencode.set_clipboard(text)
        if ok then
            log.info("copied to clipboard (" .. tostring(#text) .. " bytes)")
        else
            log.warn("clipboard copy failed: " .. tostring(err))
        end
    else
        log.warn("set_clipboard not available")
    end
end

function gui_tick()
    local i = 1
    while i <= #pending_coroutines do
        local co = pending_coroutines[i]
        if coroutine.status(co) == "suspended" then
            local ok, err = coroutine.resume(co)
            if not ok then
                log.error("coroutine error: %s", err)
                table.remove(pending_coroutines, i)
            else
                i = i + 1
            end
        elseif coroutine.status(co) == "dead" then
            table.remove(pending_coroutines, i)
        else
            i = i + 1
        end
    end
end

-- Convert messages table to JSON array string for C API.
local function messages_to_json(messages)
    return cjson.encode(messages)
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

-- Convert messages table to JSON array string for C API.
local function messages_to_json(messages)
    return cjson.encode(messages)
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
                    arguments = cjson.encode(tc.arguments)
                }
            })
        end
    end
    table.insert(messages, msg)
end

-- Format a tool result table into a human-readable string for GUI display.
local function format_tool_result(name, result)
    if type(result) ~= "table" then
        return tostring(result)
    end
    local lines = {}
    if result.ok then
        table.insert(lines, "OK")
    else
        table.insert(lines, "ERROR: " .. tostring(result.error or "unknown"))
    end
    for k, v in pairs(result) do
        if k ~= "ok" and k ~= "error" then
            if type(v) == "table" then
                if k == "files" then
                    table.insert(lines, k .. ": " .. table.concat(v, ", "))
                else
                    table.insert(lines, k .. ": " .. cjson.encode(v))
                end
            else
                table.insert(lines, k .. ": " .. tostring(v))
            end
        end
    end
    return table.concat(lines, "\n")
end

-- Execute tool_calls and append tool result messages.
local function execute_tools(messages, tool_calls)
    for _, tc in ipairs(tool_calls) do
        log.info("executing tool: %s", tc.name)
        trace.tool_call(tc.name, tc.arguments)
        local result = tools.dispatch(tc)
        log.debug("tool result: %s", cjson.encode(result):sub(1, 300))
        trace.tool_result(tc.name, result)
        table.insert(messages, {
            role = "tool",
            tool_call_id = tc.id,
            content = cjson.encode(result)
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
        return "Plan failed: " .. tostring(result.error or "unknown error") .. "\nCompleted steps:\n" .. cjson.encode(result.steps or {})
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

        local body_json = cjson.encode(request_body)
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
    _G.gui_mode = true
    local gui = require("gui")
    local model_name = (opencode.get_model and opencode.get_model()) or "kimi-latest"
    local app = gui.create({
        title = "ai coding",
        project_root = tools.get_project_root(),
        model = model_name,
        version = "0.2.0",
    })

    -- Demo: append a welcome message after window opens
    gui.append_message(app, session_id, "assistant", "opencode GUI ready. Type a message and press Send.")

    gui.on_user_message(app, function(sid, text)
        gui.append_message(app, sid, "user", text)
        gui.clear_todos(app)
        trace.init(sid, tools.get_project_root())
        trace.agent_start(text)

        -- Agent command: /plan runs the autonomous plan-and-execute agent.
        if text:match("^/plan%s+(.+)") or text == "/plan" then
            local task = text:match("^/plan%s+(.+)") or "complete the current task"
            gui.add_todo(app, "Plan agent: " .. task)
            local co = coroutine.create(function()
                local summary = run_plan_agent(sid, project_ns, task)
                gui.append_message(app, sid, "assistant", summary)
                gui.add_todo(app, "Plan done")
                gui.set_todo_done(app, "Plan done", true)
            end)
            table.insert(pending_coroutines, co)
            coroutine.resume(co)
            return
        end

        -- Agent command: /build runs the autonomous build-fix agent.
        if text:match("^/build%s*(.*)") then
            local goal = text:match("^/build%s*(.*)")
            if goal == "" then goal = "make the project compile" end
            gui.add_todo(app, "Build agent: " .. goal)
            local co = coroutine.create(function()
                local summary = run_build_agent(sid, project_ns, goal)
                gui.append_message(app, sid, "assistant", summary)
                gui.add_todo(app, "Build done")
                gui.set_todo_done(app, "Build done", true)
            end)
            table.insert(pending_coroutines, co)
            coroutine.resume(co)
            return
        end

        gui.add_todo(app, "Plan approach for: " .. text)

        local co = coroutine.create(function()
            local protocol = opencode.llm_protocol()
            local system = prompt.build_system_prompt(sid, project_ns, tools.get_project_root())
            local messages = { { role = "user", content = text } }
    local max_iterations = 8

            for i = 1, max_iterations do
                log.info("gui llm iteration %d", i)
                gui.add_todo(app, "LLM iteration " .. tostring(i))

                -- Apply sliding-window + LRU compression before sending
                local request_messages, archived = context.build_messages(sid, system, messages)
                if archived and #archived > 0 then
                    log.info("gui archived %d old messages to KV Cache", #archived)
                    trace.context_archive(#archived)
                end

                local request_body = {
                    model = opencode.get_model and opencode.get_model() or "kimi-latest",
                    messages = {},
                    temperature = 1.0
                }
                if protocol ~= "anthropic" then
                    request_body.messages = request_messages
                    request_body.tools = tools.build_tools_for_request()
                else
                    request_body.system = system
                    request_body.messages = {}
                    for _, m in ipairs(request_messages) do
                        if m.role ~= "system" then
                            table.insert(request_body.messages, m)
                        end
                    end
                    request_body.tools = tools.build_tools_for_request()
                end

                local body_json = cjson.encode(request_body)
                trace.llm_request(body_json:sub(1, 500))

                -- Build URL and headers for async HTTP
                local base_url = os.getenv("OPENAI_BASE_URL") or "https://api.openai.com/v1"
                local url = base_url .. "/chat/completions"
                local api_key = os.getenv("OPENAI_API_KEY") or ""
                local headers = "Content-Type: application/json\r\nAuthorization: Bearer " .. api_key .. "\r\n"
                local user_agent = os.getenv("LLM_USER_AGENT")
                if user_agent then
                    headers = headers .. "User-Agent: " .. user_agent .. "\r\n"
                end
                local extra = os.getenv("LLM_EXTRA_HEADER")
                if extra then
                    local key, value = extra:match("^([^:]+):%s*(.+)$")
                    if key and value then
                        headers = headers .. key .. ": " .. value .. "\r\n"
                    else
                        headers = headers .. extra .. "\r\n"
                    end
                end

                local response, err = async_http.request(url, "POST", headers, body_json)
                if not response then
                    gui.append_message(app, sid, "assistant", "(no response: " .. tostring(err) .. ")")
                    trace.error("gui llm request failed", { error = err })
                    return
                end
                log.debug("llm response preview: %s", response:sub(1, 500))
                trace.llm_response(response:sub(1, 500))

                -- Report real token usage to GUI if available.
                local usage = response:match('"usage":(%b{})')
                if usage then
                    local prompt_tokens = usage:match('"prompt_tokens":(%d+)') or 0
                    local completion_tokens = usage:match('"completion_tokens":(%d+)') or 0
                    local total = usage:match('"total_tokens":(%d+)') or 0
                    if gui.set_tokens then
                        pcall(gui.set_tokens, app, tonumber(total), tonumber(prompt_tokens), tonumber(completion_tokens))
                    end
                end

                local content, reasoning, parse_err, tool_calls
                if protocol == "anthropic" then
                    content, reasoning, parse_err, tool_calls = parse_anthropic_response(response)
                else
                    content, reasoning, parse_err, tool_calls = parse_openai_response(response)
                end
                if parse_err then
                    gui.append_message(app, sid, "assistant", "(parse error: " .. tostring(parse_err) .. ")")
                    trace.error("gui response parse error", { error = parse_err })
                    return
                end

                -- Stream assistant content word-by-word into the GUI.
                if content and content ~= "" then
                    gui.append_message(app, sid, "assistant", "")
                    local chunk_size = 3
                    local pos = 1
                    while pos <= #content do
                        local next_pos = math.min(pos + chunk_size, #content + 1)
                        local chunk = content:sub(pos, next_pos - 1)
                        gui.stream_delta(app, sid, chunk)
                        pos = next_pos
                        coroutine.yield()
                    end
                end

                -- Display reasoning in a collapsible block (default collapsed).
                if reasoning and reasoning ~= "" then
                    gui.append_message(app, sid, "reasoning", reasoning)
                end

                if not tool_calls or #tool_calls == 0 then
                    gui.add_todo(app, "Done")
                    gui.set_todo_done(app, "Done", true)
                    return
                end

                append_assistant(messages, content, reasoning, tool_calls)
                for _, tc in ipairs(tool_calls) do
                    log.info("gui executing tool: %s", tc.name)
                    trace.tool_call(tc.name, tc.arguments)
                    gui.add_todo(app, "Run tool: " .. tc.name)
                    gui.tool_output(app, sid, tc.name, "running...")
                    -- Allow GUI to show the "running..." state briefly
                    coroutine.yield()
                    local result = tools.dispatch(tc)
                    log.debug("gui tool result: %s", cjson.encode(result):sub(1, 300))
                    trace.tool_result(tc.name, result)
                    gui.tool_output(app, sid, tc.name, format_tool_result(tc.name, result):sub(1, 16000))
                    gui.set_todo_done(app, "Run tool: " .. tc.name, true)
                    table.insert(messages, {
                        role = "tool",
                        tool_call_id = tc.id,
                        content = cjson.encode(result)
                    })
                end

                gui.set_todo_done(app, "LLM iteration " .. tostring(i), true)
            end
            gui.append_message(app, sid, "assistant", "(too many tool iterations)")
        end)

        table.insert(pending_coroutines, co)
        coroutine.resume(co)
    end)

    -- Automated GUI test script (optional)
    local test_script = os.getenv("OPENCODE_GUI_TEST_SCRIPT")
    if test_script then
        log.info("scheduling gui test script: %s", test_script)
        local test_co = coroutine.create(function()
            -- Wait briefly for GUI setup
            for _ = 1, 10 do coroutine.yield() end
            local test_env = {
                _G = _G,
                app = app,
                gui = gui,
                session_id = session_id,
                project_ns = project_ns,
                opencode = opencode,
                require = require,
                print = print,
                io = io,
                os = os,
                table = table,
                tostring = tostring,
                tonumber = tonumber,
                pcall = pcall,
                assert = assert,
                error = error,
                math = math,
                string = string,
                coroutine = coroutine,
            }
            local chunk, err = loadfile(test_script)
            if chunk then
                setfenv(chunk, test_env)
                local ok, res = pcall(chunk)
                if not ok then
                    log.error("gui test script error: %s", res)
                    print("TEST FAILED: " .. tostring(res))
                end
            else
                log.error("failed to load gui test script: %s", err)
                print("TEST FAILED: cannot load script: " .. tostring(err))
            end
        end)
        table.insert(pending_coroutines, test_co)
    end

    local code = gui.run(app)
    _G.gui_mode = false
    gui.free(app)
    return "gui exit code " .. tostring(code)
end

log.info("opencode Lua runtime loaded")

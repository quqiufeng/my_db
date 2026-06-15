-- opencode main Lua runtime: prompt + tool registration + chat loop helpers

-- Global project root (set from C CLI via tools module)
local project_root = "."

function set_project_root(path)
    tools.set_project_root(path)
    project_root = tools.get_project_root()
end

function get_project_root()
    project_root = tools.get_project_root()
    return project_root
end

local cjson  = require("cjson")
local prompt = require("prompts.default")
local tools  = require("tools.default")

-- Register tools into KV Cache so C kernel/prompt builder can discover them.
tools.register_tools("/agent/default")

function set_project_root(path)
    tools.set_project_root(path)
end

-- Local conversation state (per process)
local message_history = {}

-- Convert messages table to JSON array string for C API.
local function messages_to_json(messages)
    return cjson.encode(messages)
end

-- Extract assistant content and tool_calls from OpenAI response.
local function parse_openai_response(resp_json)
    local resp = cjson.decode(resp_json)
    local choice = resp.choices and resp.choices[1]
    if not choice then return nil, "no choices in response" end
    local msg = choice.message
    local content = msg.content or ""
    local tool_calls = {}
    if msg.tool_calls then
        for _, tc in ipairs(msg.tool_calls) do
            if tc.type == "function" then
                table.insert(tool_calls, {
                    id = tc.id,
                    name = tc["function"].name,
                    arguments = cjson.decode(tc["function"].arguments)
                })
            end
        end
    end
    return content, nil, tool_calls
end

-- Extract assistant content and tool_uses from Anthropic response.
local function parse_anthropic_response(resp_json)
    local resp = cjson.decode(resp_json)
    local content_parts = {}
    local tool_calls = {}
    for _, block in ipairs(resp.content or {}) do
        if block.type == "text" then
            table.insert(content_parts, block.text)
        elseif block.type == "tool_use" then
            table.insert(tool_calls, {
                id = block.id,
                name = block.name,
                arguments = block.input
            })
        end
    end
    return table.concat(content_parts, ""), nil, tool_calls
end

-- Global entry points called from C ------------------------------------------

function build_prompt(session_id, project_ns, user_query)
    return prompt.build_prompt(session_id, project_ns, user_query)
end

-- Convert messages table to JSON array string for C API.
local function messages_to_json(messages)
    return cjson.encode(messages)
end

-- Append an assistant message (with optional tool_calls) to messages.
local function append_assistant(messages, content, tool_calls)
    local msg = { role = "assistant" }
    if content and content ~= "" then
        msg.content = content
    else
        msg.content = cjson.null
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

-- Execute tool_calls and append tool result messages.
local function execute_tools(messages, tool_calls)
    for _, tc in ipairs(tool_calls) do
        opencode.log_info("executing tool: " .. tc.name)
        local result = tools.dispatch(tc)
        opencode.log_info("tool result: " .. cjson.encode(result):sub(1, 300))
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

function chat_once(session_id, project_ns, user_query)
    local protocol = opencode.llm_protocol()
    local system = prompt.build_system_prompt(session_id, project_ns, tools.get_project_root())

    -- Start conversation with user message
    local messages = {
        { role = "user", content = user_query }
    }

    local max_iterations = 5
    for i = 1, max_iterations do
        opencode.log_info("llm iteration " .. tostring(i))

        -- Build full request body in Lua for easy debugging
        local request_body = {
            model = opencode.get_model and opencode.get_model() or "kimi-latest",
            messages = {},
            temperature = 1.0
        }
        if protocol ~= "anthropic" then
            request_body.messages = {
                { role = "system", content = system }
            }
            for _, m in ipairs(messages) do
                table.insert(request_body.messages, m)
            end
            request_body.tools = tools.build_tools_for_request()
        else
            -- Anthropic: system is top-level, messages is just conversation
            request_body.system = system
            request_body.messages = messages
            request_body.tools = tools.build_tools_for_request()
        end

        local body_json = cjson.encode(request_body)
        opencode.log_info("request body (first 500): " .. body_json:sub(1, 500))

        local response = opencode.llm_complete_raw(body_json)
        if not response then
            opencode.log_info("llm_complete_raw returned nil")
            return nil
        end
        opencode.log_info("llm response: " .. response:sub(1, 200))

        local content, err, tool_calls
        if protocol == "anthropic" then
            content, err, tool_calls = parse_anthropic_response(response)
        else
            content, err, tool_calls = parse_openai_response(response)
        end
        if err then
            opencode.log_info("parse error: " .. err)
            return content or "(parse error)"
        end

        if not tool_calls or #tool_calls == 0 then
            return content or ""
        end

        -- Append assistant message with tool_calls and execute tools
        append_assistant(messages, content, tool_calls)
        execute_tools(messages, tool_calls)
    end

    return "(too many tool iterations)"
end

function generate_summary(session_id, text)
    -- In a real implementation, call a small LLM to summarize.
    -- For now, just store a stub summary.
    prompt.save_summary(session_id, os.time(), text:sub(1, 200))
end

function handle_tool_call(tool_call_json)
    local call = cjson.decode(tool_call_json)
    local result = tools.dispatch(call)
    return cjson.encode(result)
end

function save_turn_summary(session_id, turn_id, summary)
    prompt.save_summary(session_id, turn_id, summary)
end

function save_facts(session_id, facts_json)
    local facts = cjson.decode(facts_json)
    prompt.save_facts(session_id, facts)
end

function run_gui(session_id, project_ns)
    local gui = require("gui")
    local app = gui.create({ title = "opencode", project_root = tools.get_project_root() })

    -- Demo: append a welcome message after window opens
    gui.append_message(app, session_id, "assistant", "opencode GUI ready. Type a message and press Send.")

    gui.on_user_message(app, function(sid, text)
        gui.append_message(app, sid, "user", text)

        local co = coroutine.create(function()
            local protocol = opencode.llm_protocol()
            local system = prompt.build_system_prompt(sid, project_ns, tools.get_project_root())
            local messages = { { role = "user", content = text } }
            local max_iterations = 5

            for i = 1, max_iterations do
                opencode.log_info("gui llm iteration " .. tostring(i))
                local request_body = {
                    model = opencode.get_model and opencode.get_model() or "kimi-latest",
                    messages = {},
                    temperature = 1.0
                }
                if protocol ~= "anthropic" then
                    request_body.messages = { { role = "system", content = system } }
                    for _, m in ipairs(messages) do table.insert(request_body.messages, m) end
                    request_body.tools = tools.build_tools_for_request()
                else
                    request_body.system = system
                    request_body.messages = messages
                    request_body.tools = tools.build_tools_for_request()
                end

                local body_json = cjson.encode(request_body)
                local response = opencode.llm_complete_raw(body_json)
                if not response then
                    gui.append_message(app, sid, "assistant", "(no response)")
                    return
                end

                local content, err, tool_calls
                if protocol == "anthropic" then
                    content, err, tool_calls = parse_anthropic_response(response)
                else
                    content, err, tool_calls = parse_openai_response(response)
                end
                if err then
                    gui.append_message(app, sid, "assistant", content or "(parse error)")
                    return
                end

                if not tool_calls or #tool_calls == 0 then
                    if content and content ~= "" then
                        gui.append_message(app, sid, "assistant", content)
                    end
                    return
                end

                append_assistant(messages, content, tool_calls)
                for _, tc in ipairs(tool_calls) do
                    opencode.log_info("gui executing tool: " .. tc.name)
                    gui.tool_output(app, sid, tc.name, "running...")
                    local result = tools.dispatch(tc)
                    opencode.log_info("gui tool result: " .. cjson.encode(result):sub(1, 300))
                    gui.tool_output(app, sid, tc.name, cjson.encode(result):sub(1, 800))
                    table.insert(messages, {
                        role = "tool",
                        tool_call_id = tc.id,
                        content = cjson.encode(result)
                    })
                end
            end
            gui.append_message(app, sid, "assistant", "(too many tool iterations)")
        end)

        coroutine.resume(co)
    end)

    local code = gui.run(app)
    gui.free(app)
    return "gui exit code " .. tostring(code)
end

opencode.log_info("opencode Lua runtime loaded")

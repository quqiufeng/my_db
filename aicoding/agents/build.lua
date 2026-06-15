-- agents/build.lua
-- Autonomous build-fix agent prototype.
--
-- Usage:
--   local build_agent = require("agents.build")
--   local result = build_agent.run("make the project compile", {
--       session_id = session_id,
--       project_ns = project_ns,
--       workdir = "/path/to/project",
--       max_attempts = 3,
--   })
--
-- The agent repeatedly calls the `build` tool. On failure it asks the LLM to
-- diagnose and fix the error, then retries. All intermediate results are
-- written to KV Cache so the main loop can inspect them.

local cjson = require("cjson")
local tools = require("tools.default")
local memory = require("memory")

local M = {}

local function kv_key(session_id, suffix)
    return string.format("/agent/%s/build/%s", session_id or "default", suffix)
end

local function log(session_id, msg)
    local line = "[BUILD AGENT] " .. tostring(msg)
    print(line)
    -- Keep a lightweight log in KV Cache for cross-turn inspection.
    local key = kv_key(session_id, "log")
    local existing = memory.read(key) or ""
    memory.write(key, existing .. os.date("%H:%M:%S ") .. msg .. "\n", 3600)
end

-- Run one build attempt and return the tool result table.
local function run_build(workdir, timeout)
    return tools.dispatch({
        name = "build",
        arguments = cjson.encode({
            workdir = workdir,
            timeout = timeout,
            max_output_lines = 120,
        })
    })
end

-- Ask the main chat loop to diagnose and fix build errors.
-- We reuse chat_once from main.lua via a global that main.lua exports.
local function request_fix(session_id, project_ns, goal, attempt, build_output, workdir)
    local prompt = string.format(
        "You are a build-fix agent.\n" ..
        "Goal: %s\n" ..
        "Working directory: %s\n" ..
        "Attempt: %d\n\n" ..
        "The latest build failed with this output:\n```\n%s\n```\n\n" ..
        "Read the relevant files, apply the minimal fix, and then stop. " ..
        "Do not run the build tool yourself; the outer loop will verify.",
        goal, workdir or ".", attempt, build_output
    )
    -- main.lua exposes chat_once globally.
    if not _G.chat_once then
        return nil, "chat_once not available"
    end
    local response = chat_once(session_id, project_ns, prompt)
    return response
end

function M.run(goal, opts)
    opts = opts or {}
    local session_id = opts.session_id or "default"
    local project_ns = opts.project_ns or session_id
    local workdir = opts.workdir or tools.get_project_root()
    local max_attempts = opts.max_attempts or 3
    local timeout = opts.timeout or 120

    log(session_id, "starting build agent: " .. tostring(goal))
    memory.write(kv_key(session_id, "status"), cjson.encode({
        goal = goal,
        workdir = workdir,
        state = "running",
        started_at = os.time(),
    }), 3600)

    for attempt = 1, max_attempts do
        log(session_id, "build attempt " .. tostring(attempt))
        local result = run_build(workdir, timeout)
        memory.write(kv_key(session_id, "attempt/" .. tostring(attempt)), cjson.encode({
            attempt = attempt,
            ok = result.ok,
            output = result.output,
            command = result.command,
            exit_code = result.exit_code,
            timestamp = os.time(),
        }), 3600)

        if result.ok then
            log(session_id, "build succeeded on attempt " .. tostring(attempt))
            memory.write(kv_key(session_id, "status"), cjson.encode({
                goal = goal,
                workdir = workdir,
                state = "success",
                attempts = attempt,
                finished_at = os.time(),
            }), 3600)
            return {
                ok = true,
                attempts = attempt,
                command = result.command,
                output = result.output,
            }
        end

        log(session_id, "build failed, asking LLM for fix")
        if attempt == max_attempts then
            break
        end
        local fix_response, fix_err = request_fix(session_id, project_ns, goal, attempt, result.output or "", workdir)
        if not fix_response then
            log(session_id, "failed to get fix from LLM: " .. tostring(fix_err))
            break
        end
        log(session_id, "LLM fix response: " .. fix_response:gsub("\n", " "):sub(1, 200))
    end

    log(session_id, "build agent gave up after " .. tostring(max_attempts) .. " attempts")
    memory.write(kv_key(session_id, "status"), cjson.encode({
        goal = goal,
        workdir = workdir,
        state = "failed",
        attempts = max_attempts,
        finished_at = os.time(),
    }), 3600)
    return {
        ok = false,
        attempts = max_attempts,
        error = "build did not succeed after " .. tostring(max_attempts) .. " attempts",
    }
end

return M

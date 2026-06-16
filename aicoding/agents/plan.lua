-- agents/plan.lua
-- Autonomous plan-and-execute agent.
--
-- Usage:
--   local plan_agent = require("agents.plan")
--   local result = plan_agent.run("implement a greeting function and verify it builds", {
--       session_id = session_id,
--       project_ns = project_ns,
--       verify_build = true,
--       max_steps = 8,
--   })
--
-- The agent asks the LLM to produce a JSON plan, executes each step via the
-- main chat_once loop, optionally verifies with the build agent, and re-plans
-- on failure. The plan and step results are persisted to KV Cache.

local json = require("json")
local tools = require("tools.default")
local memory = require("memory")

local M = {}

local function kv_key(session_id, suffix)
    return string.format("/agent/%s/plan/%s", session_id or "default", suffix)
end

local function log(session_id, msg)
    local line = "[PLAN AGENT] " .. tostring(msg)
    print(line)
    local key = kv_key(session_id, "log")
    local existing = memory.read(key) or ""
    memory.write(key, existing .. os.date("%H:%M:%S ") .. msg .. "\n", 3600)
end

-- Extract the outermost JSON object/array from a string that may contain
-- markdown fences or surrounding text.
local function extract_json(text)
    if not text then return nil end
    -- Try fenced code block first.
    local fenced = text:match("```json\n(.-)\n```") or text:match("```\n(.-)\n```")
    if fenced then
        local val, _ = json.decode(fenced)
            if val then return val end
    end
    -- Fall back to first { ... } or [ ... ] span.
    local first_brace = text:find("[%[{]")
    if not first_brace then return nil end
    local depth = 0
    local in_string = false
    local escape = false
    local start_char = text:sub(first_brace, first_brace)
    local end_char = start_char == "{" and "}" or "]"
    for i = first_brace, #text do
        local ch = text:sub(i, i)
        if in_string then
            if escape then
                escape = false
            elseif ch == "\\" then
                escape = true
            elseif ch == '"' then
                in_string = false
            end
        else
            if ch == '"' then
                in_string = true
            elseif ch == start_char then
                depth = depth + 1
            elseif ch == end_char then
                depth = depth - 1
                if depth == 0 then
                    local val, _ = json.decode(text:sub(first_brace, i))
            if val then return val end
                end
            end
        end
    end
    return nil
end

-- Ask the LLM to create or revise a plan.
local function generate_plan(session_id, project_ns, task, history, failed_step)
    local context_info = ""
    if history and #history > 0 then
        local lines = { "Steps completed so far:" }
        for _, h in ipairs(history) do
            table.insert(lines, string.format("- %s: %s", h.step, h.summary or "no summary"))
        end
        context_info = table.concat(lines, "\n") .. "\n\n"
    end

    local failure_info = ""
    if failed_step then
        failure_info = string.format(
            "The previous step failed:\nStep: %s\nError: %s\n\n" ..
            "Please revise the remaining plan to recover from this failure. " ..
            "Keep already-completed steps out of the new plan.\n\n",
            failed_step.description or "unknown",
            failed_step.error or "unknown error"
        )
    end

    local prompt = string.format(
        "You are a planning agent. Given the task below, produce a concise, actionable plan as a JSON array of steps.\n" ..
        "Each step must be an object with:\n" ..
        "  - description: a short, clear instruction for that step\n" ..
        "  - depends_on: an array of step indices this step depends on (0-based, can be empty)\n" ..
        "  - verify: optional boolean, true if this step should be followed by a build/test verification\n" ..
        "\n%s%sTask: %s\n\n" ..
        "Respond ONLY with the JSON array. Do not include markdown fences or explanations.",
        context_info, failure_info, task
    )

    local response = _G.chat_once(session_id, project_ns, prompt)
    if not response then
        return nil, "LLM did not return a plan"
    end
    local plan = extract_json(response)
    if not plan then
        return nil, "could not parse plan JSON from: " .. response:sub(1, 200)
    end
    if type(plan) ~= "table" or #plan == 0 then
        return nil, "plan is empty or not an array"
    end
    return plan
end

-- Execute a single plan step via the main chat loop.
local function execute_step(session_id, project_ns, step, step_idx, prior_summaries)
    local ctx = ""
    if prior_summaries and #prior_summaries > 0 then
        ctx = "Context from previous steps:\n"
        for _, s in ipairs(prior_summaries) do
            ctx = ctx .. string.format("- %s: %s\n", s.step, s.summary)
        end
        ctx = ctx .. "\n"
    end

    local prompt = string.format(
        "%sExecute this step of the plan and nothing else.\n" ..
        "Step %d: %s\n\n" ..
        "When done, summarize what you changed in one sentence.",
        ctx, step_idx + 1, step.description or ""
    )

    local response = _G.chat_once(session_id, project_ns, prompt)
    return response or "(no response)"
end

-- Build a dependency-ready execution order from the plan array.
local function topological_order(plan)
    local completed = {}
    local order = {}
    local remaining = {}
    for i, _ in ipairs(plan) do
        remaining[i] = true
    end

    while next(remaining) do
        local progress = false
        for i, _ in pairs(remaining) do
            local deps = plan[i].depends_on or {}
            local ready = true
            for _, d in ipairs(deps) do
                if not completed[d + 1] then
                    ready = false
                    break
                end
            end
            if ready then
                table.insert(order, i)
                completed[i] = true
                remaining[i] = nil
                progress = true
            end
        end
        if not progress then
            return nil, "circular or unresolvable dependencies in plan"
        end
    end
    return order
end

function M.run(task_description, opts)
    opts = opts or {}
    local session_id = opts.session_id or "default"
    local project_ns = opts.project_ns or session_id
    local verify_build = opts.verify_build ~= false  -- default true
    local max_steps = opts.max_steps or 8

    log(session_id, "starting plan agent: " .. tostring(task_description))
    memory.write(kv_key(session_id, "status"), json.encode({
        task = task_description,
        state = "planning",
        started_at = os.time(),
    }), 3600)

    local plan, err = generate_plan(session_id, project_ns, task_description)
    if not plan then
        log(session_id, "failed to generate plan: " .. tostring(err))
        return { ok = false, error = err }
    end

    log(session_id, "generated plan with " .. tostring(#plan) .. " steps")
    memory.write(kv_key(session_id, "plan"), json.encode(plan), 3600)

    local order, order_err = topological_order(plan)
    if not order then
        log(session_id, "plan dependency error: " .. tostring(order_err))
        return { ok = false, error = order_err }
    end

    local step_results = {}
    local prior_summaries = {}
    local failed_step = nil
    local attempts = 0

    for _, idx in ipairs(order) do
        if #step_results >= max_steps then
            log(session_id, "reached max_steps limit")
            break
        end
        attempts = attempts + 1
        local step = plan[idx]
        log(session_id, "executing step " .. tostring(idx) .. ": " .. tostring(step.description))

        local summary = execute_step(session_id, project_ns, step, idx, prior_summaries)
        table.insert(prior_summaries, { step = "step " .. tostring(idx), summary = summary })

        local step_record = {
            step_index = idx,
            description = step.description,
            summary = summary,
            timestamp = os.time(),
        }

        local build_ok = true
        local build_output = ""
        if verify_build and step.verify then
            log(session_id, "verifying step with build agent")
            local build_agent = require("agents.build")
            local build_result = build_agent.run("verify the project still compiles", {
                session_id = session_id,
                project_ns = project_ns,
                workdir = tools.get_project_root(),
                max_attempts = 2,
            })
            build_ok = build_result.ok
            build_output = build_result.output or ""
            step_record.build_result = build_result
        end

        table.insert(step_results, step_record)
        memory.write(kv_key(session_id, "step/" .. tostring(idx)), json.encode(step_record), 3600)

        if not build_ok then
            log(session_id, "build verification failed for step " .. tostring(idx))
            failed_step = {
                description = step.description,
                error = "build verification failed: " .. tostring(build_output),
            }
            -- Attempt one re-plan.
            log(session_id, "re-planning after failure")
            local new_plan, new_err = generate_plan(session_id, project_ns, task_description, prior_summaries, failed_step)
            if not new_plan then
                log(session_id, "re-plan failed: " .. tostring(new_err))
                break
            end
            plan = new_plan
            memory.write(kv_key(session_id, "plan"), json.encode(plan), 3600)
            order, order_err = topological_order(plan)
            if not order then
                log(session_id, "re-plan dependency error: " .. tostring(order_err))
                break
            end
            -- Reset execution from the new plan, preserving history.
            prior_summaries = {}
            for _, sr in ipairs(step_results) do
                table.insert(prior_summaries, { step = "step " .. tostring(sr.step_index), summary = sr.summary })
            end
            failed_step = nil
        end
    end

    local final_status = failed_step and "failed" or "success"
    memory.write(kv_key(session_id, "status"), json.encode({
        task = task_description,
        state = final_status,
        steps_completed = #step_results,
        finished_at = os.time(),
    }), 3600)

    if failed_step then
        return {
            ok = false,
            error = failed_step.error,
            steps = step_results,
        }
    end

    local lines = { "Plan completed successfully. Steps:" }
    for _, sr in ipairs(step_results) do
        table.insert(lines, string.format("%d. %s -> %s", sr.step_index, sr.description, sr.summary or "done"))
    end
    return {
        ok = true,
        summary = table.concat(lines, "\n"),
        steps = step_results,
    }
end

return M

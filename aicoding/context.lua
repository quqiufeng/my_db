-- context.lua - 管理发送给 LLM 的上下文窗口
--
-- 设计原则：
--   1. LLM API 的上下文永远有一个硬上限（由 OPENCODE_CONTEXT_TOKENS 控制，默认 16k）。
--   2. 系统提示固定精简，只保留当前任务必须知道的规则。
--   3. messages 保留最近 N 轮；更老的对话按 LRU/时间归档到 KV Cache。
--   4. 任何被移出窗口的事实都可以从 KV Cache 通过 kv_search/kv_get 重新召回。
--   5. 模型读代码后，应主动用 kv_set 把关键结论写回记忆。

local memory = require("memory")
local tokens = require("tokens")
local json = require("json")

local M = {}

-- 默认上下文 token 上限，可由环境变量覆盖
M.MAX_TOKENS = tonumber(os.getenv("OPENCODE_CONTEXT_TOKENS")) or 65536

-- 系统提示允许占用的比例（剩余给 messages）
M.SYSTEM_PROMPT_RATIO = 0.35

-- 最近 N 轮对话必须保留，除非超出硬上限
M.MIN_RECENT_TURNS = 4

-- Estimate tokens using the shared estimator.
local function estimate_tokens(text)
    return tokens.estimate(text)
end

-- 从 messages 数组估算总 token
function M.count_messages(messages)
    return tokens.estimate_messages(messages)
end

-- 把一条 message 归档为记忆
-- 返回归档后的 summary key
local function archive_message(session, msg, idx)
    local ns = "/agent/" .. session .. "/history"
    if msg.role == "tool" then
        -- tool 输出太长发 summary 键，原始内容放 memory
        local key = ns .. "/tool_" .. tostring(idx) .. "_" .. tostring(msg.tool_call_id or "unknown")
        memory.write(key, json.encode({
            role = msg.role,
            tool_call_id = msg.tool_call_id,
            name = msg.name,
            content = msg.content,
            ts = os.time() * 1000
        }), 86400 * 7) -- 保留 7 天
        return key
    elseif msg.role == "assistant" and msg.content then
        -- 如果是 assistant 的回复，按段落/代码块提取事实
        local c = tostring(msg.content)
        local summary = c:sub(1, 400)
        if #c > 400 then summary = summary .. "..." end
        local key = ns .. "/assistant_" .. tostring(idx)
        memory.write(key, json.encode({
            role = "assistant",
            summary = summary,
            ts = os.time() * 1000
        }), 86400 * 7)
        return key
    end
    return nil
end

-- 压缩 messages 数组，使其在 token 上限内。
-- 策略：
--   1. 保留 system prompt（由调用方单独处理）。
--   2. 保留最近 MIN_RECENT_TURNS 轮完整的 user/assistant/tool 对话组。
--   3. 把更老的对话归档到 KV Cache，按时间/LRU 删除。
--   4. 返回压缩后的 messages 数组，以及被归档的 key 列表。
function M.compress(session, messages, system_tokens)
    local available = math.floor(M.MAX_TOKENS * (1 - M.SYSTEM_PROMPT_RATIO))
    if system_tokens then
        available = M.MAX_TOKENS - system_tokens
    end
    if available < 0 then available = 0 end

    -- 先尝试全部保留
    local total = M.count_messages(messages)
    if total <= available then
        return messages, {}
    end

    -- 标记最近 MIN_RECENT_TURNS 个完整轮次（从 user 开始到下一个 user 之前）
    local n = #messages
    local protected = {}
    local recent_turns = 0
    local in_turn = false
    for i = n, 1, -1 do
        local role = messages[i].role
        if role == "user" then
            recent_turns = recent_turns + 1
            protected[i] = true
            in_turn = true
            if recent_turns >= M.MIN_RECENT_TURNS then
                break
            end
        elseif in_turn then
            protected[i] = true
        end
    end

    local kept = {}
    local archived = {}
    local kept_tokens = 0

    -- 从最新一条往前遍历
    for i = n, 1, -1 do
        local msg = messages[i]
        local msg_tokens = estimate_tokens(msg.role or "") + estimate_tokens(msg.content or "")
        if msg.tool_calls then
            msg_tokens = msg_tokens + estimate_tokens(json.encode(msg.tool_calls))
        end

        if protected[i] then
            -- 必须保留的最近轮次
            table.insert(kept, 1, msg)
            kept_tokens = kept_tokens + msg_tokens
        elseif kept_tokens + msg_tokens <= available then
            -- 还有空间，保留
            table.insert(kept, 1, msg)
            kept_tokens = kept_tokens + msg_tokens
        else
            -- 空间不足，归档到记忆
            local key = archive_message(session, msg, i)
            if key then
                table.insert(archived, 1, key)
            end
        end
    end

    -- 如果保留后仍然超限（极端情况：单条消息超长），截断最古老的非 system 消息
    while M.count_messages(kept) > available and #kept > M.MIN_RECENT_TURNS * 2 do
        local removed = table.remove(kept, 1)
        local key = archive_message(session, removed, 0)
        if key then table.insert(archived, 1, key) end
    end

    return kept, archived
end

-- 构建最终发送给 LLM 的 messages 列表
-- system_prompt: string
-- messages: 原始对话历史
-- session: session id
function M.build_messages(session, system_prompt, messages)
    local sys_tokens = estimate_tokens(system_prompt)
    local compressed, archived = M.compress(session, messages, sys_tokens)

    local out = { { role = "system", content = system_prompt } }
    for _, m in ipairs(compressed) do
        -- Copy the message wholesale. Keep every protocol field (content,
        -- tool_calls, tool_call_id, reasoning_content, ...): OpenAI-compatible
        -- APIs like deepseek consume reasoning_content natively as an
        -- assistant field and require tool_call_id on tool results, so
        -- dropping fields here breaks multi-turn tool calls or reply
        -- generation. reasoning_content stays out of system_prompt / plain
        -- text, but is preserved as its own assistant field.
        local m2 = {}
        for k, v in pairs(m) do
            m2[k] = v
        end
        table.insert(out, m2)
    end

    return out, archived
end

return M

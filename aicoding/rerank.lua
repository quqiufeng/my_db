-- rerank.lua - lightweight local reranking for KV Cache search results
--
-- No external dependencies. Uses token overlap, exact match bonuses, and
-- length penalties to reorder raw KV results before returning them to the
-- model. Designed to run fast enough for interactive agent loops.

local M = {}

local function lower(s)
    return s:lower()
end

local function tokenize(text)
    text = lower(tostring(text or ""))
    local seen = {}
    local tokens = {}
    for w in text:gmatch("[%w_]+") do
        if #w > 1 and not seen[w] then
            seen[w] = true
            table.insert(tokens, w)
        end
    end
    return tokens
end

local function token_set(tokens)
    local set = {}
    for _, t in ipairs(tokens) do set[t] = true end
    return set
end

local function overlap_score(query_tokens, value_tokens)
    local qset = token_set(query_tokens)
    local vset = token_set(value_tokens)
    local inter = 0
    for t, _ in pairs(qset) do
        if vset[t] then inter = inter + 1 end
    end
    local union = 0
    for t, _ in pairs(qset) do union = union + 1 end
    for t, _ in pairs(vset) do if not qset[t] then union = union + 1 end end
    if union == 0 then return 0 end
    return inter / union
end

-- Compute a normalized [0,1] relevance score for one result.
-- The original vector score (if present) is blended with local signals.
function M.score(query, result)
    local q = lower(query)
    local value = lower(tostring(result.value or ""))
    local key = lower(tostring(result.key or ""))

    local query_tokens = tokenize(query)
    local value_tokens = tokenize(result.value)
    local key_tokens = tokenize(result.key)

    -- Start from vector score if available and positive; otherwise neutral.
    local score = 0
    local original = tonumber(result.score) or 0
    if original and original > 0 then
        score = math.min(original, 1.0) * 0.5
    end

    -- Exact substring match in value is a strong signal.
    if value:find(q, 1, true) then
        score = score + 0.25
    end

    -- Token overlap in value.
    score = score + overlap_score(query_tokens, value_tokens) * 0.15

    -- Token overlap in key.
    score = score + overlap_score(query_tokens, key_tokens) * 0.08

    -- Key contains a query token.
    for _, t in ipairs(query_tokens) do
        if key:find(t, 1, true) then
            score = score + 0.02
            break
        end
    end

    -- Length penalty: too short is often useless; too long is noisy.
    local len = #value
    if len < 20 then
        score = score - 0.05
    elseif len > 4000 then
        score = score - 0.05
    end

    -- Slight boost for results that look like structured facts/code.
    if value:find("^%s*[%[{]") or value:find("\n[%[{]") then
        score = score + 0.02
    end

    return math.max(0, math.min(1, score))
end

-- Rerank and filter raw results.
-- opts:
--   top_k      maximum number of results to return (default #results)
--   min_score  minimum normalized score to keep (default 0)
--   dedupe     remove duplicate values (default true)
function M.rerank(query, results, opts)
    opts = opts or {}
    local top_k = opts.top_k or #results
    local min_score = opts.min_score or 0
    local dedupe = opts.dedupe ~= false

    local scored = {}
    local seen_values = {}
    for _, r in ipairs(results or {}) do
        local s = M.score(query, r)
        if s >= min_score then
            local v = tostring(r.value or "")
            if dedupe then
                if seen_values[v] then
                    -- Keep the higher-scored duplicate.
                    if s > seen_values[v].score then
                        seen_values[v] = { key = r.key, value = r.value, score = s }
                    end
                else
                    seen_values[v] = { key = r.key, value = r.value, score = s }
                    table.insert(scored, seen_values[v])
                end
            else
                table.insert(scored, { key = r.key, value = r.value, score = s })
            end
        end
    end

    table.sort(scored, function(a, b) return a.score > b.score end)

    local out = {}
    for i = 1, math.min(top_k, #scored) do
        table.insert(out, scored[i])
    end
    return out
end

return M

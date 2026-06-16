-- compress.lua - UTEL-style fixed-dictionary compression for system prompts
--
-- Based on https://github.com/quqiufeng/my-agent/blob/main/utel_encoder.py
-- Adapted for English/technical system prompts.
--
-- Core idea:
--   1. Maintain two fixed dictionaries: one for natural-language/technical terms,
--      one for code keywords.
--   2. Mark code blocks with #code ... #end.
--   3. Inside code blocks: replace keywords, collapse spaces to underscores,
--      compress comments with the NL dictionary, keep indentation as dots.
--   4. Outside code blocks: replace NL dictionary entries only.
--   5. Send the decode table + algorithm + compressed data to the model.

local M = {}

local tokens = require("tokens")

-- Natural language / technical term dictionary.
-- Longest/most frequent terms first to avoid partial replacements.
M.NL_DICT = {
    -- High-frequency multi-word phrases
    {"context window", "@CW@"},
    {"system prompt", "@SP@"},
    {"tool call", "@TC@"},
    {"KV Cache", "@KVC@"},
    {"Lua plugin", "@LP@"},

    -- Tool names
    {"kv_search", "@KS@"},
    {"kv_context", "@KC@"},
    {"kv_get", "@KG@"},
    {"kv_set", "@KSET@"},
    {"code_index", "@CI@"},
    {"plugin_create", "@PC@"},
    {"plugin_load", "@PL@"},
    {"plugin_list", "@PLI@"},
    {"apply_patch", "@AP@"},
    {"file_delete", "@FD@"},

    -- Common parameters/fields
    {"search_type", "@ST@"},
    {"description", "@DESC@"},
    {"parameters", "@PARAMS@"},
    {"properties", "@PROPS@"},
    {"required", "@REQ@"},
    {"namespace", "@NS@"},

    -- Namespaces / paths
    {"/code/local/", "@CL@"},
    {"/agent/", "@AG@"},
    {"/code/", "@CD@"},
    {"/session/", "@SS@"},
    {"/opt/my_db/aicoding/", "@OP@"},
    {"/opt/code_caches/", "@OCC@"},
    {"/opt/my_db/", "@ODB@"},

    -- Common English phrases in prompts
    {"You MUST", "@YM@"},
    {"Do not", "@DN@"},
    {"Use `", "@U`@"},
    {"with `", "@W`@"},
    {"to store", "@TS@"},
    {"to read", "@TR@"},
    {"to search", "@TSE@"},
}

-- Code keyword dictionary.
M.CODE_DICT = {
    {"function", "fn8"},
    {"local", "lc5"},
    {"return", "rn6"},
    {"require", "rq7"},
    {"if", "if2"},
    {"then", "th4"},
    {"else", "es4"},
    {"elseif", "ei6"},
    {"end", "ed3"},
    {"for", "fr3"},
    {"in", "in2"},
    {"do", "do2"},
    {"while", "wh5"},
    {"and", "ad3"},
    {"or", "or2"},
    {"not", "nt3"},
    {"true", "te4"},
    {"false", "fe5"},
    {"nil", "nl3"},
}

-- Token estimator (delegates to tokens.lua for consistency).
local function estimate_tokens(text)
    return tokens.estimate(text)
end

-- Replace all occurrences of dictionary keys with values, in order.
local function dict_encode(text, dict)
    local out = text
    for _, pair in ipairs(dict) do
        local k, v = pair[1], pair[2]
        out = out:gsub(k, v)
    end
    return out
end

-- Encode a single line of code.
local function encode_code_line(line)
    if not line:match("%S") then
        return line
    end

    -- Capture leading spaces, convert every 4 spaces to one dot.
    local leading = line:match("^(%s*)")
    local indent = string.rep(".", math.floor(#leading / 4))
    local content = line:sub(#leading + 1)

    -- Code-keyword substitution (word boundaries).
    for _, pair in ipairs(M.CODE_DICT) do
        local k, v = pair[1], pair[2]
        content = content:gsub("(%A)" .. k .. "(%A)", "%1" .. v .. "%2")
        content = content:gsub("^" .. k .. "(%A)", v .. "%1")
        content = content:gsub("(%A)" .. k .. "$", "%1" .. v)
        content = content:gsub("^" .. k .. "$", v)
    end

    -- Handle comments: code before # keeps spaces collapsed, comment gets NL encoded.
    local code_part, comment_part = content:match("^([^#]*)(#.*)$")
    if code_part then
        code_part = code_part:gsub(" ", "_")
        comment_part = dict_encode(comment_part, M.NL_DICT)
        content = code_part .. comment_part
    else
        content = content:gsub(" ", "_")
    end

    return indent .. content
end

-- Encode the inside of a #code ... #end block.
local function encode_code(code)
    local lines = {}
    for line in code:gmatch("[^\r\n]*") do
        table.insert(lines, encode_code_line(line))
    end
    return table.concat(lines, "\n")
end

-- Pack a full mixed text: extract #code ... #end blocks, compress them,
-- then apply NL dictionary to the whole text.
function M.pack(text)
    local result = text
    -- Iterate over #code ... #end blocks.
    for block in text:gmatch("#code\n(.-)\n#end") do
        local compressed = encode_code(block)
        result = result:gsub("#code\n" .. block:gsub("([%-%.%+%*%?%[%]%^%$%%])", "%%%1") .. "\n#end",
                              "#code\n" .. compressed .. "\n#end", 1)
    end
    result = dict_encode(result, M.NL_DICT)
    return result
end

-- Decode helpers for the model instructions (only used entries).
local function decode_table_lines(compressed)
    local lines = {}
    local used_any = false

    table.insert(lines, "NL/TECH DICTIONARY:")
    for _, pair in ipairs(M.NL_DICT) do
        if compressed:find(pair[2], 1, true) then
            table.insert(lines, "  " .. pair[2] .. " -> " .. pair[1])
            used_any = true
        end
    end
    if not used_any then
        table.insert(lines, "  (none used)")
    end

    local code_used = false
    table.insert(lines, "")
    table.insert(lines, "CODE DICTIONARY:")
    for _, pair in ipairs(M.CODE_DICT) do
        if compressed:find(pair[2], 1, true) then
            table.insert(lines, "  " .. pair[2] .. " -> " .. pair[1])
            code_used = true
        end
    end
    if not code_used then
        table.insert(lines, "  (none used)")
    end
    return lines
end

-- Wrap compressed text with decode instructions for the model.
function M.wrap(text, threshold_tokens)
    threshold_tokens = threshold_tokens or 8192
    local orig_tokens = estimate_tokens(text)
    if orig_tokens < threshold_tokens then
        return text, nil
    end

    local compressed = M.pack(text)
    local compressed_tokens = estimate_tokens(compressed)

    -- Only compress if there is meaningful savings in the body.
    -- The decode table is small relative to long prompts; model decoding overhead is acceptable.
    if compressed_tokens >= orig_tokens * 0.95 then
        return text, nil
    end

    -- Build the wrapper.
    local lines = {
        "[This system prompt is compressed with UTEL/LLM Token Compression. Decode it before following instructions.]",
        "",
    }
    for _, l in ipairs(decode_table_lines(compressed)) do
        table.insert(lines, l)
    end
    table.insert(lines, "")
    table.insert(lines, "DECODE ALGORITHM:")
    table.insert(lines, "1. Replace all NL/TECH dictionary tokens with their full forms.")
    table.insert(lines, "2. Inside every #code ... #end block:")
    table.insert(lines, "   a. Convert leading dots back to 4 spaces each.")
    table.insert(lines, "   b. Replace underscores with spaces, except inside string literals.")
    table.insert(lines, "   c. Replace CODE dictionary tokens with their full keywords.")
    table.insert(lines, "3. The decoded text is your actual system prompt. Follow it.")
    table.insert(lines, "")
    table.insert(lines, "--- COMPRESSED SYSTEM PROMPT ---")
    table.insert(lines, compressed)

    local wrapped = table.concat(lines, "\n")
    local wrapped_tokens = estimate_tokens(wrapped)

    return wrapped, {
        original_tokens = orig_tokens,
        compressed_tokens = compressed_tokens,
        saved_tokens = orig_tokens - compressed_tokens,
        ratio = (orig_tokens - compressed_tokens) / orig_tokens,
    }
end

return M

-- opencode tool definitions and dispatch in Lua
-- Tools are stored as KV records under /agent/default/tools/{name}
-- so that the C kernel and prompt builder can discover them dynamically.

local cjson = require("cjson")
local permissions = require("permissions")
local memory = require("memory")
local checkpoint = require("checkpoint")
local knowledge = require("knowledge")
local trace = require("trace")
local shell = require("shell")
local json = require("json")
local log = require("log")

-- Pure Lua helpers: base64 and sha256 (avoid requiring unregistered C functions)
local base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local function base64_encode(data)
    local out = {}
    local b
    for i = 1, #data, 3 do
        local b1 = data:byte(i) or 0
        local b2 = data:byte(i + 1) or 0
        local b3 = data:byte(i + 2) or 0
        b = bit.bor(bit.lshift(b1, 16), bit.lshift(b2, 8), b3)
        table.insert(out, base64_chars:sub(bit.rshift(b, 18) + 1, bit.rshift(b, 18) + 1))
        table.insert(out, base64_chars:sub(bit.band(bit.rshift(b, 12), 63) + 1, bit.band(bit.rshift(b, 12), 63) + 1))
        table.insert(out, base64_chars:sub(bit.band(bit.rshift(b, 6), 63) + 1, bit.band(bit.rshift(b, 6), 63) + 1))
        table.insert(out, base64_chars:sub(bit.band(b, 63) + 1, bit.band(b, 63) + 1))
    end
    local pad = (3 - #data % 3) % 3
    local s = table.concat(out)
    if pad > 0 then
        s = s:sub(1, -pad - 1) .. string.rep("=", pad)
    end
    return s
end

local function sha256(data)
    local function rotr(x, n) return bit.bor(bit.rshift(x, n), bit.lshift(x, 32 - n)) end
    local function bswap(x)
        return bit.bor(bit.lshift(bit.band(x, 0x000000FF), 24),
                       bit.lshift(bit.band(x, 0x0000FF00), 8),
                       bit.rshift(bit.band(x, 0x00FF0000), 8),
                       bit.rshift(bit.band(x, 0xFF000000), 24))
    end
    local h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}
    local k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    }
    local function preprocess(msg)
        local bits = #msg * 8
        local t = {}
        for i = 1, #msg do t[i] = msg:byte(i) end
        table.insert(t, 0x80)
        while (#t % 64) ~= 56 do table.insert(t, 0) end
        for i = 1, 8 do table.insert(t, 0) end
        for i = 0, 3 do table.insert(t, bit.band(bit.rshift(bits, (3 - i) * 8), 0xff)) end
        return t
    end
    local function chunks(t)
        local i = 1
        return function()
            if i > #t then return nil end
            local chunk = {}
            for j = 0, 15 do
                chunk[j + 1] = bit.bor(bit.lshift(t[i + j * 4], 24),
                                       bit.lshift(t[i + j * 4 + 1], 16),
                                       bit.lshift(t[i + j * 4 + 2], 8),
                                       t[i + j * 4 + 3])
            end
            i = i + 64
            return chunk
        end
    end
    for chunk in chunks(preprocess(data)) do
        local w = {}
        for i = 1, 16 do w[i] = chunk[i] end
        for i = 17, 64 do
            local s0 = bit.bxor(rotr(w[i - 15], 7), rotr(w[i - 15], 18), bit.rshift(w[i - 15], 3))
            local s1 = bit.bxor(rotr(w[i - 2], 17), rotr(w[i - 2], 19), bit.rshift(w[i - 2], 10))
            w[i] = bit.band(w[i - 16] + s0 + w[i - 7] + s1, 0xffffffff)
        end
        local a, b, c, d, e, f, g, hh = table.unpack(h)
        for i = 1, 64 do
            local S1 = bit.bxor(rotr(e, 6), rotr(e, 11), rotr(e, 25))
            local ch = bit.bxor(bit.band(e, f), bit.band(bit.bnot(e), g))
            local temp1 = bit.band(hh + S1 + ch + k[i] + w[i], 0xffffffff)
            local S0 = bit.bxor(rotr(a, 2), rotr(a, 13), rotr(a, 22))
            local maj = bit.bxor(bit.band(a, b), bit.band(a, c), bit.band(b, c))
            local temp2 = bit.band(S0 + maj, 0xffffffff)
            hh, g, f, e, d, c, b, a = g, f, e, bit.band(d + temp1, 0xffffffff), c, b, a, bit.band(temp1 + temp2, 0xffffffff)
        end
        h[1] = bit.band(h[1] + a, 0xffffffff); h[2] = bit.band(h[2] + b, 0xffffffff)
        h[3] = bit.band(h[3] + c, 0xffffffff); h[4] = bit.band(h[4] + d, 0xffffffff)
        h[5] = bit.band(h[5] + e, 0xffffffff); h[6] = bit.band(h[6] + f, 0xffffffff)
        h[7] = bit.band(h[7] + g, 0xffffffff); h[8] = bit.band(h[8] + hh, 0xffffffff)
    end
    local out = {}
    for i = 1, 8 do
        table.insert(out, string.format("%08x", bswap(h[i])))
    end
    return table.concat(out)
end

local M = {}

-- Helper: convert internal param spec to OpenAI JSON Schema properties + required array
local function schema_from_params(params)
    local props = {}
    local required = {}
    for name, p in pairs(params) do
        props[name] = {}
        for k, v in pairs(p) do
            if k ~= "required" then
                props[name][k] = v
            end
        end
        if p.required then
            table.insert(required, name)
        end
    end
    return {
        type = "object",
        properties = props,
        required = required
    }
end

-- Helper: remove empty required array to keep strict validators happy
local function finalize_schema(schema)
    if schema.required and #schema.required == 0 then
        schema.required = nil
    end
    return schema
end

local project_root = "."
local session_id = "default"

function M.set_project_root(path)
    project_root = path or "."
    permissions.load(project_root)
end

-- Run git diff for a single file relative to the project root.
local function git_diff_for_file(path)
    local root = project_root or "."
    local rel = path
    if path:sub(1, #root) == root then
        rel = path:sub(#root + 1)
        if rel:sub(1, 1) == "/" then rel = rel:sub(2) end
    end
    if rel == "" then rel = "." end
    local cmd = string.format("git -C %s diff -- %s 2>&1",
                              shell.quote(root), shell.quote(rel))
    local f = io.popen(cmd)
    local out = f:read("*a") or ""
    f:close()
    return out
end

function M.set_session_id(sid)
    session_id = sid or "default"
end

function M.get_project_root()
    return project_root
end

function M.get_session_id()
    return session_id
end

-- Resolve a possibly-relative path against project root
local function resolve_path(path)
    if not path then return nil end
    if path:sub(1, 1) == "/" then
        return path
    end
    if project_root == "." then
        return path
    end
    if project_root:sub(-1) == "/" then
        return project_root .. path
    end
    return project_root .. "/" .. path
end

-- Read whole file in binary mode to preserve BOM and line endings
local function read_file(path, binary)
    local mode = binary and "rb" or "r"
    local f = io.open(path, mode)
    if not f then return nil, "cannot open " .. path end
    local content = f:read("*a")
    f:close()
    return content
end

-- Write whole file in binary mode to preserve exact bytes
local function write_file(path, content)
    local f, err = io.open(path, "wb")
    if not f then return false, err end
    f:write(content)
    f:close()
    return true
end

-- Detect if content contains null bytes (binary file heuristic)
local function is_binary(content)
    if not content then return false end
    return content:find("\0") ~= nil
end

-- Check if a path points to an image file by extension
local function is_image(path)
    local ext = (path or ""):match("%.([^.]+)$")
    if not ext then return false end
    local images = { png = true, jpg = true, jpeg = true, gif = true, bmp = true, webp = true, svg = true }
    return images[ext:lower()] == true
end

-- Truncate long outputs to avoid exceeding model context window
local function truncate(s, max_len)
    max_len = max_len or 4000
    if not s then return s end
    if #s <= max_len then return s end
    return s:sub(1, max_len) .. "\n... [truncated " .. tostring(#s - max_len) .. " chars]"
end

-- Permission check wrapper
local function permit(action, resource)
    local effect = permissions.check(action, resource)
    if effect == "deny" then
        log.warn("permission denied: %s %s", action, resource)
        return false, "denied by permission rule"
    end
    if action == "bash" and resource then
        local dangerous, reason = permissions.is_dangerous_bash(resource)
        if dangerous then
            log.warn("dangerous bash command blocked: %s (%s)", resource, reason)
            return false, "dangerous bash command blocked: " .. tostring(reason)
        end
    end
    if effect == "ask" then
        log.info("asking user for permission: %s %s", action, resource)
        local answer = permissions.prompt_user(action, resource)
        if answer == "deny" then
            return false, "denied by user"
        elseif answer == "allow_session" then
            table.insert(permissions.rules, { action = action, resource = "*", effect = "allow" })
            return true, nil
        elseif answer ~= "allow" then
            return false, "denied by user"
        end
    end
    return true, nil
end

-- Create a checkpoint before destructive file operations.
local function checkpoint_files(paths, reason)
    local files = {}
    for _, p in ipairs(paths) do
        table.insert(files, { path = resolve_path(p) })
    end
    return checkpoint.create(session_id, project_root, files, reason)
end

M.tools = {
    {
        name = "kv_search",
        description = "Search the KV Cache for relevant code, facts, or history. Use search_type='semantic' for natural language code search over indexed repos. Results are reranked locally; use min_score to filter noise.",
        parameters = {
            query = { type = "string", required = true },
            namespace = { type = "string", required = false, description = "KV namespace prefix (e.g. /agent/default/facts) or repo namespace for semantic search (e.g. /code/local/linux)" },
            search_type = { type = "string", enum = {"semantic", "prefix", "regex", "tag"}, required = false },
            top_k = { type = "integer", required = false, description = "Maximum results to return (default 10 for semantic, 100 otherwise)" },
            min_score = { type = "number", required = false, description = "Minimum normalized relevance score [0,1] to keep a result (default 0)" },
            rerank = { type = "boolean", required = false, description = "Run local reranking/filtering (default true)" }
        },
        handler = function(args)
            local results, err = memory.search(args.query, {
                namespace = args.namespace,
                search_type = args.search_type,
                top_k = args.top_k,
                min_score = args.min_score,
                rerank = args.rerank
            })
            if not results then
                return { ok = false, error = tostring(err) }
            end
            return { ok = true, results = results }
        end
    },
    {
        name = "kv_get",
        description = "Read an exact key from the KV Cache.",
        parameters = {
            key = { type = "string", required = true }
        },
        handler = function(args)
            local val = memory.read(args.key)
            return { ok = val ~= nil, value = val }
        end
    },
    {
        name = "kv_set",
        description = "Write a fact or memory into the KV Cache. Optional TTL and namespace prefix.",
        parameters = {
            key = { type = "string", required = true, description = "Key to write. If namespace is given, it is prepended." },
            value = { type = "string", required = true, description = "Value to store (JSON string recommended)" },
            namespace = { type = "string", required = false, description = "Namespace prefix, e.g. /agent/default/facts" },
            ttl_seconds = { type = "integer", required = false, description = "Time-to-live in seconds (0 = permanent)" }
        },
        handler = function(args)
            local full_key = memory.key(args.key, args.namespace)
            local ok = memory.write(full_key, args.value, args.ttl_seconds)
            return { ok = ok, key = full_key }
        end
    },
    {
        name = "kv_context",
        description = "Get full context for one or more symbols (callers/callees/call-paths) from a repo namespace. Set depth to control call-chain expansion.",
        parameters = {
            symbol = { type = "string", required = true, description = "Symbol name, or comma-separated list of symbols" },
            repo = { type = "string", required = true },
            depth = { type = "integer", required = false, description = "Call-chain expansion depth (0-5, default 1)" },
            batch = { type = "boolean", required = false, description = "Treat symbol as comma-separated batch" }
        },
        handler = function(args)
            local symbols = args.symbol
            local opts = { depth = args.depth }
            if args.batch then
                local list = {}
                for s in symbols:gmatch("[^,]+") do
                    s = s:gsub("^%s+", ""):gsub("%s+$", "")
                    if s ~= "" then table.insert(list, s) end
                end
                symbols = list
                opts.batch = true
            end
            local ok, out = memory.context(symbols, args.repo, opts)
            return { ok = ok, output = truncate(out) }
        end
    },
    {
        name = "knowledge_read",
        description = "Read a project-level knowledge file from .opencode/knowledge/ (shared across sessions).",
        parameters = {
            name = { type = "string", required = true, description = "Knowledge file name (e.g. architecture.md)" }
        },
        handler = function(args)
            local content, err = knowledge.read(project_root, args.name)
            if not content then
                return { ok = false, error = err }
            end
            return { ok = true, name = args.name, content = content }
        end
    },
    {
        name = "knowledge_write",
        description = "Write a project-level knowledge file to .opencode/knowledge/ (shared across sessions). Auto-syncs to KV Cache.",
        parameters = {
            name = { type = "string", required = true, description = "Knowledge file name (e.g. decisions.md)" },
            content = { type = "string", required = true, description = "File content" }
        },
        handler = function(args)
            local ok, path = knowledge.write(project_root, args.name, args.content)
            if not ok then
                return { ok = false, error = path }
            end
            return { ok = true, path = path }
        end
    },
    {
        name = "knowledge_search",
        description = "Semantic search over project-level knowledge files.",
        parameters = {
            query = { type = "string", required = true },
            top_k = { type = "integer", required = false }
        },
        handler = function(args)
            knowledge.sync_to_kv(project_root)
            local results, err = knowledge.search(project_root, args.query, args.top_k)
            if not results then
                return { ok = false, error = tostring(err) }
            end
            return { ok = true, results = results }
        end
    },
    {
        name = "trace_query",
        description = "Query the structured agent trace for this session. Useful for debugging loops, repeated tool calls, and failures.",
        parameters = {
            event = { type = "string", required = false, description = "Filter by event type: agent_start, llm_request, llm_response, tool_call, tool_result, context_archive, error" },
            tool = { type = "string", required = false, description = "Filter tool_call/tool_result by tool name" },
            limit = { type = "integer", required = false, description = "Maximum events to return (default 50)" },
            reverse = { type = "boolean", required = false, description = "Return newest first (default true)" }
        },
        handler = function(args)
            trace.init(session_id, project_root)
            local results = trace.query({
                event = args.event,
                tool = args.tool,
                limit = args.limit or 50,
                reverse = args.reverse ~= false
            })
            return { ok = true, count = #results, events = results }
        end
    },
    {
        name = "read",
        description = "Read a file, directory listing, or image. Supports line ranges and detects binary files.",
        parameters = {
            path = { type = "string", required = true, description = "Relative or absolute path to read" },
            offset = { type = "integer", required = false, description = "Starting line (1-based). Omit to read from start." },
            limit = { type = "integer", required = false, description = "Maximum number of lines to return. Omit to read whole file." }
        },
        handler = function(args)
        local path = resolve_path(args.path)
        local attr_cmd = string.format("stat -c '%%F' %s 2>&1", cjson.encode(path))
        local p = io.popen(attr_cmd)
        local attr_type = (p:read("*a") or ""):gsub("%s+", "")
        p:close()

        if attr_type == "" then
            return { ok = false, error = "path not found: " .. path }
        end

            -- Directory listing (shell-based, lfs may not be available)
            if attr_type == "directory" then
                local cmd = string.format("ls -la %s 2>&1", cjson.encode(path))
                local p = io.popen(cmd)
                local out = p:read("*a") or ""
                p:close()
                local entries = {}
                for line in out:gmatch("[^\r\n]+") do
                    local perms, links, owner, group, size, month, day, time_or_year, name =
                        line:match("^([^%s]+)%s+([^%s]+)%s+([^%s]+)%s+([^%s]+)%s+([^%s]+)%s+([^%s]+)%s+([^%s]+)%s+([^%s]+)%s+(.*)$")
                    if name and name ~= "." and name ~= ".." then
                        table.insert(entries, {
                            name = name,
                            perms = perms,
                            size = tonumber(size) or 0,
                            entry_type = perms:sub(1, 1) == "d" and "directory" or "file"
                        })
                    end
                end
                return { ok = true, type = "directory", path = path, entries = entries }
            end

            -- Image file
            if is_image(path) then
                local content, err = read_file(path, true)
                if not content then
                    return { ok = false, error = err }
                end
                return { ok = true, type = "image", path = path, base64 = base64_encode(content) }
            end

            -- Regular file
            local raw, err = read_file(path, true)
            if not raw then
                return { ok = false, error = err }
            end

            if is_binary(raw) then
                return { ok = true, type = "binary", path = path, size = #raw, note = "binary file detected" }
            end

            -- Convert CRLF to LF for processing but remember original ending for writes
            local had_bom = raw:sub(1, 3) == "\xEF\xBB\xBF"
            local content = had_bom and raw:sub(4) or raw
            local line_ending = content:find("\r\n") and "crlf" or "lf"

            -- Apply line range pagination
            local offset = args.offset or 1
            local limit = args.limit or 0
            local lines = {}
            for line in content:gmatch("([^\r\n]*)\r?\n") do
                table.insert(lines, line)
            end
            if content:sub(-1) ~= "\n" and content ~= "" then
                table.insert(lines, content:match("[^\r\n]*$"))
            end

            local total = #lines
            local start_line = math.max(1, offset)
            local end_line = limit > 0 and math.min(total, start_line + limit - 1) or total
            local selected = {}
            for i = start_line, end_line do
                table.insert(selected, lines[i])
            end

            return {
                ok = true,
                type = "text",
                path = path,
                total_lines = total,
                offset = start_line,
                limit = limit,
                had_bom = had_bom,
                line_ending = line_ending,
                content = table.concat(selected, "\n")
            }
        end
    },
    {
        name = "edit",
        description = "Apply a precise text replacement in a file. old_string must match exactly once.",
        parameters = {
            path = { type = "string", required = true },
            old_string = { type = "string", required = true },
            new_string = { type = "string", required = true },
            expected_count = { type = "integer", required = false, description = "Expected number of replacements. Defaults to 1." }
        },
        handler = function(args)
            local ok, err = permit("edit", resolve_path(args.path))
            if not ok then return { ok = false, error = err } end
            local path = resolve_path(args.path)

            -- Checkpoint before edit
            local cp = checkpoint_files({path}, "edit")
            if not cp.ok then
                return { ok = false, error = "checkpoint failed: " .. tostring(cp.error) }
            end

            local raw, rerr = read_file(path, true)
            if not raw then
                return { ok = false, error = rerr }
            end

            local had_bom = raw:sub(1, 3) == "\xEF\xBB\xBF"
            local bom_prefix = had_bom and raw:sub(1, 3) or ""
            local content = had_bom and raw:sub(4) or raw
            local line_ending = content:find("\r\n") and "\r\n" or "\n"

            local old = args.old_string
            local expected = args.expected_count or 1
            local count = 0
            local start = 1
            while true do
                local idx = content:find(old, start, true)
                if not idx then break end
                count = count + 1
                start = idx + #old
            end
            if count == 0 then
                return { ok = false, error = "old_string not found in file" }
            end
            if count ~= expected then
                return { ok = false, error = string.format("expected %d match(es) but found %d", expected, count) }
            end

            local new_content = content
            local pos = 1
            for _ = 1, expected do
                local idx = new_content:find(old, pos, true)
                if not idx then break end
                new_content = new_content:sub(1, idx - 1) .. args.new_string .. new_content:sub(idx + #old)
                pos = idx + #args.new_string
            end

            local final = bom_prefix .. new_content
            local wok, werr = write_file(path, final)
            if not wok then
                return { ok = false, error = tostring(werr) }
            end
            local diff_out = git_diff_for_file(path)
            return { ok = true, replaced = old, with = args.new_string, count = expected, checkpoint = cp.checkpoint_id, diff = diff_out }
        end
    },
    {
        name = "write",
        description = "Create or overwrite a file with the given content. Supports conditional writes.",
        parameters = {
            path = { type = "string", required = true },
            content = { type = "string", required = true },
            create_only = { type = "boolean", required = false, description = "Fail if file already exists." },
            append = { type = "boolean", required = false, description = "Append content instead of overwriting." },
            expected_hash = { type = "string", required = false, description = "SHA-256 hash the file must match before writing." }
        },
        handler = function(args)
            local ok, err = permit("write", resolve_path(args.path))
            if not ok then return { ok = false, error = err } end
            local path = resolve_path(args.path)

            local exists = false
            local existing = read_file(path, true)
            if existing then exists = true end

            if args.create_only and exists then
                return { ok = false, error = "file already exists: " .. path }
            end

            -- Checkpoint before overwrite
            if exists then
                local cp = checkpoint_files({path}, "write")
                if not cp.ok then
                    return { ok = false, error = "checkpoint failed: " .. tostring(cp.error) }
                end
            end

            if args.expected_hash and exists then
                local hash = sha256(existing)
                if hash ~= args.expected_hash then
                    return { ok = false, error = "file hash mismatch, refusing to overwrite" }
                end
            end

            local content = args.content
            if args.append and exists then
                content = existing .. content
            end

            local dir = path:match("^(.*)/")
            if dir then
                os.execute("mkdir -p " .. shell.quote(dir))
            end

            local cok, cerr = write_file(path, content)
            if not cok then
                return { ok = false, error = tostring(cerr) }
            end
            local diff_out = exists and git_diff_for_file(path) or ""
            return { ok = true, written = path, bytes = #content, diff = diff_out }
        end
    },
    {
        name = "apply_patch",
        description = "Apply a unified diff patch to the working tree or a specific file.",
        parameters = {
            patch = { type = "string", required = true, description = "Unified diff text" },
            file_path = { type = "string", required = false, description = "Optional target file path" }
        },
        handler = function(args)
            local ok, err = permit("apply_patch", resolve_path(args.file_path or "."))
            if not ok then return { ok = false, error = err } end
            local patch = args.patch
            local target = args.file_path and resolve_path(args.file_path) or ""

            -- Determine files that will be modified by parsing the patch
            local paths_to_backup = {}
            if target ~= "" then
                table.insert(paths_to_backup, target)
            else
                for line in patch:gmatch("[^\r\n]+") do
                    local f = line:match("^--- (.+)$")
                    if f and f ~= "/dev/null" then
                        -- Strip optional timestamp
                        f = f:gsub("\t.*$", "")
                        table.insert(paths_to_backup, resolve_path(f))
                    end
                end
            end

            -- Checkpoint affected existing files
            local cp = checkpoint_files(paths_to_backup, "apply_patch")
            if not cp.ok then
                return { ok = false, error = "checkpoint failed: " .. tostring(cp.error) }
            end

            local tmp = os.tmpname() .. ".patch"
            local f = io.open(tmp, "w")
            if not f then return { ok = false, error = "cannot create temp patch file" } end
            f:write(patch)
            f:close()
            local cmd = target ~= "" and string.format("patch -p0 --forward -i %s -- %s 2>&1", shell.quote(tmp), shell.quote(target))
                or string.format("patch -p1 --forward -i %s 2>&1", shell.quote(tmp))
            local p = io.popen(cmd)
            local out = p:read("*a") or ""
            local pok = p:close()
            os.remove(tmp)
            return { ok = pok, output = truncate(out), checkpoint = cp.checkpoint_id }
        end
    },
    {
        name = "glob",
        description = "List files matching a glob pattern under the project root.",
        parameters = {
            pattern = { type = "string", required = true },
            path = { type = "string", required = false, description = "Base directory. Defaults to project root." }
        },
        handler = function(args)
            local base = resolve_path(args.path or ".")
            local pattern = args.pattern
            local cmd = string.format("cd %s && find . -name %s -print 2>/dev/null | head -200", shell.quote(base), shell.quote(pattern))
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            f:close()
            local files = {}
            for line in out:gmatch("[^\r\n]+") do
                if line ~= "" then table.insert(files, line) end
            end
            return { ok = true, files = files }
        end
    },
    {
        name = "grep",
        description = "Search file contents with ripgrep-style regex.",
        parameters = {
            pattern = { type = "string", required = true },
            path = { type = "string", required = false, description = "Base directory. Defaults to project root." },
            glob = { type = "string", required = false, description = "Glob filter for files" }
        },
        handler = function(args)
            local base = resolve_path(args.path or ".")
            local pattern = args.pattern
            local glob = args.glob and string.format("-g %s", shell.quote(args.glob)) or ""
            local cmd = string.format("cd %s && rg --line-number --no-heading %s %s 2>/dev/null | head -200",
                                      shell.quote(base), glob, shell.quote(pattern))
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            f:close()
            return { ok = true, output = truncate(out) }
        end
    },
    {
        name = "file_delete",
        description = "Delete a file.",
        parameters = {
            path = { type = "string", required = true },
            expected_hash = { type = "string", required = false, description = "SHA-256 hash the file must match before deleting." }
        },
        handler = function(args)
            local ok, err = permit("file_delete", resolve_path(args.path))
            if not ok then return { ok = false, error = err } end
            local path = resolve_path(args.path)

            local existing = read_file(path, true)
            if not existing then
                return { ok = false, error = "file not found: " .. path }
            end

            if args.expected_hash then
                local hash = sha256(existing)
                if hash ~= args.expected_hash then
                    return { ok = false, error = "file hash mismatch, refusing to delete" }
                end
            end

            -- Checkpoint before delete
            local cp = checkpoint_files({path}, "file_delete")
            if not cp.ok then
                return { ok = false, error = "checkpoint failed: " .. tostring(cp.error) }
            end

            local dok, derr = os.remove(path)
            if not dok then
                return { ok = false, error = tostring(derr) }
            end
            return { ok = true, deleted = path, checkpoint = cp.checkpoint_id }
        end
    },
    {
        name = "bash",
        description = "Run a shell command and return stdout/stderr.",
        parameters = {
            command = { type = "string", required = true },
            timeout = { type = "integer", required = false },
            workdir = { type = "string", required = false, description = "Working directory. Defaults to project root." }
        },
        handler = function(args)
            local ok, err = permit("bash", args.command)
            if not ok then return { ok = false, error = err } end
            local cmd = args.command
            local timeout = args.timeout or 30
            local workdir = shell.quote(resolve_path(args.workdir) or project_root)
            local f = io.popen(string.format("cd %s && timeout %d bash -c %s 2>&1",
                                             workdir, timeout, shell.quote(cmd)))
            local out = f:read("*a") or ""
            local bok = f:close()
            return { ok = bok, output = truncate(out) }
        end
    },
    {
        name = "git",
        description = "Run a git command in the current project. Use for status, diff, add, commit, log, branch, etc.",
        parameters = {
            command = { type = "string", required = true },
            repo_path = { type = "string", required = false }
        },
        handler = function(args)
            local ok, err = permit("git", args.command)
            if not ok then return { ok = false, error = err } end
            local repo = resolve_path(args.repo_path) or project_root
            local cmd = string.format("cd %s && git %s 2>&1", shell.quote(repo), args.command)
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            local gok = f:close()
            return { ok = gok, output = truncate(out) }
        end
    },
    {
        name = "diff",
        description = "Show unified diff of current working tree changes (git diff) or between two files.",
        parameters = {
            repo_path = { type = "string", required = false },
            file_path = { type = "string", required = false }
        },
        handler = function(args)
            local repo = resolve_path(args.repo_path) or project_root
            local file = args.file_path or ""
            local cmd
            if file ~= "" then
                cmd = string.format("cd %s && git diff -- %s 2>&1", shell.quote(repo), shell.quote(file))
            else
                cmd = string.format("cd %s && git diff 2>&1", shell.quote(repo))
            end
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            local ok = f:close()
            return { ok = ok, output = truncate(out) }
        end
    },
    {
        name = "build",
        description = "Run a project build/compile command and return structured output. If command is omitted, the tool auto-detects the build system (make, cargo, npm, cmake, go, python). Use this after editing code to verify changes compile. On failure, the truncated output is returned so you can fix errors.",
        parameters = {
            command = { type = "string", required = false, description = "Build command to run. Auto-detected if omitted." },
            workdir = { type = "string", required = false, description = "Working directory. Defaults to project root." },
            timeout = { type = "integer", required = false, description = "Timeout in seconds (default 120)." },
            max_output_lines = { type = "integer", required = false, description = "Max lines of output to return (default 200)." }
        },
        handler = function(args)
            local ok, err = permit("bash", args.command or "build")
            if not ok then return { ok = false, error = err } end

            local workdir = resolve_path(args.workdir) or project_root
            local timeout = args.timeout or 120
            local max_lines = args.max_output_lines or 200

            local function file_exists(path)
                local f = io.open(path, "r")
                if f then f:close(); return true end
                return false
            end

            local function detect_build_command(dir)
                if file_exists(dir .. "/Cargo.toml") then
                    return "cargo build --release"
                elseif file_exists(dir .. "/package.json") then
                    return "npm run build"
                elseif file_exists(dir .. "/Makefile") or file_exists(dir .. "/makefile") then
                    return "make"
                elseif file_exists(dir .. "/CMakeLists.txt") then
                    return "cmake --build build"
                elseif file_exists(dir .. "/go.mod") then
                    return "go build ./..."
                elseif file_exists(dir .. "/setup.py") or file_exists(dir .. "/pyproject.toml") then
                    return "python -m build"
                else
                    return nil
                end
            end

            local cmd = args.command
            if not cmd or cmd == "" then
                cmd = detect_build_command(workdir)
                if not cmd then
                    return { ok = false, error = "Could not auto-detect build system in " .. workdir }
                end
            end

            local full_cmd = string.format("cd %s && timeout %d bash -c %s 2>&1",
                                           shell.quote(workdir), timeout, shell.quote(cmd))
            local f = io.popen(full_cmd)
            local out = f:read("*a") or ""
            local bok = f:close()
            local exit_code = 0
            if not bok then
                -- io.popen close returns nil, exit code string, or just false on error
                -- Normalize to a numeric-ish value for downstream use.
                exit_code = 1
            end

            -- Summarize output to a reasonable line count.
            local lines = {}
            for line in out:gmatch("[^\r\n]+") do
                table.insert(lines, line)
                if #lines >= max_lines then
                    table.insert(lines, "... [truncated]")
                    break
                end
            end
            local summary = table.concat(lines, "\n")

            -- Persist result to KV Cache for cross-turn memory.
            local build_key = string.format("/agent/%s/builds/%d", session_id, os.time())
            local build_record = cjson.encode({
                command = cmd,
                workdir = workdir,
                exit_code = exit_code,
                output_summary = summary,
                ok = exit_code == 0
            })
            pcall(function() memory.write(build_key, build_record, 0) end)

            if exit_code == 0 then
                return { ok = true, command = cmd, exit_code = exit_code, output = summary }
            else
                return { ok = false, command = cmd, exit_code = exit_code, output = summary, error = "Build failed" }
            end
        end
    },
    {
        name = "web_fetch",
        description = "Fetch content from a URL. Useful for reading API docs, RFCs, library documentation, or StackOverflow answers. Returns truncated text/html/markdown.",
        parameters = {
            url = { type = "string", required = true, description = "URL to fetch" },
            format = { type = "string", enum = {"text", "html", "markdown"}, required = false, description = "Return format. Defaults to text." },
            timeout = { type = "integer", required = false, description = "Timeout in seconds (default 30)." },
            max_length = { type = "integer", required = false, description = "Max characters to return (default 8000)." }
        },
        handler = function(args)
            local ok, err = permit("bash", "web_fetch " .. tostring(args.url))
            if not ok then return { ok = false, error = err } end

            local url = args.url
            local fmt = (args.format or "text"):lower()
            local timeout = args.timeout or 30
            local max_len = args.max_length or 8000

            if url == "" then
                return { ok = false, error = "url is empty" }
            end

            -- Validate URL scheme to prevent accidental local command execution.
            local scheme = url:match("^([a-zA-Z][a-zA-Z0-9+.-]*):")
            if not scheme or (scheme ~= "http" and scheme ~= "https") then
                return { ok = false, error = "only http/https URLs are supported" }
            end

            -- Prefer lynx/w3m/pandoc for cleaner text/markdown output if available.
            local function command_available(name)
                local p = io.popen("command -v " .. shell.quote(name) .. " 2>/dev/null")
                local out = p:read("*a") or ""
                p:close()
                return out:gsub("%s+", "") ~= ""
            end

            local cmd
            if fmt == "html" then
                cmd = string.format("curl -sL --max-time %d --user-agent %s %s 2>&1",
                                    timeout, shell.quote("opencode/1.0"), shell.quote(url))
            elseif fmt == "markdown" then
                if command_available("pandoc") then
                    cmd = string.format("curl -sL --max-time %d --user-agent %s %s 2>&1 | pandoc -f html -t markdown --wrap=none 2>&1",
                                        timeout, shell.quote("opencode/1.0"), shell.quote(url))
                elseif command_available("lynx") then
                    cmd = string.format("curl -sL --max-time %d --user-agent %s %s 2>&1 | lynx -stdin -dump 2>&1",
                                        timeout, shell.quote("opencode/1.0"), shell.quote(url))
                else
                    cmd = string.format("curl -sL --max-time %d --user-agent %s %s 2>&1",
                                        timeout, shell.quote("opencode/1.0"), shell.quote(url))
                end
            else
                if command_available("lynx") then
                    cmd = string.format("curl -sL --max-time %d --user-agent %s %s 2>&1 | lynx -stdin -dump 2>&1",
                                        timeout, shell.quote("opencode/1.0"), shell.quote(url))
                elseif command_available("w3m") then
                    cmd = string.format("curl -sL --max-time %d --user-agent %s %s 2>&1 | w3m -T text/html -dump 2>&1",
                                        timeout, shell.quote("opencode/1.0"), shell.quote(url))
                else
                    -- Last resort: strip HTML tags with sed.
                    cmd = string.format("curl -sL --max-time %d --user-agent %s %s 2>&1 | sed 's/<[^>]*>//g' 2>&1",
                                        timeout, shell.quote("opencode/1.0"), shell.quote(url))
                end
            end

            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            f:close()

            -- Basic truncation with marker.
            local truncated = false
            if #out > max_len then
                out = out:sub(1, max_len) .. "\n... [truncated " .. tostring(#out - max_len) .. " chars]"
                truncated = true
            end

            -- Persist a short reference to KV Cache.
            local fetch_key = string.format("/agent/%s/web_fetches/%d", session_id, os.time())
            pcall(function()
                memory.write(fetch_key, cjson.encode({
                    url = url,
                    format = fmt,
                    truncated = truncated,
                    preview = out:sub(1, 200),
                }), 86400)
            end)

            return { ok = true, url = url, format = fmt, truncated = truncated, content = out }
        end
    },
    {
        name = "plugin_create",
        description = "Create or overwrite a Lua plugin in the agent plugins directory. The plugin can add new tools, helpers, or hooks.",
        parameters = {
            name = { type = "string", required = true, description = "Plugin filename without .lua extension" },
            code = { type = "string", required = true, description = "Full Lua source code for the plugin" },
            scope = { type = "string", enum = {"global", "project"}, required = false, description = "global writes to /opt/my_db/aicoding/plugins/, project writes to .opencode/plugins/" }
        },
        handler = function(args)
            local name = args.name:gsub("[^%w_-]", "")
            if name == "" then return { ok = false, error = "invalid plugin name" } end
            local scope = args.scope or "global"
            local dir
            if scope == "project" then
                dir = M.get_project_root() .. "/.opencode/plugins"
            else
                dir = "/opt/my_db/aicoding/plugins"
            end
            local cmd = string.format("mkdir -p %s", cjson.encode(dir))
            os.execute(cmd)
            local path = dir .. "/" .. name .. ".lua"
            local f = io.open(path, "w")
            if not f then return { ok = false, error = "cannot write " .. path } end
            f:write(args.code)
            f:close()
            return { ok = true, path = path }
        end
    },
    {
        name = "plugin_load",
        description = "Hot-load a Lua plugin by name. After loading, any new tools returned by the plugin are merged into the agent tool set and exposed to the model immediately.",
        parameters = {
            name = { type = "string", required = true, description = "Plugin filename without .lua extension" },
            scope = { type = "string", enum = {"global", "project"}, required = false, description = "Where to look for the plugin" }
        },
        handler = function(args)
            local name = args.name:gsub("[^%w_-]", "")
            local scope = args.scope or "global"
            local dir
            if scope == "project" then
                dir = M.get_project_root() .. "/.opencode/plugins"
            else
                dir = "/opt/my_db/aicoding/plugins"
            end
            local f = io.open(path, "r")
            if not f then return { ok = false, error = "plugin not found: " .. path } end
            f:close()

            -- Load plugin and capture returned tool definitions
            local chunk, load_err = loadfile(path)
            if not chunk then return { ok = false, error = tostring(load_err) } end
            local ok, result = pcall(chunk)
            if not ok then return { ok = false, error = tostring(result) } end

            -- Merge returned tools into active tool set
            if type(result) == "table" then
                for _, tool in ipairs(result) do
                    if type(tool) == "table" and tool.name then
                        table.insert(M.tools, tool)
                    end
                end
            end

            M.register_tools("/agent/default")
            return { ok = true, path = path, tools_loaded = type(result) == "table" and #result or 0 }
        end
    },
    {
        name = "plugin_list",
        description = "List available and currently loaded Lua plugins.",
        parameters = {},
        handler = function(args)
            local global_dir = "/opt/my_db/aicoding/plugins"
            local project_dir = M.get_project_root() .. "/.opencode/plugins"
            local function scan(dir)
                local out = {}
                local p = io.popen("ls " .. cjson.encode(dir) .. " 2>/dev/null")
                if p then
                    for line in p:lines() do
                        if line:match("%.lua$") then
                            table.insert(out, line)
                        end
                    end
                    p:close()
                end
                return out
            end
            return {
                ok = true,
                global_plugins = scan(global_dir),
                project_plugins = scan(project_dir),
                note = "Use plugin_create to add a plugin, plugin_load to hot-load it."
            }
        end
    },
    {
        name = "checkpoint_list",
        description = "List checkpoints (automatic backups taken before edits).",
        parameters = {
            session_id = { type = "string", required = false, description = "Session filter. Defaults to current session." }
        },
        handler = function(args)
            local sid = args.session_id or session_id
            return checkpoint.list(sid)
        end
    },
    {
        name = "undo_last",
        description = "Undo the most recent checkpoint by restoring backed-up files.",
        parameters = {
            session_id = { type = "string", required = false, description = "Session to undo. Defaults to current session." }
        },
        handler = function(args)
            local sid = args.session_id or session_id
            return checkpoint.undo_last(sid, project_root)
        end
    },
    {
        name = "rollback_to",
        description = "Restore files to a specific checkpoint id.",
        parameters = {
            checkpoint_id = { type = "string", required = true },
            session_id = { type = "string", required = false, description = "Defaults to current session." }
        },
        handler = function(args)
            local sid = args.session_id or session_id
            return checkpoint.restore(sid, project_root, args.checkpoint_id)
        end
    }
}

-- Publish tool schemas to KV Cache ------------------------------------------

function M.register_tools(agent_ns)
    agent_ns = agent_ns or "/agent/default"
    if not agent_ns:match("/$") then agent_ns = agent_ns .. "/" end
    for _, t in ipairs(M.tools) do
        local schema = {
            t = "tool",
            name = t.name,
            description = t.description,
            parameters = finalize_schema(schema_from_params(t.parameters))
        }
        local key = agent_ns .. "tools/" .. t.name
        if opencode and opencode.cache_set then
            opencode.cache_set(key, cjson.encode(schema), 0)
        end
    end
end

-- Build OpenAI-compatible tools array for LLM request
function M.build_tools_for_request()
    local out = {}
    for _, t in ipairs(M.tools) do
        table.insert(out, {
            type = "function",
            ["function"] = {
                name = t.name,
                description = t.description,
                parameters = finalize_schema(schema_from_params(t.parameters))
            }
        })
    end
    return out
end

-- Dispatch a tool call -------------------------------------------------------

function M.dispatch(tool_call)
    local name = tool_call.name
    local args = tool_call.arguments or tool_call.parameters or {}
    if type(args) == "string" then
        local decoded, err = json.decode(args)
        if not decoded then
            return { ok = false, error = "invalid tool arguments JSON: " .. tostring(err) }
        end
        args = decoded
    end
    for _, t in ipairs(M.tools) do
        if t.name == name then
            local ok, res = pcall(t.handler, args)
            if not ok then
                return { ok = false, error = tostring(res) }
            end
            return res
        end
    end
    return { ok = false, error = "unknown tool: " .. tostring(name) }
end

return M

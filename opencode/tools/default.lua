-- opencode tool definitions and dispatch in Lua
-- Tools are stored as KV records under /agent/default/tools/{name}
-- so that the C kernel and prompt builder can discover them dynamically.

local cjson = require("cjson")
local permissions = require("permissions")

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

function M.set_project_root(path)
    project_root = path or "."
    permissions.load(project_root)
end

function M.get_project_root()
    return project_root
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
        return false, "denied by permission rule"
    end
    if effect == "ask" then
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

M.tools = {
    {
        name = "kv_search",
        description = "Search the KV Cache for relevant code, facts, or history.",
        parameters = {
            query = { type = "string", required = true },
            namespace = { type = "string", required = false },
            search_type = { type = "string", enum = {"semantic", "prefix", "regex", "tag"}, required = false }
        },
        handler = function(args)
            local q = args.query or ""
            local ns = args.namespace or ""
            local prefix = ns
            if prefix ~= "" and not prefix:match("/$") then prefix = prefix .. "/" end
            local results = opencode.cache_search_prefix(prefix, 10)
            local out = {}
            for _, r in ipairs(results) do
                table.insert(out, { key = r.key, value = r.value, score = r.score })
            end
            return { ok = true, results = out }
        end
    },
    {
        name = "kv_get",
        description = "Read an exact key from the KV Cache.",
        parameters = {
            key = { type = "string", required = true }
        },
        handler = function(args)
            local val = opencode.cache_get(args.key)
            return { ok = val ~= nil, value = val }
        end
    },
    {
        name = "kv_context",
        description = "Get full context for a symbol (callers/callees) from a repo namespace.",
        parameters = {
            symbol = { type = "string", required = true },
            repo = { type = "string", required = true }
        },
        handler = function(args)
            local cmd = string.format("cd /opt/my_db && ./tools/cache_query %s --repo %s --type context 2>&1",
                                        cjson.encode(args.symbol), cjson.encode(args.repo))
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            local ok = f:close()
            return { ok = ok, output = truncate(out) }
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

            local new_content = content:gsub(old, args.new_string, expected)
            local final = bom_prefix .. new_content
            local wok, werr = write_file(path, final)
            if not wok then
                return { ok = false, error = tostring(werr) }
            end
            return { ok = true, replaced = old, with = args.new_string, count = expected }
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

            local cok, cerr = write_file(path, content)
            if not cok then
                return { ok = false, error = tostring(cerr) }
            end
            return { ok = true, written = path, bytes = #content }
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
            local tmp = os.tmpname() .. ".patch"
            local f = io.open(tmp, "w")
            if not f then return { ok = false, error = "cannot create temp patch file" } end
            f:write(patch)
            f:close()
            local cmd = target ~= "" and string.format("patch -p0 --forward -i %s -- %s 2>&1", cjson.encode(tmp), cjson.encode(target))
                or string.format("patch -p1 --forward -i %s 2>&1", cjson.encode(tmp))
            local p = io.popen(cmd)
            local out = p:read("*a") or ""
            local pok = p:close()
            os.remove(tmp)
            return { ok = pok, output = truncate(out) }
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
            local cmd = string.format("cd %s && find . -path %s -print 2>/dev/null | head -200", cjson.encode(base), cjson.encode(pattern))
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
            local glob = args.glob and string.format("-g %s", cjson.encode(args.glob)) or ""
            local cmd = string.format("cd %s && rg --line-number --no-heading %s %s 2>/dev/null | head -200",
                                      cjson.encode(base), glob, cjson.encode(pattern))
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

            if args.expected_hash then
                local existing = read_file(path, true)
                if not existing then
                    return { ok = false, error = "file not found: " .. path }
                end
                local hash = sha256(existing)
                if hash ~= args.expected_hash then
                    return { ok = false, error = "file hash mismatch, refusing to delete" }
                end
            end

            local dok, derr = os.remove(path)
            if not dok then
                return { ok = false, error = tostring(derr) }
            end
            return { ok = true, deleted = path }
        end
    },
    {
        name = "bash",
        description = "Run a shell command and return stdout/stderr.",
        parameters = {
            command = { type = "string", required = true },
            timeout = { type = "integer", required = false }
        },
        handler = function(args)
            local ok, err = permit("bash", args.command)
            if not ok then return { ok = false, error = err } end
            local cmd = args.command
            local timeout = args.timeout or 30
            local f = io.popen(string.format("timeout %d bash -c %s 2>&1",
                                             timeout, cjson.encode(cmd)))
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
            local repo = resolve_path(args.repo_path) or "."
            local cmd = string.format("cd %s && git %s 2>&1", cjson.encode(repo), args.command)
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
            local repo = resolve_path(args.repo_path) or "."
            local file = args.file_path or ""
            local cmd
            if file ~= "" then
                cmd = string.format("cd %s && git diff -- %s 2>&1", cjson.encode(repo), cjson.encode(file))
            else
                cmd = string.format("cd %s && git diff 2>&1", cjson.encode(repo))
            end
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            local ok = f:close()
            return { ok = ok, output = truncate(out) }
        end
    },
    {
        name = "code_index",
        description = "Index a code repository into KV Cache using the coding.md toolchain.",
        parameters = {
            source = { type = "string", required = true },
            namespace = { type = "string", required = true },
            language = { type = "string", required = false }
        },
        handler = function(args)
            local src = args.source
            local ns = args.namespace
            local lang = args.language or "auto"
            local script = "analyze_repo.sh"
            if lang == "nodejs" or lang == "typescript" then
                script = "analyze_nodejs_repo.sh"
            end
            local cmd = string.format("cd /opt/my_db && ./%s %s %s 2>&1",
                                        script, cjson.encode(src), cjson.encode(ns))
            local f = io.popen(cmd)
            local out = f:read("*a") or ""
            local ok = f:close()
            return { ok = ok, output = truncate(out, 8000) }
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
        opencode.cache_set(key, cjson.encode(schema), 0)
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
        args = cjson.decode(args)
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

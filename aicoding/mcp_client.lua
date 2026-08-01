-- mcp_client.lua - minimal MCP (Model Context Protocol) client over
-- Streamable HTTP transport, implemented with the `curl` CLI so it runs
-- synchronously inside the single-threaded engine (same pattern as
-- tools/default.lua web_fetch).
--
-- Supported servers: MCP servers exposing the Streamable HTTP transport
-- (POST / with JSON-RPC batches, per the MCP 2025-06-18 spec). Many hosts
-- expose this at a single endpoint.
--
-- API:
--   MCP.new() -> client
--   client:connect(endpoint, { headers = {...}, timeout = n })
--   client:tools() -> list of { name, description, inputSchema }
--   client:call(name, args) -> result object
--   client:close()

local json = require("json")
local shell = require("shell")

local M = {}

function M.new()
    return setmetatable({ endpoint = nil, headers = {}, timeout = 30 }, { __index = M })
end

-- Issue a JSON-RPC request over HTTP and return the decoded response body.
function M._request(self, method, params)
    local body = json.encode({
        jsonrpc = "2.0",
        id = math.random(1, 1e9),
        method = method,
        params = params or {},
    })
    local header_args = { "Content-Type: application/json", "Accept: application/json" }
    for _, h in ipairs(self.headers) do
        table.insert(header_args, h)
    end
    local headers = table.concat(header_args, " -H ")
    local cmd = string.format(
        "curl -s --max-time %d -X POST %s -H '%s' -d %s 2>&1",
        self.timeout, shell.quote(self.endpoint), headers, shell.quote(body)
    )
    local f = io.popen(cmd)
    local out = f:read("*a") or ""
    f:close()
    local ok, resp = pcall(json.decode, out)
    if not ok or type(resp) ~= "table" then
        return nil, "invalid MCP response: " .. out:sub(1, 200)
    end
    if resp.error then
        return nil, "MCP error: " .. tostring(resp.error.message or json.encode(resp.error))
    end
    return resp.result, nil
end

-- Connect + initialize the MCP session.
function M.connect(self, endpoint, opts)
    opts = opts or {}
    self.endpoint = endpoint
    self.timeout = opts.timeout or 30
    self.headers = {}
    local hdrs = opts.headers or {}
    if type(hdrs) == "table" then
        for _, h in ipairs(hdrs) do
            table.insert(self.headers, h)
        end
    end
    if opts.authorization then
        table.insert(self.headers, "Authorization: " .. opts.authorization)
    end

    local result, err = M._request(self, "initialize", {
        protocolVersion = "2025-06-18",
        capabilities = {},
        clientInfo = { name = "aicoding", version = "0.1.0" },
    })
    if not result then return nil, err end

    -- Optional MCP-session-id handling: capture from response headers is
    -- not available via curl CLI; servers that require it may fail here.
    -- Most Streamable HTTP servers work statelessly for tools/list+call.
    return true, nil
end

-- List available tools: { name, description, inputSchema }.
function M.tools(self)
    local result, err = M._request(self, "tools/list", {})
    if not result then return nil, err end
    return result.tools or {}, nil
end

-- Call a tool. Returns the MCP result content.
function M.call(self, name, args)
    local result, err = M._request(self, "tools/call", {
        name = name,
        arguments = args or {},
    })
    if not result then return nil, err end
    -- MCP result.content is an array of { type, text | image } parts.
    if type(result.content) == "table" then
        local parts = {}
        for _, p in ipairs(result.content) do
            if p.type == "text" then
                table.insert(parts, p.text or "")
            elseif p.type == "image" and p.data then
                table.insert(parts, "[image data]")
            end
        end
        result.content = table.concat(parts, "\n")
    end
    if result.isError then
        return nil, "MCP tool error: " .. tostring(result.content)
    end
    return result, nil
end

function M.close(self)
    self.endpoint = nil
    return true
end

return M

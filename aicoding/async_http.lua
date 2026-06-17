-- Non-blocking HTTP wrapper using C async HTTP API.
-- This module lets Lua coroutines perform HTTP requests without blocking
-- the GPUI main thread. The caller is responsible for polling/yielding.

local M = {}

-- Sleep function that yields in GUI mode and sleeps in CLI mode.
-- In GUI mode the caller should resume the coroutine from the main loop/timer.
function M.sleep(ms)
    ms = ms or 10
    if _G.gui_mode then
        coroutine.yield()
    else
        -- CLI fallback: simple blocking sleep
        -- Use LuaSocket if available, otherwise fall back to os.execute
        local ok, socket = pcall(require, "socket")
        if ok then
            socket.sleep(ms / 1000.0)
        else
            local cmd = string.format("sleep %f 2>/dev/null", ms / 1000.0)
            os.execute(cmd)
        end
    end
end

-- Perform an async HTTP request and return the response body.
-- This function is intended to be called from inside a coroutine.
function M.request(url, method, headers, body)
    local req = opencode.http_request(url, method, headers, body)
    if not req then
        return nil, "failed to start request"
    end
    while true do
        local status = opencode.http_poll(req)
        if status == 1 then
            local resp = opencode.http_response(req)
            opencode.http_free(req)
            return resp, nil
        elseif status == -1 then
            opencode.http_free(req)
            return nil, "http request failed"
        end
        M.sleep(10)
    end
end

return M

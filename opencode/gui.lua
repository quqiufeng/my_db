-- LuaJIT FFI binding for libopencode_gui.so
-- Provides a Lua-facing GUI object backed by egui/eframe.

local ffi = require("ffi")
local cjson = require("cjson")

ffi.cdef[[
    void* gui_app_create(const char* config_json);
    void  gui_app_free(void* app);

    void gui_on_user_message(
        void* app,
        void (*callback)(const char* session_id, const char* text, void* userdata),
        void* userdata
    );

    void gui_on_tool_call(
        void* app,
        char* (*callback)(const char* session_id, const char* tool_json, void* userdata),
        void* userdata
    );

    int gui_run(void* app);

    void gui_stream_delta(void* app, const char* session_id, const char* delta);
    void gui_append_message(void* app, const char* session_id, const char* role, const char* text);
    void gui_tool_output(void* app, const char* session_id, const char* tool_id, const char* output);
]]

local lib = ffi.load("opencode_gui")

local M = {}

-- Holds references to prevent LuaJIT from GCing callbacks.
M._apps = {}

-- Default user-message handler. Overridden via M.on_user_message().
M._default_user_handler = function(session_id, text)
    print("[GUI user message] " .. session_id .. ": " .. text)
end

-- Default tool-call handler. Overridden via M.on_tool_call().
M._default_tool_handler = function(session_id, tool_call)
    print("[GUI tool call] " .. session_id .. ": " .. cjson.encode(tool_call))
    return cjson.encode({ ok = false, error = "no tool handler" })
end

-- C-compatible callback for user messages.
local function make_user_callback(lua_handler)
    return ffi.cast("void (*)(const char*, const char*, void*)", function(session_id, text, userdata)
        local s = ffi.string(session_id)
        local t = ffi.string(text)
        local ok, err = pcall(lua_handler, s, t)
        if not ok then
            print("[GUI] user message handler error: " .. tostring(err))
        end
    end)
end

-- C-compatible callback for tool calls.
local function make_tool_callback(lua_handler)
    return ffi.cast("char* (*)(const char*, const char*, void*)", function(session_id, tool_json, userdata)
        local s = ffi.string(session_id)
        local t = ffi.string(tool_json)
        local ok, result = pcall(lua_handler, s, cjson.decode(t))
        local out
        if ok then
            out = cjson.encode(result)
        else
            out = cjson.encode({ ok = false, error = tostring(result) })
        end
        -- C side will free this with libc::free if we allocate via malloc,
        -- but our C code currently does not free. Use a static buffer trick
        -- or change C to copy. For now, return a Lua-owned C string.
        local cstr = ffi.new("char[?]", #out + 1)
        ffi.copy(cstr, out)
        return cstr
    end)
end

function M.create(config)
    config = config or {}
    local cfg_json = cjson.encode(config)
    local app = lib.gui_app_create(cfg_json)
    if app == nil then
        error("failed to create GUI app")
    end
    local handle = ffi.cast("void*", app)
    M._apps[handle] = {
        handle = handle,
        user_cb = nil,
        tool_cb = nil,
    }
    return handle
end

function M.free(app)
    local state = M._apps[app]
    if state then
        if state.user_cb then state.user_cb:free() end
        if state.tool_cb then state.tool_cb:free() end
        M._apps[app] = nil
    end
    lib.gui_app_free(app)
end

function M.on_user_message(app, handler)
    local state = M._apps[app]
    if not state then error("unknown GUI app") end
    if state.user_cb then state.user_cb:free() end
    state.user_cb = make_user_callback(handler)
    lib.gui_on_user_message(app, state.user_cb, nil)
end

function M.on_tool_call(app, handler)
    local state = M._apps[app]
    if not state then error("unknown GUI app") end
    if state.tool_cb then state.tool_cb:free() end
    state.tool_cb = make_tool_callback(handler)
    lib.gui_on_tool_call(app, state.tool_cb, nil)
end

function M.run(app)
    return lib.gui_run(app)
end

function M.stream_delta(app, session_id, delta)
    lib.gui_stream_delta(app, session_id, delta)
end

function M.append_message(app, session_id, role, text)
    lib.gui_append_message(app, session_id, role, text)
end

function M.tool_output(app, session_id, tool_id, output)
    lib.gui_tool_output(app, session_id, tool_id, output)
end

return M

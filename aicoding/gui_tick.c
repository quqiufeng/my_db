#include "gui_tick.h"

#include <lua.h>
#include <lauxlib.h>

void opencode_gui_tick(void* lua_state) {
    lua_State* L = (lua_State*)lua_state;
    if (!L) return;

    lua_getglobal(L, "gui_tick");
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            fprintf(stderr, "[GUI TICK ERROR] %s\n", err ? err : "unknown");
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}

void opencode_gui_notify_copy(void* lua_state, const char* text) {
    lua_State* L = (lua_State*)lua_state;
    if (!L || !text) return;

    lua_getglobal(L, "gui_on_copy");
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, text);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            fprintf(stderr, "[GUI COPY ERROR] %s\n", err ? err : "unknown");
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}

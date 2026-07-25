#include "plugin.h"
#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <errno.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/* ===================================================================
 * Lua C 函数：暴露给脚本的 API
 * =================================================================== */

/* agent_log(level, msg) */
static int l_agent_log(lua_State *L) {
    const char *level = luaL_checkstring(L, 1);
    const char *msg   = luaL_checkstring(L, 2);
    agent_log(level, "[plugin] %s", msg);
    return 0;
}

/* agent_role() -> string */
static int l_agent_role(lua_State *L) {
    lua_pushstring(L, g_agent.ctx.role);
    return 1;
}

/* agent_leader() -> string */
static int l_agent_leader(lua_State *L) {
    lua_pushstring(L, g_agent.ctx.leader_addr[0] ? g_agent.ctx.leader_addr : "none");
    return 1;
}

/* agent_self_addr() -> string */
static int l_agent_self(lua_State *L) {
    lua_pushstring(L, g_agent.ctx.self_addr);
    return 1;
}

/* agent_exec(cmd) -> string (stdout) */
static int l_agent_exec(lua_State *L) {
    const char *cmd = luaL_checkstring(L, 1);
    char buf[4096] = {0};
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        lua_pushstring(L, "");
        return 1;
    }
    size_t pos = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) && pos < sizeof(buf) - 256) {
        size_t llen = strlen(line);
        memcpy(buf + pos, line, llen);
        pos += llen;
    }
    pclose(fp);
    lua_pushstring(L, buf);
    return 1;
}

/* agent_get_state() -> table */
static int l_agent_get_state(lua_State *L) {
    lua_createtable(L, 0, 6);

    lua_pushstring(L, "role"); lua_pushstring(L, g_agent.ctx.role); lua_settable(L, -3);
    lua_pushstring(L, "epoch"); lua_pushinteger(L, g_agent.ctx.epoch); lua_settable(L, -3);
    lua_pushstring(L, "leader"); lua_pushstring(L, g_agent.ctx.leader_addr[0] ? g_agent.ctx.leader_addr : ""); lua_settable(L, -3);
    lua_pushstring(L, "self"); lua_pushstring(L, g_agent.ctx.self_addr); lua_settable(L, -3);
    lua_pushstring(L, "node_id"); lua_pushstring(L, g_agent.ctx.node_id); lua_settable(L, -3);
    lua_pushstring(L, "cluster_size"); lua_pushinteger(L, g_agent.ctx.cluster_size); lua_settable(L, -3);
    return 1;
}

/* agent_on_message(type, function) — 注册消息处理器 */
static int l_agent_on_message(lua_State *L) {
    const char *type = luaL_checkstring(L, 1);
    if (!lua_isfunction(L, 2))
        return luaL_error(L, "agent_on_message: second arg must be function");

    /* 存储到全局表 _message_handlers[type] = fn */
    lua_getglobal(L, "_message_handlers");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "_message_handlers");
    }
    lua_pushstring(L, type);
    lua_pushvalue(L, 2);
    lua_settable(L, -3);
    agent_log("DEBUG", "[plugin] registered on_message handler for '%s'", type);
    return 0;
}

/* agent_on_tick(function) — 注册 tick 回调 */
static int l_agent_on_tick(lua_State *L) {
    if (!lua_isfunction(L, 1))
        return luaL_error(L, "agent_on_tick: arg must be function");
    lua_pushvalue(L, 1);
    lua_setglobal(L, "_tick_handler");
    return 0;
}

/* agent_set_timer(ms, function) — 一次性定时器 */
static int l_agent_set_timer(lua_State *L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (!lua_isfunction(L, 2))
        return luaL_error(L, "agent_set_timer: second arg must be function");
    /* store in global _timers table */
    lua_getglobal(L, "_timers");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "_timers");
    }
    int idx = (int)lua_objlen(L, -1) + 1;
    lua_pushinteger(L, idx);
    lua_createtable(L, 0, 2);
    lua_pushstring(L, "deadline"); lua_pushnumber(L, now_ms() + (double)ms); lua_settable(L, -3);
    lua_pushstring(L, "fn"); lua_pushvalue(L, 2); lua_settable(L, -3);
    lua_settable(L, -3);
    return 0;
}

/* Lua API 函数表 */
static const struct luaL_Reg agent_lib[] = {
    {"log",          l_agent_log},
    {"role",         l_agent_role},
    {"leader",       l_agent_leader},
    {"self_addr",    l_agent_self},
    {"exec",         l_agent_exec},
    {"get_state",    l_agent_get_state},
    {"on_message",   l_agent_on_message},
    {"on_tick",      l_agent_on_tick},
    {"set_timer",    l_agent_set_timer},
    {NULL, NULL}
};

/* ===================================================================
 * 插件管理器实现
 * =================================================================== */

int plugin_init(agent_state_t *state) {
    plugin_manager_t *pm = &state->plugin_mgr;
    memset(pm, 0, sizeof(*pm));

    /* 使用配置的插件目录，或默认 */
    const char *dir = state->config.plugin_dir;
    if (!dir || dir[0] == '\0') dir = PLUGIN_DIR_DEFAULT;
    snprintf(pm->plugin_dir, sizeof(pm->plugin_dir), "%s", dir);

    /* 创建 Lua 状态 */
    pm->L = luaL_newstate();
    if (!pm->L) {
        agent_log("WARN", "[plugin] failed to create Lua state");
        return -1;
    }
    luaL_openlibs(pm->L);

    /* sandbox: remove dangerous globals/functions */
    lua_pushnil(pm->L); lua_setglobal(pm->L, "dofile");
    lua_pushnil(pm->L); lua_setglobal(pm->L, "loadfile");
    lua_pushnil(pm->L); lua_setglobal(pm->L, "load");
    lua_pushnil(pm->L); lua_setglobal(pm->L, "io");
    /* restrict os table: nil out dangerous functions, keep clock/time */
    lua_getglobal(pm->L, "os");
    if (lua_istable(pm->L, -1)) {
        lua_pushnil(pm->L); lua_setfield(pm->L, -2, "execute");
        lua_pushnil(pm->L); lua_setfield(pm->L, -2, "exit");
        lua_pushnil(pm->L); lua_setfield(pm->L, -2, "tmpname");
        lua_pushnil(pm->L); lua_setfield(pm->L, -2, "rename");
        lua_pushnil(pm->L); lua_setfield(pm->L, -2, "remove");
    }
    lua_pop(pm->L, 1);  /* pop os table */
    
    /* add cjson to package.path */    /* add cjson to package.path */
    lua_getglobal(pm->L, "package");
    if (lua_istable(pm->L, -1)) {
        lua_getfield(pm->L, -1, "path");
        const char *cur = lua_tostring(pm->L, -1);
        if (cur) {
            char newpath[2048];
            snprintf(newpath, sizeof(newpath), "/usr/local/lualib/?.so;%s", cur);
            lua_pushstring(pm->L, newpath);
            lua_setfield(pm->L, -3, "path");
        }
        lua_pop(pm->L, 1);
    }
    lua_pop(pm->L, 1);

    /* 注册 agent 库 */
    lua_newtable(pm->L);
    luaL_setfuncs(pm->L, agent_lib, 0);
    lua_setglobal(pm->L, "agent");

    /* 初始化全局表 */
    lua_newtable(pm->L); lua_setglobal(pm->L, "_message_handlers");
    lua_newtable(pm->L); lua_setglobal(pm->L, "_timers");
    lua_pushnil(pm->L);  lua_setglobal(pm->L, "_tick_handler");

    /* 创建插件目录 */
    mkdir(pm->plugin_dir, 0755);

    /* 加载所有插件 */
    int n = plugin_load_all(state);
    agent_log("INFO", "[plugin] initialized, %d plugins loaded from %s", n, pm->plugin_dir);

    /* 初始化 inotify 热更新监控 */
    pm->inotify_fd = inotify_init1(IN_NONBLOCK);
    if (pm->inotify_fd >= 0) {
        pm->watch_fd = inotify_add_watch(pm->inotify_fd, pm->plugin_dir, IN_CLOSE_WRITE | IN_MOVED_TO);
        if (pm->watch_fd < 0)
            agent_log("WARN", "[plugin] inotify watch failed: %s", strerror(errno));
    }

    /* 注册到 epoll */
    if (pm->inotify_fd >= 0 && state->epoll_fd >= 0) {
        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = pm->inotify_fd;
        epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, pm->inotify_fd, &ev);
    }

    return n >= 0 ? 0 : -1;
}

int plugin_load_all(agent_state_t *state) {
    plugin_manager_t *pm = &state->plugin_mgr;
    DIR *dir = opendir(pm->plugin_dir);
    if (!dir) {
        agent_log("DEBUG", "[plugin] plugin dir %s not found", pm->plugin_dir);
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < MAX_PLUGINS) {
        int len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 4, ".lua") != 0)
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", pm->plugin_dir, entry->d_name);
        if (plugin_load_one(state, path) == 0)
            count++;
    }
    closedir(dir);
    return count;
}

int plugin_load_one(agent_state_t *state, const char *path) {
    plugin_manager_t *pm = &state->plugin_mgr;

    /* 获取文件信息 */
    struct stat st;
    if (stat(path, &st) < 0) return -1;

    /* 检查是否已加载（重载时用） */
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    if (luaL_dofile(pm->L, path) != LUA_OK) {
        agent_log("WARN", "[plugin] load %s failed: %s", name, lua_tostring(pm->L, -1));
        lua_pop(pm->L, 1);
        return -1;
    }

    /* 注册到插件列表 */
    int idx = -1;
    for (int i = 0; i < pm->plugin_count; i++) {
        if (strcmp(pm->plugins[i].name, name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (pm->plugin_count >= MAX_PLUGINS) return -1;
        idx = pm->plugin_count++;
    }

    snprintf(pm->plugins[idx].name, sizeof(pm->plugins[idx].name), "%s", name);
    snprintf(pm->plugins[idx].path, sizeof(pm->plugins[idx].path), "%s", path);
    pm->plugins[idx].mtime  = st.st_mtime;
    pm->plugins[idx].active = 1;
    agent_log("INFO", "[plugin] loaded: %s", name);
    return 0;
}

void plugin_tick(agent_state_t *state, long now) {
    plugin_manager_t *pm = &state->plugin_mgr;
    if (!pm->L) return;

    /* 调用 _tick_handler */
    lua_getglobal(pm->L, "_tick_handler");
    if (lua_isfunction(pm->L, -1)) {
        lua_pushinteger(pm->L, now);
        if (lua_pcall(pm->L, 1, 0, 0) != LUA_OK) {
            agent_log("WARN", "[plugin] tick error: %s", lua_tostring(pm->L, -1));
            lua_pop(pm->L, 1);
        }
    } else {
        lua_pop(pm->L, 1);
    }

    /* 检查定时器 */
    lua_getglobal(pm->L, "_timers");
    if (lua_istable(pm->L, -1)) {
        lua_pushnil(pm->L);
        while (lua_next(pm->L, -2) != 0) {
            /* key=idx, value={deadline=..., fn=...} */
            lua_getfield(pm->L, -1, "deadline");
            double deadline = lua_tonumber(pm->L, -1);
            lua_pop(pm->L, 1);

            if (now >= deadline) {
                lua_getfield(pm->L, -1, "fn");
                if (lua_isfunction(pm->L, -1)) {
                    if (lua_pcall(pm->L, 0, 0, 0) != LUA_OK) {
                        agent_log("WARN", "[plugin] timer error: %s", lua_tostring(pm->L, -1));
                        lua_pop(pm->L, 1);
                    }
                }
                /* remove timer by setting _timers[key] = nil */
                lua_pushvalue(pm->L, -2);  /* push key (idx) */
                lua_pushnil(pm->L);
                lua_settable(pm->L, -5);   /* _timers is at -5 */
            }
            lua_pop(pm->L, 1);
        }
    }
    lua_pop(pm->L, 1);

    /* 检查热更新（inotify 事件由 handle_read 处理）*/
}

void plugin_check_hotreload(agent_state_t *state) {
    plugin_manager_t *pm = &state->plugin_mgr;
    if (pm->inotify_fd < 0) return;

    /* 读取 inotify 事件，每次收到事件就重新加载 */
    uint8_t buf[4096];
    int ret = read(pm->inotify_fd, buf, sizeof(buf));
    if (ret <= 0) return;

    /* 重新加载所有文件（检查 mtime）*/
    for (int i = 0; i < pm->plugin_count; i++) {
        struct stat st;
        if (stat(pm->plugins[i].path, &st) < 0) continue;
        if (st.st_mtime <= pm->plugins[i].mtime) continue;
        agent_log("INFO", "[plugin] hot-reload: %s", pm->plugins[i].name);
        plugin_load_one(state, pm->plugins[i].path);
    }
}

int plugin_handle_message(agent_state_t *state, const char *type,
                           const char *payload, uint32_t len) {
    plugin_manager_t *pm = &state->plugin_mgr;
    if (!pm->L) return 0;

    lua_getglobal(pm->L, "_message_handlers");
    if (!lua_istable(pm->L, -1)) { lua_pop(pm->L, 1); return 0; }

    lua_pushstring(pm->L, type);
    lua_gettable(pm->L, -2);

    if (lua_isfunction(pm->L, -1)) {
        lua_pushlstring(pm->L, payload, len);
        if (lua_pcall(pm->L, 1, 1, 0) != LUA_OK) {
            agent_log("WARN", "[plugin] on_message(%s) error: %s", type, lua_tostring(pm->L, -1));
            lua_pop(pm->L, 1);
            lua_pop(pm->L, 1);
            return 0;
        }
        int handled = lua_toboolean(pm->L, -1);
        lua_pop(pm->L, 2);
        return handled;
    }
    lua_pop(pm->L, 2);
    return 0;
}

void plugin_destroy(agent_state_t *state) {
    plugin_manager_t *pm = &state->plugin_mgr;
    if (pm->inotify_fd >= 0) close(pm->inotify_fd);
    if (pm->L) lua_close(pm->L);
    memset(pm, 0, sizeof(*pm));
}

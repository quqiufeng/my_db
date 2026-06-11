#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifndef PLUGIN_H
#define PLUGIN_H

#include "protocol.h"
#include "peer.h"
#include "election.h"
struct agent_state;

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * Plugin 系统：LuaJIT 热更新脚本
 *
 * 插件放在 --plugin-dir 目录下（默认 /etc/agent/plugins/）
 * 每个 .lua 文件是一个插件，支持：
 *   - agent_on_message(type, fn)  注册消息处理器
 *   - agent_on_tick(fn)           注册 tick 回调
 *   - agent_log(level, msg)       日志输出
 *   - agent_exec(cmd)             执行命令
 *   - agent_role()                获取当前角色
 *   - agent_leader()              获取 Leader 地址
 *   - agent_set_timer(ms, fn)     一次性定时器
 *
 * inotify 监控文件变更，自动热重载
 * =================================================================== */

/* 插件配置 */
#define PLUGIN_DIR_DEFAULT  "/etc/agent/plugins"
#define MAX_PLUGIN_NAME     64
#define MAX_PLUGINS         32

typedef struct {
    char     name[MAX_PLUGIN_NAME];   /* 文件名（不含路径） */
    char     path[512];               /* 完整路径 */
    long     mtime;                   /* 文件修改时间（用于重载检测） */
    int      active;                  /* 1=加载成功 */
} plugin_info_t;

/* 插件管理器 */
typedef struct {
    lua_State      *L;                /* 主 Lua 状态 */
    plugin_info_t   plugins[MAX_PLUGINS];
    int             plugin_count;

    /* 热更新监控 */
    int             inotify_fd;
    int             watch_fd;
    char            plugin_dir[512];
} plugin_manager_t;

/* ===================================================================
 * 函数声明
 * =================================================================== */

/**
 * plugin_init - 初始化 Lua 状态 + 加载插件
 * @state: agent 状态（含 config.plugin_dir）
 * 返回 0 成功，-1 失败（会记录日志但不会让 agent 崩溃）
 */
int  plugin_init(struct agent_state *state);

/**
 * plugin_load_all - 从 plugin_dir 加载所有 .lua 文件
 * 返回加载的插件数
 */
int  plugin_load_all(struct agent_state *state);

/**
 * plugin_load_one - 加载单个 .lua 文件
 */
int  plugin_load_one(struct agent_state *state, const char *path);

/**
 * plugin_tick - 每轮事件循环调用，触发 on_tick 回调 + 检查热更新
 */
void plugin_tick(struct agent_state *state, long now);

/**
 * plugin_handle_message - 消息路由到插件注册的 on_message 处理器
 * 返回 1=已处理，0=未处理
 */
int  plugin_handle_message(struct agent_state *state, const char *type,
                           const char *payload, uint32_t len);

/**
 * plugin_destroy - 关闭 Lua 状态
 */
void plugin_destroy(struct agent_state *state);

/**
 * plugin_check_hotreload - 检查文件变更并重新加载
 */
void plugin_check_hotreload(struct agent_state *state);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_H */

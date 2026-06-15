#ifndef OPCODE_GUI_TICK_H
#define OPCODE_GUI_TICK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Tick the GUI Lua coroutines. Called from the Rust GPUI timer. */
void opencode_gui_tick(void* lua_state);

/* Notify Lua that the user double-clicked a message block to copy it. */
void opencode_gui_notify_copy(void* lua_state, const char* text);

#ifdef __cplusplus
}
#endif

#endif

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::sync::{Arc, Mutex};

use gpui::*;
use gpui_component::{
    button::{Button, ButtonVariants},
    input::{Input, InputState, TabSize},
    scroll::ScrollableElement,
    v_flex, ActiveTheme, Root,
};
use serde_json::Value;

/// Opaque handle to the GUI runtime.
pub struct GuiApp {
    /// User message callback: fn(session_id, text, userdata)
    on_user_message: Option<extern "C" fn(*const c_char, *const c_char, *mut c_void)>,
    user_data: *mut c_void,

    /// Tool call callback: fn(session_id, tool_json, userdata) -> *char result_json
    on_tool_call: Option<extern "C" fn(*const c_char, *const c_char, *mut c_void) -> *mut c_char>,

    /// Chat messages per session.
    messages: Arc<Mutex<Vec<MessageRow>>>,

    /// Current input text buffer.
    input: Arc<Mutex<String>>,

    /// Window title.
    title: String,
}

#[derive(Clone)]
struct MessageRow {
    role: String,
    text: String,
    tool_id: Option<String>,
}

impl GuiApp {
    fn append(&self, role: &str, text: String, tool_id: Option<String>) {
        self.messages.lock().unwrap().push(MessageRow {
            role: role.to_string(),
            text,
            tool_id,
        });
    }
}

unsafe impl Send for GuiApp {}
unsafe impl Sync for GuiApp {}

#[no_mangle]
pub extern "C" fn gui_app_create(config_json: *const c_char) -> *mut c_void {
    if config_json.is_null() {
        return std::ptr::null_mut();
    }
    let config_str = unsafe { CStr::from_ptr(config_json).to_string_lossy() };
    let title = match serde_json::from_str::<Value>(&config_str) {
        Ok(Value::Object(m)) => m
            .get("title")
            .and_then(|v| v.as_str())
            .unwrap_or("opencode")
            .to_string(),
        _ => "opencode".to_string(),
    };

    let app = GuiApp {
        on_user_message: None,
        user_data: std::ptr::null_mut(),
        on_tool_call: None,
        messages: Arc::new(Mutex::new(Vec::new())),
        input: Arc::new(Mutex::new(String::new())),
        title,
    };

    Box::into_raw(Box::new(app)) as *mut c_void
}

#[no_mangle]
pub extern "C" fn gui_app_free(app: *mut c_void) {
    if !app.is_null() {
        unsafe { drop(Box::from_raw(app as *mut GuiApp)) };
    }
}

#[no_mangle]
pub extern "C" fn gui_on_user_message(
    app: *mut c_void,
    callback: extern "C" fn(*const c_char, *const c_char, *mut c_void),
    userdata: *mut c_void,
) {
    if app.is_null() {
        return;
    }
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };
    app.on_user_message = Some(callback);
    app.user_data = userdata;
}

#[no_mangle]
pub extern "C" fn gui_on_tool_call(
    app: *mut c_void,
    callback: extern "C" fn(*const c_char, *const c_char, *mut c_void) -> *mut c_char,
    userdata: *mut c_void,
) {
    if app.is_null() {
        return;
    }
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };
    app.on_tool_call = Some(callback);
    app.user_data = userdata;
}

#[no_mangle]
pub extern "C" fn gui_stream_delta(
    app: *mut c_void,
    _session_id: *const c_char,
    delta: *const c_char,
) {
    if app.is_null() || delta.is_null() {
        return;
    }
    let delta_str = unsafe { CStr::from_ptr(delta).to_string_lossy().to_string() };
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };
    let mut msgs = app.messages.lock().unwrap();
    if let Some(last) = msgs.last_mut() {
        if last.role == "assistant" {
            last.text.push_str(&delta_str);
            return;
        }
    }
    msgs.push(MessageRow {
        role: "assistant".to_string(),
        text: delta_str,
        tool_id: None,
    });
}

#[no_mangle]
pub extern "C" fn gui_append_message(
    app: *mut c_void,
    _session_id: *const c_char,
    role: *const c_char,
    text: *const c_char,
) {
    if app.is_null() || role.is_null() || text.is_null() {
        return;
    }
    let role_str = unsafe { CStr::from_ptr(role).to_string_lossy().to_string() };
    let text_str = unsafe { CStr::from_ptr(text).to_string_lossy().to_string() };
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };
    app.append(&role_str, text_str, None);
}

#[no_mangle]
pub extern "C" fn gui_tool_output(
    app: *mut c_void,
    _session_id: *const c_char,
    tool_id: *const c_char,
    output: *const c_char,
) {
    if app.is_null() || tool_id.is_null() || output.is_null() {
        return;
    }
    let tool_id_str = unsafe { CStr::from_ptr(tool_id).to_string_lossy().to_string() };
    let output_str = unsafe { CStr::from_ptr(output).to_string_lossy().to_string() };
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };
    app.append(
        "tool",
        format!("[{}] {}", tool_id_str, output_str),
        Some(tool_id_str),
    );
}

struct ChatView {
    app: *mut GuiApp,
    input_state: Entity<InputState>,
}

impl Render for ChatView {
    fn render(&mut self,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let app = self.app;
        let app_ref: &GuiApp = unsafe { &*app };
        let messages = app_ref.messages.lock().unwrap().clone();
        let theme_colors = cx.theme().colors.clone();

        v_flex()
            .size_full()
            .gap_2()
            .p_4()
            .child("opencode GUI")
            .child(
                div()
                    .flex_1()
                    .overflow_y_scrollbar()
                    .child(
                        v_flex()
                            .gap_2()
                            .children(messages.into_iter().map(move |m| {
                                let (label, color) = match m.role.as_str() {
                                    "user" => ("User", theme_colors.primary),
                                    "assistant" => ("Assistant", theme_colors.foreground),
                                    "tool" => ("Tool", theme_colors.success),
                                    _ => ("Unknown", theme_colors.foreground),
                                };
                                v_flex()
                                    .gap_1()
                                    .child(div().text_color(color).child(label.to_string()))
                                    .child(m.text)
                            })),
                    ),
            )
            .child(
                div()
                    .h_32()
                    .child(Input::new(&self.input_state,
                    ).h_full()),
            )
            .child(
                Button::new("send")
                    .primary()
                    .label("Send")
                    .on_click(cx.listener(move |this, _event, window, cx| {
                        let text = this.input_state.read(cx).value().to_string();
                        if text.is_empty() {
                            return;
                        }
                        this.input_state.update(cx, |state, _cx| {
                            state.set_value("", window, _cx);
                        });
                        let app_mut: &mut GuiApp = unsafe { &mut *app };
                        app_mut.append("user", text.clone(), None);
                        if let Some(cb) = app_mut.on_user_message {
                            let s = CString::new("default").unwrap();
                            let t = CString::new(text).unwrap();
                            cb(s.as_ptr(), t.as_ptr(), app_mut.user_data);
                        }
                    })),
            )
    }
}

#[no_mangle]
pub extern "C" fn gui_run(app_ptr: *mut c_void) -> c_int {
    if app_ptr.is_null() {
        return -1;
    }
    let app_ptr = app_ptr as *mut GuiApp;

    gpui_platform::application().run(move |cx: &mut App| {
        gpui_component::init(cx);
        cx.activate(true);

        let bounds = Bounds::centered(None, size(px(900.0), px(600.0)), cx);
        let title = unsafe { (*app_ptr).title.clone() };

        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                ..Default::default()
            },
            move |window, cx| {
                let input_state = cx.new(|cx| {
                    InputState::new(window, cx)
                        .code_editor("markdown")
                        .multi_line(true)
                        .tab_size(TabSize { tab_size: 4, ..Default::default() })
                });
                let chat_view = cx.new(|_cx| ChatView {
                    app: app_ptr,
                    input_state,
                });
                cx.new(|cx| Root::new(chat_view, window, cx))
            },
        )
        .ok();
    });

    0
}

#[no_mangle]
pub extern "C" fn gui_refresh(_app: *mut c_void) {}

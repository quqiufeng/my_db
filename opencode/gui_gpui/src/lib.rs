use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, mpsc};

use gpui::prelude::FluentBuilder;
use gpui::{InteractiveElement as _, StatefulInteractiveElement as _, *};
use gpui_component::{
    button::{Button, ButtonVariants},
    input::{Input, InputEvent, InputState, TabSize},
    scroll::ScrollableElement,
    v_flex, h_flex, ActiveTheme, Root, Sizable,
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

    /// Window title.
    title: String,

    /// Project root for the file explorer.
    project_root: PathBuf,

    /// Cached file list for the file explorer.
    files: Arc<Mutex<Vec<PathBuf>>>,

    /// Weak handle to the ChatView entity so C callbacks can notify the UI.
    view: Mutex<Option<WeakEntity<ChatView>>>,

    /// Foreground executor captured during gui_run so C callbacks can dispatch
    /// UI updates to the GPUI main thread.
    executor: Mutex<Option<ForegroundExecutor>>,

    /// Sender to notify the foreground task that the UI needs a redraw.
    ui_tx: Mutex<Option<mpsc::Sender<()>>>,
}

#[derive(Clone)]
struct MessageRow {
    role: String,
    text: String,
    tool_id: Option<String>,
    expanded: bool,
}

impl GuiApp {
    fn append(&self, role: &str, text: String, tool_id: Option<String>) {
        self.messages.lock().unwrap().push(MessageRow {
            role: role.to_string(),
            text,
            tool_id,
            expanded: false,
        });
    }

    fn refresh_ui(&self) {
        if let Ok(guard) = self.ui_tx.lock() {
            if let Some(tx) = guard.as_ref() {
                let _ = tx.send(());
            }
        }
    }
}

unsafe impl Send for GuiApp {}
unsafe impl Sync for GuiApp {}

fn list_files(root: &Path) -> Vec<PathBuf> {
    let mut result = Vec::new();
    if root.is_dir() {
        if let Ok(entries) = std::fs::read_dir(root) {
            for entry in entries.flatten() {
                let path = entry.path();
                if path.is_dir() {
                    result.push(path.clone());
                    result.extend(list_files(&path));
                } else {
                    result.push(path);
                }
            }
        }
    }
    result
}

#[no_mangle]
pub extern "C" fn gui_app_create(config_json: *const c_char) -> *mut c_void {
    if config_json.is_null() {
        return std::ptr::null_mut();
    }
    let config_str = unsafe { CStr::from_ptr(config_json).to_string_lossy() };
    let (title, project_root) = match serde_json::from_str::<Value>(&config_str) {
        Ok(Value::Object(m)) => {
            let title = m
                .get("title")
                .and_then(|v| v.as_str())
                .unwrap_or("opencode")
                .to_string();
            let root = m
                .get("project_root")
                .and_then(|v| v.as_str())
                .map(PathBuf::from)
                .unwrap_or_else(|| PathBuf::from("."));
            (title, root)
        }
        _ => ("opencode".to_string(), PathBuf::from(".")),
    };

    let files = list_files(&project_root);

    let app = GuiApp {
        on_user_message: None,
        user_data: std::ptr::null_mut(),
        on_tool_call: None,
        messages: Arc::new(Mutex::new(Vec::new())),
        title,
        project_root: project_root.clone(),
        files: Arc::new(Mutex::new(files)),
        view: Mutex::new(None),
        executor: Mutex::new(None),
        ui_tx: Mutex::new(None),
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
    let app: &GuiApp = unsafe { &*(app as *mut GuiApp) };
    {
        let mut msgs = app.messages.lock().unwrap();
        if let Some(last) = msgs.last_mut() {
            if last.role == "assistant" {
                last.text.push_str(&delta_str);
            } else {
                msgs.push(MessageRow {
                    role: "assistant".to_string(),
                    text: delta_str,
                    tool_id: None,
                    expanded: false,
                });
            }
        } else {
            msgs.push(MessageRow {
                role: "assistant".to_string(),
                text: delta_str,
                tool_id: None,
                expanded: false,
            });
        }
    }
    app.refresh_ui();
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
    let app: &GuiApp = unsafe { &*(app as *mut GuiApp) };
    app.append(&role_str, text_str, None);
    app.refresh_ui();
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
    let app: &GuiApp = unsafe { &*(app as *mut GuiApp) };
    app.append(
        "tool",
        format!("[{}] {}", tool_id_str, output_str),
        Some(tool_id_str),
    );
    app.refresh_ui();
}

fn notify_app(_app: &GuiApp) {
    // Kept for ABI compatibility; refresh_ui handles notification.
}

struct ChatView {
    app: *mut GuiApp,
    input_state: Entity<InputState>,
    editor_state: Entity<InputState>,
    editor_path: Option<PathBuf>,
    editor_visible: bool,
}

impl ChatView {
    fn send_message(&mut self,
        text: String,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if text.is_empty() {
            return;
        }
        self.input_state.update(cx, |state, _cx| {
            state.set_value("", window, _cx);
        });
        {
            let app: &mut GuiApp = unsafe { &mut *self.app };
            app.append("user", text.clone(), None);
            if let Some(cb) = app.on_user_message {
                let s = CString::new("default").unwrap();
                let t = CString::new(text).unwrap();
                cb(s.as_ptr(), t.as_ptr(), app.user_data);
            }
        }
        cx.notify();
    }

    fn open_file(&mut self,
        path: PathBuf,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let content = std::fs::read_to_string(&path).unwrap_or_else(|e| format!("Error reading file: {}", e));
        let lang = language_for_path(&path);
        self.editor_state.update(cx, |state, _cx| {
            state.set_value(content, window, _cx);
        });
        self.editor_path = Some(path);
        self.editor_visible = true;
        let _ = lang;
        cx.notify();
    }
}

fn language_for_path(path: &Path) -> &'static str {
    match path.extension().and_then(|e| e.to_str()) {
        Some("rs") => "rust",
        Some("py") => "python",
        Some("js") | Some("ts") | Some("jsx") | Some("tsx") => "javascript",
        Some("c") | Some("h") | Some("cpp") | Some("hpp") | Some("cc") => "c",
        Some("go") => "go",
        Some("lua") => "lua",
        Some("md") | Some("markdown") => "markdown",
        Some("json") => "json",
        Some("toml") => "toml",
        Some("yaml") | Some("yml") => "yaml",
        _ => "plaintext",
    }
}

impl Render for ChatView {
    fn render(
        &mut self,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let app = self.app;
        let app_ref: &GuiApp = unsafe { &*app };
        let messages = app_ref.messages.lock().unwrap().clone();
        let files = app_ref.files.lock().unwrap().clone();
        let project_root = app_ref.project_root.clone();
        let theme = cx.theme().clone();

        let message_list = v_flex()
            .flex_1()
            .overflow_y_scrollbar()
            .gap_2()
            .p_2()
            .children(messages.into_iter().enumerate().map(move |(idx, mut m)| {
                let role = m.role.clone();
                let (label, bg, fg) = match role.as_str() {
                    "user" => ("User", theme.colors.primary, theme.colors.background),
                    "assistant" => ("Assistant", theme.colors.muted, theme.colors.foreground),
                    "tool" => ("Tool", theme.colors.success, theme.colors.background),
                    _ => ("Unknown", theme.colors.foreground, theme.colors.background),
                };
                let is_tool = role == "tool";
                let expanded = m.expanded;
                let display_text = m.text.clone();
                v_flex()
                    .gap_1()
                    .rounded_md()
                    .p_2()
                    .bg(bg)
                    .text_color(fg)
                    .child(div().font_weight(FontWeight::BOLD).child(label.to_string()))
                    .when(is_tool, |this| {
                        let header = display_text.split('\n').next().unwrap_or("").to_string();
                        this.child(
                            Button::new(format!("toggle-{}", idx))
                                .xsmall()
                                .ghost()
                                .label(if expanded { "Collapse" } else { "Expand" }))
                        .child(header)
                        .when(expanded, |this| this.child(display_text.clone()))
                    })
                    .when(!is_tool, |this| this.child(display_text))
            }));

        let file_tree = v_flex()
            .w(px(280.0))
            .h_full()
            .border_l_1()
            .border_color(theme.colors.border)
            .child(
                div()
                    .p_2()
                    .font_weight(FontWeight::BOLD)
                    .child("Files"),
            )
            .child(
                div()
                    .flex_1()
                    .overflow_y_scrollbar()
                    .child(
                        v_flex()
                            .gap_1()
                            .p_2()
                            .children(files.into_iter().enumerate().map({
                                let cx_ref = &cx;
                                move |(idx, path)| {
                                    let rel = path
                                        .strip_prefix(&project_root)
                                        .unwrap_or(&path)
                                        .to_string_lossy()
                                        .to_string();
                                    let is_dir = path.is_dir();
                                    let icon = if is_dir { "📁" } else { "📄" };
                                    let path_for_click = path.clone();
                                    div()
                                        .id(format!("file-{}", idx))
                                        .child(format!("{} {}", icon, rel))
                                        .text_sm()
                                        .cursor_pointer()
                                        .when(!is_dir, |this| {
                                            this.on_click(cx_ref.listener(move |this, _event, window, cx| {
                                                this.open_file(path_for_click.clone(), window, cx);
                                            }))
                                        })
                                }
                            })),
                    ),
            );

        let editor_panel = v_flex()
            .h(px(240.0))
            .border_t_1()
            .border_color(theme.colors.border)
            .child(
                div()
                    .p_2()
                    .font_weight(FontWeight::BOLD)
                    .child(
                        self.editor_path
                            .as_ref()
                            .map(|p| p.file_name().unwrap_or_default().to_string_lossy().to_string())
                            .unwrap_or_else(|| "Editor".to_string()),
                    ),
            )
            .child(
                div()
                    .flex_1()
                    .child(Input::new(&self.editor_state,
                    ).h_full()),
            );

        let right_panel = v_flex()
            .w(px(280.0))
            .h_full()
            .child(file_tree)
            .when(self.editor_visible, |this| this.child(editor_panel));

        h_flex()
            .size_full()
            .gap_2()
            .p_4()
            .child(
                v_flex()
                    .flex_1()
                    .size_full()
                    .gap_2()
                    .child("opencode GUI")
                    .child(message_list)
                    .child(
                        div()
                            .h_32()
                            .child(Input::new(
                                &self.input_state,
                            ).h_full()),
                    )
                    .child(
                        Button::new("send")
                            .primary()
                            .label("Send")
                            .on_click(cx.listener(move |this, _event, window, cx| {
                                let text = this.input_state.read(cx).value().to_string();
                                this.send_message(text, window, cx);
                            })),
                    ),
            )
            .child(right_panel)
    }
}

impl EventEmitter<InputEvent> for ChatView {}

#[no_mangle]
pub extern "C" fn gui_run(app_ptr: *mut c_void) -> c_int {
    if app_ptr.is_null() {
        return -1;
    }
    let app_ptr = app_ptr as *mut GuiApp;

    gpui_platform::application().run(move |cx: &mut App| {
        gpui_component::init(cx);
        cx.activate(true);

        let executor = cx.foreground_executor().clone();
        let async_app = cx.to_async();
        {
            let app: &mut GuiApp = unsafe { &mut *app_ptr };
            *app.executor.lock().unwrap() = Some(executor);
        }

        let (ui_tx, ui_rx) = mpsc::channel::<()>();
        {
            let app: &mut GuiApp = unsafe { &mut *app_ptr };
            *app.ui_tx.lock().unwrap() = Some(ui_tx);
        }

        let bounds = Bounds::centered(None, size(px(900.0), px(600.0)), cx);

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
                let input_state_for_view = input_state.clone();
                let editor_state = cx.new(|cx| {
                    InputState::new(window, cx)
                        .code_editor("plaintext")
                        .multi_line(true)
                        .tab_size(TabSize { tab_size: 4, ..Default::default() })
                });
                let chat_view = cx.new(|cx| {
                    let view = ChatView {
                        app: app_ptr,
                        input_state,
                        editor_state,
                        editor_path: None,
                        editor_visible: false,
                    };
                    cx.subscribe_in(
                        &input_state_for_view,
                        window,
                        |this: &mut ChatView, input, ev: &InputEvent, window, cx| {
                            match ev {
                                InputEvent::PressEnter { shift, .. } if !shift => {
                                    let text = input.read(cx).value().trim().to_string();
                                    if !text.is_empty() {
                                        this.send_message(text, window, cx);
                                    }
                                }
                                _ => {}
                            }
                        },
                    )
                    .detach();
                    view
                });
                {
                    let app: &mut GuiApp = unsafe { &mut *app_ptr };
                    *app.view.lock().unwrap() = Some(chat_view.downgrade());
                }

                let weak_view = chat_view.downgrade();
                async_app
                    .spawn(async move |cx| {
                        while ui_rx.recv().is_ok() {
                            weak_view
                                .update(cx, |_this, cx| {
                                    cx.notify();
                                })
                                .ok();
                        }
                    })
                    .detach();

                cx.new(|cx| Root::new(chat_view, window, cx))
            },
        )
        .ok();
    });

    0
}

#[no_mangle]
pub extern "C" fn gui_refresh(_app: *mut c_void) {}

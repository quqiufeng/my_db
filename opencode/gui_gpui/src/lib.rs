use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

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

    /// Displayed model name (e.g. "Kimi K2.7 Code").
    model: String,

    /// Version label shown in the status bar.
    version: String,

    /// Project root shown in the status bar.
    project_root: PathBuf,

    /// Live todo list updated by the agent workflow.
    todos: Arc<Mutex<Vec<TodoItem>>>,

    /// Weak handle to the ChatView entity so C callbacks can notify the UI.
    view: Mutex<Option<WeakEntity<ChatView>>>,

    /// Foreground executor captured during gui_run so C callbacks can dispatch
    /// UI updates to the GPUI main thread.
    executor: Mutex<Option<ForegroundExecutor>>,
}

#[derive(Clone)]
enum MessageBlock {
    Text(String),
    Code { lang: String, code: String },
}

fn parse_message_blocks(text: &str) -> Vec<MessageBlock> {
    let mut blocks = Vec::new();
    let mut current = String::new();
    let mut in_code = false;
    let mut lang = String::new();
    let mut code = String::new();

    for line in text.lines() {
        if line.starts_with("```") {
            if in_code {
                // End code block
                if !current.is_empty() {
                    blocks.push(MessageBlock::Text(current.trim_end().to_string()));
                    current.clear();
                }
                blocks.push(MessageBlock::Code {
                    lang: lang.trim().to_string(),
                    code: code.trim_end().to_string(),
                });
                lang.clear();
                code.clear();
                in_code = false;
            } else {
                // Start code block
                if !current.is_empty() {
                    blocks.push(MessageBlock::Text(current.trim_end().to_string()));
                    current.clear();
                }
                lang = line.strip_prefix("```").unwrap_or("").trim().to_string();
                in_code = true;
            }
        } else if in_code {
            code.push_str(line);
            code.push('\n');
        } else {
            current.push_str(line);
            current.push('\n');
        }
    }

    if !current.is_empty() {
        blocks.push(MessageBlock::Text(current.trim_end().to_string()));
    }
    if in_code && !code.is_empty() {
        blocks.push(MessageBlock::Code {
            lang: lang.trim().to_string(),
            code: code.trim_end().to_string(),
        });
    }

    blocks
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
        if let Ok(guard) = self.executor.lock() {
            if let Some(executor) = guard.as_ref() {
                let _ = executor.spawn(async move {});
            }
        }
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
    let (title, project_root, model, version) = match serde_json::from_str::<Value>(&config_str) {
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
            let model = m
                .get("model")
                .and_then(|v| v.as_str())
                .unwrap_or("unknown")
                .to_string();
            let version = m
                .get("version")
                .and_then(|v| v.as_str())
                .unwrap_or("0.1.0")
                .to_string();
            (title, root, model, version)
        }
        _ => (
            "opencode".to_string(),
            PathBuf::from("."),
            "unknown".to_string(),
            "0.1.0".to_string(),
        ),
    };

    let app = GuiApp {
        on_user_message: None,
        user_data: std::ptr::null_mut(),
        on_tool_call: None,
        messages: Arc::new(Mutex::new(Vec::new())),
        title,
        model,
        version,
        project_root: project_root.clone(),
        todos: Arc::new(Mutex::new(Vec::new())),
        view: Mutex::new(None),
        executor: Mutex::new(None),
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

#[no_mangle]
pub extern "C" fn gui_add_todo(app: *mut c_void, text: *const c_char) {
    if app.is_null() || text.is_null() {
        return;
    }
    let text_str = unsafe { CStr::from_ptr(text).to_string_lossy().to_string() };
    let app: &GuiApp = unsafe { &*(app as *mut GuiApp) };
    app.todos.lock().unwrap().push(TodoItem {
        text: text_str,
        done: false,
    });
    app.refresh_ui();
}

#[no_mangle]
pub extern "C" fn gui_set_todo_done(app: *mut c_void, text: *const c_char, done: c_int) {
    if app.is_null() || text.is_null() {
        return;
    }
    let text_str = unsafe { CStr::from_ptr(text).to_string_lossy().to_string() };
    let app: &GuiApp = unsafe { &*(app as *mut GuiApp) };
    if let Some(todo) = app
        .todos
        .lock()
        .unwrap()
        .iter_mut()
        .find(|t| t.text == text_str)
    {
        todo.done = done != 0;
    }
    app.refresh_ui();
}

#[no_mangle]
pub extern "C" fn gui_clear_todos(app: *mut c_void) {
    if app.is_null() {
        return;
    }
    let app: &GuiApp = unsafe { &*(app as *mut GuiApp) };
    app.todos.lock().unwrap().clear();
    app.refresh_ui();
}

#[derive(Clone)]
struct TodoItem {
    text: String,
    done: bool,
}

struct ChatView {
    app: *mut GuiApp,
    input_state: Entity<InputState>,
    session_start: String,
    tokens_used: usize,
    context_tokens: usize,
}

impl ChatView {
    fn send_message(
        &mut self,
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

    fn toggle_todo(&mut self, idx: usize, cx: &mut Context<Self>) {
        let app: &mut GuiApp = unsafe { &mut *self.app };
        if let Some(todo) = app.todos.lock().unwrap().get_mut(idx) {
            todo.done = !todo.done;
        }
        cx.notify();
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
        let project_root = app_ref.project_root.clone();
        let model = app_ref.model.clone();
        let version = app_ref.version.clone();
        let project_name = app_ref.project_root.file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .to_string();
        let tokens_used = self.tokens_used;
        let context_tokens = self.context_tokens;

        let bg = gpui::rgb(0x1a1a1a);
        let card_bg = gpui::rgb(0x252526);
        let code_bg = gpui::rgb(0x1e1e1e);
        let terminal_bg = gpui::rgb(0x0f0f0f);
        let border = gpui::rgb(0x3c3c3c);

        let message_list = v_flex()
            .flex_1()
            .overflow_y_scrollbar()
            .gap_2()
            .p_2()
            .bg(bg)
            .children(messages.into_iter().enumerate().map(move |(idx, m)| {
                let role = m.role.clone();
                let (label, accent) = match role.as_str() {
                    "user" => ("User", gpui::rgb(0x4fc1ff)),
                    "assistant" => ("Assistant", gpui::rgb(0xc586c0)),
                    "tool" => ("Tool", gpui::rgb(0xffcc66)),
                    _ => ("Unknown", gpui::rgb(0xcccccc)),
                };
                let is_tool = role == "tool";
                let expanded = m.expanded;
                let display_text = m.text.clone();

                let body: Vec<AnyElement> = if is_tool {
                    let header = display_text.split('\n').next().unwrap_or("").to_string();
                    let rest = display_text.strip_prefix(&header).unwrap_or("").trim_start_matches('\n').to_string();
                    vec![
                        div().child(header).text_sm().text_color(gpui::rgb(0xffcc66)).into_any_element(),
                        div()
                            .when(expanded, |this| {
                                this.child(
                                    div()
                                        .p_2()
                                        .rounded_md()
                                        .bg(terminal_bg)
                                        .text_color(gpui::rgb(0xcccccc))
                                        .font_family("Zed Mono")
                                        .text_sm()
                                        .child(rest),
                                )
                            })
                            .into_any_element(),
                    ]
                } else {
                    parse_message_blocks(&display_text)
                        .into_iter()
                        .map(|block| match block {
                            MessageBlock::Text(t) => div().child(t).text_color(gpui::rgb(0xe0e0e0)).into_any_element(),
                            MessageBlock::Code { lang, code } => {
                                v_flex()
                                    .rounded_md()
                                    .overflow_hidden()
                                    .border_1()
                                    .border_color(border)
                                    .child(
                                        div()
                                            .px_2()
                                            .py_1()
                                            .bg(gpui::rgb(0x333333))
                                            .text_color(gpui::rgb(0xcccccc))
                                            .child(format!("{}", if lang.is_empty() { "code" } else { &lang }))
                                            .text_xs(),
                                    )
                                    .child(
                                        div()
                                            .p_2()
                                            .bg(code_bg)
                                            .text_color(gpui::rgb(0xd4d4d4))
                                            .text_sm()
                                            .font_family("Zed Mono")
                                            .child(code),
                                    )
                                    .into_any_element()
                            }
                        })
                        .collect()
                };

                v_flex()
                    .rounded_md()
                    .overflow_hidden()
                    .border_1()
                    .border_l_4()
                    .border_color(accent)
                    .bg(card_bg)
                    .text_color(gpui::rgb(0xe0e0e0))
                    .child(
                        h_flex()
                            .justify_between()
                            .px_2()
                            .py_1()
                            .child(div().font_weight(FontWeight::BOLD).text_color(accent).child(format!("#{} {}", idx + 1, label)))
                            .when(is_tool, |this| {
                                this.child(
                                    Button::new(format!("toggle-{}", idx))
                                        .xsmall()
                                        .ghost()
                                        .label(if expanded { "Collapse" } else { "Expand" })
                                        .on_click({
                                            let app_ptr = app;
                                            move |_, _window, _cx| {
                                                if let Some(msg) = unsafe { &mut *(app_ptr as *mut GuiApp) }.messages.lock().unwrap().get_mut(idx) {
                                                    msg.expanded = !msg.expanded;
                                                }
                                            }
                                        }),
                                )
                            }),
                    )
                    .children(body)
            }));

        // Right info panel (Session / Context / LSP / Todo)
        fn info_section(title: &str, content: Vec<AnyElement>) -> impl IntoElement {
            v_flex()
                .gap_1()
                .p_2()
                .child(div().font_weight(FontWeight::BOLD).text_color(gpui::rgb(0xe0e0e0)).child(title.to_string()))
                .children(content)
        }

        let token_percent = (tokens_used as f64 / context_tokens as f64 * 100.0) as usize;
        let bar_width = (tokens_used as f64 / context_tokens as f64 * 240.0) as f32;
        let progress_bar = div()
            .w(px(240.0))
            .h(px(6.0))
            .rounded_md()
            .bg(border)
            .child(
                div()
                    .w(px(bar_width.max(1.0)))
                    .h_full()
                    .rounded_md()
                    .bg(if token_percent > 90 {
                        gpui::rgb(0xf48771)
                    } else if token_percent > 70 {
                        gpui::rgb(0xffcc66)
                    } else {
                        gpui::rgb(0x89d185)
                    }),
            );
        let session_info = info_section(
            "Session",
            vec![
                div().child(format!("Started {}", self.session_start)).text_sm().text_color(gpui::rgb(0xcccccc)).into_any_element(),
            ],
        );
        let context_info = info_section(
            "Context",
            vec![
                div().child(format!("{} / {} tokens", tokens_used, context_tokens)).text_sm().text_color(gpui::rgb(0xcccccc)).into_any_element(),
                progress_bar.into_any_element(),
                div().child(format!("{}% used", token_percent)).text_sm().text_color(gpui::rgb(0xcccccc)).into_any_element(),
                div().child("$0.00 spent").text_sm().text_color(gpui::rgb(0xcccccc)).into_any_element(),
            ],
        );
        let lsp_info = info_section(
            "LSP",
            vec![
                div().child("LSPs are disabled").text_sm().text_color(gpui::rgb(0xcccccc)).into_any_element(),
            ],
        );
        let todos = unsafe { &*app }.todos.lock().unwrap().clone();
        let todo_items: Vec<AnyElement> = todos.iter().enumerate().map(|(idx, todo)| {
            let icon = if todo.done { "[✓]" } else { "[ ]" };
            let text = todo.text.clone();
            let color = if todo.done { gpui::rgb(0x89d185) } else { gpui::rgb(0xffcc66) };
            div()
                .id(format!("todo-{}", idx))
                .child(format!("{} {}", icon, text))
                .text_sm()
                .text_color(color)
                .cursor_pointer()
                .on_click(cx.listener(move |this, _event, _window, cx| {
                    this.toggle_todo(idx, cx);
                }))
                .into_any_element()
        }).collect();
        let todo_info = info_section("Todo", todo_items);

        let right_panel = v_flex()
            .w(px(260.0))
            .h_full()
            .border_l_1()
            .border_color(border)
            .bg(bg)
            .child(session_info)
            .child(context_info)
            .child(lsp_info)
            .child(todo_info);

        // Bottom input + status bar
        let input_bar = h_flex()
            .p_2()
            .border_t_1()
            .border_color(border)
            .bg(gpui::rgb(0x252526))
            .child(
                div()
                    .flex_1()
                    .h(px(80.0))
                    .child(Input::new(&self.input_state).h_full()),
            );

        let status_bar = h_flex()
            .justify_between()
            .p_1()
            .text_sm()
            .border_t_1()
            .border_color(border)
            .bg(gpui::rgb(0x1e1e1e))
            .text_color(gpui::rgb(0xcccccc))
            .child(div().child(format!("{} · {}", model, version)))
            .child(div().child(format!("{} / {} tokens ({}%)", tokens_used, context_tokens, token_percent)))
            .child(div().child(format!("{}:main", project_name)))
            .child(div().child("OpenCode ".to_string() + &version));

        // Main area: messages above, input/status below
        let main_area = v_flex()
            .flex_1()
            .size_full()
            .bg(bg)
            .child(message_list)
            .child(input_bar)
            .child(status_bar);

        h_flex()
            .size_full()
            .bg(bg)
            .child(main_area)
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

        {
            let theme = gpui_component::theme::Theme::global_mut(cx);
            let ht = std::sync::Arc::make_mut(&mut theme.highlight_theme);
            ht.style.editor_background = Some(gpui::rgb(0x1e1e1e).into());
        }

        let executor = cx.foreground_executor().clone();
        {
            let app: &mut GuiApp = unsafe { &mut *app_ptr };
            *app.executor.lock().unwrap() = Some(executor);
        }

        let bounds = Bounds::centered(None, size(px(900.0), px(600.0)), cx);

        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                #[cfg(target_os = "linux")]
                window_background: gpui::WindowBackgroundAppearance::Opaque,
                #[cfg(target_os = "linux")]
                window_decorations: Some(gpui::WindowDecorations::Client),
                window_min_size: Some(gpui::Size {
                    width: px(640.),
                    height: px(480.),
                }),
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
                let chat_view = cx.new(|cx| {
                    let view = ChatView {
                        app: app_ptr,
                        input_state,
                        session_start: "2026-06-15T12:00:00".to_string(),
                        tokens_used: 179011,
                        context_tokens: 262144,
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

                    // Poll for external message changes from C callbacks.
                    cx.spawn(async move |this, cx| {
                        loop {
                            cx.background_executor()
                                .timer(Duration::from_millis(100))
                                .await;
                            this.update(cx, |_this, cx| cx.notify()).ok();
                        }
                    })
                    .detach();

                    view
                });
                {
                    let app: &mut GuiApp = unsafe { &mut *app_ptr };
                    *app.view.lock().unwrap() = Some(chat_view.downgrade());
                }

                cx.new(|cx| Root::new(chat_view, window, cx))
            },
        )
        .ok();
    });

    0
}

#[no_mangle]
pub extern "C" fn gui_refresh(_app: *mut c_void) {}

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use gpui::{
    div, px, size, AnyElement, App, AppContext, Bounds, Context, Entity, EventEmitter, FontWeight,
    ForegroundExecutor, IntoElement, InteractiveElement, ParentElement, Render, ScrollHandle,
    StatefulInteractiveElement, Styled, Window, WindowBounds, WindowOptions, WeakEntity,
};
use gpui::prelude::FluentBuilder as _;
use gpui_component::{
    button::{Button, ButtonVariants},
    input::{Input, InputEvent, InputState, TabSize},
    scroll::ScrollableElement,
    v_flex, h_flex, Root, Sizable,
};
use serde_json::Value;
use std::sync::OnceLock;
use syntect::easy::HighlightLines;
use syntect::highlighting::{Style, ThemeSet};
use syntect::parsing::SyntaxSet;
use syntect::util::LinesWithEndings;

mod components;
use components::*;

// System font stack that covers Chinese characters on common Linux/BSD systems.
fn ui_font_family() -> gpui::SharedString {
    gpui::SharedString::from(
        "Noto Sans CJK SC, Source Han Sans SC, WenQuanYi Micro Hei, Microsoft YaHei, \
         PingFang SC, Hiragino Sans GB, sans-serif",
    )
}

fn mono_font_family() -> gpui::SharedString {
    gpui::SharedString::from(
        "Noto Sans Mono CJK SC, Source Han Mono SC, WenQuanYi Micro Hei Mono, \
         Microsoft YaHei Mono, Zed Mono, monospace",
    )
}

extern "C" {
    fn opencode_gui_tick(lua_state: *mut c_void);
    fn opencode_gui_notify_copy(lua_state: *mut c_void, text: *const c_char);
}

fn string_to_c(s: String) -> *mut c_char {
    match CString::new(s) {
        Ok(c) => c.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

fn notify_copy(lua_state: *mut c_void, text: &str) {
    if lua_state.is_null() {
        return;
    }
    if let Ok(c_text) = CString::new(text) {
        unsafe { opencode_gui_notify_copy(lua_state, c_text.as_ptr()) };
    }
}

/// Opaque handle to the GUI runtime.
pub struct GuiApp {
    /// User message callback: fn(session_id, text, userdata)
    on_user_message: Option<extern "C" fn(*const c_char, *const c_char, *mut c_void)>,
    user_data: *mut c_void,

    /// Tool call callback: fn(session_id, tool_json, userdata) -> *char result_json
    on_tool_call: Option<extern "C" fn(*const c_char, *const c_char, *mut c_void) -> *mut c_char>,

    /// Chat messages per session.
    messages: Arc<Mutex<Vec<MessageRow>>>,

    /// Displayed model name (e.g. "Kimi K2.7 Code").
    model: String,

    /// Window title.
    #[allow(dead_code)]
    title: String,

    /// Version label shown in the status bar.
    version: String,

    /// Project root shown in the status bar.
    project_root: PathBuf,

    /// Live todo list updated by the agent workflow.
    todos: Arc<Mutex<Vec<TodoItem>>>,

    /// Buffer for programmatic input submission (functional equivalent of typing).
    input_buffer: Arc<Mutex<String>>,

    /// Shared token counters (prompt / completion / total), updated by C callbacks.
    token_prompt: Arc<Mutex<usize>>,
    token_completion: Arc<Mutex<usize>>,
    token_total: Arc<Mutex<usize>>,

    /// Lua state pointer used by the timer tick callback.
    lua_state: *mut c_void,

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

fn render_markdown(text: &str) -> Vec<AnyElement> {
    let mut elements = Vec::new();
    let mut paragraph = String::new();

    fn flush_paragraph(p: &mut String, elements: &mut Vec<AnyElement>) {
        if !p.trim().is_empty() {
            elements.push(
                div()
                    .mb_2()
                    .children(parse_inline_markdown(p.trim()))
                    .text_color(gpui::rgb(0xe0e0e0))
                    .text_base()
                    .font_family(ui_font_family())
                    .into_any_element(),
            );
        }
        p.clear();
    }

    for raw in text.lines() {
        let line = raw.trim_end();
        if line.is_empty() {
            flush_paragraph(&mut paragraph, &mut elements);
            continue;
        }
        if let Some(rest) = line.strip_prefix("# ") {
            flush_paragraph(&mut paragraph, &mut elements);
            elements.push(
                div()
                    .mb_2()
                    .children(parse_inline_markdown(rest))
                    .text_color(gpui::rgb(0xe0e0e0))
                    .text_xl()
                    .font_weight(FontWeight::BOLD)
                    .font_family(ui_font_family())
                    .into_any_element(),
            );
        } else if let Some(rest) = line.strip_prefix("## ") {
            flush_paragraph(&mut paragraph, &mut elements);
            elements.push(
                div()
                    .mb_2()
                    .children(parse_inline_markdown(rest))
                    .text_color(gpui::rgb(0xe0e0e0))
                    .text_lg()
                    .font_weight(FontWeight::SEMIBOLD)
                    .font_family(ui_font_family())
                    .into_any_element(),
            );
        } else if let Some(rest) = line.strip_prefix("### ") {
            flush_paragraph(&mut paragraph, &mut elements);
            elements.push(
                div()
                    .mb_2()
                    .children(parse_inline_markdown(rest))
                    .text_color(gpui::rgb(0xe0e0e0))
                    .text_base()
                    .font_weight(FontWeight::SEMIBOLD)
                    .font_family(ui_font_family())
                    .into_any_element(),
            );
        } else if let Some(rest) = line.strip_prefix("- ") {
            flush_paragraph(&mut paragraph, &mut elements);
            elements.push(
                h_flex()
                    .mb_1()
                    .gap_2()
                    .child(div().child("•").text_color(gpui::rgb(0x4fc1ff)).text_base().font_family(ui_font_family()))
                    .child(
                        div()
                            .flex_1()
                            .children(parse_inline_markdown(rest))
                            .text_color(gpui::rgb(0xe0e0e0))
                            .text_base()
                            .font_family(ui_font_family()),
                    )
                    .into_any_element(),
            );
        } else if let Some(rest) = line.strip_prefix("* ") {
            flush_paragraph(&mut paragraph, &mut elements);
            elements.push(
                h_flex()
                    .mb_1()
                    .gap_2()
                    .child(div().child("•").text_color(gpui::rgb(0x4fc1ff)).text_base().font_family(ui_font_family()))
                    .child(
                        div()
                            .flex_1()
                            .children(parse_inline_markdown(rest))
                            .text_color(gpui::rgb(0xe0e0e0))
                            .text_base()
                            .font_family(ui_font_family()),
                    )
                    .into_any_element(),
            );
        } else if line.starts_with("    ") || line.starts_with('\t') {
            flush_paragraph(&mut paragraph, &mut elements);
            elements.push(
                div()
                    .mb_1()
                    .pl_4()
                    .child(line.to_string())
                    .text_color(gpui::rgb(0xcccccc))
                    .text_base()
                    .font_family(mono_font_family())
                    .into_any_element(),
            );
        } else {
            if !paragraph.is_empty() {
                paragraph.push(' ');
            }
            paragraph.push_str(line);
        }
    }
    flush_paragraph(&mut paragraph, &mut elements);
    elements
}

fn parse_inline_markdown(text: &str) -> Vec<AnyElement> {
    let mut elements = Vec::new();
    let mut buf = String::new();
    let mut chars = text.chars().peekable();
    let mut bold = false;
    let mut italic = false;
    let mut code = false;

    fn flush(buf: &mut String, elements: &mut Vec<AnyElement>, bold: bool, italic: bool, code: bool) {
        if buf.is_empty() {
            return;
        }
        let mut el = div()
            .child(buf.clone())
            .text_color(if code { gpui::rgb(0xffcc66) } else { gpui::rgb(0xe0e0e0) })
            .text_base()
            .font_family(if code { mono_font_family() } else { ui_font_family() });
        if bold {
            el = el.font_weight(FontWeight::BOLD);
        }
        if italic {
            el = el.italic();
        }
        if code {
            el = el.px_1().py(px(1.0)).rounded_sm().bg(gpui::rgb(0x333333));
        }
        elements.push(el.into_any_element());
        buf.clear();
    }

    while let Some(ch) = chars.next() {
        if ch == '*' {
            if chars.peek() == Some(&'*') {
                flush(&mut buf, &mut elements, bold, italic, code);
                chars.next();
                bold = !bold;
            } else {
                flush(&mut buf, &mut elements, bold, italic, code);
                italic = !italic;
            }
        } else if ch == '`' {
            flush(&mut buf, &mut elements, bold, italic, code);
            code = !code;
        } else if ch == '\\' && chars.peek() == Some(&'*') {
            chars.next();
            buf.push('*');
        } else {
            buf.push(ch);
        }
    }
    flush(&mut buf, &mut elements, bold, italic, code);
    elements
}

fn highlighted_code(code: &str, lang: &str) -> Vec<AnyElement> {
    fn syntax_set() -> &'static SyntaxSet {
        static SET: OnceLock<SyntaxSet> = OnceLock::new();
        SET.get_or_init(SyntaxSet::load_defaults_newlines)
    }
    fn theme_set() -> &'static ThemeSet {
        static SET: OnceLock<ThemeSet> = OnceLock::new();
        SET.get_or_init(ThemeSet::load_defaults)
    }
    fn style_to_rgb(style: &Style) -> gpui::Rgba {
        let c = style.foreground;
        let r = (c.r as f32 / 255.0 * 255.0) as u32;
        let g = (c.g as f32 / 255.0 * 255.0) as u32;
        let b = (c.b as f32 / 255.0 * 255.0) as u32;
        gpui::rgb((r << 16) | (g << 8) | b)
    }

    let ps = syntax_set();
    let ts = theme_set();
    let syntax = ps.find_syntax_by_extension(lang)
        .or_else(|| ps.find_syntax_by_name(lang))
        .unwrap_or_else(|| ps.find_syntax_plain_text());
    let theme = ts.themes.get("base16-ocean.dark")
        .or_else(|| ts.themes.values().next())
        .expect("no themes loaded");
    let mut h = HighlightLines::new(syntax, theme);
    let mut lines: Vec<AnyElement> = Vec::new();
    for line in LinesWithEndings::from(code) {
        let ranges = h.highlight_line(line, ps).unwrap_or_default();
        let tokens: Vec<AnyElement> = ranges
            .into_iter()
            .filter_map(|(style, text)| {
                let text = text.trim_end_matches('\n').trim_end_matches('\r');
                if text.is_empty() {
                    return None;
                }
                Some(
                    div()
                        .child(text.to_string())
                        .text_color(style_to_rgb(&style))
                        .text_base()
                        .font_family(mono_font_family())
                        .into_any_element(),
                )
            })
            .collect();
        lines.push(h_flex().children(tokens).into_any_element());
    }
    lines
}

#[derive(Clone, Copy, Debug)]
enum DiffLineKind {
    Context,
    Removed,
    Added,
    Padding,
}

#[derive(Clone, Debug)]
struct DiffRow {
    old_num: Option<usize>,
    new_num: Option<usize>,
    old_kind: DiffLineKind,
    new_kind: DiffLineKind,
    old_text: String,
    new_text: String,
}

fn parse_unified_diff(code: &str) -> Option<Vec<DiffRow>> {
    let mut rows: Vec<DiffRow> = Vec::new();
    let mut in_hunk = false;
    let mut old_num: usize = 0;
    let mut new_num: usize = 0;

    for raw in code.lines() {
        let line = raw.trim_end_matches('\r');
        if line.starts_with("@@") {
            // Parse hunk header: @@ -start,count +start,count @@ ...
            let rest = line.strip_prefix("@@")?.trim_start();
            let parts: Vec<&str> = rest.split_whitespace().collect();
            if parts.len() < 2 {
                return None;
            }
            let old_info = parts[0].strip_prefix("-")?;
            let new_info = parts[1].strip_prefix("+")?;
            old_num = old_info.split(',').next()?.parse().ok()?;
            new_num = new_info.split(',').next()?.parse().ok()?;
            in_hunk = true;
            continue;
        }
        if !in_hunk {
            continue;
        }
        if line.is_empty() {
            // Treat empty lines inside hunk as context if we are in a hunk.
            rows.push(DiffRow {
                old_num: Some(old_num),
                new_num: Some(new_num),
                old_kind: DiffLineKind::Context,
                new_kind: DiffLineKind::Context,
                old_text: String::new(),
                new_text: String::new(),
            });
            old_num += 1;
            new_num += 1;
            continue;
        }
        let marker = line.chars().next().unwrap();
        let content = &line[1..];
        match marker {
            ' ' => {
                rows.push(DiffRow {
                    old_num: Some(old_num),
                    new_num: Some(new_num),
                    old_kind: DiffLineKind::Context,
                    new_kind: DiffLineKind::Context,
                    old_text: content.to_string(),
                    new_text: content.to_string(),
                });
                old_num += 1;
                new_num += 1;
            }
            '-' => {
                rows.push(DiffRow {
                    old_num: Some(old_num),
                    new_num: None,
                    old_kind: DiffLineKind::Removed,
                    new_kind: DiffLineKind::Padding,
                    old_text: content.to_string(),
                    new_text: String::new(),
                });
                old_num += 1;
            }
            '+' => {
                rows.push(DiffRow {
                    old_num: None,
                    new_num: Some(new_num),
                    old_kind: DiffLineKind::Padding,
                    new_kind: DiffLineKind::Added,
                    old_text: String::new(),
                    new_text: content.to_string(),
                });
                new_num += 1;
            }
            _ => {
                // Unknown marker, abort and let caller fall back to plain rendering.
                return None;
            }
        }
    }

    if rows.is_empty() {
        None
    } else {
        Some(rows)
    }
}

fn render_side_by_side_diff(rows: &[DiffRow]) -> AnyElement {
    let gutter_width = px(48.0);
    let line_height = px(22.0);

    fn diff_line_cell(
        num: Option<usize>,
        text: &str,
        kind: DiffLineKind,
        gutter_width: gpui::Pixels,
        line_height: gpui::Pixels,
    ) -> AnyElement {
        let (bg, text_color) = match kind {
            DiffLineKind::Removed => (gpui::rgb(0x3c1616), gpui::rgb(0xffb3b3)),
            DiffLineKind::Added => (gpui::rgb(0x163c16), gpui::rgb(0xb3ffb3)),
            DiffLineKind::Context => (gpui::rgb(0x1e1e1e), gpui::rgb(0xcccccc)),
            DiffLineKind::Padding => (gpui::rgb(0x1a1a1a), gpui::rgb(0x666666)),
        };
        h_flex()
            .h(line_height)
            .bg(bg)
            .child(
                div()
                    .w(gutter_width)
                    .h_full()
                    .flex_none()
                    .border_r_1()
                    .border_color(gpui::rgb(0x3c3c3c))
                    .child(num.map(|n| n.to_string()).unwrap_or_default())
                    .text_sm()
                    .text_color(gpui::rgb(0x888888))
                    .font_family(mono_font_family())
                    .flex()
                    .items_center()
                    .justify_center(),
            )
            .child(
                div()
                    .flex_1()
                    .h_full()
                    .px_2()
                    .child(if text.is_empty() && matches!(kind, DiffLineKind::Padding) {
                        " ".to_string()
                    } else {
                        text.to_string()
                    })
                    .text_base()
                    .text_color(text_color)
                    .font_family(mono_font_family())
                    .flex()
                    .items_center(),
            )
            .into_any_element()
    }

    v_flex()
        .border_1()
        .border_color(gpui::rgb(0x3c3c3c))
        .rounded_md()
        .overflow_hidden()
        .children(rows.iter().map(|row| {
            h_flex()
                .child(diff_line_cell(row.old_num, &row.old_text, row.old_kind, gutter_width, line_height))
                .child(diff_line_cell(row.new_num, &row.new_text, row.new_kind, gutter_width, line_height))
                .into_any_element()
        }))
        .into_any_element()
}

#[derive(Clone)]
struct MessageRow {
    role: String,
    text: String,
    #[allow(dead_code)]
    tool_id: Option<String>,
    expanded: bool,
}

impl GuiApp {
    fn append(&self, role: &str, text: String, tool_id: Option<String>) {
        self.messages.lock().unwrap().push(MessageRow {
            role: role.to_string(),
            text,
            tool_id,
            expanded: role != "reasoning",
        });
    }

    fn refresh_ui(&self) {
        // No-op: the 16ms timer loop calls cx.notify() on every tick,
        // so any changes to messages/todos/tokens will be rendered
        // within the next frame (max 16ms latency).
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
        input_buffer: Arc::new(Mutex::new(String::new())),
        token_prompt: Arc::new(Mutex::new(0)),
        token_completion: Arc::new(Mutex::new(0)),
        token_total: Arc::new(Mutex::new(0)),
        lua_state: std::ptr::null_mut(),
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

#[no_mangle]
pub extern "C" fn gui_set_tokens(
    app: *mut c_void,
    total: c_int,
    prompt: c_int,
    completion: c_int,
) {
    if app.is_null() {
        return;
    }
    let app: &GuiApp = unsafe { &*(app as *mut GuiApp) };
    if let Ok(mut g) = app.token_total.lock() {
        *g = total.max(0) as usize;
    }
    if let Ok(mut g) = app.token_prompt.lock() {
        *g = prompt.max(0) as usize;
    }
    if let Ok(mut g) = app.token_completion.lock() {
        *g = completion.max(0) as usize;
    }
    app.refresh_ui();
}

#[no_mangle]
pub extern "C" fn gui_set_input_value(app: *mut c_void, text: *const c_char) {
    if app.is_null() || text.is_null() {
        return;
    }
    let text_str = unsafe { CStr::from_ptr(text).to_string_lossy().to_string() };
    let app: &GuiApp = unsafe { &*(app as *mut GuiApp) };
    if let Ok(mut buf) = app.input_buffer.lock() {
        *buf = text_str;
    }
}

#[no_mangle]
pub extern "C" fn gui_submit_input(app: *mut c_void) {
    if app.is_null() {
        return;
    }
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };
    let text = {
        let mut buf = app.input_buffer.lock().unwrap();
        let t = buf.clone();
        buf.clear();
        t
    };
    if text.is_empty() {
        return;
    }
    app.append("user", text.clone(), None);
    if let Some(cb) = app.on_user_message {
        let s = CString::new("default").unwrap();
        let t = CString::new(text).unwrap();
        cb(s.as_ptr(), t.as_ptr(), app.user_data);
    }
    app.refresh_ui();
}

#[no_mangle]
pub extern "C" fn gui_get_messages(app: *mut c_void) -> *mut c_char {
    if app.is_null() {
        return std::ptr::null_mut();
    }
    let app: &GuiApp = unsafe { &*(app as *mut GuiApp) };
    let msgs = app.messages.lock().unwrap();
    let arr: Vec<Value> = msgs.iter().map(|m| {
        serde_json::json!({
            "role": m.role,
            "text": m.text,
        })
    }).collect();
    string_to_c(serde_json::to_string(&arr).unwrap_or_else(|_| "[]".to_string()))
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
    context_tokens: usize,
    scroll_handle: ScrollHandle,
    msg_count: usize,
    last_text_len: usize,
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
        self.dispatch_user_message(&text, cx);
    }

    fn dispatch_user_message(&mut self, text: &str, cx: &mut Context<Self>) {
        {
            let app: &mut GuiApp = unsafe { &mut *self.app };
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
        let msg_count = messages.len();
        let last_text_len: usize = messages.last().map(|m| m.text.len()).unwrap_or(0);
        let should_scroll = (msg_count != self.msg_count && msg_count > 0)
            || (last_text_len != self.last_text_len && msg_count > 0);
        self.msg_count = msg_count;
        self.last_text_len = last_text_len;

        let model = app_ref.model.clone();
        let version = app_ref.version.clone();
        let project_name = app_ref.project_root.file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .to_string();
        let context_tokens = self.context_tokens;
        let tokens_used = *app_ref.token_total.lock().unwrap();
        let prompt_tokens = *app_ref.token_prompt.lock().unwrap();
        let completion_tokens = *app_ref.token_completion.lock().unwrap();

        let bg = gpui::rgb(0x1a1a1a);
        let card_bg = gpui::rgb(0x252526);
        let code_bg = gpui::rgb(0x1e1e1e);
        let terminal_bg = gpui::rgb(0x0f0f0f);
        let border = gpui::rgb(0x3c3c3c);

        let message_list = v_flex()
            .id("message-list")
            .flex_1()
            .gap_2()
            .p_2()
            .bg(bg)
            .overflow_y_scroll()
            .vertical_scrollbar(&self.scroll_handle)
            .children(messages.into_iter().enumerate().map(move |(idx, m)| {
                let role = m.role.clone();
                let (label, accent) = match role.as_str() {
                    "user" => ("User", gpui::rgb(0x4fc1ff)),
                    "assistant" => ("Assistant", gpui::rgb(0xc586c0)),
                    "tool" => ("Tool", gpui::rgb(0xffcc66)),
                    "reasoning" => ("Thinking", gpui::rgb(0x888888)),
                    _ => ("Unknown", gpui::rgb(0xcccccc)),
                };
                let is_tool = role == "tool";
                let is_reasoning = role == "reasoning";
                let expanded = m.expanded;
                let display_text = m.text.clone();

                let body: Vec<AnyElement> = if is_tool {
                    let header = display_text.split('\n').next().unwrap_or("").to_string();
                    let rest = display_text.strip_prefix(&header).unwrap_or("").trim_start_matches('\n').to_string();
                    vec![
                        div().child(header).text_base().text_color(gpui::rgb(0xffcc66)).font_family(ui_font_family()).into_any_element(),
                        div()
                            .when(expanded, |this| {
                                this.child(
                                    div()
                                        .p_2()
                                        .rounded_md()
                                        .bg(terminal_bg)
                                        .text_color(gpui::rgb(0xcccccc))
                                        .font_family(mono_font_family())
                                        .text_base()
                                        .child(rest),
                                )
                            })
                            .into_any_element(),
                    ]
                } else if is_reasoning {
                    vec![
                        div().child("Thinking...").text_base().text_color(gpui::rgb(0x888888)).font_family(ui_font_family()).into_any_element(),
                        div()
                            .when(expanded, |this| {
                                this.child(
                                    div()
                                        .p_2()
                                        .rounded_md()
                                        .bg(terminal_bg)
                                        .text_color(gpui::rgb(0x999999))
                                        .font_family(mono_font_family())
                                        .text_base()
                                        .child(display_text.clone()),
                                )
                            })
                            .into_any_element(),
                    ]
                } else {
                    parse_message_blocks(&display_text)
                        .into_iter()
                        .enumerate()
                        .map(|(block_idx, block)| {
                            let block_id = gpui::ElementId::Name(format!("msg-{}-block-{}", idx, block_idx).into());
                            match block {
                                MessageBlock::Text(t) => div()
                                    .id(block_id)
                                    .children(render_markdown(&t))
                                    .text_color(gpui::rgb(0xe0e0e0))
                                    .text_base()
                                    .font_family(ui_font_family())
                                    .cursor_pointer()
                                    .on_click({
                                        let app_ptr = app;
                                        let text = t.clone();
                                        move |event: &gpui::ClickEvent, _window, _cx| {
                                            if event.click_count() == 2 {
                                                notify_copy(unsafe { &*(app_ptr as *const GuiApp) }.lua_state, &text);
                                            }
                                        }
                                    })
                                    .into_any_element(),
                                MessageBlock::Code { lang, code } => {
                                    let code_id = gpui::ElementId::Name(format!("msg-{}-block-{}-code", idx, block_idx).into());
                                    let is_diff = lang.trim().eq_ignore_ascii_case("diff");
                                    let diff_view = if is_diff {
                                        parse_unified_diff(&code).map(|rows| render_side_by_side_diff(&rows))
                                    } else {
                                        None
                                    };
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
                                                .text_sm()
                                                .font_family(ui_font_family()),
                                        )
                                        .child(
                                            div()
                                                .id(code_id)
                                                .p_2()
                                                .bg(code_bg)
                                                .font_family(mono_font_family())
                                                .children(diff_view.map(|el| vec![el]).unwrap_or_else(|| highlighted_code(&code, &lang)))
                                                .cursor_pointer()
                                                .on_click({
                                                    let app_ptr = app;
                                                    let code = code.clone();
                                                    move |event: &gpui::ClickEvent, _window, _cx| {
                                                        if event.click_count() == 2 {
                                                            notify_copy(unsafe { &*(app_ptr as *const GuiApp) }.lua_state, &code);
                                                        }
                                                    }
                                                }),
                                        )
                                        .into_any_element()
                                }
                            }
                        })
                        .collect()
                };

                let has_toggle = is_tool || is_reasoning;
                v_flex()
                    .bg(card_bg)
                    .text_color(gpui::rgb(0xe0e0e0))
                    .text_base()
                    .font_family(ui_font_family())
                    .child(
                        h_flex()
                            .justify_between()
                            .px_2()
                            .py_1()
                            .child(div().font_weight(FontWeight::BOLD).text_color(accent).child(format!("#{} {}", idx + 1, label)))
                            .when(has_toggle, |this| {
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
        let token_percent = if context_tokens > 0 {
            (tokens_used as f64 / context_tokens as f64 * 100.0) as usize
        } else {
            0
        };
        let _bar_width = if context_tokens > 0 {
            (tokens_used as f64 / context_tokens as f64 * 240.0) as f32
        } else {
            0.0
        };
        let progress_bar = TokenProgress::new(tokens_used, context_tokens);
        let session_info = InfoSection::new("Session")
            .child(div().child(format!("Started {}", self.session_start)).text_base().text_color(gpui::rgb(0xcccccc)).font_family(ui_font_family()));
        let context_info = InfoSection::new("Context")
            .child(div().child(format!("{} / {} tokens", tokens_used, context_tokens)).text_base().text_color(gpui::rgb(0xcccccc)).font_family(ui_font_family()))
            .child(progress_bar)
            .child(div().child(format!("{}% used", token_percent)).text_base().text_color(gpui::rgb(0xcccccc)).font_family(ui_font_family()))
            .child(div().child(format!("prompt {} + completion {}", prompt_tokens, completion_tokens)).text_sm().text_color(gpui::rgb(0x999999)).font_family(ui_font_family()))
            .child(div().child("$0.00 spent").text_base().text_color(gpui::rgb(0xcccccc)).font_family(ui_font_family()));
        let lsp_info = InfoSection::new("LSP")
            .child(div().child("LSPs are disabled").text_base().text_color(gpui::rgb(0xcccccc)).font_family(ui_font_family()));
        let todos = unsafe { &*app }.todos.lock().unwrap().clone();
        let todo_items: Vec<AnyElement> = todos.iter().enumerate().map(|(idx, todo)| {
            let icon = if todo.done { "[✓]" } else { "[ ]" };
            let text = todo.text.clone();
            let color = if todo.done { gpui::rgb(0x89d185) } else { gpui::rgb(0xffcc66) };
            div()
                .id(format!("todo-{}", idx))
                .child(format!("{} {}", icon, text))
                .text_base()
                .text_color(color)
                .font_family(ui_font_family())
                .cursor_pointer()
                .on_click(cx.listener(move |this, _event, _window, cx| {
                    this.toggle_todo(idx, cx);
                }))
                .into_any_element()
        }).collect();
        let todo_info = {
            let mut sec = InfoSection::new("Todo");
            for item in todo_items {
                sec = sec.child(item);
            }
            sec
        };

        let right_panel = v_flex()
            .w_1_4()
            .min_w(px(220.0))
            .max_w(px(360.0))
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

        let status_bar = StatusBar::new(model, version)
            .project(project_name)
            .tokens(tokens_used, context_tokens, prompt_tokens, completion_tokens);

        // Main area: messages, input, status
        let main_area = v_flex()
            .flex_1()
            .size_full()
            .bg(bg)
            .child(message_list)
            .child(input_bar)
            .child(status_bar);

        if should_scroll {
            cx.defer_in(_window, |this, _window, _cx| {
                this.scroll_handle.scroll_to_bottom();
            });
        }

        h_flex()
            .size_full()
            .bg(bg)
            .child(main_area)
            .child(right_panel)
    }
}

impl EventEmitter<InputEvent> for ChatView {}

fn create_app_icon() -> Option<std::sync::Arc<image::RgbaImage>> {
    let size = 32u32;
    let mut img = image::RgbaImage::new(size, size);
    let cx = size as f32 / 2.0;
    let cy = size as f32 / 2.0;
    let face_r = size as f32 / 2.0 - 2.0;
    for y in 0..size {
        for x in 0..size {
            let dx = x as f32 - cx;
            let dy = y as f32 - cy;
            let dist = (dx * dx + dy * dy).sqrt();
            if dist < face_r {
                // ☺ Yellow face
                img.put_pixel(x, y, image::Rgba([0xff, 0xcc, 0x33, 0xff]));
            } else if dist < face_r + 1.5 {
                let alpha = ((face_r + 1.5 - dist) / 1.5).clamp(0.0, 1.0);
                img.put_pixel(x, y, image::Rgba([0xff, 0xcc, 0x33, (alpha * 255.0) as u8]));
            } else {
                img.put_pixel(x, y, image::Rgba([0, 0, 0, 0]));
            }
        }
    }
    // Eyes: two black dots
    let eye_y = (cy - 8.0) as u32;
    for ex in [cx - 10.0, cx + 10.0] {
        for dy in -3i32..=3i32 {
            for dx in -3i32..=3i32 {
                let px = (ex as i32 + dx) as u32;
                let py = (eye_y as i32 + dy) as u32;
                if px < size && py < size {
                    let edx = px as f32 - ex;
                    let edy = py as f32 - (eye_y as f32);
                    if (edx * edx + edy * edy).sqrt() <= 3.0 {
                        img.put_pixel(px, py, image::Rgba([0x1a, 0x1a, 0x2e, 0xff]));
                    }
                }
            }
        }
    }
    // Smile: arc at the bottom half of the face
    let mouth_cy = cy + 6.0;
    for x in 0..size {
        for y in 0..size {
            let dx = x as f32 - cx;
            let dy = y as f32 - mouth_cy;
            // Smile parabola: y = a*x^2 + b, roughly at the bottom
            let parabola = 0.025 * dx * dx;
            let target_y = mouth_cy + parabola;
            if (y as f32 - target_y).abs() < 1.5 && dx.abs() < 20.0 && y as f32 > cy {
                img.put_pixel(x, y, image::Rgba([0x1a, 0x1a, 0x2e, 0xff]));
            }
        }
    }
    Some(std::sync::Arc::new(img))
}

#[no_mangle]
pub extern "C" fn gui_run(app_ptr: *mut c_void, lua_state: *mut c_void) -> c_int {
    if app_ptr.is_null() {
        return -1;
    }
    {
        let app: &mut GuiApp = unsafe { &mut *(app_ptr as *mut GuiApp) };
        app.lua_state = lua_state;
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
        gpui_component::theme::Theme::change(gpui_component::theme::ThemeMode::Dark, None, cx);

        let executor = cx.foreground_executor().clone();
        {
            let app: &mut GuiApp = unsafe { &mut *app_ptr };
            *app.executor.lock().unwrap() = Some(executor);
            let mut messages = app.messages.lock().unwrap();
            if messages.is_empty() {
                messages.push(MessageRow {
                    role: "assistant".to_string(),
                    text: "Welcome to OpenCode. Double-click any text or code block to copy it to the clipboard.".to_string(),
                    expanded: false,
                    tool_id: None,
                });
            }
        }

        let bounds = Bounds::centered(None, size(px(1280.0), px(720.0)), cx);
        let title = unsafe { (*app_ptr).title.clone() };

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
                titlebar: Some(gpui::TitlebarOptions {
                    title: Some(gpui::SharedString::from(title)),
                    ..Default::default()
                }),
                icon: create_app_icon(),
                ..Default::default()
            },
            move |window, cx| {
                let input_state = cx.new(|cx| {
                    InputState::new(window, cx)
                        .code_editor("markdown")
                        .multi_line(true)
                        .rows(5)
                        .line_number(false)
                        .folding(false)
                        .tab_size(TabSize { tab_size: 4, ..Default::default() })
                });
                let input_state_for_view = input_state.clone();
                let chat_view = cx.new(|cx| {
                    let view = ChatView {
                        app: app_ptr,
                        input_state,
                        session_start: "2026-06-15T12:00:00".to_string(),
                        context_tokens: 262144,
                        scroll_handle: ScrollHandle::new(),
                        msg_count: 0,
                        last_text_len: 0,
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

                // Poll for external message changes from C callbacks and tick Lua coroutines.
                    cx.spawn(async move |this, cx| {
                        loop {
                            cx.background_executor()
                                .timer(Duration::from_millis(16))
                                .await;
                            // Tick Lua coroutines FIRST (they may send stream deltas).
                            unsafe {
                                let app: &GuiApp = &*app_ptr;
                                if !app.lua_state.is_null() {
                                    opencode_gui_tick(app.lua_state);
                                }
                            }
                            // THEN render (captures the just-updated messages).
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

                input_state_for_view.update(cx, |state, cx| {
                    state.focus(window, cx);
                });

                if let Ok(test_msg) = std::env::var("OPENCODE_GUI_TEST_MSG") {
                    if !test_msg.is_empty() {
                        let chat_view_weak = chat_view.downgrade();
                        cx.spawn(async move |cx| {
                            cx.background_executor()
                                .timer(Duration::from_secs(2))
                                .await;
                            chat_view_weak
                                .update(cx, |view, cx| {
                                    view.dispatch_user_message(&test_msg, cx);
                                })
                                .ok();
                        })
                        .detach();
                    }
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

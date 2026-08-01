use agent_acp::types::{SessionUpdateParams, Update};
use agent_acp::{AcpError, Client, ClientConfig};
use serde_json::Value;
use std::future::Future;
use std::ops::{Deref, DerefMut};
use std::pin::Pin;
use std::sync::Arc;
use tokio::sync::broadcast;
use tokio::sync::mpsc;

use crate::event::{self, UiEvent};
use crate::input::Input;
use crossterm::event::{KeyCode, KeyModifiers, KeyEvent};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};

#[derive(Clone, Debug)]
pub struct ToolItem {
    pub id: String,
    pub title: String,
    pub kind: String,
    pub status: String,
    pub output: String,
    pub expanded: bool,
}

#[derive(Clone, Debug)]
pub enum UiMsg {
    User(String),
    Assistant(String),
    Thinking(String),
    ToolCall(ToolItem),
    Info(String),
    Error(String),
}

pub struct PendingPermission {
    pub request_id: u64,
    pub action: String,
    pub resource: String,
}

pub struct PendingQuestion {
    pub request_id: u64,
    pub question: String,
    pub options: Vec<String>,
    pub input: String,
}

#[derive(Clone)]
pub struct TodoItem {
    pub id: String,
    pub content: String,
    pub status: String,
}

/// Pure UI state and logic. No I/O: it does not hold a Client, terminal or
/// engine handle, so it can be unit-tested without a running engine.
pub struct AppState {
    pub session_id: String,
    pub project: String,
    pub model: String,
    pub messages: Vec<UiMsg>,
    pub input: Input,
    pub history: Vec<String>,
    pub hist_idx: Option<usize>,
    pub busy: bool,
    pub esc_armed: bool,
    pub should_quit: bool,
    pub status: String,
    pub focus: Option<usize>,
    pub pending_permission: Option<PendingPermission>,
    pub pending_question: Option<PendingQuestion>,
    pub todos: Vec<TodoItem>,
}

/// Thin wrapper pairing the testable state machine with the live client.
pub struct App {
    pub client: Arc<Client>,
    pub state: AppState,
}

impl Deref for App {
    type Target = AppState;
    fn deref(&self) -> &Self::Target {
        &self.state
    }
}

impl DerefMut for App {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.state
    }
}

impl App {
    pub fn new(client: Arc<Client>, session_id: String, project: String, model: String) -> Self {
        Self {
            client,
            state: AppState::new(session_id, project, model),
        }
    }
}

type ActionFut = Pin<Box<dyn Future<Output = Result<ActionOutcome, AcpError>>>>;

#[derive(Debug)]
pub enum SubmitAction {
    Prompt { session_id: String, text: String },
    SwitchModel(String),
}pub enum ActionOutcome {
    Prompt(agent_acp::SessionPromptResult),
    SwitchModel(String),
}

/// Handle a key while a question dialog is open.
/// Returns Some((request_id, answer)) when a numbered option was picked
/// immediately; otherwise updates the input buffer in place.
fn handle_question_key(pq: &mut Option<PendingQuestion>, k: &KeyEvent) -> Option<(u64, String)> {
    let q = pq.as_mut()?;
    if !q.options.is_empty() {
        if let KeyCode::Char(c) = k.code {
            if k.modifiers.is_empty() {
                if let Some(n) = c.to_digit(10) {
                    if n >= 1 && n <= q.options.len() as u32 {
                        let answer = q.options[n as usize - 1].clone();
                        let request_id = q.request_id;
                        pq.take();
                        return Some((request_id, answer));
                    }
                }
            }
        }
        return None;
    }
    match k.code {
        KeyCode::Char(c)
            if k.modifiers.is_empty() || k.modifiers.contains(KeyModifiers::SHIFT) =>
        {
            q.input.push(c);
        }
        KeyCode::Backspace => {
            q.input.pop();
        }
        _ => {}
    }
    None
}

fn diff_line_style(l: &str) -> Option<Style> {
    if l.starts_with("diff --git") || l.starts_with("index ") || l.starts_with("+++") || l.starts_with("---")
    {
        Some(
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        )
    } else if l.starts_with('+') {
        Some(Style::default().fg(Color::Green))
    } else if l.starts_with('-') {
        Some(Style::default().fg(Color::Red))
    } else if l.starts_with("@@") {
        Some(Style::default().fg(Color::Cyan))
    } else {
        None
    }
}

pub struct LineInfo {
    pub msg: Option<usize>,
    pub is_tool_title: bool,
    pub line: Line<'static>,
}

impl AppState {
    pub fn new(session_id: String, project: String, model: String) -> Self {
        Self {
            session_id,
            project,
            model,
            messages: Vec::new(),
            input: Input::new(),
            history: Vec::new(),
            hist_idx: None,
            busy: false,
            esc_armed: false,
            should_quit: false,
            status: String::new(),
            focus: None,
            pending_permission: None,
            pending_question: None,
            todos: Vec::new(),
        }
    }

    pub fn push(&mut self, msg: UiMsg) {
        self.messages.push(msg);
    }

    pub fn submit(&mut self) -> Option<SubmitAction> {
        let text = self.input.buf.trim().to_string();
        if text.is_empty() || self.busy {
            return None;
        }
        // Bare quit words exit the app (no need for the / prefix).
        if text == "quit" || text == "exit" || text == "q" {
            self.input.clear();
            self.should_quit = true;
            return None;
        }
        if let Some(name) = text.strip_prefix("/model ") {
            let name = name.trim().to_string();
            self.input.clear();
            if name.is_empty() {
                return None;
            }
            self.busy = true;
            self.focus = None;
            return Some(SubmitAction::SwitchModel(name));
        }
        if text.starts_with('/') {
            self.handle_command(&text);
            self.input.clear();
            return None;
        }
        self.history.push(text.clone());
        self.hist_idx = None;
        self.messages.push(UiMsg::User(text.clone()));
        self.input.clear();
        self.busy = true;
        self.focus = None;
        Some(SubmitAction::Prompt {
            session_id: self.session_id.clone(),
            text,
        })
    }

    fn handle_command(&mut self, cmd: &str) {
        let name = match cmd.split_once(' ') {
            Some((n, _)) => n,
            None => cmd,
        };
        match name {
            "/quit" | "/exit" | "/q" => self.should_quit = true,
            "/new" => {
                self.messages.clear();
                self.focus = None;
                self.status = "new session".into();
            }
            "/models" => {
                self.messages.push(UiMsg::Info(format!(
                    "current model: {} · switch with /model <name> (e.g. /model deepseek-v4-pro)",
                    self.model
                )));
            }
            "/help" => {
                self.messages.push(UiMsg::Info(
                    "commands: /quit /new /model <name> /models /help\n\
                     enter send · shift+enter newline · esc interrupt (x2)\n\
                     (empty input) j/k or ↑/↓ scroll · pagedown/pageup page · enter/tab expand tool"
                        .into(),
                ));
            }
            other => {
                self.messages
                    .push(UiMsg::Error(format!("unknown command: {other}")));
            }
        }
    }

    pub fn on_event(&mut self, ev: agent_acp::types::Event) {
        if let Some(req_id) = ev.engine_request_id {
            if ev.method == "permission/request" {
                let (action, resource) = ev
                    .params
                    .as_ref()
                    .map(|p| {
                        (
                            p.get("action")
                                .and_then(|v| v.as_str())
                                .unwrap_or("?")
                                .to_string(),
                            p.get("resource")
                                .and_then(|v| v.as_str())
                                .unwrap_or("?")
                                .to_string(),
                        )
                    })
                    .unwrap_or(("?".into(), "?".into()));
                self.pending_permission = Some(PendingPermission {
                    request_id: req_id,
                    action,
                    resource,
                });
            } else if ev.method == "question/request" {
                let (question, options) = ev
                    .params
                    .as_ref()
                    .map(|p| {
                        (
                            p.get("question")
                                .and_then(|v| v.as_str())
                                .unwrap_or("?")
                                .to_string(),
                            p.get("options")
                                .and_then(|v| v.as_array())
                                .map(|a| {
                                    a.iter()
                                        .filter_map(|v| v.as_str().map(String::from))
                                        .collect()
                                })
                                .unwrap_or_default(),
                        )
                    })
                    .unwrap_or(("?".into(), Vec::new()));
                self.pending_question = Some(PendingQuestion {
                    request_id: req_id,
                    question,
                    options,
                    input: String::new(),
                });
            }
            return;
        }
        if ev.method != "session/update" {
            return;
        }
        let Ok(params) =
            serde_json::from_value::<SessionUpdateParams>(ev.params.unwrap_or(Value::Null))
        else {
            return;
        };
        match params.update {
            Update::AgentMessageChunk { content } => {
                let text = content.text;
                let is_thinking = content.kind == "thinking";
                if is_thinking {
                    if let Some(UiMsg::Thinking(buf)) = self.messages.last_mut() {
                        buf.push_str(&text);
                    } else {
                        self.messages.push(UiMsg::Thinking(text));
                    }
                } else if let Some(UiMsg::Assistant(buf)) = self.messages.last_mut() {
                    buf.push_str(&text);
                } else {
                    self.messages.push(UiMsg::Assistant(text));
                }
            }
            Update::ToolCall {
                tool_call_id,
                title,
                kind,
                status,
                raw_input,
                ..
            } => {
                // Show tool invocations expanded by default so the process of
                // implementing code (arguments + output) is visible without
                // pressing Enter on each one.
                let input_text = match raw_input {
                    Some(Value::String(s)) => s,
                    Some(other) => other.to_string(),
                    None => String::new(),
                };
                self.messages.push(UiMsg::ToolCall(ToolItem {
                    id: tool_call_id,
                    title,
                    kind,
                    status,
                    output: input_text,
                    expanded: true,
                }));
            }
            Update::ToolCallUpdate {
                tool_call_id,
                status,
                raw_output,
            } => {
                let text = match raw_output {
                    Some(Value::String(s)) => s,
                    Some(Value::Object(obj)) => match obj.get("output") {
                        Some(Value::String(s)) => s.clone(),
                        Some(other) => other.to_string(),
                        None => Value::Object(obj).to_string(),
                    },
                    Some(other) => other.to_string(),
                    None => String::new(),
                };
                for msg in self.messages.iter_mut().rev() {
                    if let UiMsg::ToolCall(item) = msg {
                        if item.id == tool_call_id {
                            item.status = status;
                            if !text.is_empty() {
                                item.output = text;
                            }
                            break;
                        }
                    }
                }
            }
            Update::Todo {
                id, content, status, ..
            } => {
                if let Some(t) = self.todos.iter_mut().find(|t| t.id == id) {
                    t.content = content;
                    t.status = status;
                } else {
                    self.todos.push(TodoItem {
                        id,
                        content,
                        status,
                    });
                }
            }
        }
    }

    pub fn layout_lines(&self) -> Vec<LineInfo> {
        let mut out = Vec::new();
        for (i, msg) in self.messages.iter().enumerate() {
            match msg {
                UiMsg::User(text) => {
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::from(Span::styled(
                            "┌ You",
                            Style::default().fg(Color::Green).add_modifier(Modifier::BOLD),
                        )),
                    });
                    for l in text.lines() {
                        out.push(LineInfo {
                            msg: Some(i),
                            is_tool_title: false,
                            line: Line::from(Span::styled(
                                format!("│ {l}"),
                                Style::default().fg(Color::Green),
                            )),
                        });
                    }
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::from(Span::styled(
                            "└",
                            Style::default().fg(Color::Green),
                        )),
                    });
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::default(),
                    });
                }
                UiMsg::Assistant(text) => {
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::from(Span::styled(
                            "┌ Assistant",
                            Style::default().fg(Color::Blue).add_modifier(Modifier::BOLD),
                        )),
                    });
                    for l in text.lines() {
                        out.push(LineInfo {
                            msg: Some(i),
                            is_tool_title: false,
                            line: Line::from(Span::styled(
                                format!("│ {l}"),
                                Style::default().fg(Color::Blue),
                            )),
                        });
                    }
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::from(Span::styled(
                            "└",
                            Style::default().fg(Color::Blue),
                        )),
                    });
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::default(),
                    });
                }
                UiMsg::Thinking(text) => {
                    let title_style = Style::default()
                        .fg(Color::DarkGray)
                        .add_modifier(Modifier::ITALIC);
                    let body_style = Style::default()
                        .fg(Color::DarkGray)
                        .add_modifier(Modifier::ITALIC);
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::from(Span::styled("┌ Thinking", title_style)),
                    });
                    for l in text.lines() {
                        out.push(LineInfo {
                            msg: Some(i),
                            is_tool_title: false,
                            line: Line::from(Span::styled(format!("│ {l}"), body_style)),
                        });
                    }
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::from(Span::styled("└", title_style)),
                    });
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::default(),
                    });
                }
                UiMsg::ToolCall(item) => {
                    let (icon, fg) = match item.status.as_str() {
                        "completed" => ("✓", Color::Green),
                        "failed" => ("✖", Color::Red),
                        _ => ("●", Color::Yellow),
                    };
                    let title = if item.status == "running" {
                        format!("▸ {} ({})", item.title, item.kind)
                    } else {
                        format!("{icon} {} ({})", item.title, item.kind)
                    };
                    let expand_hint = if item.expanded { "▾" } else { "▸" };
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: true,
                        line: Line::from(vec![
                            Span::styled(
                                format!("  {title}"),
                                Style::default()
                                    .fg(fg)
                                    .add_modifier(Modifier::BOLD),
                            ),
                            Span::styled(
                                format!("  [{id}] ", id = item.id),
                                Style::default().fg(Color::DarkGray),
                            ),
                            Span::styled(
                                if item.expanded || item.status == "running" {
                                    format!("{expand_hint}")
                                } else {
                                    String::new()
                                },
                                Style::default().fg(Color::DarkGray),
                            ),
                        ]),
                    });
                    if item.expanded {
                        let is_diff = item.output.lines().any(|l| {
                            l.starts_with("diff --git")
                                || (l.starts_with("@@") && l.contains("@@"))
                                || l.starts_with("+++ ")
                        });
                        for (n, l) in item.output.lines().enumerate() {
                            if n >= 200 {
                                out.push(LineInfo {
                                    msg: Some(i),
                                    is_tool_title: false,
                                    line: Line::from(Span::styled(
                                        format!("  ⋮ ({n} more lines hidden, scroll to view)"),
                                        Style::default().fg(Color::DarkGray),
                                    )),
                                });
                                break;
                            }
                            let style = if is_diff {
                                diff_line_style(l)
                                    .unwrap_or(Style::default().fg(Color::DarkGray))
                            } else {
                                Style::default().fg(Color::DarkGray)
                            };
                            out.push(LineInfo {
                                msg: Some(i),
                                is_tool_title: false,
                                line: Line::from(Span::styled(format!("  │ {l}"), style)),
                            });
                        }
                        out.push(LineInfo {
                            msg: Some(i),
                            is_tool_title: false,
                            line: Line::default(),
                        });
                    } else if item.status == "completed" {
                        out.push(LineInfo {
                            msg: Some(i),
                            is_tool_title: false,
                            line: Line::default(),
                        });
                    }
                }
                UiMsg::Info(text) => {
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::from(Span::styled(
                            format!("ℹ {text}"),
                            Style::default().fg(Color::DarkGray),
                        )),
                    });
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::default(),
                    });
                }
                UiMsg::Error(text) => {
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::from(Span::styled(
                            format!("✖ {text}"),
                            Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
                        )),
                    });
                    out.push(LineInfo {
                        msg: Some(i),
                        is_tool_title: false,
                        line: Line::default(),
                    });
                }
            }
        }
        if out.is_empty() {
            out.push(LineInfo {
                msg: None,
                is_tool_title: false,
                line: Line::from(Span::styled(
                    " (no messages yet)",
                    Style::default().fg(Color::DarkGray),
                )),
            });
        }
        out
    }

    pub fn view_top(&self, view_h: usize) -> usize {
        let total = self.layout_lines().len();
        match self.focus {
            Some(f) => {
                if view_h == 0 {
                    return f;
                }
                let center = f.saturating_sub(view_h / 2);
                center.min(total.saturating_sub(view_h))
            }
            None => total.saturating_sub(view_h),
        }
    }

    fn scroll_step(&mut self, delta: isize) {
        let total = self.layout_lines().len();
        let cur = self.focus.unwrap_or(total.saturating_sub(1));
        if delta > 0 {
            let next = cur + delta as usize;
            if next + 1 >= total {
                self.focus = None;
            } else {
                self.focus = Some(next);
            }
        } else {
            self.focus = Some(cur.saturating_sub((-delta) as usize));
        }
    }

    fn toggle_focus(&mut self) {
        let Some(f) = self.focus else { return };
        let lines = self.layout_lines();
        let mut target = None;
        for i in (0..=f).rev() {
            if lines[i].is_tool_title {
                target = Some(i);
                break;
            }
        }
        let Some(ti) = target else { return };
        let Some(mi) = lines[ti].msg else { return };
        self.focus = Some(ti);
        if let Some(UiMsg::ToolCall(item)) = self.messages.get_mut(mi) {
            if item.status != "running" {
                item.expanded = !item.expanded;
            }
        }
    }
}

pub async fn run(
    cfg: ClientConfig,
    project: String,
    continue_mode: bool,
    resume_session: Option<String>,
    attach_addr: Option<String>,
) -> Result<(), Box<dyn std::error::Error>> {
    let (client, mut events_rx, _engine_task) = match &attach_addr {
        Some(addr) => Client::attach(cfg, addr.clone()).await?,
        None => Client::spawn(cfg).await?,
    };
    let info = client.initialize().await?;

    let resume = if let Some(sid) = resume_session {
        Some(sid)
    } else if continue_mode {
        match client.session_list().await {
            Ok(list) => {
                let project_sessions: Vec<_> = list
                    .sessions
                    .iter()
                    .filter(|s| s.cwd == project)
                    .collect();
                project_sessions
                    .first()
                    .map(|s| s.session_id.clone())
                    .or_else(|| list.sessions.first().map(|s| s.session_id.clone()))
            }
            Err(e) => {
                eprintln!("[agent-tui] session_list failed: {e}");
                None
            }
        }
    } else {
        None
    };

    let sess = match &resume {
        Some(sid) => client.session_new_with_id(&project, sid).await?,
        None => client.session_new(&project).await?,
    };
    let model = client.model_name().to_string();
    let client = Arc::new(client);

    let mut app = App::new(client, sess.session_id, project, model.clone());
    let mut notice = format!(
        "connected: {} v{} (protocol {}) · model {}\n\
         enter send · shift+enter newline · esc interrupt (x2) · /help",
        info.agent_info.title, info.agent_info.version, info.protocol_version, model
    );
    if let Some(sid) = &resume {
        notice.push_str(&format!("\nresumed session {sid}"));
    }
    app.push(UiMsg::Info(notice));

    ratatui::init();
    let (input_tx, mut input_rx) = mpsc::channel::<UiEvent>(64);
    event::spawn_input_thread(input_tx);
    let mut terminal = ratatui::Terminal::new(ratatui::backend::CrosstermBackend::new(
        std::io::stdout(),
    ))?;

    let mut action_fut: Option<ActionFut> = None;

    // Draw the initial frame immediately so the UI isn't blank until the
    // first keypress.
    let _ = terminal.draw(|frame| crate::render::draw(frame, &app));

    let result: Result<(), Box<dyn std::error::Error>> = loop {
        tokio::select! {
            input = input_rx.recv() => {
                match input {
                    Some(UiEvent::Key(k)) => {
                        if app.pending_permission.is_some() {
                            if let Some(choice) = event::permission_choice(&k) {
                                let req = app
                                    .pending_permission
                                    .take()
                                    .expect("guarded by is_some");
                                if let Some(result) = choice {
                                    let arc = app.client.clone();
                                    tokio::spawn(async move {
                                        if let Err(e) = arc.respond(req.request_id, result).await {
                                            eprintln!("[tui] permission respond failed: {e}");
                                        }
                                    });
                                }
                            }
                        } else if app.pending_question.is_some() {
                            if event::is_interrupt(&k) {
                                let req = app
                                    .pending_question
                                    .take()
                                    .expect("guarded by is_some");
                                let arc = app.client.clone();
                                tokio::spawn(async move {
                                    if let Err(e) = arc
                                        .respond(req.request_id, serde_json::json!({ "answer": "" }))
                                        .await
                                    {
                                        eprintln!("[tui] question cancel failed: {e}");
                                    }
                                });
                            } else if event::is_enter(&k) {
                                let req = app
                                    .pending_question
                                    .take()
                                    .expect("guarded by is_some");
                                let answer = req.input.trim().to_string();
                                let arc = app.client.clone();
                                tokio::spawn(async move {
                                    if let Err(e) = arc
                                        .respond(req.request_id, serde_json::json!({ "answer": answer }))
                                        .await
                                    {
                                        eprintln!("[tui] question respond failed: {e}");
                                    }
                                });
                            } else {
                                if let Some((rid, answer)) =
                                    handle_question_key(&mut app.pending_question, &k)
                                {
                                    let arc = app.client.clone();
                                    tokio::spawn(async move {
                                        if let Err(e) = arc
                                            .respond(rid, serde_json::json!({ "answer": answer }))
                                            .await
                                        {
                                            eprintln!("[tui] question respond failed: {e}");
                                        }
                                    });
                                }
                            }
                        } else if event::is_quit(&k) && app.input.is_empty() {
                            app.should_quit = true;
                        } else if event::is_interrupt(&k) {
                            if app.busy {
                                if app.esc_armed {
                                    action_fut = None;
                                    let _ = app.client.session_cancel(&app.session_id).await;
                                    app.busy = false;
                                    app.push(UiMsg::Info("interrupted".into()));
                                } else {
                                    app.esc_armed = true;
                                    app.status = "esc again to interrupt".into();
                                }
                            }
                        } else if event::is_enter(&k) && !app.input.is_empty() {
                            if let Some(action) = app.submit() {
                                match action {
                                    SubmitAction::Prompt { session_id, text } => {
                                        let cwd = app.project.clone();
                                        let arc = app.client.clone();
                                        let fut = async move {
                                            Ok(ActionOutcome::Prompt(
                                                arc.session_prompt(&session_id, &text, Some(&cwd))
                                                    .await?,
                                            ))
                                        };
                                        action_fut = Some(Box::pin(fut));
                                    }
                                    SubmitAction::SwitchModel(name) => {
                                        let arc = app.client.clone();
                                        let fut = async move {
                                            Ok(ActionOutcome::SwitchModel(
                                                arc.switch_model(&name).await?,
                                            ))
                                        };
                                        action_fut = Some(Box::pin(fut));
                                    }
                                }
                            }
                        } else {
                            handle_key(&mut app, &k);
                        }
                        app.esc_armed = false;
                    }
                    Some(UiEvent::Error(e)) => app.push(UiMsg::Error(e)),
                    None => break Ok(()),
                    _ => {}
                }
            }
            ev = events_rx.recv() => {
                match ev {
                    Ok(ev) => app.on_event(ev),
                    Err(broadcast::error::RecvError::Closed) => {
                        break Err("engine closed".into());
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                }
            }
            res = async {
                action_fut.as_mut().expect("guarded by precondition").await
            }, if action_fut.is_some() => {
                match res {
                    Ok(ActionOutcome::Prompt(resp)) => {
                        app.busy = false;
                        if resp.stop_reason == "max_iterations" {
                            app.push(UiMsg::Info("reached max tool iterations".into()));
                        }
                    }
                    Ok(ActionOutcome::SwitchModel(name)) => {
                        app.busy = false;
                        app.model = name.clone();
                        app.push(UiMsg::Info(format!("model switched to {name}")));
                    }
                    Err(e) => {
                        app.busy = false;
                        app.push(UiMsg::Error(format!("turn error: {e}")));
                    }
                }
                action_fut = None;
            }
        }
        if app.should_quit {
            break Ok(());
        }
        let _ = terminal.draw(|frame| crate::render::draw(frame, &app));
    };

    ratatui::restore();
    let _ = app.client.session_close(&app.session_id).await;
    let _ = result?;
    Ok(())
}

fn handle_key(app: &mut App, k: &crossterm::event::KeyEvent) {
    use crossterm::event::{KeyCode, KeyModifiers};
    if event::is_soft_enter(k) {
        app.input.insert_char('\n');
        return;
    }
    if app.busy {
        return;
    }
    if app.input.is_empty() {
        match k.code {
            KeyCode::Down => app.scroll_step(1),
            KeyCode::Up => app.scroll_step(-1),
            KeyCode::PageDown | KeyCode::Char('f') if k.modifiers.contains(KeyModifiers::CONTROL) => {
                app.scroll_step(10)
            }
            KeyCode::PageUp | KeyCode::Char('b') if k.modifiers.contains(KeyModifiers::CONTROL) => {
                app.scroll_step(-10)
            }
            KeyCode::Enter | KeyCode::Tab => app.toggle_focus(),
            _ => handle_edit_key(app, k),
        }
    } else {
        handle_edit_key(app, k);
    }
}

fn handle_edit_key(app: &mut App, k: &crossterm::event::KeyEvent) {
    use crossterm::event::{KeyCode, KeyModifiers};
    let input = &mut app.input;
    match k.code {
        KeyCode::Char('a') if k.modifiers.contains(KeyModifiers::CONTROL) => input.move_home(),
        KeyCode::Char('e') if k.modifiers.contains(KeyModifiers::CONTROL) => input.move_end(),
        KeyCode::Char('b') if k.modifiers.contains(KeyModifiers::CONTROL) => input.move_left(),
        KeyCode::Char('f') if k.modifiers.contains(KeyModifiers::CONTROL) => input.move_right(),
        KeyCode::Char('w') if k.modifiers.contains(KeyModifiers::CONTROL) => input.kill_word_before(),
        KeyCode::Char('k') if k.modifiers.contains(KeyModifiers::CONTROL) => input.kill_to_end(),
        KeyCode::Char('u') if k.modifiers.contains(KeyModifiers::CONTROL) => input.kill_to_start(),
        KeyCode::Char('h') if k.modifiers.contains(KeyModifiers::CONTROL) => input.backspace(),
        KeyCode::Char('d') if k.modifiers.contains(KeyModifiers::CONTROL) => input.delete(),
        KeyCode::Left => input.move_left(),
        KeyCode::Right => input.move_right(),
        KeyCode::Home => input.move_home(),
        KeyCode::End => input.move_end(),
        KeyCode::Backspace => input.backspace(),
        KeyCode::Delete => input.delete(),
        KeyCode::Char(c) => input.insert_char(c),
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use agent_acp::types::Event;
    use serde_json::json;

    fn state() -> AppState {
        AppState::new("s1".into(), "/proj".into(), "test-model".into())
    }

    fn event(method: &str, params: Value) -> Event {
        Event {
            jsonrpc: "2.0".into(),
            method: method.into(),
            params: Some(params),
            engine_request_id: None,
        }
    }

    fn update(params: Value) -> Event {
        event("session/update", params)
    }

    #[test]
    fn submit_sends_prompt() {
        let mut s = state();
        s.input.set("hello agent");
        let action = s.submit().expect("action");
        match action {
            SubmitAction::Prompt { session_id, text } => {
                assert_eq!(session_id, "s1");
                assert_eq!(text, "hello agent");
            }
            other => panic!("expected prompt, got {other:?}"),
        }
        assert!(s.busy);
        assert!(s.input.is_empty());
        assert_eq!(s.messages.len(), 1); // user message pushed
    }

    #[test]
    fn submit_empty_or_busy_is_none() {
        let mut s = state();
        assert!(s.submit().is_none()); // empty
        s.input.set("text");
        s.busy = true;
        assert!(s.submit().is_none()); // busy blocks submit
        assert!(s.busy);
    }

    #[test]
    fn submit_model_command() {
        let mut s = state();
        s.input.set("/model deepseek-v4-pro");
        let action = s.submit().expect("action");
        match action {
            SubmitAction::SwitchModel(name) => assert_eq!(name, "deepseek-v4-pro"),
            other => panic!("expected switch model, got {other:?}"),
        }
        assert!(s.busy);
    }

    #[test]
    fn submit_unknown_slash_is_command() {
        let mut s = state();
        s.input.set("/nope");
        assert!(s.submit().is_none());
        assert!(!s.busy);
        assert!(matches!(s.messages.last(), Some(UiMsg::Error(_))));
    }

    #[test]
    fn quit_command() {
        let mut s = state();
        s.input.set("/quit");
        assert!(s.submit().is_none());
        assert!(s.should_quit);
    }

    #[test]
    fn bare_quit_words_exit() {
        for word in ["quit", "exit", "q"] {
            let mut s = state();
            s.input.set(word);
            assert!(s.submit().is_none(), "{word} should not send a prompt");
            assert!(s.should_quit, "{word} should quit");
        }
        // "quality" is not a quit word.
        let mut s = state();
        s.input.set("quality");
        assert!(s.submit().is_some());
        assert!(!s.should_quit);
    }

    #[test]
    fn help_and_models_commands() {
        let mut s = state();
        s.input.set("/help");
        s.submit();
        assert!(matches!(s.messages.last(), Some(UiMsg::Info(_))));
        s.input.set("/models");
        s.submit();
        assert!(matches!(s.messages.last(), Some(UiMsg::Info(_))));
    }

    #[test]
    fn new_command_clears_messages() {
        let mut s = state();
        s.push(UiMsg::User("old".into()));
        s.input.set("/new");
        s.submit();
        assert!(s.messages.is_empty());
        assert_eq!(s.status, "new session");
    }

    #[test]
    fn agent_chunk_accumulates() {
        let mut s = state();
        s.on_event(update(json!({
            "sessionId": "s1",
            "update": { "sessionUpdate": "agent_message_chunk", "content": { "type": "text", "text": "Hello" } }
        })));
        s.on_event(update(json!({
            "sessionId": "s1",
            "update": { "sessionUpdate": "agent_message_chunk", "content": { "type": "text", "text": " world" } }
        })));
        match &s.messages[0] {
            UiMsg::Assistant(t) => assert_eq!(t, "Hello world"),
            other => panic!("expected assistant, got {other:?}"),
        }
    }

    #[test]
    fn thinking_chunk_accumulates_separately() {
        let mut s = state();
        s.on_event(update(json!({
            "sessionId": "s1",
            "update": { "sessionUpdate": "agent_message_chunk", "content": { "type": "thinking", "text": "hmm" } }
        })));
        s.on_event(update(json!({
            "sessionId": "s1",
            "update": { "sessionUpdate": "agent_message_chunk", "content": { "type": "thinking", "text": "..." } }
        })));
        s.on_event(update(json!({
            "sessionId": "s1",
            "update": { "sessionUpdate": "agent_message_chunk", "content": { "type": "text", "text": "answer" } }
        })));
        assert_eq!(s.messages.len(), 2);
        match &s.messages[0] {
            UiMsg::Thinking(t) => assert_eq!(t, "hmm..."),
            other => panic!("expected thinking, got {other:?}"),
        }
        match &s.messages[1] {
            UiMsg::Assistant(t) => assert_eq!(t, "answer"),
            other => panic!("expected assistant, got {other:?}"),
        }
        let rendered = s.layout_lines();
        assert!(rendered.iter().any(|l| l.line.to_string().contains("Thinking")));
    }

    #[test]
    fn tool_call_lifecycle() {
        let mut s = state();
        s.on_event(update(json!({
            "sessionId": "s1",
            "update": {
                "sessionUpdate": "tool_call",
                "toolCallId": "t1",
                "title": "write",
                "kind": "other",
                "status": "in_progress",
                "rawInput": { "path": "a.txt", "content": "hi" }
            }
        })));
        assert_eq!(s.messages.len(), 1);
        match &s.messages[0] {
            UiMsg::ToolCall(item) => {
                assert_eq!(item.id, "t1");
                assert_eq!(item.status, "in_progress");
                assert!(item.expanded, "tool calls are expanded by default");
                assert!(item.output.contains("a.txt"), "raw input shown: {}", item.output);
            }
            other => panic!("expected toolcall, got {other:?}"),
        }
        s.on_event(update(json!({
            "sessionId": "s1",
            "update": {
                "sessionUpdate": "tool_call_update",
                "toolCallId": "t1",
                "status": "completed",
                "rawOutput": { "output": "wrote a.txt" }
            }
        })));
        match &s.messages[0] {
            UiMsg::ToolCall(item) => {
                assert_eq!(item.status, "completed");
                assert_eq!(item.output, "wrote a.txt");
            }
            other => panic!("expected toolcall, got {other:?}"),
        }
    }

    #[test]
    fn todo_add_and_update() {
        let mut s = state();
        s.on_event(update(json!({
            "sessionId": "s1",
            "update": { "sessionUpdate": "todo", "type": "upsert", "id": "a", "content": "fix", "status": "in_progress" }
        })));
        assert_eq!(s.todos.len(), 1);
        s.on_event(update(json!({
            "sessionId": "s1",
            "update": { "sessionUpdate": "todo", "type": "upsert", "id": "a", "content": "fix", "status": "completed" }
        })));
        assert_eq!(s.todos.len(), 1);
        assert_eq!(s.todos[0].status, "completed");
    }

    #[test]
    fn permission_request_sets_pending() {
        let mut s = state();
        let mut ev = event(
            "permission/request",
            json!({ "action": "bash", "resource": "ls" }),
        );
        ev.engine_request_id = Some(42);
        s.on_event(ev);
        let p = s.pending_permission.expect("pending");
        assert_eq!(p.request_id, 42);
        assert_eq!(p.action, "bash");
        assert_eq!(p.resource, "ls");
    }

    #[test]
    fn question_request_with_options() {
        let mut s = state();
        let mut ev = event(
            "question/request",
            json!({ "question": "pick", "options": ["one", "two"] }),
        );
        ev.engine_request_id = Some(7);
        s.on_event(ev);
        let q = s.pending_question.expect("pending");
        assert_eq!(q.options, vec!["one".to_string(), "two".to_string()]);
    }

    #[test]
    fn non_update_event_ignored() {
        let mut s = state();
        s.on_event(event("unknown/method", json!({})));
        assert!(s.messages.is_empty());
        assert!(s.pending_permission.is_none());
        assert!(s.pending_question.is_none());
    }

    #[test]
    fn question_key_numbered_options() {
        let mut q = Some(PendingQuestion {
            request_id: 5,
            question: "pick".into(),
            options: vec!["a".into(), "b".into()],
            input: String::new(),
        });
        let k = KeyEvent::new(KeyCode::Char('2'), KeyModifiers::NONE);
        let (rid, answer) = handle_question_key(&mut q, &k).expect("picked");
        assert_eq!(rid, 5);
        assert_eq!(answer, "b");
        assert!(q.is_none()); // dialog consumed
    }

    #[test]
    fn question_key_free_text_and_backspace() {
        let mut q = Some(PendingQuestion {
            request_id: 1,
            question: "name?".into(),
            options: vec![],
            input: String::new(),
        });
        let k = KeyEvent::new(KeyCode::Char('h'), KeyModifiers::NONE);
        assert!(handle_question_key(&mut q, &k).is_none());
        assert_eq!(q.as_ref().unwrap().input, "h");
        let bs = KeyEvent::new(KeyCode::Backspace, KeyModifiers::NONE);
        assert!(handle_question_key(&mut q, &bs).is_none());
        assert_eq!(q.as_ref().unwrap().input, "");
    }

    #[test]
    fn layout_lines_structure() {
        let mut s = state();
        s.push(UiMsg::User("hi".into()));
        s.push(UiMsg::Assistant("yo".into()));
        s.push(UiMsg::Info("note".into()));
        s.push(UiMsg::Error("bad".into()));
        let lines = s.layout_lines();
        let text: Vec<String> = lines.iter().map(|l| l.line.to_string()).collect();
        let joined = text.join("\n");
        assert!(joined.contains("┌ You"));
        assert!(joined.contains("│ hi"));
        assert!(joined.contains("┌ Assistant"));
        assert!(joined.contains("│ yo"));
        assert!(joined.contains("ℹ note"));
        assert!(joined.contains("✖ bad"));
    }

    #[test]
    fn view_top_empty_and_scroll_to_bottom() {
        let s = state();
        assert_eq!(s.view_top(10), 0); // empty => top 0
        let mut s = state();
        for i in 0..20 {
            s.push(UiMsg::User(format!("line {i}")));
        }
        // No focus => anchored to bottom.
        assert_eq!(s.view_top(10), s.layout_lines().len().saturating_sub(10));
        // With focus => centered on that line.
        s.focus = Some(0);
        assert_eq!(s.view_top(10), 0);
    }

    #[test]
    fn scroll_step_moves_focus() {
        let mut s = state();
        for i in 0..20 {
            s.push(UiMsg::User(format!("line {i}")));
        }
        let total = s.layout_lines().len();
        // Scrolling down past the bottom stays anchored at the bottom (focus None).
        s.scroll_step(1);
        assert!(s.focus.is_none());
        // Scrolling up from the bottom moves focus into view.
        s.scroll_step(-1);
        assert!(s.focus.is_some());
        assert!(s.focus.unwrap() + 1 < total);
        // Scrolling far up clamps at the top.
        s.scroll_step(-1000);
        assert_eq!(s.focus, Some(0));
    }

    fn style_fg(s: &Style) -> Option<Color> {
        s.fg
    }

    #[test]
    fn diff_detection() {
        let diff = "diff --git a/app.txt b/app.txt\nindex 3b18e51..a2b2a71 100644\n--- a/app.txt\n+++ b/app.txt\n@@ -1 +1,2 @@\n hello world\n+hello world again\n";
        let is_diff = diff.lines().any(|l| {
            l.starts_with("diff --git")
                || (l.starts_with("@@") && l.contains("@@"))
                || l.starts_with("+++ ")
        });
        assert!(is_diff, "diff output should be detected");

        let normal = "-rw-r--r-- 1 root root 10 file.txt\n-rw-r--r-- 1 root root 20 other\n";
        let is_diff = normal.lines().any(|l| {
            l.starts_with("diff --git")
                || (l.starts_with("@@") && l.contains("@@"))
                || l.starts_with("+++ ")
        });
        assert!(!is_diff, "ls-style output should not be flagged as diff");
    }

    #[test]
    fn diff_line_colors() {
        let cases = [
            ("diff --git a/x b/x", Some(Color::Cyan)),
            ("index 3b18e51..a2b2a71 100644", Some(Color::Cyan)),
            ("--- a/x", Some(Color::Cyan)),
            ("+++ b/x", Some(Color::Cyan)),
            ("+added line", Some(Color::Green)),
            ("-removed line", Some(Color::Red)),
            ("@@ -1 +1,2 @@", Some(Color::Cyan)),
            (" context line", None),
            ("  indented context", None),
        ];
        for (line, want) in cases {
            let got = diff_line_style(line).map(|s| style_fg(&s)).flatten();
            assert_eq!(got, want, "line: {line:?}");
        }
    }
}

use agent_acp::types::{SessionUpdateParams, Update};
use agent_acp::{AcpError, Client, ClientConfig};
use serde_json::Value;
use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use tokio::sync::broadcast;
use tokio::sync::mpsc;

use crate::event::{self, UiEvent};
use crate::input::Input;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};

#[derive(Clone)]
pub struct ToolItem {
    pub id: String,
    pub title: String,
    pub kind: String,
    pub status: String,
    pub output: String,
    pub expanded: bool,
}

#[derive(Clone)]
pub enum UiMsg {
    User(String),
    Assistant(String),
    ToolCall(ToolItem),
    Info(String),
    Error(String),
}

pub struct PendingPermission {
    pub request_id: u64,
    pub action: String,
    pub resource: String,
}

pub struct App {
    pub client: Arc<Client>,
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
}

type PromptFut = Pin<Box<dyn Future<Output = Result<agent_acp::SessionPromptResult, AcpError>>>>;

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

impl App {
    pub fn new(client: Arc<Client>, session_id: String, project: String, model: String) -> Self {
        Self {
            client,
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
        }
    }

    pub fn push(&mut self, msg: UiMsg) {
        self.messages.push(msg);
    }

    pub fn submit(&mut self) -> Option<(String, String)> {
        let text = self.input.buf.trim().to_string();
        if text.is_empty() || self.busy {
            return None;
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
        Some((self.session_id.clone(), text))
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
                    "models: deepseek-v4-flash, deepseek-v4-pro (current: {}; restart to switch via AICODING_MODEL)",
                    self.model
                )));
            }
            "/help" => {
                self.messages.push(UiMsg::Info(
                    "commands: /quit /new /models /help\n\
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
                if let Some(UiMsg::Assistant(buf)) = self.messages.last_mut() {
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
                ..
            } => {
                self.messages.push(UiMsg::ToolCall(ToolItem {
                    id: tool_call_id,
                    title,
                    kind,
                    status,
                    output: String::new(),
                    expanded: false,
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
) -> Result<(), Box<dyn std::error::Error>> {
    let (client, mut events_rx, _engine_task) = Client::spawn(cfg).await?;
    let info = client.initialize().await?;
    let sess = client.session_new(&project).await?;
    let model = client.model_name().to_string();
    let client = Arc::new(client);

    let mut app = App::new(client, sess.session_id, project, model.clone());
    app.push(UiMsg::Info(format!(
        "connected: {} v{} (protocol {}) · model {}\n\
         enter send · shift+enter newline · esc interrupt (x2) · /help",
        info.agent_info.title, info.agent_info.version, info.protocol_version, model
    )));

    ratatui::init();
    let (input_tx, mut input_rx) = mpsc::channel::<UiEvent>(64);
    event::spawn_input_thread(input_tx);
    let mut terminal = ratatui::Terminal::new(ratatui::backend::CrosstermBackend::new(
        std::io::stdout(),
    ))?;

    let mut prompt_fut: Option<PromptFut> = None;

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
                        } else if event::is_quit(&k) && app.input.is_empty() {
                            app.should_quit = true;
                        } else if event::is_interrupt(&k) {
                            if app.busy {
                                if app.esc_armed {
                                    prompt_fut = None;
                                    let _ = app.client.session_cancel(&app.session_id).await;
                                    app.busy = false;
                                    app.push(UiMsg::Info("interrupted".into()));
                                } else {
                                    app.esc_armed = true;
                                    app.status = "esc again to interrupt".into();
                                }
                            }
                        } else if event::is_enter(&k) && !app.input.is_empty() {
                            if let Some((sid, text)) = app.submit() {
                                let cwd = app.project.clone();
                                let arc = app.client.clone();
                                let fut = async move {
                                    arc.session_prompt(&sid, &text, Some(&cwd)).await
                                };
                                prompt_fut = Some(Box::pin(fut));
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
                prompt_fut.as_mut().expect("guarded by precondition").await
            }, if prompt_fut.is_some() => {
                match res {
                    Ok(resp) => {
                        app.busy = false;
                        if resp.stop_reason == "max_iterations" {
                            app.push(UiMsg::Info("reached max tool iterations".into()));
                        }
                    }
                    Err(e) => {
                        app.busy = false;
                        app.push(UiMsg::Error(format!("turn error: {e}")));
                    }
                }
                prompt_fut = None;
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

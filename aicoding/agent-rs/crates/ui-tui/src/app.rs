use agent_acp::types::{SessionUpdateParams, Update};
use agent_acp::{AcpError, Client, ClientConfig};
use std::sync::Arc;
use std::future::Future;
use std::pin::Pin;
use tokio::sync::broadcast;
use tokio::sync::mpsc;

use crate::event::{self, UiEvent};
use crate::input::Input;

#[derive(Clone)]
pub enum UiMsg {
    User(String),
    Assistant(String),
    ToolStart {
        id: String,
        title: String,
        kind: String,
    },
    ToolDone {
        id: String,
        status: String,
    },
    Info(String),
    Error(String),
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
}

type PromptFut = Pin<Box<dyn Future<Output = Result<agent_acp::SessionPromptResult, AcpError>>>>;

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
        Some((self.session_id.clone(), text))
    }

    fn handle_command(&mut self, cmd: &str) {
        let (name, args) = match cmd.split_once(' ') {
            Some((n, a)) => (n, a.trim()),
            None => (cmd, ""),
        };
        match name {
            "/quit" | "/exit" | "/q" => self.should_quit = true,
            "/new" => {
                self.messages.clear();
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
                     ctrl+a/e home/end · ctrl+w/k/u edit · up/down history"
                        .into(),
                ));
            }
            other => {
                self.messages
                    .push(UiMsg::Error(format!("unknown command: {other}")));
            }
        }
        let _ = args;
    }

    pub fn on_event(&mut self, ev: agent_acp::types::Event) {
        if ev.method != "session/update" {
            return;
        }
        let Ok(params) = serde_json::from_value::<SessionUpdateParams>(
            ev.params.unwrap_or(serde_json::Value::Null),
        ) else {
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
                ..
            } => {
                self.messages.push(UiMsg::ToolStart {
                    id: tool_call_id,
                    title,
                    kind,
                });
            }
            Update::ToolCallUpdate {
                tool_call_id,
                status,
                ..
            } => {
                self.messages.push(UiMsg::ToolDone {
                    id: tool_call_id,
                    status,
                });
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
                        if event::is_quit(&k) && app.input.is_empty() {
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
                        } else if event::is_soft_enter(&k) {
                            app.input.insert_char('\n');
                        } else if event::is_enter(&k) {
                            if let Some((sid, text)) = app.submit() {
                                let cwd = app.project.clone();
                                let arc = app.client.clone();
                                let fut = async move {
                                    arc.session_prompt(&sid, &text, Some(&cwd)).await
                                };
                                prompt_fut = Some(Box::pin(fut));
                            }
                        } else if k.code == crossterm::event::KeyCode::Up {
                            if let Some(idx) = app.hist_idx {
                                if idx > 0 {
                                    app.hist_idx = Some(idx - 1);
                                    app.input.set(&app.history[idx - 1]);
                                }
                            } else if !app.history.is_empty() {
                                app.hist_idx = Some(app.history.len() - 1);
                                app.input.set(&app.history[app.history.len() - 1]);
                            }
                        } else if k.code == crossterm::event::KeyCode::Down {
                            if let Some(idx) = app.hist_idx {
                                if idx + 1 < app.history.len() {
                                    app.hist_idx = Some(idx + 1);
                                    app.input.set(&app.history[idx + 1]);
                                } else {
                                    app.hist_idx = None;
                                    app.input.clear();
                                }
                            }
                        } else {
                            handle_edit_key(&mut app, &k);
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

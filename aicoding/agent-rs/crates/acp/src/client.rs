use std::collections::HashMap;
use std::path::Path;
use std::process::Stdio;
use std::sync::Arc;

use serde::Serialize;
use serde_json::{json, Value};
use thiserror::Error;
use tokio::sync::{mpsc, oneshot, Mutex};
use tokio::task::JoinHandle;

use crate::types::*;

#[derive(Debug, Error)]
pub enum AcpError {
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
    #[error("protocol: {0}")]
    Protocol(String),
    #[error("engine exited: {0}")]
    EngineExited(String),
    #[error("engine error: code={0} {1}")]
    Engine(i64, String),
}

pub struct Client {
    tx: mpsc::Sender<Outbound>,
    next_id: Arc<std::sync::atomic::AtomicU64>,
    engine_name: String,
    model: String,
}

impl Client {
    pub fn model_name(&self) -> &str {
        &self.model
    }
}

#[derive(Clone)]
pub struct ClientConfig {
    pub bin_path: String,
    pub project_root: String,
    pub model: String,
    pub session: Option<String>,
    pub env_file: Option<String>,
    pub extra_env: Vec<(String, String)>,
    pub ld_library_path: Option<String>,
}

enum Outbound {
    Request {
        id: u64,
        payload: String,
        reply: oneshot::Sender<Result<Value, AcpError>>,
    },
    Raw(String),
}

impl Client {
    pub async fn spawn(
        cfg: ClientConfig,
    ) -> Result<(Self, tokio::sync::broadcast::Receiver<Event>, JoinHandle<Result<(), AcpError>>), AcpError>
    {
        Self::open(cfg, None).await
    }

    /// Connect to a remote engine bridge (`aicoding serve --port N`).
    pub async fn attach(
        cfg: ClientConfig,
        addr: String,
    ) -> Result<(Self, tokio::sync::broadcast::Receiver<Event>, JoinHandle<Result<(), AcpError>>), AcpError>
    {
        Self::open(cfg, Some(addr)).await
    }

    async fn open(
        cfg: ClientConfig,
        tcp_addr: Option<String>,
    ) -> Result<(Self, tokio::sync::broadcast::Receiver<Event>, JoinHandle<Result<(), AcpError>>), AcpError>
    {
        use tokio::io::{AsyncBufReadExt, AsyncRead, AsyncWrite, AsyncWriteExt, BufReader};

        let engine_name: String;
        let writer: Box<dyn AsyncWrite + Unpin + Send>;
        let reader: Box<dyn AsyncRead + Unpin + Send>;
        if let Some(addr) = &tcp_addr {
            let stream = tokio::net::TcpStream::connect(addr).await?;
            let (r, w) = stream.into_split();
            writer = Box::new(w);
            reader = Box::new(r);
            engine_name = format!("{addr} (tcp)");
        } else {
            let mut cmd = tokio::process::Command::new(&cfg.bin_path);
            cmd.args(["--acp", "--project", &cfg.project_root, "--model", &cfg.model])
                .stdin(Stdio::piped())
                .stdout(Stdio::piped())
                .stderr(Stdio::inherit());
            if let Some(session) = &cfg.session {
                cmd.args(["--session", session]);
            }
            if let Some(env_file) = &cfg.env_file {
                cmd.args(["--env", env_file]);
            }
            if let Some(ld) = &cfg.ld_library_path {
                cmd.env("LD_LIBRARY_PATH", ld);
            }
            for (k, v) in &cfg.extra_env {
                cmd.env(k, v);
            }
            let mut child = cmd.spawn()?;
            let pid = child.id().unwrap_or(0);
            let stdin = child.stdin.take().ok_or_else(|| AcpError::Protocol("no stdin".into()))?;
            let stdout = child.stdout.take().ok_or_else(|| AcpError::Protocol("no stdout".into()))?;
            writer = Box::new(stdin);
            reader = Box::new(stdout);
            engine_name = format!("{} (pid {})", cfg.bin_path, pid);
        }

        let (tx, mut rx) = mpsc::channel::<Outbound>(64);
        let (events_tx, events_rx) = tokio::sync::broadcast::channel::<Event>(256);
        let pending = Arc::new(Mutex::new(HashMap::new()));

        let writer_pending = pending.clone();
        let mut writer_stdin = writer;
        tokio::spawn(async move {
            while let Some(msg) = rx.recv().await {
                match msg {
                    Outbound::Request { id, payload, reply } => {
                        match writer_stdin.write_all(payload.as_bytes()).await {
                            Ok(_) => match writer_stdin.flush().await {
                                Ok(_) => {
                                    let _ = writer_pending.lock().await.insert(id, reply);
                                }
                                Err(e) => {
                                    let _ = reply.send(Err(AcpError::Io(e)));
                                }
                            },
                            Err(e) => {
                                let _ = reply.send(Err(AcpError::Io(e)));
                            }
                        }
                    }
                    Outbound::Raw(payload) => match writer_stdin.write_all(payload.as_bytes()).await {
                        Ok(_) => {
                            let _ = writer_stdin.flush().await;
                        }
                        Err(e) => {
                            eprintln!("[acp] respond write failed: {e}");
                        }
                    },
                }
            }
        });

        let reader_pending = pending.clone();
        let reader_events = events_tx.clone();
        let mut reader = BufReader::new(reader);
        let mut buf = String::new();
        let reader_task = tokio::spawn(async move {
            loop {
                buf.clear();
                let n = reader.read_line(&mut buf).await.map_err(AcpError::Io)?;
                if n == 0 {
                    return Err(AcpError::EngineExited("stdout closed".into()));
                }
                let line = buf.trim();
                if line.is_empty() {
                    continue;
                }
                let v: Value = match serde_json::from_str(line) {
                    Ok(v) => v,
                    Err(_) => continue,
                };
                let has_id = v.get("id").is_some_and(|i| i.is_number());
                let is_response = has_id && (v.get("result").is_some() || v.get("error").is_some());
                if has_id && !is_response {
                    // Engine-initiated request (e.g. permission/request):
                    // broadcast it; the caller answers via Client::respond.
                    let ev = Event {
                        jsonrpc: "2.0".into(),
                        method: v["method"].as_str().unwrap_or("").to_string(),
                        params: v.get("params").cloned(),
                        engine_request_id: v["id"].as_u64(),
                    };
                    let _ = reader_events.send(ev);
                } else if is_response {
                    let id = v["id"].as_u64().unwrap_or(0);
                    if let Some(reply) = reader_pending.lock().await.remove(&id) {
                        let result = if let Some(err) = v.get("error") {
                            Err(AcpError::Engine(
                                err["code"].as_i64().unwrap_or(-1),
                                err["message"].as_str().unwrap_or("unknown").to_string(),
                            ))
                        } else {
                            Ok(v.get("result").cloned().unwrap_or(Value::Null))
                        };
                        let _ = reply.send(result);
                    }
                } else {
                    if let Ok(ev) = serde_json::from_value::<Event>(v) {
                        let _ = reader_events.send(ev);
                    }
                }
            }
        });

        let client = Self {
            tx,
            next_id: Arc::new(std::sync::atomic::AtomicU64::new(1)),
            engine_name,
            model: cfg.model.clone(),
        };
        client
            .request::<InitializeParams, InitializeResult>("initialize", Some(InitializeParams {}))
            .await?;
        Ok((client, events_rx, reader_task))
    }

    pub async fn request<Req: Serialize, Res: for<'de> serde::Deserialize<'de>>(
        &self,
        method: &str,
        params: Option<Req>,
    ) -> Result<Res, AcpError> {
        let id = self.next_id.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        let (reply_tx, reply_rx) = oneshot::channel();
        let payload = serde_json::to_string(&Request {
            jsonrpc: "2.0".into(),
            id,
            method: method.into(),
            params: params.map(|p| serde_json::to_value(p).expect("serialize params")),
        })
        .map_err(|e| AcpError::Protocol(e.to_string()))?
            + "\n";
        self.tx
            .send(Outbound::Request { id, payload, reply: reply_tx })
            .await
            .map_err(|_| AcpError::EngineExited("engine channel closed".into()))?;
        let raw = reply_rx
            .await
            .map_err(|_| AcpError::EngineExited("engine died mid-request".into()))??;
        serde_json::from_value(raw).map_err(|e| AcpError::Protocol(format!("bad response: {e}")))
    }

    pub async fn notify(&self, method: &str, params: Value) -> Result<(), AcpError> {
        let id = self.next_id.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        let (reply_tx, _) = oneshot::channel();
        let payload = json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params})
            .to_string()
            + "\n";
        self.tx
            .send(Outbound::Request { id, payload, reply: reply_tx })
            .await
            .map_err(|_| AcpError::EngineExited("engine channel closed".into()))
    }

    pub async fn initialize(&self) -> Result<InitializeResult, AcpError> {
        self.request("initialize", Some(InitializeParams {})).await
    }

    pub async fn session_new(&self, cwd: &str) -> Result<SessionNewResult, AcpError> {
        self.request("session/new", Some(SessionNewParams { cwd: cwd.into(), session_id: None }))
            .await
    }

    /// Open a session, optionally resuming an existing one by id
    /// (used by --continue / --session).
    pub async fn session_new_with_id(&self, cwd: &str, session_id: &str) -> Result<SessionNewResult, AcpError> {
        self.request(
            "session/new",
            Some(SessionNewParams {
                cwd: cwd.into(),
                session_id: Some(session_id.into()),
            }),
        )
        .await
    }

    pub async fn session_prompt(
        &self,
        session_id: &str,
        text: &str,
        cwd: Option<&str>,
    ) -> Result<SessionPromptResult, AcpError> {
        self.request(
            "session/prompt",
            Some(SessionPromptParams {
                session_id: session_id.into(),
                cwd: cwd.map(String::from),
                prompt: vec![Prompt { text: text.into() }],
            }),
        )
        .await
    }

    pub async fn session_set_mode(
        &self,
        session_id: &str,
        mode_id: &str,
    ) -> Result<SessionSetModeResult, AcpError> {
        self.request(
            "session/set_mode",
            Some(SessionSetModeParams {
                session_id: session_id.into(),
                mode_id: mode_id.into(),
            }),
        )
        .await
    }

    pub async fn session_close(&self, session_id: &str) -> Result<(), AcpError> {
        let _: Value = self.request(
            "session/close",
            Some(SessionCloseParams { session_id: session_id.into() }),
        )
        .await?;
        Ok(())
    }

    pub async fn session_list(&self) -> Result<SessionListResult, AcpError> {
        self.request("session/list", Some(SessionListParams {})).await
    }

    pub async fn switch_model(&self, name: &str) -> Result<String, AcpError> {
        #[derive(serde::Serialize)]
        struct SwitchParams<'a> {
            model: &'a str,
        }
        #[derive(serde::Deserialize)]
        struct SwitchResult {
            model: String,
        }
        Ok(self
            .request::<_, SwitchResult>("model/switch", Some(SwitchParams { model: name }))
            .await?
            .model)
    }

    pub async fn session_cancel(&self, session_id: &str) -> Result<(), AcpError> {
        self.notify("session/cancel", json!({ "sessionId": session_id }))
            .await
    }

    /// Answer an engine-initiated request (see Event::engine_request_id).
    pub async fn respond(&self, id: u64, result: Value) -> Result<(), AcpError> {
        let payload = json!({ "jsonrpc": "2.0", "id": id, "result": result }).to_string() + "\n";
        self.tx
            .send(Outbound::Raw(payload))
            .await
            .map_err(|_| AcpError::EngineExited("engine channel closed".into()))
    }

    pub fn engine_name(&self) -> &str {
        &self.engine_name
    }
}

impl ClientConfig {
    pub fn default_config(project_root: &Path) -> ClientConfig {
        ClientConfig {
            bin_path: std::env::var("AICODING_BIN")
                .unwrap_or_else(|_| "/opt/my_db/aicoding/aicoding".into()),
            project_root: project_root.display().to_string(),
            model: std::env::var("AICODING_MODEL").unwrap_or_else(|_| "kimi-latest".into()),
            session: None,
            env_file: std::env::var("AICODING_ENV_FILE").ok(),
            extra_env: vec![],
            ld_library_path: std::env::var("AICODING_LD_PATH")
                .ok()
                .or_else(|| {
                    let mut p = "/opt/my_db".to_string();
                    if let Ok(lj) = std::env::var("LUAJIT_LIB") {
                        p.push(':');
                        p.push_str(&lj);
                    }
                    p.push_str(":.");
                    Some(p)
                }),
        }
    }
}

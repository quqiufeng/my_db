//! Integration test: `aicoding serve --port N` bridge + `Client::attach`.
//!
//! This exercises the exact code path the TUI uses to drive a remote engine
//! (attach -> initialize -> session/new -> prompt -> session/update events),
//! but from a headless test, so no pty/terminal is required.
//!
//! Requires a fake LLM endpoint on 127.0.0.1:9999 (pong server) and the
//! engine binary at the configured path. Skips silently when the endpoint or
//! engine is unavailable so the test stays green in CI-like environments.

use std::path::Path;
use std::process::Stdio;
use std::time::Duration;

use agent_acp::types::{Event, Update};
use agent_acp::{Client, ClientConfig};

fn engine_bin() -> String {
    std::env::var("AICODING_BIN")
        .unwrap_or_else(|_| "/opt/my_db/aicoding/aicoding".into())
}

async fn llm_available() -> bool {
    match tokio::net::TcpStream::connect("127.0.0.1:9999").await {
        Ok(_) => true,
        Err(_) => false,
    }
}

async fn start_serve(port: u16) -> tokio::process::Child {
    let mut cmd = tokio::process::Command::new(engine_bin());
    cmd.arg("serve")
        .arg("--port")
        .arg(port.to_string())
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    cmd.env("OPENAI_BASE_URL", "http://127.0.0.1:9999");
    if let Ok(ld) = std::env::var("AICODING_LD_PATH") {
        cmd.env("LD_LIBRARY_PATH", ld);
    }
    cmd.spawn().expect("spawn engine")
}

async fn wait_listen(port: u16, mut child: &mut tokio::process::Child) -> bool {
    for _ in 0..30 {
        if tokio::net::TcpStream::connect(("127.0.0.1", port)).await.is_ok() {
            return true;
        }
        // If the child died, give up.
        if let Some(status) = child.try_wait().ok().flatten() {
            eprintln!("serve exited early: {status}");
            return false;
        }
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    false
}

#[tokio::test]
async fn attach_prompt_receives_events() {
    if !llm_available().await {
        eprintln!("skipping: no fake LLM on 127.0.0.1:9999");
        return;
    }
    let bin = engine_bin();
    if !Path::new(&bin).exists() {
        eprintln!("skipping: engine binary {bin} not found");
        return;
    }

    let port: u16 = 19876;
    let mut serve = start_serve(port).await;
    if !wait_listen(port, &mut serve).await {
        serve.kill().await.ok();
        panic!("serve did not listen on {port}");
    }

    let cfg = ClientConfig::default_config(Path::new("/tmp/conttest"));
    let (client, mut events_rx, _task) = Client::attach(cfg, format!("127.0.0.1:{port}"))
        .await
        .expect("attach");
    let info = client.initialize().await.expect("initialize");
    assert!(!info.agent_info.title.is_empty());

    let sess = client.session_new("/tmp/conttest").await.expect("session/new");
    client
        .session_prompt(&sess.session_id, "hi", Some("/tmp/conttest"))
        .await
        .expect("prompt");

    // Collect a couple of events; expect at least one session/update.
    let mut saw_update = false;
    let mut saw_agent_chunk = false;
    let deadline = tokio::time::sleep(Duration::from_secs(15));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            ev = events_rx.recv() => {
                match ev {
                    Ok(Event { method, params, .. }) if method == "session/update" => {
                        saw_update = true;
                        if let Ok(p) = serde_json::from_value::<agent_acp::types::SessionUpdateParams>(
                            params.unwrap_or(serde_json::Value::Null),
                        ) {
                            if matches!(p.update, Update::AgentMessageChunk { .. }) {
                                saw_agent_chunk = true;
                            }
                        }
                    }
                    Ok(_) => {}
                    Err(_) => break,
                }
                if saw_agent_chunk {
                    break;
                }
            }
            _ = &mut deadline => break,
        }
    }

    client.session_close(&sess.session_id).await.ok();
    let _ = serve.kill().await;
    let _ = serve.wait().await;

    assert!(saw_update, "expected at least one session/update event");
    assert!(
        saw_agent_chunk,
        "expected an agent_message_chunk event from the pong reply"
    );
}

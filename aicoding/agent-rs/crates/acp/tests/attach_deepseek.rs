use std::path::Path;
use std::time::Duration;
use agent_acp::types::{Event, Update};
use agent_acp::{Client, ClientConfig};

#[tokio::test]
async fn attach_live_deepseek_pong() {
    // Requires `aicoding serve --port 9876 --env ~/.aicoding/.env` running.
    // Skip gracefully when it isn't.
    match tokio::net::TcpStream::connect("127.0.0.1:9876").await {
        Ok(_) => {}
        Err(_) => {
            eprintln!("skipping: no serve on 127.0.0.1:9876 (run aicoding serve --port 9876)");
            return;
        }
    }
    let cfg = ClientConfig::default_config(Path::new("/tmp/aicoding_demo"));
    let (client, mut events_rx, _t) = Client::attach(cfg, "127.0.0.1:9876".into()).await.expect("attach");
    client.initialize().await.expect("init");
    let sess = client.session_new("/tmp/aicoding_demo").await.expect("new");
    client.session_prompt(&sess.session_id, "reply with exactly: pong", Some("/tmp/aicoding_demo")).await.expect("prompt");
    let mut got = String::new();
    let deadline = tokio::time::sleep(Duration::from_secs(25));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            ev = events_rx.recv() => {
                match ev {
                    Ok(Event { method, params, .. }) if method == "session/update" => {
                        if let Ok(p) = serde_json::from_value::<agent_acp::types::SessionUpdateParams>(params.unwrap_or(serde_json::Value::Null)) {
                            if let Update::AgentMessageChunk { content } = p.update {
                                // Skip reasoning (thinking) chunks; only the visible reply.
                                if content.kind != "thinking" {
                                    got.push_str(&content.text);
                                }
                            }
                        }
                    }
                    Ok(_) => {}
                    Err(_) => break,
                }
            }
            _ = &mut deadline => break,
        }
    }
    client.session_close(&sess.session_id).await.ok();
    println!("ASSISTANT TEXT: [{got}]");
    assert!(
        got.trim().eq_ignore_ascii_case("pong"),
        "expected pong, got: {got}"
    );
}

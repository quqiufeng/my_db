use std::path::Path;
use std::time::Duration;
use agent_acp::types::{Event, Update};
use agent_acp::{Client, ClientConfig};

#[tokio::test]
async fn thinking_check() {
    match tokio::net::TcpStream::connect("127.0.0.1:9876").await {
        Ok(_) => {}
        Err(_) => { eprintln!("skipping: no serve"); return; }
    }
    let cfg = ClientConfig::default_config(Path::new("/tmp/aicoding_demo"));
    let (client, mut rx, _t) = Client::attach(cfg, "127.0.0.1:9876".into()).await.unwrap();
    client.initialize().await.unwrap();
    let sess = client.session_new("/tmp/aicoding_demo").await.unwrap();
    client.session_prompt(&sess.session_id, "what is 2+2? answer briefly", Some("/tmp/aicoding_demo")).await.unwrap();
    let mut n_text = 0;
    let mut n_thinking = 0;
    let deadline = tokio::time::sleep(Duration::from_secs(30));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            ev = rx.recv() => match ev {
                Ok(Event { method, params, .. }) if method == "session/update" => {
                    if let Ok(p) = serde_json::from_value::<agent_acp::types::SessionUpdateParams>(params.unwrap_or(serde_json::Value::Null)) {
                        if let Update::AgentMessageChunk { content } = p.update {
                            if content.kind == "thinking" { n_thinking += 1; }
                            else { n_text += 1; }
                        }
                    }
                }
                Ok(_) => {}
                Err(_) => break,
            },
            _ = &mut deadline => break,
        }
    }
    println!("THINKING={n_thinking} TEXT={n_text}");
}

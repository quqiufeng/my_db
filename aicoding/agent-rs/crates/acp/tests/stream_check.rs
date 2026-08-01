use std::path::Path;
use std::time::Duration;
use agent_acp::types::{Event, Update};
use agent_acp::{Client, ClientConfig};

#[tokio::test]
async fn stream_check() {
    match tokio::net::TcpStream::connect("127.0.0.1:9876").await {
        Ok(_) => {}
        Err(_) => { eprintln!("skipping: no serve"); return; }
    }
    let cfg = ClientConfig::default_config(Path::new("/tmp/aicoding_demo"));
    let (client, mut rx, _t) = Client::attach(cfg, "127.0.0.1:9876".into()).await.unwrap();
    client.initialize().await.unwrap();
    let sess = client.session_new("/tmp/aicoding_demo").await.unwrap();
    let t0 = std::time::Instant::now();
    client.session_prompt(&sess.session_id, "count from 1 to 10, writing each number on its own line", Some("/tmp/aicoding_demo")).await.unwrap();
    let mut nchunks = 0;
    let mut first_chunk_time: Option<Duration> = None;
    let deadline = tokio::time::sleep(Duration::from_secs(30));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            ev = rx.recv() => match ev {
                Ok(Event { method, params, .. }) if method == "session/update" => {
                    if let Ok(p) = serde_json::from_value::<agent_acp::types::SessionUpdateParams>(params.unwrap_or(serde_json::Value::Null)) {
                        if let Update::AgentMessageChunk { .. } = p.update {
                            nchunks += 1;
                            if first_chunk_time.is_none() { first_chunk_time = Some(t0.elapsed()); }
                        }
                    }
                }
                Ok(_) => {}
                Err(_) => break,
            },
            _ = &mut deadline => break,
        }
    }
    println!("CHUNKS={nchunks} FIRST_CHUNK={:?}", first_chunk_time.map(|d| d.as_millis()));
    assert!(nchunks > 1, "expected streaming (>1 chunk), got {nchunks}");
}

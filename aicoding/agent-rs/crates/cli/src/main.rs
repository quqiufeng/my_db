use std::env;
use std::io::Write;
use std::path::PathBuf;

use agent_acp::types::{SessionUpdateParams, Update};
use agent_acp::{Client, ClientConfig};

async fn run_turn(
    client: &Client,
    events_rx: tokio::sync::broadcast::Receiver<agent_acp::types::Event>,
    session_id: &str,
    text: &str,
    cwd: &str,
) -> Result<bool, Box<dyn std::error::Error>> {
    let mut events = events_rx;
    let prompt = client.session_prompt(session_id, text, Some(cwd));
    tokio::pin!(prompt);
    let mut ctrl_c_count = 0;
    loop {
        tokio::select! {
            result = &mut prompt => {
                let resp = result?;
                eprintln!("\n[agent-cli] done: {}", resp.stop_reason);
                return Ok(false);
            }
            ev = events.recv() => {
                match ev {
                    Ok(ev) if ev.method == "session/update" => {
                        let params: SessionUpdateParams =
                            serde_json::from_value(ev.params.unwrap_or(serde_json::Value::Null))?;
                        match params.update {
                            Update::AgentMessageChunk { content } => {
                                print!("{}", content.text);
                                std::io::stdout().flush()?;
                            }
                            Update::ToolCall { tool_call_id, title, kind, .. } => {
                                println!("\n\ntool[{tool_call_id}] {title} ({kind}) ...");
                            }
                            Update::ToolCallUpdate { tool_call_id, status, .. } => {
                                println!("tool[{tool_call_id}] -> {status}");
                            }
                            Update::Todo { content, status, .. } => {
                                println!("\ntodo[{status}] {content}");
                            }
                        }
                    }
                    Ok(_) => {}
                    Err(e) => eprintln!("\n[event error: {e}]"),
                }
            }
            _ = tokio::signal::ctrl_c() => {
                ctrl_c_count += 1;
                if ctrl_c_count >= 2 {
                    return Ok(true);
                }
            }
        }
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = env::args().skip(1).collect();
    let mut project = PathBuf::from(".");
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--project" | "-p" => {
                i += 1;
                if i < args.len() {
                    project = PathBuf::from(&args[i]);
                }
            }
            other => {
                eprintln!("unknown arg: {other}");
                std::process::exit(2);
            }
        }
        i += 1;
    }
    let project = project.canonicalize().unwrap_or(project);

    let cfg = ClientConfig::default_config(&project);
    let project_root = cfg.project_root.clone();
    let bin_path = cfg.bin_path.clone();
    eprintln!("[agent-cli] engine: {bin_path}");
    eprintln!("[agent-cli] project: {project_root}");

    let (client, events_rx, _reader_task) = Client::spawn(cfg).await?;
    let info = client.initialize().await?;
    eprintln!(
        "[agent-cli] connected to {} v{} (protocol {})",
        info.agent_info.title, info.agent_info.version, info.protocol_version
    );

    let sess = client.session_new(&project.display().to_string()).await?;
    eprintln!("[agent-cli] session: {}", sess.session_id);

    eprintln!("[agent-cli] type your message, /quit to exit");
    let stdin = tokio::io::BufReader::new(tokio::io::stdin());
    use tokio::io::AsyncBufReadExt;
    let mut lines = stdin.lines();

    loop {
        let line = lines.next_line().await?.unwrap_or_default();
        let text = line.trim().to_string();
        if text.is_empty() {
            if line.is_empty() {
                break;
            }
            continue;
        }
        if text == "/quit" || text == "/exit" {
            break;
        }
        let cancelled = run_turn(&client, events_rx.resubscribe(), &sess.session_id, &text, &project_root).await?;
        if cancelled {
            let _ = client.session_cancel(&sess.session_id).await;
            eprintln!("[agent-cli] turn cancelled");
        }
    }

    let _ = client.session_close(&sess.session_id).await;
    eprintln!("[agent-cli] session closed");
    Ok(())
}

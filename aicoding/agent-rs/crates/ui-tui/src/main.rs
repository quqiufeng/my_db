mod app;
mod event;
mod input;
mod render;

use std::env;
use std::path::PathBuf;

use agent_acp::ClientConfig;

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
    let project_str = project.display().to_string();

    let cfg = ClientConfig::default_config(&project);
    eprintln!("[agent-tui] engine: {}", cfg.bin_path);
    eprintln!("[agent-tui] project: {project_str}");

    app::run(cfg, project_str).await
}

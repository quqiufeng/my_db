use crossterm::event::{Event, KeyCode, KeyEvent, KeyModifiers};

use tokio::sync::mpsc;

pub enum UiEvent {
    Key(KeyEvent),
    Resize(u16, u16),
    Error(String),
}

pub fn spawn_input_thread(tx: mpsc::Sender<UiEvent>) {
    std::thread::spawn(move || loop {
        match crossterm::event::read() {
            Ok(Event::Key(k)) => {
                if tx.blocking_send(UiEvent::Key(k)).is_err() {
                    break;
                }
            }
            Ok(Event::Resize(w, h)) => {
                if tx.blocking_send(UiEvent::Resize(w, h)).is_err() {
                    break;
                }
            }
            Ok(_) => {}
            Err(e) => {
                eprintln!("[input] error: {e}");
                let _ = tx.blocking_send(UiEvent::Error(e.to_string()));
                break;
            }
        }
    });
}

pub fn is_enter(k: &KeyEvent) -> bool {
    k.code == KeyCode::Enter && k.modifiers.is_empty()
}

pub fn is_soft_enter(k: &KeyEvent) -> bool {
    (k.code == KeyCode::Enter
        && (k.modifiers.contains(KeyModifiers::SHIFT)
            || k.modifiers.contains(KeyModifiers::CONTROL)
            || k.modifiers.contains(KeyModifiers::ALT)))
        || (k.code == KeyCode::Char('j') && k.modifiers.contains(KeyModifiers::CONTROL))
}

pub fn is_quit(k: &KeyEvent) -> bool {
    k.code == KeyCode::Char('c') && k.modifiers.contains(KeyModifiers::CONTROL)
        || k.code == KeyCode::Char('d') && k.modifiers.contains(KeyModifiers::CONTROL)
}

pub fn is_interrupt(k: &KeyEvent) -> bool {
    k.code == KeyCode::Esc
}

/// Interpret a key for the permission dialog.
/// Returns None if the key is not a permission key;
/// Some(None) means "dismiss without answering" (Esc);
/// Some(Some(result)) answers the request.
pub fn permission_choice(k: &KeyEvent) -> Option<Option<serde_json::Value>> {
    use serde_json::json;
    let c = match k.code {
        KeyCode::Char(c) if k.modifiers.is_empty() || k.modifiers.contains(KeyModifiers::SHIFT) => c,
        KeyCode::Esc => return Some(None),
        _ => return None,
    };
    match c.to_ascii_lowercase() {
        'y' => Some(Some(json!({ "allow": true }))),
        'n' => Some(Some(json!({ "allow": false }))),
        'a' => Some(Some(json!({ "always": true }))),
        _ => None,
    }
}

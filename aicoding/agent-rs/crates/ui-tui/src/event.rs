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

#[cfg(test)]
mod tests {
    use super::*;

    fn key(code: KeyCode, modifiers: KeyModifiers) -> KeyEvent {
        KeyEvent::new(code, modifiers)
    }

    #[test]
    fn enter_detection() {
        assert!(is_enter(&key(KeyCode::Enter, KeyModifiers::NONE)));
        // shift+enter is a soft enter, not a send
        assert!(!is_enter(&key(KeyCode::Enter, KeyModifiers::SHIFT)));
        assert!(is_soft_enter(&key(KeyCode::Enter, KeyModifiers::SHIFT)));
        assert!(is_soft_enter(&key(KeyCode::Enter, KeyModifiers::CONTROL)));
        assert!(is_soft_enter(&key(KeyCode::Enter, KeyModifiers::ALT)));
        assert!(is_soft_enter(&key(KeyCode::Char('j'), KeyModifiers::CONTROL)));
        assert!(!is_soft_enter(&key(KeyCode::Char('j'), KeyModifiers::NONE)));
    }

    #[test]
    fn quit_detection() {
        assert!(is_quit(&key(KeyCode::Char('c'), KeyModifiers::CONTROL)));
        assert!(is_quit(&key(KeyCode::Char('d'), KeyModifiers::CONTROL)));
        assert!(!is_quit(&key(KeyCode::Char('c'), KeyModifiers::NONE)));
        assert!(!is_quit(&key(KeyCode::Char('q'), KeyModifiers::CONTROL)));
    }

    #[test]
    fn interrupt_is_esc_only() {
        assert!(is_interrupt(&key(KeyCode::Esc, KeyModifiers::NONE)));
        assert!(!is_interrupt(&key(KeyCode::Char('c'), KeyModifiers::CONTROL)));
    }

    #[test]
    fn permission_keys() {
        use serde_json::json;
        assert_eq!(
            permission_choice(&key(KeyCode::Char('y'), KeyModifiers::NONE)),
            Some(Some(json!({ "allow": true })))
        );
        assert_eq!(
            permission_choice(&key(KeyCode::Char('N'), KeyModifiers::SHIFT)),
            Some(Some(json!({ "allow": false })))
        );
        assert_eq!(
            permission_choice(&key(KeyCode::Char('a'), KeyModifiers::NONE)),
            Some(Some(json!({ "always": true })))
        );
        assert_eq!(
            permission_choice(&key(KeyCode::Esc, KeyModifiers::NONE)),
            Some(None)
        );
        assert_eq!(permission_choice(&key(KeyCode::Enter, KeyModifiers::NONE)), None);
        assert_eq!(permission_choice(&key(KeyCode::Char('x'), KeyModifiers::NONE)), None);
    }
}

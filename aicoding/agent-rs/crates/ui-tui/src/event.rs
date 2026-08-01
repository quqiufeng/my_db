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

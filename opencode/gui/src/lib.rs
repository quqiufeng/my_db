use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::sync::{Arc, Mutex};

use eframe::egui;
use serde_json::Value;

/// Opaque handle to the GUI runtime.
pub struct GuiApp {
    /// User message callback: fn(session_id, text, userdata)
    on_user_message: Option<extern "C" fn(*const c_char, *const c_char, *mut c_void)>,
    user_data: *mut c_void,

    /// Tool call callback: fn(session_id, tool_json, userdata) -> *char result_json
    on_tool_call: Option<extern "C" fn(*const c_char, *const c_char, *mut c_void) -> *mut c_char>,

    /// Chat messages per session.
    messages: Arc<Mutex<Vec<MessageRow>>>,

    /// Current input text buffer.
    input: Arc<Mutex<String>>,

    /// Window title.
    title: String,
}

#[derive(Clone)]
struct MessageRow {
    role: String, // "user" | "assistant" | "tool"
    text: String,
    tool_id: Option<String>,
}

/// Create the GUI app handle. Does not open window yet.
#[no_mangle]
pub extern "C" fn gui_app_create(config_json: *const c_char) -> *mut c_void {
    if config_json.is_null() {
        return std::ptr::null_mut();
    }
    let config_str = unsafe { CStr::from_ptr(config_json).to_string_lossy() };
    let title = match serde_json::from_str::<Value>(&config_str) {
        Ok(Value::Object(m)) => m
            .get("title")
            .and_then(|v| v.as_str())
            .unwrap_or("opencode")
            .to_string(),
        _ => "opencode".to_string(),
    };

    let app = GuiApp {
        on_user_message: None,
        user_data: std::ptr::null_mut(),
        on_tool_call: None,
        messages: Arc::new(Mutex::new(Vec::new())),
        input: Arc::new(Mutex::new(String::new())),
        title,
    };

    Box::into_raw(Box::new(app)) as *mut c_void
}

/// Free the app handle.
#[no_mangle]
pub extern "C" fn gui_app_free(app: *mut c_void) {
    if !app.is_null() {
        unsafe { drop(Box::from_raw(app as *mut GuiApp)) };
    }
}

/// Register callback for user messages.
#[no_mangle]
pub extern "C" fn gui_on_user_message(
    app: *mut c_void,
    callback: extern "C" fn(*const c_char, *const c_char, *mut c_void),
    userdata: *mut c_void,
) {
    if app.is_null() {
        return;
    }
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };
    app.on_user_message = Some(callback);
    app.user_data = userdata;
}

/// Register callback for tool calls.
#[no_mangle]
pub extern "C" fn gui_on_tool_call(
    app: *mut c_void,
    callback: extern "C" fn(*const c_char, *const c_char, *mut c_void) -> *mut c_char,
    userdata: *mut c_void,
) {
    if app.is_null() {
        return;
    }
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };
    app.on_tool_call = Some(callback);
    app.user_data = userdata;
}

/// Append a stream delta to the current assistant message.
#[no_mangle]
pub extern "C" fn gui_stream_delta(
    app: *mut c_void,
    _session_id: *const c_char,
    delta: *const c_char,
) {
    if app.is_null() || delta.is_null() {
        return;
    }
    let delta_str = unsafe { CStr::from_ptr(delta).to_string_lossy().to_string() };
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };
    let mut msgs = app.messages.lock().unwrap();
    if let Some(last) = msgs.last_mut() {
        if last.role == "assistant" {
            last.text.push_str(&delta_str);
            return;
        }
    }
    msgs.push(MessageRow {
        role: "assistant".to_string(),
        text: delta_str,
        tool_id: None,
    });
}

/// Append a complete message.
#[no_mangle]
pub extern "C" fn gui_append_message(
    app: *mut c_void,
    _session_id: *const c_char,
    role: *const c_char,
    text: *const c_char,
) {
    if app.is_null() || role.is_null() || text.is_null() {
        return;
    }
    let role_str = unsafe { CStr::from_ptr(role).to_string_lossy().to_string() };
    let text_str = unsafe { CStr::from_ptr(text).to_string_lossy().to_string() };
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };
    app.messages.lock().unwrap().push(MessageRow {
        role: role_str,
        text: text_str,
        tool_id: None,
    });
}

/// Append tool output as a message.
#[no_mangle]
pub extern "C" fn gui_tool_output(
    app: *mut c_void,
    _session_id: *const c_char,
    tool_id: *const c_char,
    output: *const c_char,
) {
    if app.is_null() || tool_id.is_null() || output.is_null() {
        return;
    }
    let tool_id_str = unsafe { CStr::from_ptr(tool_id).to_string_lossy().to_string() };
    let output_str = unsafe { CStr::from_ptr(output).to_string_lossy().to_string() };
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };
    app.messages.lock().unwrap().push(MessageRow {
        role: "tool".to_string(),
        text: format!("[{}] {}", tool_id_str, output_str),
        tool_id: Some(tool_id_str),
    });
}

/// Run the GUI event loop. Blocks until window closed.
#[no_mangle]
pub extern "C" fn gui_run(app: *mut c_void) -> c_int {
    if app.is_null() {
        return -1;
    }
    let app: &mut GuiApp = unsafe { &mut *(app as *mut GuiApp) };

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([900.0, 600.0]),
        ..Default::default()
    };

    let messages = app.messages.clone();
    let input = app.input.clone();
    let callback = app.on_user_message;
    let userdata = app.user_data;
    let title = app.title.clone();

    let result = eframe::run_native(
        &title,
        options,
        Box::new(|_cc| Ok(Box::new(ChatApp {
            messages,
            input,
            callback,
            userdata,
        }))),
    );

    match result {
        Ok(_) => 0,
        Err(_) => -1,
    }
}

struct ChatApp {
    messages: Arc<Mutex<Vec<MessageRow>>>,
    input: Arc<Mutex<String>>,
    callback: Option<extern "C" fn(*const c_char, *const c_char, *mut c_void)>,
    userdata: *mut c_void,
}

impl eframe::App for ChatApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        egui::CentralPanel::default().show(ctx, |ui| {
            ui.heading("opencode GUI");

            // Message list
            egui::ScrollArea::vertical()
                .auto_shrink([false; 2])
                .stick_to_bottom(true)
                .show(ui, |ui| {
                    let msgs = self.messages.lock().unwrap().clone();
                    for m in msgs {
                        let (label, color) = match m.role.as_str() {
                            "user" => ("User", egui::Color32::BLUE),
                            "assistant" => ("Assistant", egui::Color32::GRAY),
                            "tool" => ("Tool", egui::Color32::GREEN),
                            _ => ("Unknown", egui::Color32::WHITE),
                        };
                        ui.horizontal(|ui| {
                            ui.colored_label(color, format!("[{}]", label));
                            ui.label(m.text);
                        });
                    }
                });

            // Input area
            ui.separator();
            ui.horizontal(|ui| {
                let mut text = self.input.lock().unwrap().clone();
                let response = ui.text_edit_multiline(&mut text);
                *self.input.lock().unwrap() = text;

                let send_clicked = ui.button("Send").clicked();
                let enter_pressed = response.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter) && i.modifiers.shift);

                if send_clicked || enter_pressed {
                    let text_to_send = {
                        let mut i = self.input.lock().unwrap();
                        let t = i.clone();
                        i.clear();
                        t
                    };
                    if !text_to_send.is_empty() {
                        {
                            self.messages.lock().unwrap().push(MessageRow {
                                role: "user".to_string(),
                                text: text_to_send.clone(),
                                tool_id: None,
                            });
                        }
                        if let Some(cb) = self.callback {
                            let s = CString::new("default").unwrap();
                            let t = CString::new(text_to_send).unwrap();
                            unsafe { cb(s.as_ptr(), t.as_ptr(), self.userdata) };
                        }
                    }
                }
            });
        });
    }
}

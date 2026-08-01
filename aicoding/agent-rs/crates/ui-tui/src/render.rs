use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, Paragraph};
use ratatui::Frame;

use crate::app::{App, UiMsg};

pub fn draw(frame: &mut Frame, app: &App) {
    let area = frame.area();
    let input_h = (app.input.line_count() as u16).clamp(1, 5) + 2;
    let chunks = Layout::vertical([
        Constraint::Min(0),
        Constraint::Length(input_h),
        Constraint::Length(1),
    ])
    .split(area);

    draw_messages(frame, app, chunks[0]);
    draw_input(frame, app, chunks[1]);
    draw_status(frame, app, chunks[2]);
}

fn draw_messages(frame: &mut Frame, app: &App, area: Rect) {
    let lines = build_lines(app);
    let content_height = area.height.saturating_sub(2);
    let total = lines.len() as u16;
    let scroll = total.saturating_sub(content_height);
    let paragraph = Paragraph::new(lines)
        .block(
            Block::default()
                .borders(Borders::ALL)
                .border_type(BorderType::Rounded)
                .title(Span::styled(
                    format!(" {} ", app.project),
                    Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD),
                ))
                .title_bottom(Span::styled(
                    format!(" {} messages ", app.messages.len()),
                    Style::default().fg(Color::DarkGray),
                )),
        )
        .scroll((scroll, 0));
    frame.render_widget(paragraph, area);
}

fn build_lines(app: &App) -> Vec<Line<'_>> {
    let mut lines: Vec<Line<'_>> = Vec::new();
    for msg in &app.messages {
        match msg {
            UiMsg::User(text) => {
                lines.push(Line::from(Span::styled(
                    "┌ You",
                    Style::default().fg(Color::Green).add_modifier(Modifier::BOLD),
                )));
                for l in text.lines() {
                    lines.push(Line::from(Span::styled(
                        format!("│ {l}"),
                        Style::default().fg(Color::Green),
                    )));
                }
                lines.push(Line::from(Span::styled(
                    "└",
                    Style::default().fg(Color::Green),
                )));
                lines.push(Line::default());
            }
            UiMsg::Assistant(text) => {
                lines.push(Line::from(Span::styled(
                    "┌ Assistant",
                    Style::default().fg(Color::Blue).add_modifier(Modifier::BOLD),
                )));
                for l in text.lines() {
                    lines.push(Line::from(Span::styled(
                        format!("│ {l}"),
                        Style::default().fg(Color::Blue),
                    )));
                }
                lines.push(Line::from(Span::styled(
                    "└",
                    Style::default().fg(Color::Blue),
                )));
                lines.push(Line::default());
            }
            UiMsg::ToolStart { id, title, kind, .. } => {
                lines.push(Line::from(vec![
                    Span::styled(
                        format!("▸ {title} ({kind})"),
                        Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD),
                    ),
                    Span::styled(
                        format!("  [{id}] "),
                        Style::default().fg(Color::DarkGray),
                    ),
                    Span::styled("·", Style::default().fg(Color::Yellow)),
                ]));
            }
            UiMsg::ToolDone { id, status, .. } => {
                lines.push(Line::from(vec![
                    Span::styled("✓", Style::default().fg(Color::Green)),
                    Span::styled(
                        format!("  [{id}] "),
                        Style::default().fg(Color::DarkGray),
                    ),
                    Span::styled(status, Style::default().fg(Color::Green)),
                ]));
                lines.push(Line::default());
            }
            UiMsg::Info(text) => {
                lines.push(Line::from(Span::styled(
                    format!("ℹ {text}"),
                    Style::default().fg(Color::DarkGray),
                )));
                lines.push(Line::default());
            }
            UiMsg::Error(text) => {
                lines.push(Line::from(Span::styled(
                    format!("✖ {text}"),
                    Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
                )));
                lines.push(Line::default());
            }
        }
    }
    if lines.is_empty() {
        lines.push(Line::from(Span::styled(
            " (no messages yet)",
            Style::default().fg(Color::DarkGray),
        )));
    }
    lines
}

fn draw_input(frame: &mut Frame, app: &App, area: Rect) {
    let style = if app.busy {
        Style::default().fg(Color::DarkGray)
    } else {
        Style::default().fg(Color::White)
    };
    let title = if app.busy {
        if app.esc_armed {
            " input · esc again to interrupt ".to_string()
        } else {
            " input · agent running (esc to interrupt) ".to_string()
        }
    } else {
        format!(" input · {} ", app.model)
    };
    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(style)
        .title(Span::styled(
            title,
            Style::default().fg(if app.busy { Color::Yellow } else { Color::Cyan }),
        ));
    let input = Paragraph::new(app.input.buf.as_str())
        .block(block)
        .style(style);
    frame.render_widget(input, area);
    if !app.busy {
        let x = area.x + 1 + app.input.cursor as u16;
        let y = area.y + 1;
        frame.set_cursor_position((x, y));
    }
}

fn draw_status(frame: &mut Frame, app: &App, area: Rect) {
    let left = if app.busy {
        Span::styled(
            "● running",
            Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD),
        )
    } else {
        Span::styled("○ idle", Style::default().fg(Color::Green))
    };
    let right = Span::styled(
        format!(
            "{} · {} · enter send · esc interrupt · /help · ctrl+c quit",
            app.model,
            app.messages.len()
        ),
        Style::default().fg(Color::DarkGray),
    );
    let line = Line::from(vec![left, Span::raw("  "), right]);
    frame.render_widget(Paragraph::new(line), area);
}

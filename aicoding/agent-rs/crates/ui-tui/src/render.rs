use ratatui::layout::{Constraint, Layout, Margin, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, Paragraph};
use ratatui::Frame;

use crate::app::App;

pub fn draw(frame: &mut Frame, app: &App) {
    let area = frame.area();
    let input_h = (app.input.line_count() as u16).clamp(1, 5) + 2;
    let chunks = Layout::vertical([
        Constraint::Min(0),
        Constraint::Length(input_h),
        Constraint::Length(1),
    ])
    .split(area);

    let mut msgs_area = chunks[0];
    if !app.todos.is_empty() {
        let row = Layout::horizontal([
            Constraint::Min(20),
            Constraint::Length(36.min(area.width.saturating_div(3))),
        ])
        .split(msgs_area);
        msgs_area = row[0];
        draw_todos(frame, app, row[1]);
    }

    draw_messages(frame, app, msgs_area);
    draw_input(frame, app, chunks[1]);
    draw_status(frame, app, chunks[2]);
    if let Some(p) = &app.pending_permission {
        draw_permission(frame, app, p);
    }
    if let Some(q) = &app.pending_question {
        draw_question(frame, app, q);
    }
}

fn draw_todos(frame: &mut Frame, app: &App, area: Rect) {
    let mut lines = Vec::new();
    for t in &app.todos {
        let (mark, color) = match t.status.as_str() {
            "completed" => ("✓", Color::Green),
            "cancelled" => ("✗", Color::Red),
            "in_progress" => ("◐", Color::Yellow),
            _ => ("·", Color::Cyan),
        };
        lines.push(Line::from(vec![
            Span::styled(format!("{mark} "), Style::default().fg(color).add_modifier(Modifier::BOLD)),
            Span::styled(&t.content, Style::default().fg(Color::Gray)),
        ]));
    }
    let block = Block::default()
        .title(" todo ")
        .title_style(Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Cyan));
    let inner = area.inner(Margin { horizontal: 1, vertical: 0 });
    let _ = app;
    if area.width < 8 || area.height < 2 {
        return;
    }
    frame.render_widget(block, area);
    let text = ratatui::text::Text::from(lines);
    frame.render_widget(Paragraph::new(text).wrap(ratatui::widgets::Wrap { trim: false }), inner);
}

fn draw_permission(frame: &mut Frame, app: &App, p: &crate::app::PendingPermission) {
    let width = 64.min(frame.area().width.saturating_sub(4));
    let lines = vec![
        Line::from(vec![
            Span::styled(" Permission request ", Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)),
        ]),
        Line::from(Span::styled(
            format!("{} '{}'", p.action, p.resource),
            Style::default().fg(Color::White),
        )),
        Line::from(Span::raw("")),
        Line::from(Span::styled(
            "[y] allow   [n] deny   [a] always   [esc] cancel",
            Style::default().fg(Color::Cyan),
        )),
    ];
    let text = ratatui::text::Text::from(lines);
    let inner = Paragraph::new(text)
        .block(
            Block::default()
                .borders(Borders::ALL)
                .border_style(Style::default().fg(Color::Yellow))
                .border_type(BorderType::Rounded),
        )
        .wrap(ratatui::widgets::Wrap { trim: false });
    let h = 8.min(frame.area().height.saturating_sub(2));
    let area = ratatui::layout::Rect {
        x: (frame.area().width.saturating_sub(width)) / 2,
        y: (frame.area().height.saturating_sub(h)) / 2,
        width,
        height: h,
    };
    frame.render_widget(ratatui::widgets::Clear, area);
    frame.render_widget(inner, area);
    let _ = app;
}

fn draw_question(frame: &mut Frame, app: &App, q: &crate::app::PendingQuestion) {
    let width = 72.min(frame.area().width.saturating_sub(4));
    let mut lines = vec![Line::from(vec![
        Span::styled(
            " Question ",
            Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD),
        ),
    ])];
    lines.push(Line::from(Span::styled(
        &q.question,
        Style::default().fg(Color::White),
    )));
    lines.push(Line::from(Span::raw("")));
    if q.options.is_empty() {
        lines.push(Line::from(vec![
            Span::styled("Answer: ", Style::default().fg(Color::Cyan)),
            Span::styled(
                if q.input.is_empty() { " " } else { &q.input },
                Style::default().fg(Color::White),
            ),
        ]));
        lines.push(Line::from(Span::styled(
            "[enter] send   [esc] cancel",
            Style::default().fg(Color::Cyan),
        )));
    } else {
        for (i, o) in q.options.iter().enumerate() {
            lines.push(Line::from(vec![
                Span::styled(format!("  {} ", i + 1), Style::default().fg(Color::Yellow)),
                Span::styled(o, Style::default().fg(Color::White)),
            ]));
        }
        lines.push(Line::from(Span::raw("")));
        lines.push(Line::from(Span::styled(
            "press the number to choose   [esc] cancel",
            Style::default().fg(Color::Cyan),
        )));
    }
    let text = ratatui::text::Text::from(lines);
    let inner = Paragraph::new(text)
        .block(
            Block::default()
                .borders(Borders::ALL)
                .border_style(Style::default().fg(Color::Yellow))
                .border_type(BorderType::Rounded),
        )
        .wrap(ratatui::widgets::Wrap { trim: false });
    let h = (q.options.len() as u16 + 8).min(frame.area().height.saturating_sub(2));
    let area = ratatui::layout::Rect {
        x: (frame.area().width.saturating_sub(width)) / 2,
        y: (frame.area().height.saturating_sub(h)) / 2,
        width,
        height: h,
    };
    frame.render_widget(ratatui::widgets::Clear, area);
    frame.render_widget(inner, area);
    let _ = app;
}

fn draw_messages(frame: &mut Frame, app: &App, area: Rect) {
    let lines = app.layout_lines();
    let content_height = area.height.saturating_sub(2) as usize;

    let top = app.view_top(content_height);
    let mut visible: Vec<Line<'_>> = Vec::with_capacity(content_height);
    for (i, li) in lines.iter().enumerate().skip(top).take(content_height) {
        let line = if Some(i) == app.focus {
            li.line
                .clone()
                .patch_style(Style::default().add_modifier(Modifier::REVERSED))
        } else {
            li.line.clone()
        };
        visible.push(line);
    }
    let paragraph = Paragraph::new(visible)
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
        .scroll((0, 0));
    frame.render_widget(paragraph, area);
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
            "{} · {} msgs · enter send · esc interrupt · /help · ctrl+c quit",
            app.model,
            app.messages.len()
        ),
        Style::default().fg(Color::DarkGray),
    );
    let line = Line::from(vec![left, Span::raw("  "), right]);
    frame.render_widget(Paragraph::new(line), area);
}

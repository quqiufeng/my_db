// aicoding ggui — custom component library
//
// All components follow: #[derive(IntoElement)] + builder methods + RenderOnce.
// Colors use cx.theme() for automatic dark/light mode support.

use gpui::*;
use gpui::prelude::FluentBuilder as _;
use gpui_component::{
    ActiveTheme,
    button::Button,
    h_flex, v_flex,
};
use std::sync::OnceLock;

// ── Font stack ──────────────────────────────────────────────

pub fn ui_font_family() -> gpui::SharedString {
    gpui::SharedString::from(
        "Noto Sans CJK SC, Source Han Sans SC, WenQuanYi Micro Hei, \
         Microsoft YaHei, PingFang SC, Hiragino Sans GB, sans-serif",
    )
}

pub fn mono_font_family() -> gpui::SharedString {
    gpui::SharedString::from(
        "Noto Sans Mono CJK SC, Source Han Mono SC, WenQuanYi Micro Hei Mono, \
         Microsoft YaHei Mono, Zed Mono, monospace",
    )
}

// ── AccentCard (generic card with colored left border) ─────

#[derive(IntoElement)]
pub struct AccentCard {
    accent: Hsla,
    style: StyleRefinement,
    children: Vec<AnyElement>,
}

impl AccentCard {
    pub fn new(accent: Hsla) -> Self {
        Self { accent, style: StyleRefinement::default(), children: Vec::new() }
    }
    pub fn child(mut self, el: impl IntoElement) -> Self {
        self.children.push(el.into_any_element());
        self
    }
}

impl RenderOnce for AccentCard {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        h_flex()
            .rounded_md()
            .overflow_hidden()
            .border_1()
            .border_color(theme.border)
            // Colored left accent bar
            .child(div().w(px(4.0)).h_full().bg(self.accent).flex_none())
            .child(
                div()
                    .flex_1()
                    .bg(gpui::rgb(0x252526))
                    .p_2()
                    .children(self.children)
            )
    }
}

// ── ModelBadge ─────────────────────────────────────────────

#[derive(IntoElement)]
pub struct ModelBadge {
    name: SharedString,
    style: StyleRefinement,
    children: Vec<AnyElement>,
}

impl ModelBadge {
    pub fn new(name: impl Into<SharedString>) -> Self {
        Self { name: name.into(), style: StyleRefinement::default(), children: Vec::new() }
    }
}

impl RenderOnce for ModelBadge {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        h_flex()
            .px_2()
            .py(px(3.0))
            .rounded_sm()
            .bg(gpui::rgb(0x2a2a3e))
            .gap_1()
            .child(div().child("AI").text_color(theme.primary).text_xs().font_weight(FontWeight::BOLD).font_family("monospace"))
            .child(div().child(self.name.clone()).text_color(theme.muted_foreground).text_xs())
    }
}

// ── AppLogo ────────────────────────────────────────────────

#[derive(IntoElement)]
pub struct AppLogo {
    style: StyleRefinement,
    children: Vec<AnyElement>,
}

impl AppLogo {
    pub fn new() -> Self {
        Self { style: StyleRefinement::default(), children: Vec::new() }
    }
}

impl RenderOnce for AppLogo {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        h_flex()
            .gap_2()
            .items_center()
            .child(
                div()
                    .w(px(30.0)).h(px(30.0))
                    .rounded_full()
                    .bg(gpui::rgb(0x2a2a3e))
                    .flex()
                    .items_center()
                    .justify_center()
                    .child(div().child("😊").text_base())
            )
            .child(
                div()
                    .child("aicoding")
                    .text_color(theme.foreground)
                    .text_base()
                    .font_weight(FontWeight::SEMIBOLD)
                    .font_family(ui_font_family())
            )
    }
}

// ── StatusBar ───────────────────────────────────────────────

#[derive(IntoElement)]
pub struct StatusBar {
    model: SharedString,
    version: SharedString,
    project: SharedString,
    tokens_used: usize,
    context_tokens: usize,
    prompt_tokens: usize,
    completion_tokens: usize,
    style: StyleRefinement,
    children: Vec<AnyElement>,
}

impl StatusBar {
    pub fn new(model: impl Into<SharedString>, version: impl Into<SharedString>) -> Self {
        Self {
            model: model.into(),
            version: version.into(),
            project: SharedString::default(),
            tokens_used: 0, context_tokens: 0, prompt_tokens: 0, completion_tokens: 0,
            style: StyleRefinement::default(),
            children: Vec::new(),
        }
    }
    pub fn project(mut self, p: impl Into<SharedString>) -> Self { self.project = p.into(); self }
    pub fn tokens(mut self, used: usize, total: usize, prompt: usize, completion: usize) -> Self {
        self.tokens_used = used; self.context_tokens = total;
        self.prompt_tokens = prompt; self.completion_tokens = completion; self
    }
}

impl RenderOnce for StatusBar {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        let pct = if self.context_tokens > 0 {
            (self.tokens_used as f64 / self.context_tokens as f64 * 100.0) as usize
        } else { 0 };

        h_flex()
            .justify_between()
            .px_3()
            .py(px(6.0))
            .text_sm()
            .border_t_1()
            .border_color(theme.border)
            .bg(gpui::rgb(0x1a1a1a))
            .text_color(theme.muted_foreground)
            // Left: model badge
            .child(ModelBadge::new(self.model.clone()))
            // Center: tokens
            .child(
                div()
                    .child(format!("{} / {} ({}%)", self.tokens_used, self.context_tokens, pct))
            )
            // Right: project name
            .child(
                div()
                    .child(self.project.clone())
                    .text_color(theme.muted_foreground)
            )
    }
}

// ── ThinkingBlock ──────────────────────────────────────────

#[derive(IntoElement)]
pub struct ThinkingBlock {
    text: SharedString,
    expanded: bool,
    style: StyleRefinement,
    children: Vec<AnyElement>,
}

impl ThinkingBlock {
    pub fn new(text: impl Into<SharedString>) -> Self {
        Self {
            text: text.into(),
            expanded: false,
            style: StyleRefinement::default(),
            children: Vec::new(),
        }
    }
    pub fn expanded(mut self, v: bool) -> Self { self.expanded = v; self }
}

impl RenderOnce for ThinkingBlock {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        v_flex()
            .rounded_md()
            .overflow_hidden()
            .border_1()
            .border_color(theme.border)
            .child(
                h_flex()
                    .px_3()
                    .py(px(6.0))
                    .bg(gpui::rgb(0x1e1e28))
                    .gap_2()
                    .child(div().child("🧠").text_sm())
                    .child(div().child("Thinking...").text_color(theme.muted_foreground).text_sm().italic())
            )
            .when(self.expanded, |this| {
                this.child(
                    div()
                        .p_3()
                        .bg(gpui::rgb(0x0d0d14))
                        .child(self.text.clone())
                        .text_color(gpui::rgb(0x999999))
                        .font_family(mono_font_family())
                        .text_sm()
                )
            })
    }
}

// ── TokenProgress ───────────────────────────────────────────

#[derive(IntoElement)]
pub struct TokenProgress {
    used: usize,
    total: usize,
    style: StyleRefinement,
    children: Vec<AnyElement>,
}

impl TokenProgress {
    pub fn new(used: usize, total: usize) -> Self {
        Self { used, total, style: StyleRefinement::default(), children: Vec::new() }
    }
}

impl RenderOnce for TokenProgress {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let pct = if self.total > 0 { (self.used as f64 / self.total as f64 * 100.0) as usize } else { 0 };
        let bar_color = if pct > 90 { cx.theme().danger }
            else if pct > 70 { cx.theme().warning }
            else { cx.theme().success };

        div()
            .w_full()
            .h(px(8.0))
            .rounded_md()
            .bg(cx.theme().border)
            .overflow_hidden()
            .child(
                div()
                    .w(px((pct as f32 * 2.4).min(240.0)))
                    .h_full()
                    .rounded_md()
                    .bg(bar_color)
            )
    }
}

// ── InfoSection ─────────────────────────────────────────────

#[derive(IntoElement)]
pub struct InfoSection {
    title: SharedString,
    children: Vec<AnyElement>,
    style: StyleRefinement,
}

impl InfoSection {
    pub fn new(title: impl Into<SharedString>) -> Self {
        Self { title: title.into(), children: Vec::new(), style: StyleRefinement::default() }
    }
    pub fn child(mut self, el: impl IntoElement) -> Self {
        self.children.push(el.into_any_element());
        self
    }
    pub fn children(mut self, els: Vec<AnyElement>) -> Self {
        for el in els { self.children.push(el); }
        self
    }
}

impl RenderOnce for InfoSection {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        v_flex()
            .gap(px(4.0))
            .p_3()
            .border_b_1()
            .border_color(theme.border)
            .child(
                div()
                    .text_xs()
                    .font_weight(FontWeight::BOLD)
                    .text_color(theme.muted_foreground)
                    .child(self.title.clone().to_uppercase())
            )
            .children(self.children)
    }
}

// ── CodeBlock (syntax highlighted) ─────────────────────────

use syntect::easy::HighlightLines;
use syntect::highlighting::{Style, ThemeSet};
use syntect::parsing::SyntaxSet;
use syntect::util::LinesWithEndings;

fn syntax_set() -> &'static SyntaxSet {
    static SET: OnceLock<SyntaxSet> = OnceLock::new();
    SET.get_or_init(SyntaxSet::load_defaults_newlines)
}
fn theme_set() -> &'static ThemeSet {
    static SET: OnceLock<ThemeSet> = OnceLock::new();
    SET.get_or_init(ThemeSet::load_defaults)
}
fn style_to_rgb(style: &Style) -> gpui::Rgba {
    let c = style.foreground;
    gpui::rgb((c.r as u32) << 16 | (c.g as u32) << 8 | c.b as u32)
}

pub fn highlighted_code(code: &str, lang: &str) -> Vec<AnyElement> {
    let ps = syntax_set();
    let ts = theme_set();
    let syntax = ps.find_syntax_by_extension(lang)
        .or_else(|| ps.find_syntax_by_name(lang))
        .unwrap_or_else(|| ps.find_syntax_plain_text());
    let theme = ts.themes.get("base16-ocean.dark")
        .or_else(|| ts.themes.values().next())
        .expect("no themes loaded");
    let mut h = HighlightLines::new(syntax, theme);
    let mut lines: Vec<AnyElement> = Vec::new();
    for line in LinesWithEndings::from(code) {
        let ranges = h.highlight_line(line, ps).unwrap_or_default();
        let tokens: Vec<AnyElement> = ranges.into_iter().filter_map(|(style, text)| {
            let t = text.trim_end_matches('\n').trim_end_matches('\r');
            if t.is_empty() { return None; }
            Some(div().child(t.to_string()).text_color(style_to_rgb(&style)).text_base().font_family(mono_font_family()).into_any_element())
        }).collect();
        lines.push(h_flex().children(tokens).into_any_element());
    }
    lines
}

#[derive(IntoElement)]
pub struct CodeBlock {
    lang: SharedString,
    code: SharedString,
    style: StyleRefinement,
    children: Vec<AnyElement>,
}

impl CodeBlock {
    pub fn new(lang: impl Into<SharedString>, code: impl Into<SharedString>) -> Self {
        Self {
            lang: lang.into(),
            code: code.into(),
            style: StyleRefinement::default(),
            children: Vec::new(),
        }
    }
}

impl RenderOnce for CodeBlock {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        v_flex()
            .rounded_md()
            .overflow_hidden()
            .border_1()
            .border_color(theme.border)
            .child(
                h_flex()
                    .justify_between()
                    .px_3()
                    .py(px(4.0))
                    .bg(gpui::rgb(0x2d2d2d))
                    .child(
                        div()
                            .child(if self.lang.is_empty() { SharedString::from("code") } else { self.lang.clone() })
                            .text_xs()
                            .text_color(gpui::rgb(0x999999))
                            .font_family(ui_font_family())
                    )
                    .child(
                        div()
                            .child("📋")
                            .text_xs()
                            .text_color(gpui::rgb(0x666666))
                    )
            )
            .child(
                div()
                    .p_3()
                    .bg(gpui::rgb(0x1e1e1e))
                    .font_family(mono_font_family())
                    .children(highlighted_code(&self.code, &self.lang))
            )
    }
}

// ── ToolOutputBlock ─────────────────────────────────────────

#[derive(IntoElement)]
pub struct ToolOutputBlock {
    header: SharedString,
    body: SharedString,
    expanded: bool,
    style: StyleRefinement,
    children: Vec<AnyElement>,
}

impl ToolOutputBlock {
    pub fn new(header: impl Into<SharedString>, body: impl Into<SharedString>) -> Self {
        Self {
            header: header.into(),
            body: body.into(),
            expanded: false,
            style: StyleRefinement::default(),
            children: Vec::new(),
        }
    }
    pub fn expanded(mut self, v: bool) -> Self { self.expanded = v; self }
}

impl RenderOnce for ToolOutputBlock {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        v_flex()
            .rounded_md()
            .overflow_hidden()
            .border_1()
            .border_color(theme.border)
            .child(
                h_flex()
                    .px_3()
                    .py(px(6.0))
                    .bg(gpui::rgb(0x222218))
                    .gap_2()
                    .child(div().child("⚙️").text_sm())
                    .child(div().child(self.header.clone()).text_color(theme.warning).text_sm().font_family(mono_font_family()))
            )
            .when(self.expanded, |this| {
                this.child(
                    div()
                        .p_3()
                        .bg(gpui::rgb(0x0f0f0f))
                        .child(self.body.clone())
                        .text_color(gpui::rgb(0xcccccc))
                        .font_family(mono_font_family())
                        .text_sm()
                )
            })
    }
}

use gpui::*;
use gpui::prelude::FluentBuilder as _;
use gpui_component::{
    ActiveTheme,
    h_flex, v_flex,
};

// ── Font stack ──────────────────────────────────────────────

pub fn ui_font_family() -> gpui::SharedString {
    gpui::SharedString::from(
        "Noto Sans CJK SC, Source Han Sans SC, WenQuanYi Micro Hei, Microsoft YaHei, \
         PingFang SC, Hiragino Sans GB, sans-serif",
    )
}

pub fn mono_font_family() -> gpui::SharedString {
    gpui::SharedString::from(
        "Noto Sans Mono CJK SC, Source Han Mono SC, WenQuanYi Micro Hei Mono, \
         Microsoft YaHei Mono, Zed Mono, monospace",
    )
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
            tokens_used: 0,
            context_tokens: 0,
            prompt_tokens: 0,
            completion_tokens: 0,
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
        let token_percent = if self.context_tokens > 0 {
            (self.tokens_used as f64 / self.context_tokens as f64 * 100.0) as usize
        } else { 0 };

        h_flex()
            .justify_between()
            .p_1()
            .text_sm()
            .border_t_1()
            .border_color(cx.theme().border)
            .bg(cx.theme().background)
            .text_color(cx.theme().muted_foreground)
            .child(div().child(format!("{} · {}", self.model, self.version)))
            .child(div().child(format!("{} / {} tokens ({}%)  prompt {} + completion {}",
                self.tokens_used, self.context_tokens, token_percent,
                self.prompt_tokens, self.completion_tokens)))
            .child(div().child(format!("{}:main", self.project)))
    }
}

// ── ThinkingBlock (collapsible reasoning) ───────────────────

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
            .bg(theme.background)
            .rounded_md()
            .border_1()
            .border_color(theme.border)
            .child(
                h_flex()
                    .px_2()
                    .py_1()
                    .child(div().child("Thinking...").text_color(theme.muted_foreground).text_sm())
            )
            .when(self.expanded, |this| {
                this.child(
                    div()
                        .p_2()
                        .bg(gpui::rgb(0x0f0f0f))
                        .child(self.text.clone())
                        .text_color(theme.muted_foreground)
                        .font_family("monospace")
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
            .child(
                div()
                    .w(px((pct as f32 * 2.4).min(240.0)))
                    .h_full()
                    .rounded_md()
                    .bg(bar_color)
            )
    }
}

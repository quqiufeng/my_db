use gpui::prelude::FluentBuilder;
use gpui::{AppContext as _, div, size, px, App, Bounds, Context, InteractiveElement as _, IntoElement, ParentElement, Render, StatefulInteractiveElement as _, Styled, Window, WindowBounds, WindowOptions};
use gpui_component::{Root, v_flex};

struct DemoView;

impl Render for DemoView {
    fn render(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        v_flex()
            .size_full()
            .bg(gpui::rgb(0xff0000))
            .child(div().size_full().bg(gpui::rgb(0x00ff00)))
    }
}

fn main() {
    gpui_platform::application().run(|cx: &mut App| {
        gpui_component::init(cx);
        cx.activate(true);

        let bounds = Bounds::centered(None, size(px(600.0), px(400.0)), cx);
        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                #[cfg(target_os = "linux")]
                window_background: gpui::WindowBackgroundAppearance::Opaque,
                #[cfg(target_os = "linux")]
                window_decorations: Some(gpui::WindowDecorations::Client),
                ..Default::default()
            },
            |window, cx| {
                let view = cx.new(|_cx| DemoView);
                cx.new(|cx| Root::new(view, window, cx))
            },
        ).ok();
    });
}

#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

use std::fs;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};
use serde_json::json;
use winit::{
    application::ApplicationHandler,
    dpi::{LogicalPosition, LogicalSize},
    event::WindowEvent,
    event_loop::{ActiveEventLoop, EventLoop, EventLoopProxy},
    window::{Theme, Window, WindowId},
};
use wry::{PageLoadEvent, Rect, WebContext, WebView, WebViewBuilder};

const HOME_URL: &str = "avant://newtab";
const TOOLBAR_HEIGHT: f64 = 106.0;
const SETTINGS_HEIGHT: f64 = 218.0;
const MAX_TABS: usize = 24;

#[derive(Debug)]
enum Event {
    Command(String),
    ToolbarLoaded,
    Title { id: u64, title: String },
    Loading { id: u64, url: String, loading: bool },
    Popup { url: String, private: bool },
}

#[derive(Serialize, Deserialize, Default)]
struct Session {
    #[serde(default)]
    nickname: String,
    #[serde(default)]
    tabs: Vec<String>,
}

#[derive(Deserialize)]
struct Command {
    cmd: String,
    #[serde(default)]
    id: Option<u64>,
    #[serde(default)]
    value: Option<String>,
}

struct Tab {
    id: u64,
    title: String,
    url: String,
    private: bool,
    loading: bool,
    webview: WebView,
}

struct Avant {
    window: Option<Window>,
    toolbar: Option<WebView>,
    tabs: Vec<Tab>,
    active: Option<u64>,
    next_id: u64,
    settings_open: bool,
    toolbar_ready: bool,
    proxy: EventLoopProxy<Event>,
    context: WebContext,
    data_dir: PathBuf,
    nickname: String,
    restore_urls: Vec<String>,
}

fn app_dir() -> PathBuf {
    let base = std::env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir);
    base.join("Avant")
}

fn valid_page_url(url: &str) -> bool {
    url.starts_with("https://") || url.starts_with("http://")
}

fn destination(query: &str) -> String {
    let query = query.trim();
    if query.is_empty() {
        return HOME_URL.to_string();
    }
    if query.eq_ignore_ascii_case(HOME_URL) {
        return HOME_URL.to_string();
    }
    if valid_page_url(query) {
        return query.to_string();
    }
    if query.contains('.')
        && !query.chars().any(char::is_whitespace)
        && !query.contains("://")
        && !query.starts_with('.')
    {
        return format!("https://{query}");
    }
    let params = url::form_urlencoded::Serializer::new(String::new())
        .append_pair("q", query)
        .finish();
    format!("https://www.google.com/search?{params}")
}

fn rect(x: f64, y: f64, width: f64, height: f64) -> Rect {
    Rect {
        position: LogicalPosition::new(x, y).into(),
        size: LogicalSize::new(width.max(1.0), height.max(1.0)).into(),
    }
}

impl Avant {
    fn new(proxy: EventLoopProxy<Event>) -> Self {
        let data_dir = app_dir();
        let _ = fs::create_dir_all(&data_dir);
        let saved: Session = fs::read_to_string(data_dir.join("session.json"))
            .ok()
            .and_then(|s| serde_json::from_str(&s).ok())
            .unwrap_or_default();

        Self {
            window: None,
            toolbar: None,
            tabs: Vec::new(),
            active: None,
            next_id: 1,
            settings_open: false,
            toolbar_ready: false,
            proxy,
            context: WebContext::new(Some(data_dir.join("webview"))),
            data_dir,
            nickname: if saved.nickname.is_empty() {
                "Guest".to_owned()
            } else {
                saved.nickname
            },
            restore_urls: saved.tabs.into_iter().take(12).collect(),
        }
    }

    fn viewport_size(&self) -> (f64, f64) {
        let Some(window) = &self.window else {
            return (1100.0, 700.0);
        };
        let size = window.inner_size();
        let factor = window.scale_factor();
        (size.width as f64 / factor, size.height as f64 / factor)
    }

    fn toolbar_height(&self) -> f64 {
        if self.settings_open {
            SETTINGS_HEIGHT
        } else {
            TOOLBAR_HEIGHT
        }
    }

    fn content_rect(&self) -> Rect {
        let (width, height) = self.viewport_size();
        let top = self.toolbar_height();
        rect(0.0, top, width, (height - top).max(1.0))
    }

    fn layout(&self) {
        let (width, _) = self.viewport_size();
        if let Some(toolbar) = &self.toolbar {
            let _ = toolbar.set_bounds(rect(0.0, 0.0, width, self.toolbar_height()));
        }
        let content = self.content_rect();
        for tab in &self.tabs {
            let _ = tab.webview.set_bounds(content);
        }
    }

    fn save(&self) {
        let session = Session {
            nickname: self.nickname.clone(),
            tabs: self
                .tabs
                .iter()
                .filter(|tab| !tab.private)
                .map(|tab| tab.url.clone())
                .collect(),
        };
        if let Ok(json) = serde_json::to_string_pretty(&session) {
            let tmp = self.data_dir.join("session.tmp");
            if fs::write(&tmp, json).is_ok() {
                let _ = fs::rename(&tmp, self.data_dir.join("session.json"));
            }
        }
    }

    fn active_tab(&self) -> Option<&Tab> {
        self.tabs.iter().find(|tab| Some(tab.id) == self.active)
    }

    fn refresh_toolbar(&self) {
        if !self.toolbar_ready {
            return;
        }
        let Some(toolbar) = &self.toolbar else {
            return;
        };
        let tabs: Vec<_> = self
            .tabs
            .iter()
            .map(|tab| {
                json!({
                    "id": tab.id,
                    "title": tab.title,
                    "url": tab.url,
                    "private": tab.private,
                    "loading": tab.loading,
                })
            })
            .collect();
        let (url, private, can_back, can_forward, loading) = match self.active_tab() {
            Some(tab) => (
                tab.url.clone(),
                tab.private,
                tab.webview.can_go_back().unwrap_or(false),
                tab.webview.can_go_forward().unwrap_or(false),
                tab.loading,
            ),
            None => (HOME_URL.to_owned(), false, false, false, false),
        };
        let message = json!({
            "tabs": tabs,
            "active": self.active,
            "url": if url == HOME_URL { "" } else { url.as_str() },
            "private": private,
            "canBack": can_back,
            "canForward": can_forward,
            "loading": loading,
            "nickname": self.nickname,
            "settingsOpen": self.settings_open,
        });
        let _ = toolbar.evaluate_script(&format!("window.avantSync({message});"));
    }

    fn show_active(&self) {
        for tab in &self.tabs {
            let selected = Some(tab.id) == self.active;
            let _ = tab.webview.set_visible(selected);
        }
        self.refresh_toolbar();
    }

    fn open_tab(&mut self, input: &str, private: bool, focus: bool) -> Result<(), String> {
        if self.tabs.len() >= MAX_TABS {
            return Ok(());
        }
        let address = destination(input);
        let id = self.next_id;
        self.next_id += 1;
        let is_home = address == HOME_URL;
        let bounds = self.content_rect();
        let visible = focus || self.active.is_none();
        let p_title = self.proxy.clone();
        let p_load = self.proxy.clone();
        let p_popup = self.proxy.clone();

        let builder = if private {
            WebViewBuilder::new().with_incognito(true)
        } else {
            WebViewBuilder::new_with_web_context(&mut self.context)
        };
        let builder = builder
            .with_bounds(bounds)
            .with_visible(visible)
            .with_document_title_changed_handler(move |title| {
                let _ = p_title.send_event(Event::Title { id, title });
            })
            .with_on_page_load_handler(move |state, url| {
                let _ = p_load.send_event(Event::Loading {
                    id,
                    url,
                    loading: matches!(state, PageLoadEvent::Started),
                });
            })
            .with_new_window_req_handler(move |url, _features| {
                if valid_page_url(&url) {
                    let _ = p_popup.send_event(Event::Popup { url, private });
                }
                wry::NewWindowResponse::Deny
            });
        let builder = if is_home {
            builder.with_html(include_str!("../assets/home.html"))
        } else {
            builder.with_url(address.clone())
        };
        let window = self.window.as_ref().ok_or("Application window missing")?;
        let webview = builder
            .build_as_child(window)
            .map_err(|err| format!("Failed to create browser view: {err}"))?;

        self.tabs.push(Tab {
            id,
            title: if private {
                "Private tab".to_owned()
            } else {
                "New tab".to_owned()
            },
            url: address,
            private,
            loading: !is_home,
            webview,
        });
        if visible {
            self.active = Some(id);
        }
        self.show_active();
        self.save();
        Ok(())
    }

    fn select_tab(&mut self, id: u64) {
        if self.tabs.iter().any(|tab| tab.id == id) {
            self.active = Some(id);
            self.settings_open = false;
            self.layout();
            self.show_active();
        }
    }

    fn close_tab(&mut self, id: u64) {
        let Some(index) = self.tabs.iter().position(|tab| tab.id == id) else {
            return;
        };
        let was_active = self.active == Some(id);
        self.tabs.remove(index);
        if was_active {
            self.active = self
                .tabs
                .get(index.min(self.tabs.len().saturating_sub(1)))
                .map(|t| t.id);
        }
        if self.tabs.is_empty() {
            let _ = self.open_tab(HOME_URL, false, true);
        } else {
            self.show_active();
        }
        self.save();
    }

    fn navigate_active(&mut self, input: &str) {
        let address = destination(input);
        if let Some(tab) = self.tabs.iter_mut().find(|tab| Some(tab.id) == self.active) {
            if address == HOME_URL {
                let _ = tab.webview.load_html(include_str!("../assets/home.html"));
                tab.url = HOME_URL.to_owned();
                tab.title = "New tab".to_owned();
            } else {
                let _ = tab.webview.load_url(&address);
                tab.url = address;
                tab.loading = true;
            }
            self.settings_open = false;
            self.layout();
            self.refresh_toolbar();
            self.save();
        }
    }

    fn execute(&mut self, raw: &str) {
        let Ok(command) = serde_json::from_str::<Command>(raw) else {
            return;
        };
        match command.cmd.as_str() {
            "new" => {
                let _ = self.open_tab(HOME_URL, false, true);
            }
            "private" => {
                let _ = self.open_tab(HOME_URL, true, true);
            }
            "select" => {
                if let Some(id) = command.id {
                    self.select_tab(id);
                }
            }
            "close" => {
                if let Some(id) = command.id {
                    self.close_tab(id);
                }
            }
            "navigate" => {
                if let Some(value) = &command.value {
                    self.navigate_active(value);
                }
            }
            "home" => self.navigate_active(HOME_URL),
            "back" => {
                if let Some(tab) = self.active_tab() {
                    let _ = tab.webview.go_back();
                }
            }
            "forward" => {
                if let Some(tab) = self.active_tab() {
                    let _ = tab.webview.go_forward();
                }
            }
            "reload" => {
                if let Some(tab) = self.active_tab() {
                    let _ = tab.webview.reload();
                }
            }
            "settings" => {
                self.settings_open = !self.settings_open;
                self.layout();
            }
            "nickname" => {
                if let Some(value) = command.value {
                    let value = value.trim();
                    self.nickname = if value.is_empty() {
                        "Guest".to_owned()
                    } else {
                        value.chars().take(36).collect()
                    };
                    self.save();
                }
                self.settings_open = false;
                self.layout();
            }
            "focus" => {
                if let Some(toolbar) = &self.toolbar {
                    let _ = toolbar.evaluate_script("window.focusAddress()");
                }
            }
            _ => {}
        }
        self.refresh_toolbar();
    }
}

impl ApplicationHandler<Event> for Avant {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.window.is_some() {
            return;
        }
        let attributes = Window::default_attributes()
            .with_title("Avant")
            .with_theme(Some(Theme::Dark))
            .with_inner_size(LogicalSize::new(1180.0, 790.0))
            .with_min_inner_size(LogicalSize::new(680.0, 460.0));
        let window = match event_loop.create_window(attributes) {
            Ok(window) => window,
            Err(error) => {
                eprintln!("Cannot create window: {error}");
                event_loop.exit();
                return;
            }
        };
        self.window = Some(window);
        let width = self.viewport_size().0;
        let events = self.proxy.clone();
        let ready = self.proxy.clone();
        let toolbar = WebViewBuilder::new()
            .with_bounds(rect(0.0, 0.0, width, TOOLBAR_HEIGHT))
            .with_html(include_str!("../assets/shell.html"))
            .with_ipc_handler(move |message| {
                let _ = events.send_event(Event::Command(message.body().to_string()));
            })
            .with_on_page_load_handler(move |state, _| {
                if matches!(state, PageLoadEvent::Finished) {
                    let _ = ready.send_event(Event::ToolbarLoaded);
                }
            })
            .build_as_child(self.window.as_ref().expect("window created"));

        match toolbar {
            Ok(toolbar) => self.toolbar = Some(toolbar),
            Err(error) => {
                eprintln!("Cannot create Avant toolbar: {error}");
                event_loop.exit();
                return;
            }
        }
        let restore = std::mem::take(&mut self.restore_urls);
        if restore.is_empty() {
            if let Err(error) = self.open_tab(HOME_URL, false, true) {
                eprintln!("{error}");
                event_loop.exit();
                return;
            }
        } else {
            for url in restore {
                if url == HOME_URL || valid_page_url(&url) {
                    if let Err(error) = self.open_tab(&url, false, true) {
                        eprintln!("{error}");
                    }
                }
            }
            if self.tabs.is_empty() {
                let _ = self.open_tab(HOME_URL, false, true);
            }
        }
        self.layout();
        self.refresh_toolbar();
    }

    fn user_event(&mut self, _event_loop: &ActiveEventLoop, event: Event) {
        match event {
            Event::Command(raw) => self.execute(&raw),
            Event::ToolbarLoaded => {
                self.toolbar_ready = true;
                self.refresh_toolbar();
            }
            Event::Title { id, title } => {
                if let Some(tab) = self.tabs.iter_mut().find(|tab| tab.id == id) {
                    if !title.is_empty() && title.len() < 512 {
                        tab.title = title;
                    }
                    self.refresh_toolbar();
                }
            }
            Event::Loading { id, url, loading } => {
                if let Some(tab) = self.tabs.iter_mut().find(|tab| tab.id == id) {
                    tab.loading = loading;
                    if valid_page_url(&url) {
                        tab.url = url;
                    }
                    if !loading {
                        self.save();
                    }
                    self.refresh_toolbar();
                }
            }
            Event::Popup { url, private } => {
                let _ = self.open_tab(&url, private, true);
            }
        }
    }

    fn window_event(
        &mut self,
        event_loop: &ActiveEventLoop,
        _window_id: WindowId,
        event: WindowEvent,
    ) {
        match event {
            WindowEvent::CloseRequested => {
                self.save();
                event_loop.exit();
            }
            WindowEvent::Resized(_) | WindowEvent::ScaleFactorChanged { .. } => self.layout(),
            _ => {}
        }
    }
}

fn main() {
    let event_loop = EventLoop::<Event>::with_user_event()
        .build()
        .expect("Cannot initialize Windows event loop");
    let mut avant = Avant::new(event_loop.create_proxy());
    event_loop.run_app(&mut avant).expect("Event loop failed");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn address_bar_normalizes_domains_and_searches() {
        assert_eq!(destination("example.com"), "https://example.com");
        assert_eq!(destination("https://example.com/a"), "https://example.com/a");
        assert_eq!(
            destination("a b"),
            "https://www.google.com/search?q=a+b"
        );
        assert_eq!(destination(""), HOME_URL);
    }

    #[test]
    fn only_http_urls_are_restored() {
        assert!(valid_page_url("https://example.com"));
        assert!(!valid_page_url("file:///C:/secret.txt"));
        assert!(!valid_page_url("javascript:alert(1)"));
    }
}

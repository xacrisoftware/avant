# avant / v0.2

**Avant** is a compact, dark-mode tabbed browser for Windows 11 built with **Rust + Wry + WebView2**.

This replaces the original C++ HTML-text prototype. Websites now run in a real, modern browser engine, so JavaScript, CSS, images, video, forms and most normal sites work.

> Avant is a hobby-stage application, **not** a hardened or audited Chrome/Edge replacement. Wry hosts the **Microsoft Edge WebView2 runtime** already present on most Windows 11 installations. The rendering engine is **not written from scratch** and is **not independent of Chromium**. It does not install a separate full Chromium distribution alongside the executable.

## Quick start: nothing to install to build

1. In this repository, open **Actions → Build Avant (Rust / Windows)**.
2. Choose a **successful** run for the latest commit. If needed, click **Run workflow**.
3. Download the artifact named **Avant-Windows-x64**.
4. Extract the ZIP and open **Avant.exe** on Windows 11.

No local Rust, Visual Studio or Node.js installation is needed. GitHub Actions performs the compilation. A WebView2 runtime must exist on the device for the program to run. If it is absent, obtain the runtime from Microsoft's official WebView2 installer. Windows might show a warning because this build is unsigned.

### Features

- Native Windows application with lightweight Rust core.
- Carefully designed dark toolbar and new-tab UI (HTML/CSS in \`assets/\`).
- Real page rendering using the installed WebView2 runtime.
- Native tabs with independent back/forward navigation.
- Search from the address bar through Google.
- Private tabs using Wry's WebView2 incognito mode (not stored in session).
- Reload, home, open/close/switch tabs, popup links opening in new tabs.
- Restore normal tabs after a restart.
- Small local settings panel to save a display name.

### Limitations / privacy

- This is not an anonymity tool. Websites, ISPs and Google can see requests. Private mode avoids persistent profile data but does not hide IP addresses or conceal activity from networks.
- Each normal tab uses a shared persistent WebView2 profile in \`%LOCALAPPDATA%\Avant\webview\`. WebView2 can store cookies, cache, history and other browsing data there. Open normal-tab URLs and the nickname are stored in \`%LOCALAPPDATA%\Avant\session.json\`.
- Private tab URLs are not written to \`session.json\`; Wry's \`with_incognito(true)\` is applied to private views.
- There is no account sync, browser extension system, custom download manager, or advanced privacy/security hardening. Browser permissions are handled by the embedded WebView2 runtime.
- Some websites that prohibit WebView-based clients or require unusual hardware DRM may not work.
- This is a Windows-oriented implementation. The Rust/Wry stack can support other platforms but this project only builds and tests Windows.

### Sources

- Rust window/event handling: \`src/main.rs\` (winit + Wry).
- Browser toolbar: \`assets/shell.html\`.
- New tab page: \`assets/home.html\`.
- Build/test/package: \`.github/workflows/windows.yml\`.

### Developer build (optional)

Install Rust + MSVC C++ Build Tools, then run:

\`\`\`powershell
cargo check
cargo test
cargo run
\`\`\`

Or compile with \`cargo build --release\`. GitHub Actions does all of this on hosted Windows runners instead.

### Design intentions

Aim for a *quiet* browser: one focused address bar, no unnecessary panels, no external application UI framework, no bundled Chromium binaries.

License: MIT.

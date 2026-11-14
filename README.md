# Avant

A tiny, experimental Windows browser written in **C++17**. Dark UI, tabs, omnibox with Google search, incognito tabs, and session restore. No Chromium, WebView2, CEF, or other browser engine.

> **Reality check:** Avant 0.1 is a learning project, not a Chrome replacement. Its original HTML-to-text renderer displays readable text and links, not full websites. There is **no JavaScript, CSS layout, video, cookies, login support, downloads, or HTML forms**. Most modern apps (YouTube, Gmail, etc.) will not work. Do not use it for sensitive browsing or authentication.

## Features
- Native dark Windows window and tab strip.
- Enter a URL or search query in the address bar. Searches use Google.
- New, switch, close tabs. Scroll the document and click extracted links.
- Restore normal tabs on next launch; incognito tabs are never saved.
- Local profile nickname in Settings; no accounts or telemetry.
- HTTP and HTTPS through the Windows WinHTTP system library.
- Build on GitHub Actions, without installing Visual Studio locally.

## Download and run
1. Open the repository's **Actions** tab.
2. Select the latest successful **Build Avant for Windows** run.
3. Download the **Avant-Windows-x64** artifact, extract it, and run `Avant.exe`.
4. Alternatively install Visual Studio 2022 with Desktop development with C++ and run:
   ```powershell
   cmake -S . -B build -A x64
   cmake --build build --config Release
   .\build\Release\Avant.exe
   ```

Windows may show a warning for this unsigned experimental executable. Only run builds from a repository you trust.

## Shortcuts
| Keys | Action |
|---|---|
| Ctrl+L | Focus address bar |
| Ctrl+T | New tab |
| Ctrl+W | Close tab |
| Ctrl+Tab | Next tab |
| Ctrl+Shift+N | New incognito tab |
| Alt+Left | Back |
| Alt+Right | Forward |
| F5 | Reload |
| Enter in address bar | Navigate / Google search |

Click **+** for new tab, **N** for a private tab, and the gear symbol (shown as **S**) for settings. Settings allows a local nickname. The address bar uses the same search provider in normal and private windows.

## Data and privacy
Normal tab URLs and nickname are stored in `%LOCALAPPDATA%\Avant\session.txt` and `%LOCALAPPDATA%\Avant\settings.txt`. Incognito URLs are never written to those files. Incognito mode is **not anonymous on the network**: sites, a network provider and Google may still see requests. This prototype has no browser-style sandbox or hardened HTML parser.

## Architecture
- `src/main.cpp`: Win32/GDI UI, tab state, WinHTTP request, small HTML text/link extractor.
- The renderer is custom and deliberately incomplete. It strips scripts/styles and turns HTML into text with a clickable link list.
- Requests run in a worker thread; the UI remains responsive during network loads.

## Roadmap
Next possible steps: Unicode/charset handling, HTML tree parser, actual CSS layout and box model, images, history/bookmarks, security hardening and testing. Building a modern JavaScript-capable browser engine from scratch is a major, multi-year undertaking.

License: MIT.

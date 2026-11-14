#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

constexpr UINT WM_PAGE_READY = WM_APP + 1;
constexpr int BAR_H = 92;
constexpr int TAB_H = 40;
constexpr int MAX_BYTES = 4 * 1024 * 1024;
constexpr int MAX_LINKS = 100;
constexpr wchar_t* HOME = const_cast<wchar_t*>(L"avant://home");

struct Link { std::wstring title, url; RECT rect{}; };
struct Page {
    std::wstring title = L"New tab", text, error;
    std::vector<Link> links;
};
struct Tab {
    unsigned long long id = 0, request = 0;
    std::wstring url = HOME;
    std::vector<std::wstring> history;
    size_t position = 0;
    Page page;
    bool privateTab = false, loading = false;
    int scroll = 0;
};
struct Finished {
    unsigned long long id, request;
    Page page;
};

HWND mainWindow = nullptr, addressEdit = nullptr, nameEdit = nullptr;
WNDPROC addressProc = nullptr, nameProc = nullptr;
std::vector<Tab> tabs;
int currentTab = 0;
bool settingsOpen = false;
unsigned long long nextId = 1;
std::wstring nickname = L"Guest";
std::filesystem::path dataDir;
HBRUSH bgBrush = nullptr, editBrush = nullptr;
COLORREF BG = RGB(15, 16, 20), PANEL = RGB(26, 28, 34), FG = RGB(225, 228, 235), MUTED = RGB(139, 146, 160), ACCENT = RGB(165, 175, 250);

std::string utf8(const std::wstring& input) {
    if (input.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, input.data(), (int)input.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), (int)input.size(), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string& input) {
    if (input.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), (int)input.size(), nullptr, 0);
    if (n <= 0) return L"(Unable to decode UTF-8 response)";
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), (int)input.size(), out.data(), n);
    return out;
}
std::wstring trim(std::wstring s) {
    const auto a = s.find_first_not_of(L" \r\n\t");
    if (a == std::wstring::npos) return L"";
    const auto b = s.find_last_not_of(L" \r\n\t");
    return s.substr(a, b - a + 1);
}
std::wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    return s;
}
std::wstring encodeQuery(const std::wstring& q) {
    const std::string bytes = utf8(q);
    const char* hex = "0123456789ABCDEF";
    std::wstring out;
    for (unsigned char c : bytes) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += wchar_t(c);
        else if (c == ' ') out += L'+';
        else { out += L'%'; out += wchar_t(hex[c >> 4]); out += wchar_t(hex[c & 15]); }
    }
    return out;
}
std::wstring normalize(std::wstring s) {
    s = trim(s);
    if (s.empty()) return HOME;
    if (s.rfind(L"avant://", 0) == 0) return s;
    if (s.rfind(L"https://", 0) == 0 || s.rfind(L"http://", 0) == 0) return s;
    if (s.find(L' ') == std::wstring::npos && s.find(L'.') != std::wstring::npos && s.find(L'.') != s.size() - 1)
        return L"https://" + s;
    return L"https://www.google.com/search?q=" + encodeQuery(s);
}
void replaceAll(std::wstring& s, const std::wstring& a, const std::wstring& b) {
    size_t pos = 0;
    while ((pos = s.find(a, pos)) != std::wstring::npos) { s.replace(pos, a.size(), b); pos += b.size(); }
}
std::wstring decodeEntities(std::wstring s) {
    replaceAll(s, L"&nbsp;", L" "); replaceAll(s, L"&amp;", L"&");
    replaceAll(s, L"&lt;", L"<"); replaceAll(s, L"&gt;", L">");
    replaceAll(s, L"&quot;", L"\""); replaceAll(s, L"&#39;", L"'");
    std::wregex num(LR"(&#(x[0-9a-fA-F]+|[0-9]+);)");
    std::wstring result; size_t last = 0;
    for (std::wsregex_iterator it(s.begin(), s.end(), num), end; it != end; ++it) {
        auto m = *it; result.append(s, last, (size_t)m.position() - last);
        try {
            auto v = m[1].str(); unsigned long cp = std::stoul(v[0] == L'x' ? v.substr(1) : v, nullptr, v[0] == L'x' ? 16 : 10);
            if (cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff)) {
                if (cp <= 0xffff) result += wchar_t(cp);
                else { cp -= 0x10000; result += wchar_t(0xd800 + (cp >> 10)); result += wchar_t(0xdc00 + (cp & 1023)); }
            }
        } catch (...) {}
        last = (size_t)m.position() + m.length();
    }
    result.append(s, last, s.size() - last);
    return result;
}
std::wstring stripTags(const std::wstring& html) {
    std::wstring s = std::regex_replace(html, std::wregex(LR"(<(script|style|svg|noscript|template)\b[^>]*>[\s\S]*?</\1\s*>)", std::regex::icase), L" ");
    s = std::regex_replace(s, std::wregex(LR"(<!--[\s\S]*?-->)"), L" ");
    s = std::regex_replace(s, std::wregex(LR"(<\s*(br|p|div|li|h[1-6]|tr|section|article)\b[^>]*>)", std::regex::icase), L"\n");
    s = std::regex_replace(s, std::wregex(LR"(<[^>]*>)"), L" ");
    s = decodeEntities(s);
    std::wistringstream in(s);
    std::wstring line, output;
    while (std::getline(in, line)) {
        line = std::regex_replace(trim(line), std::wregex(LR"(\s{2,})"), L" ");
        if (!line.empty()) { output += line + L"\n"; if (output.size() > 50000) break; }
    }
    return output;
}
std::wstring resolveUrl(const std::wstring& base, const std::wstring& href) {
    if (href.empty() || href[0] == L'#' || href.rfind(L"javascript:", 0) == 0 || href.rfind(L"data:", 0) == 0 || href.rfind(L"mailto:", 0) == 0) return L"";
    std::vector<wchar_t> out(8192);
    DWORD n = (DWORD)out.size();
    if (SUCCEEDED(UrlCombineW(base.c_str(), href.c_str(), out.data(), &n, 0))) {
        std::wstring url(out.data());
        if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0) return url;
    }
    return L"";
}
Page parseHtml(const std::wstring& base, const std::string& bytes) {
    Page p;
    std::wstring html = wide(bytes);
    std::wsmatch m;
    if (std::regex_search(html, m, std::wregex(LR"(<title\b[^>]*>([\s\S]*?)</title\s*>)", std::regex::icase)))
        p.title = trim(decodeEntities(stripTags(m[1].str())));
    if (p.title.empty()) p.title = base;
    p.text = stripTags(html);
    std::wregex anchors(LR"(<a\b[^>]*href\s*=\s*["']([^"']+)["'][^>]*>([\s\S]*?)</a\s*>)", std::regex::icase);
    for (std::wsregex_iterator it(html.begin(), html.end(), anchors), end; it != end && p.links.size() < MAX_LINKS; ++it) {
        const auto a = *it;
        std::wstring label = trim(stripTags(a[2].str()));
        std::wstring url = resolveUrl(base, decodeEntities(a[1].str()));
        if (!url.empty() && !label.empty() && label.size() < 240) p.links.push_back({label, url, {}});
    }
    return p;
}
Page fetchPage(const std::wstring& url) {
    Page error; error.title = L"Cannot load page";
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    wchar_t host[512]{}, path[8192]{}, extra[4096]{};
    parts.lpszHostName = host; parts.dwHostNameLength = 512;
    parts.lpszUrlPath = path; parts.dwUrlPathLength = 8192;
    parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) { error.error = L"Invalid URL."; return error; }
    HINTERNET session = WinHttpOpen(L"Avant/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error.error = L"Unable to open network session."; return error; }
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);
    HINTERNET conn = WinHttpConnect(session, host, parts.nPort, 0);
    std::wstring resource = std::wstring(path, parts.dwUrlPathLength) + std::wstring(extra, parts.dwExtraInfoLength);
    if (resource.empty()) resource = L"/";
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", resource.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    std::string bytes;
    DWORD status = 0, statusSize = sizeof(status);
    bool ok = req && WinHttpSendRequest(req, L"Accept: text/html, text/plain;q=0.8\r\nAccept-Encoding: identity\r\n", -1, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(req, &available)) { ok = false; break; }
            if (!available) break;
            if (bytes.size() + available > MAX_BYTES) { error.error = L"Page exceeds 4 MB safety limit."; ok = false; break; }
            std::string chunk(available, '\0'); DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), available, &read)) { ok = false; break; }
            if (!read) break;
            bytes.append(chunk.data(), read);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    if (!ok) {
        if (error.error.empty()) error.error = L"Network request failed. Check your connection, URL, or TLS settings.";
        return error;
    }
    Page p = parseHtml(url, bytes);
    if (status >= 400) p.error = L"HTTP status " + std::to_wstring(status);
    if (p.text.empty() && p.error.empty()) p.text = L"This page has no readable static HTML. It may require JavaScript.";
    return p;
}
std::filesystem::path storeFile(const char* name) { return dataDir / name; }
void saveSettings() {
    std::ofstream out(storeFile("settings.txt"), std::ios::binary | std::ios::trunc);
    out << utf8(nickname) << "\n";
}
void saveTabs() {
    std::ofstream out(storeFile("session.txt"), std::ios::binary | std::ios::trunc);
    for (const auto& t : tabs) if (!t.privateTab && t.url != HOME && t.url.rfind(L"avant://", 0) != 0) out << utf8(t.url) << "\n";
}
void selectTab(int index) {
    if (index < 0 || index >= (int)tabs.size()) return;
    currentTab = index;
    settingsOpen = false;
    ShowWindow(nameEdit, SW_HIDE);
    SetWindowTextW(addressEdit, tabs[index].url == HOME ? L"" : tabs[index].url.c_str());
    InvalidateRect(mainWindow, nullptr, FALSE);
}
void requestPage(Tab& t) {
    t.scroll = 0;
    t.loading = t.url.rfind(L"avant://", 0) != 0;
    t.page = {};
    if (!t.loading) { t.page.title = L"Avant"; InvalidateRect(mainWindow, nullptr, FALSE); return; }
    const auto id = t.id, request = ++t.request;
    std::wstring url = t.url;
    HWND target = mainWindow;
    std::thread([id, request, url, target]() {
        auto result = std::make_unique<Finished>();
        result->id = id; result->request = request;
        result->page = fetchPage(url);
        if (IsWindow(target) && PostMessageW(target, WM_PAGE_READY, 0, (LPARAM)result.get())) result.release();
    }).detach();
}
void navigate(const std::wstring& value, bool fromHistory = false) {
    if (tabs.empty()) return;
    Tab& t = tabs[currentTab];
    std::wstring url = normalize(value);
    if (!fromHistory) {
        if (t.position + 1 < t.history.size()) t.history.resize(t.position + 1);
        t.history.push_back(url); t.position = t.history.size() - 1;
    }
    t.url = url;
    requestPage(t);
    SetWindowTextW(addressEdit, url == HOME ? L"" : url.c_str());
    saveTabs();
    InvalidateRect(mainWindow, nullptr, FALSE);
}
void newTab(bool priv = false, const std::wstring& url = HOME) {
    Tab t; t.id = nextId++; t.privateTab = priv; t.url = url;
    t.history.push_back(url); t.page.title = priv ? L"Private tab" : L"New tab";
    tabs.push_back(std::move(t));
    selectTab((int)tabs.size() - 1);
    if (url != HOME) requestPage(tabs.back());
    saveTabs();
    SetFocus(addressEdit);
}
void closeTab(int index) {
    if (index < 0 || index >= (int)tabs.size()) return;
    tabs.erase(tabs.begin() + index);
    if (tabs.empty()) newTab();
    else selectTab(std::min(index, (int)tabs.size() - 1));
    saveTabs();
}
void historyMove(int delta) {
    Tab& t = tabs[currentTab];
    int next = (int)t.position + delta;
    if (next >= 0 && next < (int)t.history.size()) { t.position = next; navigate(t.history[next], true); }
}
void drawText(HDC dc, const std::wstring& text, RECT rc, COLORREF color, UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rc, flags);
}
void fill(HDC dc, RECT r, COLORREF color) { HBRUSH b = CreateSolidBrush(color); FillRect(dc, &r, b); DeleteObject(b); }
void paint(HWND h) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(h, &ps);
    RECT client{}; GetClientRect(h, &client);
    fill(dc, client, BG);
    SetBkMode(dc, TRANSPARENT);
    HFONT font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT bold = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    RECT header{0, 0, client.right, BAR_H}; fill(dc, header, PANEL);
    int tabWidth = std::min(180, std::max(85, (client.right - 150) / std::max(1, (int)tabs.size())));
    for (int i = 0; i < (int)tabs.size(); ++i) {
        int x = i * tabWidth;
        if (x + tabWidth > client.right - 115) break;
        RECT r{x + 4, 5, x + tabWidth - 2, 37};
        fill(dc, r, i == currentTab ? RGB(48, 50, 60) : RGB(31, 33, 39));
        std::wstring title = tabs[i].privateTab ? L"[P] " : L"";
        title += tabs[i].loading ? L"Loading..." : tabs[i].page.title;
        RECT label{x + 12, 5, x + tabWidth - 25, 37}; drawText(dc, title, FG);
        RECT close{x + tabWidth - 25, 6, x + tabWidth - 5, 36}; drawText(dc, L"x", MUTED);
    }
    RECT plus{client.right - 112, 4, client.right - 84, 38}; drawText(dc, L"+", FG);
    RECT priv{client.right - 79, 4, client.right - 52, 38}; drawText(dc, L"N", MUTED);
    RECT sett{client.right - 43, 4, client.right - 12, 38}; drawText(dc, L"S", MUTED);
    RECT back{12, 47, 40, 87}, forward{46, 47, 74, 87}, reload{80, 47, 107, 87};
    drawText(dc, L"<", FG); drawText(dc, L">", FG); drawText(dc, L"R", MUTED);
    if (tabs.empty()) { SelectObject(dc, old); DeleteObject(font); DeleteObject(bold); EndPaint(h, &ps); return; }
    const Tab& t = tabs[currentTab];
    int left = 38, top = BAR_H + 22;
    if (settingsOpen) {
        SelectObject(dc, bold);
        drawText(dc, L"Settings", RECT{left, top, client.right - 30, top + 45}, FG);
        SelectObject(dc, font);
        drawText(dc, L"Local display name (saved only on this device):", RECT{left, top + 68, client.right - 30, top + 100}, MUTED);
        drawText(dc, L"Your tabs are restored on restart. Private tabs are not saved.", RECT{left, top + 150, client.right - 30, top + 186}, MUTED);
        drawText(dc, L"No sync, account, analytics, JavaScript, or browser engine dependencies.", RECT{left, top + 186, client.right - 30, top + 220}, MUTED);
    } else if (t.url == HOME) {
        SelectObject(dc, bold);
        drawText(dc, L"avant.", RECT{left, top + 65, client.right - 30, top + 120}, FG);
        SelectObject(dc, font);
        drawText(dc, L"An independent little corner of the internet.", RECT{left, top + 128, client.right - 30, top + 160}, MUTED);
        drawText(dc, L"Type a URL or Google search above. Ctrl+T for a new tab.", RECT{left, top + 180, client.right - 30, top + 212}, FG);
        drawText(dc, L"Custom HTML text renderer. Complex websites are not supported.", RECT{left, top + 210, client.right - 30, top + 243}, MUTED);
    } else if (t.loading) {
        drawText(dc, L"Loading " + t.url, RECT{left, top, client.right - 30, top + 36}, MUTED);
    } else {
        int y = top - t.scroll;
        SelectObject(dc, bold);
        RECT title{left, y, client.right - 35, y + 75};
        drawText(dc, t.page.title, title, FG, DT_WORDBREAK);
        y += 82;
        SelectObject(dc, font);
        if (!t.page.error.empty()) { drawText(dc, t.page.error, RECT{left, y, client.right - 35, y + 50}, RGB(235, 136, 136), DT_WORDBREAK); y += 55; }
        int width = std::max(120, client.right - left - 48);
        RECT body{left, y, left + width, y};
        DrawTextW(dc, t.page.text.c_str(), -1, &body, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        body.bottom = std::min(body.bottom, body.top + 200000);
        drawText(dc, t.page.text, body, FG, DT_WORDBREAK | DT_NOPREFIX);
        y = body.bottom + 25;
        if (!t.page.links.empty()) {
            SelectObject(dc, bold);
            drawText(dc, L"Links", RECT{left, y, client.right - 35, y + 42}, FG);
            SelectObject(dc, font);
            y += 48;
            for (auto& link : const_cast<Page&>(t.page).links) {
                RECT r{left, y, client.right - 35, y + 29};
                link.rect = r;
                if (y + 29 >= BAR_H && y < client.bottom) drawText(dc, link.title, r, ACCENT);
                y += 33;
            }
        }
    }
    SelectObject(dc, old);
    DeleteObject(font); DeleteObject(bold);
    EndPaint(h, &ps);
}
LRESULT CALLBACK editSubclass(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    bool isAddress = hwnd == addressEdit;
    if (msg == WM_KEYDOWN && w == VK_RETURN) {
        if (isAddress) navigate(getText(hwnd));
        else { nickname = trim(getText(hwnd)); if (nickname.empty()) nickname = L"Guest"; saveSettings(); SetFocus(mainWindow); }
        return 0;
    }
    return CallWindowProcW(isAddress ? addressProc : nameProc, hwnd, msg, w, l);
}
void layout(HWND h) {
    RECT rc{}; GetClientRect(h, &rc);
    MoveWindow(addressEdit, 117, 49, std::max(100, rc.right - 130), 32, TRUE);
    MoveWindow(nameEdit, 38, 204, std::max(200, std::min(400, rc.right - 70)), 32, TRUE);
}
void initData() {
    wchar_t path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, path))) dataDir = std::filesystem::path(path) / L"Avant";
    else dataDir = std::filesystem::current_path() / L"AvantData";
    std::error_code ec; std::filesystem::create_directories(dataDir, ec);
    std::ifstream settings(storeFile("settings.txt"), std::ios::binary);
    std::string s;
    if (std::getline(settings, s) && !s.empty()) nickname = wide(s);
}
LRESULT CALLBACK windowProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        mainWindow = h;
        bgBrush = CreateSolidBrush(BG); editBrush = CreateSolidBrush(RGB(43, 46, 56));
        addressEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 117, 49, 500, 32, h, nullptr, nullptr, nullptr);
        nameEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 38, 204, 400, 32, h, nullptr, nullptr, nullptr);
        HFONT ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(addressEdit, WM_SETFONT, (WPARAM)ui, TRUE);
        SendMessageW(nameEdit, WM_SETFONT, (WPARAM)ui, TRUE);
        addressProc = (WNDPROC)SetWindowLongPtrW(addressEdit, GWLP_WNDPROC, (LONG_PTR)editSubclass);
        nameProc = (WNDPROC)SetWindowLongPtrW(nameEdit, GWLP_WNDPROC, (LONG_PTR)editSubclass);
        initData();
        std::ifstream in(storeFile("session.txt"), std::ios::binary);
        std::string line;
        while (std::getline(in, line) && tabs.size() < 12) {
            if (line.rfind("https://", 0) == 0 || line.rfind("http://", 0) == 0) newTab(false, wide(line));
        }
        if (tabs.empty()) newTab();
        layout(h);
        return 0;
    }
    case WM_SIZE: layout(h); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, FG); SetBkColor((HDC)w, RGB(43, 46, 56));
        return (LRESULT)editBrush;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(h); return 0;
    case WM_PAGE_READY: {
        std::unique_ptr<Finished> result((Finished*)l);
        for (auto& t : tabs) if (t.id == result->id && t.request == result->request) {
            t.page = std::move(result->page); t.loading = false;
            InvalidateRect(h, nullptr, FALSE); break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (!settingsOpen && !tabs.empty()) {
            Tab& t = tabs[currentTab];
            t.scroll = std::max(0, t.scroll - GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 90);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        RECT rc{}; GetClientRect(h, &rc);
        if (y < TAB_H) {
            if (x >= rc.right - 112 && x < rc.right - 84) newTab();
            else if (x >= rc.right - 79 && x < rc.right - 52) newTab(true);
            else if (x >= rc.right - 43) {
                settingsOpen = !settingsOpen;
                if (settingsOpen) { SetWindowTextW(nameEdit, nickname.c_str()); ShowWindow(nameEdit, SW_SHOW); }
                else ShowWindow(nameEdit, SW_HIDE);
                InvalidateRect(h, nullptr, FALSE);
            } else {
                int tabWidth = std::min(180, std::max(85, (rc.right - 150) / std::max(1, (int)tabs.size())));
                int index = x / tabWidth;
                if (index >= 0 && index < (int)tabs.size()) {
                    if (x % tabWidth >= tabWidth - 27) closeTab(index);
                    else selectTab(index);
                }
            }
            return 0;
        }
        if (y >= 47 && y <= 87) {
            if (x < 42) historyMove(-1);
            else if (x < 76) historyMove(1);
            else if (x < 112 && !tabs.empty()) requestPage(tabs[currentTab]);
            return 0;
        }
        if (!settingsOpen && !tabs.empty()) {
            for (const auto& link : tabs[currentTab].page.links) {
                if (x >= link.rect.left && x < link.rect.right && y >= link.rect.top && y < link.rect.bottom) { navigate(link.url); break; }
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_F5 && !tabs.empty()) { requestPage(tabs[currentTab]); return 0; }
        if (w == VK_LEFT && GetKeyState(VK_MENU) < 0) { historyMove(-1); return 0; }
        if (w == VK_RIGHT && GetKeyState(VK_MENU) < 0) { historyMove(1); return 0; }
        break;
    case WM_DESTROY:
        saveTabs(); saveSettings();
        DeleteObject(bgBrush); DeleteObject(editBrush);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls);
    cls.hInstance = instance; cls.lpszClassName = L"AvantNativeBrowser";
    cls.lpfnWndProc = windowProc; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassExW(&cls)) return 1;
    HWND h = CreateWindowExW(0, cls.lpszClassName, L"Avant", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760, nullptr, nullptr, instance, nullptr);
    if (!h) return 1;
    ShowWindow(h, show); UpdateWindow(h);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0) {
            if (msg.wParam == 'L') { SetFocus(addressEdit); SendMessageW(addressEdit, EM_SETSEL, 0, -1); continue; }
            if (msg.wParam == 'T') { newTab(); continue; }
            if (msg.wParam == 'W') { closeTab(currentTab); continue; }
            if (msg.wParam == VK_TAB && !tabs.empty()) { selectTab((currentTab + 1) % (int)tabs.size()); continue; }
            if (msg.wParam == 'N' && GetKeyState(VK_SHIFT) < 0) { newTab(true); continue; }
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F5 && !tabs.empty()) { requestPage(tabs[currentTab]); continue; }
        if (msg.message == WM_SYSKEYDOWN && msg.wParam == VK_LEFT) { historyMove(-1); continue; }
        if (msg.message == WM_SYSKEYDOWN && msg.wParam == VK_RIGHT) { historyMove(1); continue; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

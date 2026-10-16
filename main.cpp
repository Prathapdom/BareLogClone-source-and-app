// BareLog: native Win32 real-time log viewer (tail -f for Windows).
#include "settings.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <unordered_map>
#include <cwchar>

using namespace logcore;
using settings::Rule;
using settings::TabState;

constexpr UINT WM_DOCCHANGED = WM_APP + 1, WM_FINDDONE = WM_APP + 2, WM_TESTQUERY = WM_APP + 100;
constexpr ULONG_PTR CopyDataOpen = 0x42A1E106;
constexpr size_t MaxLineChars = 32768;
constexpr int QueryEditId = 200;
constexpr UINT_PTR TimerQuery = 1, TimerSave = 2, TimerAutoScroll = 3;
const wchar_t* const AppName = L"BareLog";
const wchar_t* const MainClass = L"BareLog.Main";
const wchar_t* const ViewClass = L"BareLog.View";
const wchar_t* const DialogClass = L"BareLog.Dialog";

enum Cmd : int {
    CmdOpen = 100, CmdRecent, CmdFollow, CmdWrap, CmdLineNumbers, CmdHighlight, CmdTheme, CmdMore,
    CmdCase, CmdRegex, CmdFilter, CmdPrev, CmdNext, CmdFind, CmdGoto, CmdCloseTab, CmdCloseOthers, CmdCloseAll,
    CmdNextTab, CmdPrevTab, CmdReload, CmdZoomIn, CmdZoomOut, CmdZoomReset, CmdFont, CmdCopy, CmdSelectAll,
    CmdOpenEditor, CmdShowFolder, CmdCopyPath, CmdRestoreSession, CmdSettingsFolder, CmdAbout, CmdExit, CmdTabsOverflow, CmdAddTab,
    CmdTabBase = 1000, CmdTabCloseBase = 2000, CmdRecentBase = 3000, CmdRecentClear = 3099, CmdThemeBase = 3100,
    CmdEncodingBase = 3200, CmdTabSizeBase = 3300, CmdEmptyRecentBase = 3500,
};

// ---------------------------------------------------------------- appearance

struct Palette {
    bool dark;
    COLORREF chrome, bg, surface, text, muted, border, hover, pressed, accent, accentText, accentSoft, selection, selectionText, gutter, gutterText, match, matchText, danger;
};
const Palette LightPalette{false, RGB(243, 243, 243), RGB(255, 255, 255), RGB(255, 255, 255), RGB(28, 28, 28), RGB(96, 96, 96), RGB(212, 212, 212), RGB(230, 230, 230), RGB(218, 218, 218),
                           RGB(0, 95, 184), RGB(255, 255, 255), RGB(214, 232, 248), RGB(204, 228, 247), RGB(0, 0, 0), RGB(247, 247, 247), RGB(150, 150, 150), RGB(255, 214, 0), RGB(0, 0, 0), RGB(196, 43, 28)};
const Palette DarkPalette{true, RGB(39, 39, 39), RGB(28, 28, 28), RGB(45, 45, 45), RGB(232, 232, 232), RGB(160, 160, 160), RGB(68, 68, 68), RGB(55, 55, 55), RGB(64, 64, 64),
                          RGB(76, 194, 255), RGB(0, 0, 0), RGB(36, 72, 102), RGB(38, 79, 120), RGB(255, 255, 255), RGB(28, 28, 28), RGB(105, 105, 105), RGB(255, 200, 0), RGB(0, 0, 0), RGB(255, 120, 110)};
const Palette AmoledPalette{true, RGB(0, 0, 0), RGB(0, 0, 0), RGB(16, 16, 16), RGB(225, 225, 225), RGB(140, 140, 140), RGB(48, 48, 48), RGB(28, 28, 28), RGB(40, 40, 40),
                            RGB(76, 194, 255), RGB(0, 0, 0), RGB(18, 50, 80), RGB(24, 58, 92), RGB(255, 255, 255), RGB(0, 0, 0), RGB(90, 90, 90), RGB(255, 200, 0), RGB(0, 0, 0), RGB(255, 120, 110)};

// Undocumented uxtheme exports (Windows 10 1903+) for dark context menus and scrollbars.
namespace darkmode {
using SetPreferredAppModeFn = int(WINAPI*)(int);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
using FlushMenuThemesFn = void(WINAPI*)();
SetPreferredAppModeFn setPreferredAppMode = nullptr;
AllowDarkModeForWindowFn allowForWindow = nullptr;
FlushMenuThemesFn flushMenuThemes = nullptr;
void init() {
    using RtlGetNtVersionNumbersFn = void(WINAPI*)(DWORD*, DWORD*, DWORD*);
    auto rtl = (RtlGetNtVersionNumbersFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtVersionNumbers");
    DWORD major = 0, minor = 0, build = 0;
    if (rtl) rtl(&major, &minor, &build);
    build &= ~0xF0000000u;
    if (major < 10 || build < 18362) return;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    setPreferredAppMode = (SetPreferredAppModeFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    allowForWindow = (AllowDarkModeForWindowFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    flushMenuThemes = (FlushMenuThemesFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
}
void app(bool dark) { if (setPreferredAppMode) setPreferredAppMode(dark ? 2 : 3); if (flushMenuThemes) flushMenuThemes(); }
void window(HWND h, bool dark) { if (!h) return; if (allowForWindow) allowForWindow(h, dark); SetWindowTheme(h, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr); }
} // namespace darkmode

HINSTANCE gInst;
HWND gMain, gView, gEdit, gTip;
settings::Settings S;
std::wstring gSettingsPath;
Palette P = LightPalette;
int dpi = 96;
int px(int v) { return MulDiv(v, dpi, 96); }
HFONT uiFont, uiBold, uiLarge, iconFont, iconSmall, monoFont;
std::wstring iconFace = L"Segoe Fluent Icons";
int charW = 8, lineH = 16, textOffset = 1;
HBRUSH surfaceBrush, chromeBrush;
std::vector<std::shared_ptr<const Matcher>> ruleMatchers;
bool editGuard = false;

void fill(HDC dc, const RECT& r, COLORREF c) { SetDCBrushColor(dc, c); FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH)); }
void roundRect(HDC dc, const RECT& r, COLORREF face, COLORREF border, int radius) {
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, face); SetDCPenColor(dc, border);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op);
}
void drawText(HDC dc, const std::wstring& s, RECT r, COLORREF c, HFONT f, UINT fmt = DT_LEFT) {
    SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}
int textWidth(HDC dc, HFONT f, const std::wstring& s) { SelectObject(dc, f); SIZE sz{}; GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz); return sz.cx; }
// Shortens a path from the middle, keeping the drive and the file name visible.
std::wstring compactPath(HDC dc, HFONT f, const std::wstring& s, int width) {
    if (width <= 0 || textWidth(dc, f, s) <= width) return s;
    size_t name = s.find_last_of(L"\\/");
    size_t keepTail = name == std::wstring::npos ? s.size() / 2 : s.size() - name;
    size_t head = std::min<size_t>(s.size() - keepTail, 24);
    for (;;) {
        std::wstring c = s.substr(0, head) + L"\u2026" + s.substr(s.size() - keepTail);
        if (textWidth(dc, f, c) <= width) return c;
        if (head > 3) --head;
        else if (keepTail > 4) --keepTail;
        else return c;
    }
}
bool hasGlyph(wchar_t c) {
    static std::unordered_map<wchar_t, bool> known;
    auto it = known.find(c);
    if (it != known.end()) return it->second;
    HDC dc = GetDC(nullptr); HGDIOBJ o = SelectObject(dc, iconFont);
    WORD g = 0xFFFF; GetGlyphIndicesW(dc, &c, 1, &g, GGI_MARK_NONEXISTING_GLYPHS);
    SelectObject(dc, o); ReleaseDC(nullptr, dc);
    return known[c] = g != 0xFFFF;
}
std::wstring fmtNum(uint64_t v) {
    std::wstring s = std::to_wstring(v), o;
    for (size_t i = 0; i < s.size(); ++i) { if (i && (s.size() - i) % 3 == 0) o += L','; o += s[i]; }
    return o;
}
std::wstring fmtSize(uint64_t b) {
    wchar_t buf[64];
    if (b < 1024) swprintf(buf, 64, L"%llu bytes", (unsigned long long)b);
    else if (b < 1024 * 1024) swprintf(buf, 64, L"%.1f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) swprintf(buf, 64, L"%.1f MB", b / (1024.0 * 1024));
    else swprintf(buf, 64, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    return buf;
}
std::wstring fileName(const std::wstring& p) { size_t s = p.find_last_of(L"\\/"); return s == std::wstring::npos ? p : p.substr(s + 1); }
std::wstring folderOf(const std::wstring& p) { size_t s = p.find_last_of(L"\\/"); return s == std::wstring::npos ? std::wstring() : p.substr(0, s); }
bool samePath(const std::wstring& a, const std::wstring& b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL; }
std::wstring windowText(HWND h) { int n = GetWindowTextLengthW(h); std::wstring s(n + 1, L'\0'); GetWindowTextW(h, &s[0], n + 1); s.resize(n); return s; }
std::wstring errorText(DWORD e) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring s = msg ? msg : L"Error " + std::to_wstring(e);
    LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}

void titleBar(HWND h) {
    BOOL dark = P.dark;
    DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    COLORREF caption = S.theme == 3 ? RGB(0, 0, 0) : 0xFFFFFFFF; // DWMWA_COLOR_DEFAULT
    DwmSetWindowAttribute(h, 35 /*DWMWA_CAPTION_COLOR, Windows 11*/, &caption, sizeof caption);
}

// ---------------------------------------------------------------- tabs

struct CacheLine { std::wstring text; int rule; };
struct Tab {
    uint32_t id = 0;
    std::unique_ptr<LogDoc> doc;
    TabState st;
    uint64_t top = 0; int sub = 0; int hcol = 0;
    int64_t anchor = -1, caret = -1;
    bool unread = false, loaded = false, finding = false;
    uint64_t lastLines = 0, generation = UINT64_MAX, pendingTop = UINT64_MAX, maxCols = 0;
    std::shared_ptr<const Matcher> query;
    std::wstring queryMsg;
    std::unordered_map<uint64_t, CacheLine> cache;
    std::thread finder;
    std::atomic<bool> findCancel{false};
    uint32_t findSeq = 0;
    ~Tab() { findCancel = true; if (finder.joinable()) finder.join(); doc.reset(); }
};
std::vector<std::unique_ptr<Tab>> tabs;
int active = -1;
uint32_t nextTabId = 1;
Tab* cur() { return active >= 0 && active < (int)tabs.size() ? tabs[active].get() : nullptr; }
Tab* tabById(uint32_t id) { for (auto& t : tabs) if (t->id == id) return t.get(); return nullptr; }
bool filtering(const Tab& t) { return t.st.filter && t.query && t.query->valid && !t.query->empty(); }
uint64_t rowCount(const Tab& t, const DocInfo& i) { return filtering(t) ? i.filteredRows : i.lines; }
uint64_t rowCount(const Tab& t) { return rowCount(t, t.doc->info()); }
uint64_t rowLine(const Tab& t, uint64_t r) { return filtering(t) ? t.doc->filteredLine(r) : r; }
uint64_t lineRow(const Tab& t, uint64_t line) { return filtering(t) ? t.doc->rowForLine(line) : line; }

void rebuildRuleMatchers() {
    ruleMatchers.clear();
    for (auto& r : S.rules) ruleMatchers.push_back(r.text.empty() ? nullptr : Matcher::make(r.text, r.matchCase, r.regex));
}
int ruleFor(const std::wstring& raw) {
    for (size_t i = 0; i < ruleMatchers.size() && i < S.rules.size(); ++i)
        if (S.rules[i].enabled && ruleMatchers[i] && ruleMatchers[i]->valid && ruleMatchers[i]->matches(raw)) return (int)i;
    return -1;
}
std::wstring expandTabs(const std::wstring& s) {
    bool plain = true;
    for (wchar_t c : s) if (c < 0x20) { plain = false; break; }
    if (plain) return s;
    std::wstring o; o.reserve(s.size() + 16);
    for (wchar_t c : s) {
        if (c == L'\t') o.append(size_t(S.tabSize - (o.size() % S.tabSize)), L' ');
        else o += c < 0x20 ? L'·' : c;
        if (o.size() >= MaxLineChars) break;
    }
    return o;
}
void clearCaches() { for (auto& t : tabs) { t->cache.clear(); t->maxCols = 0; } }
void fetch(Tab& t, std::vector<uint64_t> lines) {
    lines.erase(std::remove_if(lines.begin(), lines.end(), [&](uint64_t l) { return l == UINT64_MAX || t.cache.count(l); }), lines.end());
    if (lines.empty()) return;
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    if (t.cache.size() + lines.size() > 8000) t.cache.clear();
    auto texts = t.doc->readLines(lines, MaxLineChars);
    for (size_t k = 0; k < lines.size(); ++k) {
        CacheLine c; c.rule = ruleFor(texts[k]); c.text = expandTabs(texts[k]);
        t.maxCols = std::max<uint64_t>(t.maxCols, c.text.size());
        t.cache[lines[k]] = std::move(c);
    }
}
const CacheLine& lineAt(Tab& t, uint64_t l) { fetch(t, {l}); return t.cache[l]; }

// ---------------------------------------------------------------- view geometry and scrolling

int viewW = 0, viewH = 0, gutterW = 0, textLeft = 0, wrapCols = 80, fullRows = 1;
struct PaintedRow { int y; uint64_t line, row; };
std::vector<PaintedRow> painted;
std::vector<std::pair<RECT, int>> emptyLinks;

void viewMetrics(Tab* t) {
    RECT r; GetClientRect(gView, &r); viewW = r.right; viewH = r.bottom;
    uint64_t lines = t ? t->doc->info().lines : 0;
    int digits = 1; for (uint64_t v = std::max<uint64_t>(lines, 1); v >= 10; v /= 10) ++digits;
    gutterW = S.lineNumbers ? std::max(digits, 4) * charW + px(22) : 0;
    textLeft = gutterW + px(8);
    wrapCols = std::max(8, (viewW - textLeft - px(6)) / std::max(1, charW));
    fullRows = std::max(1, viewH / std::max(1, lineH));
}
int wrapRows(Tab& t, uint64_t line) {
    if (!S.wrap) return 1;
    size_t n = lineAt(t, line).text.size();
    return std::max(1, (int)((n + wrapCols - 1) / wrapCols));
}
std::pair<uint64_t, int> endPos(Tab& t, uint64_t rows) {
    if (!rows) return {0, 0};
    if (!S.wrap) return {rows > (uint64_t)fullRows ? rows - fullRows : 0, 0};
    std::vector<uint64_t> ls;
    for (uint64_t r = rows > (uint64_t)fullRows ? rows - fullRows : 0; r < rows; ++r) ls.push_back(rowLine(t, r));
    fetch(t, ls);
    int need = fullRows; uint64_t r = rows - 1;
    for (;;) {
        int k = wrapRows(t, rowLine(t, r));
        if (k >= need) return {r, k - need};
        need -= k;
        if (r == 0) return {0, 0};
        --r;
    }
}
void clampTop(Tab& t) {
    if (!S.wrap) t.sub = 0;
    auto e = endPos(t, rowCount(t));
    if (t.top > e.first || (t.top == e.first && t.sub > e.second)) { t.top = e.first; t.sub = e.second; }
}
bool atEnd(Tab& t) { auto e = endPos(t, rowCount(t)); return t.top > e.first || (t.top == e.first && t.sub >= e.second); }
void goEnd(Tab& t) { auto e = endPos(t, rowCount(t)); t.top = e.first; t.sub = e.second; }
void scrollBy(Tab& t, int64_t delta) {
    uint64_t rows = rowCount(t);
    if (!rows) return;
    if (!S.wrap) {
        int64_t nt = (int64_t)t.top + delta;
        t.top = nt < 0 ? 0 : (uint64_t)nt;
        clampTop(t);
        return;
    }
    while (delta > 0) { int k = wrapRows(t, rowLine(t, t.top)); if (t.sub + 1 < k) t.sub++; else if (t.top + 1 < rows) { t.top++; t.sub = 0; } else break; --delta; }
    while (delta < 0) { if (t.sub > 0) t.sub--; else if (t.top > 0) { t.top--; t.sub = wrapRows(t, rowLine(t, t.top)) - 1; } else break; ++delta; }
    clampTop(t);
}
void ensureVisible(Tab& t, uint64_t row, bool center = false) {
    uint64_t rows = rowCount(t);
    if (row >= rows) return;
    if (!S.wrap) {
        uint64_t full = (uint64_t)fullRows;
        if (center && (row < t.top || row >= t.top + full)) t.top = row > full / 3 ? row - full / 3 : 0;
        else if (row < t.top) t.top = row;
        else if (row >= t.top + full) t.top = row - full + 1;
        clampTop(t);
        return;
    }
    if (row < t.top || (row == t.top && t.sub > 0)) {
        t.top = row; t.sub = 0;
        if (center) scrollBy(t, -fullRows / 3);
    } else {
        int n = wrapRows(t, rowLine(t, t.top)) - t.sub;
        for (uint64_t r = t.top + 1; r <= row && n <= fullRows; ++r) n += wrapRows(t, rowLine(t, r));
        if (n > fullRows) {
            int need = fullRows; uint64_t r = row;
            for (;;) {
                int k = wrapRows(t, rowLine(t, r));
                if (k >= need) { t.top = r; t.sub = k - need; break; }
                need -= k;
                if (r == 0) { t.top = 0; t.sub = 0; break; }
                --r;
            }
            if (center) scrollBy(t, fullRows / 3);
        }
    }
    clampTop(t);
}

constexpr uint64_t ScrollMax = 0x7FFF0000;
void updateScrollbars() {
    Tab* t = cur();
    viewMetrics(t);
    SCROLLINFO si{sizeof si};
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    uint64_t rows = t ? rowCount(*t) : 0;
    double f = rows > ScrollMax ? double(ScrollMax) / double(rows) : 1.0;
    bool need = t && (rows > (uint64_t)fullRows || t->top > 0 || (S.wrap && rows > 0 && !(t->top == 0 && t->sub == 0 && atEnd(*t))));
    si.nMin = 0;
    si.nMax = need ? (int)((rows - 1) * f) : 0;
    si.nPage = need ? std::max(1u, (UINT)(fullRows * f)) : 1;
    if (need && S.wrap) si.nMax += 1; // let the thumb reach the last wrapped line
    si.nPos = t ? (int)(t->top * f) : 0;
    if (!need) { si.nMax = 0; si.nPage = 2; }
    SetScrollInfo(gView, SB_VERT, &si, TRUE);
    SCROLLINFO h{sizeof h};
    h.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (t && !S.wrap) {
        uint64_t cols = std::max<uint64_t>(t->maxCols, std::min<uint64_t>(t->doc->info().maxLineChars, MaxLineChars));
        int visible = std::max(1, (viewW - textLeft) / std::max(1, charW));
        if ((int64_t)cols + 2 > visible) { h.nMax = (int)cols + 2; h.nPage = visible; h.nPos = t->hcol; }
        else { h.nMax = 0; h.nPage = 2; t->hcol = 0; }
    } else { h.nMax = 0; h.nPage = 2; if (t) t->hcol = 0; }
    SetScrollInfo(gView, SB_HORZ, &h, TRUE);
}

// ---------------------------------------------------------------- chrome (command bar, tabs, find bar, status)

enum HitKind { KButton, KTab, KTabClose, KChip };
struct Hit { RECT r; int cmd; int kind; std::wstring glyph, label, tip; bool on = false, dropdown = false; };
std::vector<Hit> hits;
std::vector<int> separators;
int hoverHit = -1, pressHit = -1, toolCount = 0;
RECT rcCmd, rcTabs, rcQuery, rcView, rcStatus, rcEditBox, rcQueryMsg;

void setTool(int id, const RECT& r, const std::wstring& tip) {
    TOOLINFOW ti{sizeof ti};
    ti.uFlags = TTF_SUBCLASS; ti.hwnd = gMain; ti.uId = id; ti.rect = r; ti.lpszText = const_cast<wchar_t*>(tip.c_str());
    SendMessageW(gTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}
void buildChrome() {
    RECT c; GetClientRect(gMain, &c);
    int w = c.right;
    rcCmd = {0, 0, w, px(46)};
    rcTabs = {0, rcCmd.bottom, w, rcCmd.bottom + px(36)};
    rcQuery = {0, rcTabs.bottom, w, rcTabs.bottom + px(44)};
    rcStatus = {0, std::max<LONG>(rcQuery.bottom, c.bottom - px(28)), w, c.bottom};
    rcView = {0, rcQuery.bottom, w, rcStatus.top};
    for (int i = 0; i < toolCount; ++i) { TOOLINFOW ti{sizeof ti}; ti.hwnd = gMain; ti.uId = i + 1; SendMessageW(gTip, TTM_DELTOOLW, 0, (LPARAM)&ti); }
    hits.clear(); separators.clear();
    HDC dc = GetDC(gMain);
    Tab* t = cur();

    struct B { int cmd; const wchar_t* glyph; std::wstring label, tip; bool on, dropdown; };
    std::vector<B> left{
        {CmdOpen, L"", L"Open", L"Open files (Ctrl+O)", false, false},
        {CmdRecent, L"", L"Recent", L"Recently opened files", false, true},
        {0, nullptr, L"", L"", false, false},
        {CmdFollow, L"", L"Follow tail", L"Follow new lines as they are written (Ctrl+T)", t && t->st.follow, false},
        {CmdWrap, L"", L"Wrap", L"Wrap long lines (Alt+Z)", S.wrap, false},
        {CmdLineNumbers, L"", L"Line numbers", L"Show line numbers", S.lineNumbers, false},
    };
    std::vector<B> right{
        {CmdHighlight, L"", L"Highlighting", L"Line highlighting rules (Ctrl+H)", false, false},
        {CmdTheme, P.dark ? L"" : L"", L"Theme", L"Light, dark or AMOLED black", false, true},
        {CmdMore, L"", L"", L"More options", false, false},
    };
    auto widthOf = [&](const B& b, bool compact) {
        int wv = px(12) + px(18) + px(12);
        if (!compact && !b.label.empty()) wv += px(8) + textWidth(dc, uiFont, b.label);
        if (b.dropdown) wv += px(16);
        return wv;
    };
    int total = 0;
    for (auto& b : left) total += b.cmd ? widthOf(b, false) + px(4) : px(13);
    for (auto& b : right) total += widthOf(b, false) + px(4);
    bool compact = total > w - px(24);
    int bh = px(32), by = (rcCmd.bottom - bh) / 2, x = px(8);
    for (auto& b : left) {
        if (!b.cmd) { separators.push_back(x + px(4)); x += px(13); continue; }
        int bw = widthOf(b, compact);
        Hit h; h.r = {x, by, x + bw, by + bh}; h.cmd = b.cmd; h.kind = KButton; h.glyph = b.glyph; h.label = compact ? L"" : b.label; h.tip = b.tip; h.on = b.on; h.dropdown = b.dropdown;
        hits.push_back(h); x += bw + px(4);
    }
    int rx = w - px(8);
    for (auto it = right.rbegin(); it != right.rend(); ++it) {
        int bw = widthOf(*it, compact);
        Hit h; h.r = {rx - bw, by, rx, by + bh}; h.cmd = it->cmd; h.kind = KButton; h.glyph = it->glyph; h.label = compact ? L"" : it->label; h.tip = it->tip; h.dropdown = it->dropdown;
        hits.push_back(h); rx -= bw + px(4);
    }

    // Tabs: shrink to fit, keep the active tab visible, overflow menu for the rest.
    int tx = px(8), ty = rcTabs.top + px(4), avail = w - px(8) - px(40) - px(40);
    std::vector<int> widths;
    for (auto& tb : tabs) widths.push_back(std::max(px(120), std::min(px(260), textWidth(dc, uiFont, fileName(tb->st.path)) + px(58))));
    int sum = 0; for (int v : widths) sum += v;
    if (sum > avail && !tabs.empty()) { int each = std::max(px(110), avail / (int)tabs.size()); for (auto& v : widths) v = std::min(v, each); }
    size_t first = 0;
    auto fits = [&](size_t from, size_t to) { int s = 0; for (size_t i = from; i <= to; ++i) s += widths[i]; return s <= avail; };
    if (active >= 0) while (first < (size_t)active && !fits(first, active)) ++first;
    bool overflow = first > 0;
    for (size_t i = first; i < tabs.size(); ++i) {
        if (tx + widths[i] > px(8) + avail) { overflow = true; break; }
        RECT tr{tx, ty, tx + widths[i], rcTabs.bottom};
        Hit close; close.r = {tr.right - px(30), ty + (rcTabs.bottom - ty - px(22)) / 2, tr.right - px(8), ty + (rcTabs.bottom - ty - px(22)) / 2 + px(22)};
        close.cmd = CmdTabCloseBase + (int)i; close.kind = KTabClose; close.tip = L"Close (Ctrl+W)";
        hits.push_back(close);
        Hit h; h.r = tr; h.cmd = CmdTabBase + (int)i; h.kind = KTab; h.label = fileName(tabs[i]->st.path); h.tip = tabs[i]->st.path; h.on = (int)i == active;
        hits.push_back(h);
        tx += widths[i] + px(2);
    }
    { Hit h; h.r = {tx + px(4), ty + px(2), tx + px(36), rcTabs.bottom - px(4)}; h.cmd = CmdAddTab; h.kind = KButton; h.glyph = L""; h.tip = L"Open files (Ctrl+O)"; hits.push_back(h); }
    if (overflow) { Hit h; h.r = {w - px(44), ty + px(2), w - px(8), rcTabs.bottom - px(4)}; h.cmd = CmdTabsOverflow; h.kind = KButton; h.glyph = L""; h.tip = L"All open tabs"; hits.push_back(h); }

    // Find / filter bar.
    int qy = rcQuery.top + px(7), qh = px(30);
    rcEditBox = {px(12), qy, px(12) + std::max(px(180), std::min(px(460), w * 42 / 100)), qy + qh};
    int cx = rcEditBox.right + px(8);
    auto chip = [&](int cmd, const wchar_t* glyph, const std::wstring& label, const std::wstring& tip, bool on, bool textGlyph) {
        int cw = px(12) + (glyph ? px(18) : 0) + (label.empty() ? 0 : (glyph ? px(6) : 0) + textWidth(dc, textGlyph ? uiBold : uiFont, label)) + px(12);
        Hit h; h.r = {cx, qy, cx + cw, qy + qh}; h.cmd = cmd; h.kind = KChip; h.glyph = glyph ? glyph : L""; h.label = label; h.tip = tip; h.on = on;
        hits.push_back(h); cx += cw + px(4);
    };
    chip(CmdCase, nullptr, L"Aa", L"Match case", t && t->st.matchCase, true);
    chip(CmdRegex, nullptr, L".*", L"Regular expression (ECMAScript)", t && t->st.regex, true);
    chip(CmdFilter, L"", L"Filter", L"Show only matching lines (Ctrl+L)", t && t->st.filter, false);
    separators.push_back(-(cx + px(4))); cx += px(13);
    chip(CmdPrev, L"", L"", L"Previous match (Shift+F3)", false, false);
    chip(CmdNext, L"", L"", L"Next match (F3 or Enter)", false, false);
    rcQueryMsg = {cx + px(8), qy, w - px(12), qy + qh};
    ReleaseDC(gMain, dc);

    toolCount = 0;
    for (auto& h : hits) setTool(++toolCount, h.r, h.tip);
    int eh = 0;
    { HDC d2 = GetDC(gEdit); SelectObject(d2, uiFont); TEXTMETRICW tm; GetTextMetricsW(d2, &tm); eh = tm.tmHeight; ReleaseDC(gEdit, d2); }
    MoveWindow(gEdit, rcEditBox.left + px(34), rcEditBox.top + (qh - eh) / 2, rcEditBox.right - rcEditBox.left - px(42), eh, TRUE);
    MoveWindow(gView, rcView.left, rcView.top, rcView.right - rcView.left, rcView.bottom - rcView.top, TRUE);
}
void refreshChrome() { buildChrome(); InvalidateRect(gMain, nullptr, FALSE); }

void drawGlyph(HDC dc, const std::wstring& g, RECT r, COLORREF c, HFONT f = nullptr) {
    if (g.empty() || !hasGlyph(g[0])) return;
    drawText(dc, g, r, c, f ? f : iconFont, DT_CENTER);
}
std::wstring statusState(Tab& t, const DocInfo& i) {
    if (!i.exists) return i.error == ERROR_FILE_NOT_FOUND || i.error == ERROR_PATH_NOT_FOUND ? L"Waiting for file" : L"Cannot read file";
    if (i.indexing() && i.size) return L"Indexing " + std::to_wstring(int(i.scanned * 100 / i.size)) + L"%";
    if (filtering(t) && i.filterBusy() && i.complete) return L"Filtering " + std::to_wstring(int(i.filterScanned * 100 / i.complete)) + L"%";
    return t.st.follow ? L"Following" : L"Paused";
}
void paintChrome(HDC target) {
    RECT c; GetClientRect(gMain, &c);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, c.right, c.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    fill(dc, c, P.chrome);
    fill(dc, rcQuery, P.bg);
    { RECT line{0, rcQuery.bottom - 1, c.right, rcQuery.bottom}; fill(dc, line, P.border); }
    Tab* t = cur();
    for (int s : separators) {
        bool query = s < 0; int x = query ? -s : s;
        RECT r = query ? RECT{x, rcQuery.top + px(13), x + 1, rcQuery.bottom - px(13)} : RECT{x, rcCmd.top + px(13), x + 1, rcCmd.bottom - px(13)};
        fill(dc, r, P.border);
    }
    for (size_t i = 0; i < hits.size(); ++i) {
        const Hit& h = hits[i];
        bool hot = (int)i == hoverHit, pressed = hot && (int)i == pressHit;
        if (h.kind == KButton || h.kind == KChip) {
            bool disabled = !t && h.kind == KChip;
            COLORREF face = h.on ? P.accentSoft : pressed ? P.pressed : hot ? P.hover : (h.kind == KChip ? P.bg : P.chrome);
            if (h.on || hot || pressed) roundRect(dc, h.r, face, h.on ? face : face, px(8));
            COLORREF fg = disabled ? P.border : h.on ? (P.dark ? P.accent : P.accent) : P.text;
            RECT r = h.r;
            int glyphW = h.glyph.empty() ? 0 : px(18);
            if (h.label.empty()) { RECT g = r; if (h.dropdown) g.right -= px(16); drawGlyph(dc, h.glyph, g, fg); }
            else {
                int x = r.left + px(12);
                if (glyphW) { RECT g{x, r.top, x + glyphW, r.bottom}; drawGlyph(dc, h.glyph, g, fg); x += glyphW + px(h.kind == KChip ? 6 : 8); }
                RECT tr{x, r.top, r.right - (h.dropdown ? px(16) : px(4)), r.bottom};
                drawText(dc, h.label, tr, fg, h.kind == KChip && h.glyph.empty() ? uiBold : uiFont, h.kind == KChip && h.glyph.empty() ? DT_LEFT : DT_LEFT);
            }
            if (h.dropdown) { RECT d{r.right - px(22), r.top, r.right - px(6), r.bottom}; drawGlyph(dc, L"", d, P.muted, iconSmall); }
        } else if (h.kind == KTab) {
            int idx = h.cmd - CmdTabBase;
            Tab* tb = idx < (int)tabs.size() ? tabs[idx].get() : nullptr;
            RECT r = h.r;
            if (h.on) { RECT rr = r; rr.bottom += px(10); roundRect(dc, rr, P.bg, P.dark ? P.bg : P.border, px(10)); RECT cover{r.left + 1, r.bottom - 1, r.right - 1, r.bottom + px(2)}; fill(dc, cover, P.bg); }
            else if (hot) { RECT rr = r; rr.bottom -= px(4); roundRect(dc, rr, P.hover, P.hover, px(8)); }
            int x = r.left + px(12);
            bool missing = tb && !tb->doc->info().exists;
            if (tb && tb->unread) { HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(NULL_PEN)); SetDCBrushColor(dc, P.accent); int cy = (r.top + r.bottom) / 2 - (h.on ? 0 : px(2)); Ellipse(dc, x, cy - px(4), x + px(8), cy + px(4)); SelectObject(dc, ob); SelectObject(dc, op); x += px(14); }
            RECT tr{x, r.top, r.right - px(34), r.bottom - (h.on ? 0 : px(4))};
            drawText(dc, h.label + (missing ? L" (missing)" : L""), tr, h.on ? P.text : P.muted, h.on ? uiBold : uiFont);
        } else if (h.kind == KTabClose) {
            int idx = h.cmd - CmdTabCloseBase;
            bool tabHot = hoverHit >= 0 && hits[hoverHit].kind == KTab && hits[hoverHit].cmd == CmdTabBase + idx;
            if (idx == active || hot || tabHot) {
                if (hot) roundRect(dc, h.r, pressed ? P.pressed : P.hover, pressed ? P.pressed : P.hover, px(6));
                drawGlyph(dc, L"", h.r, P.muted, iconSmall);
            }
        }
    }
    // Close buttons are listed before their tabs for hit testing; repaint them over the tab background.
    for (size_t i = 0; i < hits.size(); ++i) {
        const Hit& h = hits[i];
        if (h.kind != KTabClose) continue;
        int idx = h.cmd - CmdTabCloseBase;
        bool hot = (int)i == hoverHit;
        bool tabHot = hoverHit >= 0 && hits[hoverHit].kind == KTab && hits[hoverHit].cmd == CmdTabBase + idx;
        if (idx == active || hot || tabHot) {
            if (hot) roundRect(dc, h.r, (int)i == pressHit ? P.pressed : P.hover, (int)i == pressHit ? P.pressed : P.hover, px(6));
            drawGlyph(dc, L"", h.r, hot ? P.text : P.muted, iconSmall);
        }
    }
    // Search box.
    bool focused = GetFocus() == gEdit;
    roundRect(dc, rcEditBox, P.surface, focused ? P.accent : P.border, px(8));
    if (focused) { RECT u{rcEditBox.left + px(6), rcEditBox.bottom - px(2), rcEditBox.right - px(6), rcEditBox.bottom - px(1)}; fill(dc, u, P.accent); }
    { RECT g{rcEditBox.left + px(8), rcEditBox.top, rcEditBox.left + px(30), rcEditBox.bottom}; drawGlyph(dc, L"", g, P.muted, iconSmall); }
    if (t) {
        std::wstring msg = t->queryMsg;
        COLORREF mc = P.muted;
        if (t->query && !t->query->valid) mc = P.danger;
        if (msg.empty() && filtering(*t)) {
            DocInfo i = t->doc->info();
            msg = fmtNum(i.filteredRows) + L" of " + fmtNum(i.complete) + L" lines match" + (i.filterBusy() ? L"…" : L"");
        }
        drawText(dc, msg, rcQueryMsg, mc, uiFont);
    }
    // Status bar.
    { RECT line{0, rcStatus.top, c.right, rcStatus.top + 1}; fill(dc, line, P.border); }
    {
        RECT r = rcStatus; r.left += px(12); r.right -= px(12);
        std::vector<std::pair<std::wstring, COLORREF>> segs;
        if (t) {
            DocInfo i = t->doc->info();
            std::wstring state = statusState(*t, i);
            segs.push_back({encodingName(i.encoding), P.muted});
            segs.push_back({filtering(*t) ? fmtNum(i.filteredRows) + L" of " + fmtNum(i.lines) + L" lines" : fmtNum(i.lines) + (i.lines == 1 ? L" line" : L" lines"), P.muted});
            if (i.exists) segs.push_back({fmtSize(i.size), P.muted});
            segs.push_back({state, state == L"Following" ? P.accent : (i.exists ? P.text : P.danger)});
        }
        HDC m = dc;
        int x = r.right;
        for (auto& s : segs) {
            int wv = textWidth(m, uiFont, s.first);
            RECT sr{x - wv, r.top, x, r.bottom};
            drawText(m, s.first, sr, s.second, uiFont);
            x -= wv + px(20);
        }
        RECT pr{r.left, r.top, std::max<LONG>(r.left, x - px(4)), r.bottom};
        drawText(m, t ? compactPath(m, uiFont, t->st.path, pr.right - pr.left) : std::wstring(L"No file open"), pr, P.muted, uiFont, DT_LEFT);
    }
    BitBlt(target, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
}

// ---------------------------------------------------------------- view painting

void drawRun(HDC dc, const wchar_t* s, size_t n, int x, int y, COLORREF c) {
    static std::vector<INT> dx;
    if (dx.size() < n || (!dx.empty() && dx[0] != charW)) dx.assign(std::max(n, dx.size()), charW);
    SetTextColor(dc, c);
    ExtTextOutW(dc, x, y + textOffset, 0, nullptr, s, (UINT)n, dx.data());
}
void paintView(HDC target) {
    RECT rc; GetClientRect(gView, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    fill(dc, rc, P.bg);
    SetBkMode(dc, TRANSPARENT);
    painted.clear(); emptyLinks.clear();
    Tab* t = cur();
    auto centered = [&](const std::wstring& title, const std::wstring& sub) {
        RECT a{px(24), rc.bottom / 2 - px(60), rc.right - px(24), rc.bottom / 2 - px(24)};
        drawText(dc, title, a, P.text, uiLarge, DT_CENTER);
        RECT b{px(24), a.bottom, rc.right - px(24), a.bottom + px(28)};
        drawText(dc, sub, b, P.muted, uiFont, DT_CENTER);
        return b.bottom;
    };
    if (!t) {
        int y = centered(L"Open a log file to start tailing", L"Ctrl+O to browse, or drop files here. New lines appear as they are written.");
        if (!S.recent.empty()) {
            y += px(18);
            RECT h{px(24), y, rc.right - px(24), y + px(24)};
            drawText(dc, L"Recent", h, P.muted, uiBold, DT_CENTER);
            y += px(28);
            HDC m = dc;
            for (size_t i = 0; i < S.recent.size() && i < 8; ++i) {
                std::wstring label = S.recent[i];
                int wv = std::min<int>(textWidth(m, uiFont, label), rc.right - px(48));
                RECT r{(rc.right - wv) / 2, y, (rc.right + wv) / 2, y + px(24)};
                drawText(m, compactPath(m, uiFont, label, wv), r, P.accent, uiFont, DT_CENTER);
                emptyLinks.push_back({r, (int)i});
                y += px(26);
            }
        }
    } else {
        DocInfo inf = t->doc->info();
        viewMetrics(t);
        uint64_t rows = rowCount(*t, inf);
        if (rows == 0) {
            std::wstring name = fileName(t->st.path);
            if (!inf.exists && (inf.error == ERROR_FILE_NOT_FOUND || inf.error == ERROR_PATH_NOT_FOUND)) centered(L"Waiting for " + name + L" to appear", L"BareLog keeps checking and starts showing lines as soon as the file is created.");
            else if (!inf.exists && inf.error) centered(L"Cannot open " + name, errorText(inf.error));
            else if (inf.indexing() || inf.generation == 0) centered(L"Reading " + name + L"…", L"");
            else if (filtering(*t)) centered(inf.filterBusy() ? L"Filtering…" : L"No lines match the filter", inf.filterBusy() ? L"" : L"New matching lines will appear here as they are written.");
            else centered(name + L" is empty", L"Waiting for new lines…");
        } else {
            std::vector<uint64_t> ls;
            for (uint64_t r = t->top; r < rows && ls.size() < (size_t)fullRows + 2; ++r) ls.push_back(rowLine(*t, r));
            fetch(*t, ls);
            if (S.lineNumbers) { RECT g{0, 0, gutterW, rc.bottom}; fill(dc, g, P.gutter); if (P.gutter == P.bg) { RECT sep{gutterW - 1, 0, gutterW, rc.bottom}; fill(dc, sep, P.dark ? RGB(48, 48, 48) : P.border); } }
            SelectObject(dc, monoFont);
            int64_t lo = std::min(t->anchor, t->caret), hi = std::max(t->anchor, t->caret);
            bool focused = GetFocus() == gView;
            std::vector<std::pair<size_t, size_t>> spans;
            bool search = t->query && t->query->valid && !t->query->empty();
            int y = 0, sub = S.wrap ? t->sub : 0;
            for (size_t li = 0; li < ls.size() && y < rc.bottom; ++li) {
                uint64_t line = ls[li];
                if (line == UINT64_MAX) break;
                const CacheLine& cl = t->cache[line];
                const std::wstring& s = cl.text;
                int segs = S.wrap ? std::max(1, (int)((s.size() + wrapCols - 1) / wrapCols)) : 1;
                spans.clear();
                if (search) t->query->findAll(s, spans);
                bool sel = t->caret >= 0 && (int64_t)line >= lo && (int64_t)line <= hi;
                COLORREF fg = P.text, bg = P.bg;
                bool ruleBack = false;
                if (cl.rule >= 0 && cl.rule < (int)S.rules.size()) { const Rule& R = S.rules[cl.rule]; fg = R.fore; if (R.useBack) { bg = R.back; ruleBack = true; } }
                if (sel) { bg = P.selection; if (ruleBack || cl.rule < 0) fg = P.selectionText; }
                for (int sg = sub; sg < segs && y < rc.bottom; ++sg) {
                    RECT row{gutterW, y, rc.right, y + lineH};
                    if (bg != P.bg) fill(dc, row, bg);
                    if (sel && ruleBack) { RECT edge{gutterW, y, gutterW + px(3), y + lineH}; fill(dc, edge, S.rules[cl.rule].back); }
                    if (S.lineNumbers && sg == 0) {
                        std::wstring num = std::to_wstring(line + 1);
                        drawRun(dc, num.c_str(), num.size(), gutterW - px(12) - (int)num.size() * charW, y, (int64_t)line == t->caret ? P.text : P.gutterText);
                    }
                    if ((int64_t)line == t->caret && focused) { RECT bar{std::max(0, gutterW - px(4)), y + px(1), std::max(px(3), gutterW - px(1)), y + lineH - px(1)}; if (!S.lineNumbers) bar = {0, y + px(1), px(3), y + lineH - px(1)}; fill(dc, bar, P.accent); }
                    size_t c0 = S.wrap ? (size_t)sg * wrapCols : (size_t)t->hcol;
                    size_t c1 = std::min(s.size(), c0 + (size_t)wrapCols + (S.wrap ? 0 : 2));
                    if (c0 < c1) {
                        int x = textLeft;
                        drawRun(dc, s.data() + c0, c1 - c0, x, y, fg);
                        for (auto& sp : spans) {
                            size_t a = std::max(sp.first, c0), b = std::min(sp.second, c1);
                            if (a >= b) continue;
                            RECT m{x + (int)(a - c0) * charW, y + px(1), x + (int)(b - c0) * charW, y + lineH - px(1)};
                            fill(dc, m, P.match);
                            drawRun(dc, s.data() + a, b - a, m.left, y, P.matchText);
                        }
                    }
                    painted.push_back({y, line, t->top + li});
                    y += lineH;
                }
                sub = 0;
            }
        }
    }
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
}

// ---------------------------------------------------------------- actions

void saveNow();
void scheduleSave() { SetTimer(gMain, TimerSave, 400, nullptr); }
void updateTitle() {
    Tab* t = cur();
    std::wstring title = t ? fileName(t->st.path) + L" — " + AppName : AppName;
    SetWindowTextW(gMain, title.c_str());
}
void refreshView() { updateScrollbars(); InvalidateRect(gView, nullptr, FALSE); InvalidateRect(gMain, &rcStatus, FALSE); }
void setFollow(Tab& t, bool on) {
    if (t.st.follow == on) return;
    t.st.follow = on;
    refreshChrome();
}
// After the user moves the view: following resumes only when they land on the last line.
void afterUserScroll(Tab& t, uint64_t oldTop, int oldSub) {
    if (t.top != oldTop || t.sub != oldSub) setFollow(t, atEnd(t));
    refreshView();
}
void setEditText(const std::wstring& s) { editGuard = true; SetWindowTextW(gEdit, s.c_str()); editGuard = false; }

void activateTab(int i) {
    if (i < 0 || i >= (int)tabs.size()) return;
    active = i;
    Tab& t = *tabs[i];
    t.unread = false;
    setEditText(t.st.query);
    updateTitle();
    viewMetrics(&t);
    if (t.st.follow) goEnd(t); else clampTop(t);
    refreshChrome();
    refreshView();
}
void applyQuery(Tab& t) {
    uint64_t keep = t.caret >= 0 ? (uint64_t)t.caret : (rowCount(t) ? rowLine(t, t.top) : 0);
    bool wasFiltering = filtering(t);
    t.query = t.st.query.empty() ? nullptr : Matcher::make(t.st.query, t.st.matchCase, t.st.regex);
    t.queryMsg = t.query && !t.query->valid ? L"Invalid regular expression" : L"";
    bool now = filtering(t);
    if (now || wasFiltering) {
        t.doc->setFilter(now ? t.query : nullptr);
        if (!t.st.follow) { t.pendingTop = keep; }
        t.top = 0; t.sub = 0;
        if (!now && !t.st.follow) { t.pendingTop = UINT64_MAX; ensureVisible(t, keep, true); }
    }
    if (t.st.follow) goEnd(t);
    if (&t == cur()) { refreshChrome(); refreshView(); }
    scheduleSave();
}
void queryFromEdit() {
    KillTimer(gMain, TimerQuery);
    Tab* t = cur();
    if (!t) return;
    std::wstring q = windowText(gEdit);
    if (q == t->st.query) return;
    t->st.query = q;
    applyQuery(*t);
}
void selectLine(Tab& t, uint64_t line, bool extend = false) {
    t.caret = (int64_t)line;
    if (!extend || t.anchor < 0) t.anchor = t.caret;
}
void findNext(int dir) {
    queryFromEdit();
    Tab* t = cur();
    if (!t) return;
    if (!t->query || t->query->empty()) { SetFocus(gEdit); return; }
    if (!t->query->valid) return;
    DocInfo inf = t->doc->info();
    uint64_t rows = rowCount(*t, inf);
    if (!rows) return;
    if (filtering(*t)) {
        int64_t row;
        if (t->caret < 0) row = dir > 0 ? (int64_t)t->top : (int64_t)std::min<uint64_t>(t->top + fullRows - 1, rows - 1);
        else {
            uint64_t r = t->doc->rowForLine((uint64_t)t->caret);
            bool exact = r < rows && rowLine(*t, r) == (uint64_t)t->caret;
            row = dir > 0 ? (int64_t)r + (exact ? 1 : 0) : (int64_t)r - 1;
        }
        if (row < 0) row = (int64_t)rows - 1;
        if (row >= (int64_t)rows) row = 0;
        uint64_t oldTop = t->top; int oldSub = t->sub;
        selectLine(*t, rowLine(*t, (uint64_t)row));
        ensureVisible(*t, (uint64_t)row);
        afterUserScroll(*t, oldTop, oldSub);
        return;
    }
    int64_t from;
    if (t->caret >= 0) from = t->caret + dir;
    else from = (int64_t)rowLine(*t, dir > 0 ? t->top : std::min<uint64_t>(t->top + fullRows - 1, rows - 1));
    t->findCancel = true;
    if (t->finder.joinable()) t->finder.join();
    t->findCancel = false;
    uint32_t seq = ++t->findSeq;
    t->finding = true;
    t->queryMsg = L"Searching…";
    InvalidateRect(gMain, &rcQuery, FALSE);
    auto m = t->query; LogDoc* d = t->doc.get(); uint32_t id = t->id; HWND w = gMain; std::atomic<bool>* cancel = &t->findCancel;
    uint64_t lines = inf.lines;
    t->finder = std::thread([=] {
        bool wrapped = false;
        int64_t r = from >= 0 && from < (int64_t)lines ? d->find((uint64_t)from, dir > 0, *m, *cancel) : -1;
        if (r < 0 && !*cancel) { wrapped = true; r = dir > 0 ? d->find(0, true, *m, *cancel) : d->find(lines ? lines - 1 : 0, false, *m, *cancel); }
        PostMessageW(w, WM_FINDDONE, (WPARAM)id | ((WPARAM)seq << 32) | (wrapped ? ((WPARAM)1 << 63) : 0), (LPARAM)r);
    });
}
void onFindDone(WPARAM wp, LPARAM lp) {
    uint32_t id = (uint32_t)(wp & 0xFFFFFFFF), seq = (uint32_t)((wp >> 32) & 0x7FFFFFFF);
    bool wrapped = (wp >> 63) != 0;
    Tab* t = tabById(id);
    if (!t || seq != t->findSeq) return;
    t->finding = false;
    int64_t r = (int64_t)lp;
    if (r >= 0) {
        uint64_t oldTop = t->top; int oldSub = t->sub;
        selectLine(*t, (uint64_t)r);
        if (t == cur()) viewMetrics(t);
        ensureVisible(*t, lineRow(*t, (uint64_t)r), true);
        t->queryMsg = wrapped ? L"Search wrapped around" : L"";
        if (t == cur()) { if (t->top != oldTop || t->sub != oldSub) setFollow(*t, false); refreshView(); }
    } else t->queryMsg = L"Not found";
    if (t == cur()) InvalidateRect(gMain, &rcQuery, FALSE);
}

void openFiles(const std::vector<std::wstring>& paths, bool activateLast = true);
Tab* openTab(const std::wstring& rawPath, const TabState* st = nullptr, bool activate = true) {
    wchar_t full[32768];
    DWORD n = GetFullPathNameW(rawPath.c_str(), 32768, full, nullptr);
    std::wstring path = n && n < 32768 ? std::wstring(full) : rawPath;
    for (size_t i = 0; i < tabs.size(); ++i)
        if (samePath(tabs[i]->st.path, path)) { if (activate) activateTab((int)i); return tabs[i].get(); }
    auto t = std::make_unique<Tab>();
    t->id = nextTabId++;
    if (st) t->st = *st;
    t->st.path = path;
    uint32_t id = t->id; HWND w = gMain;
    t->doc = std::make_unique<LogDoc>(path, t->st.encoding, [w, id] { PostMessageW(w, WM_DOCCHANGED, id, 0); });
    if (st && !st->follow) t->pendingTop = st->top;
    if (!t->st.query.empty()) t->query = Matcher::make(t->st.query, t->st.matchCase, t->st.regex);
    if (filtering(*t)) t->doc->setFilter(t->query);
    t->doc->start();
    Tab* raw = t.get();
    tabs.push_back(std::move(t));
    settings::addRecent(S, path);
    if (activate) activateTab((int)tabs.size() - 1); else refreshChrome();
    scheduleSave();
    return raw;
}
void openFiles(const std::vector<std::wstring>& paths, bool activateLast) {
    for (size_t i = 0; i < paths.size(); ++i) openTab(paths[i], nullptr, activateLast && i + 1 == paths.size());
}
void closeTab(int i) {
    if (i < 0 || i >= (int)tabs.size()) return;
    tabs.erase(tabs.begin() + i);
    if (tabs.empty()) { active = -1; setEditText(L""); updateTitle(); refreshChrome(); refreshView(); }
    else activateTab(std::min(i < active ? active - 1 : active, (int)tabs.size() - 1));
    scheduleSave();
}
void openDialog() {
    std::vector<wchar_t> buf(65536, L'\0');
    OPENFILENAMEW o{sizeof o};
    o.hwndOwner = gMain;
    o.lpstrFilter = L"All files (*.*)\0*.*\0Log files (*.log;*.txt;*.out;*.err)\0*.log;*.txt;*.out;*.err\0";
    o.lpstrFile = buf.data(); o.nMaxFile = (DWORD)buf.size();
    std::wstring dir = cur() ? folderOf(cur()->st.path) : (!S.recent.empty() ? folderOf(S.recent[0]) : std::wstring());
    if (!dir.empty()) o.lpstrInitialDir = dir.c_str();
    o.lpstrTitle = L"Open log files";
    o.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&o)) return;
    std::vector<std::wstring> parts;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) parts.push_back(p);
    std::vector<std::wstring> files;
    if (parts.size() == 1) files = parts;
    else for (size_t i = 1; i < parts.size(); ++i) files.push_back(parts[0] + L"\\" + parts[i]);
    openFiles(files);
}
void copySelection() {
    Tab* t = cur();
    if (!t || t->caret < 0) return;
    uint64_t lo = (uint64_t)std::min(t->anchor, t->caret), hi = (uint64_t)std::max(t->anchor, t->caret);
    std::vector<uint64_t> lines;
    if (filtering(*t)) {
        uint64_t rows = rowCount(*t);
        for (uint64_t r = t->doc->rowForLine(lo); r < rows && lines.size() < 2000000; ++r) { uint64_t l = rowLine(*t, r); if (l > hi) break; lines.push_back(l); }
    } else for (uint64_t l = lo; l <= hi && lines.size() < 2000000; ++l) lines.push_back(l);
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::wstring out;
    for (size_t i = 0; i < lines.size() && out.size() < (64u << 20); i += 20000) {
        std::vector<uint64_t> part(lines.begin() + i, lines.begin() + std::min(lines.size(), i + 20000));
        for (auto& s : t->doc->readLines(part, 1u << 20)) { out += s; out += L"\r\n"; }
    }
    if (lines.size() == 1 && out.size() >= 2) out.resize(out.size() - 2);
    SetCursor(old);
    if (!OpenClipboard(gMain)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (out.size() + 1) * sizeof(wchar_t));
    if (g) { memcpy(GlobalLock(g), out.c_str(), (out.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g); }
    CloseClipboard();
}
void copyText(const std::wstring& s) {
    if (!OpenClipboard(gMain)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (g) { memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g); }
    CloseClipboard();
}
void createMonoFont();
void zoom(int delta) {
    S.fontSize = delta ? std::max(60, std::min(360, S.fontSize + delta)) : 100;
    createMonoFont();
    if (Tab* t = cur()) { viewMetrics(t); if (t->st.follow) goEnd(*t); else clampTop(*t); }
    refreshView();
    scheduleSave();
}

// ---------------------------------------------------------------- dialogs

std::unordered_map<HWND, int> buttonKind; // 0 normal, 1 primary, 2 toggle, 3 colour swatch
std::unordered_map<HWND, bool> toggleOn;
std::unordered_map<HWND, COLORREF> swatch;

struct Dialog {
    HWND hwnd = nullptr;
    bool done = false;
    std::vector<std::pair<RECT, HWND>> outlines; // edit fields drawn as rounded boxes
    virtual ~Dialog() {}
    virtual void create() = 0;
    virtual bool command(int, int, HWND) { return false; }
    virtual void paintExtra(HDC) {}
    virtual bool drawItem(DRAWITEMSTRUCT*) { return false; }
    virtual bool measureItem(MEASUREITEMSTRUCT*) { return false; }
    HWND child(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), hwnd, (HMENU)(INT_PTR)id, gInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)uiFont, TRUE);
        return c;
    }
    HWND button(int id, const wchar_t* text, int x, int y, int w, int h, int kind = 0) {
        HWND b = child(L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, id, x, y, w, h);
        buttonKind[b] = kind;
        return b;
    }
    HWND edit(int id, int x, int y, int w, int h) {
        RECT box{px(x), px(y), px(x + w), px(y + h)};
        HDC dc = GetDC(hwnd); SelectObject(dc, uiFont); TEXTMETRICW tm; GetTextMetricsW(dc, &tm); ReleaseDC(hwnd, dc);
        HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, box.left + px(10), box.top + (box.bottom - box.top - tm.tmHeight) / 2, box.right - box.left - px(20), tm.tmHeight, hwnd, (HMENU)(INT_PTR)id, gInst, nullptr);
        SendMessageW(e, WM_SETFONT, (WPARAM)uiFont, TRUE);
        outlines.push_back({box, e});
        return e;
    }
};
void drawOwnerButton(DRAWITEMSTRUCT* d) {
    HWND b = d->hwndItem;
    int kind = buttonKind.count(b) ? buttonKind[b] : 0;
    RECT r = d->rcItem;
    bool pressed = d->itemState & ODS_SELECTED, focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT), disabled = d->itemState & ODS_DISABLED;
    fill(d->hDC, r, P.chrome);
    bool on = kind == 2 && toggleOn[b];
    COLORREF face = kind == 1 ? P.accent : on ? P.accentSoft : pressed ? P.pressed : P.surface;
    COLORREF border = focus ? P.accent : kind == 1 ? P.accent : on ? P.accentSoft : P.border;
    roundRect(d->hDC, r, face, border, px(8));
    std::wstring label = windowText(b);
    COLORREF fg = disabled ? P.muted : kind == 1 ? P.accentText : on ? P.accent : P.text;
    RECT tr = r;
    if (kind == 2) {
        RECT g{r.left + px(8), r.top, r.left + px(28), r.bottom};
        std::wstring glyph = on ? L"" : L"";
        if (hasGlyph(glyph[0])) drawText(d->hDC, glyph, g, on ? P.accent : P.muted, iconSmall, DT_CENTER);
        else drawText(d->hDC, on ? L"☑" : L"☐", g, fg, uiFont, DT_CENTER);
        tr.left = g.right + px(4);
        drawText(d->hDC, label, tr, fg, uiFont, DT_LEFT);
    } else if (kind == 3) {
        RECT sw{r.left + px(8), r.top + (r.bottom - r.top - px(18)) / 2, r.left + px(26), r.top + (r.bottom - r.top - px(18)) / 2 + px(18)};
        roundRect(d->hDC, sw, swatch[b], P.border, px(4));
        tr.left = sw.right + px(8);
        drawText(d->hDC, label, tr, fg, uiFont, DT_LEFT);
    } else drawText(d->hDC, label, tr, fg, kind == 1 ? uiBold : uiFont, DT_CENTER);
}
LRESULT CALLBACK DialogProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Dialog* d = (Dialog*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE: { auto cs = (CREATESTRUCTW*)l; d = (Dialog*)cs->lpCreateParams; d->hwnd = h; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d); break; }
    case WM_CTLCOLORSTATIC: SetTextColor((HDC)w, P.text); SetBkColor((HDC)w, P.chrome); return (LRESULT)chromeBrush;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: SetTextColor((HDC)w, P.text); SetBkColor((HDC)w, P.surface); return (LRESULT)surfaceBrush;
    case WM_MEASUREITEM: if (d && d->measureItem((MEASUREITEMSTRUCT*)l)) return TRUE; break;
    case WM_DRAWITEM: {
        auto di = (DRAWITEMSTRUCT*)l;
        if (d && d->drawItem(di)) return TRUE;
        if (di->CtlType == ODT_BUTTON) { drawOwnerButton(di); return TRUE; }
        break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT c; GetClientRect(h, &c); fill(dc, c, P.chrome);
        if (d) {
            HWND f = GetFocus();
            for (auto& o : d->outlines) {
                roundRect(dc, o.first, P.surface, f == o.second ? P.accent : P.border, px(8));
                if (f == o.second) { RECT u{o.first.left + px(6), o.first.bottom - px(2), o.first.right - px(6), o.first.bottom - px(1)}; fill(dc, u, P.accent); }
            }
            d->paintExtra(dc);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND: {
        if (!d) break;
        int id = LOWORD(w), code = HIWORD(w);
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) { for (auto& o : d->outlines) { RECT r = o.first; InflateRect(&r, 2, 2); InvalidateRect(h, &r, FALSE); } }
        if (d->command(id, code, (HWND)l)) return 0;
        if (id == IDOK || id == IDCANCEL) { d->done = true; return 0; }
        return 0;
    }
    case WM_CLOSE: if (d) d->done = true; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
void runModal(Dialog& d, const wchar_t* title, int w, int h) {
    RECT pr; GetWindowRect(gMain, &pr);
    RECT r{0, 0, px(w), px(h)};
    AdjustWindowRectEx(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int W = r.right - r.left, H = r.bottom - r.top;
    int x = pr.left + (pr.right - pr.left - W) / 2, y = pr.top + (pr.bottom - pr.top - H) / 3;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, DialogClass, title, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y, W, H, gMain, nullptr, gInst, &d);
    if (!hwnd) return;
    titleBar(hwnd);
    d.create();
    ShowWindow(hwnd, SW_SHOW);
    EnableWindow(gMain, FALSE);
    MSG msg;
    while (!d.done) {
        BOOL r2 = GetMessageW(&msg, nullptr, 0, 0);
        if (r2 <= 0) { PostQuitMessage(r2 == 0 ? (int)msg.wParam : 0); break; }
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(gMain, TRUE);
    SetActiveWindow(gMain);
    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) { buttonKind.erase(c); toggleOn.erase(c); swatch.erase(c); }
    DestroyWindow(hwnd);
}

struct PromptDialog : Dialog {
    std::wstring label, value; bool ok = false; HWND field = nullptr;
    void create() override {
        child(L"STATIC", label.c_str(), 0, -1, 16, 14, 328, 20);
        field = edit(100, 16, 40, 328, 32);
        SetWindowTextW(field, value.c_str());
        button(IDOK, L"OK", 164, 92, 86, 32, 1);
        button(IDCANCEL, L"Cancel", 258, 92, 86, 32);
        SetFocus(field);
        SendMessageW(field, EM_SETSEL, 0, -1);
    }
    bool command(int id, int, HWND) override {
        if (id == IDOK) { value = windowText(field); ok = true; done = true; return true; }
        return false;
    }
};
bool prompt(const wchar_t* title, const std::wstring& label, std::wstring& value) {
    PromptDialog d; d.label = label; d.value = value;
    runModal(d, title, 360, 140);
    if (d.ok) value = d.value;
    return d.ok;
}

void rulesChanged() {
    rebuildRuleMatchers();
    clearCaches();
    InvalidateRect(gView, nullptr, FALSE);
}
struct HighlightDialog : Dialog {
    enum { IdList = 300, IdText, IdCase, IdRegex, IdEnabled, IdFore, IdBack, IdUseBack, IdAdd, IdRemove, IdUp, IdDown };
    HWND list = nullptr, text = nullptr, bCase = nullptr, bRegex = nullptr, bEnabled = nullptr, bFore = nullptr, bBack = nullptr, bUseBack = nullptr;
    RECT preview{};
    bool loading = false;
    int sel() { return (int)SendMessageW(list, LB_GETCURSEL, 0, 0); }
    void create() override {
        child(L"STATIC", L"Rules are checked top to bottom; the first match colours the whole line.", 0, -1, 16, 12, 588, 20);
        list = child(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, IdList, 16, 40, 262, 296);
        darkmode::window(list, P.dark);
        button(IdAdd, L"Add", 16, 346, 62, 32);
        button(IdRemove, L"Remove", 84, 346, 76, 32);
        button(IdUp, L"Up", 166, 346, 54, 32);
        button(IdDown, L"Down", 226, 346, 52, 32);
        child(L"STATIC", L"Line contains", 0, -1, 298, 40, 300, 20);
        text = edit(IdText, 298, 62, 306, 32);
        bCase = button(IdCase, L"Match case", 298, 104, 116, 32, 2);
        bRegex = button(IdRegex, L"Regex", 420, 104, 86, 32, 2);
        bEnabled = button(IdEnabled, L"Enabled", 512, 104, 92, 32, 2);
        child(L"STATIC", L"Text colour", 0, -1, 298, 150, 140, 20);
        bFore = button(IdFore, L"Choose…", 298, 172, 146, 32, 3);
        child(L"STATIC", L"Background", 0, -1, 452, 150, 140, 20);
        bBack = button(IdBack, L"Choose…", 452, 172, 152, 32, 3);
        bUseBack = button(IdUseBack, L"Fill background", 452, 210, 152, 32, 2);
        child(L"STATIC", L"Preview", 0, -1, 298, 254, 140, 20);
        preview = {px(298), px(276), px(604), px(336)};
        button(IDOK, L"Done", 504, 346, 100, 32, 1);
        for (size_t i = 0; i < S.rules.size(); ++i) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)L"");
        if (!S.rules.empty()) SendMessageW(list, LB_SETCURSEL, 0, 0);
        load();
    }
    bool measureItem(MEASUREITEMSTRUCT* mi) override { if (mi->CtlType != ODT_LISTBOX) return false; mi->itemHeight = px(30); return true; }
    bool drawItem(DRAWITEMSTRUCT* di) override {
        if (di->CtlType != ODT_LISTBOX) return false;
        RECT r = di->rcItem;
        fill(di->hDC, r, P.surface);
        if (di->itemID == (UINT)-1 || di->itemID >= S.rules.size()) return true;
        const Rule& R = S.rules[di->itemID];
        RECT cell{r.left + px(4), r.top + px(2), r.right - px(4), r.bottom - px(2)};
        bool selected = di->itemState & ODS_SELECTED;
        roundRect(di->hDC, cell, R.useBack ? R.back : P.surface, selected ? P.accent : (R.useBack ? R.back : P.border), px(6));
        if (selected) { RECT bar{cell.left + px(1), cell.top + px(6), cell.left + px(4), cell.bottom - px(6)}; fill(di->hDC, bar, P.accent); }
        RECT tr{cell.left + px(12), cell.top, cell.right - px(8), cell.bottom};
        std::wstring label = R.text.empty() ? L"(empty)" : R.text;
        if (!R.enabled) label += L"  (off)";
        if (R.regex) label += L"  .*";
        drawText(di->hDC, label, tr, R.enabled ? R.fore : P.muted, monoFont, DT_LEFT);
        return true;
    }
    void paintExtra(HDC dc) override {
        int i = sel();
        if (i < 0 || i >= (int)S.rules.size()) { roundRect(dc, preview, P.bg, P.border, px(8)); return; }
        const Rule& R = S.rules[i];
        roundRect(dc, preview, R.useBack ? R.back : P.bg, P.border, px(8));
        RECT tr{preview.left + px(12), preview.top, preview.right - px(12), preview.bottom};
        bool invalid = R.regex && i < (int)ruleMatchers.size() && ruleMatchers[i] && !ruleMatchers[i]->valid;
        if (invalid) drawText(dc, L"Invalid regular expression", tr, P.danger, uiFont, DT_LEFT);
        else drawText(dc, L"12:00:01 " + (R.text.empty() ? std::wstring(L"text") : R.text) + L" sample line", tr, R.fore, monoFont, DT_LEFT);
    }
    void load() {
        loading = true;
        int i = sel();
        bool has = i >= 0 && i < (int)S.rules.size();
        for (HWND c : {text, bCase, bRegex, bEnabled, bFore, bBack, bUseBack}) EnableWindow(c, has);
        if (has) {
            const Rule& R = S.rules[i];
            SetWindowTextW(text, R.text.c_str());
            toggleOn[bCase] = R.matchCase; toggleOn[bRegex] = R.regex; toggleOn[bEnabled] = R.enabled; toggleOn[bUseBack] = R.useBack;
            swatch[bFore] = R.fore; swatch[bBack] = R.back;
        } else SetWindowTextW(text, L"");
        for (HWND c : {bCase, bRegex, bEnabled, bFore, bBack, bUseBack}) InvalidateRect(c, nullptr, FALSE);
        RECT pv = preview; InflateRect(&pv, 2, 2); InvalidateRect(hwnd, &pv, FALSE);
        loading = false;
    }
    void changed() {
        rulesChanged();
        InvalidateRect(list, nullptr, FALSE);
        RECT pv = preview; InflateRect(&pv, 2, 2); InvalidateRect(hwnd, &pv, FALSE);
    }
    bool pickColor(COLORREF& c) {
        static COLORREF custom[16] = {RGB(196, 43, 28), RGB(140, 20, 20), RGB(255, 196, 60), RGB(16, 124, 16), RGB(0, 95, 184), RGB(135, 100, 184), RGB(128, 128, 128), RGB(255, 255, 255), RGB(0, 0, 0)};
        CHOOSECOLORW cc{sizeof cc}; cc.hwndOwner = hwnd; cc.rgbResult = c; cc.lpCustColors = custom; cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (!ChooseColorW(&cc)) return false;
        c = cc.rgbResult; return true;
    }
    bool command(int id, int code, HWND) override {
        int i = sel();
        bool has = i >= 0 && i < (int)S.rules.size();
        switch (id) {
        case IdList: if (code == LBN_SELCHANGE) load(); return true;
        case IdText: if (code == EN_CHANGE && has && !loading) { S.rules[i].text = windowText(text); changed(); } return true;
        case IdCase: case IdRegex: case IdEnabled: case IdUseBack: {
            if (!has) return true;
            HWND b = GetDlgItem(hwnd, id);
            toggleOn[b] = !toggleOn[b];
            Rule& R = S.rules[i];
            if (id == IdCase) R.matchCase = toggleOn[b]; else if (id == IdRegex) R.regex = toggleOn[b]; else if (id == IdEnabled) R.enabled = toggleOn[b]; else R.useBack = toggleOn[b];
            InvalidateRect(b, nullptr, FALSE); changed(); return true;
        }
        case IdFore: case IdBack: {
            if (!has) return true;
            COLORREF& c = id == IdFore ? S.rules[i].fore : S.rules[i].back;
            if (pickColor(c)) { swatch[GetDlgItem(hwnd, id)] = c; if (id == IdBack) { S.rules[i].useBack = true; toggleOn[bUseBack] = true; InvalidateRect(bUseBack, nullptr, FALSE); } InvalidateRect(GetDlgItem(hwnd, id), nullptr, FALSE); changed(); }
            return true;
        }
        case IdAdd: {
            Rule r; r.text = cur() && !cur()->st.query.empty() ? cur()->st.query : L"";
            r.fore = RGB(0, 0, 0); r.back = RGB(255, 196, 60);
            S.rules.insert(S.rules.begin() + (has ? i + 1 : (int)S.rules.size()), r);
            int at = has ? i + 1 : (int)S.rules.size() - 1;
            SendMessageW(list, LB_INSERTSTRING, at, (LPARAM)L"");
            SendMessageW(list, LB_SETCURSEL, at, 0);
            load(); changed(); SetFocus(text); return true;
        }
        case IdRemove: {
            if (!has) return true;
            S.rules.erase(S.rules.begin() + i);
            SendMessageW(list, LB_DELETESTRING, i, 0);
            if (!S.rules.empty()) SendMessageW(list, LB_SETCURSEL, std::min(i, (int)S.rules.size() - 1), 0);
            load(); changed(); return true;
        }
        case IdUp: case IdDown: {
            int j = id == IdUp ? i - 1 : i + 1;
            if (!has || j < 0 || j >= (int)S.rules.size()) return true;
            std::swap(S.rules[i], S.rules[j]);
            SendMessageW(list, LB_SETCURSEL, j, 0);
            load(); changed(); return true;
        }
        }
        return false;
    }
};

// ---------------------------------------------------------------- menus and commands

void execute(int cmd);
int popup(HMENU m, POINT pt) {
    int r = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, gMain, nullptr);
    DestroyMenu(m);
    return r;
}
POINT below(int cmd) {
    for (auto& h : hits) if (h.cmd == cmd) { POINT p{h.r.left, h.r.bottom + px(2)}; ClientToScreen(gMain, &p); return p; }
    POINT p; GetCursorPos(&p); return p;
}
void addItem(HMENU m, int id, const std::wstring& text, bool checked = false, bool enabled = true) {
    AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, text.c_str());
}
void showRecentMenu() {
    HMENU m = CreatePopupMenu();
    if (S.recent.empty()) addItem(m, 0, L"No recent files", false, false);
    for (size_t i = 0; i < S.recent.size(); ++i) {
        std::wstring label = (i < 9 ? L"&" + std::to_wstring(i + 1) + L"  " : L"    ") + S.recent[i];
        std::wstring escaped; for (wchar_t c : label) { if (c == L'&' && escaped.size() > 1) escaped += L'&'; escaped += c; }
        addItem(m, CmdRecentBase + (int)i, escaped);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    addItem(m, CmdRecentClear, L"Clear recent files", false, !S.recent.empty());
    if (int r = popup(m, below(CmdRecent))) execute(r);
}
void showThemeMenu() {
    HMENU m = CreatePopupMenu();
    const wchar_t* names[] = {L"Use system setting", L"Light", L"Dark", L"AMOLED black"};
    for (int i = 0; i < 4; ++i) addItem(m, CmdThemeBase + i, names[i], S.theme == i);
    if (int r = popup(m, below(CmdTheme))) execute(r);
}
HMENU fileMenu(Tab* t) {
    HMENU m = CreatePopupMenu();
    addItem(m, CmdOpenEditor, L"Open in editor", false, t);
    addItem(m, CmdShowFolder, L"Show in folder", false, t);
    addItem(m, CmdCopyPath, L"Copy file path", false, t);
    return m;
}
void showMoreMenu() {
    Tab* t = cur();
    HMENU m = CreatePopupMenu();
    addItem(m, CmdGoto, L"Go to line…\tCtrl+G", false, t);
    addItem(m, CmdFind, L"Find\tCtrl+F", false, t);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    addItem(m, CmdCopy, L"Copy selected lines\tCtrl+C", false, t && t->caret >= 0);
    addItem(m, CmdSelectAll, L"Select all\tCtrl+A", false, t);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    addItem(m, CmdReload, L"Reload\tF5", false, t);
    HMENU enc = CreatePopupMenu();
    for (int e = EncAuto; e <= EncUtf16BE; ++e) addItem(enc, CmdEncodingBase + e, e == EncAuto ? L"Auto-detect" : encodingName(e), t && t->doc->encodingChoice() == e);
    AppendMenuW(m, MF_POPUP | (t ? 0 : MF_GRAYED), (UINT_PTR)enc, L"Encoding");
    HMENU ts = CreatePopupMenu();
    for (int n : {2, 4, 8}) addItem(ts, CmdTabSizeBase + n, std::to_wstring(n) + L" spaces", S.tabSize == n);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)ts, L"Tab width");
    addItem(m, CmdFont, L"Font…");
    addItem(m, CmdZoomIn, L"Zoom in\tCtrl++");
    addItem(m, CmdZoomOut, L"Zoom out\tCtrl+-");
    addItem(m, CmdZoomReset, L"Reset zoom\tCtrl+0");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    addItem(m, CmdOpenEditor, L"Open in editor", false, t);
    addItem(m, CmdShowFolder, L"Show in folder", false, t);
    addItem(m, CmdCopyPath, L"Copy file path", false, t);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    addItem(m, CmdCloseTab, L"Close tab\tCtrl+W", false, t);
    addItem(m, CmdCloseOthers, L"Close other tabs", false, tabs.size() > 1);
    addItem(m, CmdCloseAll, L"Close all tabs", false, !tabs.empty());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    addItem(m, CmdRestoreSession, L"Reopen files on startup", S.restoreSession);
    addItem(m, CmdSettingsFolder, L"Open settings folder");
    addItem(m, CmdAbout, L"About BareLog");
    addItem(m, CmdExit, L"Exit");
    POINT p = below(CmdMore);
    for (auto& h : hits) if (h.cmd == CmdMore) { p = {h.r.right, h.r.bottom + px(2)}; ClientToScreen(gMain, &p); }
    int r = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_TOPALIGN, p.x, p.y, gMain, nullptr);
    DestroyMenu(m);
    if (r) execute(r);
}
void showTabMenu(int i, POINT pt) {
    if (i < 0 || i >= (int)tabs.size()) return;
    activateTab(i);
    HMENU m = CreatePopupMenu();
    addItem(m, CmdCloseTab, L"Close\tCtrl+W");
    addItem(m, CmdCloseOthers, L"Close other tabs", false, tabs.size() > 1);
    addItem(m, CmdCloseAll, L"Close all tabs");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    addItem(m, CmdReload, L"Reload\tF5");
    addItem(m, CmdOpenEditor, L"Open in editor");
    addItem(m, CmdShowFolder, L"Show in folder");
    addItem(m, CmdCopyPath, L"Copy file path");
    if (int r = popup(m, pt)) execute(r);
}
void showViewMenu(POINT pt) {
    Tab* t = cur();
    if (!t) return;
    HMENU m = CreatePopupMenu();
    addItem(m, CmdCopy, L"Copy\tCtrl+C", false, t->caret >= 0);
    addItem(m, CmdSelectAll, L"Select all\tCtrl+A");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    addItem(m, CmdGoto, L"Go to line…\tCtrl+G");
    addItem(m, CmdFollow, L"Follow tail\tCtrl+T", t->st.follow);
    addItem(m, CmdHighlight, L"Highlighting rules…\tCtrl+H");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    addItem(m, CmdOpenEditor, L"Open in editor");
    addItem(m, CmdShowFolder, L"Show in folder");
    addItem(m, CmdCopyPath, L"Copy file path");
    if (int r = popup(m, pt)) execute(r);
}
void createMonoFont() {
    if (monoFont) DeleteObject(monoFont);
    monoFont = CreateFontW(-MulDiv(S.fontSize, dpi, 720), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, S.font.c_str());
    HDC dc = GetDC(nullptr); HGDIOBJ o = SelectObject(dc, monoFont);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    SIZE sz{}; GetTextExtentPoint32W(dc, L"0000000000", 10, &sz);
    SelectObject(dc, o); ReleaseDC(nullptr, dc);
    charW = std::max(1, (int)((sz.cx + 5) / 10));
    lineH = tm.tmHeight + px(4);
    textOffset = px(2);
}
void chooseFont() {
    LOGFONTW lf{};
    HDC screen = GetDC(nullptr);
    lf.lfHeight = -MulDiv(S.fontSize, GetDeviceCaps(screen, LOGPIXELSY), 720);
    ReleaseDC(nullptr, screen);
    wcsncpy(lf.lfFaceName, S.font.c_str(), LF_FACESIZE - 1);
    CHOOSEFONTW cf{sizeof cf};
    cf.hwndOwner = gMain; cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_FIXEDPITCHONLY | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;
    if (!ChooseFontW(&cf)) return;
    S.font = lf.lfFaceName;
    S.fontSize = std::max(60, std::min(360, cf.iPointSize));
    createMonoFont();
    if (Tab* t = cur()) { viewMetrics(t); clampTop(*t); }
    refreshView();
    scheduleSave();
}
bool systemDark() {
    DWORD v = 1, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return false;
    return v == 0;
}
void applyTheme() {
    HIGHCONTRASTW hc{sizeof hc};
    bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    bool dark = !highContrast && (S.theme == 2 || S.theme == 3 || (S.theme == 0 && systemDark()));
    P = !dark ? LightPalette : S.theme == 3 ? AmoledPalette : DarkPalette;
    darkmode::app(dark);
    titleBar(gMain);
    darkmode::window(gView, dark);
    darkmode::window(gTip, dark);
    if (surfaceBrush) DeleteObject(surfaceBrush);
    if (chromeBrush) DeleteObject(chromeBrush);
    surfaceBrush = CreateSolidBrush(P.surface);
    chromeBrush = CreateSolidBrush(P.chrome);
    SetWindowPos(gMain, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    buildChrome();
    RedrawWindow(gMain, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}
void shellOpen(Tab* t, bool folder) {
    if (!t) return;
    if (folder) {
        std::wstring args = L"/select,\"" + t->st.path + L"\"";
        ShellExecuteW(gMain, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }
    if ((INT_PTR)ShellExecuteW(gMain, L"edit", t->st.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32) {
        std::wstring args = L"\"" + t->st.path + L"\"";
        ShellExecuteW(gMain, nullptr, L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}
void gotoLine() {
    Tab* t = cur();
    if (!t) return;
    DocInfo i = t->doc->info();
    std::wstring v = t->caret >= 0 ? std::to_wstring(t->caret + 1) : L"";
    if (!prompt(L"Go to line", L"Line number (1 – " + fmtNum(i.lines) + L"):", v)) return;
    v.erase(std::remove(v.begin(), v.end(), L','), v.end());
    uint64_t n = wcstoull(v.c_str(), nullptr, 10);
    if (!n || !i.lines) return;
    uint64_t line = std::min<uint64_t>(n, i.lines) - 1;
    if (filtering(*t)) { t->st.filter = false; applyQuery(*t); }
    uint64_t oldTop = t->top; int oldSub = t->sub;
    selectLine(*t, line);
    viewMetrics(t);
    ensureVisible(*t, lineRow(*t, line), true);
    afterUserScroll(*t, oldTop, oldSub);
    SetFocus(gView);
}
void execute(int cmd) {
    Tab* t = cur();
    if (cmd >= CmdTabBase && cmd < CmdTabBase + 1000) { activateTab(cmd - CmdTabBase); return; }
    if (cmd >= CmdTabCloseBase && cmd < CmdTabCloseBase + 1000) { closeTab(cmd - CmdTabCloseBase); return; }
    if (cmd >= CmdRecentBase && cmd < CmdRecentClear) { size_t i = cmd - CmdRecentBase; if (i < S.recent.size()) { std::wstring p = S.recent[i]; openTab(p); } return; }
    if (cmd >= CmdEmptyRecentBase && cmd < CmdEmptyRecentBase + 100) { size_t i = cmd - CmdEmptyRecentBase; if (i < S.recent.size()) { std::wstring p = S.recent[i]; openTab(p); } return; }
    if (cmd >= CmdThemeBase && cmd < CmdThemeBase + 4) { S.theme = cmd - CmdThemeBase; applyTheme(); scheduleSave(); return; }
    if (cmd >= CmdEncodingBase && cmd <= CmdEncodingBase + EncUtf16BE) { if (t) { t->st.encoding = cmd - CmdEncodingBase; t->doc->setEncoding(t->st.encoding); scheduleSave(); } return; }
    if (cmd >= CmdTabSizeBase && cmd < CmdTabSizeBase + 17) { S.tabSize = cmd - CmdTabSizeBase; clearCaches(); refreshView(); scheduleSave(); return; }
    switch (cmd) {
    case CmdOpen: case CmdAddTab: openDialog(); break;
    case CmdRecent: showRecentMenu(); break;
    case CmdRecentClear: S.recent.clear(); scheduleSave(); InvalidateRect(gView, nullptr, FALSE); break;
    case CmdTheme: showThemeMenu(); break;
    case CmdMore: showMoreMenu(); break;
    case CmdFollow: if (t) { setFollow(*t, !t->st.follow); if (t->st.follow) { viewMetrics(t); goEnd(*t); } refreshView(); scheduleSave(); } break;
    case CmdWrap: S.wrap = !S.wrap; if (t) { t->sub = 0; viewMetrics(t); if (t->st.follow) goEnd(*t); else clampTop(*t); } refreshChrome(); refreshView(); scheduleSave(); break;
    case CmdLineNumbers: S.lineNumbers = !S.lineNumbers; if (t) { viewMetrics(t); if (t->st.follow) goEnd(*t); } refreshChrome(); refreshView(); scheduleSave(); break;
    case CmdHighlight: { HighlightDialog d; runModal(d, L"Highlighting rules", 620, 394); rulesChanged(); saveNow(); break; }
    case CmdCase: if (t) { queryFromEdit(); t->st.matchCase = !t->st.matchCase; applyQuery(*t); } break;
    case CmdRegex: if (t) { queryFromEdit(); t->st.regex = !t->st.regex; applyQuery(*t); } break;
    case CmdFilter: if (t) { queryFromEdit(); t->st.filter = !t->st.filter; applyQuery(*t); if (t->st.filter && t->st.query.empty()) SetFocus(gEdit); } break;
    case CmdPrev: findNext(-1); break;
    case CmdNext: findNext(1); break;
    case CmdFind: SetFocus(gEdit); SendMessageW(gEdit, EM_SETSEL, 0, -1); break;
    case CmdGoto: gotoLine(); break;
    case CmdCloseTab: closeTab(active); break;
    case CmdCloseOthers: if (t) { auto keep = std::move(tabs[active]); tabs.clear(); tabs.push_back(std::move(keep)); activateTab(0); scheduleSave(); } break;
    case CmdCloseAll: tabs.clear(); active = -1; setEditText(L""); updateTitle(); refreshChrome(); refreshView(); scheduleSave(); break;
    case CmdNextTab: if (!tabs.empty()) activateTab((active + 1) % (int)tabs.size()); break;
    case CmdPrevTab: if (!tabs.empty()) activateTab((active + (int)tabs.size() - 1) % (int)tabs.size()); break;
    case CmdReload: if (t) t->doc->reload(); break;
    case CmdZoomIn: zoom(10); break;
    case CmdZoomOut: zoom(-10); break;
    case CmdZoomReset: zoom(0); break;
    case CmdFont: chooseFont(); break;
    case CmdCopy: copySelection(); break;
    case CmdSelectAll: if (t) { uint64_t rows = rowCount(*t); if (rows) { t->anchor = (int64_t)rowLine(*t, 0); t->caret = (int64_t)rowLine(*t, rows - 1); InvalidateRect(gView, nullptr, FALSE); } } break;
    case CmdOpenEditor: shellOpen(t, false); break;
    case CmdShowFolder: shellOpen(t, true); break;
    case CmdCopyPath: if (t) copyText(t->st.path); break;
    case CmdRestoreSession: S.restoreSession = !S.restoreSession; scheduleSave(); break;
    case CmdSettingsFolder: { std::wstring args = L"/select,\"" + gSettingsPath + L"\""; saveNow(); ShellExecuteW(gMain, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL); break; }
    case CmdAbout: {
        std::wstring msg = L"BareLog 1.0\nA fast, native real-time log viewer for Windows.\n\nSettings and session are saved to:\n" + gSettingsPath;
        MessageBoxW(gMain, msg.c_str(), L"About BareLog", MB_OK | MB_ICONINFORMATION);
        break;
    }
    case CmdExit: PostMessageW(gMain, WM_CLOSE, 0, 0); break;
    case CmdTabsOverflow: {
        HMENU m = CreatePopupMenu();
        for (size_t i = 0; i < tabs.size(); ++i) addItem(m, CmdTabBase + (int)i, fileName(tabs[i]->st.path) + L"\t" + folderOf(tabs[i]->st.path), (int)i == active);
        POINT p = below(CmdTabsOverflow);
        if (int r = popup(m, p)) execute(r);
        break;
    }
    }
}

// ---------------------------------------------------------------- settings sync

void saveNow() {
    KillTimer(gMain, TimerSave);
    S.tabs.clear();
    if (S.restoreSession) {
        for (auto& t : tabs) {
            TabState st = t->st;
            uint64_t rows = rowCount(*t);
            st.top = t->pendingTop != UINT64_MAX ? t->pendingTop : (rows ? rowLine(*t, std::min(t->top, rows - 1)) : 0);
            if (st.top == UINT64_MAX) st.top = 0;
            st.encoding = t->doc->encodingChoice();
            S.tabs.push_back(st);
        }
        S.activeTab = std::max(0, active);
    }
    WINDOWPLACEMENT wp{sizeof wp};
    if (gMain && GetWindowPlacement(gMain, &wp) && IsWindowVisible(gMain)) {
        S.window = wp.rcNormalPosition; S.maximized = wp.showCmd == SW_SHOWMAXIMIZED; S.haveWindow = true;
    }
    settings::save(gSettingsPath, S);
}

// ---------------------------------------------------------------- window procedures

bool dragging = false;
int wheelRemainder = 0;
int hitAt(POINT p) { for (size_t i = 0; i < hits.size(); ++i) if (PtInRect(&hits[i].r, p)) return (int)i; return -1; }
const PaintedRow* paintedAt(int y) {
    if (painted.empty()) return nullptr;
    if (y < 0) return &painted.front();
    for (size_t i = 0; i < painted.size(); ++i) if (y < painted[i].y + lineH) return &painted[i];
    return &painted.back();
}
void moveCaret(Tab& t, int64_t delta, bool extend) {
    uint64_t rows = rowCount(t);
    if (!rows) return;
    int64_t row;
    if (t.caret < 0) row = (int64_t)t.top;
    else { uint64_t r = lineRow(t, (uint64_t)t.caret); row = (int64_t)std::min(r, rows - 1) + delta; }
    row = std::max<int64_t>(0, std::min<int64_t>(row, (int64_t)rows - 1));
    uint64_t oldTop = t.top; int oldSub = t.sub;
    selectLine(t, rowLine(t, (uint64_t)row), extend);
    ensureVisible(t, (uint64_t)row);
    if (row == (int64_t)rows - 1 && !t.st.follow) setFollow(t, true);
    else if (t.top != oldTop || t.sub != oldSub) setFollow(t, atEnd(t));
    refreshView();
}

LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Tab* t = cur();
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paintView(dc); EndPaint(h, &ps); return 0; }
    case WM_SIZE: if (t) { viewMetrics(t); if (t->st.follow) goEnd(*t); else clampTop(*t); } updateScrollbars(); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_VSCROLL: {
        if (!t) return 0;
        uint64_t oldTop = t->top; int oldSub = t->sub, page = std::max(1, fullRows - 1);
        uint64_t rows = rowCount(*t);
        switch (LOWORD(w)) {
        case SB_LINEUP: scrollBy(*t, -1); break;
        case SB_LINEDOWN: scrollBy(*t, 1); break;
        case SB_PAGEUP: scrollBy(*t, -page); break;
        case SB_PAGEDOWN: scrollBy(*t, page); break;
        case SB_TOP: t->top = 0; t->sub = 0; break;
        case SB_BOTTOM: goEnd(*t); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: {
            SCROLLINFO si{sizeof si, SIF_TRACKPOS | SIF_RANGE | SIF_PAGE}; GetScrollInfo(h, SB_VERT, &si);
            double f = rows > ScrollMax ? double(rows) / double(ScrollMax) : 1.0;
            t->top = (uint64_t)(si.nTrackPos * f); t->sub = 0;
            if (si.nTrackPos + (int)si.nPage > si.nMax) goEnd(*t); else clampTop(*t);
            break;
        }
        }
        afterUserScroll(*t, oldTop, oldSub);
        return 0;
    }
    case WM_HSCROLL: {
        if (!t) return 0;
        SCROLLINFO si{sizeof si, SIF_ALL}; GetScrollInfo(h, SB_HORZ, &si);
        int pos = t->hcol, page = std::max(1, (int)si.nPage);
        switch (LOWORD(w)) {
        case SB_LINELEFT: pos -= 4; break; case SB_LINERIGHT: pos += 4; break;
        case SB_PAGELEFT: pos -= page; break; case SB_PAGERIGHT: pos += page; break;
        case SB_LEFT: pos = 0; break; case SB_RIGHT: pos = si.nMax; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        }
        t->hcol = std::max(0, std::min(pos, std::max(0, si.nMax - page + 1)));
        refreshView();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (GetKeyState(VK_CONTROL) < 0) { zoom(GET_WHEEL_DELTA_WPARAM(w) > 0 ? 10 : -10); return 0; }
        if (!t) return 0;
        UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        wheelRemainder += GET_WHEEL_DELTA_WPARAM(w);
        int steps = wheelRemainder / (WHEEL_DELTA / std::max(1, (int)std::min(lines, 20u)));
        wheelRemainder -= steps * (WHEEL_DELTA / std::max(1, (int)std::min(lines, 20u)));
        if (lines == WHEEL_PAGESCROLL) steps = (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1) * fullRows, wheelRemainder = 0;
        if (!steps) return 0;
        uint64_t oldTop = t->top; int oldSub = t->sub;
        scrollBy(*t, -steps);
        afterUserScroll(*t, oldTop, oldSub);
        return 0;
    }
    case WM_MOUSEHWHEEL: if (t && !S.wrap) { t->hcol = std::max(0, t->hcol + GET_WHEEL_DELTA_WPARAM(w) / 30); updateScrollbars(); SCROLLINFO si{sizeof si, SIF_ALL}; GetScrollInfo(h, SB_HORZ, &si); t->hcol = std::min(t->hcol, std::max(0, si.nMax - (int)si.nPage + 1)); refreshView(); } return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        SetFocus(h);
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (!t) { for (auto& e : emptyLinks) if (PtInRect(&e.first, p)) { execute(CmdEmptyRecentBase + e.second); return 0; } return 0; }
        if (auto r = paintedAt(p.y)) { selectLine(*t, r->line, GetKeyState(VK_SHIFT) < 0); dragging = true; SetCapture(h); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (!t) { bool over = false; for (auto& e : emptyLinks) if (PtInRect(&e.first, p)) over = true; SetCursor(LoadCursor(nullptr, over ? IDC_HAND : IDC_ARROW)); return 0; }
        if (!dragging) return 0;
        if (p.y < 0 || p.y >= viewH) SetTimer(h, TimerAutoScroll, 40, nullptr); else KillTimer(h, TimerAutoScroll);
        if (auto r = paintedAt(p.y)) { if ((int64_t)r->line != t->caret) { t->caret = (int64_t)r->line; InvalidateRect(h, nullptr, FALSE); } }
        return 0;
    }
    case WM_TIMER:
        if (w == TimerAutoScroll && t && dragging) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            uint64_t oldTop = t->top; int oldSub = t->sub;
            scrollBy(*t, p.y < 0 ? -1 - (-p.y / lineH) : 1 + ((p.y - viewH) / lineH));
            UpdateWindow(h);
            if (auto r = paintedAt(p.y)) t->caret = (int64_t)r->line;
            afterUserScroll(*t, oldTop, oldSub);
        }
        return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED: if (dragging) { dragging = false; KillTimer(h, TimerAutoScroll); if (m == WM_LBUTTONUP) ReleaseCapture(); } return 0;
    case WM_RBUTTONDOWN: {
        SetFocus(h);
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (t) if (auto r = paintedAt(p.y)) {
            int64_t lo = std::min(t->anchor, t->caret), hi = std::max(t->anchor, t->caret);
            if (t->caret < 0 || (int64_t)r->line < lo || (int64_t)r->line > hi) { selectLine(*t, r->line); InvalidateRect(h, nullptr, FALSE); }
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (p.x == -1 && p.y == -1) { p = {px(40), px(40)}; ClientToScreen(h, &p); }
        showViewMenu(p);
        return 0;
    }
    case WM_KEYDOWN: {
        if (!t) return 0;
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        int page = std::max(1, fullRows - 1);
        switch (w) {
        case VK_UP: moveCaret(*t, -1, shift); return 0;
        case VK_DOWN: moveCaret(*t, 1, shift); return 0;
        case VK_PRIOR: moveCaret(*t, -page, shift); return 0;
        case VK_NEXT: moveCaret(*t, page, shift); return 0;
        case VK_HOME: { uint64_t rows = rowCount(*t); if (rows) { selectLine(*t, rowLine(*t, 0), shift); t->top = 0; t->sub = 0; setFollow(*t, rows <= (uint64_t)fullRows && t->st.follow); refreshView(); } return 0; }
        case VK_END: { uint64_t rows = rowCount(*t); if (rows) { selectLine(*t, rowLine(*t, rows - 1), shift); goEnd(*t); setFollow(*t, true); refreshView(); } return 0; }
        case VK_LEFT: case VK_RIGHT: SendMessageW(h, WM_HSCROLL, w == VK_LEFT ? (ctrl ? SB_PAGELEFT : SB_LINELEFT) : (ctrl ? SB_PAGERIGHT : SB_LINERIGHT), 0); return 0;
        case VK_ESCAPE: t->anchor = t->caret = -1; InvalidateRect(h, nullptr, FALSE); return 0;
        case 'C': if (ctrl) copySelection(); return 0;
        case 'A': if (ctrl) execute(CmdSelectAll); return 0;
        case VK_INSERT: if (ctrl) copySelection(); return 0;
        }
        return 0;
    }
    case WM_CHAR:
        // Typing in the log view starts a search.
        if (w >= 0x20 && w != 0x7F && GetKeyState(VK_CONTROL) >= 0) { SetFocus(gEdit); SendMessageW(gEdit, EM_SETSEL, 0, -1); SendMessageW(gEdit, WM_CHAR, w, l); }
        return 0;
    case WM_DROPFILES: return SendMessageW(gMain, m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (w == VK_RETURN) { findNext(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); return 0; }
        if (w == VK_ESCAPE) { if (GetWindowTextLengthW(h)) SetWindowTextW(h, L""); else SetFocus(gView); return 0; }
        if (w == VK_DOWN) { SetFocus(gView); return 0; }
        if (w == 'A' && ctrl) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    }
    if (m == WM_CHAR && (w == L'\r' || w == 27 || w == 1)) return 0;
    return DefSubclassProc(h, m, w, l);
}

void createFonts() {
    for (HFONT* f : {&uiFont, &uiBold, &uiLarge, &iconFont, &iconSmall}) if (*f) { DeleteObject(*f); *f = nullptr; }
    auto make = [](int height, int weight, const wchar_t* face) {
        return CreateFontW(height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    uiFont = make(-MulDiv(9, dpi, 72), FW_NORMAL, L"Segoe UI");
    uiBold = make(-MulDiv(9, dpi, 72), FW_SEMIBOLD, L"Segoe UI");
    uiLarge = make(-MulDiv(15, dpi, 72), FW_SEMIBOLD, L"Segoe UI");
    iconFont = make(-px(16), FW_NORMAL, iconFace.c_str());
    iconSmall = make(-px(12), FW_NORMAL, iconFace.c_str());
    createMonoFont();
    if (gEdit) SendMessageW(gEdit, WM_SETFONT, (WPARAM)uiFont, TRUE);
}
void pickIconFace() {
    HDC dc = GetDC(nullptr);
    HFONT f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe Fluent Icons");
    HGDIOBJ o = SelectObject(dc, f);
    wchar_t face[LF_FACESIZE]{}; GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, o); DeleteObject(f); ReleaseDC(nullptr, dc);
    iconFace = wcscmp(face, L"Segoe Fluent Icons") == 0 ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
}

void onDocChanged(uint32_t id) {
    Tab* t = tabById(id);
    if (!t) return;
    t->doc->acknowledge();
    DocInfo inf = t->doc->info();
    bool isCur = t == cur();
    if (inf.generation != t->generation) {
        bool restarted = t->generation != UINT64_MAX;
        t->generation = inf.generation;
        t->cache.clear(); t->maxCols = 0;
        if (restarted) { t->anchor = t->caret = -1; t->top = 0; t->sub = 0; t->hcol = 0; t->lastLines = 0; }
    } else {
        if (t->lastLines) t->cache.erase(t->lastLines - 1);
        if (inf.lines) t->cache.erase(inf.lines - 1);
    }
    bool grew = inf.lines > t->lastLines;
    bool wasLoaded = t->loaded;
    if (!inf.indexing() && inf.generation) t->loaded = true;
    t->lastLines = inf.lines;
    if (t->pendingTop != UINT64_MAX) {
        bool ready = filtering(*t) ? (!inf.indexing() && !inf.filterBusy()) : (inf.lines > t->pendingTop || !inf.indexing());
        if (ready) {
            uint64_t want = t->pendingTop;
            t->pendingTop = UINT64_MAX;
            if (!t->st.follow) {
                if (isCur) viewMetrics(t);
                uint64_t row = lineRow(*t, want);
                t->top = row; t->sub = 0;
                if (t->caret >= 0 && isCur) ensureVisible(*t, lineRow(*t, (uint64_t)t->caret)); else clampTop(*t);
            }
        }
    }
    if (isCur) {
        viewMetrics(t);
        if (t->st.follow) goEnd(*t);
        else if (t->top >= rowCount(*t, inf)) clampTop(*t);
        refreshView();
        InvalidateRect(gMain, &rcQuery, FALSE);
        InvalidateRect(gMain, &rcTabs, FALSE);
    } else if (grew && wasLoaded && !t->unread) {
        t->unread = true;
        InvalidateRect(gMain, &rcTabs, FALSE);
    }
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        gMain = h;
        gTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, h, nullptr, gInst, nullptr);
        SendMessageW(gTip, TTM_SETMAXTIPWIDTH, 0, 600);
        gView = CreateWindowExW(0, ViewClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 0, 0, 0, 0, h, (HMENU)(INT_PTR)101, gInst, nullptr);
        gEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, h, (HMENU)(INT_PTR)QueryEditId, gInst, nullptr);
        SendMessageW(gEdit, WM_SETFONT, (WPARAM)uiFont, TRUE);
        SendMessageW(gEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Find or filter   (Ctrl+F)");
        SendMessageW(gEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SetWindowSubclass(gEdit, EditSubclass, 1, 0);
        DragAcceptFiles(h, TRUE);
        DragAcceptFiles(gView, TRUE);
        applyTheme();
        return 0;
    }
    case WM_SIZE: buildChrome(); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: { auto mm = (MINMAXINFO*)l; mm->ptMinTrackSize = {px(560), px(340)}; return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paintChrome(dc); EndPaint(h, &ps); return 0; }
    case WM_DPICHANGED: {
        dpi = HIWORD(w);
        createFonts();
        auto r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        buildChrome();
        refreshView();
        return 0;
    }
    case WM_SETFOCUS: SetFocus(gView); return 0;
    case WM_CTLCOLOREDIT: SetTextColor((HDC)w, P.text); SetBkColor((HDC)w, P.surface); return (LRESULT)surfaceBrush;
    case WM_COMMAND:
        if (LOWORD(w) == QueryEditId) {
            int code = HIWORD(w);
            if (code == EN_CHANGE && !editGuard) SetTimer(h, TimerQuery, 220, nullptr);
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) { RECT r = rcEditBox; InflateRect(&r, 2, 2); InvalidateRect(h, &r, FALSE); }
            return 0;
        }
        execute(LOWORD(w));
        return 0;
    case WM_TIMER:
        if (w == TimerQuery) queryFromEdit();
        else if (w == TimerSave) saveNow();
        return 0;
    case WM_MOUSEMOVE: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        int hit = hitAt(p);
        if (hit != hoverHit) {
            hoverHit = hit;
            InvalidateRect(h, &rcCmd, FALSE); InvalidateRect(h, &rcTabs, FALSE); InvalidateRect(h, &rcQuery, FALSE);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0}; TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE: if (hoverHit >= 0) { hoverHit = -1; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        int hit = hitAt(p);
        if (hit >= 0 && hits[hit].kind == KTab) { activateTab(hits[hit].cmd - CmdTabBase); return 0; }
        pressHit = hit;
        if (hit >= 0) { SetCapture(h); InvalidateRect(h, &hits[hit].r, FALSE); }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (PtInRect(&rcTabs, p) && hitAt(p) < 0) openDialog();
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        int hit = hitAt(p), pressed = pressHit;
        pressHit = -1;
        if (GetCapture() == h) ReleaseCapture();
        InvalidateRect(h, nullptr, FALSE);
        if (hit >= 0 && hit == pressed) execute(hits[hit].cmd);
        return 0;
    }
    case WM_MBUTTONUP: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        int hit = hitAt(p);
        if (hit >= 0 && (hits[hit].kind == KTab || hits[hit].kind == KTabClose)) closeTab(hits[hit].kind == KTab ? hits[hit].cmd - CmdTabBase : hits[hit].cmd - CmdTabCloseBase);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        int hit = hitAt(p);
        if (hit >= 0 && hits[hit].kind == KTab) { POINT s = p; ClientToScreen(h, &s); showTabMenu(hits[hit].cmd - CmdTabBase, s); }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)w;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> files;
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring f(len + 1, L'\0'); DragQueryFileW(drop, i, &f[0], len + 1); f.resize(len);
            DWORD attr = GetFileAttributesW(f.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(f);
        }
        DragFinish(drop);
        openFiles(files);
        SetForegroundWindow(h);
        return 0;
    }
    case WM_COPYDATA: {
        auto cd = (COPYDATASTRUCT*)l;
        if (cd->dwData != CopyDataOpen) return FALSE;
        std::wstring all((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        std::vector<std::wstring> files;
        size_t pos = 0;
        while (pos < all.size()) { size_t nl = all.find(L'\n', pos); if (nl == std::wstring::npos) nl = all.size(); if (nl > pos) files.push_back(all.substr(pos, nl - pos)); pos = nl + 1; }
        openFiles(files);
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        return TRUE;
    }
    case WM_DOCCHANGED: onDocChanged((uint32_t)w); return 0;
    case WM_FINDDONE: onFindDone(w, l); return 0;
    case WM_SETTINGCHANGE:
        if (l && wcscmp((const wchar_t*)l, L"ImmersiveColorSet") == 0 && S.theme == 0) applyTheme();
        return 0;
    case WM_SYSCOLORCHANGE: case WM_THEMECHANGED: applyTheme(); return 0;
    case WM_TESTQUERY: {
        Tab* t = cur();
        switch (w) {
        case 0: return (LRESULT)tabs.size();
        case 1: return t ? (LRESULT)t->doc->info().lines : -1;
        case 2: return t ? t->st.follow : -1;
        case 3: return t ? (LRESULT)rowCount(*t) : -1;
        case 4: return t ? (LRESULT)t->top : -1;
        case 5: return active;
        case 6: return P.dark ? (S.theme == 3 ? 2 : 1) : 0;
        case 7: return t ? (t->doc->info().filterBusy() ? 0 : 1) : -1;
        case 8: return t ? (LRESULT)t->caret : -1;
        case 9: return S.wrap;
        case 10: return t ? (LRESULT)(t->finding ? 1 : 0) : -1;
        case 11: return t ? (LRESULT)t->unread : -1;
        case 12: return (LRESULT)fullRows;
        case 13: saveNow(); return 1;
        }
        return -1;
    }
    case WM_QUERYENDSESSION: saveNow(); return TRUE;
    case WM_CLOSE: saveNow(); DestroyWindow(h); return 0;
    case WM_DESTROY: tabs.clear(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- startup

std::wstring instanceKey() {
    std::wstring p = gSettingsPath;
    CharLowerBuffW(&p[0], (DWORD)p.size());
    uint32_t hsh = 2166136261u;
    for (wchar_t c : p) { hsh ^= (uint32_t)c; hsh *= 16777619u; }
    wchar_t buf[16]; swprintf(buf, 16, L"%08X", hsh);
    return buf;
}
HWND findInstance(const std::wstring& key) {
    struct Ctx { std::wstring key; HWND found; } ctx{key, nullptr};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto c = (Ctx*)lp;
        wchar_t cls[64]; GetClassNameW(h, cls, 64);
        if (wcscmp(cls, MainClass) != 0) return TRUE;
        wchar_t k[32]{};
        HANDLE v = GetPropW(h, L"BareLog.Instance");
        if (v) { swprintf(k, 32, L"%08X", (unsigned)(UINT_PTR)v); if (c->key == k) { c->found = h; return FALSE; } }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    gInst = inst;
    // The manifest declares PerMonitorV2; this covers launches where it is ignored.
    using SetDpiCtxFn = BOOL(WINAPI*)(HANDLE);
    if (auto f = (SetDpiCtxFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) f((HANDLE)(INT_PTR)-4);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> files;
    for (int i = 1; i < argc; ++i) {
        wchar_t full[32768];
        DWORD n = GetFullPathNameW(argv[i], 32768, full, nullptr);
        files.push_back(n && n < 32768 ? full : argv[i]);
    }
    LocalFree(argv);

    gSettingsPath = settings::defaultPath();
    std::wstring key = instanceKey();
    HANDLE mutex = CreateMutexW(nullptr, FALSE, (L"Local\\BareLog-" + key).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = nullptr;
        for (int i = 0; i < 40 && !other; ++i) { other = findInstance(key); if (!other) Sleep(50); }
        if (other) {
            std::wstring payload;
            for (auto& f : files) { payload += f; payload += L'\n'; }
            DWORD pid = 0; GetWindowThreadProcessId(other, &pid); AllowSetForegroundWindow(pid);
            COPYDATASTRUCT cd{CopyDataOpen, (DWORD)(payload.size() * sizeof(wchar_t)), (void*)payload.c_str()};
            DWORD_PTR result = 0;
            SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &result);
            if (mutex) CloseHandle(mutex);
            return 0;
        }
    }

    settings::load(gSettingsPath, S);
    rebuildRuleMatchers();
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    darkmode::init();
    pickIconFace();
    {
        HDC screen = GetDC(nullptr);
        dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(nullptr, screen);
    }
    createFonts();

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = MainProc; wc.hInstance = inst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon; wc.lpszClassName = MainClass;
    RegisterClassExW(&wc);
    WNDCLASSEXW vc{sizeof vc};
    vc.style = CS_DBLCLKS; vc.lpfnWndProc = ViewProc; vc.hInstance = inst; vc.hCursor = LoadCursor(nullptr, IDC_IBEAM); vc.lpszClassName = ViewClass;
    RegisterClassExW(&vc);
    WNDCLASSEXW dc{sizeof dc};
    dc.lpfnWndProc = DialogProc; dc.hInstance = inst; dc.hCursor = LoadCursor(nullptr, IDC_ARROW); dc.lpszClassName = DialogClass; dc.hIcon = wc.hIcon;
    RegisterClassExW(&dc);

    HWND h = CreateWindowExW(0, MainClass, AppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, px(1200), px(780), nullptr, nullptr, inst, nullptr);
    if (!h) return 1;
    SetPropW(h, L"BareLog.Instance", (HANDLE)(UINT_PTR)wcstoul(key.c_str(), nullptr, 16));
    {
        UINT wdpi = 0;
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        if (auto f = (GetDpiForWindowFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")) wdpi = f(h);
        if (wdpi && (int)wdpi != dpi) { dpi = (int)wdpi; createFonts(); }
    }
    int showCmd = show == SW_HIDE ? SW_HIDE : (show == SW_SHOWMINIMIZED || show == SW_MINIMIZE || show == SW_SHOWMINNOACTIVE) ? show : SW_SHOWNORMAL;
    if (S.haveWindow && MonitorFromRect(&S.window, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT wp{sizeof wp};
        wp.rcNormalPosition = S.window;
        wp.showCmd = showCmd == SW_SHOWNORMAL && S.maximized ? SW_SHOWMAXIMIZED : showCmd;
        SetWindowPlacement(h, &wp);
    } else ShowWindow(h, showCmd);
    UpdateWindow(h);

    if (S.restoreSession) {
        auto saved = S.tabs;
        for (auto& st : saved) openTab(st.path, &st, false);
        if (!tabs.empty()) activateTab(std::min(S.activeTab, (int)tabs.size() - 1));
    }
    openFiles(files);
    refreshChrome();
    refreshView();

    ACCEL acc[] = {
        {FCONTROL | FVIRTKEY, 'O', CmdOpen}, {FCONTROL | FVIRTKEY, 'W', CmdCloseTab}, {FCONTROL | FVIRTKEY, VK_F4, CmdCloseTab},
        {FCONTROL | FVIRTKEY, 'F', CmdFind}, {FVIRTKEY, VK_F3, CmdNext}, {FSHIFT | FVIRTKEY, VK_F3, CmdPrev},
        {FCONTROL | FVIRTKEY, 'L', CmdFilter}, {FCONTROL | FVIRTKEY, 'G', CmdGoto}, {FVIRTKEY, VK_F5, CmdReload},
        {FCONTROL | FVIRTKEY, VK_TAB, CmdNextTab}, {FCONTROL | FSHIFT | FVIRTKEY, VK_TAB, CmdPrevTab},
        {FCONTROL | FVIRTKEY, VK_OEM_PLUS, CmdZoomIn}, {FCONTROL | FVIRTKEY, VK_ADD, CmdZoomIn},
        {FCONTROL | FVIRTKEY, VK_OEM_MINUS, CmdZoomOut}, {FCONTROL | FVIRTKEY, VK_SUBTRACT, CmdZoomOut},
        {FCONTROL | FVIRTKEY, '0', CmdZoomReset}, {FCONTROL | FVIRTKEY, VK_NUMPAD0, CmdZoomReset},
        {FCONTROL | FVIRTKEY, 'T', CmdFollow}, {FALT | FVIRTKEY, 'Z', CmdWrap}, {FCONTROL | FVIRTKEY, 'H', CmdHighlight},
    };
    HACCEL accel = CreateAcceleratorTableW(acc, (int)(sizeof acc / sizeof acc[0]));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if ((msg.hwnd == gMain || IsChild(gMain, msg.hwnd)) && TranslateAcceleratorW(gMain, accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyAcceleratorTable(accel);
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}

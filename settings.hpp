// BareLog settings and session, stored as a UTF-8 text file (not the registry):
//   %APPDATA%\BareLog\settings.ini, or the path in the BARELOG_SETTINGS environment variable.
#pragma once
#include "logcore.hpp"
#include <shlobj.h>
#include <cstdio>

namespace settings {

struct Rule {
    std::wstring text;
    bool matchCase = false, regex = false, enabled = true, useBack = true;
    COLORREF fore = RGB(255, 255, 255), back = RGB(196, 43, 28);
};
struct TabState {
    std::wstring path, query;
    bool follow = true, matchCase = false, regex = false, filter = false;
    uint64_t top = 0;
    int encoding = 0;
};
struct Settings {
    int theme = 0; // 0 system, 1 light, 2 dark, 3 AMOLED black
    std::wstring font = L"Consolas";
    int fontSize = 100; // tenths of a point
    int tabSize = 4;
    bool wrap = false, lineNumbers = true, restoreSession = true;
    bool haveWindow = false, maximized = false;
    RECT window{};
    std::vector<Rule> rules;
    std::vector<std::wstring> recent;
    std::vector<TabState> tabs;
    int activeTab = 0;
};
constexpr size_t MaxRecent = 15;

inline std::vector<Rule> defaultRules() {
    auto rule = [](const wchar_t* t, COLORREF fore, COLORREF back, bool useBack) { Rule r; r.text = t; r.fore = fore; r.back = back; r.useBack = useBack; return r; };
    return {
        rule(L"FATAL", RGB(255, 255, 255), RGB(140, 20, 20), true),
        rule(L"ERROR", RGB(255, 255, 255), RGB(196, 43, 28), true),
        rule(L"Exception", RGB(255, 255, 255), RGB(196, 43, 28), true),
        rule(L"WARN", RGB(0, 0, 0), RGB(255, 196, 60), true),
        rule(L"DEBUG", RGB(128, 128, 128), RGB(0, 0, 0), false),
    };
}

inline std::wstring defaultPath() {
    wchar_t env[32768];
    DWORD n = GetEnvironmentVariableW(L"BARELOG_SETTINGS", env, 32768);
    if (n && n < 32768) return env;
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) dir = appData;
    CoTaskMemFree(appData);
    if (dir.empty()) { wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); dir = tmp; }
    return dir + L"\\BareLog\\settings.ini";
}

inline std::string toUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}
inline std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}
// Fields are joined with '|'; '%', '|' and line breaks are written as %XX so paths stay readable.
inline std::wstring escape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'%') o += L"%25"; else if (c == L'|') o += L"%7C"; else if (c == L'\n') o += L"%0A"; else if (c == L'\r') o += L"%0D"; else o += c;
    }
    return o;
}
inline std::wstring unescape(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2])) {
            o += (wchar_t)wcstoul(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}
inline std::vector<std::wstring> split(const std::wstring& s) {
    std::vector<std::wstring> parts(1);
    for (wchar_t c : s) { if (c == L'|') parts.emplace_back(); else parts.back() += c; }
    for (auto& p : parts) p = unescape(p);
    return parts;
}
inline std::wstring hexColor(COLORREF c) { wchar_t b[8]; swprintf(b, 8, L"%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c)); return b; }
inline COLORREF parseColor(const std::wstring& s) { unsigned long v = wcstoul(s.c_str(), nullptr, 16); return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255); }
inline int toInt(const std::wstring& s, int fallback) { if (s.empty()) return fallback; wchar_t* end; long v = wcstol(s.c_str(), &end, 10); return end == s.c_str() ? fallback : (int)v; }

inline void addRecent(Settings& s, const std::wstring& path) {
    s.recent.erase(std::remove_if(s.recent.begin(), s.recent.end(), [&](const std::wstring& p) { return CompareStringOrdinal(p.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL; }), s.recent.end());
    s.recent.insert(s.recent.begin(), path);
    if (s.recent.size() > MaxRecent) s.recent.resize(MaxRecent);
}

// Missing or unreadable file: defaults (including the default highlighting rules) and false.
inline bool load(const std::wstring& path, Settings& s) {
    s = Settings();
    s.rules = defaultRules();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string raw((size_t)std::min<LONGLONG>(size.QuadPart, 16 << 20), '\0');
    DWORD got = 0;
    if (!raw.empty()) ReadFile(h, &raw[0], (DWORD)raw.size(), &got, nullptr);
    CloseHandle(h);
    raw.resize(got);
    if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) raw.erase(0, 3);
    std::wstring text = fromUtf8(raw), section;
    bool sawRules = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            section = line.substr(1, line.find(L']') == std::wstring::npos ? std::wstring::npos : line.find(L']') - 1);
            if (section == L"highlight") { sawRules = true; s.rules.clear(); }
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq), value = line.substr(eq + 1);
        if (section == L"settings") {
            if (key == L"theme") s.theme = std::max(0, std::min(3, toInt(value, 0)));
            else if (key == L"font") s.font = unescape(value);
            else if (key == L"fontSize") s.fontSize = std::max(50, std::min(480, toInt(value, 100)));
            else if (key == L"tabSize") s.tabSize = std::max(1, std::min(16, toInt(value, 4)));
            else if (key == L"wrap") s.wrap = toInt(value, 0) != 0;
            else if (key == L"lineNumbers") s.lineNumbers = toInt(value, 1) != 0;
            else if (key == L"restoreSession") s.restoreSession = toInt(value, 1) != 0;
            else if (key == L"window") {
                auto p = split(value);
                if (p.size() == 5) { s.window = {toInt(p[0], 0), toInt(p[1], 0), toInt(p[2], 0), toInt(p[3], 0)}; s.maximized = toInt(p[4], 0) != 0; s.haveWindow = s.window.right > s.window.left && s.window.bottom > s.window.top; }
            }
        } else if (section == L"highlight" && key == L"rule") {
            auto p = split(value);
            if (p.size() == 7) {
                Rule r; r.enabled = p[0] == L"1"; r.matchCase = p[1] == L"1"; r.regex = p[2] == L"1"; r.useBack = p[3] == L"1";
                r.fore = parseColor(p[4]); r.back = parseColor(p[5]); r.text = p[6];
                s.rules.push_back(r);
            }
        } else if (section == L"recent" && key == L"file") {
            if (s.recent.size() < MaxRecent && !value.empty()) s.recent.push_back(unescape(value));
        } else if (section == L"session") {
            if (key == L"active") s.activeTab = std::max(0, toInt(value, 0));
            else if (key == L"tab") {
                auto p = split(value);
                if (p.size() == 8 && !p[6].empty()) {
                    TabState t; t.follow = p[0] == L"1"; t.top = wcstoull(p[1].c_str(), nullptr, 10); t.encoding = std::max(0, std::min(4, toInt(p[2], 0)));
                    t.matchCase = p[3] == L"1"; t.regex = p[4] == L"1"; t.filter = p[5] == L"1"; t.path = p[6]; t.query = p[7];
                    s.tabs.push_back(t);
                }
            }
        }
    }
    if (!sawRules) s.rules = defaultRules();
    return true;
}

// Writes to a temporary file and swaps it in, so a crash never leaves a half-written settings file.
inline bool save(const std::wstring& path, const Settings& s) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) SHCreateDirectoryExW(nullptr, path.substr(0, slash).c_str(), nullptr);
    auto b = [](bool v) { return v ? L"1" : L"0"; };
    std::wstring o = L"; BareLog settings. Safe to edit while BareLog is closed.\r\n[settings]\r\n";
    o += L"theme=" + std::to_wstring(s.theme) + L"\r\n";
    o += L"font=" + escape(s.font) + L"\r\n";
    o += L"fontSize=" + std::to_wstring(s.fontSize) + L"\r\n";
    o += L"tabSize=" + std::to_wstring(s.tabSize) + L"\r\n";
    o += std::wstring(L"wrap=") + b(s.wrap) + L"\r\n";
    o += std::wstring(L"lineNumbers=") + b(s.lineNumbers) + L"\r\n";
    o += std::wstring(L"restoreSession=") + b(s.restoreSession) + L"\r\n";
    if (s.haveWindow)
        o += L"window=" + std::to_wstring(s.window.left) + L"|" + std::to_wstring(s.window.top) + L"|" + std::to_wstring(s.window.right) + L"|" + std::to_wstring(s.window.bottom) + L"|" + b(s.maximized) + L"\r\n";
    o += L"[highlight]\r\n";
    for (auto& r : s.rules)
        o += std::wstring(L"rule=") + b(r.enabled) + L"|" + b(r.matchCase) + L"|" + b(r.regex) + L"|" + b(r.useBack) + L"|" + hexColor(r.fore) + L"|" + hexColor(r.back) + L"|" + escape(r.text) + L"\r\n";
    o += L"[recent]\r\n";
    for (auto& p : s.recent) o += L"file=" + escape(p) + L"\r\n";
    o += L"[session]\r\nactive=" + std::to_wstring(s.activeTab) + L"\r\n";
    for (auto& t : s.tabs)
        o += std::wstring(L"tab=") + b(t.follow) + L"|" + std::to_wstring(t.top) + L"|" + std::to_wstring(t.encoding) + L"|" + b(t.matchCase) + L"|" + b(t.regex) + L"|" + b(t.filter) + L"|" + escape(t.path) + L"|" + escape(t.query) + L"\r\n";
    std::string data = toUtf8(o);
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(tmp.c_str()); return false; }
    return true;
}

} // namespace settings

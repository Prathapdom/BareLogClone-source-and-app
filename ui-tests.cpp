// Hidden-window integration test: launches BareLog.exe with an isolated settings file and drives it with messages.
#ifndef UNICODE
#define UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

constexpr UINT WM_TESTQUERY = WM_APP + 100;
enum { QTabs, QLines, QFollow, QRows, QTop, QActive, QDark, QFilterDone, QCaret, QWrap, QFinding, QUnread, QFullRows, QSave };
enum { CmdFollow = 102, CmdWrap = 103, CmdFilter = 110, CmdNext = 112, CmdThemeBase = 3100 };

DWORD targetPid = 0;
HWND app = nullptr;
std::wstring root;

void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
BOOL CALLBACK findWindow(HWND h, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (pid == targetPid && wcscmp(cls, L"BareLog.Main") == 0) { app = h; return FALSE; }
    return TRUE;
}
LRESULT q(int what) { return SendMessageW(app, WM_TESTQUERY, what, 0); }
void cmd(int id) { SendMessageW(app, WM_COMMAND, MAKEWPARAM(id, 0), 0); }
bool waitFor(const std::function<bool()>& f, DWORD timeout = 5000) {
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < timeout) { if (f()) return true; Sleep(25); }
    return f();
}
void writeLines(const std::wstring& path, int from, int to, bool append) {
    FILE* f = _wfopen(path.c_str(), append ? L"ab" : L"wb");
    require(f != nullptr, "write fixture");
    for (int i = from; i < to; ++i) fprintf(f, "2026-10-16 12:00:00 [%s] line %d\n", i % 10 == 0 ? "ERROR" : "INFO", i);
    fclose(f);
}
PROCESS_INFORMATION launch(const std::wstring& exe, const std::wstring& args) {
    std::wstring command = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    require(CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi), "launch BareLog");
    CloseHandle(pi.hThread);
    return pi;
}
void attach(const PROCESS_INFORMATION& pi) {
    targetPid = pi.dwProcessId; app = nullptr;
    for (int i = 0; i < 200 && !app; ++i) { EnumWindows(findWindow, 0); if (!app) Sleep(25); }
    require(app != nullptr, "main window created");
}
std::string readFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string s(1 << 20, '\0'); DWORD n = 0; ReadFile(h, &s[0], (DWORD)s.size(), &n, nullptr); CloseHandle(h);
    s.resize(n); return s;
}
bool closeApp(PROCESS_INFORMATION& pi) {
    PostMessageW(app, WM_CLOSE, 0, 0);
    bool ok = WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0;
    CloseHandle(pi.hProcess); pi.hProcess = nullptr;
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    PROCESS_INFORMATION pi{};
    try {
        require(argc == 2, "pass the BareLog.exe path");
        std::wstring exe = argv[1];
        wchar_t temp[MAX_PATH]; GetTempPathW(MAX_PATH, temp);
        root = std::wstring(temp) + L"barelog-ui-" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(root.c_str(), nullptr);
        std::wstring a = root + L"\\app.log", b = root + L"\\later.log", ini = root + L"\\settings.ini";
        SetEnvironmentVariableW(L"BARELOG_SETTINGS", ini.c_str());
        writeLines(a, 0, 100, false);

        pi = launch(exe, L"\"" + a + L"\"");
        attach(pi);
        require(waitFor([] { return q(QLines) == 100; }), "initial file indexed (100 lines)");
        int rows = (int)q(QFullRows);
        require(rows > 5 && rows < 100, "view has a usable height while hidden");
        require(q(QFollow) == 1 && waitFor([&] { return q(QTop) == 100 - rows; }), "follow tail starts at the end");

        writeLines(a, 100, 150, true);
        require(waitFor([&] { return q(QLines) == 150 && q(QTop) == 150 - rows; }), "appended lines arrive and the view follows them");

        HWND view = FindWindowExW(app, nullptr, L"BareLog.View", nullptr);
        require(view != nullptr, "log view exists");
        SendMessageW(view, WM_VSCROLL, SB_TOP, 0);
        require(q(QTop) == 0 && q(QFollow) == 0, "scrolling up pauses following");
        writeLines(a, 150, 160, true);
        require(waitFor([] { return q(QLines) == 160; }) && q(QTop) == 0, "paused view stays put while lines arrive");
        cmd(CmdFollow);
        require(q(QFollow) == 1 && q(QTop) == 160 - rows, "follow command jumps back to the tail");

        HWND edit = FindWindowExW(app, nullptr, L"Edit", nullptr);
        require(edit != nullptr, "query box exists");
        SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)L"ERROR");
        cmd(CmdFilter);
        require(waitFor([] { return q(QFilterDone) == 1 && q(QRows) == 16; }), "live filter keeps 16 ERROR lines");
        writeLines(a, 160, 171, true);
        require(waitFor([] { return q(QLines) == 171 && q(QRows) == 18; }), "live filter picks up new matching lines");
        cmd(CmdFilter);
        require(waitFor([] { return q(QRows) == 171; }), "turning the filter off shows every line");

        SendMessageW(view, WM_VSCROLL, SB_TOP, 0);
        cmd(CmdNext);
        require(waitFor([] { return q(QFinding) == 0 && q(QCaret) >= 0; }), "find next completes");
        require(q(QCaret) == 0, "find selects the first ERROR line");
        cmd(CmdNext);
        require(waitFor([] { return q(QFinding) == 0 && q(QCaret) == 10; }), "find next moves to the following match");

        cmd(CmdWrap);
        require(q(QWrap) == 1, "wrap toggles on");
        cmd(CmdWrap);

        writeLines(a, 0, 5, false);
        require(waitFor([] { return q(QLines) == 5; }), "truncated file is re-read from the start");

        cmd(CmdThemeBase + 2); require(q(QDark) == 1, "dark theme");
        cmd(CmdThemeBase + 3); require(q(QDark) == 2, "AMOLED theme");
        cmd(CmdThemeBase + 1); require(q(QDark) == 0, "light theme");

        // A second launch hands its file to the running window and exits.
        PROCESS_INFORMATION second = launch(exe, L"\"" + b + L"\"");
        require(WaitForSingleObject(second.hProcess, 5000) == WAIT_OBJECT_0, "second instance exits after forwarding");
        CloseHandle(second.hProcess);
        require(waitFor([] { return q(QTabs) == 2 && q(QActive) == 1; }), "forwarded file opens in a new tab");
        require(q(QLines) == 0, "missing file shows as waiting");
        writeLines(b, 0, 7, false);
        require(waitFor([] { return q(QLines) == 7; }, 6000), "file that appears later is picked up");

        DWORD_PTR r = 0;
        require(SendMessageTimeoutW(app, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 500, &r) != 0, "UI stays responsive");

        require(closeApp(pi), "clean shutdown");
        std::string saved = readFile(ini);
        require(saved.find("theme=1") != std::string::npos, "theme persisted");
        require(saved.find("app.log") != std::string::npos && saved.find("later.log") != std::string::npos, "open tabs persisted");
        require(saved.find("[recent]") != std::string::npos, "recent files persisted");

        pi = launch(exe, L"");
        attach(pi);
        require(waitFor([] { return q(QTabs) == 2; }), "last session reopens both files");
        require(q(QActive) == 1 && waitFor([] { return q(QLines) == 7; }), "active tab restored");
        require(closeApp(pi), "clean shutdown after restore");

        DeleteFileW(a.c_str()); DeleteFileW(b.c_str()); DeleteFileW(ini.c_str()); RemoveDirectoryW(root.c_str());
        std::cout << "PASS: hidden launch, tail follow, pause/resume, live filter, find, wrap, truncation, themes, single instance, late file, persistence, session restore\n";
        return 0;
    } catch (const std::exception& e) {
        if (pi.hProcess) { TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hProcess); }
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}

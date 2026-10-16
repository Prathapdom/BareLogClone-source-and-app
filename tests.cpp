// Engine and settings tests: encodings, index, matching, live growth, truncation, rotation, filter, find.
#include "settings.hpp"
#include <cstdio>
#include <iostream>
#include <stdexcept>

using namespace logcore;
static int checks = 0;
#define REQUIRE(c) do { ++checks; if (!(c)) throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #c); } while (0)

static std::wstring tempDir;
static std::wstring file(const wchar_t* name) { return tempDir + L"\\" + name; }
static void write(const std::wstring& path, const std::string& data, bool append = false) {
    HANDLE h = CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("write fixture");
    DWORD n; WriteFile(h, data.data(), (DWORD)data.size(), &n, nullptr); CloseHandle(h);
}
template <class F> static bool waitFor(F f, DWORD ms = 5000) { auto t = GetTickCount64(); while (GetTickCount64() - t < ms) { if (f()) return true; Sleep(10); } return f(); }
static std::wstring lineText(LogDoc& d, uint64_t l) { return d.readLines({l}, 32768)[0]; }

static void testEncodings() {
    const unsigned char u8bom[] = {0xEF, 0xBB, 0xBF, 'a'};
    REQUIRE(detectEncoding(u8bom, 4).encoding == EncUtf8 && detectEncoding(u8bom, 4).bom == 3);
    const unsigned char le[] = {0xFF, 0xFE, 'a', 0};
    REQUIRE(detectEncoding(le, 4).encoding == EncUtf16LE);
    const unsigned char leNoBom[] = {'a', 0, 'b', 0, 'c', 0, '\n', 0, 'd', 0, 'e', 0};
    REQUIRE(detectEncoding(leNoBom, sizeof leNoBom).encoding == EncUtf16LE);
    const char* utf8 = "caf\xC3\xA9\n";
    REQUIRE(detectEncoding((const unsigned char*)utf8, strlen(utf8)).encoding == EncUtf8);
    const char* ansi = "caf\xE9 au lait\n";
    REQUIRE(detectEncoding((const unsigned char*)ansi, strlen(ansi)).encoding == EncAnsi);
    REQUIRE(detectEncoding(u8bom, 4, EncAnsi).encoding == EncAnsi && detectEncoding(u8bom, 4, EncAnsi).bom == 0);
    std::wstring out; decodeLine(EncUtf8, "abc\r\n", 5, out); REQUIRE(out == L"abc");
    decodeLine(EncUtf8, utf8, strlen(utf8), out); REQUIRE(out == L"caf\u00e9");
}

static void testIndex() {
    LineIndex idx;
    for (uint64_t i = 0; i < 5000; ++i) idx.push(i * 10);
    REQUIRE(idx.size() == 5000 && idx[4321] == 43210 && idx.back() == 49990);
    LineIndex wide;
    wide.push(0); wide.push(5ull << 32); wide.push((5ull << 32) + 7);
    REQUIRE(wide[0] == 0 && wide[1] == (5ull << 32) && wide[2] == (5ull << 32) + 7);
}

static void testMatcher() {
    auto m = Matcher::make(L"error", false, false);
    REQUIRE(m->matches(std::wstring(L"An ERROR here")) && !m->matches(std::wstring(L"all good")));
    const char* bytes = "line one\nan ErRoR line\nlast\n";
    REQUIRE(m->findBytes((const unsigned char*)bytes, strlen(bytes), 0) == 12);
    auto cs = Matcher::make(L"Error", true, false);
    REQUIRE(!cs->matches(std::wstring(L"ERROR")) && cs->matches(std::wstring(L"Error")));
    REQUIRE(cs->findBytes((const unsigned char*)"xxErrorx", 8, 0) == 2 && cs->findBytes((const unsigned char*)"ERROR", 5, 0) == std::string::npos);
    auto u = Matcher::make(L"\u00c9T\u00c9", false, false);
    REQUIRE(u->matches(std::wstring(L"l'\u00e9t\u00e9 ici")) && !u->byteFast(EncUtf8));
    auto re = Matcher::make(L"status=[45]\\d\\d", false, true);
    REQUIRE(re->valid && re->matches(std::wstring(L"GET / status=404")) && !re->matches(std::wstring(L"status=200")));
    REQUIRE(!Matcher::make(L"[", false, true)->valid);
    std::vector<std::pair<size_t, size_t>> spans;
    Matcher::make(L"ab", false, false)->findAll(L"xABxab", spans);
    REQUIRE(spans.size() == 2 && spans[0].first == 1 && spans[1].first == 4);
}

static void testLiveFile() {
    auto path = file(L"live.log");
    write(path, "first\r\nsecond ERROR\r\nthird");
    std::atomic<int> notified{0};
    LogDoc doc(path, EncAuto, [&] { ++notified; }, 20);
    doc.start();
    REQUIRE(waitFor([&] { auto i = doc.info(); return i.lines == 3 && i.scanned == i.size; }));
    REQUIRE(doc.info().complete == 2 && lineText(doc, 1) == L"second ERROR" && lineText(doc, 2) == L"third");
    doc.acknowledge();
    write(path, " continues\nfourth\n", true);
    REQUIRE(waitFor([&] { return doc.info().lines == 4 && doc.info().complete == 4; }));
    REQUIRE(lineText(doc, 2) == L"third continues" && lineText(doc, 3) == L"fourth" && notified > 0);

    // Live filter keeps up with appended lines.
    doc.setFilter(Matcher::make(L"error", false, false));
    REQUIRE(waitFor([&] { auto i = doc.info(); return !i.filterBusy() && i.filteredRows == 1; }));
    REQUIRE(doc.filteredLine(0) == 1 && doc.rowForLine(2) == 1);
    write(path, "fifth error\nsixth\n", true);
    REQUIRE(waitFor([&] { auto i = doc.info(); return i.lines == 6 && !i.filterBusy() && i.filteredRows == 2; }));
    REQUIRE(doc.filteredLine(1) == 4);

    std::atomic<bool> cancel{false};
    auto m = Matcher::make(L"ERROR", true, false);
    REQUIRE(doc.find(0, true, *m, cancel) == 1 && doc.find(2, true, *m, cancel) == -1 && doc.find(5, false, *m, cancel) == 1);

    // Truncation (copytruncate style rotation) restarts the view.
    auto gen = doc.info().generation;
    write(path, "new start\n");
    REQUIRE(waitFor([&] { auto i = doc.info(); return i.generation != gen && i.lines == 1 && i.scanned == i.size; }));
    REQUIRE(lineText(doc, 0) == L"new start");
    REQUIRE(waitFor([&] { auto i = doc.info(); return !i.filterBusy() && i.filteredRows == 0; }));

    // Rename-and-recreate rotation: a new file at the same path is followed.
    auto rotated = file(L"live.log.1");
    REQUIRE(MoveFileExW(path.c_str(), rotated.c_str(), MOVEFILE_REPLACE_EXISTING));
    write(path, "after rotation line 1\nafter rotation line 2 is longer\n");
    REQUIRE(waitFor([&] { return doc.info().lines == 2 && lineText(doc, 1) == L"after rotation line 2 is longer"; }));

    // Deletion: reported missing, then picked up again when it reappears.
    REQUIRE(DeleteFileW(path.c_str()));
    REQUIRE(waitFor([&] { return !doc.info().exists; }));
    write(path, "back again\n");
    REQUIRE(waitFor([&] { auto i = doc.info(); return i.exists && i.lines == 1 && lineText(doc, 0) == L"back again"; }));
    DeleteFileW(rotated.c_str());
}

static void testUtf16AndForcedEncoding() {
    auto path = file(L"wide.log");
    std::string d = "\xFF\xFE";
    for (wchar_t c : std::wstring(L"h\u00e9llo\r\nw\u00f6rld\r\n")) { d += char(c & 0xFF); d += char(c >> 8); }
    write(path, d);
    LogDoc doc(path, EncAuto, nullptr, 20);
    doc.start();
    REQUIRE(waitFor([&] { auto i = doc.info(); return i.lines == 2 && i.scanned == i.size; }));
    REQUIRE(doc.info().encoding == EncUtf16LE && lineText(doc, 0) == L"h\u00e9llo" && lineText(doc, 1) == L"w\u00f6rld");
    std::atomic<bool> cancel{false};
    REQUIRE(doc.find(0, true, *Matcher::make(L"W\u00d6RLD", false, false), cancel) == 1);

    auto path2 = file(L"ansi.log");
    write(path2, "caf\xE9\n");
    LogDoc forced(path2, EncUtf8, nullptr, 20);
    forced.start();
    REQUIRE(waitFor([&] { return forced.info().lines == 1; }));
    REQUIRE(forced.info().encoding == EncUtf8);
    forced.setEncoding(EncAnsi);
    REQUIRE(waitFor([&] { return forced.info().encoding == EncAnsi && forced.info().lines == 1 && lineText(forced, 0) == L"caf\u00e9"; }));
}

static void testLargeFile() {
    auto path = file(L"large.log");
    std::string block;
    for (int i = 0; i < 200000; ++i) block += "2026-10-16 12:00:00.000 INFO  worker-" + std::to_string(i % 7) + " processed request id=" + std::to_string(i) + (i % 50000 == 49999 ? " ERROR timeout\n" : "\n");
    write(path, block);
    LogDoc doc(path, EncAuto, nullptr, 20);
    auto t0 = GetTickCount64();
    doc.start();
    REQUIRE(waitFor([&] { auto i = doc.info(); return i.size && i.scanned == i.size; }, 20000));
    auto indexMs = GetTickCount64() - t0;
    REQUIRE(doc.info().lines == 200000);
    REQUIRE(lineText(doc, 199999).find(L"id=199999") != std::wstring::npos);
    t0 = GetTickCount64();
    doc.setFilter(Matcher::make(L"error", false, false));
    REQUIRE(waitFor([&] { auto i = doc.info(); return !i.filterBusy() && i.filteredRows == 4; }, 20000));
    REQUIRE(doc.filteredLine(0) == 49999 && doc.filteredLine(3) == 199999);
    auto filterMs = GetTickCount64() - t0;
    doc.setFilter(Matcher::make(L"id=1234\\d\\b", false, true));
    REQUIRE(waitFor([&] { auto i = doc.info(); return !i.filterBusy() && i.filteredRows == 10; }, 20000));
    std::cout << "  " << block.size() / (1024 * 1024) << " MiB, 200k lines: index " << indexMs << " ms, literal filter " << filterMs << " ms\n";
}

static void testSettings() {
    settings::Settings s;
    auto path = file(L"cfg\\settings.ini");
    REQUIRE(!settings::load(path, s) && s.rules.size() == 5);
    s.theme = 3; s.wrap = true; s.font = L"Cascadia Mono"; s.fontSize = 115;
    s.haveWindow = true; s.window = {10, 20, 810, 620}; s.maximized = true;
    s.rules.clear(); settings::Rule r; r.text = L"a|b\\c"; r.regex = true; r.back = RGB(1, 2, 3); s.rules.push_back(r);
    settings::addRecent(s, L"C:\\one.log"); settings::addRecent(s, L"C:\\two.log"); settings::addRecent(s, L"c:\\ONE.log");
    settings::TabState t; t.path = L"C:\\logs\\a|b.log"; t.query = L"x|y %7C 50%\n"; t.follow = false; t.top = 12345678901ull; t.filter = true; t.encoding = 3;
    s.tabs.push_back(t); s.activeTab = 0;
    REQUIRE(settings::save(path, s));
    settings::Settings l;
    REQUIRE(settings::load(path, l));
    REQUIRE(l.theme == 3 && l.wrap && l.font == L"Cascadia Mono" && l.fontSize == 115 && l.maximized && l.window.right == 810);
    REQUIRE(l.rules.size() == 1 && l.rules[0].text == L"a|b\\c" && l.rules[0].regex && l.rules[0].back == RGB(1, 2, 3));
    REQUIRE(l.recent.size() == 2 && l.recent[0] == L"c:\\ONE.log");
    REQUIRE(l.tabs.size() == 1 && l.tabs[0].path == L"C:\\logs\\a|b.log" && l.tabs[0].query == L"x|y %7C 50%\n" && !l.tabs[0].follow && l.tabs[0].top == 12345678901ull && l.tabs[0].filter && l.tabs[0].encoding == 3);
    {
        // Backslashes are written as-is so the file stays easy to edit by hand.
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        char buf[8192]; DWORD n = 0; ReadFile(h, buf, sizeof buf, &n, nullptr); CloseHandle(h);
        REQUIRE(std::string(buf, n).find("C:\\logs\\a%7Cb.log|x%7Cy %257C 50%25%0A") != std::string::npos);
    }
    // An empty [highlight] section means "no rules", not "defaults".
    l.rules.clear(); REQUIRE(settings::save(path, l)); REQUIRE(settings::load(path, s) && s.rules.empty());
    DeleteFileW(path.c_str()); RemoveDirectoryW(file(L"cfg").c_str());
}

int main() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    tempDir = std::wstring(tmp) + L"barelog-tests-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(tempDir.c_str(), nullptr);
    int rc = 0;
    try {
        testEncodings(); testIndex(); testMatcher(); testLiveFile(); testUtf16AndForcedEncoding(); testLargeFile(); testSettings();
        std::cout << "PASS: " << checks << " engine/settings checks\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << "\n"; rc = 1; }
    for (auto n : {L"live.log", L"live.log.1", L"wide.log", L"ansi.log", L"large.log"}) DeleteFileW(file(n).c_str());
    RemoveDirectoryW(tempDir.c_str());
    return rc;
}

// BareLog tail engine: line indexing, encoding detection, matching, filtering and search.
// No UI code lives here so the engine can be unit tested on its own.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace logcore {

enum Encoding : int { EncAuto, EncAnsi, EncUtf8, EncUtf16LE, EncUtf16BE };
inline const wchar_t* encodingName(int e) {
    switch (e) { case EncAnsi: return L"ANSI"; case EncUtf8: return L"UTF-8"; case EncUtf16LE: return L"UTF-16 LE"; case EncUtf16BE: return L"UTF-16 BE"; default: return L"Auto"; }
}
inline bool isWide(int e) { return e == EncUtf16LE || e == EncUtf16BE; }

inline bool validUtf8(const unsigned char* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }
        int len = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        if (!len) return false;
        if (i + len > n) return true; // sequence cut off by the end of the sample
        for (int k = 1; k < len; ++k) if ((p[i + k] & 0xC0) != 0x80) return false;
        if ((c == 0xE0 && p[i + 1] < 0xA0) || (c == 0xED && p[i + 1] > 0x9F) || (c == 0xF0 && p[i + 1] < 0x90) || (c == 0xF4 && p[i + 1] > 0x8F)) return false;
        i += len;
    }
    return true;
}

struct Detected { int encoding; uint32_t bom; };
// A forced encoding still skips its own BOM. Auto: BOM, then BOM-less UTF-16 by NUL pattern, then UTF-8 validity, else ANSI.
inline Detected detectEncoding(const unsigned char* p, size_t n, int choice = EncAuto) {
    int found = EncAuto; uint32_t bom = 0;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { found = EncUtf8; bom = 3; }
    else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { found = EncUtf16LE; bom = 2; }
    else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { found = EncUtf16BE; bom = 2; }
    if (choice != EncAuto) return {choice, found == choice ? bom : 0};
    if (found != EncAuto) return {found, bom};
    size_t m = std::min(n, size_t(8192)) & ~size_t(1), odd = 0, even = 0;
    for (size_t i = 0; i < m; ++i) if (!p[i]) (i & 1 ? odd : even)++;
    if (m >= 8) {
        if (odd > m / 4 && even <= m / 64) return {EncUtf16LE, 0};
        if (even > m / 4 && odd <= m / 64) return {EncUtf16BE, 0};
    }
    return {validUtf8(p, n) ? EncUtf8 : EncAnsi, 0};
}

// Decodes one line, dropping its trailing LF / CRLF.
inline void decodeLine(int enc, const char* p, size_t n, std::wstring& out) {
    out.clear();
    if (isWide(enc)) {
        size_t units = n / 2;
        out.resize(units);
        const unsigned char* b = (const unsigned char*)p;
        if (enc == EncUtf16LE) for (size_t i = 0; i < units; ++i) out[i] = wchar_t(b[2 * i] | (b[2 * i + 1] << 8));
        else for (size_t i = 0; i < units; ++i) out[i] = wchar_t((b[2 * i] << 8) | b[2 * i + 1]);
        while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r')) out.pop_back();
        return;
    }
    while (n && (p[n - 1] == '\n' || p[n - 1] == '\r')) --n;
    if (!n) return;
    bool ascii = true;
    for (size_t i = 0; i < n; ++i) if ((unsigned char)p[i] >= 0x80) { ascii = false; break; }
    if (ascii) { out.resize(n); for (size_t i = 0; i < n; ++i) out[i] = wchar_t((unsigned char)p[i]); return; }
    UINT cp = enc == EncAnsi ? CP_ACP : CP_UTF8;
    int len = MultiByteToWideChar(cp, 0, p, (int)std::min(n, size_t(INT_MAX)), nullptr, 0);
    out.resize(len);
    if (len) MultiByteToWideChar(cp, 0, p, (int)std::min(n, size_t(INT_MAX)), &out[0], len);
}

// Line start offsets at 4 bytes per line: a 64-bit base per 1024 lines plus 32-bit deltas.
// Deltas that do not fit (blocks spanning 4 GiB) live in a small sorted side table.
class LineIndex {
    static constexpr size_t Block = 1024;
    static constexpr uint32_t Far = 0xFFFFFFFFu;
    std::vector<uint64_t> bases_;
    std::vector<uint32_t> deltas_;
    std::vector<std::pair<size_t, uint64_t>> far_;
public:
    void clear() { bases_.clear(); deltas_.clear(); far_.clear(); bases_.shrink_to_fit(); deltas_.shrink_to_fit(); }
    size_t size() const { return deltas_.size(); }
    bool empty() const { return deltas_.empty(); }
    void push(uint64_t offset) {
        size_t i = deltas_.size();
        if (i % Block == 0) bases_.push_back(offset);
        uint64_t d = offset - bases_[i / Block];
        if (d >= Far) { far_.push_back({i, offset}); deltas_.push_back(Far); }
        else deltas_.push_back((uint32_t)d);
    }
    uint64_t operator[](size_t i) const {
        uint32_t d = deltas_[i];
        if (d != Far) return bases_[i / Block] + d;
        auto it = std::lower_bound(far_.begin(), far_.end(), std::make_pair(i, uint64_t(0)));
        return it->second;
    }
    uint64_t back() const { return (*this)[size() - 1]; }
};

inline wchar_t foldAscii(wchar_t c) { return (c >= L'A' && c <= L'Z') ? wchar_t(c + 32) : c; }
inline unsigned char foldByte(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }

inline bool singleByteAnsi() { static int v = -1; if (v < 0) { CPINFO ci{}; v = GetCPInfo(CP_ACP, &ci) && ci.MaxCharSize == 1; } return v == 1; }

// Literal or ECMAScript regex matcher. Case-insensitive ASCII patterns fold ASCII only, so the byte fast path
// (Boyer-Moore-Horspool over raw UTF-8 / single-byte ANSI) agrees exactly with the decoded path.
class Matcher {
public:
    static constexpr size_t RegexLimit = 4096; // std::regex recursion depth guard
    std::wstring pattern; bool matchCase = false, regex = false, valid = true;
    static std::shared_ptr<const Matcher> make(const std::wstring& pattern, bool matchCase, bool regex) {
        auto m = std::make_shared<Matcher>();
        m->pattern = pattern; m->matchCase = matchCase; m->regex = regex;
        m->compile();
        return m;
    }
    bool empty() const { return pattern.empty(); }
    bool byteFast(int enc) const { return valid && !regex && ascii_ && !pattern.empty() && (enc == EncUtf8 || (enc == EncAnsi && singleByteAnsi())); }
    bool matches(const wchar_t* s, size_t n) const {
        if (!valid) return false;
        if (pattern.empty()) return true;
        if (regex) { try { return std::regex_search(s, s + std::min(n, RegexLimit), re_); } catch (...) { return false; } }
        return find(s, n, 0) != std::wstring::npos;
    }
    bool matches(const std::wstring& s) const { return matches(s.data(), s.size()); }
    // Appends [start, end) ranges of matches, at most `limit`.
    void findAll(const std::wstring& s, std::vector<std::pair<size_t, size_t>>& out, size_t limit = 512) const {
        if (!valid || pattern.empty()) return;
        if (regex) {
            try {
                const wchar_t* b = s.data(); const wchar_t* e = b + std::min(s.size(), RegexLimit);
                for (std::wcregex_iterator it(b, e, re_), end; it != end && out.size() < limit; ++it) {
                    size_t pos = (size_t)it->position(), len = (size_t)it->length();
                    if (len) out.push_back({pos, pos + len});
                }
            } catch (...) {}
            return;
        }
        size_t pos = 0;
        while (out.size() < limit) {
            size_t hit = find(s.data(), s.size(), pos);
            if (hit == std::wstring::npos) break;
            out.push_back({hit, hit + needle_.size()});
            pos = hit + std::max<size_t>(1, needle_.size());
        }
    }
    size_t find(const wchar_t* s, size_t n, size_t from) const {
        size_t m = needle_.size();
        if (m > n || from > n - m) return std::wstring::npos;
        if (matchCase) {
            for (size_t i = from; i + m <= n; ++i) if (s[i] == needle_[0] && std::wmemcmp(s + i, needle_.data(), m) == 0) return i;
            return std::wstring::npos;
        }
        if (ascii_) {
            wchar_t first = needle_[0];
            for (size_t i = from; i + m <= n; ++i) {
                if (foldAscii(s[i]) != first) continue;
                size_t k = 1; while (k < m && foldAscii(s[i + k]) == needle_[k]) ++k;
                if (k == m) return i;
            }
            return std::wstring::npos;
        }
        thread_local std::wstring lower;
        lower.assign(s, n);
        if (n) CharLowerBuffW(&lower[0], (DWORD)n);
        size_t r = lower.find(needle_, from);
        return r;
    }
    size_t findBytes(const unsigned char* p, size_t n, size_t from) const {
        size_t m = bytes_.size();
        if (!m || m > n) return std::string::npos;
        const unsigned char last = (unsigned char)bytes_[m - 1];
        for (size_t i = from; i + m <= n;) {
            unsigned char c = matchCase ? p[i + m - 1] : foldByte(p[i + m - 1]);
            if (c == last) {
                size_t k = 0;
                if (matchCase) { if (std::memcmp(p + i, bytes_.data(), m - 1) == 0) return i; }
                else { while (k + 1 < m && foldByte(p[i + k]) == (unsigned char)bytes_[k]) ++k; if (k + 1 == m) return i; }
            }
            i += skip_[c];
        }
        return std::string::npos;
    }
private:
    bool ascii_ = false;
    std::wstring needle_;
    std::string bytes_;
    size_t skip_[256]{};
    std::wregex re_;
    void compile() {
        if (regex) {
            try {
                auto f = std::regex_constants::ECMAScript;
                if (!matchCase) f |= std::regex_constants::icase;
                re_.assign(pattern, f);
            } catch (const std::regex_error&) { valid = false; }
            return;
        }
        ascii_ = std::all_of(pattern.begin(), pattern.end(), [](wchar_t c) { return c < 0x80; });
        needle_ = pattern;
        if (!matchCase) {
            if (ascii_) for (auto& c : needle_) c = foldAscii(c);
            else if (!needle_.empty()) CharLowerBuffW(&needle_[0], (DWORD)needle_.size());
        }
        if (ascii_ && !needle_.empty()) {
            bytes_.assign(needle_.begin(), needle_.end());
            size_t m = bytes_.size();
            for (auto& s : skip_) s = m;
            for (size_t i = 0; i + 1 < m; ++i) {
                unsigned char c = (unsigned char)bytes_[i];
                skip_[c] = m - 1 - i;
                if (!matchCase && c >= 'a' && c <= 'z') skip_[c - 32] = m - 1 - i; // never read (input is folded) but harmless
            }
        }
    }
};

struct DocInfo {
    uint64_t lines = 0, complete = 0, scanned = 0, size = 0, generation = 0, maxLineChars = 0;
    uint64_t filteredRows = 0, filterScanned = 0;
    int encoding = EncUtf8;
    bool exists = false, filterOn = false;
    DWORD error = 0;
    ULONGLONG changedTick = 0;
    bool indexing() const { return scanned < size; }
    bool filterBusy() const { return filterOn && filterScanned < complete; }
};

class SharedLock { SRWLOCK* l_; public: explicit SharedLock(SRWLOCK& l) : l_(&l) { AcquireSRWLockShared(l_); } ~SharedLock() { ReleaseSRWLockShared(l_); } };
class ExclusiveLock { SRWLOCK* l_; public: explicit ExclusiveLock(SRWLOCK& l) : l_(&l) { AcquireSRWLockExclusive(l_); } ~ExclusiveLock() { ReleaseSRWLockExclusive(l_); } };

struct FileHandle {
    HANDLE h;
    explicit FileHandle(HANDLE handle) : h(handle) {}
    ~FileHandle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    FileHandle(const FileHandle&) = delete; FileHandle& operator=(const FileHandle&) = delete;
};

inline size_t readAt(HANDLE h, uint64_t offset, void* buffer, size_t size) {
    size_t total = 0; char* p = (char*)buffer;
    while (total < size) {
        OVERLAPPED o{}; uint64_t at = offset + total;
        o.Offset = (DWORD)at; o.OffsetHigh = (DWORD)(at >> 32);
        DWORD n = 0, want = (DWORD)std::min<size_t>(size - total, 1u << 24);
        if (!ReadFile(h, p + total, want, &n, &o) || !n) break;
        total += n;
    }
    return total;
}

// A run of consecutive lines read in one go. starts has count+1 entries; the last is the end offset.
struct Chunk {
    uint64_t first = 0, base = 0, generation = 0;
    std::vector<uint64_t> starts;
    std::string bytes;
    int encoding = EncUtf8;
    size_t count() const { return starts.empty() ? 0 : starts.size() - 1; }
    // Bytes of line k that were actually read (a single oversized line can be capped).
    void line(size_t k, const char*& p, size_t& n) const {
        uint64_t s = starts[k] - base, e = std::min<uint64_t>(starts[k + 1] - base, bytes.size());
        p = bytes.data() + s; n = e > s ? size_t(e - s) : 0;
    }
};

// One open log file. A worker thread polls the file, extends the line index, and runs the live filter.
// The UI thread reads text through readLines(); it only ever takes the shared lock.
class LogDoc {
public:
    LogDoc(std::wstring path, int encodingChoice, std::function<void()> notify, DWORD pollMs = 250)
        : path_(std::move(path)), notify_(std::move(notify)), pollMs_(pollMs), encodingChoice_(encodingChoice) {
        InitializeSRWLock(&lock_);
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~LogDoc() {
        stop_ = true; SetEvent(wake_);
        if (worker_.joinable()) worker_.join();
        CloseHandle(wake_);
    }
    LogDoc(const LogDoc&) = delete; LogDoc& operator=(const LogDoc&) = delete;
    void start() { worker_ = std::thread([this] { run(); }); }
    const std::wstring& path() const { return path_; }
    int encodingChoice() const { return encodingChoice_; }
    void setEncoding(int choice) { encodingChoice_ = choice; reload(); }
    void reload() { resetRequested_ = true; SetEvent(wake_); }
    void poke() { SetEvent(wake_); }
    void acknowledge() { pending_ = false; }

    DocInfo info() const {
        SharedLock g(lock_);
        DocInfo d;
        d.complete = index_.empty() ? 0 : index_.size() - 1;
        d.lines = d.complete + (!index_.empty() && scanned_ > index_.back() ? 1 : 0);
        d.scanned = scanned_; d.size = size_; d.generation = generation_; d.maxLineChars = maxLineChars_;
        d.encoding = encoding_; d.exists = exists_; d.error = error_; d.changedTick = changedTick_;
        d.filterOn = (bool)filter_; d.filteredRows = filtered_.size(); d.filterScanned = filterScanned_;
        return d;
    }
    // Null turns the filter off. Rows are rebuilt in the background from the first line.
    void setFilter(std::shared_ptr<const Matcher> m) {
        { ExclusiveLock g(lock_); filter_ = std::move(m); std::vector<uint64_t>().swap(filtered_); filterScanned_ = 0; ++filterGen_; }
        SetEvent(wake_);
    }
    uint64_t filteredLine(uint64_t row) const { SharedLock g(lock_); return row < filtered_.size() ? filtered_[row] : UINT64_MAX; }
    // First filtered row whose line is >= line (== row count when none).
    uint64_t rowForLine(uint64_t line) const { SharedLock g(lock_); return uint64_t(std::lower_bound(filtered_.begin(), filtered_.end(), line) - filtered_.begin()); }

    bool readChunk(uint64_t first, uint64_t last, size_t maxBytes, Chunk& c) const {
        std::shared_ptr<FileHandle> file;
        {
            SharedLock g(lock_);
            uint64_t complete = index_.empty() ? 0 : index_.size() - 1;
            uint64_t lines = complete + (!index_.empty() && scanned_ > index_.back() ? 1 : 0);
            last = std::min(last, lines);
            if (first >= last || !file_) return false;
            uint64_t count = last - first, s0 = index_[first];
            // Shrink the run so it fits in maxBytes (always keep at least one line).
            if (count > 1) {
                uint64_t lo = 1, hi = count;
                auto endOf = [&](uint64_t k) { uint64_t l = first + k; return l < index_.size() ? index_[l] : scanned_; };
                while (lo < hi) { uint64_t mid = (lo + hi + 1) / 2; if (endOf(mid) - s0 <= maxBytes) lo = mid; else hi = mid - 1; }
                count = lo;
            }
            c.starts.resize(count + 1);
            for (uint64_t k = 0; k <= count; ++k) { uint64_t l = first + k; c.starts[k] = l < index_.size() ? index_[l] : scanned_; }
            c.first = first; c.base = s0; c.encoding = encoding_; c.generation = generation_;
            file = file_;
        }
        size_t want = (size_t)std::min<uint64_t>(c.starts.back() - c.base, maxBytes);
        if (isWide(c.encoding)) want &= ~size_t(1);
        c.bytes.resize(want);
        size_t got = want ? readAt(file->h, c.base, &c.bytes[0], want) : 0;
        c.bytes.resize(got);
        return true;
    }
    // Text of the given lines (any order), each capped at maxChars. Missing lines come back empty.
    std::vector<std::wstring> readLines(const std::vector<uint64_t>& lines, size_t maxChars) const {
        std::vector<std::wstring> out(lines.size());
        size_t i = 0; Chunk c;
        size_t perLine = maxChars * 4 + 4;
        while (i < lines.size()) {
            size_t j = i + 1;
            while (j < lines.size() && lines[j] == lines[j - 1] + 1 && j - i < 4096) ++j;
            uint64_t next = lines[i];
            while (next <= lines[j - 1]) {
                if (!readChunk(next, lines[j - 1] + 1, std::max<size_t>(perLine, 4u << 20), c)) { next = lines[j - 1] + 1; break; }
                for (size_t k = 0; k < c.count(); ++k) {
                    const char* p; size_t n; c.line(k, p, n);
                    n = std::min(n, perLine);
                    if (isWide(c.encoding)) n &= ~size_t(1);
                    auto& s = out[i + size_t(c.first + k - lines[i])];
                    decodeLine(c.encoding, p, n, s);
                    if (s.size() > maxChars) s.resize(maxChars);
                }
                next = c.first + c.count();
            }
            i = j;
        }
        return out;
    }
    // Calls cb(line) for each matching line in the chunk; cb returns false to stop.
    static void matchChunk(const Chunk& c, const Matcher& m, const std::function<bool(uint64_t)>& cb) {
        size_t count = c.count();
        bool capped = c.base + c.bytes.size() < c.starts.back();
        if (m.byteFast(c.encoding) && !capped) {
            const unsigned char* p = (const unsigned char*)c.bytes.data();
            size_t n = c.bytes.size(), pos = 0;
            while (pos < n) {
                size_t hit = m.findBytes(p, n, pos);
                if (hit == std::string::npos) break;
                size_t k = size_t(std::upper_bound(c.starts.begin(), c.starts.end() - 1, c.base + hit) - c.starts.begin()) - 1;
                if (!cb(c.first + k)) return;
                pos = size_t(c.starts[k + 1] - c.base);
            }
            return;
        }
        std::wstring w;
        for (size_t k = 0; k < count; ++k) {
            const char* p; size_t n; c.line(k, p, n);
            decodeLine(c.encoding, p, n, w);
            if (m.matches(w) && !cb(c.first + k)) return;
        }
    }
    // Nearest matching line at or after `from` (forward) or at or before it (backward); -1 if none or cancelled.
    int64_t find(uint64_t from, bool forward, const Matcher& m, const std::atomic<bool>& cancel) const {
        if (!m.valid || m.empty()) return -1;
        uint64_t lines = info().lines;
        Chunk c;
        if (forward) {
            for (uint64_t a = from; a < lines && !cancel;) {
                if (!readChunk(a, lines, 4u << 20, c)) break;
                int64_t found = -1;
                matchChunk(c, m, [&](uint64_t l) { found = (int64_t)l; return false; });
                if (found >= 0) return found;
                a = c.first + c.count();
            }
            return -1;
        }
        if (lines == 0) return -1;
        uint64_t b = std::min(from, lines - 1) + 1;
        while (b > 0 && !cancel) {
            uint64_t a = b > 8192 ? b - 8192 : 0;
            int64_t found = -1;
            for (uint64_t x = a; x < b && !cancel;) {
                if (!readChunk(x, b, 4u << 20, c)) break;
                matchChunk(c, m, [&](uint64_t l) { found = (int64_t)l; return true; });
                x = c.first + c.count();
            }
            if (found >= 0) return found;
            b = a;
        }
        return -1;
    }

private:
    const std::wstring path_;
    std::function<void()> notify_;
    const DWORD pollMs_;
    std::atomic<int> encodingChoice_;
    mutable SRWLOCK lock_;
    HANDLE wake_ = nullptr;
    std::thread worker_;
    std::atomic<bool> stop_{false}, resetRequested_{false}, pending_{false};
    // Guarded by lock_ (written by the worker only, except filter state).
    LineIndex index_;
    uint64_t scanned_ = 0, size_ = 0, generation_ = 0, maxLineChars_ = 0;
    int encoding_ = EncUtf8;
    bool exists_ = false;
    DWORD error_ = 0;
    ULONGLONG changedTick_ = 0;
    std::shared_ptr<FileHandle> file_;
    std::shared_ptr<const Matcher> filter_;
    std::vector<uint64_t> filtered_;
    uint64_t filterScanned_ = 0, filterGen_ = 0;
    // Worker-only state.
    struct FileId { DWORD volume = 0, high = 0, low = 0; bool operator==(const FileId& o) const { return volume == o.volume && high == o.high && low == o.low; } };
    FileId id_; bool haveId_ = false, detected_ = false;
    uint32_t bom_ = 0;
    std::string prefix_;
    ULONGLONG lastSignal_ = 0;

    void signal() { lastSignal_ = GetTickCount64(); if (!pending_.exchange(true) && notify_) notify_(); }
    void signalThrottled() { if (GetTickCount64() - lastSignal_ >= 50) signal(); }

    void run() {
        std::vector<char> buffer(1 << 20);
        while (!stop_) {
            poll(buffer);
            bool more = filterStep();
            WaitForSingleObject(wake_, more ? 0 : pollMs_);
        }
    }
    void resetIndex() {
        {
            ExclusiveLock g(lock_);
            index_.clear(); scanned_ = 0; maxLineChars_ = 0; ++generation_;
            std::vector<uint64_t>().swap(filtered_); filterScanned_ = 0; ++filterGen_;
            changedTick_ = GetTickCount64();
        }
        detected_ = false; prefix_.clear();
        signal();
    }
    void markMissing(DWORD error) {
        bool changed;
        { ExclusiveLock g(lock_); changed = exists_ || error_ != error || file_; exists_ = false; error_ = error; file_.reset(); }
        if (changed) signal();
    }
    void poll(std::vector<char>& buffer) {
        bool force = resetRequested_.exchange(false);
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &a)) {
            markMissing(GetLastError());
            if (force && haveId_) { haveId_ = false; resetIndex(); }
            return;
        }
        if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { markMissing(ERROR_DIRECTORY); return; }
        uint64_t pathSize = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
        std::shared_ptr<FileHandle> f = file_; // only this thread writes file_
        bool reopen = !f || force;
        if (!reopen) {
            BY_HANDLE_FILE_INFORMATION fi{};
            if (!GetFileInformationByHandle(f->h, &fi)) reopen = true;
            else if (CompareFileTime(&fi.ftCreationTime, &a.ftCreationTime) != 0 || ((uint64_t(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow) != pathSize) reopen = true;
        }
        if (reopen) {
            HANDLE h = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                // Keep reading through the old handle if we have one (e.g. a transient sharing violation).
                if (!f) {
                    DWORD e = GetLastError();
                    bool changed;
                    { ExclusiveLock g(lock_); changed = error_ != e; error_ = e; exists_ = e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND; }
                    if (changed) signal();
                    return;
                }
            } else {
                BY_HANDLE_FILE_INFORMATION fi{};
                GetFileInformationByHandle(h, &fi);
                FileId id{fi.dwVolumeSerialNumber, fi.nFileIndexHigh, fi.nFileIndexLow};
                bool same = haveId_ && id == id_ && !force;
                f = std::make_shared<FileHandle>(h);
                { ExclusiveLock g(lock_); file_ = f; exists_ = true; error_ = 0; }
                if (!same) { id_ = id; haveId_ = true; resetIndex(); }
            }
        }
        {
            bool changed = false;
            { ExclusiveLock g(lock_); if (!exists_ || error_) { exists_ = true; error_ = 0; changed = true; } }
            if (changed) signal();
        }
        LARGE_INTEGER li{};
        if (!GetFileSizeEx(f->h, &li)) return;
        uint64_t size = (uint64_t)li.QuadPart;
        if (size < scanned_) resetIndex();
        if (size != size_) { ExclusiveLock g(lock_); size_ = size; }
        if (size > scanned_ || !detected_) {
            if (detected_ && !prefixMatches(f->h)) resetIndex();
            indexMore(f->h, size, buffer);
        }
    }
    bool prefixMatches(HANDLE h) {
        if (prefix_.empty()) return true;
        std::string now(prefix_.size(), '\0');
        return readAt(h, 0, &now[0], now.size()) == now.size() && now == prefix_;
    }
    void indexMore(HANDLE h, uint64_t size, std::vector<char>& buffer) {
        if (!detected_) {
            std::vector<unsigned char> head((size_t)std::min<uint64_t>(size, 65536));
            size_t got = head.empty() ? 0 : readAt(h, 0, head.data(), head.size());
            auto d = detectEncoding(head.data(), got, encodingChoice_);
            if (got == 0 && encodingChoice_ == EncAuto) d.encoding = EncUtf8;
            bom_ = d.bom;
            prefix_.assign((const char*)head.data(), std::min<size_t>(got, 64));
            { ExclusiveLock g(lock_); encoding_ = d.encoding; index_.clear(); index_.push(bom_); scanned_ = std::min<uint64_t>(bom_, size); size_ = size; }
            detected_ = true;
            if (size < bom_) { signal(); return; }
        }
        int enc; { SharedLock g(lock_); enc = encoding_; }
        bool wide = isWide(enc);
        uint64_t pos = scanned_, lastStart = index_.empty() ? 0 : index_.back(), maxLine = maxLineChars_;
        std::vector<uint64_t> found;
        while (pos < size && !stop_ && !resetRequested_) {
            size_t want = (size_t)std::min<uint64_t>(buffer.size(), size - pos);
            if (wide) want &= ~size_t(1);
            if (!want) break;
            size_t got = readAt(h, pos, buffer.data(), want);
            if (wide) got &= ~size_t(1);
            if (!got) break;
            const char* b = buffer.data();
            if (!wide) {
                const char* p = b; const char* end = b + got;
                while (p < end) {
                    const char* nl = (const char*)std::memchr(p, '\n', size_t(end - p));
                    if (!nl) break;
                    uint64_t next = pos + uint64_t(nl - b) + 1;
                    maxLine = std::max(maxLine, next - lastStart);
                    found.push_back(next); lastStart = next; p = nl + 1;
                }
            } else {
                const unsigned char* u = (const unsigned char*)b;
                bool le = enc == EncUtf16LE;
                for (size_t i = 0; i + 1 < got; i += 2) {
                    if (le ? (u[i] == 0x0A && u[i + 1] == 0) : (u[i] == 0 && u[i + 1] == 0x0A)) {
                        uint64_t next = pos + i + 2;
                        maxLine = std::max(maxLine, (next - lastStart) / 2);
                        found.push_back(next); lastStart = next;
                    }
                }
            }
            pos += got;
            uint64_t partial = wide ? (pos - lastStart) / 2 : pos - lastStart;
            {
                ExclusiveLock g(lock_);
                for (uint64_t x : found) index_.push(x);
                scanned_ = pos; maxLineChars_ = std::min<uint64_t>(std::max(maxLine, partial), 1u << 20);
                changedTick_ = GetTickCount64();
            }
            found.clear();
            signalThrottled();
        }
        if (prefix_.size() < 64 && pos > prefix_.size()) {
            std::string head((size_t)std::min<uint64_t>(pos, 64), '\0');
            head.resize(readAt(h, 0, &head[0], head.size()));
            if (head.compare(0, prefix_.size(), prefix_) == 0) prefix_ = head;
        }
        signal();
    }
    bool filterStep() {
        std::shared_ptr<const Matcher> m; uint64_t gen, from, complete;
        {
            SharedLock g(lock_);
            m = filter_; gen = filterGen_; from = filterScanned_;
            complete = index_.empty() ? 0 : index_.size() - 1;
        }
        if (!m || from >= complete || stop_) return false;
        Chunk c;
        if (!readChunk(from, complete, 4u << 20, c)) return false;
        std::vector<uint64_t> hits;
        matchChunk(c, *m, [&](uint64_t l) { hits.push_back(l); return true; });
        bool more;
        {
            ExclusiveLock g(lock_);
            if (gen != filterGen_ || c.generation != generation_) return true;
            filtered_.insert(filtered_.end(), hits.begin(), hits.end());
            filterScanned_ = c.first + c.count();
            more = filterScanned_ < complete;
        }
        if (more) signalThrottled(); else signal();
        return more;
    }
};

} // namespace logcore

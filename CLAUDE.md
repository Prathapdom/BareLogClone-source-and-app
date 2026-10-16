# BareLog — handoff notes for Claude

BareLog is a BareTail-style real-time log viewer: **Windows only**, native **C++17 + Win32**, no frameworks. It builds to a single static portable `dist\BareLog.exe` (about 1.3 MB). User-facing docs (features, shortcuts, settings format, limits) are in `README.md`. This file covers what you need in order to change the code safely.

The user's priorities, from the original brief:
1. Very fast and responsive.
2. Windows only.
3. A modern or native UI.
4. All of BareTail's core features, plus quality-of-life features such as dark mode.
5. The last session is persisted in a **settings file, not the registry**.

A sibling project, `F:\Prathap\Others\BareClone-source-and-app` (a BareGrep clone), uses the same build and test style.

## Build & test

```
powershell -ExecutionPolicy Bypass -File build.ps1            # icon, resources, exe, engine tests, UI tests
powershell -ExecutionPolicy Bypass -File build.ps1 -SkipTests # exe only
```

- **Compiler:** TDM-GCC 9.2 at `F:\Software\TDM-GCC-64\bin\g++.exe`, with windres from the same folder.
- **Close running copies first.** Stop any running `BareLog.exe` before rebuilding, or the link step fails because the exe is locked.
- **Always finish with a full `build.ps1`.** Both suites must print `PASS:`.
- **Engine tests:** `tests.cpp` → `dist\engine-tests.exe`. Plain asserts using a `REQUIRE` macro. Covers `logcore.hpp` and `settings.hpp`.
- **UI tests:** `ui-tests.cpp` → `dist\ui-tests.exe <path to BareLog.exe>`.
  - Launches the app with `SW_HIDE` and `BARELOG_SETTINGS` pointing at a temp INI file.
  - Drives it with `WM_COMMAND` and reads its state back through `WM_TESTQUERY`.
  - When you add a feature, add a check here.
- **Visual check:** `tools\Capture-Window.ps1 -Out x.png [-Send cmdId | -Post cmdId] [-Class BareLog.Dialog] [-W -H]`.
  - It captures the window with PrintWindow. Read the PNG to look at it.
  - Before launching the app for manual or visual checks, set `$env:BARELOG_SETTINGS` to a scratch path so the user's real `%APPDATA%\BareLog\settings.ini` is never touched.

## Files

| File | Role |
| --- | --- |
| `logcore.hpp` | Engine (namespace `logcore`). No UI code. |
| `settings.hpp` | INI settings (namespace `settings`). Includes `logcore.hpp`. |
| `main.cpp` | The whole UI, about 1,900 lines, one translation unit, organised by `// ----` section banners. |
| `tests.cpp`, `ui-tests.cpp` | Test suites |
| `app.rc`, `app.manifest` | Resources and manifest (PerMonitorV2, comctl32 v6, longPathAware) |
| `packaging/Make-Assets.ps1` | Draws `packaging/Assets/app.ico` (multi-size PNG ICO) |
| `build.ps1`, `Build.cmd` | Build entry points |

## Engine (`logcore.hpp`)

- **Threads:** each open file gets a `LogDoc` with one worker thread. Every 250 ms (`poll`) the worker:
  - stats the file;
  - indexes newly appended bytes in 1 MB chunks (`indexMore`);
  - then advances the live filter (`filterStep`).
- **File handle:** a `shared_ptr<FileHandle>` opened with full sharing. Reads are positional (`readAt`, using an OVERLAPPED offset).
- **Change detection:**
  - Rotation: the file ID or creation time changed.
  - Truncation: the size shrank.
  - Overwrite: the 64-byte prefix changed.
  - Any of these calls `resetIndex()`, which increments `generation` and clears the filter.
  - Deletion: `markMissing()`, and the file is picked up again when it reappears.
- **Line index:** `LineIndex` uses about 4 bytes per line (a 64-bit base every 1024 lines, 32-bit deltas, and an overflow side table).
  - `index_[0]` is the BOM length.
  - `complete = index.size() - 1`.
  - `lines = complete + (partial last line ? 1 : 0)`.
- **Locking:** state sits behind an SRWLOCK. The UI only takes the shared lock, via `info()`, `readLines()`, `filteredLine()` and `rowForLine()`.
- **Notifying the UI:** the worker calls the `notify` callback, which in main.cpp posts `WM_DOCCHANGED`.
  - Notifications are coalesced with a pending flag and a 50 ms throttle.
  - The UI must call `doc->acknowledge()` when it handles one.
- **Encodings:** BOM detection, a UTF-16 heuristic without a BOM, a UTF-8 validity check, then the ANSI fallback. A per-tab forced encoding uses `setEncoding`.
- **Matcher:** `Matcher::make(pattern, matchCase, regex)`.
  - Literal patterns use a Boyer-Moore-Horspool byte fast path for UTF-8 and single-byte ANSI.
  - Regex uses `std::wregex` ECMAScript, capped at 4,096 characters.
  - Case-insensitive matching folds ASCII only.
- **Filter:** `setFilter(matcher or nullptr)` scans incrementally into `filtered_`, a list of line numbers. It only scans complete lines.
- **Find:** `find(from, forward, matcher, cancel)` blocks, so the UI runs it on the tab's `finder` thread.

## UI (`main.cpp`)

**Windows and drawing**
- Window classes: `BareLog.Main` (the frame, which custom-paints all the chrome), `BareLog.View` (the virtual log view) and `BareLog.Dialog` (the modal dialog base).
- The query box is the only standard child control: an `EDIT`, id 200, subclassed by `EditSubclass`.
- `buildChrome()` lays out every clickable rectangle into `hits` (buttons, tabs, tab-close buttons and chips) and registers tooltips.
  - `paintChrome()` draws them.
  - `MainProc` mouse handlers do hit-testing and call `execute(cmd)`.
  - To add a button, add it in `buildChrome` (the `left`/`right` vectors or a `chip(...)` call) and handle it in `execute`.
- Commands are in `enum Cmd`:
  - Plain IDs from 100, in this order: Open 100, Recent, Follow 102, Wrap 103, LineNumbers, Highlight 105, Theme, More, Case, Regex, Filter 110, Prev, Next 112, Find, Goto, CloseTab, CloseOthers, CloseAll 117, …
  - Ranges: tabs 1000+, tab close 2000+, recent 3000+, theme 3100+ (0 = system, 1 = light, 2 = dark, 3 = AMOLED), encoding 3200+, tab size 3300+.
  - **Only add new IDs at the end of the plain list.** `ui-tests.cpp` hard-codes numbers (102, 103, 110, 112, 3100+).
- `struct Tab` holds the `LogDoc`, the persisted `TabState st`, the scroll position (`top` row, `sub` wrapped segment, `hcol`), the selection (`anchor`/`caret` as **line** numbers), a `cache` of decoded and tab-expanded lines, and the find thread.

**Rows versus lines**
- When `filtering(t)` is true, rows map to file lines through the filter. Always convert with `rowLine` and `lineRow`, and size things with `rowCount`.

**Scrolling and follow**
- Scroll state uses `scrollBy`, `ensureVisible`, `clampTop`, `goEnd` and `atEnd`. These are wrap-aware, so call `viewMetrics(t)` first.
- After any user-driven scroll, call `afterUserScroll(t, oldTop, oldSub)`. It sets follow to `atEnd()`, which is how scrolling up pauses and returning to the bottom resumes.
- `onDocChanged` handles growth, generation resets (which clear the cache and selection), unread dots, and `pendingTop`. `pendingTop` restores a saved scroll position once indexing reaches it; `UINT64_MAX` means none.

**Painting**
- `paintView` is double-buffered.
- `drawRun` uses `ExtTextOutW` with a fixed per-character dx so columns stay aligned.
- Each line gets its colour from the first matching highlight rule (`ruleFor`). Query matches are painted on top.

**Themes**
- The `Palette` structs are `LightPalette`, `DarkPalette` and `AmoledPalette`. The current one is in the global `P`.
- `applyTheme()` handles the DWM title bar and the undocumented uxtheme ordinals 133/135/136 (the `darkmode` namespace) for dark menus and scrollbars.
- Icons come from the Segoe Fluent Icons font, with Segoe MDL2 Assets as the fallback (`pickIconFace`, `hasGlyph`).
- Sizes are in DIPs; convert with `px(v)`. Window sizes go through `dpi`.

**Dialogs**
- Subclass `struct Dialog` and run it with `runModal(d, title, w, h)` (sizes in DIPs).
- Helpers: `child()`, `edit()` (draws a rounded outline) and `button(id, text, …, kind)`. Button kinds: 0 normal, 1 primary, 2 toggle (`toggleOn[hwnd]`), 3 colour swatch (`swatch[hwnd]`).
- Existing dialogs: `PromptDialog` (Go to line) and `HighlightDialog`.

**Persistence**
- `scheduleSave()` debounces saves by 400 ms and calls `saveNow()`, which also runs on close. `saveNow()` collects the tabs, the window placement and `S`.

**Single instance**
- A mutex named `Local\BareLog-<FNV hash of the settings path>` plus the window property `BareLog.Instance`.
- A second process forwards its paths with `WM_COPYDATA` (dwData `0x42A1E106`, newline-separated) and exits.

**Test hook**
- `WM_TESTQUERY` (`WM_APP+100`), with wParam:
  - 0 tabs, 1 lines, 2 follow, 3 rows, 4 top, 5 active tab, 6 dark (0, 1, or 2 = AMOLED)
  - 7 filter done, 8 caret, 9 wrap, 10 finding, 11 unread, 12 fullRows, 13 save now
- If you add a code, also add it to the enum in `ui-tests.cpp`.

## Settings file

- Location: `%APPDATA%\BareLog\settings.ini` (UTF-8). Override it with the `BARELOG_SETTINGS` environment variable.
- Sections: `[settings]`, `[highlight]` (`rule=enabled|case|regex|useBack|RRGGBB|RRGGBB|text`), `[recent]` (`file=`) and `[session]` (`active=`, `tab=follow|top|enc|case|regex|filter|path|query`).
- Fields are separated by `|`. Only `%`, `|`, CR and LF are escaped (as `%25`, `%7C`, `%0D`, `%0A`), so backslashes in paths are written as-is.
- An empty `[highlight]` section means no rules. A missing file means the defaults.
- Saves are atomic: write `.tmp`, then `MoveFileEx`.

## Toolchain gotchas (they cost time before)

**TDM-GCC 9.2 headers**
- `far` is a macro, so don't name a variable `far`.
- `ERROR_DIRECTORY_NOT_SUPPORTED` and `SetProcessDpiAwarenessContext` / `DPI_AWARENESS_CONTEXT_*` are missing. Load the function with `GetProcAddress`.
- `GetTickCount64` needs `_WIN32_WINNT 0x0A00` defined before `<windows.h>`.
- `std::max`/`std::min` on `LONG` (RECT fields) and `int` won't compile. Use `std::max<LONG>(…)`.
- Builds use `-Wno-missing-field-initializers` because of the `STRUCT s{sizeof s}` idiom. Builds should otherwise be free of warnings.

**Scripting on this machine**
- `python` is **Python 2**: no f-strings and no `open(encoding=)`.
- `sed` with backslashes or `|` in C++ string literals has mangled code before. Prefer the Edit tool for anything that isn't trivial.

**PowerShell and windows**
- `FindWindowW` called from PowerShell was unreliable. Use `(Get-Process BareLog).MainWindowHandle`, or EnumWindows by class as `tools\Capture-Window.ps1` does.
- `SendMessage` of a command that opens a modal dialog (Highlight, Go to line, Open) blocks the caller. Use `PostMessage` instead.

## Status (2026-10-16)

- Version 1.0 is complete. All tests pass.
- Screenshots were checked for light, dark, AMOLED, narrow (compact command bar), wrap, the empty state and the highlight dialog.
- Not exercised by hand yet: the context menus, the Open, font and colour pickers, drag and drop, multi-monitor DPI changes, and very large files (multiple GB).
- The folder is not a git repository yet.

Ideas not yet built (candidates if the user asks for features):
- Bookmarks
- Multiple filters, or "exclude" filters
- Copying with rule colours
- Tail several files merged into one view
- Notifications on a pattern match
- A pause indicator in the tab
- Saving the filtered output to a file

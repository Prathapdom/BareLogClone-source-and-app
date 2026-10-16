# BareLog

A fast, native real-time log viewer for Windows, in the spirit of BareTail. It is a single portable `BareLog.exe` (about 1.3 MB) written in plain C++17 and Win32, with no runtime dependencies.

## Features

**Tailing**
- Opens files of any size instantly. A background thread builds a compact line index (about 4 bytes per line) and you can scroll while it runs.
- New lines appear as they are written. The file is checked every 250 ms and the file handle is opened with full sharing, so the writing application is never blocked.
- Detects truncation, log rotation (rename and recreate), overwrites and deletion. A file that does not exist yet shows "Waiting for file" and starts showing lines as soon as it is created.
- **Follow tail** is on by default:
  - Scrolling up pauses it.
  - Scrolling back to the bottom, pressing End or pressing Ctrl+T resumes it.
- Tabs for multiple files. A dot on a background tab means new lines arrived there.

**Finding and filtering**
- A single find box for both literal text and ECMAScript regular expressions, with a Match case toggle.
- F3 and Shift+F3 search the whole file on a background thread and wrap around at the end.
- **Filter** (Ctrl+L) shows only matching lines, like `tail -f | grep`. It updates live as matching lines are written, and the status bar shows "N of M lines".
- Matches are highlighted inline.

**Highlighting**
- Rule-based line colouring: a text colour plus an optional background colour.
- Rules can be literal or regex, with a match-case option, and are checked top to bottom (the first match wins).
- Default rules cover FATAL, ERROR, Exception, WARN and DEBUG. Edit them with Ctrl+H.

**Viewing**
- Themes: follow the Windows setting, Light, Dark or AMOLED black. Dark mode also covers the title bar, menus and scrollbars.
- Word wrap, line numbers, tab width (2, 4 or 8), any fixed-pitch font, and zoom with Ctrl+wheel.
- Encoding is detected automatically: UTF-8, UTF-16 LE/BE (with or without a BOM) and the ANSI code page. You can force an encoding per tab from **⋯ → Encoding**.
- Lines can be selected with click, Shift+click, drag or Shift+arrows and copied with Ctrl+C.
- File actions: Go to line, Open in editor, Show in folder and Copy path.
- Drag and drop files onto the window.
- Single instance: running `BareLog.exe file.log` again opens the file as a new tab in the existing window.
- Per-monitor DPI aware, and the window adapts to narrow widths.

## Persistence

All settings are stored in a UTF-8 INI file, not the registry:

```
%APPDATA%\BareLog\settings.ini
```

- **Contents:**
  - Theme, font, view options and highlight rules.
  - Up to 15 recent files.
  - The last session: open tabs, the active tab, the scroll position (or follow state), the find text and filter state, the forced encoding, and the window position.
- **Reopening:** the last session reopens when you start BareLog. To turn this off, untick **⋯ → Reopen files on startup**.
- **Saving:** the file is saved shortly after each change and again on exit. Saves are atomic: BareLog writes a temporary file and then renames it.
- **Editing by hand:** you can edit the file while BareLog is closed.
- **Portable or test setup:** set the `BARELOG_SETTINGS` environment variable to a full file path to use a different settings file.

## Keyboard shortcuts

| Keys | Action |
| --- | --- |
| Ctrl+O | Open files |
| Ctrl+W / Ctrl+F4 | Close tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+F | Focus find box (or just start typing in the log) |
| Enter / F3 | Find next |
| Shift+Enter / Shift+F3 | Find previous |
| Ctrl+L | Toggle filter |
| Esc (in find box) | Clear text, then return to the log |
| Ctrl+G | Go to line |
| Ctrl+T | Toggle follow tail |
| End / Ctrl+End | Jump to the last line and resume following |
| Home | Jump to the first line |
| Alt+Z | Toggle word wrap |
| Ctrl+H | Highlighting rules |
| Ctrl+C / Ctrl+A | Copy selected lines / select all |
| Ctrl+= / Ctrl+- / Ctrl+0 / Ctrl+wheel | Zoom in / out / reset |
| F5 | Reload the file from scratch |

## Building

Requirements:
- **Compiler:** a MinGW-w64 g++ with C++17 support. The script defaults to `F:\Software\TDM-GCC-64\bin\g++.exe` and falls back to `g++` on your PATH.
- **Icon generation:** Windows PowerShell 5.1.

Build:

```
Build.cmd
```

or

```
powershell -ExecutionPolicy Bypass -File build.ps1 [-Compiler path\to\g++.exe] [-SkipTests]
```

What the script does:
- Generates the icon (`packaging/Make-Assets.ps1`).
- Compiles the resources.
- Builds `dist\BareLog.exe`.
- Builds and runs both test suites (unless `-SkipTests` is passed):
  - `engine-tests.exe`: decoding, the line index, the matcher, live growth, partial lines, live filter, find, truncation, rotation, delete and reappear, UTF-16, a 200k-line file, and a settings round-trip.
  - `ui-tests.exe`: launches the app hidden with a temporary settings file and checks tail follow, pause and resume, live filter, find, wrap, truncation, themes, single-instance forwarding, a file that appears later, persistence and session restore.

## Source layout

| File | Contents |
| --- | --- |
| `logcore.hpp` | Engine: file watching, line index, encodings, matcher, filter, find |
| `settings.hpp` | INI settings load/save |
| `main.cpp` | Win32 UI |
| `tests.cpp`, `ui-tests.cpp` | Test suites |
| `app.rc`, `app.manifest`, `packaging/` | Resources, manifest, icon generator |
| `tools/Capture-Window.ps1` | Screenshot helper for visual checks |
| `CLAUDE.md` | Developer handoff notes (architecture, conventions, gotchas) |

## Limits

- Lines longer than 32,768 characters are shown truncated. Copying a line gives you up to 1 MiB of it.
- Regular expressions are matched one line at a time. Patterns longer than 4,096 characters are rejected.
- Files are polled rather than watched with change notifications. This is reliable on network shares and with writers that keep the file open, at the cost of up to 250 ms latency.

# NorthRay Browser: Development and Reference Manual

| | |
|---|---|
| Application | NorthRay Browser (`NorthRayBrowser.exe`) |
| Version at the time of writing | 0.9.2 (`apps/browser/VERSION`) |
| Status | **Preview**, released |
| Source | `apps/browser/` (C++20; Microsoft Edge WebView2; UI in `libs/cjui`) |
| Architecture notes | `apps/browser/docs/ARCHITECTURE.md` |
| Release notes | `shared/metadata/releases/browser.json` |
| Manual last checked against code | 26 November 2026 |

Status labels used in this manual:
- **Implemented**: in the code and released.
- **Partial**: some of it is built; the rest is noted.
- **Planned**: agreed with the project director, not built.
- **Proposed**: suggested, not yet agreed.

"Tested in Wine" means it ran under Wine without the engine, using `--no-engine`. Anything that needs WebView2 must be checked on Windows.

---

## 1. Overview

**Purpose.** A desktop web browser for Windows in the NorthRay look, with creative tools: reference boards and a colour picker.

**Audience.** Everyday browsing, plus artists and writers who collect references.

**Philosophy.**
- Use the engine Windows already keeps secure. Pages are shown by the Edge WebView2 Evergreen runtime with its sandbox always on.
- Refuse certificate errors.
- Never send a file path or a local network address to a search engine.
- No NorthRay telemetry. No extensions, ever (the director's decision). No sync: local backup files only.

**Where it sits.** One of the four suite apps. It shares cjui, the palette and dialogs with the others. Links to Capture and Writing Studio are planned (section 2).

## 2. Features

### Implemented (0.1 to 0.9.2)

**Tabs and windows**
- Open, close and reopen tabs; middle-click closes. The wheel over the strip switches tabs.
- Hover preview with a page picture. Close tabs to the right.
- Tabs down the side (vertical tabs), shown on the night sky.
- Drag a tab out to give it its own window.
- Last time's tabs come back, loaded lazily.
- A question before closing a window with 5 or more tabs.
- A speaker on tabs playing sound, with mute.

**Address bar**
- Opens addresses, local files and searches. Google is the default; other engines can be chosen.
- The host is drawn in full ink and the rest of the address fainter.
- Copy link, and Paste and go.
- A padlock or "Not secure". Certificate errors are refused.

**Start page**
- Greeting and search; bookmarks as tiles; recently closed tabs.
- Your own background picture, a clock, Notes to yourself.
- Optional weather from Open-Meteo, for a town you type.

**Bookmarks**
- Star (Ctrl+D), bookmarks bar (Ctrl+Shift+B) and a bookmarks page (Ctrl+Shift+O).
- Folders, search, rename, reorder and move.
- Import and export in the Chrome, Edge and Firefox format.
- A daily backup.

**History, downloads, blocking**
- History (Ctrl+H) is kept 90 days, by day, with search, Remove and Clear.
- Downloads page (Ctrl+J) with pause, resume and cancel, and a progress ring on the toolbar button.
- Ad and tracker blocking: about 120 domains written for NorthRay, blocked only when on a different site from the page. A shield in the address bar shows the count and turns blocking off per site or everywhere.

**Saved passwords and form filling**
- Handled by the engine's own password manager.
- Encrypted with a key protected by the Windows account.
- Can be turned off, or all cleared.

**Creative tools**
- Reference boards: right-click a picture, then "Save picture to a reference board".
- Pick a colour from the page, with a loupe and the hex value.
- Main colours of a saved picture.
- Each board remembers its zoom and scroll position.

**Video pop-out (Alt+P)**
- Moves the page's video into an always-on-top window that keeps playing.
- Shows only the video and keeps the video's shape.
- Back 10 s, play or pause, forward 10 s, and Skip ad when the site offers one.

**Other**
- Back up and restore: one zip holding bookmarks, reference boards, notes, settings and (optionally) history. Passwords and the downloads list are never included. Restore keeps the replaced files in a "Before restore" folder.
- Per-site zoom; smooth scrolling; no white flash in dark mode.
- An offer to become the default browser: once, with a setting.
- Full screen, print, developer tools; light and dark themes.
- The menu links to NorthRay Software's website (0.9.2).

### Needs checking on Windows (built, engine-dependent)

- Page pictures in tab previews.
- The speaker and mute.
- Per-site zoom.
- Copy link.
- Site icons arriving on the bookmarks bar.
- The dark background before a page paints.
- Smooth scrolling.
- Real download operations.
- The video pop-out's window, which was checked only for building. Its scripts were tested in Chromium.

### Planned (the director's answers of 15 and 18 Nov 2026; none in the release notes yet)

- Screenshots: region, visible area or full page.
- Reader view.
- Two pages side by side.
- A notes panel per site.
- Tab groups.
- Workspaces with their own tabs.
- Send to Capture (a page or video as a browser source).
- Save as PDF.
- An always-on-top reference window with a see-through slider.
- Font identifier.
- Translate via Google Translate.
- Double-click a word for its meaning.
- Search shortcuts (yt, w, art).
- Forced dark look.
- Cookie banners declined automatically.
- A password generator.
- Named saved sessions.
- A QR code for the page.
- A media button for whatever is playing.
- Pinned app tabs.
- Sleeping tabs.
- Better find in page.
- Bookmark tags and thumbnails.

### Known limitations

- No UI Automation provider in cjui, so the drawn tabs and toolbar aren't announced by screen readers. The address bar and the page are.
- Closing a tab doesn't run the page's "leave this page?" prompt.
- No profiles and no private windows, both by the director's choice.
- No Chrome Web Store or extensions, by decision.
- If CEF were ever needed (extensions, custom network stack, Linux or macOS), the engine is isolated in `src/engine.*` (see the architecture document's §1).

## 3. Interface and user experience

- **Top:** the tab strip is the title bar, with the caption buttons at its right. Below it, the toolbar: back, forward, reload, home, the address bar, the downloads button, the ⋯ menu. Then the page. There is no status bar: link targets show in WebView2's bubble, and messages in a small layered note window.
- **Internal pages** (`northray://`): start, bookmarks, history, downloads, references.
- **Principles:** the suite's calm buttons, palette and menus (`cjui::showMenu`, themed, keyboard-driven, searchable when long). No scroll bars in NorthRay-drawn UI.
- **Workflows:**
  - Browse.
  - Bookmark, then organise on the bookmarks page.
  - Collect references: right-click save, then open Reference boards.
  - Pick colours.
  - Pop a video out while working.
  - Back up, then restore on another PC.

## 4. Technical documentation

| Item | Detail |
|---|---|
| Language | C++20 |
| Engine | Microsoft Edge WebView2 SDK 1.0.4314.35 (`libs/third_party/webview2-sdk`); Evergreen runtime (not shipped) |
| UI | `libs/cjui` |
| Storage | SQLite (schema-versioned stores in `core/`) and JSON settings |
| Other | miniz (backup zips), WIC (icons and pictures) |
| Build | CMake via the root `CMakeLists.txt`; MinGW cross-build (`tools/build-windows.sh`) |

**Structure**

`core/` is portable C++ with no Windows or engine code, and is unit-tested:

| Module | Contents |
|---|---|
| `omnibox` | What typed text means: address, file, search, or refused with a reason |
| `url` | Display text and site kind |
| `load_errors` | Plain-words error pages |
| `tab_model` | Tab order and closing rules |
| `bookmarks` | `BookmarkStore` |
| `downloads`, `history`, `references` | The SQLite stores |
| `blocker` | The domain list and `shouldBlock` |
| `backup` | Make, unpack and apply-pending |

`src/`:

| File | Contents |
|---|---|
| `engine.*` | The only WebView2 code (`Engine`, `Page`) |
| `browser_window.*` | The window |
| `browser_bookmarks.cpp`, `browser_downloads.cpp`, `browser_history.cpp`, `browser_references.cpp`, `browser_start.cpp`, `browser_popout.cpp` | One file per feature |
| `settings.*` | `settings.json` |
| `main.cpp` | Start-up, `--no-engine`, `--after-restore`, a single session per user |

**Data storage** (`%LOCALAPPDATA%\NorthRay\Browser\`)

| Path | Contents |
|---|---|
| `settings.json` | Settings, including the saved session, per-site zoom and blocking exceptions |
| `User\bookmarks.db`, `history.db`, `references.db`, `downloads.db` | SQLite stores, in WAL mode |
| Reference pictures, start page notes and background | Kept beside the stores |
| The engine's own profile folder | Passwords, cookies, cache |

**External integrations**
- Search engines (Google by default).
- Open-Meteo, only when weather is switched on.
- Windows default-apps registration.
- NorthRay's website link.

## 5. Development

**Status:** preview.

**Milestones**
- 0.1: foundation.
- 0.2: bookmarks, start page, passwords.
- 0.3: history, vertical tabs.
- 0.4: downloads, blocking.
- 0.5: reference boards, colour picker.
- 0.6: start page extras, default browser.
- 0.7: video pop-out.
- 0.8: backup and restore.
- 0.9: polish.
- 0.9.1: calm controls and contrast.
- 0.9.2: website link.

**Outstanding:** the planned list above, UI Automation, and the "leave page?" prompt.

**Technical debt and notes**
- The blocklist is hand-maintained (`core/blocker`).
- WebView2 features vary with the runtime version; check `ICoreWebView2_*` interfaces before use.

**Priorities** (the director's ranking, 15 Nov): first, links to other apps; second, creative tools.

## 6. Maintenance

**Run**
- Install with `NorthRayBrowser-Setup`, or run `build/windows/apps/browser/NorthRayBrowser.exe`.
- `--no-engine` opens the interface without WebView2 (a development aid for screenshots and Wine).

**Build**
- `tools/build-windows.sh` builds the whole suite.
- Native tests: `cmake -S . -B build/native -G Ninja && ninja -C build/native browsercore_tests`.

**Test**
- `browsercore_tests`: 315 checks, all passing on 26 Nov 2026.
- Run with `ASAN_OPTIONS=detect_leaks=0 ./build/native/apps/browser/browsercore_tests`, or `ctest` in `build/native`.

**Troubleshooting**

| Problem | Cause and fix |
|---|---|
| Blank pages or "engine not available" | The WebView2 Runtime is missing; Windows 11 includes it |
| A site broken by blocking | Shield, then allow on this site |
| A restore didn't apply | It applies at the next start. Look in `Before restore <date>` for the replaced files |

**Implementation notes**
- Never call a download handle from its own callback; release it on a timer.
- `Page::reparent` moves a WebView without reloading it. The pop-out and tab-dragging rely on this.

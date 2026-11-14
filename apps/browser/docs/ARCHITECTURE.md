# NorthRay Browser: architecture

Status: **0.1.0, Stage 1 (technical foundation)**. A working, sandboxed browser window in the NorthRay design language,
with tabs, an address bar, navigation, NorthRay error and start pages. Last updated 14 November 2026.
Developed by the NorthRay Software project. Contact: NorthRaySoftware@protonmail.com.

## 1. Engine decision: WebView2, not CEF

The brief preferred the Chromium Embedded Framework. Research showed it's workable, but it has costs that matter for a
small project, and the project director chose **Microsoft Edge WebView2** on 13 November 2026 after this comparison.

| | CEF (official binaries) | WebView2 (Evergreen runtime) |
|---|---|---|
| Engine | Chromium, shipped by us (~250 MB per release) | Chromium (Edge), part of Windows 11, updated by Microsoft |
| Security updates | We ship a new build roughly every 4 weeks, forever (the brief's Stage 10 "sustainable update process") | Microsoft updates it automatically, usually within days of Chrome |
| Sandbox | Since M138 the `cef_sandbox` library can't be linked outside Chromium's build: the app must be a DLL loaded by CEF's `bootstrap.exe` | Edge's sandbox, always on; nothing to link |
| Toolchain | MSVC only (CEF's binaries, `/MT`, no exceptions/RTTI in its wrapper); can't build in our cloud workspace (CEF's CDN and Microsoft's toolchain downloads are blocked there) | MinGW, like the rest of the suite; builds and links in the cloud workspace |
| Control | Full: custom schemes, network interception, Chrome-style or Alloy-style runtime | Less: no custom network stack, no Chrome Web Store, fewer low-level hooks |
| Media | Proprietary codecs (H.264, AAC) need a custom Chromium build; no Widevine | Edge's codecs and PlayReady/Widevine DRM where Edge has them |
| Privacy | Nothing goes to Microsoft or Google unless we add it | The runtime's diagnostic data follows Windows' settings; SmartScreen (on) checks addresses with Microsoft |

What would make us revisit: needing Chrome Web Store extensions, custom network-level blocking that WebView2 can't do,
or a Linux/macOS version. The engine is isolated in `src/engine.*` (the only files that include WebView2), so a CEF
engine could replace it behind the same `Engine`/`Page` interface.

The CEF research (branches 150 LTS / 156 extended / 158 beta as of November 2026; the M138 bootstrap arrangement;
Chrome style's `IsReadyToBeClosed()` close flow; CEF's `/GR-` and `_HAS_EXCEPTIONS=0` flags) is kept here for that day.

## 2. Shape

```
apps/browser/
  core/            portable C++20, no Windows or engine code; unit-tested on Linux and as a Windows .exe
    omnibox        what typed text means: address, file, search, or refused (with a reason)
    url            display text, site kind (secure, not secure, file, internal)
    load_errors    plain-words error pages (WebView2's WEB_ERROR_STATUS values)
    tab_model      tab order, where new tabs go, which tab activates on close, recently closed
  src/
    engine.*       the only WebView2 code: Engine (environment) and Page (one tab's view)
    browser_window.*  the window, drawn with cjui: tab strip, toolbar, address bar, pages, status bar
    settings.*     settings.json (versioned, read defensively, saved atomically)
    main.cpp       start-up, one window per session (a second launch hands its addresses over), message loop
  tests/           browsercore_tests (169 checks)
libs/cjui          shared with the other apps; additions: cjui/northray.hpp (palette, night sky, aurora band),
                   WindowSpec::style and ::quitOnDestroy, drawPixels(..., useAlpha)
libs/third_party/webview2-sdk   Microsoft.Web.WebView2 1.0.4314.35 (see its README)
```

Later modules (profiles, storage, bookmarks, history, downloads, creative tools) go beside these as they're built,
with their logic in `core/` where it can be tested without an engine.

## 3. Look

NorthRay Browser follows the Launcher (0.17+), not the older `docs/DESIGN.md` (which still describes the warm-paper,
3 px look). The tokens are `shared/metadata/brand/tokens.json`; the palette and night sky now live in
`libs/cjui/northray.*`, and the Launcher draws from the same code.

- **Tab strip**: the night sky with the aurora band, like the Launcher's sidebar. The active tab is the toolbar's colour,
  joined to it. Site icons, a spinner while loading, a small NorthRay tile on the start page.
- **Toolbar**: back, forward, reload/stop, home, the address bar (a borderless native edit control in a drawn 8 px frame,
  as in the Writing Studio, so text input, IME, clipboard and screen readers are Windows' own), the menu.
- **Site indicator**: a padlock for https; "Not secure" for http; "Certificate problem"; "File"; "NorthRay" for the
  engine's own pages. Click it for an explanation. `http://` always stays visible in the address.
- **Pages drawn by NorthRay**: the start page (night banner, keyboard shortcuts; nothing fake), error pages, the
  engine-missing page. Status bar: link under the pointer, loading, messages.
- Light and dark follow Windows (or the menu). No scroll bars in the window; web pages use overlay scroll bars.

## 4. Security and privacy (as built)

- Web pages are untrusted: **no host objects and no web messages** are enabled for them, so there's no channel from a
  page into the app. Internal NorthRay pages (later) will get a separate, origin-checked channel.
- **No extra engine switches**; Edge's renderer sandbox is untouched. Checked on the development PC with `edge://sandbox`:
  renderers at Lockdown with Untrusted integrity, GPU at Limited/Low.
- **Certificate errors are always refused** (`ServerCertificateErrorDetected` → cancel) with a plain explanation; no
  "continue anyway" in this version.
- The address bar never runs `javascript:` or opens `data:` pages; `\\server\share` and `file://server/...` are refused
  (they'd offer the Windows sign-in to that server); local, private and intranet addresses are opened, never searched.
- Addresses from the command line (a second launch, later the default-browser hand-off) are limited to http, https and
  local files.
- **Saved passwords and form autofill are off** until NorthRay designs credential storage (Windows Credential
  Manager / DPAPI is the likely base).
- **SmartScreen is on** (`IsReputationCheckingRequired`). It's real protection against phishing and malware downloads,
  and it sends addresses to Microsoft; the privacy policy and Settings must say so. *Decision pending with the director.*
- Single sign-on with the Windows account is off. No telemetry of our own.
- Browsing data (`%LOCALAPPDATA%\NorthRay\Browser\Engine`) is separate from settings, and will be separate from creative
  data, so clearing browsing data can never delete someone's work.

## 5. Tested

Unit tests: `browsercore_tests` (169 checks, Linux with AddressSanitizer/UBSan and as a Windows .exe under Wine).
Build: the whole suite with MinGW-w64 (no warnings in the browser). Under Wine (no engine): window, tabs, address bar
rules, themes at 150 %, clean exit.

On the development PC (Windows 11, WebView2 runtime present), 14 November 2026, by computer use:

| Check | Result |
|---|---|
| Starts, engine runs, version resource correct | Pass |
| example.com renders; padlock; site icon in tab | Pass |
| Unknown host → NorthRay error page; "Search for …" works | Pass (after fixing: the start page hid the first page; stale address and padlock on error pages) |
| expired.badssl.com → "identity couldn't be confirmed" | Pass (after handling `ServerCertificateErrorDetected`) |
| DuckDuckGo search results; overlay scroll bars | Pass |
| YouTube video plays | Pass |
| F11 full screen and back | Pass |
| Maximise: page re-lays out | Pass |
| `edge://sandbox`: renderers locked down | Pass |
| Closing: process exits (exe could be replaced at once) | Pass |
| Second launch hands over to the running window | Pass |

Not yet checked: `target=_blank` / `window.open` pop-ups (built, untested), page-initiated full screen (YouTube's
button), whether any `msedgewebview2.exe` outlives the app (Task Manager runs elevated, so it couldn't be inspected),
printing, downloads (WebView2's own download bubble appears; NorthRay's comes in 0.3), screen readers.

## 6. Known limitations in 0.1

- One window; pop-up windows open as tabs. Tab overflow beyond the strip isn't scrollable yet (Ctrl+Tab reaches them).
- Closing a tab doesn't run the page's "leave this page?" prompt.
- Menus are Windows' native popup menus (not NorthRay-drawn); no Settings page yet (theme and search engine are in the menu).
- No bookmarks, history, downloads panel, profiles, private windows, find-in-page UI or session restore yet (stages 0.2–0.4).
- cjui has no UI Automation provider, so the drawn controls (tabs, toolbar buttons) aren't announced by screen readers;
  the address bar and the page are.
- The `--no-engine` switch is a development aid (interface without an engine, for screenshots); the status bar says so.

## 7. Next (Stage 2, 0.2)

Multiple windows and real pop-up windows; tab overflow and drag between windows; find in page; zoom indicator;
keyboard focus order across tab strip, toolbar and page; the "leave page?" prompt on close; then Stage 3's SQLite
storage (bookmarks, history, sessions) with migrations, beside `studiocore`'s proven pattern.

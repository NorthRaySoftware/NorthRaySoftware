# NorthRay Browser: architecture

Status: **0.2.0**: Stage 1 (technical foundation) plus bookmarks, a new start page and saved passwords (from Stage 3/4,
asked for early by the director). Last updated 14 November 2026.
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
    bookmarks      BookmarkStore: SQLite, schema-versioned (user_version), folders, import/export, daily backup, recovery
  src/
    engine.*       the only WebView2 code: Engine (environment) and Page (one tab's view)
    browser_window.*  the window, drawn with cjui: tab strip, toolbar, address bar, pages, status bar
    browser_bookmarks.cpp  star, bookmarks bar, menus, the bookmarks page (northray://bookmarks), import/export,
                   password and form settings
    browser_start.cpp  the start page
    settings.*     settings.json (versioned, read defensively, saved atomically)
    main.cpp       start-up, one window per session (a second launch hands its addresses over), message loop
  tests/           browsercore_tests (213 checks)
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

- **Title bar = tab strip** (0.1.1): Windows' caption is removed (`WM_NCCALCSIZE` keeps the side and bottom sizing borders, takes the caption; maximised, the top keeps the border's height so nothing hangs off the screen). `WM_NCHITTEST`: the top edge resizes, the strip's empty space is `HTCAPTION` (drag, double-click, window menu, Aero snap), maximise is `HTMAXBUTTON` so Windows 11 shows snap layouts (its clicks are handled in `WM_NCLBUTTONDOWN/UP`, never by Windows, which would draw an old button). Minimise and close are ordinary cjui hits. At least 44 DIPs of strip are always left empty to drag by. Wine without a virtual desktop shows the client offset by the old caption; `wine explorer /desktop=...` shows it correctly.
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
- **Saved passwords and form filling** (0.2.0, asked for by the director): the engine's own password manager, the one
  Microsoft Edge has (`ICoreWebView2Profile6::IsPasswordAutosaveEnabled` / `IsGeneralAutofillEnabled`). Chromium keeps
  them in the profile's `Login Data` / `Web Data` under the engine folder, encrypted with an AES key that is itself
  protected with DPAPI for the Windows account (os_crypt). NorthRay's code never sees a password. On by default, with
  "Offer to save passwords" and "Fill in forms automatically" in the menu; "Delete saved passwords" uses
  `ICoreWebView2Profile2::ClearBrowsingData(PASSWORD_AUTOSAVE | GENERAL_AUTOFILL)` (creating a page for the purpose if
  none is open). Private pages (when they exist) always have both off. WebView2 has no API to list or edit saved
  passwords, so there's no password list yet; a NorthRay-designed vault would be the way to add one.
- **Bookmarks** are in `%LOCALAPPDATA%\NorthRay\Browser\User\bookmarks.db` (SQLite, WAL), apart from the engine's data.
  Only http, https, file and northray addresses can be bookmarked (never `javascript:`, including on import). The file is
  checked on open (`quick_check`); a damaged one is kept as `.damaged` and replaced from `.backup` (made at most daily
  with `VACUUM INTO`), or started afresh, and the person is told.
- **SmartScreen is on** (`IsReputationCheckingRequired`). It's real protection against phishing and malware downloads,
  and it sends addresses to Microsoft; the privacy policy and Settings must say so. *Decision pending with the director.*
- Single sign-on with the Windows account is off. No telemetry of our own.
- Browsing data (`%LOCALAPPDATA%\NorthRay\Browser\Engine`) is separate from settings, and will be separate from creative
  data, so clearing browsing data can never delete someone's work.

## 5. Tested

Unit tests: `browsercore_tests` (213 checks, Linux with AddressSanitizer/UBSan and as a Windows .exe under Wine), including
bookmarks: order, moves, folders, safe addresses, export and re-import, another browser's file, on-disk persistence,
the daily backup and recovery from a damaged file. 0.2.0 under Wine (no engine, virtual desktop): start page, tiles,
bookmarks bar and folder menu, bookmarks page (rename, search), menus in both themes. Not yet checked on Windows:
the star on real pages, saving and filling a password, deleting saved passwords, import/export file dialogs.
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
- No Settings page yet (theme and search engine are in the menu). Menus are NorthRay-drawn (`cjui::showMenu`, 0.1.2) in the app's palette: a modal loop like `TrackPopupMenu(TPM_RETURNCMD)`, with mouse capture on the top menu so a click anywhere else (including on the page) closes it; submenus open after 200 ms or with Right; Windows 11 rounded corners via DWM.
- No history, downloads panel, profiles, private windows, find-in-page UI or session restore yet. Bookmarks can't be
  dragged to reorder (Move up/down and Move to are in their menu), and their site icons are saved only when a page is
  bookmarked from the star.
- cjui has no UI Automation provider, so the drawn controls (tabs, toolbar buttons) aren't announced by screen readers;
  the address bar and the page are.
- The `--no-engine` switch is a development aid (interface without an engine, for screenshots); the status bar says so.

## 7. Next (Stage 2, 0.2)

Multiple windows and real pop-up windows; tab overflow and drag between windows; find in page; zoom indicator;
keyboard focus order across tab strip, toolbar and page; the "leave page?" prompt on close; then Stage 3's SQLite
storage (bookmarks, history, sessions) with migrations, beside `studiocore`'s proven pattern.

## 8. Downloads, blocking, reference boards (0.4.0 to 0.5.0)

- **Downloads** (`core/downloads.*`, `src/browser_downloads.cpp`): in `DownloadStarting` the engine's own pop-up is turned off (`put_Handled(TRUE)`), and the `ICoreWebView2DownloadOperation` is wrapped in a `DownloadHandle`. Its bytes, end-time and state events are sent through a shared sink, so a handle that has been destroyed is never called. `DownloadStore` (SQLite, `User\downloads.db`) keeps the list, writing a download only when its state changes. Downloads still running when the browser closed are marked "Stopped when the browser closed". When a download finishes, the window lets go of its handle on a timer, not inside the handle's own callback. A tab closed mid-download has its `Page` moved to `parked_` until the download finishes. The Downloads page is `northray://downloads` (Ctrl+J), and a toolbar button with a progress ring appears after a download in the last day. The page is shown even when the engine isn't working.
- **Blocking** (`core/blocker.*`): a list of about 120 advertising, tracking, data-broker and session-recording domains written for NorthRay. It leaves out sign-in, consent, video-player and CDN domains. `siteOf` works out the registrable domain, using common two-part suffixes such as co.uk. `shouldBlock` blocks a request only when its host is listed and is on a different site from the page. Each `Page` adds a `WebResourceRequested` filter per domain, so only possible matches reach the handler, and refuses a match with a 403 response. The page's count goes to the shield in the address bar, whose menu turns blocking off for a site (`settings.blockerAllowed`) or everywhere (`blockTrackers`).
- **Search**: Google is the default. `searchEngineChosen` records whether the person picked an engine in the menu; a choice made that way is kept.
- **Reference boards** (`core/references.*`, `src/browser_references.cpp`): the right-click menu (`ContextMenuRequested`, `CreateContextMenuItem`) gains "Save picture to a reference board" for pictures and "Pick a colour from the page". A picture is downloaded on a worker thread (`URLDownloadToFileW`, or decoded when it's a `data:` URI). It is identified by its magic bytes (PNG, JPEG, GIF, WebP or BMP), named by an FNV hash of its address, and handed back with `WM_APP+21`. `ReferenceStore` (SQLite, `User\references.db`) holds boards, pictures and colours, and a picture file is deleted only when no board still uses it. `northray://references` shows board chips, the board's colours and its pictures in columns, with Colours, Move and Remove on hover. Board names are edited in the bookmarks page's rename field. `mainColours` builds a 4-bit-per-channel histogram and merges shades closer than 48.
- **Colour picker**: `Page::capture` (`CapturePreview`, PNG decoded through WIC) gives a still of the page. While picking, the page view is hidden and the still is drawn in its place, with an 11 × 11 loupe and the colour's hex value. A click copies the hex to the clipboard and adds the colour to the current board; Esc or a right-click cancels.

## 9. Video pop-out (0.7.0)

- `Page::reparent(hwnd)` moves the WebView2 controller into another window (`put_ParentWindow`), so the page isn't loaded twice and playback carries on.
- `browser_popout.cpp`: an unowned `WS_POPUP` window with `WS_EX_TOPMOST | WS_EX_TOOLWINDOW` (unowned, so it stays when the browser is minimised). A GDI bar is 30 px; the caption is dragged through `WM_NCHITTEST`, the edges resize, and there are back-to-tab and close buttons. Its place is saved in settings (`popX/popY/popW/popH`). The tab's place in the window shows "Playing in the pop-out". `placePages` skips the tab, and closing the tab or the window puts it back first.
- `Page::setVideoPopout(on)` runs `kPopoutScript` (and runs it again after each new document while it's on):
  - It picks the main video (playing first, then size) and walks up to its player: the outermost parent within 10% of the video's size that holds nothing beside the video (controls and adverts sit over it).
  - The player becomes `position: fixed` and fills the viewport. The wrappers between it and the video stretch, and the page's scroll bars are hidden.
  - Every 0.7 s it checks that the player still holds the playing video. If nothing is playing (an advert's end screen) it leaves things alone; if another video is playing, it moves to that one's player. This is what keeps the pop-out from breaking when an advert ends.
  - It was tested in Chromium (Playwright) with an advert video paused and removed, the programme's video swapped in, and the whole player replaced. Under Wine there's no WebView2, so the window itself was only checked for building and the "no page" message.
- 0.7.1, just the video. While popped out, `html.__nr_popping` hides every element outside the player (`body *:not(.__nr_pop):not(.__nr_pop *)` gets `visibility: hidden`). This works whatever stacking context the site's fixed header is in, which a z-index alone couldn't beat. The pop-out's frame is now a 3 px black edge. The bar is a separate layered window owned by the pop-out, so it can sit over the page's child window. A 120 ms timer shows it only while the cursor is over the pop-out. Dragging the bar sends `WM_NCLBUTTONDOWN HTCAPTION` to the pop-out.
- 0.7.2, shape and controls. `Page::videoCommand("state"|"back"|"toggle"|"forward"|"skip")` runs `kVideoControlScript` and returns "found|width|height|paused|canSkip".
  - The skip button is found from YouTube's classes (`.ytp-skip-ad-button`, `.ytp-ad-skip-button(-modern)`, and others whose class contains skip-ad or ad-skip; a wrapper gives its inner button). Failing those, it's a visible element whose own short text is "Skip", "Skip ad" or "Skip ads" and that holds no video or blocks. `skip` calls `.click()` on it. A site that ignores clicks made by script (`isTrusted`) needs its own button pressed in the pop-out.
  - The pop-out polls the state about twice a second (every 4th tick of the 120 ms timer).
  - A change of shape refits the window (same width, height to match, at most 90% of the screen, bottom edge kept). `WM_SIZING` holds the shape while resizing.
  - The controls are a second layered window owned by the pop-out, along its bottom: back 10, play or pause, forward 10 (drawn with GDI), and "Skip ad" when it's offered. With the mouse elsewhere and a skip offered, a small window with only "Skip ad" sits in the bottom right corner.
  - The scripts were tested in Chromium with Playwright: seeking, play and pause, YouTube's skip slot and button, and a plain "Skip Ad" link, with no false sightings from the player's own text.

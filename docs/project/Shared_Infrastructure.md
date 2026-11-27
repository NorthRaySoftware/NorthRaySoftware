# NorthRay Shared Infrastructure

Everything more than one NorthRay app depends on. Checked against the code on 26 November 2026.

## 1. Libraries

| Library | Path | Portable? | Used by | What it holds |
|---|---|---|---|---|
| **cjui** | `libs/cjui` | No (Windows) | Launcher, Writing Studio, Browser, Files, Capture | The UI toolkit; see below |
| **cjcore** | `libs/cjcore` | Yes (Linux, Windows, macOS) | Launcher, Browser, Files, Capture (and cjui) | `registry` (the app metadata format), `time` (UK time and formatting), `medialib` (the NorthRay media library), `json_safe.hpp` (`cj::valueOr`, defensive JSON reads), `skillswap` (dormant rules for the abandoned SkillSwap app) |
| **studiocore** | `libs/studiocore` | Yes | Writing Studio only | The library format, the manuscript, the bible, planning, typesetting, publishing, import and sync. See the Writing Studio manual |
| App cores | `apps/browser/core` (`nrb/`), `apps/files/core` (`nrf/`), `apps/capture/core` (`ncap/`) | Yes | One app each | Logic kept out of Windows code so it can be unit-tested on Linux |

### cjui modules (`libs/cjui/include/cjui/`)

| Header | What it provides |
|---|---|
| `window.hpp` | `AppWindow`: the custom frame and caption buttons, per-monitor DPI, the interface zoom (0.172: `setInterfaceZoom`, `zoomStep`, `zoomControl`, the `ownZoomAt`/`ctrlZoomKeysAreApps`/`onInterfaceZoomChanged` hooks; `interfaceZoomOf(hwnd)`, `zoomSteps()`), hit areas, focus and keyboard, buttons (calm style), segmented controls, links, toasts with Undo, animations, `drawCaptionButtons`, `linkButton`, `setLabelStyle` |
| `gfx.hpp` | `Gfx`: Direct2D and DirectWrite drawing, a text layout cache, clip stack and culling (0.88), images (`CachedImage`, rounded), `drawPixels`, arcs, gradients drawn as solid bands |
| `menu.hpp` | `showMenu` and `trackMenu`: themed pop-up menus with keyboard control, submenus, icons, and type-to-search for lists of 13 or more (capped and scrolling) |
| `dialog.hpp` | `messageBox`: themed question and message boxes. Since 0.93, long text scrolls inside the box and the box always fits the screen. `choiceBox` (0.170) has buttons named for what they do, a default and Cancel. `askText` is a one-line text prompt. The interface zoom doesn't apply to them (0.173) |
| `icons.hpp` | About 66 line icons drawn with `Gfx`; `iconFor(label)` picks one from a command's words |
| `northray.hpp` | The NorthRay palette (`northray::palette`), the night sky with twinkling stars kept clear of text, the aurora band, `kWebsite` and `openWebsite(page)` |
| `http.hpp` | WinHTTP requests (any method, headers zeroed after sending) |
| `packages.hpp` | Finding installed apps (`findInstalledApp`), package verification, running state (named mutex), the crash report (`installCrashReport`) and `northRayDataDir()` |
| `format.hpp` | Dates and numbers in the UK style (`formatShortDay` and similar) |

**Interface zoom (0.172; the content only since 0.173).** An app opens its zoom area in `drawFrame` with `beginZoomArea(rect)` after drawing or laying out its bars, and closes it with `endZoomArea()` before drawing them. `Gfx::beginZoom` scales about the area's top-left by the zoom and clips to the area. Content is laid out in the returned rect, `{x, y, w/zoom, h/zoom}`. Hits, sliders and scroll areas registered inside are in that space. The base maps the mouse into it when the pointer is over the last frame's area (`mapPoint`): `mouseX_`, `onMouseDown`, `onWheel`, `onRightClick`, `ownZoomAt` and the hit test all work unchanged, because the bars' space (left of and above the area) and the area's space never overlap. `toWindow(rect)` turns an area rect back into window DIPs, for child windows and menu positions, and passes a bar's rect through unchanged. `toArea(x, y)` maps a system point (an OLE drop, the cursor). `rawMouseX_` keeps the window position for the tooltip. `Gfx::setDpi` is the display's DPI alone again; menus and message boxes are not zoomed. Keys: Ctrl+Alt with + / − / 0 always; Ctrl with them after the app's `onKeyDown` has declined them, unless `ctrlZoomKeysAreApps()` (the Browser). Ctrl+wheel zooms unless `ownZoomAt(x, y)`. The smallest window size still grows with the zoom, capped at the screen. The zoom is stored in `interface.ini` under the window class. Native controls in the area that make their own fonts from the DPI multiply by the zoom; those in a bar don't.

**Whole-window scaling (suite 0.185, off by default).** `setScaleWholeWindow(true)` brings back 0.96's behaviour as an option. `applyDpi()` gives gfx the display DPI × zoom, `contentZoom()` returns 1, so the zoom area adds nothing, and `uiDpiOf(owner)` scales menus, message boxes and askText (window property `NorthRay.ScaleWholeWindow`). It is stored as `[WholeWindow] <class>=1` in `interface.ini`. `zoomControl` draws its tick box; `setWholeWindowLabel` names an app's own bars. Code that draws at "display × zoom" by hand must use `gfx_.dpiScale() * contentZoom()`, not `interfaceZoom()`.

**House rules enforced in cjui.** No scroll bars anywhere (scrolling is by wheel or keys, with fades and markers). No menu bars. No status bars (messages appear as a toast near the bottom).

## 2. Third-party code (`libs/third_party`)

| Package | Licence | Used by |
|---|---|---|
| nlohmann/json 3.11.3 | MIT | All apps |
| SQLite 3.54.1 | Public domain | Writing Studio, Browser |
| miniz | MIT | Writing Studio (EPUB, DOCX), Browser (backup), Files, cjcore |
| HarfBuzz 10.1.0 and FreeType 2.13.3 | MIT; FreeType licence | Writing Studio typesetting |
| stb_image and stb_image_write | Public domain or MIT | Writing Studio |
| QR Code generator (Nayuki) | MIT | Capture |
| WebView2 SDK 1.0.961.33 (`webview2`) and 1.0.4314.35 (`webview2-sdk`) | BSD-3-Clause | Studio editor, Browser |

Full credits are in `CREDITS.md`; dependency notes are in `docs/DEPENDENCIES.md`. The licence rule for the whole suite is **permissive only**: public domain, MIT, BSD, zlib, Apache-2.0, or MPL where offered. No GPL or AGPL code may be linked or bundled.

## 3. Shared metadata (`shared/metadata`)

The single source of truth for the Launcher and the website.

| Path | Contents |
|---|---|
| `apps/<id>.json` | One file per listed app: name, tagline, description, features, category, status, version (a fallback), accent, artwork, credits, launch, website, installer. Adding a file registers an app; no code change is needed |
| `releases/<id>.json` | Release notes, newest first. They are inserted at index 0 for each release, and the Launcher and website read them |
| `brand/tokens.json` | Colours (light, dark, night and aurora), type families, radii |
| `brand/identity.json` | The developer name and line, the contact email, the logo path. "Never add invented names, staff or history" |
| `brand/northray-logo.svg`, `northray-mark.svg/.png` | The NorthRay mark |
| `community/links.json` | `catalogueUrl` and `downloadPage` (GitHub Pages) |
| `icons/`, `screenshots/` | App icons; screenshots with captions, taken from an invented demo library (`studio_seed_demo`) |
| `demo/` | Demonstration content, always flagged `"demo": true` |
| `postponed/` | SkillSwap's metadata (abandoned, not listed) |
| `skillswap/` | SkillSwap categories |

Validate the metadata with `node tools/validate-metadata.mjs` (current result: "Metadata OK: 4 application(s)").

## 4. Files shared between apps on a user's PC

These are one-way. Each file has one writer, and readers never write to it.

| File | Written by | Read by |
|---|---|---|
| `%LOCALAPPDATA%\NorthRay\Writing Studio\summary.json` | Writing Studio (every 30 s when changed) | Launcher (Today, Jump back in), Capture (writing overlays) |
| `%LOCALAPPDATA%\NorthRay\Capture\settings.json`, `scenes\*.json` | Capture | Launcher (Jump back in) |
| `%LOCALAPPDATA%\NorthRay\Library\` | Capture (the NorthRay media library) | Capture now; meant for the suite |
| `%LOCALAPPDATA%\NorthRay\profile.json` | Capture's designer | Templates (shared details) |
| `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\NorthRay.<id>` | Each installer | `cjui::findInstalledApp` (Launcher) |
| `%LOCALAPPDATA%\NorthRay\interface.ini` (`[Zoom]`, one line per window class) | Each app's own line, by cjui (0.172) | That app only |

**Command-line hand-offs**
- `WritingStudio.exe --open-book <id> --chapter <id> --library <folder>`.
- `NorthRayCapture.exe --collection <name> --scene <name>`.

Both are passed on to an already-running copy with `WM_COPYDATA`.

## 5. Build, installers and release tooling

| Tool | Purpose |
|---|---|
| `CMakeLists.txt` (root) | The whole suite. `CJ_VERSION` comes from `/VERSION`. Native builds produce the test executables; Windows builds produce the apps |
| `tools/build-windows.sh` | MinGW-w64 cross-build of every app into `build/windows` (static runtime, no extra DLLs) |
| `tools/make-installers.sh` | NSIS installers (`installer/*.nsi`) into `build/installers`, plus `catalogue.json` with size and SHA-256 per product |
| `tools/validate-metadata.mjs` | Checks the app registry and release notes |
| `tools/publish-site.sh <pages-clone> --push` | Copies `website/dist` into a clone of the `main` branch and pushes it (GitHub Pages). `COMMIT_TRAILER` adds lines to the commit message |
| `tools/editor-harness.cjs` | Runs Writing Studio's editor in Chromium with a stand-in host |
| `tools/build-thesaurus.mjs` | Builds the Studio thesaurus from WordNet |
| `tools/check-*.sh`, `accounts-mock.py`, `webview2-uuids.py` | Capture's format and network checks; WebView2 interface ids for MinGW |
| `cmake/make-package.cmake` | Packages an app (`package.json` with sizes and SHA-256) |
| `installer/` | NSIS scripts. Installs are per user and need no admin rights |

## 6. Website (`website/`)

- Astro 7.4.2, static output.
- It reads `shared/metadata` at build time: a catalogue, a page per app, downloads with SHA-256, About, Contact.
- Live at **https://northraysoftware.github.io/NorthRaySoftware/**, from the `main` branch of `NorthRaySoftware/NorthRaySoftware`.
- `website/src/generated/releases.json` is written by the build. It is committed after each release as "Release list for suite X".

## 7. Repository and branches

- GitHub: `NorthRaySoftware/NorthRaySoftware`.
- **`source`** holds the development source. Always push there (`git push origin HEAD:source`).
- **`main`** holds the published website and downloads. Only `tools/publish-site.sh` updates it.
- The local checkout's branch name in past sessions was `browser-0.1.0`. That name doesn't matter, because pushes go to `source`.
- **Never** commit secrets. `.gitignore` excludes `build/` and `node_modules/`.

## 8. Postponed and removed

- **SkillSwap**, abandoned (26 Nov 2026). Its dormant rules live in `libs/cjcore` (`skillswap.cpp`, still tested) and its metadata in `shared/metadata/postponed`, `skillswap/` and `demo/`. The build option that pointed at the never-committed `apps/skillswap` was removed.
- **The community launcher and the radio app** were removed in suite 0.131 (22 Nov 2026) at the director's request. Their code is only in git history, before commit `638652c`.

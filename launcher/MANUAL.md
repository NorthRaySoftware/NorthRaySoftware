# NorthRay Launcher: Development and Reference Manual

| | |
|---|---|
| Application | NorthRay Launcher (`NorthRayLauncher.exe`) |
| Version at the time of writing | 0.174.0. The launcher's version is the suite version, from `/VERSION` |
| Status | **Preview**, released. The online catalogue is live on GitHub Pages |
| Source | `launcher/` (C++20, Win32 + Direct2D via `libs/cjui`) |
| Release notes | `shared/metadata/releases/launcher.json` |
| Architecture notes | `docs/ARCHITECTURE.md` (the suite document, which also covers the launcher) |
| Manual last checked against code | 26 November 2026 |

Status labels used in this manual:
- **Implemented**: in the code and released.
- **Partial**: some of it is built; the rest is noted.
- **Planned**: agreed with the project director, not built.
- **Proposed**: suggested, not yet agreed.

"Tested in Wine" means it was run as a Windows build under Wine on Linux. It has not necessarily been checked on real Windows.

---

## 1. Overview

**Purpose.** NorthRay Launcher is the suite's app manager. It finds, installs, updates, opens and uninstalls the other NorthRay apps. It also has a small home page that shows what the other apps left for it on this PC.

**Audience.** Anyone using more than one NorthRay app. No account is needed, and no payment.

**Philosophy.** The launcher is not a monolith:
- The apps are separate executables. They never link against it, and they keep running when it closes.
- It knows about apps only through JSON metadata. Adding an app is a metadata change, not a code change.
- It never shows anything as installed, live or downloadable unless that is true.

**Where it sits.** It is the entry point of the suite and its update channel. Each app also has its own installer, so the launcher is optional for any single app.

## 2. Features

### Implemented

- **Interface size** (0.172; the page only since 0.174, the nav and top strip keeping their size): 50–200%, from Settings › Appearance or Ctrl with + / − / 0 and the wheel. Kept on this PC. The same engine is in every app (cjui).
**Library (home page)**
- Big app cards: art, version, Open and Update. They show "updated <date>" from the installed program's file date, and an update badge on the art's corner.
- Cards can be dragged into a new order, saved as `Settings::appOrder`.
- A banner with counts and Update all, then a "What's new" row with the latest release note of each listed app.

**App pages**
- Screenshots with a full-size viewer, taken from `shared/metadata/screenshots/<id>.json`.
- Release notes.
- "On disk": how much space the app's folder takes.

**Install, update and uninstall**
- Reads `catalogue.json` from `catalogueUrl`. It accepts https only, plus `http://localhost` for testing.
- Caches the last catalogue (`catalogue-cache.json`) and shows it at once while the online copy is checked.
- Downloads to `%LOCALAPPDATA%\NorthRay\Launcher\Downloads`.
- Refuses anything larger than the catalogue announced.
- Checks size and SHA-256, then runs the app's own installer silently (`/S /D=<folder>`).
- Retries a failed download up to 3 times, after 3 s and then 10 s. HTTP 403, 404 and 410 are not retried. Speed and time left are shown while downloading.
- Automatic updates are on by default. They never touch a running app; an update waiting for its app to close is retried every 20 minutes.
- Uninstalling runs the app's own uninstaller.
- The launcher updates itself the same way.

**Finding installed apps**
- `cjui::findInstalledApp` looks in this order:
  1. `HKCU\...\Uninstall\NorthRay.<id>`.
  2. Older pre-NorthRay entries.
  3. The default folders.
- It re-checks whenever the window is activated.

**Tray and start-up**
- A tray icon (`Shell_NotifyIconW`). Left click shows the window. Right click opens a NorthRay menu: open the launcher or any app, check for updates, Continue writing, Open Capture, Quit.
- Closing the window hides it to the tray (`keepInTray`).
- `startWithWindows` is off by default. When on, it writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` with the value `"<exe>" --tray`.
- The tooltip names the apps with updates. The icon shows a progress bar while an app installs.

**Today strip**
- Words written today, the daily goal and the streak, from Writing Studio's `%LOCALAPPDATA%\NorthRay\Writing Studio\summary.json`.
- The four newest pictures in `Pictures\Screenshots` and `Pictures\NorthRay Capture`.
- Can be switched off (`showToday`).

**Jump back in**
- Up to four recent books from Writing Studio's `summary.json`. Clicking one runs `WritingStudio.exe --open-book <id> --chapter <id> --library <folder>`.
- A NorthRay Capture card, read from Capture's `settings.json` and `scenes\<collection>.json`. It runs `NorthRayCapture.exe --collection <name> --scene <name>`.
- Can be switched off (`showJumpBack`).

**Other**
- First-run welcome tour in three steps: what it is and the checksum check; the install folder; updates, tray and start-up.
- Settings: theme (Light, Dark, follow Windows), install folder, automatic updates, tray, start with Windows, Today, Jump back in. Settings › About shows the launcher's last four releases and a Visit the website link.
- A Website item at the foot of the side menu opens `https://northraysoftware.github.io/NorthRaySoftware/`.
- Custom window frame: the app draws its own minimise, maximise and close buttons, and snap layouts work on maximise.
- Crash report: `cjui::installCrashReport` appends to `%LOCALAPPDATA%\NorthRay\Launcher\crash.log`.

### Partial or not verified on real Windows

- **Interface size (0.172):** built and cross-compiled, not yet run on Windows (Wine would not start in the build environment).
These were tested in Wine but still need checking on Windows:
- Windows 11 snap layouts.
- Real toast notifications.
- High-DPI caption buttons.
- The tray icon's progress bar and tooltip.
- The tray submenus.

The installers are **not code-signed** (the route is undecided), so SmartScreen may warn.

### Planned (from the 18 Nov 2026 answers, not built)

None of these appear in the release notes:
- Search across the suite (books, files, scenes).
- Back up every NorthRay app's settings in one file.
- Roll back to an app's previous version.
- One feed of every app's release notes. The Library's "What's new" shows only the latest note per app.

### Proposed (in `docs/ARCHITECTURE.md`, not agreed)

- A signed release manifest.
- Authenticode-signed installers (Azure Trusted Signing).
- CI builds on a Windows runner.
- A "shared view protocol" for showing app panels inside the launcher (investigated, not built).

### Known limitations

- No screen-reader support. cjui has no UI Automation provider; this is the largest accessibility gap in the suite.
- Windows only.

## 3. Interface and user experience

- **Layout:** a night-sky side menu (stars and aurora) with the pages; content pages on cool neutrals. Pages: Library/home, Updates, an app's page, Settings.
- **Principles:**
  - Information first.
  - Plain words.
  - Never pretend.
  - No menu bars, no scroll bars and no status bar (messages appear as a short note near the bottom).
  - Calm buttons: one style, an icon with words, a soft frame that lifts on hover.
- **Workflows:** install an app, update all, open an app, continue writing a book, open Capture on a scene.
- **Accessibility:**
  - Keyboard focus rings and keyboard paths throughout.
  - Contrast raised to WCAG AA for faint text (0.140).
  - Animations respect Windows' "animation effects" setting for the twinkling stars.
  - No UI Automation yet.

## 4. Technical documentation

| Item | Detail |
|---|---|
| Language | C++20 |
| UI | Win32 + Direct2D/DirectWrite through `libs/cjui` (`AppWindow`, `Gfx`, menus, dialogs, `northray.hpp` palette and sky) |
| Build | CMake ≥ 3.21. MinGW-w64 cross-build from Linux (`tools/build-windows.sh`) or Visual Studio 2022 |
| Dependencies | nlohmann/json 3.11.3 (vendored); WinHTTP and BCrypt (SHA-256); Shell, DWM. See `docs/DEPENDENCIES.md` |
| Installer | NSIS (`installer/launcher.nsi`), built by `tools/make-installers.sh` |

**Key files**

| File | Contents |
|---|---|
| `launcher/src/main.cpp` | Start-up, `--tray` and `--app <id>`, single instance |
| `launcher/src/model.hpp/.cpp` | `Settings`, `Today` and `readToday()`, the catalogue (`allowedUrl`, release parsing), install detection, `Model::launch(args)` |
| `launcher/src/jobs.hpp/.cpp` | Download, retry, size and SHA-256 checks, silent install and uninstall, self-update |
| `launcher/src/launcher.hpp/.cpp` | The window and every page, the tray, the tour, cards and drag ordering |

**Data storage** (`%LOCALAPPDATA%\NorthRay\Launcher\`)

| File | Contents |
|---|---|
| `settings.json` | Settings, written atomically and read defensively with `cj::valueOr` |
| `catalogue-cache.json` | The last catalogue fetched |
| `Downloads\` | Downloaded installers |
| `tray-explained` | Marker that the one-time tray explanation was shown |

**External integrations**
- The catalogue on GitHub Pages: `shared/metadata/community/links.json` holds `catalogueUrl` and `downloadPage`.
- Files written by other NorthRay apps, all read-only: Writing Studio's `summary.json`, and Capture's `settings.json` and `scenes\`.

**Metadata**
- `shared/metadata/apps/*.json` lists the apps (one file per app).
- `shared/metadata/releases/launcher.json` holds the launcher's notes.
- `shared/metadata/brand/` holds the tokens and identity.
- The metadata is copied next to the launcher at build time.

## 5. Development

**Status:** preview. Every app release bumps the suite version, which is the launcher's version, so the launcher can compare installers.

**Milestones done**
- 0.4: becomes the app manager.
- 0.17: the NorthRay look.
- 0.19: tray and custom frame.
- 0.59: NorthRay apps only, and Today.
- 0.98: download polish.
- 0.129: Jump back in.
- 0.131: the community and radio apps removed.
- 0.140: calm controls.
- 0.141: website link.

**Outstanding:** the planned items above, code signing, UI Automation, and verifying the tray and notifications on Windows.

**Technical debt**
- The brand colours are copied by hand into `launcher.cpp` (`northRayPalette`, `kNight*`, `kAurora*`) and `website/src/styles/northray.css`. They must be changed together with `shared/metadata/brand/tokens.json`.
- The fallback `version` fields in `shared/metadata/apps/*.json` can go stale (see the suite issues list).

## 6. Maintenance

**Run**
- Install with `build/installers/NorthRayLauncher-Setup*.exe`.
- In development, run `build/windows/launcher/NorthRayLauncher.exe`. The packaged apps sit next to it in `packages/`.

**Build**

```
tools/build-windows.sh            # MinGW cross-build of the whole suite (Linux)
tools/make-installers.sh          # NSIS installers + catalogue.json
# or on Windows:
cmake -S . -B build/vs -G "Visual Studio 17 2022" -A x64 && cmake --build build/vs --config Release
```

**Test**
- `node tools/validate-metadata.mjs` checks the app registry and release notes.
- The launcher's logic has no unit tests of its own. `libs/cjcore` (registry, time) is tested by `cjcore_tests`: 68 checks, all passing on 26 Nov 2026.
- UI behaviour has been checked by hand in Wine with Xvfb. Downloads were tested against a local HTTP server; there is no script for this in `tools/`.

**Troubleshooting**

| Problem | Cause and fix |
|---|---|
| "Downloads aren't available yet" | `catalogueUrl` is empty or unreachable. Check `links.json` and the network |
| An app isn't found | Check `HKCU\...\Uninstall\NorthRay.<id>` and the install folder |
| The launcher closes on start | Read `crash.log`. Settings are read defensively, but a corrupted file can be deleted to reset |
| An update never installs | The app is running. Updates wait for it to close and retry every 20 minutes |

**Implementation notes**
- `WM_CLOSE` with wParam 1 really quits. The installer and self-update use it.
- The catalogue's URLs must pass `allowedUrl`.
- Never run an executable that resolves outside the app's install folder.

# NorthRay architecture

Status: NorthRay Launcher, NorthRay Writing Studio, NorthRay Browser, NorthRay Files and NorthRay Capture. Last updated 22 November 2026.

**Identity (13 Nov 2026, final).** The software is developed by the **NorthRay Software project**, a separate development identity (`shared/metadata/brand/identity.json`). Never invent developers, partnerships or company history. Legal notices must state the real ownership accurately when they're written. At launch: no subscriptions, paid tiers, adverts, mandatory accounts or upgrade prompts, and nothing claims the apps are permanently free or nonprofit. **SkillSwap is postponed**: its core stays in `libs/cjcore`, but it isn't listed or installed; the safety and payment notes about it below are kept for if it returns. **The community and radio apps were removed in 0.131.0 (22 Nov 2026)**: their code, installers, catalogue entries, release notes and website section are gone; copies already installed elsewhere aren't uninstalled and get no more updates. The Studio has its own architecture document: `docs/studio/ARCHITECTURE.md`.

This document records the decisions made so far, why they were made, and what is deliberately left for later. Anything marked *recommended* is a proposal that still needs agreement before it's built.

## NorthRay structure (0.4.0, 13 Nov 2026)

| Product | Folder | Executable | Installs to | Settings |
|---|---|---|---|---|
| NorthRay Launcher | `launcher/` | NorthRayLauncher.exe | `%LOCALAPPDATA%\Programs\NorthRay Launcher` | `%LOCALAPPDATA%\NorthRay\Launcher` |
| NorthRay Writing Studio | `apps/studio`, `libs/studiocore` | WritingStudio.exe | `%LOCALAPPDATA%\NorthRay\Apps\writing-studio` (or the launcher's chosen folder) | `%LOCALAPPDATA%\NorthRay\Writing Studio`; libraries are user folders |

- **Finding installs:** each installer writes `HKCU\...\Uninstall\NorthRay.<id>` (folder and version). `cjui::findInstalledApp` reads it, then the older entries from before the NorthRay name, then the default folders. Every app uses the same function.
- **Catalogue and updates:** `tools/make-installers.sh` writes `catalogue.json` (version, file, size and SHA-256 for each product). The launcher fetches it from `catalogueUrl` in `shared/metadata/community/links.json` (https only; `http://localhost` for testing), and caches the last copy for offline use. Installs and updates run in `launcher/src/jobs.cpp`: download to `%LOCALAPPDATA%\NorthRay\Launcher\Downloads`, refuse anything larger than announced, check size and SHA-256, then run the app's own installer with `/S /D=<folder>`. Automatic updates are on by default (since 0.19.0) and never touch a running app; an update that waits for its app to close is retried every 20 minutes.
- **Migration:** the installers silently remove a copy installed by the pre-NorthRay setup, and the old launcher. Apps copy their old settings across once and leave the originals.
- **Release notes:** `shared/metadata/releases/<id>.json`, read by the launcher's app pages and the website.
- **Website:** NorthRay Software branding (`website/`): catalogue, a page for each app, a prominent launcher download, direct installers, About, Contact (address from `brand/identity.json`).
- `catalogueUrl` and `downloadPage` stay empty until the website is hosted. Until then the launcher says downloads aren't available yet, and still finds and opens installed apps.

**Update 21 Nov 2026 (0.129.0): Jump back in.** CJ asked for quick starts from the Launcher's home. Under Today, **Jump back in** shows up to four books from Writing Studio's summary.json `recent` list (three when Capture is shown too): the cover (or the first letter), title, series, the chapter last written in and the book's words. Clicking one runs `WritingStudio.exe --open-book <id> --chapter <id> --library <folder>` (`Model::launch` now takes arguments, each quoted). NorthRay Capture's card reads Capture's `settings.json` and `scenes\<collection>.json` (the scenes in order and the current one) and runs `NorthRayCapture.exe --collection <name> --scene <name>`; Other scenes... lists them all. Both apps pass the request on to a copy that's already running (WM_COPYDATA), and Capture never changes scene while recording or streaming. The tray menu (re-reading the files as it opens) gains Continue writing and Open Capture on submenus. Settings › Jump back in turns it all off (`showJumpBack`). Only installed apps are offered; nothing is read but those apps' own files. Tested in Wine with both apps closed and open; the tray submenus need checking on Windows.

**Update 18 Nov 2026 (0.98.0): NorthRay Launcher polish.** Downloads show speed and time left (`JobState::rate`, bytes over each half second smoothed 70/30, and `attempt`). A download that drops, comes back short, gets a server error or fails its SHA-256 is tried again by itself (`Jobs::kAttempts` = 3, after 3 s then 10 s, cancellable; 403, 404 and 410 aren't retried), with "The download stopped; trying again (2 of 3)..." while it waits. App cards have 112 DIP art, an update badge on the art's corner (with a tooltip), "updated 18 Nov" from the installed program's file date, and can be dragged into another order (`Settings::appOrder` in settings.json; a line shows where it will land; filtered-out apps keep their places). The tray icon's tooltip names the apps with updates ready, or the app installing and its percentage, and its icon gets a progress bar along its foot while one installs (drawn into a 32-bit DIB, `CreateIconIndirect`, redrawn every 4%). Settings › About lists the launcher's last four releases with their notes. Already there: the cached catalogue shown at once while the online one is checked. Tested in Wine against a local server that dropped the first download a third of the way and then served about 260 KB/s: the note, the retry (two requests seen), speed and time left counting down, the checksum passing; plus reordering, badges, dates and Settings. The tray icon's progress and tooltip need checking on Windows.

**Update 15 Nov 2026 (0.59.0): NorthRay Launcher's Today strip.** NorthRay Launcher's home page gained **Today**: words written today, the goal and the streak from Writing Studio's new `%LOCALAPPDATA%\NorthRay\Writing Studio\summary.json` (written every 30 seconds when it changes; a streak is now days in a row with any words written), and the four newest pictures in Pictures\Screenshots and Pictures\NorthRay Capture (click to open). It reads only local files and can be switched off in Settings. An app's page shows how much space its folder takes ("On disk"), and library cards grow to fit long taglines. Tested in Wine: Writing Studio writing summary.json; the Today strip with real files; On disk for Writing Studio (14.8 MB).

**Update 13 Nov 2026 (0.16.2): settings are read defensively.** `cj::valueOr` (`libs/cjcore/include/cj/json_safe.hpp`) returns the default when a key has the wrong type, where nlohmann's `value()` throws; every settings and cache reader in the apps uses it. An app closed on start where an older version had saved `"theme": "system"` (themes are now written as 0/1/2 and both are read). `cjui::installCrashReport` (terminate handler and unhandled-exception filter) shows what went wrong and appends to `%LOCALAPPDATA%\NorthRay\<app>\crash.log`.

**Update 14 Nov 2026 (0.19.0): the apps draw their own window buttons; the Launcher runs in the tray.**
- *Custom frame for every cjui app.* `WindowSpec::customFrame` (with `captionHeight`) removes the Windows title bar (`WM_NCCALCSIZE` keeps the side and bottom sizing borders; a maximised window keeps its border off the top). `AppWindow::drawCaptionButtons(strip, onNight)` draws minimise, maximise/restore and close; maximise answers `WM_NCHITTEST` with `HTMAXBUTTON`, so Windows 11 shows snap layouts there. Empty space in the top `captionHeight` DIPs is `HTCAPTION` (drag, double-click to maximise, Aero Snap). Where they sit: the Launcher's top strip (40), Writing Studio's row of tabs (minimum width now 1160), NorthRay Files' toolbar. NorthRay Browser keeps its own equivalent in its tab strip.
- *Launcher in the notification area.* `Settings::keepInTray` (on) and `startWithWindows` (off; `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value "NorthRay Launcher" = `"<exe>" --tray`). Closing the window hides it (`WM_CLOSE` with wParam 1 really quits: the launcher's own update and its setup use that). Tray icon (`Shell_NotifyIconW`, re-added on `TaskbarCreated`): left click shows the window; right click opens a NorthRay menu (`cjui::showMenu`) to open the launcher or any installed app, check for updates, or quit. Balloons report updates installed (or failed) while hidden, and explain the tray once (`tray-explained` marker). `settingsVersion` 2 turns automatic updates on for existing users.
- *Welcome tour* in three steps (what it is and the checksum check; the install folder; updates, tray and start-up). *Library:* bigger cards (`Launcher::appCard`).
- *Setup:* `installer/launcher.nsi` asks a running launcher to quit (`WM_CLOSE`, wParam 1) before installing or uninstalling, and the uninstaller removes the start-up entry.
- Tested under Wine: the tour, settings and the Run entry; hide to tray, the balloon, the tray menu, Quit; `--tray` start with no window; a second launch showing the hidden window; the setup quitting a hidden launcher; the uninstaller removing the start-up entry; each app's buttons, hover, maximise/restore and dragging. Not testable here: Windows 11 snap layouts, real notification toasts, and high-DPI caption buttons.

The sections below record earlier decisions; where they mention the launcher, read NorthRay Launcher.

## The shape of the system

```
                      shared/metadata  (apps, brand, release notes, demo data)
                       |            |
             read at start-up    read at build time
                       |            |
   +-------------------+--+     +---+------------------+
   |  NorthRay Launcher     |     |  Website (Astro)     |
   |  Win32 + Direct2D      |     |  static HTML/CSS     |
   +-----------+------------+     +----------------------+
               | installs verified packages, starts apps (CreateProcess), never links
   +-----------+------------+------------------+
   |  WritingStudio.exe     |  Capture, Browser, Files ...
   +------------------------+------------------+
   shared: libs/cjcore (portable rules) + libs/cjui (Windows UI, HTTP, packages)
               \                 |
                `--- later, if ever: online services (accounts,
                     notifications) over HTTPS ---'
```

- **The launcher isn't a monolith.** It knows about apps only through their metadata files. Apps are separate executables in their own folders, started with `CreateProcess`. They don't link against the launcher and keep running when it closes.
- **`libs/cjcore` is the shared, portable heart.** It holds the app registry format, UK time handling and the media library. It has no Windows code, so it compiles on Linux and macOS today and is unit-tested on both Linux and Windows. the Studio has its own portable core, `libs/studiocore`.
- **One source of truth for content.** `shared/metadata` is copied next to the launcher at build time and read by the website at build time. Adding an app means adding one JSON file. Neither the launcher nor the website needs a code change, and they can't drift apart.

## Decisions by area

| Area | Milestone 1 choice | Notes / later |
|---|---|---|
| Desktop UI | Win32 window, custom-drawn with Direct2D + DirectWrite (C++20) | See "UI framework" below. |
| Build | CMake 3.21+, single `VERSION` file feeds the .exe version resource, the UI and the website | Builds with Visual Studio 2022 on Windows or MinGW-w64 cross-compiling from Linux. |
| App installation and launching | Registry of JSON files; apps live in `%LOCALAPPDATA%\NorthRay\Apps\<id>\`; launcher only starts an executable that resolves inside that folder | Installing/updating from the Library comes with the release pipeline (Milestone 8). See "Updates and downloads". |
| Accounts and authentication | None yet. A local demonstration profile, always labelled as such; a profile file can't claim to be an online account | *Recommended:* a managed identity provider using OpenID Connect with PKCE and the system browser, e.g. Auth0, Clerk or Supabase Auth. No home-made password handling. The launcher stores refresh tokens with Windows DPAPI / Credential Manager. Decide in Milestone 6. |
| Backend services | None yet | *Recommended:* one small HTTP API (TypeScript on Node, or Go) plus Postgres, started only when Milestone 6 needs it. Hosted on a modest managed platform (e.g. Fly.io, Render or Railway) rather than self-managed servers. |
| Real-time messages and notifications | None yet | WebSockets from the same API for chat, presence and in-app notifications; Windows toast notifications from each app. |
| Database | Flat JSON files (metadata only) | Postgres once accounts exist. Local launcher state is a small JSON settings file, written atomically. |
| Website | Astro 7 (static output), reads `shared/metadata` at build | Cheap to host on any static host (Cloudflare Pages, Netlify, GitHub Pages). |
| Other operating systems | Core logic is portable; launcher UI is Windows-only | See "UI framework". |

## Installing and updating apps

**Update 13 Nov 2026 (0.3.0): separate installers.** Each app has its own NSIS setup program (`installer/`, built by `tools/make-installers.sh`): `NorthRayLauncher-Setup`, `WritingStudio-Setup` and one for each later app. They're per-user (no admin prompt), 64-bit, with a Start menu entry and an entry in Windows' Installed apps. App installers put the verified package (including `package.json`) into the apps folder, which is where the launcher looks, so it finds them without any extra step; it re-checks whenever its window is activated. The launcher installs to its own folder and no longer carries the apps. For an app that isn't installed it shows **Download**, which opens `downloadPage` from `shared/metadata/community/links.json`; that's empty until the website is hosted. Remove in the launcher runs the app's own uninstaller. The installers aren't code-signed yet, so SmartScreen may warn; the download page says so and lists SHA-256 checksums. The bundled-package flow below still works for developer builds.


Apps ship as **packages** next to the launcher (`packages/<id>/package.json` lists every file with size and SHA-256). Install verifies every file, copies to a staging folder, then swaps it into the app's folder, so a failed install never leaves a half-updated app. Update works the same way, and the launcher refuses to update an app that's running. Each app holds a named mutex while running, so the launcher can show "Running" and a second launch brings the existing window forward. Nothing is downloaded yet. Online updates will add a signed release manifest (Milestone 8); the free route to a signed installer is the Microsoft Store, which signs MSIX packages at no cost.

## NorthRay look and Launcher features (0.17.0, 13 Nov 2026)

- **Look.** A northern night: the Launcher's sidebar and banners, and the website's banners, are night sky with stars and aurora light; pages are cool neutrals. The colours are recorded in `shared/metadata/brand/tokens.json` and followed by hand in `launcher/src/launcher.cpp` (`northRayPalette`, `kNight*`, `kAurora*`) and `website/src/styles/northray.css` (the `.night` class); change them together. (The old `tools/sync-brand.mjs`, which wrote an unused CSS file, is gone.)
- **Drawing.** `Gfx::fillGradient` and `Gfx::glow` (in `libs/cjui`) draw with bands and rings of solid colour rather than gradient brushes, so they look the same on every Direct2D; rounded banner corners are painted over with the page colour for the same reason.
- **Screenshots.** `shared/metadata/screenshots/<id>.json` lists files (`<id>/*.png`) and captions. The Launcher shows them on each app's page with a full-size viewer; the website shows the same files on the app pages. They're taken from an invented demo library made by `studio_seed_demo <folder>` (`tests/seed_demo.cpp`), never from anyone's real books.
- **First run.** With no Launcher settings file, a welcome page asks where apps are installed and whether updates install automatically, then saves the settings.
- **Website.** The website advertises NorthRay's apps: the catalogue, a page for each, and the downloads.
- **Library.** A banner with counts and "Update all", then "What's new" (the latest release note of each listed app), then the app rows.

## UI framework

The brief asked for C++, a native-feeling, customisable interface, and no website-in-a-window. The realistic options:

- **Qt 6 (Widgets or Quick).** My first choice on paper. It handles accessibility (screen readers via UI Automation), high-DPI scaling, text input and focus out of the box, and makes macOS and Linux versions realistic. LGPL licensing is fine with dynamic linking. *I couldn't use it for Milestone 1:* this build environment blocks Qt's download servers.
- **Win32 + Direct2D/DirectWrite (chosen for Milestone 1).** Microsoft's native drawing stack. It gives a small (3.8 MB, no DLLs), fast-starting app with full control over the look and correct per-monitor DPI handling. The cost is that we write controls ourselves, including **screen-reader support (UI Automation), which isn't done yet**, and the UI layer is Windows-only.
- **WinUI 3.** Native, accessible, Fluent-styled. But it's Windows-only, C++/WinRT tooling is heavy, and its strong house style works against a distinctive identity.
- **Electron/Tauri/WebView2.** Ruled out by the brief's "not a website in a window".

**Decision (12 Nov 2026): stay native Win32/Direct2D** ("proper Windows app"). All three apps share `libs/cjui` (drawing, window base, controls, focus and keyboard handling). Screen-reader support (UI Automation) is still to do, and it's the largest accessibility gap. Switching only replaces `launcher/src/gfx.*`, `launcher.*` and `pages.cpp`; `cjcore`, the metadata and the website are unaffected. I lean towards Qt if macOS or Linux versions are likely within a couple of years, and towards staying on Win32 if Windows is the long-term target.

## Embedding apps inside the launcher

Investigated, not built. The brief wants apps to stay technically independent. Two workable approaches:

1. **Window hosting.** The launcher starts the app with a flag; the app creates a child-capable window and the launcher reparents it into its content area. Fragile across DPI changes and focus handling, and it breaks if the app crashes. Not recommended.
2. **Shared view protocol (recommended if we do this).** An app that opts in (`launch.canOpenInsideLauncher` in its metadata) exposes a small read-only "panel" (e.g. SkillSwap's upcoming bookings) that the launcher renders from data the app or the backend provides. The full app still opens in its own window.

## Updates and downloads (recommended for Milestone 8)

- A signed **release manifest** (JSON: app id, version, download URL, size, SHA-256, minimum launcher version) published alongside each release. The launcher and website both read it, so download information can't disagree.
- Installers are **Authenticode-signed**. Azure Trusted Signing is the most affordable route for an independent project. The launcher verifies the manifest signature, the file hash and the Authenticode signature before running anything, and always asks first.
- Installer: **WiX Toolset** (MSI) or **Inno Setup**, per-user install to `%LOCALAPPDATA%` so no admin prompt is needed. App updates replace the app folder and leave user data alone.
- Releases are built by CI (GitHub Actions on a Windows runner), never from someone's laptop.

## Safety, privacy and permissions (to design before Milestone 6)

- Roles are separate from profiles: creators have verified **creator** profiles; administration and technical roles belong to the development team. Moderator permissions are scoped (chat moderation ≠ account suspension ≠ broadcast control).
- Younger members: no unsolicited contact from adults they aren't connected with, no private one-to-one lessons by default, and lesson conversations stay inside SkillSwap.
- Recordings involving guests or callers need recorded consent before publication.
- Paid SkillSwap services go through a payment provider (e.g. Stripe Connect), never card details on our servers, and only after refund, dispute and fee policies are written.

## Known gaps in Milestone 1

- No screen-reader support yet (see UI framework).
- No text input controls (not needed until search, sign-in and messaging).
- No standard menu bars in any app: commands live in the window itself (the Writing Studio lists its shortcuts on its Settings page).
- Artwork is drawn by code. Real icons and banners can be dropped in via the metadata `artwork` fields once they exist, but loading image files isn't implemented yet.

## Stars clear of text (0.27.3)

`cjui::Gfx` records the ink rectangle of every `text()` call in a frame (`lastTextRects()`, the previous frame's), and `northray::nightSky` skips stars within 5 DIPs of any of them, so words on the night sky (title bars, sidebars, side tabs) are never crossed by a star. Layouts are stable from frame to frame (the twinkle redraws continuously), so the previous frame's text is where this frame's goes.

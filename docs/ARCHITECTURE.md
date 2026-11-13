# CJ Community architecture

Status: 0.7.0 preview: NorthRay Launcher, NorthRay Writing Studio, CJ Radio and CJ Philip Community. Last updated 13 November 2026.

**Identity (13 Nov 2026, final).** The software is developed by the **NorthRay Software project**, a separate development identity (`shared/metadata/brand/identity.json`). CJ Philip Community has a creative association with author and broadcaster CJ Philip; CJ Radio features The CJ Philip Show. Never present the apps as personally made by CJ Philip, and never invent developers, partnerships or company history. Legal notices must state the real ownership accurately when they're written. At launch: no subscriptions, paid tiers, adverts, mandatory accounts or upgrade prompts, and nothing claims the apps are permanently free or nonprofit. **SkillSwap is postponed**: its code stays (`-DCJ_BUILD_SKILLSWAP=ON`), but it isn't listed or installed; the safety and payment notes about it below are kept for if it returns. The Studio has its own architecture document: `docs/studio/ARCHITECTURE.md`.

This document records the decisions made so far, why they were made, and what is deliberately left for later. Anything marked *recommended* is a proposal that still needs agreement before it's built.

## NorthRay structure (0.4.0, 13 Nov 2026)

| Product | Folder | Executable | Installs to | Settings |
|---|---|---|---|---|
| NorthRay Launcher | `launcher/` | NorthRayLauncher.exe | `%LOCALAPPDATA%\Programs\NorthRay Launcher` | `%LOCALAPPDATA%\NorthRay\Launcher` |
| NorthRay Writing Studio | `apps/studio`, `libs/studiocore` | WritingStudio.exe | `%LOCALAPPDATA%\NorthRay\Apps\writing-studio` (or the launcher's chosen folder) | `%LOCALAPPDATA%\NorthRay\Writing Studio`; libraries are user folders |
| CJ Radio | `apps/cj-radio` | CJRadio.exe | `...\NorthRay\Apps\cj-radio` | `%LOCALAPPDATA%\NorthRay\CJ Radio` |
| CJ Philip Community (optional) | `apps/community` | CJPhilipCommunity.exe | `...\NorthRay\Apps\cj-philip-community` | `%LOCALAPPDATA%\NorthRay\CJ Philip Community` |

- **Finding installs:** each installer writes `HKCU\...\Uninstall\NorthRay.<id>` (folder and version). `cjui::findInstalledApp` reads it, then the old `CJCommunity.<id>` entries, then the default folders. Every app uses the same function.
- **Catalogue and updates:** `tools/make-installers.sh` writes `catalogue.json` (version, file, size and SHA-256 for each product). The launcher fetches it from `catalogueUrl` in `shared/metadata/community/links.json` (https only; `http://localhost` for testing), and caches the last copy for offline use. Installs and updates run in `launcher/src/jobs.cpp`: download to `%LOCALAPPDATA%\NorthRay\Launcher\Downloads`, refuse anything larger than announced, check size and SHA-256, then run the app's own installer with `/S /D=<folder>`. Automatic updates are off by default and never touch a running app.
- **Migration:** the new installers silently remove a copy installed by the old CJ Community setup, and the old launcher. Apps copy their old settings across once and leave the originals.
- **Release notes:** `shared/metadata/releases/<id>.json`, read by the launcher's app pages and the website.
- **Website:** NorthRay Software branding (`website/`): catalogue, a page for each app, a prominent launcher download, direct installers, About, Contact (address from `brand/identity.json`).
- `catalogueUrl` and `downloadPage` stay empty until the website is hosted. Until then the launcher says downloads aren't available yet, and still finds and opens installed apps.

**Update 13 Nov 2026 (0.16.2): settings are read defensively.** `cj::valueOr` (`libs/cjcore/include/cj/json_safe.hpp`) returns the default when a key has the wrong type, where nlohmann's `value()` throws; every settings and cache reader in the apps uses it. CJ Philip Community closed on start where an older version had saved `"theme": "system"` (it now writes 0/1/2 and reads both). `cjui::installCrashReport` (terminate handler and unhandled-exception filter) shows what went wrong and appends to `%LOCALAPPDATA%\NorthRay\<app>\crash.log`.

**Update 13 Nov 2026 (0.16.1): CJ Philip Community is the home of the CJ apps.** NorthRay Launcher no longer lists apps whose `credits.associatedCreator` is "CJ Philip" (`Model::hideCjPhilipApps`; it still lists and installs CJ Philip Community). CJ Philip Community builds in the launcher's `model.cpp` and `jobs.cpp`, so it reads the same catalogue (and shares `catalogue-cache.json`), installs and updates CJ apps the same way (download, size and SHA-256 check, silent setup into `installDirFor`), and uninstalls them; an installed app's card always opens it (Update is a separate link). CJ Radio's installer no longer makes its own Start menu shortcut (`-DNO_SHORTCUT`). Fixed: CJ Radio read CJ Philip Community's `settings.json` theme as text, but Community writes 0/1/2, so CJ Radio closed on start once Community had saved its settings; it now accepts both.

**Update 13 Nov 2026 (0.7.0): CJ Philip Community is a launcher for CJ apps.** `apps/community/src/hub.cpp` lists every app in `data/apps/*.json` whose `credits.associatedCreator` is "CJ Philip" (except itself), so new CJ apps appear without code changes. Each card shows the icon, name, tagline and a status: Installed (with version), Running (`cjui::isAppRunning`), Update available (compared with the version in NorthRay Launcher's `catalogue-cache.json`) or Not installed. One button: Open, Show, Update or Get it; the last two run `NorthRayLauncher.exe --app <id>` (or open the download page if the launcher isn't installed). State refreshes every 4 seconds and when the window is activated. Footer: theme (as Windows, light, dark; saved in `%LOCALAPPDATA%\NorthRay\CJ Philip Community\settings.json`), a link to NorthRay Launcher, and the version. The old pages (announcements, events, friends, messages, profile) and their demo data were removed. Across all apps there are no scroll bars at all, not even the faint indicator that used to show while scrolling.

The sections below record earlier decisions; where they mention the CJ Community launcher, read NorthRay Launcher (app management) or CJ Philip Community (community pages).

## The shape of the system

```
                      shared/metadata  (apps, brand, schedule, demo data)
                       |            |
             read at start-up    read at build time
                       |            |
   +-------------------+--+     +---+------------------+
   |  CJ Community launcher |     |  Website (Astro)     |
   |  Win32 + Direct2D      |     |  static HTML/CSS     |
   +-----------+------------+     +----------------------+
               | installs verified packages, starts apps (CreateProcess), never links
   +-----------+------------+------------------+
   |  CJ Radio (CJRadio.exe)|  Studio (WritingStudio.exe)  ...
   +------------------------+------------------+
   shared: libs/cjcore (portable rules) + libs/cjui (Windows UI, HTTP, packages)
               \                 |
                `--- later: CJ Community services (accounts, notifications,
                     schedule, broadcast status) over HTTPS ---'
```

- **The launcher isn't a monolith.** It knows about apps only through their metadata files. Apps are separate executables in their own folders, started with `CreateProcess`. They don't link against the launcher and keep running when it closes. CJ Radio's background playback therefore doesn't depend on the launcher at all.
- **`libs/cjcore` is the shared, portable heart.** It holds the app registry format, the radio schedule model, the broadcast-status rules and UK time handling. It has no Windows code, so it compiles on Linux and macOS today and is unit-tested on both Linux and Windows. CJ Radio and the future backend use it; the Studio has its own portable core, `libs/studiocore`.
- **One source of truth for content.** `shared/metadata` is copied next to the launcher at build time and read by the website at build time. Adding an app means adding one JSON file. Neither the launcher nor the website needs a code change, and they can't drift apart.

## Decisions by area

| Area | Milestone 1 choice | Notes / later |
|---|---|---|
| Desktop UI | Win32 window, custom-drawn with Direct2D + DirectWrite (C++20) | See "UI framework" below. |
| Build | CMake 3.21+, single `VERSION` file feeds the .exe version resource, the UI and the website | Builds with Visual Studio 2022 on Windows or MinGW-w64 cross-compiling from Linux. |
| App installation and launching | Registry of JSON files; apps live in `%LOCALAPPDATA%\CJ Community\Apps\<id>\`; launcher only starts an executable that resolves inside that folder | Installing/updating from the Library comes with the release pipeline (Milestone 8). See "Updates and downloads". |
| Accounts and authentication | None yet. A local demonstration profile, always labelled as such; a profile file can't claim to be an online account | *Recommended:* a managed identity provider using OpenID Connect with PKCE and the system browser, e.g. Auth0, Clerk or Supabase Auth. No home-made password handling. The launcher stores refresh tokens with Windows DPAPI / Credential Manager. Decide in Milestone 6. |
| Backend services | None yet | *Recommended:* one small HTTP API (TypeScript on Node, or Go) plus Postgres, started only when Milestone 6 needs it. Hosted on a modest managed platform (e.g. Fly.io, Render or Railway) rather than self-managed servers. |
| Real-time messages and notifications | None yet | WebSockets from the same API for chat, presence and in-app notifications; Windows toast notifications from each app. |
| Live audio | None yet; status is always "off air" because nothing can report a live signal | *Recommended:* host console sends Opus audio to an **Icecast** server for listening (works in browsers without installing anything). Call-in guests join a **WebRTC** room (e.g. LiveKit, self-hosted or cloud) that's mixed by the host console. Icecast's own status endpoint becomes the `LiveSignal`. Recording happens server-side. Prototype in Milestone 7. |
| Database | Flat JSON files (metadata only) | Postgres once accounts exist. Local launcher state is a small JSON settings file, written atomically. |
| Website | Astro 7 (static output), reads `shared/metadata` at build | Cheap to host on any static host (Cloudflare Pages, Netlify, GitHub Pages). Live radio status and the "next programme" later come from the API at view time, so a stale build can never claim a show is on. |
| Other operating systems | Core logic is portable; launcher UI is Windows-only | See "UI framework". |

## CJ Radio broadcasting (decided: hybrid)

**Update 13 Nov 2026: Twitch player first.** Oracle had no free Ampere capacity in London, so the audio server is on hold. CJ Radio's Listen page now shows **Twitch's own embedded player** (Microsoft Edge WebView2, `apps/cj-radio/src/twitch_view.*`, host page `apps/cj-radio/web/`). It's always visible at full size with Twitch's controls, and it's paused when another page is open. The player's own ONLINE/OFFLINE events are the live signal (`cj::station::combineLive`), so CJ Radio still only shows ON AIR when Twitch says the channel is broadcasting. Recordings are MP3 files in the app's `Recordings` folder, played locally. AzuraCast below remains the plan for audio-only listening and recordings once a server is available; both live sources can run together.

The host keeps using **OBS + StreamElements SE.Live** for Twitch and YouTube. OBS also sends an audio-only feed to a free **AzuraCast** server (Oracle Cloud Always Free). AzuraCast is the single source of truth for CJ Radio's apps and website: live status (`live.is_live` **and** `is_online`), listener count, the MP3 stream, and recordings of each live show.

Why not play Twitch/YouTube directly in the app: Twitch's Developer Services Agreement requires its own embedded player for Twitch video, and YouTube's API policies forbid separating audio from video or playing in a background player. A radio app can't legitimately do either, so the app plays its own stream and links out to Twitch/YouTube.

- **Playback** (`apps/cj-radio/src/player.cpp`): WinHTTP stream, then miniaudio MP3 decoding, then a lock-free ring buffer, then the audio device. It reconnects with backoff, stops cleanly when the server reports the show has ended, and never plays while claiming to be live when it isn't.
- **Call-ins** (changed 13 Nov 2026): a **Discord Stage** channel. Callers press Request to Speak and the host invites them up; OBS captures Discord with Application Audio Capture. Listeners see "Call in on Discord" only while live, after an age and consent check. Any other https link still works as a plain "Request to call" link.
- **Sound only**: Twitch's own Audio Only quality via the embed API (`setQuality`). The player stays visible at 534×300, within Twitch's 400×300 minimum.
- **Presenter mode** is switched on per PC by a local file, for CJ Philip's own PC. CJ Radio is his station only; there are no other presenters.

Step-by-step server and OBS setup: `docs/CJ-RADIO-SETUP.md`.

## Installing and updating apps

**Update 13 Nov 2026 (0.3.0): separate installers.** Each app has its own NSIS setup program (`installer/`, built by `tools/make-installers.sh`): `CJCommunity-Setup`, `CJRadio-Setup`, `WritingStudio-Setup` (SkillSwap's is no longer built). They're per-user (no admin prompt), 64-bit, with a Start menu entry under "CJ Community" and an entry in Windows' Installed apps. App installers put the verified package (including `package.json`) into `%LOCALAPPDATA%\CJ Community\Apps\<id>`, which is where the launcher looks, so it finds them without any extra step; it re-checks whenever its window is activated. The launcher installs to `%LOCALAPPDATA%\Programs\CJ Community` and no longer carries the apps. For an app that isn't installed it shows **Download**, which opens `downloadPage` from `shared/metadata/community/links.json`; that's empty until the website is hosted. Remove in the launcher runs the app's own uninstaller. The installers aren't code-signed yet, so SmartScreen may warn; the download page says so and lists SHA-256 checksums. The bundled-package flow below still works for developer builds.


Apps ship as **packages** next to the launcher (`packages/<id>/package.json` lists every file with size and SHA-256). Install verifies every file, copies to a staging folder, then swaps it into `%LOCALAPPDATA%\CJ Community\Apps\<id>`, so a failed install never leaves a half-updated app. Update works the same way, and the launcher refuses to update an app that's running. Each app holds a named mutex while running, so the launcher can show "Running" and a second launch brings the existing window forward. Nothing is downloaded yet. Online updates will add a signed release manifest (Milestone 8); the free route to a signed installer is the Microsoft Store, which signs MSIX packages at no cost.

## NorthRay look and Launcher features (0.17.0, 13 Nov 2026)

- **Look.** A northern night: the Launcher's sidebar and banners, and the website's banners, are night sky with stars and aurora light; pages are cool neutrals. The colours are recorded in `shared/metadata/brand/tokens.json` and followed by hand in `launcher/src/launcher.cpp` (`northRayPalette`, `kNight*`, `kAurora*`) and `website/src/styles/northray.css` (the `.night` class); change them together. (The old `tools/sync-brand.mjs`, which wrote an unused CSS file, is gone.)
- **Drawing.** `Gfx::fillGradient` and `Gfx::glow` (in `libs/cjui`) draw with bands and rings of solid colour rather than gradient brushes, so they look the same on every Direct2D; rounded banner corners are painted over with the page colour for the same reason.
- **Screenshots.** `shared/metadata/screenshots/<id>.json` lists files (`<id>/*.png`) and captions. The Launcher shows them on each app's page with a full-size viewer; the website shows the same files on the app pages. They're taken from an invented demo library made by `studio_seed_demo <folder>` (`tests/seed_demo.cpp`), never from anyone's real books. CJ Radio has none yet: its main screens need the internet and WebView2.
- **First run.** With no Launcher settings file, a welcome page asks where apps are installed and whether updates install automatically, then saves the settings.
- **Website and CJ Philip.** The website advertises NorthRay's own apps (the Launcher and the Writing Studio). CJ Philip's apps (CJ Philip Community and CJ Radio inside it) get only a brief mention: they're an optional download inside NorthRay Launcher, which marks CJ Philip Community "Optional". Their installers are still hosted, because the Launcher's catalogue points at them, but the site has no pages or download links for them.
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

## CJ Radio scheduling rules (implemented in `cjcore`)

- Programmes are authored in **UK wall-clock time** (`startUk`) and resolved to UTC with GMT/BST rules. Times in the spring-forward gap are rejected. Ambiguous autumn times take the first (BST) occurrence, and the future schedule editor should warn about both.
- Kinds: `regular-saturday`, `additional`, `spontaneous`. Statuses: `draft`, `confirmed`, `cancelled`, `postponed` (with `movedTo`).
- A **series** (e.g. "The CJ Philip Show, Saturdays around 19:00") only produces *draft suggestions* for the host. Nothing is published to listeners until a specific programme is confirmed.
- Station state is **Live** only when the broadcast infrastructure reports it (`LiveSignal`). Otherwise it's Starting soon (within 30 minutes of a confirmed start), Upcoming (the station is off air, but a confirmed programme exists) or Off air. A confirmed programme whose start time has passed without a live signal is reported as *overdue*, never as live.
- Listeners see times in their own time zone, with the UK time added when theirs differs.

## Updates and downloads (recommended for Milestone 8)

- A signed **release manifest** (JSON: app id, version, download URL, size, SHA-256, minimum launcher version) published alongside each release. The launcher and website both read it, so download information can't disagree.
- Installers are **Authenticode-signed**. Azure Trusted Signing is the most affordable route for an independent project. The launcher verifies the manifest signature, the file hash and the Authenticode signature before running anything, and always asks first.
- Installer: **WiX Toolset** (MSI) or **Inno Setup**, per-user install to `%LOCALAPPDATA%` so no admin prompt is needed. App updates replace the app folder and leave user data in `%APPDATA%\CJ Community\<app>` alone.
- Releases are built by CI (GitHub Actions on a Windows runner), never from someone's laptop.

## Safety, privacy and permissions (to design before Milestone 6)

- Roles are separate from profiles: CJ Philip has a verified **creator** profile; administration and technical roles belong to the development team. Moderator permissions are scoped (chat moderation ≠ account suspension ≠ broadcast control).
- CJ Radio is CJ Philip's own station: the broadcast console is for him (and anyone he invites as a guest). Other creators who want a station get their own app, not slots on his.
- Younger members: no unsolicited contact from adults they aren't connected with, no private one-to-one lessons by default, and lesson conversations stay inside SkillSwap.
- Recordings involving guests or callers need recorded consent before publication.
- Paid SkillSwap services go through a payment provider (e.g. Stripe Connect), never card details on our servers, and only after refund, dispute and fee policies are written.

## Known gaps in Milestone 1

- No screen-reader support yet (see UI framework).
- No text input controls (not needed until search, sign-in and messaging).
- No standard menu bars in any app: commands live in the window itself (the Writing Studio lists its shortcuts on its Settings page).
- Artwork is drawn by code. Real icons and banners can be dropped in via the metadata `artwork` fields once they exist, but loading image files isn't implemented yet.

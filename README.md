# CJ Philip Community

Creative applications for writing, publishing and broadcasting, **developed by the NorthRay Software project**. The platform has a creative association with author and broadcaster CJ Philip, while the software is presented under its own development identity (`shared/metadata/brand/identity.json`).

Don't add invented developers, staff, partnerships or company history anywhere, and don't present the apps as personally made by CJ Philip. Where a legal notice, privacy policy or licence needs the real owner or operator, state it accurately; that's a decision for the project director, not something to fill in with a placeholder. At launch there are no subscriptions, paid tiers, adverts, mandatory accounts or upgrade prompts, and nothing claims the apps are free forever or nonprofit.

**Products (0.4.0 preview), all published by NorthRay Software:**
- **NorthRay Launcher** (`launcher/`): installs, updates, opens and uninstalls the apps.
- **NorthRay Writing Studio** (`apps/studio`): writing and publishing for authors. Milestone 1 (the Author Library: worlds, series and books) is built; see `docs/studio/`.
- **CJ Radio** (`apps/cj-radio`): CJ Philip's own radio app: The CJ Philip Show, live and usually on Saturdays. Other creators can ask for their own. See `docs/CJ-RADIO-SETUP.md`.
- **CJ Philip Community** (`apps/community`): an optional launcher for the CJ Philip apps (CJ Radio today). It shows what's installed, running or has an update, opens them, and hands getting or updating them to NorthRay Launcher.

See `docs/MIGRATION-NORTHRAY.md` for how the old CJ Community launcher became NorthRay Launcher plus CJ Philip Community.

SkillSwap is postponed. Its code is kept (`apps/skillswap`, off by default) but it isn't part of the launch; see `shared/metadata/postponed/`.

## Layout

```
launcher/        NorthRay Launcher (C++20, Win32 + Direct2D)
apps/community/  CJ Philip Community (optional)
apps/cj-radio/   CJ Radio: live listening, schedule, recordings, presenter page
apps/studio/     Writing & Publishing Studio (Windows app)
apps/skillswap/  SkillSwap (postponed, not built by default)
libs/cjcore/     Portable core: app registry, radio schedule and live-status rules, UK time
libs/studiocore/ Portable Studio core: library format (SQLite), worlds, series, books, backups
libs/cjui/       Shared Windows UI: drawing, window base, controls, HTTP, station monitor, package installer
libs/third_party/ Vendored libraries (nlohmann/json)
shared/metadata/ App registry, brand tokens, real and demonstration content
website/         Public website (Astro, static output)
tools/           Metadata validation, brand sync, build helpers
tests/           Unit tests for cjcore
docs/            Architecture and design notes
VERSION          Single version number for every component
```

## Running the launcher

Install with the setup programs in `build/installers/` (made by `tools/make-installers.sh`): CJ Philip Community, CJ Radio and the Writing & Publishing Studio each install on their own, and the launcher finds the others.

## Building

### On Windows (Visual Studio 2022 with "Desktop development with C++")

```
cmake -S . -B build/vs -G "Visual Studio 17 2022" -A x64
cmake --build build/vs --config Release
ctest --test-dir build/vs -C Release
```

The launcher ends up in `build/vs/launcher/Release/` with its `data` folder.

### Cross-compiling from Linux (how the Milestone 1 build was made)

```
sudo apt install mingw-w64 cmake
tools/build-windows.sh
```

### Core library tests on any platform

```
cmake -S . -B build/native && cmake --build build/native && ctest --test-dir build/native
```

### Website

```
cd website
npm install
npm run dev      # local preview
npm run build    # static files in website/dist
```

### Checking metadata

```
node tools/validate-metadata.mjs
```

## Adding an application

Add a JSON file to `shared/metadata/apps/` (copy an existing one) and run the validator. To make it installable, build its executable and package it with `cmake/make-package.cmake` (see `apps/cj-radio/CMakeLists.txt`). Installed apps live in `%LOCALAPPDATA%\NorthRay\Apps\<app-id>\` by default, and register `NorthRay.<app-id>` in Windows' installed-apps list.

## Installers and website

```
tools/build-windows.sh       # build the three apps
tools/make-installers.sh     # NSIS installers + releases.json, copied into the website
cd website && npm run build  # static site with a download page, output in website/dist
```

Upload `website/dist` to any static host (Cloudflare Pages, Netlify, GitHub Pages). Then put the download page's address in `shared/metadata/community/links.json` (`downloadPage`) so the launcher's Download buttons work.

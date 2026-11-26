# NorthRay Software

Creative applications for writing, publishing, streaming and recording, **developed by the NorthRay Software project** (`shared/metadata/brand/identity.json`).

Don't add invented developers, staff, partnerships or company history anywhere. Where a legal notice, privacy policy or licence needs the real owner or operator, state it accurately; that's a decision for the project director, not something to fill in with a placeholder. At launch there are no subscriptions, paid tiers, adverts, mandatory accounts or upgrade prompts, and nothing claims the apps are free forever or nonprofit.

**Products, all published by NorthRay Software:**
- **NorthRay Launcher** (`launcher/`): installs, updates, opens and uninstalls the apps.
- **NorthRay Writing Studio** (`apps/studio`): writing and publishing for authors; see `docs/studio/`.
- **NorthRay Browser** (`apps/browser`), **NorthRay Files** (`apps/files`) and **NorthRay Capture** (`apps/capture`), each with its own docs folder.

SkillSwap is abandoned (26 Nov 2026): it isn't part of NorthRay. Its dormant rules and metadata remain; see `shared/metadata/postponed/`.

## Layout

```
launcher/        NorthRay Launcher (C++20, Win32 + Direct2D)
apps/studio/     Writing Studio (Windows app)
apps/browser/    NorthRay Browser
apps/files/      NorthRay Files
apps/capture/    NorthRay Capture
libs/cjcore/     Portable core: app registry, UK time, media library
libs/studiocore/ Portable Studio core: library format (SQLite), worlds, series, books, backups
libs/cjui/       Shared Windows UI: drawing, window base, controls, HTTP, package installer
libs/third_party/ Vendored libraries
shared/metadata/ App registry, brand tokens, release notes, demonstration content
website/         Public website (Astro, static output)
tools/           Metadata validation, build helpers
tests/           Unit tests
docs/            Architecture and design notes
VERSION          The suite's version number
```

## Running the launcher

Install with the setup programs in `build/installers/` (made by `tools/make-installers.sh`): each app installs on its own, and the launcher finds the others.

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

Add a JSON file to `shared/metadata/apps/` (copy an existing one) and run the validator. To make it installable, build its executable and package it with `cmake/make-package.cmake` (see `apps/studio/CMakeLists.txt`). Installed apps live in `%LOCALAPPDATA%\NorthRay\Apps\<app-id>\` by default, and register `NorthRay.<app-id>` in Windows' installed-apps list.

## Installers and website

```
tools/build-windows.sh       # build the apps
tools/make-installers.sh     # NSIS installers + releases.json, copied into the website
cd website && npm run build  # static site with a download page, output in website/dist
```

Upload `website/dist` to any static host (Cloudflare Pages, Netlify, GitHub Pages). Then put the download page's address in `shared/metadata/community/links.json` (`downloadPage`) so the launcher's Download buttons work.

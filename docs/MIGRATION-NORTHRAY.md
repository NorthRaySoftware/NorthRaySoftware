# Migration plan: CJ Community launcher → NorthRay Launcher

Decided 13 Nov 2026. **NorthRay Software** is the overarching software publishing identity (`shared/metadata/brand/identity.json`, contact NorthRaySoftware@protonmail.com). The structure becomes:

| Product | What it is | Where its code comes from |
|---|---|---|
| **NorthRay Launcher** | General-purpose app manager: catalogue, install, update, uninstall, launch | Today's `launcher/`, minus community features |
| **NorthRay Writing Studio** | Independent authoring and publishing app | `apps/studio` + `libs/studiocore` (new) |
| **CJ Radio** | CJ Philip's own radio app | `apps/cj-radio` (unchanged in behaviour) |
| **CJ Philip Community** | Optional community app for CJ Philip's work and broadcasts | Community pages moved out of today's launcher into `apps/community` |

None of the apps needs the launcher or Community to install or run.

## What the current launcher contains (reviewed)

- **App management (keep, extend):** `model.cpp` reads the app registry (`shared/metadata/apps/*.json`), detects installs in `%LOCALAPPDATA%\CJ Community\Apps\<id>`, reads `package.json` versions, launches apps, runs each app's uninstaller, and re-checks on window activation. `libs/cjui` has HTTP (WinHTTP), SHA-256 (BCrypt), package verification, the running-app mutex and the window and UI base.
- **Community features (move to CJ Philip Community):** Home announcements, the CJ Radio panel and station monitor, Events, the planned Friends/Messages/Notifications pages, and the demo profile.
- **Shared (keep in both):** theme setting, Settings page skeleton, keyboard handling, the Library page's app detail layout.

## Steps (each one builds, runs and is testable on its own)

1. **Identity.** Done: NorthRay Software name, contact and logo live in one setting. Window titles, credits, Windows file details and installers read it.
2. **CJ Philip Community app.** Copy the community pages, model and station monitor into `apps/community` (exe `CJPhilipCommunity.exe`) with its own registry entry, icon, version, config folder and installer. Nothing is deleted from the launcher until this runs.
3. **NorthRay Launcher.** Remove the community pages from `launcher/`, rename it (exe `NorthRayLauncher.exe`), and refocus it on the catalogue: Library (installed and available), an app page (description, version, release notes, Install / Update / Open / Uninstall, download progress), Updates, and Settings (install folder, automatic updates, theme).
4. **Online catalogue and downloads.** The website publishes `catalogue.json`: per app, the version, release notes, installer URL, size and SHA-256. The launcher downloads an installer with a progress bar, checks its size and SHA-256 before running anything, runs it silently into the chosen install folder, and refreshes. Optional automatic updates (off by default) do the same for apps that aren't running. Only `https://` downloads are allowed, apart from `localhost` for testing. Offline, the launcher still lists and opens installed apps; the last catalogue it saw is cached.
5. **Install detection.** Installers register `HKCU\…\Uninstall\NorthRay.<id>` with the install folder and version, so apps can live in any folder the user picks. The launcher reads those entries, with the default folder (`%LOCALAPPDATA%\NorthRay\Apps\<id>`) as a fallback.
6. **Website.** NorthRay Software branding: the catalogue, a page for each app, a prominent NorthRay Launcher download, direct installer downloads, About and Contact. CJ Philip Community gets its own page, not the whole site.

## Compatibility with existing installs

- The new installers check for the old `CJCommunity.<id>` uninstall entries and remove the old copy silently before installing. Settings aren't touched.
- App settings move to `%LOCALAPPDATA%\NorthRay\<App>`. On first run, each app copies its old settings across from `%LOCALAPPDATA%\CJ Community\…` if the new location is empty. The old files are left in place, as a fallback.
- Writing Studio libraries are user folders and never move.

## Rollback

Every step is a separate build. Old installers keep working until the new ones replace them, and no user data is deleted at any step.

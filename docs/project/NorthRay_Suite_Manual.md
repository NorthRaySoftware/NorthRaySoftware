# NorthRay Suite Manual

The whole NorthRay ecosystem in one document: what it is for, every application, how they connect, and where it is going. Checked against the repository at suite version **0.169.0** on **26 November 2026**.

Status labels used in this manual:
- **Implemented**: in the code and released.
- **Partial**: some of it is built; the rest is noted.
- **Planned**: agreed with the project director, not built.
- **Proposed**: suggested, not yet agreed.

Each app's manual has the detail:

| App | Manual |
|---|---|
| Launcher | `launcher/MANUAL.md` |
| Writing Studio | `apps/studio/MANUAL.md` |
| Browser | `apps/browser/MANUAL.md` |
| Files | `apps/files/MANUAL.md` |
| Capture | `apps/capture/MANUAL.md` |

---

## 1. Vision and philosophy

NorthRay is a suite of creative desktop applications for Windows: writing and publishing, browsing, managing files, and streaming and recording. The aim is software that **looks like a small, careful team made it, not a template**, and that a creator can live in all day.

Principles, drawn from `docs/DESIGN.md`, `docs/ARCHITECTURE.md`, `README.md` and the director's decisions:

1. **Never pretend.** Nothing is shown as live, online, installed or available unless it is. Unbuilt features say so. Demonstration content is always marked.
2. **Local first, no account.**
   - No subscriptions, paid tiers, adverts, mandatory accounts or upgrade prompts at launch.
   - No NorthRay telemetry.
   - Nothing claims the apps are permanently free or non-profit.
3. **Native and light.**
   - C++20 on Win32 with Direct2D and DirectWrite.
   - Small static executables with no runtime to install, except the WebView2 runtime that Windows already ships.
4. **Plain words.** Buttons say what they do. Messages say what's true and what will change it.
5. **Honest identity.**
   - The software is "Developed by the NorthRay Software project".
   - Never invent developers, staff, partnerships or company history.
   - Legal notices must state the real owner accurately; that is the director's decision.
6. **Permissive licences only.** No GPL or AGPL code linked or bundled.
7. **Safety with secrets.**
   - Stream keys and tokens live only in Windows Credential Manager, never in files or logs.
   - Untrusted plug-ins are never enabled automatically.
8. **No AI-generated artwork** anywhere in the apps, branding or promotion. Writing Studio has **no AI features at all**.
9. **House interface rules.**
   - No menu bars.
   - No scroll bars.
   - No status bars.
   - The app draws its own window frame.
   - One calm button style with icons and words.
   - Undo in preference to "are you sure?".

## 2. Application directory

| Application | Executable | Version | Status | One line |
|---|---|---|---|---|
| **NorthRay Launcher** | `NorthRayLauncher.exe` | 0.169.0 (= suite) | Preview, released | Installs, updates and opens the apps; Today and Jump back in |
| **NorthRay Writing Studio** | `WritingStudio.exe` | 0.93.0 | Preview, released | Writing, world bible, planning, typesetting and publishing for authors |
| **NorthRay Browser** | `NorthRayBrowser.exe` | 0.9.2 | Preview, released | A WebView2 browser with reference boards, a colour picker and a video pop-out |
| **NorthRay Files** | `NorthRayFiles.exe` | 0.9.2 | Preview, released | A file manager with labels, tags, collections and art-file previews |
| **NorthRay Capture** | `NorthRayCapture.exe` | 0.21.2 | **In development**; released early | Streaming and recording: per-program audio, multistreaming, overlays. Recording and streaming are **not yet verified on real hardware** |
| SkillSwap | none | none | **Postponed** | Only its core rules (`libs/cjcore`) and metadata remain. The app folder isn't in the repository |
| Community launcher and radio app | none | none | **Removed** (suite 0.131, 22 Nov 2026) | Removed at the director's request; only in git history |

**Shared infrastructure** (detail in `docs/project/Shared_Infrastructure.md`):
- `libs/cjui`: the UI toolkit.
- `libs/cjcore`: portable rules, the registry and time.
- `libs/studiocore`: Writing Studio's core.
- `shared/metadata`: the registry, brand and release notes.
- `website/`: the public site and downloads.
- `installer/` and `tools/`: the build and release pipeline.

## 3. Each application

### NorthRay Launcher
- **Purpose:** the suite's front door and update channel.
- **Audience:** anyone with more than one NorthRay app.
- **What it does:**
  - Installs apps after checking size and SHA-256.
  - Updates automatically while an app is closed, and retries failed downloads.
  - Runs in the tray and can start with Windows.
  - Shows a Today strip: today's words, the goal, the streak and the newest pictures.
  - Jump back in: reopen a book at its chapter, or Capture on its scene.

### NorthRay Writing Studio
- **Purpose:** a complete studio for authors, from the first draft to a print-ready PDF/X-1a and an EPUB 3.
- **Audience:** novelists and series writers (comics and scripts too), especially those with invented worlds.
- **What it does:**
  - Library of worlds, series and books.
  - ProseMirror editor with spelling, a thesaurus, style hints and comments.
  - A deep world bible: families, maps, calendars, languages, checks, sharing as a website or PDF.
  - Planning: corkboard, timeline, structure, threads, chapter pages.
  - Review, typesetting, ebook preview, editions, cover designer.
  - Optional sync with the writer's own GitHub repository.

### NorthRay Browser
- **Purpose:** an everyday browser with creative tools.
- **Audience:** general users, artists and writers who collect references.
- **What it does:**
  - Tabs, vertical tabs and tab windows.
  - Bookmarks with import and export; history; downloads.
  - Ad and tracker blocking.
  - Passwords through the engine's own store.
  - Reference boards and a colour picker.
  - Video pop-out.
  - Backup and restore.

### NorthRay Files
- **Purpose:** a File Explorer alternative with creative organising.
- **Audience:** anyone with folders of files, especially art and media.
- **What it does:**
  - Tabs and two panes.
  - Search, including the whole PC through Windows Search.
  - A preview pane and quick look, with Photoshop, Procreate and Krita pictures.
  - Batch rename.
  - Labels, tags, collections and a gallery view.
  - The Recycle Bin inside the app.

### NorthRay Capture
- **Purpose:** a real alternative to OBS Studio, light on older PCs.
- **Audience:** streamers and creators.
- **What it does:**
  - Display, window and camera capture.
  - Per-program audio and a routing matrix.
  - Filters and plug-ins.
  - MKV and MP4 recording.
  - RTMP and RTMPS multistreaming from one encode.
  - An overlay designer with nine themes, a brand kit, timers, chat and now playing.
  - Writing Studio overlays.
  - Automation, hotkeys and a setup wizard.

## 4. How the applications relate

```
                 shared/metadata (registry, brand, release notes)
                    |                        |
           copied at build              read at build
                    v                        v
             NorthRay Launcher  ----->  website (GitHub Pages: catalogue.json, installers)
                    |  installs, updates, opens (never links)
     +--------------+--------------+--------------+
     v              v              v              v
 Writing Studio   Browser        Files         Capture
     |                                            ^
     +-- summary.json (book, chapter, words, ---->+  (writing overlays,
         cast, sprint, goal)          \             spoiler-safe switch)
                                       +--> Launcher (Today, Jump back in)
 Capture's settings.json + scenes --------> Launcher (Jump back in)
 All apps: libs/cjui (look, windows, menus, dialogs), libs/cjcore
```

- Apps are independent executables. They never link to each other or to the Launcher.
- Integration goes through small, one-way files and command-line hand-offs (see Shared Infrastructure §4).

## 5. Shared design and user experience

**Look**
- A northern night. Title areas and sidebars are night sky with softly twinkling stars kept clear of text, and an aurora band.
- Pages use cool neutrals.
- Writing Studio warms them to paper (#F4F1EA light, #12141B dark), with serif headings and gold kickers.

**Tokens**
- `shared/metadata/brand/tokens.json`: light, dark, night and aurora colours; Segoe UI Variable; radii 8 and 12.
- These are copied by hand into `launcher/src/launcher.cpp` and `website/src/styles/northray.css`. Change all three together.

**Per-app accents**

| App | Accent |
|---|---|
| Writing Studio | #2D4A6E (its identity colour; the app now uses the NorthRay palette, warmed) |
| Browser | #2FC6A0 |
| Files | #3A8FD8 |
| Capture | #D84A5A |

**Controls**
- Calm buttons: radius 7, lift on hover, an icon from the label's words. A setting chooses icons only, words only, or both (in Writing Studio).
- Themed menus that search when long.
- Themed question boxes that scroll long text.
- Toasts with Undo.

**Accessibility baseline**
- Keyboard everywhere.
- Focus rings.
- WCAG AA contrast for faint text.
- Colour never the only signal.
- Reduced motion respected.
- **Gap:** no UI Automation provider in cjui, so screen readers can't read drawn controls. This is the biggest shared accessibility debt.

**Note:** `docs/DESIGN.md` still describes the original warm-paper and claret palette and says "no gradients, glows or glass". That predates the NorthRay night-and-aurora look (0.17) and needs rewriting. `docs/project/NorthRay_Design_Guidelines.md` is the current summary.

## 6. Shared infrastructure and integrations (summary)

| Area | Detail |
|---|---|
| UI | cjui: `AppWindow`, `Gfx`, menus, dialogs, icons, the NorthRay sky |
| Storage | JSON settings (versioned, read defensively with `cj::valueOr`, saved atomically); SQLite for libraries and stores |
| Secrets | Windows Credential Manager (Capture's stream keys, Writing Studio's GitHub token) |
| Distribution | Per-user NSIS installers; `catalogue.json` with SHA-256; the Launcher's verified updates; the website on GitHub Pages. **Installers are not code-signed yet** |
| Crash reports | `%LOCALAPPDATA%\NorthRay\<app>\crash.log` |
| Web links | `cjui::northray::openWebsite(page)` |

## 7. Opportunities for integration

**Planned or requested by the director, not built:**
- Browser → Capture: send a page or video as a browser source.
- Browser or Files → Writing Studio: send pictures to boards.
- Files → Capture: drag into the NorthRay library; a folder as a slideshow.
- Launcher: search across the suite; back up every app's settings; roll back an update; one release feed.
- A shared social-handles profile across apps (Capture's `profile.json` is a start).

**Proposed:**
- One shared media library used by every app (`%LOCALAPPDATA%\NorthRay\Library` exists, through Capture).
- A cjui UI Automation provider, which would make every app accessible at once.

## 8. Future expansion

- **Capture to stable:**
  - Verify recording and streaming on real hardware and real platforms.
  - Measure performance against OBS.
  - Browser sources.
  - Vertical canvases.
- **Writing Studio:**
  - Milestone 10: accessibility, performance and recovery testing.
  - Writing extras: ambience sounds, a writing heatmap, standard manuscript format export.
  - Word round trip.
- **Browser:** screenshots, reader view, tab groups and workspaces.
- **Files:** zip, picture tools, duplicates.
- **Release engineering** (proposed in the architecture documents):
  - Code signing (Azure Trusted Signing).
  - A signed release manifest.
  - CI builds on a Windows runner.
- **Cross-platform:** only the core libraries are portable today. A macOS or Linux version would need a new UI layer. The architecture document records Qt 6 as the likely route if that becomes a goal.
- **SkillSwap:** postponed. The safety and payment notes in `docs/ARCHITECTURE.md` stay for if it returns.

## 9. Where to look next

| Need | Document |
|---|---|
| Start a session | `CLAUDE_PROJECT_INSTRUCTIONS.md` |
| What's happening now | `NorthRay_Handover.md` |
| Shared code | `docs/project/Shared_Infrastructure.md` |
| Problems and gaps | `ISSUES_AND_MISSING_INFO.md` |
| The director's decisions | `07_Decision_Logs/` (archive) or the Claude project docs |

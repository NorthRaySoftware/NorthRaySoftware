# NorthRay Development Handover

**Written:** 26 November 2026. Last updated 27 November 2026 at suite **0.185.0** (Writing Studio 0.108.0, Browser 0.9.6, Files 0.9.5, Capture 0.22.1).

**Purpose:** everything needed to resume NorthRay development in a fresh conversation, with no access to earlier chats.

---

## 1. NorthRay in one paragraph

NorthRay Software is a suite of native Windows creative apps:
- **Launcher**
- **Writing Studio**
- **Browser**
- **Files**
- **Capture**

They are written in C++20 on Win32 and Direct2D, with a shared toolkit (cjui) and a shared look (night sky and aurora). There are no accounts, no telemetry and no subscriptions. Releases are built by cross-compiling with MinGW, packaged as per-user NSIS installers, and published to GitHub Pages, where the Launcher reads a checksummed catalogue. The project director is CJ Philip; the development partner (Claude) is called Claw.

## 2. Applications and status

| App | Version | Status | Health |
|---|---|---|---|
| NorthRay Launcher | 0.185.0 | Preview, released | Stable. Tray and notifications not checked on real Windows |
| NorthRay Writing Studio | 0.108.0 | Preview, released | Very active. Core tests 2,711 pass and editor tests 31 pass. Editor features need checking on Windows |
| NorthRay Browser | 0.9.6 | Preview, released | Stable. Engine-dependent features need checking on Windows. 315 tests pass |
| NorthRay Files | 0.9.5 | Preview, released | Stable. 373 tests pass |
| NorthRay Capture | 0.22.1 | In development | Feature-rich. **Recording and streaming are unverified on real hardware or platforms.** 45,641 tests pass |
| SkillSwap | none | **Abandoned** (26 Nov 2026) | Dormant rules and metadata only; delete when agreed |
| Community launcher and radio app | none | Removed in 0.131 | History only |

## 3. Important file locations

| What | Where |
|---|---|
| Project instructions (for Claw) | `docs/project/CLAUDE_PROJECT_INSTRUCTIONS.md` |
| Suite manual | `docs/project/NorthRay_Suite_Manual.md` |
| App manuals | `launcher/MANUAL.md`, `apps/studio/MANUAL.md`, `apps/browser/MANUAL.md`, `apps/files/MANUAL.md`, `apps/capture/MANUAL.md` |
| Architecture (suite, Launcher) | `docs/ARCHITECTURE.md` |
| Architecture (per app) | `docs/studio/ARCHITECTURE.md`, `apps/browser/docs/ARCHITECTURE.md`, `apps/files/docs/ARCHITECTURE.md`, `apps/capture/docs/ARCHITECTURE.md` |
| Writing Studio library format | `docs/studio/FORMAT.md` (needs a refresh; see the issues list) |
| Roadmaps | `docs/ROADMAP-2026-11.md`, `docs/studio/BIBLE-ROADMAP.md`, `docs/project/Roadmap_and_Outstanding_Work.md` |
| Decisions | `docs/project/Decision_Log.md`, plus the director's request documents (archive `07_Decision_Logs/`) |
| Issues and gaps | `docs/project/ISSUES_AND_MISSING_INFO.md` |
| Design | `docs/project/NorthRay_Design_Guidelines.md` (current); `docs/DESIGN.md` (outdated) |
| Shared code | `docs/project/Shared_Infrastructure.md` |
| Setup | `docs/project/Environment_and_Setup.md` |
| Versions and release notes | `VERSION` (suite); `apps/*/VERSION` (Browser, Files, Capture); `shared/metadata/releases/*.json` (Writing Studio's version is the top entry of `writing-studio.json`) |
| Writing Studio user manual | Claude Doc "NorthRay Writing Studio Manual" (artifact `5c5e6e8b-fabc-4a0c-aca9-4b42c7ca40dd`); latest PDF in the archive's `02_Application_Manuals/` |
| Live website | https://northraysoftware.github.io/NorthRaySoftware/ |
| Repository | `github.com/NorthRaySoftware/NorthRaySoftware`, branch `source` (development) and `main` (site) |

## 4. Current development priorities

1. **Real-hardware verification of Capture**: recording, streaming, multistreaming, per-program audio, cameras. The director tests builds on Windows; plan a test session with a checklist.
2. **Windows verification of Writing Studio's editor features** added since 0.34: the 0.92 chapter openings, the right-click additions, pasted pictures.
3. **Housekeeping:**
   - Rewrite `docs/DESIGN.md`.
   - Refresh `docs/studio/FORMAT.md`.
   - Update `writing-studio.json` (description, features, version).
   - Delete the stray `ed1.png`.
4. **The Writing Studio user manual:** replace the personal-world examples with generic ones (NorthRay independence).
5. **UI Automation**, which affects every app. Code signing when the director decides on a route (undecided).
6. **Feature work**, in each app's agreed priorities (`Roadmap_and_Outstanding_Work.md`).

## 5. Outstanding tasks

Writing Studio 0.94.0 (phase 1 of the director's combined request of 26 Nov) and 0.95.0 (the bible and plan as Markdown) are released. Interface zoom (phase 2a, suite 0.172.0) is published, but **hasn't run on Windows yet** (Wine wouldn't start in the build environment); the director tests it from the Launcher or `H:\NorthRay_Project\Test_Builds\0.172.0`. Standing instruction (26 Nov): every release is published (steps 6 and 7), not only pushed. Phase 2b, the writing page's own zoom and Auto width, is 0.97.0; since 0.98.0 (suite 0.174.0) interface size changes only what's between the bars, at the director's word. Phase 2c (responsive layouts) started with Writing Studio 0.99.0 (suite 0.175.0): on a narrow window or a large interface size the outline is a drawer behind an Outline button and details cards sit under the page; the Library's shelves widen on a wide screen. Suite 0.176.0 finishes 2c: Capture 0.22.0 puts its panels in rows on a narrow dock (the rule is `Layout::arrange`, unit-tested), the Launcher's banner and app page stack when narrow, Browser 0.9.5's Bookmarks actions go under the title; Files already gave way. None of 2c has run on Windows. Phase 3 is Writing Studio 0.100.0 (suite 0.177.0): a one-row toolbar per workspace built from its tools list (`studio/toolbar.hpp`, tested), pinned / hidden / moved / regrouped by right-click, hideable per workspace, Show tools as Menus in Settings, and Ctrl+K finding the page's tools. Not run on Windows. Phase 4a (the bible's character icons: colour, picture or typed symbols, badges, Small/Medium/Large) is 0.101.0 (suite 0.178.0), also not run on Windows. Phase 4b (the same icons over the text in Writing, on name cards and the corkboard) is 0.102.0 (suite 0.179.0). Phase 4c (the viewpoint icon in the printed book and the ebook, the key of viewpoints, the missing-symbol and soft-picture checks) is 0.103.0 (suite 0.180.0); the director asked for DOCX icons with that export. The **DOCX export** is 0.104.0 (suite 0.181.0): laid out as the typeset book, with the viewpoint icons; read back by the Studio's Word import and opened in LibreOffice, not yet in Word. Phase 5a, chapter dates as formatted text (`studio/chapter_dates.hpp`, one line for Writing and every output), is 0.105.0 (suite 0.182.0). Phase 5b (the date's look per part, alignment, place, scene dates at breaks) is 0.106.0 (suite 0.183.0). Phase 5c (typed date lines into chapter dates, with a preview and Undo) is 0.107.0 (suite 0.184.0): the director's combined request of 26 Nov is complete. None of phases 2c–5 has run on Windows yet; the director's checks there come first.

**Suite 0.185.0 (27 Nov, done unattended overnight at the director's request):** Writing Studio 0.108.0 finishes his three outstanding requests:
- **1.7:** bible quotes skip typed scene-break marks, headings typed as paragraphs and title repeats.
- **2.1:** Interface size's "Also scale the top bar, rail, menus and messages", off by default, in cjui for every app. Capture and the Launcher show it in Settings, Browser and Files in their Interface size menus.
- **Scene dates:** right-click a break › Set this scene's date…; the date under the break; typed lines after breaks converted; asked once to print; scenes in the outline; "Scene 2 of …".

Decisions I made without him (he was asleep), for him to confirm or change:
1. **The scene's date lives on the break itself** (`scene_break` attrs `sid`, `day`, `time`/`part`), not on Planning's timeline. It moves, copies and undoes with the text; the timeline doesn't show in-text scenes yet.
2. **"The same date picker as a chapter's"** is the details card, with the same Date and Time of day boxes, headed "Scene 2 of [chapter]". The chapter has no separate picker window.
3. **Ask once rather than turn on silently.** The first time a scene's date differs from its chapter's, a Yes/No box asks whether to print scene dates. Either answer is remembered per book, and Typesetting › Chapter dates keeps the setting.
4. **Writing shows a scene's date under its break even when it equals the chapter's.** Print shows it only when it differs.
5. **The outline shows scenes by default** for chapters with breaks: Scene 1 is the chapter's opening, and each row has a date when it differs. "Hide scenes" turns them off.
6. **Quotes skip "heading-like" lines**: short lines starting Chapter/Prologue/Part…, short all-caps lines without a sentence end, short all-bold lines, and the chapter's title. A one-word shout such as "THEY RAN." is kept because it ends a sentence.
7. **Browser and Files** got the whole-window option in their Interface size menus, as they have no Settings page for it.

Not run on Windows: the right-click item, the scene details card, the outline rows, the ask-once box, and whole-window scaling in every app. The editor side was run in Chromium, and printing was checked in the tests. The per-app "Planned" lists are in each manual and in `Roadmap_and_Outstanding_Work.md`.

## 6. Important technical decisions (summary; full list in `Decision_Log.md`)

**Platform and code**
- Native Win32, Direct2D and DirectWrite through cjui for every app. Writing Studio's editor, the Browser's pages, and the ebook and review-copy views use WebView2.
- Portable cores (`libs/cjcore`, `libs/studiocore`, `apps/*/core`) hold the logic and are unit-tested on Linux.
- Permissive licences only.

**Data and secrets**
- Integration between apps is one-way files plus command-line hand-offs. No app links to another.
- Secrets only in Windows Credential Manager. No accounts, no telemetry.

**Releases and interface**
- Every release bumps the suite `VERSION`. Push to `source`; only `publish-site.sh` updates `main`.
- No menu bars, scroll bars or status bars.

**Writing Studio**
- No AI. Rule-based checks only.

**Capture**
- Pasted stream keys, not sign-in.
- No game hooking.
- No virtual camera.

## 7. Known problems

See `ISSUES_AND_MISSING_INFO.md`. The main ones:
- Verification gaps (Capture hardware; Writing Studio editor; Browser engine).
- Outdated `DESIGN.md` and `FORMAT.md`.
- Stale Writing Studio metadata.
- Unsigned installers (route undecided).
- No UI Automation.
- No legal pages yet. The legal owner and operator is CJ Philip.

## 8. Dependencies and setup

See `Environment_and_Setup.md`.
- **Toolchains:** MinGW-w64 GCC 13 with CMake and NSIS for Windows builds; GCC or Clang for native tests; Node ≥ 22 for the editor and website; Wine and Xvfb plus Playwright Chromium for UI checks without Windows.
- **Bundled:** all third-party C and C++ code is vendored.
- **Run time:** the WebView2 Runtime (included with Windows 11).

## 9. Recommended next steps

0. Ask the director to check 0.185.0 on Windows (scene dates end to end, and Interface size with the new option on and off in each app) and to confirm or change the seven decisions under section 5.

1. Start a fresh conversation in the Claude project, with `CLAUDE_PROJECT_INSTRUCTIONS.md` as the instructions and this archive attached or extracted.
2. Choose: a Windows verification pass (Capture and the Studio editor), housekeeping, or the next feature round.
3. If continuing Writing Studio: the next unbuilt items the director said yes to on 18 Nov are ambience sounds, a deadline goal, a heatmap, set-ups and pay-offs, a revision checklist, character comparison, charts and manuscript format. Ask him which to do first.
4. If continuing Capture: browser sources (WebView2) were promised "soon" on 15 Nov and are the most requested missing source.

## 10. How to resume development from this archive

1. Extract `NorthRay_Complete_Project.zip`.
2. Read `00_Project_Overview/README.md` (the contents guide), then this handover.
3. Give the new conversation `08_Claw_Instructions/CLAUDE_PROJECT_INSTRUCTIONS.md`. Paste it into the project instructions, or attach it.
4. **To work on code:**
   - Either clone the GitHub repository (branch `source`), which is the live copy,
   - or restore `10_Git_History/northray.bundle` with `git clone -b source northray.bundle NorthRay`.
   - `03_Application_Projects/NorthRay-source/` is the same tree without history or build outputs, for reading.
5. Follow the continuity procedure in the instructions: identify the app, read its manual, check the roadmap and issues, inspect the source, confirm the state (`VERSION`, release notes, run the tests), then continue.
6. At the end of the session, update the manuals, the architecture document, the release notes and this handover.

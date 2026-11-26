# NorthRay Roadmap and Outstanding Work

This document consolidates what's planned, as of suite 0.178.0 (26 November 2026).

**Writing Studio, the director's combined request (26 Nov):** phase 1 (fixes) is 0.94.0; the bible and plan as Markdown is 0.95.0; phase 2a, interface zoom in cjui for every app, is suite 0.172.0. Phase 2b, the writing page's own zoom and Auto width, is 0.97.0; 0.98.0 (suite 0.174.0) makes interface size apply only between the bars. Phase 2c, responsive layouts app by app, began with Writing Studio 0.99.0 (suite 0.175.0: the outline as a drawer, cards under the page, wider Library shelves); suite 0.176.0 finishes it (Capture 0.22.0's panels in rows, the Launcher's narrow banner and app page, Browser 0.9.5's Bookmarks actions; Files already gave way when narrow). Phase 3, tools visible at all times (one-row toolbars you can arrange, Show tools as Menus, Ctrl+K finding tools), is Writing Studio 0.100.0 (suite 0.177.0). Phase 4a, the bible's character icons (colour, symbol as picture or typed character, badge, size setting), is Writing Studio 0.101.0 (suite 0.178.0). Then, in order: phase 4b (viewpoint icons in Writing and Planning, hover cards) and 4c (printed icons); phase 5 chapter dates as formatted text; and a DOCX export of the manuscript (the director's choice), which phases 4 and 5 print into.

**Sources:**
- `docs/ROADMAP-2026-11.md` (14 Nov)
- the director's feature answers of 15 Nov (`northray-feature-questions-2026-11.md`) and 18 Nov (`northray-answers-2026-11-18.md`)
- the architecture documents
- the release notes

**How the status was checked.** An item counts as **Built** only when it appears in the release notes or architecture documents. Everything else here is **not confirmed built**. Check the code before assuming either way.

---

## 1. Priorities across the suite (recommended order)

1. **Verify Capture on real hardware.** Recording with a hardware encoder, streaming to a real platform, multistreaming, per-program audio and cameras. These are built but have only been checked against ffmpeg and a local server.
2. **Verify the Writing Studio editor inside the real app on Windows.** WebView2 isn't available in the test environment. This covers the right-click items, pasted pictures, chapter openings, keys reaching the app, the gliding cursor and similar.
3. **Code signing and a signed catalogue**, once the director picks a route (undecided on 26 Nov). Installers currently trigger SmartScreen warnings.
4. **Accessibility: a UI Automation provider in cjui.** It benefits every app.
5. **Housekeeping from the issues list** (`ISSUES_AND_MISSING_INFO.md`): `docs/DESIGN.md`, `docs/studio/FORMAT.md`, the stale `writing-studio.json` metadata.
6. Then feature work, in the director's per-app priorities below.

## 2. NorthRay Launcher

| Item | Status |
|---|---|
| The ten roadmap decisions (auto-updates, tray, cards, tour, install folder, themes, checksums...) | Built (0.19) |
| The 18 Nov polish list (speed and time left, retries, badges, dates, drag order, tray progress and tooltip, About) | Built (0.98) |
| Recent screenshots widget (Today) | Built (0.59) |
| Search across the suite | Not confirmed built |
| Back up every app's settings in one file | Not confirmed built |
| Roll back an app's update | Not confirmed built |
| One feed of every app's release notes | Not confirmed built ("What's new" shows the latest note per app) |
| Code signing, signed manifest, CI | Proposed (architecture document) |

## 3. NorthRay Writing Studio

**Director's priorities:** worldbuilding first, then editing tools; bigger, tested releases.

| Item | Status |
|---|---|
| Roadmap stages 1–3 (focus mode, versions, spelling and hints, timeline, corkboard, structure, boards, wiki links) | Built (0.20–0.23) |
| World bible round 2 (clean-up, templates, galleries, family trees, maps, checks, glossary, magic, spoilers, sharing) | Built (0.31–0.34 and later) |
| 18 Nov polish list (Ctrl+K, cursor, scroll memory, night paper, goal bar, family-tree zoom, and more) | Built (0.35) |
| Maps, calendars, sprints, conlangs, magic pages, bible PDF and wiki, mood and image boards, plot-thread lanes | Built |
| Chapter header pictures, scene-break ornaments, chapter pages, openings in Writing | Built (0.89–0.92) |
| Ambience sounds while writing | Not confirmed built |
| Name generator from the world's sounds (rule-based) | Not confirmed built |
| Goal with a deadline; writing heatmap | Not confirmed built |
| Set-ups and pay-offs; revision checklist per chapter | Not confirmed built |
| Compare two characters; charts (scenes per viewpoint, sentence length, name frequency) | Not confirmed built |
| Standard manuscript format export; query letter and synopsis templates | Not confirmed built |
| USB export and import for a laptop | Not confirmed built (GitHub sync is built and covers multi-PC use) |
| iPad sketch import; Word round trip (comments and tracked changes back) | Not confirmed built (Word import accepts tracked changes) |
| Split view; scratchpad per book; series-wide find and replace with preview | Not confirmed built (rename a name everywhere exists in Review) |
| Milestone 10: accessibility, performance and recovery testing | Not done |
| AI features, dictation, read aloud, dramatis personae | Decided against |

## 4. NorthRay Browser

**Director's priorities:** links to other apps first, then creative tools.

| Item | Status |
|---|---|
| History, downloads, blocking, vertical tabs, start page (picture, clock, notes, weather), default-browser offer, reference boards, colour picker | Built |
| 18 Nov polish list (previews, tab windows, mute, zoom per site, session restore...) | Built (0.9) |
| Send to Capture (page or video) | Not confirmed built |
| Screenshots, reader view, side-by-side pages, notes per site | Not confirmed built |
| Tab groups, workspaces, named sessions, pinned app tabs, sleeping tabs | Not confirmed built |
| Save as PDF, always-on-top reference window, font identifier, dictionary on double-click, translate | Not confirmed built |
| Search shortcuts, forced dark, cookie banners declined, password generator, QR code, media button | Not confirmed built |
| Extensions, sync, profiles, private windows | Decided against |

## 5. NorthRay Files

**Director's priorities:** creative file tools, organising, looks and speed. Batch rename was first.

| Item | Status |
|---|---|
| Roadmap decisions (operations, tabs and panes, labels and tags, collections, gallery, preview, search, Windows menu, sidebar, auto-refresh) | Built (0.2–0.6) |
| Batch rename | Built (0.7) |
| 18 Nov polish list | Built (0.8–0.9) |
| Thumbnails for videos, PDFs, PSD and Procreate | Built (0.4.1) |
| Replace File Explorer as the default (optional) | Not built ("later") |
| Duplicates, picture conversion, resizing and rotation, zip, folder compare | Not confirmed built |
| Palette in preview, font previews, video and audio in preview, rename by date taken | Not confirmed built |
| Saved searches, Recent, disk-space map, notes on files, sort Downloads | Not confirmed built |
| Send to Writing Studio boards, drag into Capture's library, folder as a Capture slideshow | Not confirmed built |
| UI Automation; labels that follow outside moves | Planned (architecture §7k) |

## 6. NorthRay Capture

**Director's priorities:** overlay designer and templates first, setup wizard and profiles second. Both are built.

| Item | Status |
|---|---|
| Milestones 1–10 and 0.11–0.21 extras (see its manual) | Built; recording and streaming **unverified on real hardware** |
| Web pages as sources (browser sources), Send to Capture | Not built ("Not yet" in the features list) |
| Vertical canvases per destination; replay buffer | Not built (later) |
| Camera background blur or removal; colour correction | Not confirmed built |
| Import scenes from OBS; back up everything to one file; draw on stream | Not confirmed built |
| Dedicated Stream Deck plug-in; auto-framing; WebM export | Not built |
| Performance measurement against OBS on the target PC (Milestone 12) | Not done |
| Milestones 11–14 of the original brief | **Unknown** (not in the repository, and the director doesn't have them). Define new milestones from this table |
| Virtual camera, live captions, live bitrate graph, TikTok, Twitch/YouTube sign-in | Decided against or parked |

## 7. Abandoned

**SkillSwap: abandoned** (26 Nov 2026). Not coming back. The broken build option was removed; dormant rules and metadata remain until deleted.

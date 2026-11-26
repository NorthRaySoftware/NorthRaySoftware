# Known Issues, Inconsistencies and Missing Information

Found during the project audit of 26 November 2026, suite 0.169.0. Nothing here was invented to fill a gap. Each item says what is known and what is missing.

## A. Verification gaps (built, not proven on real Windows hardware)

| # | Area | What's unverified | Why |
|---|---|---|---|
| A1 | Capture | Recording with a real hardware encoder; streaming and multistreaming to real platforms; process-loopback audio; cameras; WGC; hotkeys in games | The build environment has no GPU encoder, audio devices or internet ingest. Formats were checked against ffmpeg and a local RTMP server |
| A2 | Writing Studio | Editor features inside the app: right-click items, pasted pictures, chapter openings (0.92), keys reaching the app, and more | WebView2 isn't available in Wine. The editor is tested in Chromium with a stand-in host |
| A3 | Browser | Every engine-dependent feature: page previews, mute, per-site zoom, site icons, smooth scrolling, downloads, the video pop-out window | Tested with `--no-engine` in Wine |
| A4 | All apps | Windows 11 snap layouts, real toast notifications, high-DPI caption buttons, tray icon progress | Wine limitations |
| A5 | Writing Studio | PDF/X-1a conformance | Built to the specification and checked with qpdf; never run through a real preflight validator |
| A6 | Capture | Performance on the target PC (i7-3770K and GTX 960) compared with OBS | Planned for Milestone 12; not done |

## B. Repository issues

| # | Issue | Suggested fix |
|---|---|---|
| B1 | `-DCJ_BUILD_SKILLSWAP=ON` runs `add_subdirectory(apps/skillswap)`, but `apps/skillswap` has never been in this repository | Remove the option, or recover the app's source. Update `shared/metadata/postponed/README.md` either way |
| B2 | `docs/DESIGN.md` describes the original warm-paper and claret palette and says "No gradients, glows or glass". The suite has used the NorthRay night-and-aurora look since 0.17, and Writing Studio uses warm paper with cards and shadows since 0.68 | Rewrite it from `06_Design_Guidelines/NorthRay_Design_Guidelines.md` |
| B3 | `docs/studio/FORMAT.md` is titled "schema version 3" and its table says 7. The code is `kSchemaVersion = 8`, plus columns added since without a bump | Refresh the format document from `library.cpp` |
| B4 | `shared/metadata/apps/writing-studio.json` still holds the 0.61.0 version fallback, description and feature list. The website uses the installer's version when it has one, so this mostly shows only in fallbacks, but the feature list misses everything from 0.62 onwards | Update the description and features. Consider having the release script set `version` |
| B5 | `apps/studio/editor/ed1.png` is a stray screenshot committed in 0.28 | Delete it if it isn't used |
| B6 | Brand colours are copied by hand in three places: `tokens.json`, `launcher/src/launcher.cpp`, `website/src/styles/northray.css` | Generate them from `tokens.json`, or check them in `validate-metadata.mjs` |
| B7 | Very large source files: `apps/studio/src/studio_app.cpp`, `studio_world.cpp` | Split them when next touched |
| B8 | The installers aren't code-signed | Azure Trusted Signing or the Microsoft Store (proposed) |

## C. Documentation and continuity notes

| # | Note |
|---|---|
| C1 | The **Writing Studio user manual** is a Claude Doc (project artifact `5c5e6e8b-fabc-4a0c-aca9-4b42c7ca40dd`, "NorthRay Writing Studio Manual"). Its examples and templates use the director's own fictional world and characters. Under the NorthRay Independence rule they should be replaced with generic examples. It was left unchanged here, because rewriting it wasn't part of this task. Its latest PDF is in `02_Application_Manuals/`. |
| C2 | Other apps have no separate end-user guide. Their `MANUAL.md` files are development and reference manuals. |
| C3 | The decision logs in `07_Decision_Logs/` are the director's requests and answers, kept word for word. Some use his own fictional names, because they were requests about his own test library. Keep them as records; don't reuse them as examples. |
| C4 | Two older Claude project docs, `claude/cj-community-architecture.md` and `claude/cj-radio-setup.md`, describe the removed community and radio apps. They aren't copied into the archive: the apps were removed from NorthRay at the director's request. They remain in the Claude project. |
| C5 | The Claude project's copies of the Browser, Files and Capture architecture documents and the roadmap are older than the repository's. **The repository versions are authoritative.** |
| C6 | Capture's original brief, with milestones 11–14 in full, isn't in the repository. Only Milestone 12 (performance measurement) and 14 (release) are mentioned. |
| C7 | Legal pages (privacy policy, licence terms, owner and operator) haven't been written. `README.md` says they need the real owner stated accurately, which is the director's decision. |

## D. Missing information (needs the director)

- Who the legal owner and operator is, for notices and the privacy policy (C7).
- Whether SkillSwap should be revived, and where its app source is (B1).
- The intended route and budget for code signing (B8).
- Capture's remaining milestones (C6).
- Whether macOS or Linux versions are a goal. That decides between staying on Win32 and moving to Qt, as recorded in `docs/ARCHITECTURE.md`.

# NorthRay Decision Log

Important decisions, newest first. Each links to its source. The director's own request and answer documents are kept word for word in the archive's `07_Decision_Logs/director_requests_and_answers/` and in the Claude project (`claude/*.md`). The architecture documents record the technical decisions in full.

## Suite-wide

| Date | Decision | Source |
|---|---|---|
| 26 Nov 2026 | **Legal owner and operator: CJ Philip.** The developer line stays "Developed by the NorthRay Software project" | The director's answers after the archive |
| 26 Nov 2026 | **SkillSwap abandoned** (not coming back); its broken build option removed | The director's answers after the archive |
| 26 Nov 2026 | **Windows only.** macOS and Linux versions aren't a goal, so the suite stays on Win32/Direct2D | The director's answers after the archive |
| 26 Nov 2026 | Code signing: undecided. Capture milestones 11–14: unknown, to be defined afresh | The director's answers after the archive |
| 26 Nov 2026 | Long question boxes scroll inside the box and always fit the screen. This applies to every app through cjui | Suite 0.169.0; `docs/studio/ARCHITECTURE.md` §0.93 |
| 23 Nov 2026 | Calm controls for every app: one button style with icons, WCAG AA faint text, a deeper light-theme green | Suite 0.140; Browser 0.9.1, Files 0.9.1, Capture 0.21.1 |
| 23 Nov 2026 | Every app links to NorthRay's website | Suite 0.141 |
| 22 Nov 2026 | **Remove the community and radio apps and every mention of them.** NorthRay holds NorthRay apps only | `claude/studio-requests-2026-11-22.md` item 9; commit `638652c` |
| 22 Nov 2026 | Every app release bumps the suite `VERSION`, because installers carry the suite version, which the Launcher compares | `docs/studio/ARCHITECTURE.md` §0.62 |
| 15 Nov 2026 | The Launcher holds NorthRay apps only, plus the Today strip | Launcher 0.59; `northray-feature-questions-2026-11.md` |
| 14 Nov 2026 | The roadmap's ten-question decisions per app | `docs/ROADMAP-2026-11.md` |
| 14 Nov 2026 | The apps draw their own window frames; the Launcher runs in the tray | `docs/ARCHITECTURE.md` (0.19.0) |
| 13 Nov 2026 | **Identity:** "Developed by the NorthRay Software project". No invented people or history. No subscriptions, ads or mandatory accounts at launch | `docs/ARCHITECTURE.md`; `README.md` |
| 13 Nov 2026 | SkillSwap postponed (abandoned 26 Nov) | `shared/metadata/postponed/README.md` |
| 13 Nov 2026 | The NorthRay look (night sky and aurora) | `docs/ARCHITECTURE.md` (0.17.0) |
| 12 Nov 2026 | **Native Win32 + Direct2D**, not Qt, WinUI or Electron (confirmed 26 Nov: Windows only) | `docs/ARCHITECTURE.md` "UI framework" |
| Standing | No menu bars, no scroll bars, no status bars. No fake functionality. Secrets only in Credential Manager. No telemetry. No AI-generated artwork. NorthRay independence from the director's personal works | Project instructions |

## Writing Studio

| Date | Decision | Source |
|---|---|---|
| 26 Nov | Chapter header pictures (0.89), a chapter's own page in Planning (0.90), scene-break pictures and print checks (0.91), openings in Writing (0.92) | `studio-chapter-headers-*`, `studio-chapter-page-request-2026-11-26.md` |
| 26 Nov | More visual, less text: icons, kind symbols, portraits, the year wheel and strip, status rings; speed work | `studio-visual-requests-2026-11-26.md` |
| 26 Nov | Ages from the calendar; Checks for ages, time gaps and capitals; plans that round-trip; chapter dates in typeset books | `studio-requests-2026-11-26.md`, `studio-ages-answers-2026-11-26.md` |
| 23 Nov | Book types and sub-books; a calendar maker with month 0; plans in the shared bible | `studio-series-calendar-plans-answers-2026-11-23.md` |
| 23 Nov | Excerpts in the shared bible, chosen from Planning's Who's there | `studio-bible-excerpts-answers-2026-11-23.md` |
| 23 Nov | Second UI/UX redesign: no left sidebars (tabs plus a tools menu), editing in place, book pages with tabs, warm paper and cards | `studio-uiux-pass2-answers-2026-11-23.md` |
| 23 Nov | First UI/UX pass: side rail, top-bar search, calm buttons, undo instead of asking | `studio-uiux-answers-2026-11-23.md` |
| 22 Nov | Sync a world with the writer's own GitHub repository (a pasted token, automatic, everything) | §0.61 |
| 21 Nov | Import improvements: plans fill existing chapters; pictures, renames, merges and removals in imports; notes per book | `studio-import-family-requests-2026-11-21.md` |
| 15 Nov | **No AI at all.** Checks are rule-based. Priorities: worldbuilding, then editing tools | `northray-feature-questions-2026-11.md` |
| 14 Nov | World bible round 2: clean-up, templates, families, maps, checks, glossary, magic, spoilers, sharing | `docs/studio/BIBLE-ROADMAP.md` |
| 13 Nov | Stack and engine: one layout engine (HarfBuzz, FreeType, Knuth–Plass) for preview and PDF; SQLite library; ProseMirror in WebView2; permissive licences only; IngramSpark's PDF/X-1a rules | `docs/studio/ARCHITECTURE.md` "Decisions" |

## Capture

| Date | Decision | Source |
|---|---|---|
| 22–23 Nov | One Writing card with toggles; nine themes as full design sets; one switch restyles the whole stream | `capture-writing-overlay-themes-answers-2026-11-22.md`; §67 |
| 20 Nov | Writing Studio link: one way, Studio to Capture; a spoiler-safe switch; sprint overlay; templates | `studio-capture-answers-2026-11-20.md` |
| 20 Nov | Overlay polish: aurora and bookish looks; designer editing | `capture-overlay-polish-answers-2026-11-20.md` |
| 18 Nov | 200 yes/no answers. No virtual camera, no live captions, no TikTok, no live bitrate graph | `northray-answers-2026-11-18.md` |
| 15 Nov | Priorities: overlay designer and templates, then wizard and profiles. Twitch chat read-only, no alerts, no guests | `northray-feature-questions-2026-11.md` |
| 14 Nov | **Pasted stream keys instead of signing in** (OAuth parked) | Capture architecture §14 |
| 14 Nov | cjui (not Qt 6); D3D11; WGC and Duplication; no game hooking; MF encoders; own FLV, MP4 and MKV muxers and RTMP; Credential Manager; redacted logs | Capture architecture §0 |

## Browser and Files

| Date | Decision | Source |
|---|---|---|
| 15 Nov | Browser: no extensions, no profiles (workspaces instead), no sync. Priorities: links to other apps, then creative tools | `northray-feature-questions-2026-11.md` |
| 14 Nov | Files: cjui, not WinUI 3; every change through Windows' own file operations; labels kept outside the files | Files architecture §2 and §5 |
| 13 Nov | Browser: **WebView2, not CEF**. The engine is isolated in `src/engine.*` | Browser architecture §1 |

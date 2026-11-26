# NorthRay Writing Studio: Development and Reference Manual

| | |
|---|---|
| Application | NorthRay Writing Studio (`WritingStudio.exe`) |
| Version at the time of writing | 0.93.0. The latest entry in `shared/metadata/releases/writing-studio.json`; the installer carries the suite version, 0.169.0 |
| Status | **Preview**, released. The suite's most developed app |
| Source | `apps/studio/` (the Windows app), `apps/studio/editor/` (the ProseMirror editor, bundled to `apps/studio/web/editor.js`), `libs/studiocore/` (the portable core) |
| Architecture notes | `docs/studio/ARCHITECTURE.md`: decisions, then a section per release, newest near the top |
| Library format | `docs/studio/FORMAT.md` |
| World bible roadmap | `docs/studio/BIBLE-ROADMAP.md` |
| User guide | A separate user manual is kept as a Claude Doc. Its PDF is in `02_Application_Manuals` of the project archive (see the note there) |
| Manual last checked against code | 26 November 2026 |

Status labels used in this manual:
- **Implemented**: in the code and released.
- **Partial**: some of it is built; the rest is noted.
- **Planned**: agreed with the project director, not built.
- **Proposed**: suggested, not yet agreed.

**Testing note.** The editor runs in WebView2, which isn't available in Wine. Editor features are therefore tested in Chromium with `tools/editor-harness.cjs` (a stand-in WebView2 host). The native app is tested in Wine. **The editor inside the real app needs checking on Windows.**

---

## 1. Overview

**Purpose.** A desktop app for writing and publishing books:
- Organise work as worlds, series and books.
- Write chapters and scenes.
- Keep a world bible.
- Plan.
- Review.
- Typeset.
- Preview as an ebook.
- Export a print-ready PDF/X-1a and an EPUB 3.

**Audience.** Authors of novels and series (including comics and scripts), especially those with large invented worlds.

**Philosophy.**
- **Print quality, offline reliability and long-manuscript performance on Windows** are the defensible core. Real-time cloud collaboration isn't.
- Everything stays on the PC. An optional GitHub repository of your own allows writing on more than one computer.
- No NorthRay account, no subscription.
- **No AI at all.** All checks are rule-based (the director's decision).
- Organisation is metadata, never file location.
- Permissive licences only. No GPL or AGPL code is linked or bundled.

**Where it sits.** The suite's flagship. It writes `summary.json`, which the Launcher (Today, Jump back in) and Capture (writing overlays) read.

## 2. Features (implemented unless marked)

### Library
- Worlds, series (reading and chronological order) and books (in a world, in series, or independent).
- Book types: novel, novella, novelette, short story, flash fiction, collection, serial, and your own. Sub-books set between or during books, numbered automatically (2.5, 1.1).
- Tabs: Your worlds, All books, Independent books, Artwork, Trash.
- A book's page with tabs: Overview, Book settings, Editions, Import & export, History.
- Bookshelf and list views with status tags, word counts and progress rings.
- Trash with undo instead of questions.

### Writing
- ProseMirror editor in WebView2, one chapter or scene at a time.
- Semantic paragraph styles. Comic-script styles: page, panel, dialogue, caption, SFX.
- Saves as you type; versions of every chapter; undo that survives reopening.
- A ribbon-style writing bar; the outline (narrow, hideable); a details card.
- Focus and typewriter modes; fonts, size, width and spacing; warm night paper.

**Checks while writing**
- Spell checker (nspell with SCOWL en-GB and en-US) with per-world dictionaries.
- Constructed-language words are known to the checker.
- Thesaurus (WordNet 3.1).
- Style hints with fixes (Off, Quiet, Full), plus your own rules (`.nrstyle.json`).

**Navigation and goals**
- Names linked to bible entries, with hover cards.
- Ctrl+K quick jump; Ctrl+/ shortcut sheet.
- Writing sprints; daily goal bar; streaks.

**Chapter openings (0.92)**
- Each chapter's header picture, date and viewpoint shown above its text.
- Display only, never part of the text.

### Comments and review
- Comments pinned to words. Review: Changes (word-level comparison with versions and snapshots), Snapshots, Names (likely misspellings, names missing from the bible), Comments, Goals.
- Review copies for beta readers: a single HTML file. Readers' comments come back as `.nrcomments` files.

### World bible (Worldbuilding)
**Entries**
- Kinds: character, creature, place, realm, race, group, thing, lore, magic, event.
- Templated sections, including choice sections.
- Facets: gender, sexuality, race, realm, type.
- Symbols, galleries, image boards, notes per book.
- Spoilers per fact, by the book that reveals them.

**Relationships**
- Built-in pairs plus the world's own.
- Family trees (twins, half, step, adoptive, foster, former partners), drawn and saved as pictures.
- A relationship web.

**Places**
- Place nesting.
- Maps with pins, zoom and maps inside maps.
- A world map.

**Words and time**
- Glossary with pronunciation.
- Constructed languages, imported and exported (CSV, SFM, lists, `.nrlang`).
- A calendar maker: months, month 0, weeks, leap rules, eras (counting up or down), moons with anchors, pauses and eclipses, seasons, holidays, times of day. It feeds dates in the bible, ages per book and Planning's timeline.

**Checks**
- Name spellings, facts that disagree, unused entries.
- Ages against the calendar.
- Time between chapters.
- Names written with the wrong capitals.

**Import and sharing**
- Pasting a whole page puts each part where it belongs. Import many pages at once from text, Markdown, Word or ODT, with templates for every kind, a preview of what changes, and undo.
- Long import confirmations scroll, so OK and Cancel stay on screen (0.93).
- Share the bible: a themed static website, a printable PDF, or a reader's guide (cut at a chosen book). It can include excerpts, each book's plan and ages.
- Sync a world with a private GitHub repository of your own. The token is kept in Windows Credential Manager.

### Planning
- Views: Corkboard, Timeline, Structure and Threads.
  - Corkboard: rows or columns, card sizes, colours by viewpoint, thread or status.
  - Timeline: lanes by viewpoint, thread or place; story days or real calendar dates with times of day; story events.
  - Structure: eleven built-in templates plus your own, with beats.
  - Threads.
- Who's there, viewpoint, place and dates on each card.
- A chapter's own page (0.90): its opening, facts, notes, scene cards, the text folded, ‹ › stepping.
- Plans imported and exported as plain text that round-trips.
- Header pictures per chapter.

### Typesetting, preview, publishing
**Typesetting**
- Own engine: HarfBuzz plus FreeType shaping; Liang hyphenation (en-GB and en-US); Knuth–Plass line breaking; pages with running heads, folios, drop caps, chapter openings and contents.
- Front and back matter.
- Chapter dates.
- Chapter header pictures and ornaments (by chapter, part default or viewpoint; tones).
- Scene-break marks or pictures.
- Symbol fallback fonts; 17 bundled typefaces plus a world's own fonts.

**Preview**
- Spreads with guides.
- An ebook on a phone, an e-reader and a tablet.

**Publishing**
- Editions (paperback, hardback, large print, ebook) with their own ISBNs.
- PDF/X-1a:2001 with bleed (validated with qpdf structurally only; **no external PDF/X validator is run**).
- EPUB 3 with NCX, embedded fonts and accessibility metadata.
- A cover designer (ebook and print wrap).
- Pre-flight checks, including header picture resolution, colour in black-and-white editions, and missing descriptions.
- Spine width.

### Import
- EPUB (DRM refused), Word (.docx), OpenDocument (.odt), HTML, Markdown and text.
- PDF is **experimental** (PDFium, loaded at run time).
- Runs in a helper process (`--import-worker`) so a crash can't close the app.

### Interface
- A side rail of workspaces. Search in the top bar.
- Pages with tabs and a "tools ▾" menu instead of sidebars.
- Editing in place, on warm paper with cards.
- Settings › Buttons and menus show: icons only, words only, or both.

### Partial or not verified
- The editor's behaviour inside the real app (WebView2) on Windows: the right-click menu items, pasting pictures, keys reaching the app.
- PDF import: experimental.
- PDF/X conformance is by construction plus a qpdf check. It has not been run through a preflight validator such as Acrobat's or callas pdfToolbox.
- Ornament and emoji glyphs in Wine's UI font show as boxes. On Windows the symbol fonts cover them in typeset output; the UI depends on Windows' fonts.

### Planned (the director's answers; not in the release notes yet)

From 15 and 18 Nov 2026:
- Ambience sounds while writing.
- A name generator from the world's sounds.
- A goal with a deadline.
- A writing heatmap.
- Set-ups and pay-offs.
- A revision checklist.
- Compare two characters.
- Charts: scenes per viewpoint, sentence length, name frequency.
- Standard manuscript format export.
- Query letter and synopsis templates.
- USB export and import for a laptop.
- Import sketches from an iPad.
- Word round trip (comments and tracked changes back).
- A split view.
- A scratchpad per book.
- Series-wide find and replace with preview.

### Decided against
- AI features of any kind.
- Dictation.
- Read aloud.
- Dramatis personae.

### Known limitations
- Windows only. No screen-reader support for drawn UI (no UI Automation in cjui). The editor's HTML is accessible to screen readers.
- The library folder should be kept out of OneDrive and Dropbox (the app warns). Use Settings › Backups' second folder instead.

## 3. Interface and user experience

**Layout**
- Night-sky top bar: the app mark, where you are, the search pill, the caption buttons.
- A 76 DIP rail on the left: Library, Write, Plan, World, Review, Typeset, Preview, Publish, Settings.
- Pages: a gold kicker, a serif title, tabs, a tools ▾ menu, then cards on warm paper (light #F4F1EA, dark #12141B).
- Writing keeps a narrow, hideable outline (F3) and an optional details card (F4).

**Principles** (the director's answers, 23 Nov)
- Fewer things taking attention.
- One button style, with icons and words.
- One accent.
- Undo instead of questions.
- Quiet notes.
- One-line empty states.
- Comfortable density.
- Light and dark treated equally.
- No menu bars, no scroll bars, no status bar.

**Workflows**
1. Create a world, then a series and books.
2. Write.
3. Build the bible: paste or import pages.
4. Plan on the corkboard or timeline.
5. Review.
6. Typeset.
7. Preview.
8. Publish the editions.

**Accessibility**
- Keyboard paths throughout, with a shortcut sheet.
- Focus rings.
- WCAG AA contrast for faint text and gold kickers.
- Colour is never the only signal (kinds have icons; statuses have rings and words).
- Tooltips.
- Reduced motion respected in the editor.

## 4. Technical documentation

| Item | Detail |
|---|---|
| Languages | C++20 (app and core); JavaScript (editor, bundled with esbuild) |
| UI | `libs/cjui` (Win32, Direct2D, DirectWrite); WebView2 for the editor, the ebook preview and review copies |
| Storage | SQLite 3.54.1 (WAL), schema version 8 (`Library::kSchemaVersion`); columns added since through an "adds" list without a schema bump |
| Typesetting | HarfBuzz 10.1.0 (with the subsetter); FreeType 2.13.3; stb_image and stb_image_write; miniz; Liang hyphenation patterns from hyph-utf8 |
| Editor | ProseMirror, nspell, re2js, SCOWL dictionaries, WordNet 3.1 thesaurus (`tools/build-thesaurus.mjs`) |
| Import | miniz (EPUB, DOCX, ODT); PDFium (bblanchon/pdfium-binaries) for PDF |
| Fonts | `apps/studio/fonts` (OFL; one Apache 2.0), `fonts/symbols` (Noto) |

Licences: `CREDITS.md` and `apps/studio/licenses/`.

**Structure**

`libs/studiocore/` is portable, with no Windows code:

| Area | Files |
|---|---|
| Library and store | `library.cpp` |
| Manuscript and documents | `manuscript.cpp`, `document.cpp` |
| Import | `import.cpp`, `markup.cpp`, `docx.cpp`, `text.cpp`, `pdf.cpp` |
| Artwork and editions | `artwork.cpp`, `editions.cpp` |
| Bible | `entries.cpp`, `entry_paste.cpp`, `family*.cpp`, `conlang.cpp`, `calendar.cpp`, `ages.cpp`, `continuity.cpp`, `bible_export.cpp`, `words.cpp` |
| Planning and review | `planning.cpp`, `review.cpp`, `reviewcopy.cpp`, `comments.cpp` |
| Typesetting and output | `typeset*.cpp`, `ebook.cpp`, `publish.cpp`, `cover.cpp`, `fonts.cpp`, `headers.cpp` |
| Sync | `sync.cpp` |

`apps/studio/src/`:

| File | Contents |
|---|---|
| `studio_app.cpp/.hpp` | The window, workspaces, fields, settings, summary |
| `studio_writing.cpp` | Writing |
| `studio_world.cpp` | The bible |
| `studio_plan.cpp`, `studio_plot.cpp` | Planning, timeline and structure |
| `studio_planpage.cpp` | A chapter's own page |
| `studio_headers.cpp` | Chapter headers |
| `studio_typeset.cpp`, `studio_preview.cpp`, `studio_publish.cpp`, `studio_cover.cpp` | Output |
| `studio_review.cpp`, `studio_comments.cpp` | Review |
| `studio_paste.cpp` | Paste and import of pages |
| `studio_calendar.cpp` | Calendars |
| `studio_sync.cpp` | GitHub sync |
| `studio_share.cpp` | Sharing the bible |
| `studio_fonts.cpp`, `studio_languages.cpp`, `studio_boards.cpp`, `studio_relations.cpp` | Bible tools |
| `studio_search.cpp` | Search |
| `studio_visual.cpp` | Portraits, kind marks, date chips, rings, the year wheel and strip |
| `studio_pages.cpp` | Shared page pieces |
| `studio_booktypes.cpp` | Book types |
| `web_pane.cpp` | WebView2 host |

`apps/studio/editor/src/`: `editor.js`, `hints.js`, `spell.js`, `style_ui.js`, `namecard.js`, `paste.js`.

`apps/studio/web/`: the built editor, the ebook preview, the review-copy template, dictionaries.

**Data storage**
- **Library folder** (user's choice; default `%USERPROFILE%\Writing Studio Library`):
  - `library.db`.
  - `objects/` (content-addressed by SHA-256).
  - `backups/`: on opening (newest 10), every 30 minutes when changed (newest 20), and on closing.
  - `fonts/world-<id>/`.
  - `cache/appearances-<book>.json` (not synced).
- **App state** (`%LOCALAPPDATA%\NorthRay\Writing Studio\`):
  - `settings.json`.
  - `summary.json`, read by the Launcher and Capture.
  - `crash.log`.
- **Secrets:** the GitHub token in Windows Credential Manager. It is never in files or messages.

**Environment variables** (development only)

| Variable | Use |
|---|---|
| `NORTHRAY_TIMING` | Timing log file |
| `NORTHRAY_FRAME_LOG` | Frame times (cjui) |
| `NORTHRAY_GITHUB_API` | A stand-in GitHub API; only `http://127.0.0.1:` addresses are accepted |
| `STUDIO_DEBUG_IMPORT` | Prints converted HTML |

**Command line**
- `--open-book <id> [--chapter <id>] [--library <folder>]`, passed on to a running copy with WM_COPYDATA.
- `--import-worker`, used internally.

**External integrations**
- GitHub's REST git data API (optional sync).
- WebView2.
- Files read by the Launcher and Capture.

## 5. Development

**Status:** preview, 122 releases (0.1 to 0.93).

**Milestones**

| Milestone | Release |
|---|---|
| 1. Library | 0.1 to 0.4 |
| 2. Editor | 0.5 |
| 3. Import | 0.6 |
| 4. Artwork | 0.8 |
| 5. Typesetting | 0.9 |
| 6. Preview | 0.11 |
| 7. Publishing | 0.12 and 0.13 |
| 8. Worldbuilding and planning | 0.14 and 0.15 |
| 9. Review | 0.16 and 0.18 |

Milestone 10, release (accessibility, performance and recovery testing), is **not done**.

Since then:
- 0.20 to 0.35: writing tools, the bible, polish.
- 0.36 to 0.62: bible depth, imports, family, sync.
- 0.64 to 0.72: two UI/UX passes and speed.
- 0.73 to 0.86: book types, calendars, plans in the bible, ages and checks.
- 0.87 and 0.88: visuals and drawing speed.
- 0.89 to 0.92: chapter headers, the chapter page, scene breaks, openings in Writing.
- 0.93: scrolling confirmations.

**Outstanding**
- Verify the editor features on Windows.
- Milestone 10.
- The planned list above.
- UI Automation.
- PDF/X validation with a real preflight tool.

**Technical debt**
- `studio_app.cpp` and `studio_world.cpp` are very large.
- `docs/studio/FORMAT.md` says "schema version 3" in its title and 7 in its table, while the code is 8 plus added columns. It needs a refresh.
- `shared/metadata/apps/writing-studio.json` still has the 0.61 description, features and version fallback.

## 6. Maintenance

**Run:** `WritingStudio-Setup`, or `build/windows/apps/studio/WritingStudio.exe`.

**Build**

```
cd apps/studio/editor && npm install && npm run build     # editor bundle -> apps/studio/web/editor.js
timeout 1500 cmake --build build/windows --target WritingStudio -j6   # after tools/build-windows.sh has configured build/windows
```

**Test**

| Suite | How to run | Result on 26 Nov 2026 |
|---|---|---|
| Core | `ninja -C build/native studiocore_tests && ASAN_OPTIONS=detect_leaks=0 build/native/studiocore_tests` | 2,384 checks, all passing |
| Editor | `cd apps/studio/editor && npm test` | 28 tests, all passing |

**Inspection tools** (built with the native tests)

| Tool | What it does |
|---|---|
| `studio_seed_demo <folder>` | Makes the invented demo library used for screenshots |
| `studio_typeset_dump` | A sample book's pages, PDF, print PDF, EPUB and pre-flight |
| `studio_import_dump` | What an import finds |
| `studio_bible_dump` | A bible site and PDF |
| `studio_bench_seed` | 600 entries and an 80-chapter book, for speed |
| `studio_sync_dump` | Round-trips a library through sync |

**Editor harness:** `tools/editor-harness.cjs` runs the editor in Chromium (Playwright) with a stand-in host.

**Troubleshooting**

| Problem | Cause and fix |
|---|---|
| The editor is blank | The WebView2 Runtime is missing |
| A library won't open | Its schema is newer than this build; or restore a copy from `backups/` |
| Sync errors | The token or repository permission; messages never include the token. Big worlds are sent in batches |
| A slow first open of the bible | The appearance cache is being built; later opens use `cache/` |

**Implementation notes**
- Every library change runs in a transaction.
- Imports run in a separate process.
- Never trust ZIP entries: path, size and ratio limits apply.
- Comments never reach published output.
- A whole-book plan import replaces only what the plan covers, and trashes only empty nodes.
- Bump the suite `VERSION` with every Studio release, because installers carry the suite version.

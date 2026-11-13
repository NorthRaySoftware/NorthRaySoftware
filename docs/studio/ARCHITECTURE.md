# Writing & Publishing Studio: architecture

Status: **Milestones 1-4 and 6 built, Milestone 5 (typesetting) first version** (Author Library, manuscript editor, import from EPUB, Word, OpenDocument, HTML, Markdown and text with PDF experimental, the artwork library, and typesetting with PDF export). Next: Milestone 7, publishing (PDF/X and EPUB). Last updated 13 November 2026. Developed by the NorthRay Software project, as part of CJ Philip Community. The Studio is a product for any author; it isn't presented as CJ Philip's personal writing app.

This records the research findings that shaped the design (full report: the research brief of 13 Nov 2026), the decisions taken, and what each later milestone builds on.

## What we're competing with, and the gap

| Product | Best at | Weak at | What we take |
|---|---|---|---|
| Reedsy Studio | Free browser editor, goals, version timeline, boards | Online-only, account needed, template formatting, paywalled planning | Goal engine, version browser, boards as views over world records |
| Atticus | Writing and formatting in one, theme builder, cross-platform | Browser-engine print composition, fewer trims | Theme builder, nonfiction elements; beat it on print quality |
| Vellum | Best default styles and print micro-typography, previews | Mac-only, formatting only, little control | "Beautiful by default", per-retailer exports, device previews; Windows is the gap |
| Scrivener | Binder, corkboard, outliner, research, snapshots | Compile is hard; weak print typography | Binder/corkboard/outliner as views of one tree, per-scene snapshots |
| Calibre / Sigil | EPUB metadata, conversion, XHTML editing, EPUBCheck | GPL; no manuscript model | Behaviour only (no code: GPL) |
| Manuskript | Characters, plots, worldbuilding | Little maintenance, basic export | World-level records linked to scenes |
| InDesign / Affinity / Scribus | Page geometry, PDF/X, preflight | Steep, no manuscript structure | Prepress rigour behind a Vellum-simple interface |

The defensible core is **print quality, offline reliability and long-manuscript performance** on Windows. Real-time cloud collaboration isn't the core.

## Decisions

1. **Stack: C++20, Win32 + Direct2D/DirectWrite, CMake, MinGW static builds**, the same as the rest of the suite. Qt was rejected (LGPL relinking duties with static builds, and its text layout doesn't solve book composition). Rust/Typst was rejected for the core: Typst can't produce PDF/X (its issue #6012 is still open), and IngramSpark requires PDF/X-1a or PDF/X-3.
2. **One layout engine.** From Milestone 5: HarfBuzz + FreeType shaping, libunibreak, libhyphen (MPL licence option), an in-house Knuth–Plass line breaker and page builder. It outputs a **page display list**. The Direct2D preview and the PDF writer (with Little CMS for colour) both draw that same list, so the preview can't drift from the export. This is the largest piece of work (roughly 9–15 developer-months to novel-grade quality).
3. **Library storage: a folder with a SQLite database** (`library.db`, WAL mode, transactions, schema version), a content-addressed `objects/` store for images, fonts and snapshot payloads, and rotating `backups/`. SQLite is public domain and the most-tested storage engine there is.
4. **Organisation is metadata, never file location.** Books link to series through *membership* rows (reading order, chronological order, number, kind). Moving a book between series, making it standalone, or moving it to another world changes rows inside one transaction; no files move, and nothing is duplicated.
5. **Editor (Milestone 2):** ProseMirror (MIT) inside WebView2, editing one chapter or scene at a time, with the C++ core as the source of truth. Styles are semantic ("Body", "Chapter Title"), so changing one restyles the whole book. A native editor stays possible later because the model and editor are separate.
6. **Licence rule:** permissive dependencies only (public domain, MIT, BSD, zlib, Apache-2.0, or the MPL option where a library offers it). No GPL/AGPL code linked or bundled: not MuPDF, CKEditor 5, pandoc, Calibre, Sigil or Scribus. EPUBCheck (BSD) will run as a separate optional process.
7. **Print targets (Milestone 7):** IngramSpark's stricter rules by default: PDF/X-1a:2001 (or X-3), CMYK or grey with 100% K body text, ink limit 240%, 0.125 in bleed, 300 ppi images, fonts embedded according to their licence flags (OS/2 `fsType`). EPUB 3.3 with an NCX fallback, plus accessibility metadata.
8. **Imports (Milestone 3):** EPUB (refusing DRM: Adobe ADEPT, Apple FairPlay, Readium LCP; allowing font obfuscation), DOCX, ODT, Markdown, HTML, TXT, each converted into the Studio's own model. ZIP entries are treated as untrusted (no path traversal, size and ratio limits, no nested archives), and HTML/SVG is sanitised.

## Layers

```
libs/studiocore   portable C++ (no Windows code; tested on Linux and Windows)
  model           Library, World, Series, Book, Membership, statuses
  store           SQLite schema + migrations, transactions, trash, backups, export
apps/studio       Windows app: top bar (workspaces, Settings), library sidebar, world/series/book pages,
                  properties panel (native edit controls), cover images (WIC)
later:
  core.text       HarfBuzz/FreeType shaping, hyphenation, line-break opportunities
  core.layout     Knuth–Plass paragraphs, page builder -> PageDisplayList
  render.d2d      preview: draws the PageDisplayList
  export.pdf      PDF/X writer: draws the same PageDisplayList
  export.epub     walks the semantic model (not pages)
  import.*        EPUB, DOCX, ODT, Markdown, HTML, TXT -> semantic model
```

Library hierarchy, manuscript content, worldbuilding data, typesetting rules and exporters stay in separate modules and tables.

## Milestone plan

| # | Milestone | State |
|---|---|---|
| 1 | Foundation, library format, Author Library (World → Series → Book) | **Built** (see below) |
| 2 | Manuscript editor: parts, chapters, scenes, styles, autosave, statistics | **Built** (0.5.0) |
| 3 | Import: EPUB (with DRM refusal), DOCX, ODT, Markdown, HTML, TXT; conversion preview and report | **Built** (0.6.0), with PDF experimental |
| 4 | Artwork: insert, non-destructive crop and scale, units, effective-PPI warnings, artwork library | **Built** (0.8.0) |
| 5 | Typesetting engine | **First version** (0.9.0): shaping, Knuth–Plass, hyphenation, pages, PDF |
| 6 | Preview: spreads, zoom, guides; ebook device simulation | **Built** (0.11.0) |
| 7 | Publishing: PDF/X, EPUB 3, front and back matter, validation, edition profiles | **Built** (0.12.0, editions 0.13.0): PDF/X-1a, EPUB 3, pre-flight, spine, editions |
| 8 | Worldbuilding and planning | **Built** (worldbuilding 0.14.0, planning 0.15.0) |
| 9 | Editorial: snapshots, comparisons, comments, goals, continuity checks | **Built** (0.16.0, comments 0.18.0): changes, snapshots, name checks, goals, comments |
| 10 | Release: accessibility, performance and recovery testing | |

## Milestone 1: what exists

- **Library format** (`docs/studio/FORMAT.md`): SQLite with schema versioning, WAL, foreign keys, every change in a transaction; a backup at each opening (rotating, last 10); a portable JSON export of the whole library.
- **Worlds**: name, description, accent colour, banner image, logo, notes; ordering.
- **Series**: in a world or on their own; name, description, cover, logo, status, notes; reading order and an independent chronological order; numbered and unnumbered instalments; kind (novel, novella, companion, short story, collection).
- **Books**: in a world, in one or more series, or outside any world; title, subtitle, author, pen name, synopsis, genre, language, ISBN, edition, publishing status, tags, cover. Word count is stored but stays 0 until the editor exists.
- **Operations**: create, rename, edit, reorder (drag or keyboard), move books between series and worlds, make a book standalone, move a series between worlds (with its books), delete to Trash and restore, empty Trash.
- **Views**: library sidebar; world page with series and standalone books; series page in reading or chronological order; bookshelf (gallery) and list views; sort by title, series order, author, status or last change; filter by text, status and tag.

Not in Milestone 1 (and not shown as working): manuscripts and the editor, imports, worldbuilding records, typesetting, preview, publishing. Their workspace tabs are visible but disabled, and they say which milestone brings them.

## Milestones 2 and 3: what exists (0.5.0)

- **Manuscript model** (`libs/studiocore/src/manuscript.cpp`): parts, chapters and scenes per book, nesting rules, moves, Trash and restore, word counts, saved versions. Schema 2 (`docs/studio/FORMAT.md`).
- **Document model** (`document.cpp`): the editor's JSON, a sanitiser that enforces the schema, word counting, plain text, and a builder for importers.
- **Editor** (`apps/studio/editor/src/editor.js`, bundled into `apps/studio/web/editor.js` with esbuild): ProseMirror in WebView2 (`web_pane.cpp`), one chapter or scene at a time; the app saves what it reports, and asks for a final save before switching or closing. Pictures are served from the library's `objects` folder through a WebView2 virtual host; the page can't navigate anywhere else.
- **Import** (`import.cpp`, `markup.cpp`, `text.cpp`, `pdf.cpp`): a two-step plan/apply design with a preview. EPUB: safe ZIP reading (miniz, size/count/ratio limits), DRM refusal (Adobe ADEPT, Apple FairPlay, Readium LCP, encrypted content; font obfuscation allowed), OPF metadata and cover, EPUB 3 nav or NCX, parts from a two-level contents, chapters split across files rejoined, CSS classes mapped to italic/bold/small caps/centring/no-indent, scene breaks from `<hr>`, ornaments and asterisms. Text/Markdown: chapter headings by pattern. PDF: PDFium loaded at run time; lines rebuilt from glyph positions, running headers and page numbers removed, headings from type size or the PDF's bookmarks, hyphenation rejoined; password-protected, copy-restricted and scanned PDFs are refused with a reason.
- Tests: `tests/studiocore_tests.cpp` (documents, manuscripts, upgrade from schema 1, EPUB and text import against `tests/fixtures/`), and `studio_import_dump` for looking at what an import finds.
- **Word and OpenDocument** (`docx.cpp`, 0.6.0): both are turned into simple semantic HTML (heading levels from styles or outline levels, style names as classes, run formatting resolved through character and paragraph styles, lists from numbering, quotes, tables as rows, links, pictures, footnotes gathered into a Notes section; tracked deletions, field codes and duplicate fallback drawings left out), then share the EPUB converter and chapter splitting. Documents without heading styles are split at paragraphs like "CHAPTER ONE" or "Prologue". Imports run in a helper process (`WritingStudio.exe --import-worker`), so a reader crash can't close the app.

## Interface (0.7.0)

- **No menu bar.** Every command has a place in the window: New book, Import and New world on the Library page; Move to Trash (and Move up/down for chapters, Import into this book for books) in the details panel's MORE row; + Chapter / + Scene / + Part in the outline; previous and next chapter buttons and formatting in the editor toolbar. The **Settings** page (top bar, or Ctrl+,) holds the library folder actions (open or create, show folder, export, back up), appearance (theme, bookshelf or list, details panel) and the full list of keyboard shortcuts. Alt on its own does nothing. Accelerators are still built in `buildMenu()`.
- **Details panel:** one layout for worlds, series, books, chapters and scenes, built from `panelHeading` (a ruled section with a small-caps label) and `panelChoice` (a labelled menu button for status and kind).
- **Saving indicator:** the editor sends `saved-state {dirty:true}` when typing starts and `change` after 800 ms; the app stores it and answers `saved {id, ok}`. The editor and the status bar show "Saving..." then "Saved" (or "Not saved"). Library fields show "Saving..." until their 700 ms save runs.
- **Navigation:** Ctrl+Up / Ctrl+Down (in the text: editor message `navigate`; elsewhere: accelerators) move by chapter, skipping parts with no text of their own. Up/Down walk the outline without moving focus to the text; Enter moves to the text. Double-clicking a book opens it in Writing.
- **New chapters, scenes and parts** are created as "New chapter" etc. with the title selected (`load` is sent with `focus:false` so the editor doesn't take the keyboard); Enter in the title goes to the text.
- **Your place:** `settings.json` keeps `lastBook`, `lastNode` and `lastInWriting`; `restorePlace()` reopens them on start (cleared when a different library is opened).
- **Editor:** an empty document shows the placeholder sent with `load` (by kind). Switching chapters jumps to the top without smooth scrolling, so it doesn't flicker.
- **No scroll bars:** none in the window (cjui draws none), the editor page (CSS) or the multi-line text fields (no `WS_VSCROLL`; a subclass scrolls them with the wheel).

## Milestone 4: artwork (0.8.0)

- **Core** (`libs/studiocore/src/artwork.cpp`, `include/studio/artwork.hpp`): `imageInfo` reads pixel size from PNG, JPEG, GIF, WebP and BMP headers without decoding; trim sizes (6 × 9 in default, 5.5 × 8.5, 5.25 × 8, 5 × 8, B-format, A5, Royal, 8.5 × 11) with an estimated text width (trim width less 1.25 in until typesetting sets real margins); `Placement` (crop fractions, width as a fraction of the text width, alignment) with `clampPlacement`; `printSize` (printed size and effective ppi), `printQuality` (300+ sharp, 150-300 soft, below 150 poor) and `sharpWidthFraction`; units (in, cm, mm, px).
- **Artwork library** (schema 3, `artworks` table): each stored picture listed once by content; add, rename, kind, notes, Trash and restore; `artworkUsage` finds the chapters whose text uses it. Imports and older libraries register their pictures automatically.
- **In the text**: the editor's image node carries the placement; a node view shows the crop, width and alignment without touching the file. Selecting a picture sends `picture {pos, attrs}`; the details panel then shows the PICTURE section (drag the frame or a corner to crop, width slider, left/centre/right, caption, screen-reader description, printed size and ppi for the book's trim, and a "make it sharp" width). The app answers with `setPicture` / `removePicture`. New pictures start at the widest size that prints at 300 ppi.
- **Artwork page** (Library sidebar): a grid with a print-quality dot for each picture; the panel shows name, kind, notes, pixel size, the largest sharp print size, the chapters using it, and "Insert in" the chapter open in Writing.
- **Scenes**: "+ Scene" adds a *** divider at the end of the open chapter (cursor on the new scene's first line) rather than a separate scene page; scene pages from imports still work.

## Milestone 5: typesetting, first version (0.9.0)

`libs/studiocore/include/studio/typeset.hpp`, `src/typeset*.cpp`:

- **Input**: `snapshot(lib, book)` copies what's needed (titles, chapters' document JSON, picture paths and sizes), so `typesetBook(Source, Design, Resources)` runs on a worker thread while editing carries on.
- **Fonts and shaping** (`typeset_fonts.cpp`): FreeType 2.13.3 (trimmed: TrueType, SFNT, psnames, smooth) and HarfBuzz 10.1.0 (one translation unit, no dependencies), both static. One process-wide font cache with a recursive lock. Families are folders of `-Regular/-Italic/-Bold/-BoldItalic.ttf`; bundled: Crimson Text and Gentium Book Plus (OFL). Small capitals use `smcp` when the font has it, otherwise scaled capitals.
- **Hyphenation** (`typeset_hyphen.cpp`): Liang's algorithm, written here, over hyph-utf8's en-GB (MIT) and en-US patterns, with their exception lists; at least 2 letters before and 3 after a break; names in capitals or small capitals aren't broken.
- **Paragraphs** (`typeset.cpp`): the document model becomes blocks with paragraph styles (body, first, centred, epigraph, attribution, verse, letter, note), headings, quotes, lists (hanging bullets and numbers), scene breaks and pictures. Text becomes boxes, glue and penalties; Knuth–Plass total-fit breaking (tolerance 2.5, then 12; hyphen penalty 50, consecutive-hyphen demerits 3000, fitness classes) and first-fit for ragged settings. Lines are reshaped as whole words so kerning and ligatures are right, then justified by spreading the spaces.
- **Pages**: a baseline grid; title page and blank verso; part pages (recto) with a blank back; chapters on a new page (optionally recto) sunk by `chapterSinkLines`; scene breaks; pictures with their crop and caption (scaled down if taller than the page); orphan, widow and heading-at-foot control; running heads (book title in small capitals on versos, chapter in italic on rectos) and folios.
- **Output** (`typeset_output.cpp`): one display list (glyph runs, rules, pictures) drawn two ways: `renderPage` (FreeType, sub-pixel positioned, for the preview) and `writePdf` (PDF 1.7: Type0/CIDFontType2 fonts with the whole TrueType file embedded, widths, ToUnicode maps for search and copy; JPEGs passed through, other pictures decoded with stb_image and Flate-compressed; TrimBox = MediaBox). Font subsetting came in 0.10.0; PDF/X-1a, bleed and CMYK in 0.12.0 (Milestone 7).
- **Design**: four presets (Classic, Open, Ragged right, Large print) plus typeface, size, line spacing, trim (the book's), justification, hyphenation, chapters on rectos, running heads and title page. Saved per book as `setting:typeset:<book id>` in the `meta` table (no schema change).
- **App** (`apps/studio/src/studio_typeset.cpp`): the Typesetting tab; design controls on the left; spreads in the middle (page 1 alone, then pairs), drawn from `renderPage` at the screen's pixel size and cached per spread; contents with page numbers, settings and notes on the right; Export PDF. Arrow keys, Page Up/Down, Home/End and the wheel turn pages.
- Tests: `testTypeset` in `tests/studiocore_tests.cpp` (hyphenation, justification, running heads, every glyph mapped to text, pixels, PDF structure, trim sizes, missing fonts), and `studio_typeset_dump` writes a sample book's pages and PDF for inspection. `studio_seed_demo <folder>` makes an invented demo library (a world, a series, three books, a bible, a planned and partly written manuscript, a snapshot and a revision) for screenshots.
- **0.10.0 additions**: a contents page (set twice: once with placeholder numbers to fix its length, then with the real page numbers once the pages are laid out); a copyright page on the title verso built only from the book's author, edition and ISBN (blank if none); lead-in small capitals for a chapter's first three words; margin controls; PDF fonts subset with HarfBuzz's subsetter (glyph ids kept, layout tables dropped, `ABCDEF+` names), which cut the sample PDF from 207 KB to 65 KB. HarfBuzz is now built from `harfbuzz-subset.cc` (the whole library plus subsetting).
- **PDF import fixes (0.10.0)**: in `buildFromLines`, a size-detected heading under a "Chapter ..." line joins it as its title, and size-only headings before the first "Chapter"/"Part" line are left as front matter (not for Markdown); PDF metadata placeholders ("unspecified", "untitled", ...) and Word's "Microsoft Word - " title prefix are dropped.

## Milestone 6: previews (0.11.0)

- **Print**: the Typesetting view shows spreads or single pages, at fit, actual size (a point at its real size on a 96-dpi screen) or 200%, scrolled with the wheel when larger than the window; guides draw the text block, the baseline grid and the spine edge. Pages are rendered at the screen's pixel size and cached for the visible pages only.
- **Chapter openings** (typesetting): `chapterStyle` "title", "number" (“CHAPTER ONE” letter-spaced above the title) or "numeral"; `splitChapterTitle` keeps numbers already in titles ("Chapter 3: The Storm") and leaves front and back matter unnumbered; drop caps (three lines, sized so the capital spans from the first line's cap height to the third baseline); `frontRoman` numbers front matter i, ii, iii and starts the story at 1 on a recto.
- **Ebook HTML** (`libs/studiocore/src/ebook.cpp`): `ebook::build(Source, pictureUrl)` turns each part and chapter into a `<section>` with `h1`, paragraph-style classes (`first` after headings and breaks), `em`/`strong`/`span.sc`/`u`/`s`/`sup`/`sub`/`code`, links (web and mail only), `hr.scene`, lists, quotes and `figure` (width as a percentage, crop as `data-crop`); everything escaped. `ebook::stylesheet()` is the reading stylesheet. Milestone 7's EPUB export reuses both.
- **Ebook preview** (`apps/studio/web/ebook.html`, `ebook.css`, `ebook.js`; `apps/studio/src/studio_preview.cpp`): a second WebView2 pane. The page draws the device (phone 390×820, 6-inch e-reader 640×900 with e-ink greys, tablet 860×1150) scaled to fit, flows one chapter at a time into CSS columns the size of the screen, and turns pages by click, arrow keys or wheel. Reader settings: page colour (white, sepia, night), text size, font (the book's typeface from its typesetting design, served from a `fonts.nrstudio.localhost` host, or a system serif or sans). Settings are remembered in `settings.json`.


## Milestone 7: publishing, first version (0.12.0)

`libs/studiocore/include/studio/publish.hpp`, `src/publish.cpp`; `apps/studio/src/studio_publish.cpp`.

- **PDF/X-1a:2001** (`typeset::writePdf` with `PdfOptions::printReady`): the same display list as the proof PDF, written as PDF 1.3; text and rules in DeviceGray `0 g` (pure black, black plate only) instead of the screen's warm near-black; JPEGs passed through only when greyscale, everything else decoded and written as DeviceGray or a plain CMYK conversion (`colourPictures`); no transparency (alpha is composited on white); an `/OutputIntents` entry with `/S /GTS_PDFX` and the registered condition `CGATS TR 001` (SWOP), so no ICC profile has to be embedded; Info with `/GTS_PDFXVersion`, `/GTS_PDFXConformance`, `/Trapped /False`, `/ModDate`; a trailer `/ID`. With `bleed`, pages are 0.125 in larger at the top, bottom and outside edge (right on rectos, left on versos), content shifted with a `cm`, and the `/TrimBox` marks the finished page inside the `/MediaBox`/`/BleedBox`. Checked with qpdf; no external PDF/X validator is run.
- **EPUB 3** (`publish::buildEpub` / `writeEpub`): a zip written here (`mimetype` first and stored, everything else raw-deflated with miniz's `tdefl`, CRCs with `mz_crc32`). `OEBPS/` holds `content.opf` (dc metadata, `urn:isbn:` from a valid ISBN or a name-based `urn:uuid:` from the book id, `dcterms:modified`, schema.org accessibility metadata), `nav.xhtml` (contents, with parts holding their chapters, and landmarks), `toc.ncx` (EPUB 2 readers), `style.css` (`ebook::stylesheet()` plus page styles), cover/title/copyright pages, one XHTML file per part or chapter from `ebook::buildCropped` (the preview's HTML, `epub:type` and `role` added, scene breaks as a `div role="separator"`), and `images/`. Pictures are cropped and scaled (2400 px longest side) ahead of time with stb_image and written with stb_image_write (JPEG q90 for opaque photos, PNG otherwise); uncropped JPEG, PNG and GIF files within the size go in unchanged; WebP and BMP are converted. Control characters are dropped from all text so every file is well-formed XML.
- **Pre-flight** (`publish::preflight(Source, Book*)`): Problem / Advice / Ok checks for title, author, ISBN (ISBN-10 and -13 check digits: `validIsbn`), language, description, empty chapters, cover size (1600 px tall), pictures without descriptions, effective print resolution of every placed picture (150 and 300 ppi), page count (24 minimum), spine text (about 100 pages), the typesetter's warnings, embedded fonts.
- **Spine** (`spineInches`): pages × 0.002252 in (white) or 0.0025 in (cream), with the page count rounded up to even; the tab adds trim, bleed and full cover size, in the user's units.
- **App**: the Publishing tab sets the book's pages again on opening (the Typesetting design, `prepareTypeset`) and re-checks when they finish; a step list on the left; checks, spine and cover, and three files (print-ready PDF with bleed and colour switches, EPUB, proof PDF) in the middle; notes on the right. Paper, bleed and colour were saved per book as `setting:publish:<book id>` (0.12.0); from 0.13.0 they belong to editions.
- Tests: `testPublish` (ISBNs, spine, EPUB structure read back with miniz, crops applied, identifiers stable, no control characters) and PDF/X checks in `testTypeset`; `studio_typeset_dump` also writes `print.pdf` (with bleed) and `book.epub` and prints the pre-flight list.

### Editions (0.13.0)

- **Library** (schema 4, `src/editions.cpp`): an `editions` table (kind: paperback, hardback, large-print, ebook; name; ISBN; for print editions an optional trim, a design preset, paper, bleed, colour). `ensureEditions(book)` gives a book with none a paperback (taking the book's ISBN and its 0.12.0 publishing settings) and an ebook. Editions are removed for good, not trashed, and go with their book (ON DELETE CASCADE); they're in the JSON export.
- **Copyright page**: `snapshot()` lists every edition's ISBN (`Source::isbns`), so the typeset copyright page and the EPUB's read "ISBN 978... (paperback)", one line per edition.
- **Publishing** (`publish::editionSource`, `editionDesign`): an edition is the book's snapshot with that edition's ISBN, kind and trim, set with the book's Typesetting design or the edition's own preset (a new large-print edition starts with the large-print design). Pre-flight takes the edition's kind: ebooks get no print checks, print editions no ebook cover checks, and two editions sharing an ISBN is a problem.
- **App**: editions are listed in the Publishing sidebar (+ Add an edition); the right-hand panel edits the chosen one (name, ISBN, kind, trim, design, remove). Print editions are set on their own worker thread (`WM_APP_EDITION`), separate from the Typesetting tab. Print editions get spine and cover measurements (hardbacks: the thickness of the pages, with the printer's case template for the rest), the print-ready PDF and a proof; the ebook edition gets the EPUB. Once a book has editions, its details panel lists their ISBNs instead of one ISBN field. The last edition used is kept as `setting:publish-edition:<book id>`.

## Milestone 8: worldbuilding (0.14.0)

- **Library** (schema 5, `src/entries.cpp`): `entries` (a world's, or an independent book's when `world_id` is empty: kind character / place / group / thing / lore / event, name, other names as a JSON array, summary, details as "Label: value" lines, notes, picture blob; in the Trash like books) and `entry_links` (from, to, label). Relationships are stored once and read from either side through `relations()` ("Lives in" / "Home of", "Parent of" / "Child of", ...); a link to an entry in the Trash isn't shown. Emptying the Trash of a world or book takes its entries with it (ON DELETE CASCADE).
- **Appears in** (`mentionsOf`): the plain text of every chapter and scene in the world's live books (or the one book) is searched for the entry's name and other names as whole words (a word character is a letter, a digit or any byte of a non-ASCII character, so "Maren's" matches and "Marenna" doesn't), with a count and the first appearance in context. It runs when an entry is opened or its names change.
- **App** (`apps/studio/src/studio_world.cpp`): the Worldbuilding tab (from a world, a series, a book or any book workspace: the book's world, or the independent book itself). The sidebar filters by kind and lists the world's books; the middle shows cards (picture or initials, kind, name, summary) or one entry (details as a table, relationships with links to the other entry, notes, where it appears; a mention opens that chapter in Writing); the right-hand panel edits it (name, kind, other names, summary, picture, details, notes, Move to Trash). A world's Library page summarises its entries and links to it. The Trash lists entries.

## Milestone 8: planning (0.15.0)

- **Library** (schema 6): chapters and scenes gain `pov_id` (a worldbuilding entry) and `target_words`, added to existing libraries with `ALTER TABLE` like the trim size before them. The book's own target is `setting:plan:<book id>` (`{"targetWords": n}`). `appearancesIn(book)` reads each chapter's and scene's text once and finds every entry of the book's world (or the independent book) in it, most mentioned first, using the same whole-word matching as `mentionsOf`.
- **App** (`apps/studio/src/studio_plan.cpp`): the Planning tab. The sidebar shows progress towards the book's target and filters by status (outline, draft, revised, final); the board shows parts as headings and every chapter and scene as a card (number, status with its colour on the card's edge, title, synopsis, viewpoint, the characters found in its text, words against its target, a chapter counting its scenes too). The panel edits the selected card (title, status, viewpoint from the world's characters, target, synopsis; the entries appearing in it link to Worldbuilding; open in Writing; move earlier or later) or, with nothing selected, the book's target. A new `FTarget` field takes numbers only.

## Milestone 9: review, first version (0.16.0)

- **Library** (schema 7, `src/review.cpp`): `snapshots` and `snapshot_nodes` keep a whole book's chapters, scenes and parts (title, kind, document JSON, words, position) until removed, apart from the autosaved `node_versions` (pruned to 40 per node). `writing_log` (day, book, words) gains the words a save adds (`saveNodeContent` only, so imports and restores don't count); `localDay` is the computer's local date. `restoreNodeText` restores any text, keeping the current one as a version first (`restoreNodeVersion` uses it).
- **Comparing** (`review::diffText`): Myers' algorithm on paragraphs, then on words (compared without their trailing spaces) inside each run of changed paragraphs paired up; the rest of a run is whole paragraphs added or removed. A work limit falls back to "all replaced" for pathological inputs. `stats` counts words added and removed.
- **Names** (`review::nameChecks`): words starting with a capital and never written in lower case in the book (possessive "'s" removed; all-capitals words and a short list of titles, days, months and the like skipped). One within 1 letter (2 for names of 8 letters or more) of a word in an entry's name or other names is a likely misspelling, wherever it appears; otherwise one used at least three times, at least once mid-sentence, and not in the bible, is reported as missing from it. Names marked as fine are kept as `setting:names-ok:<book id>`.
- **App** (`apps/studio/src/studio_review.cpp`): the Review tab. Changes: a chapter or scene and something to compare it with (its saved versions, and every snapshot holding it); the text is laid out word by word once per width (added words shaded, removed struck through in red) and drawn only where visible; Restore. Snapshots: take (named from the date, renamed in the panel), list, remove; the selected one against the book now, chapter by chapter, each opening Changes. Names: each finding with Show it, It's another name for…, Add to the bible, It's fine. Goals: today, 7 and 28 days, the streak, a 28-day chart (this book over all books) and the daily goal (`setting:goal:daily`); Writing's status bar shows today's words against it.

## Comments (0.18.0)

- **Library** (schema 8, `src/comments.cpp`): a `comments` row per comment (id, node, body, quote, created, modified, resolved). The comment's place is a `comment` mark in the node's document holding its id (8 to 32 hex digits; several may overlap); the sanitiser in `document.cpp` keeps only valid ones. `resolveComment` keeps the mark so a comment can be reopened; `removeComment(id, stripText)` deletes it and, when asked, takes the mark out of stored text. `commentAnchored` says whether the mark is still there. Typesetting, the ebook and EPUB ignore the mark (tested), so comments never reach what's published.
- **Editor** (`editor/src/editor.js`): the `comment` mark (`excludes: ""`, not inclusive). Comment (✎) on the toolbar or Ctrl+Alt+M marks the selection with a random 16-hex-digit id, saves straight away and sends `{type:"comment", id, node, quote}`. The cursor in an open comment sends `commentFocus`. Highlights are decorations (`cm-open`, `cm-active`), so resolved comments, sent by the app as `{type:"comments", resolved}`, keep their mark but aren't shown. `selectComment` selects a comment's words; `stripComment` removes its mark (the app uses this for the open chapter, because the editor holds that text).
- **App** (`apps/studio/src/studio_comments.cpp`): `Focus::Comment` shows one comment in the panel (the words it's on, what it says in the description field, made/changed, Resolve or Reopen, Delete, a note when its words were deleted). A chapter's or scene's details list its open comments (and resolved ones on request). Review has a Comments section: every comment in the book by chapter, Open or All, each opening its words in Writing. The demo library from `studio_seed_demo` has some.

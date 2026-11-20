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

## Writing tools and backups (0.20.0)

- **Writing view** (`editor/src/editor.js`, `web/editor.html`, `web/editor.css`): the "Aa" button opens a panel (`#viewmenu`) with font (Serif: Cambria/Georgia; Book: Palatino Linotype/Book Antiqua; Sans: Segoe UI; Typewriter: Courier New), size (1.0, 1.12, 1.26, 1.42 rem), page width (36, 44, 56 rem) and switches for focus mode, typewriter scrolling, spell check, style hints and name links. They're CSS variables (`--text-font`, `--text-size`, `--page-width`) and classes on `<html>`. The editor sends `{type:"view", settings}` when the writer changes one; the app keeps it as `writingView` in `settings.json` and sends it back with `{type:"view", settings, names}` when the editor is ready and before each `load`.
- **Focus mode** (Ctrl+Shift+F): a decoration plugin (`focusLook`) gives every top-level block but the one holding the cursor the class `dim` (faded); the toolbar fades until pointed at. The app's `focusMode()` drops the outline and details panel from the Writing layout (`pageRect_` spans the window).
- **Typewriter scrolling** (Ctrl+Shift+T): after each change of text or selection, the scroller moves so the caret sits at 42% of the window's height; the page gets 55vh of space below so the last line can get there.
- **Spell check**: the editor's `spellcheck` attribute (WebView2 uses Windows' spell checker and the language of the page, the book's language); now switchable.
- **Style hints** (`lookups` plugin, off by default): per text block, doubled words, filler words (very, really, just, actually, basically, suddenly, literally, quite, somewhat, totally, completely, simply), a be-verb followed by an -ed word ("maybe passive"), and sentences over 40 words (dotted). Inline decorations with a `title` saying why; a count in the toolbar. Worked out again only when the text or the view changes. Never edits the text. Better rules and writer-made rules are being researched (see the roadmap).
- **Names** (`lookups` plugin, on by default): the app sends every live entry of the book's world (or the independent book) with its other names; one Unicode-aware whole-word pattern, longest names first, marks them `.wl` with the entry id. Ctrl+click sends `{type:"entry", id}`; the app opens the entry in Worldbuilding.
- **Backups** (`Library::backup(err, keep, madePath)`, `changesMade()` = `sqlite3_total_changes64`): on opening (as before, newest 10), every 30 minutes if anything changed (`kBackupTimer`, newest 20) and on closing if anything changed. `settings.json`'s `backupFolder`: after each backup, `mirrorBackups()` copies backups that folder doesn't have yet into `<folder>\<library folder name> backups\` (as `.part`, then renamed, so a sync client never sees half a file), keeping the newest 20. A folder inside the library is refused; a missing drive is reported and the copy waits for the next backup. Settings has a BACKUPS section.
- Tested: the editor in Chromium with Playwright (hints of each kind, names linked, Ctrl+click reporting the entry, focus mode fading, the panel, the setting sent back); `testBackup` in `studiocore_tests` (the copy opens as a library with the book, the path is reported, changes are counted, only the newest are kept); the app under Wine (Settings, backups copied to the second folder). WebView2 isn't available under Wine, so the editor inside the app needs checking on a real PC.

## Style hints and writers' own rules (0.21.0)

Based on the research report "Better Style Hints and Custom Rules" (14 Nov 2026): fewer, higher-signal hints by default; context (dialogue vs narration) before more rules; judgement calls (filter words, passives, hedges) off by default and explained; a form-first rule model like Vale's checks (existence, substitution, occurrence, repetition, consistency) with a safe pattern engine.

- **Engine** (`editor/src/hints.js`, pure functions, tested with `npm test` in `apps/studio/editor`: `test/hints.test.mjs`, 18 tests): `analyse(blocks, opts)` returns `{hints: [{from, to, key, cat|rule, name, msg, why, text, context}], counts, slow}`.
  - *Dialogue*: `dialogueRanges` per paragraph; double quotes, and single quotes told from apostrophes by what's around them (‘ after a space opens; ’ closes only before a space or punctuation and not between letters); an unclosed quote runs to the end of the paragraph.
  - *Sentences*: `Intl.Segmenter`, joining after titles such as "Mr.".
  - *Categories* (`CATEGORIES`; `essential` ones show in Quiet): doubled words (not "had had"/"that that"), mixed spellings and dashes (en-GB/en-US pairs such as -ise/-ize, -our/-or, grey/gray, towards/toward; closed em vs spaced en/em dashes; the less-used form marked, the book's language breaking ties), redundancies (phrase list with suggestions), adverbs on dialogue tags (speech verb within two words of an -ly word that isn't on a not-an-adverb list, next to dialogue), fancy dialogue tags (said-bookisms next to dialogue), echoes (same distinctive word, simple stem, within 50 words of narration; stop words and every word of the bible's names skipped), repeated sentence starts (3+ in narration), clichés, filler words (two or more in a paragraph), long sentences (over 40 words of narration); off by default: started/began to, There was/were openings, filter words, possible passive (be/get + participle, irregular list, adjectival participles skipped), hedges. All word-choice kinds skip dialogue.
  - *Word lists* were written for NorthRay (no third-party lists copied; LanguageTool and commercial tools only informed the design).
- **Writers' rules**: kinds `flag`, `replace` (pairs), `limit` (max per chapter or per 1,000 words), `nearby` (within N words), `consistent` (first spelling kept if used), `pattern`; options `where` (all / narration / dialogue), `wholeWord`, `inflections`, `caseSensitive`, `message` (`%s` = the match), `explanation`, `enabled`. Word lists become one escaped alternation (no backtracking risk); patterns run with **re2js** (MIT, RE2's linear-time matching, no backreferences), at most 300 characters, and a rule taking over 150 ms on a chapter is paused for the session. `readRuleSet` checks imported files (kept fields only, broken rules listed and skipped).
- **Scope**: rules and ignored words belong to the book's world (`setting:style:world:<id>` in the library's `meta` table), or to the book when it's in no world (`setting:style:book:<id>`): `{version:1, rules, ignore}`. The app sends them with `{type:"view", ..., style:{scope, name, label, rules, ignore}}`; the editor sends `{type:"styleRules", rules, ignore}` to save. Export and import use `.nrstyle.json` files through the app's file dialogs (`styleExport`, `styleImport` → `styleImported`).
- **Interface** (`editor/src/style_ui.js`): the hint count in the toolbar opens the hints list (Off / Quiet / Full, each kind on or off with its count, stepping to the next one, the writer's rules with counts, Flesch reading ease clamped to 0–100 as a rough guide); hovering an underline shows a card (why, Ignore once, Ignore "…" in the world, Turn off this kind / Edit the rule); the rules editor covers the page (list with live counts, a form per kind, ignored words, Import, Export). Each kind of hint has its own underline style as well as colour. Hints are worked out 350 ms after typing stops and mapped through edits in between.
- **Settings** (`writingView`): `hints` is now "off" | "quiet" | "full" (old true/false read as full/off; default quiet), `hintCats` holds the writer's on/off per kind.

## No status bar (0.21.0)

The status bar is gone from every app. `AppWindow::drawToast(area)` shows `flash` messages as a rounded note that rises in near the bottom of the given area. Writing Studio: today's words at the foot of the outline while writing, "Saving..." / "Not saved" as a small note at the page's corner (only while it applies); the library path is in Settings. NorthRay Launcher: when updates were last checked is on the Updates page. NorthRay Files: counts and selection at the foot of the sidebar (a note at the list's corner when the sidebar is tucked away). CJ Philip Community: Open NorthRay Launcher in the header, version under the list. NorthRay Browser: messages in a small layered window over the page (`updateNote`, since nothing can be drawn on the page's own window), and WebView2's own link bubble (`IsStatusBarEnabled`). CJ Radio keeps its player bar.

## Themed menus and name cards (0.21.1)

- **Menus**: `cjui::trackMenu(HMENU, POINT, owner)` turns a Windows menu built with `AppendMenuW` (text, `\t` shortcuts, checks, greyed items, separators, submenus) into `cjui::showMenu` items, drawn in the window's palette (`setMenuPalette`, called by `AppWindow::applyPalette`). Every `TrackPopupMenu` in the Studio now goes through it.
- **Name cards** (`editor/src/namecard.js`): `sendView` sends, for each bible entry, its name, kind, other names, summary (600 characters), the first six non-empty details lines and its picture's object hash. Hovering a linked name (400 ms) shows a card: picture from the objects host (initials if none or it fails), kind, name, "Also:" names, summary, details as a two-column list, and Open in Worldbuilding (`{type:"entry"}`). A style hint on the same words keeps its own card.

## Planning: corkboard, timeline and structure (0.22.0)

- **Views** (`pl_.view`, the sidebar's VIEW list): Corkboard (`drawPlanBoard`), Timeline and Structure (`studio_plot.cpp`).
- **Corkboard**: the board drawn as cork (a seeded grain) with index cards (red heading rule, faint blue lines, a pin in the status colour). Cards register `Drop::NodeRow` drops with `horizontal = true`; dragging a card (`pl:card:` hits arm a `DragState::Node`) uses the outline's drop logic read left to right: the outer thirds put it before or after, the middle third of a chapter takes a scene into it (`Library::canContain`).
- **Timeline**: `setting:timeline:<book>` holds `{"unit": "day"|"week"|"month"|"year", "nodes": {"<node>": {"day": n, "when": "words"}}}` (`storyTimes`, `setStoryTime`, `timelineUnit`). The details panel's WHEN IN THE STORY has two new fields, `FStoryDay` (a number; blank takes it off) and `FWhen`. The view has a lane per viewpoint character (then "No viewpoint"), cards stacked when they'd overlap, ticks every few units, a line joining cards in reading order (orange where it goes back in time) and, below, chips for cards not placed yet with Place (after the last). Dragging a card (`planMouseDown/Move/Up`) moves it in whole units; Shift with the wheel scrolls along.
- **Structure**: `setting:structure:<book>` holds `{"template": id, "beats": {"<beat>": "<node>"}}`. Six templates with beats at fractions of the book and guidance written for NorthRay. The view shows the book as a bar of chapters by words (or targets, or equal shares before there's text) with the beats marked, and a row per beat: its position, guidance, the chapter at that length, a chooser for the chapter that carries it, and whether the chosen one comes early or late. `outlineFromStructure` adds a chapter per beat (status Outline, the beat's guidance as synopsis) and matches them.
- **Names** (`nameParts`, `searchNames` in `entries.cpp`, and the same rule in the editor): a character's name and other names also match by their parts (contiguous runs of words starting and ending on a capitalised word that isn't a title or joining word, plus first and last). `countNames` takes longer names first and counts a place once. In the editor, a name shared by several entries links to all of them (`data-entry` holds every id) and its card shows each.
- **cjui fix**: a hit's action is copied before it runs, because an action that opens a menu or dialog lets the window repaint, which rebuilds the hits and freed the running action (it could choose the wrong item or crash).

## Spell checker, wiki links and image boards (0.23.0)

- **Spell checker** (`editor/src/spell.js`): nspell (MIT) with the SCOWL Hunspell dictionaries (`web/dict/en-GB.*` -ise, `en-US.*`), fetched from the editor host (`connect-src 'self'`) by the book's language. Known words: the dictionary, every word of the bible's names, and the scope's own dictionary (`style.words`, saved with the style rules as `words`); British -iz- forms are accepted when the -is- form is a word; possessive 's and CAPITALS, numbers and codes are skipped. Misspellings join the hint pass as `cat:"spell"` (red wavy line, not counted as style hints); the card offers up to five suggestions ranked by Damerau distance and commonness (keeping the word's capitalisation), Add to the dictionary for the world, and Ignore once. While the dictionary loads (or if it can't), the browser's own spell check is used.
- **Hint settings migration**: `writingView` gains `v: 2`; settings without it (0.20's `hints:false`, 0.21's default "quiet") become "full" unless "off" was chosen. The default is now "full" (the noisy kinds stay off by their own defaults).
- **Wiki links** (`findNames` in `entries.cpp`: spans of entries' names in any text, longest first, never overlapping, shared names giving every id; tested): `drawLinkedText` lays out text word by word and draws names as dotted links (dashed when shared; `openEntryChoice` offers a menu). Used for an entry's summary, details values and notes. LINKED FROM lists the entries whose text names this one.
- **Image boards** (`studio_boards.cpp`): `setting:boards:world:<id>` or `boards:book:<id>` = `{"boards": [{"id", "name", "items": [{"id", "blob", "x", "y", "w", "caption", "entry"}]}]}`. Pictures are added with `addArtwork` (so they're in Artwork) and laid out in rows; on the board they're dragged to move and by the corner handle to resize (`boardMouseDown/Move/Up`); `Focus::Board` edits the name, `Focus::BoardItem` the caption, the tied entry (a tag on the picture opens it) and "Use as the entry's picture". Removing a picture or deleting a board leaves the pictures in Artwork.

## Name parts, revised (0.23.1)

A character's name stands for them by its first name or last name alone, or by a run of words that includes the first or the last (titles and joining words aside): "Snarf Jill Grave" gives Snarf, Grave, Snarf Jill, Jill Grave and Snarf Grave, never Jill alone. Every name is also matched in capitals ("JACKSON"). Same rule in `nameParts`/`searchNames` (core: Appears in, planning casts, wiki links) and the editor's links; tested in both.

## Skipped middle names and far suggestions (0.23.2)

Name parts also include every in-order selection of the named words that keeps the first or the last ("Albus Brian Dumbledore", "Albus Dumbledore"); names over 8 words use their first 4 and last 4. Spelling: when nspell offers fewer than 3 suggestions, `farSuggestions` scans the dictionary's base words with the same first letter, adds the misspelling's own ending (s, es, ed, ing, ...) where that makes a real word, and keeps those within 2-4 edits (by length). Tested in `test/spell.test.mjs`.

## Fixes and the thesaurus (0.24.0)

- **Fixes** (`hints.js`): a hint can carry `fixes: [{label, from, to, text, orig}]` (document positions and the text they replace; `applyFix` does nothing if that text has changed). `removal()` takes a word out with a space beside it (and a comma after it), capitalising the next word at a sentence start; `matchCase()` keeps capitals. Doubled words, fillers, hedges and tag adverbs: remove; fancy tags: "asked" (after a question) or "said"; redundancies and the writer's replace/consistent rules: their replacement; spellings and dashes: the chapter's main form; clichés: `CLICHE_PLAIN` (plainer starting points, the possessive carried over); began/started to: `beganFix` (past with an irregular list, third person, or the base form). Echoes carry `earlier` (the earlier use's range; "Show the earlier one" selects it).
- **Thesaurus** (`spell.js`): `web/dict/thesaurus.json` (about 2 MB, fetched on first use) is built by `tools/build-thesaurus.mjs` from WordNet 3.1: for every word or phrase of up to four dictionary words, its three most used senses per part of speech, ordered by tag counts, with similar adjectives and, for nouns and verbs with few synonyms, their most used narrower terms; `"word": "n:a|b;v:c|d"`. `synonyms(text)` takes endings off (-s, -ed, -ing, doubled consonants, -ly) and puts them back on words of the right kind (verb phrases on their first word: "kicked the bucket"), checked against the spell checker, in the original capitalisation.
- **Other words** (≈ on the toolbar, Ctrl+Shift+O): the selection (punctuation trimmed, up to six words) or the word at the cursor; the card stays until a click elsewhere.

## Twinkling stars (cjui, 0.24.0)

`northray::nightSky` dims and brightens each star on its own slow sine (3-10 s, between 60% and 115% of its brightness), seeded by its index so the sky keeps its layout. It reads `cjui::ambientTime()` (seconds, or -1 when Windows' client-area animations are off) and calls `wantAmbientFrame()`; `AppWindow::paint` then sets an 83 ms timer (about 12 frames a second) while the window is visible and not minimised. Checked under Wine: between two frames only star pixels change.

## Hints with fixes, worldbuilding facets, front matter, symbols (0.25.0)

- **Every hint suggests something** (`hints.js`): long sentences `splitFixes` (break at ", and" / ", but" / ";" / ", which" near the middle, both halves 40+ characters); "There was" `thereFix` ("There was a dog barking" → "A dog was barking"); passives `activeFix` (whole sentence "X was Ved by Y." → "Y V-ed X.", irregular participles listed); repeated starts `joinFix` (two short sentences joined with "and"); tag adverbs offer verbs (`STRONGER`: quietly → whispered); filter words get advice with an example (`filterAdvice`). Fancy tags are judged over the chapter: action tags (`ACTION_TAGS`: smiled, grinned, sighed...) always; others when there are 5+ and more than plain tags, or the same one's third use.
- **Spelling**: far suggestions always run; a candidate with the same letters scores 1.5 closer (Capitan → Captain); `SPLIT` words (alot → a lot).
- **Creatures** (`creature` entry kind). **Facets** (`studio_world.cpp`: gender, race, realm for characters; type and realm for places; type and realm for creatures) live as "Label: value" lines in the details (`facetOf`/`withFacet`, several labels read: Gender/Sex, Race/Species, Realm/Plane, Type); open lists (race, realm) offer what the world already uses first. The sidebar lists values in use under the selected kind; `wb_.facet` = "key=value" ("key=-" not set). **Kind colours** (`kindColor`, CSS `.k-<kind>` with `--k`): card band, initials, kind label, sidebar dot; editor name links and name cards.
- **Relationships per kind**: `relations()` lists both directions of every pair; `relationsFor(kind)` gives each kind's choices with the kinds they usually point at (offered first in the entry menu).
- **Review**: `review::replaceName` (whole words in every chapter's document JSON; old text kept with `restoreNodeText`); "Change to <like>" and "Change to…" (bible names, closest first); "Add to the bible…" asks the kind. Name checks fold ’ into ' and skip word forms (same first letters, only the last one or two differ).
- **Show it**: `showInWriting(book, node, words)` → `{type:"find", words}`; the editor's `foundLook` highlights every use, selects the first and scrolls it to 40% of the window; the highlight goes at the next edit.
- **Front and back matter**: `nodes.matter` (0, 1, 2; chapters only; column added on open). Typesetting never numbers them; front matter before the story keeps roman folios.
- **Symbol fallback** (`typeset_fonts.cpp`): `Fonts::useFallbacks(fonts/symbols)` opens every .ttf there in name order (Noto Sans Symbols 2, Noto Sans Symbols, Noto Emoji; OFL). `shape()` splits text into runs by coverage (spaces, ZWJ and variation selectors stay with their neighbours), shapes each run in its face, and records `Shaped::faces`; `Setter::emit` starts a new glyph run whenever the face changes, so the PDF embeds (and subsets) the symbol font too.
- **Panels**: `settings.showSide` (F3, or the button beside Settings) and `showPanel` (F4); the editor sends `togglePanel` for F3/F4.

## Comic scripts, bible by book, races and realms (0.26.0)

- **Format**: `setting:format:<book>` = "comic" (details panel). Choosing it gives a book with no design the `comic-script` preset.
- **Editor**: paragraph styles page, panel, dialogue, caption, sfx (also kept by `document.cpp`'s sanitiser); `comicLook` decorates each with `data-label` (PAGE ONE (right · two panels), Panel n, balloon numbers; pages continue from `comicPageStart`, the page paragraphs in earlier chapters) and the speaker before the colon in bold; `comicEnter` (page → panel → dialogue; Enter on an empty line goes back to a panel); input rules for "PAGE ", "PANEL ", "CAPTION: ", "SFX: " and "NAME: "; Alt+1 to Alt+5.
- **Typesetting**: `Design::comicScript` (preset "Comic script": Courier Prime, OFL, 12/14.4 on 8.5 × 11 in, 1 in margins, ragged, no hyphenation). `comicBlocks` numbers pages (spelled, left/right, panel count), panels and balloons (SFX unnumbered), and a page block has `breakBefore`, which starts a new page.
- **Bible by book**: `booksInReadingOrder` (each series in reading order, then books in no series); each entry's first book from `appearancesIn`, cached until `changesMade()` moves. Places nest under the place they're "Located in"/"Part of" (or that "Contains"/"Includes" them), depth-first, in their outermost place's book. In Characters, each book's entries are split by gender (listed order, others, Not set) with a sign and colour.
- **Races and Realms** are entry kinds; the race and realm facets list the bible's own first, and choosing a listed one it lacks creates the entry.
- **Link buttons** (cjui `linkText`): a framed button 7 DIPs around the label's text, so callers that measure the label still place it.

## The NorthRay look (0.27.0)

The Studio uses `cjui::northray::palette` (cool neutrals, aurora-green ribbon) instead of its own warm paper and ink blue; the top bar is the night sky with the aurora band (`northray::nightSky`, `auroraBand`, caption buttons on night), with the app mark (`web/studio-mark.png`, from `resources/writing-studio.svg`) and "NorthRay / WRITING STUDIO" like the Launcher. The editor's CSS variables follow the same palette. The icon (`writing-studio.svg/.ico`, `shared/metadata/icons/writing-studio.png`) shares Browser's and Files' sky, aurora and north star, with an open book and a ribbon on the horizon; the installer art matches Files'.

## 0.27.1

- The hint card's fix buttons call `styleApi.replaceWord` (the object passed to `initStyleUi` is now named, so its own methods can reach each other; before, `api` was undefined and every fix silently failed). Checked in Chromium by clicking every kind of fix.
- `attachedTo(block, from, to)`: a verb is a dialogue tag only when it's within three words after a closing quote whose line ends in , ? or ! or right before an opening quote with a comma between. Fancy tags and tag adverbs use it; fancy tags are then marked for action tags, a third use of the same one, or 6+ at over twice the plain tags.
- Creatures nest by "Kind of" / "Has kind" (`parentPlace` covers places and creatures; `isUnder`); the creature panel has "A kind of"; the sidebar lists top-level kinds with everything under them (`wb_.facet` = "under=<id>").

## 0.27.2

- Action tags (`ACTION_TAGS`) are marked only when `commaJoined` (the speech ends with a comma before the tag, or the tag comes first): after ? ! or . they read as beats.
- Members (`addMembers`): groups and races list "Member of" / "Has member" links as cards; the menu ticks characters and creatures in and out and stays open for the next. Those links aren't repeated under Relationships.
- Characters: `wb_.groupBy` (gender, race, none); creatures split by their top-level "Kind of" ancestor ("Other creatures" for those in no family).

## 0.28.0

- Review copies (`libs/studiocore/src/reviewcopy.cpp`, `review.hpp`): `makeReviewCopy` fills `web/review-copy.html` ({{LANG}}, {{TITLE}}, {{AUTHOR}}, {{DATA}} inside a JSON script tag, with `</` escaped). Each chapter is sent with its node id and its HTML from `ebook::documentHtml`, and pictures up to 3 MB are embedded as data URIs. The reader's page keeps its comments in localStorage (`nr-review-<copy>`). "Send my comments" downloads a `.nrcomments` file (`format: "nrcomments"`), which holds each comment's node, quote, prefix, suffix and text.
- `importReviewComments` checks that the file belongs to this book. `markQuote` finds each quote in the chapter's ProseMirror JSON, ignoring differences in whitespace and preferring the copy whose surrounding text best matches the prefix and suffix. It splits the text nodes and adds a `comment` mark. The comment reads "Reader: text". A comment on a whole chapter, or on words that have since changed, has no mark and is listed with the chapter. Review › Comments has "Make a review copy…" and "Import a reader's comments…" (`studio_comments.cpp`).
- `cjui::messageBox` (`libs/cjui/src/dialog.cpp`): a themed modal with the aurora band, an icon, primary and secondary buttons and keyboard support, using `menuPalette()`. It falls back to `MessageBoxW` if Direct2D isn't available. Every app uses it in place of `MessageBoxW`.
- Top bars: inactive tabs are drawn in `kNightInk` at 0.8 alpha. The Browser side-tabs title uses BodyStrong at full ink.
- Apostrophes: `searchNames`, the spelling extra words (`setExtraWords` normalises ’ ‘ to ') and the editor's name words accept both ' and ’.
- Editor: `deleteWord(±1)` is bound to Mod-Backspace and Mod-Delete; width `fit` is `calc(100% - 48px)`; `hyphens: manual` with `overflow-wrap: break-word`. Enter is `chainCommands(comicEnter, splitListItem)`. A duplicate "Enter" key had dropped the list command.

## 0.29.0: EPUB styling and covers

- **Ebook style** (`ebook::Style`, `ebook::styleFrom(Design)`): chapter openings ("number": `h1.numbered` holding `span.chapnum` "Chapter One" and `span.chaptitle`; "numeral": `span.numeral`), numbered like the typesetter: `typeset::chapterTitleParts` and `spelledNumber` are now public. Prologues, other special titles and front and back matter are not numbered. `markOpening` wraps a chapter's first paragraph: `span.dropcap` (the first letter, with any opening quotation marks) and `span.leadin` (the first three words). `stylesheet(style)` adds the indent or spaced paragraphs, alignment, hyphenation, the scene break as a CSS string (spaces widened to em spaces) and, when a typeface is embedded, `body { font-family }`. The preview and the EPUB build from the same style.
- **EPUB** (`EpubOptions::style`, `fontsFolder`, `fontId`): the book's family goes in `OEBPS/fonts/` with `@font-face` rules ("NR Book") and its `OFL.txt` (a `text/plain` foreign resource). `fontMayBeEmbedded` reads OS/2 `fsType` and refuses restricted or bitmap-only fonts, with a note. The copyright page says "Set in …". The cover page is an SVG of the picture's own size (`viewBox`, `preserveAspectRatio="xMidYMid meet"`, `properties="svg"`), so it fills any screen. Scene breaks use the design's symbol.
- **Covers** (`studio/cover.hpp`, `src/cover.cpp`): `cover::render(Spec, fontsFolder, w, h)` draws a cover at 1600 × 2560. Text is shaped with HarfBuzz and rasterised with FreeType through the typesetter's `Fonts` (1 pt = 1 px). Pictures are scaled to fill and focused by `focusY`. The output is written as PNG with miniz. There are four layouts (classic, picture, band, minimal) and seven palettes. `fit` finds the largest size and fewest balanced lines for the title. `toJson` and `fromJson` keep the design in `setting:cover-design:<book>`, with the picture as a blob id.
- **App** (`studio_cover.cpp`): the cover designer replaces the Publishing page while open, with the words in the right-hand panel (`Focus::Cover`, reusing FName, FSubtitle, FAuthor and FDescription). The preview is rendered at 500 × 800 to a numbered file in the state folder. "Use as the book's cover" renders the full size, stores it as a blob and sets `coverBlob`. Ebook editions have a "Cover and look" step: the cover, Design / Use a picture, and two choices stored in `setting:ebook-look:<edition>` (`font`: book or reader; `spaced`). `ebookStyleFor` combines these with the book's Typesetting design.

## 0.31.0: tidying the world bible, creatures by level

- `Library::findEntryNamed` compares names by `nameKey`: lower case, curly apostrophes made straight, spaces squeezed.
- `mergeEntries(keep, other)` moves `other`'s name and other names, its summary, details and notes, and its picture (if `keep` has none) into `keep`. Facet lines are kept once per label. Its links are re-pointed, with no self-links and no duplicates, and so are nodes' `pov_id`. It all happens in one transaction; `other` goes to the Trash.
- `tidyBible(world)` moves entries kept with the world's books into the world, then merges copies until none are left: the same `nameKey` and kind, or a character and a creature of one name (kept as the character, which keeps the creature's "Kind of"). It runs after `moveBookToWorld` and `addBookToSeries`, and the app runs it for every world when a library opens.
- `kindSuggestions` comes from a facet line in another entry ("Race: X"), a "Kind of" link pointing at a character, and the endings of people's names (-ians, -ites, -folk...). Choices are dismissed per world in `setting:kind-ok:<world>`.
- Review's "Add to the bible" refuses a name that is already in the bible.
- Creatures (`drawCreatureTree`): with `wb_.facet` empty, the species are shown as cards; with "under=<id>", the kinds under that entry, then the characters linked to it with "Kind of" ("named"). A path leads back up. Cards are `wb:tree:<id>` drag sources (`DragState::Entry`) and `Drop::KindOf` targets; there's a "species of its own" zone while dragging. `setKindOf` refuses to create a loop. A character's page has "A creature?" for choosing its kind.
- The gender and sex signs are drawn in the Title font on character cards and group headings.

## 0.32.0: entry pages, notes per book, galleries

- One column, `entries.page` (TEXT, JSON, default `'{}'`), added through the "adds" list, so `kSchemaVersion` stays 8 and older builds still open the library (they keep the column). It holds `sections` (`[[title, text], ...]`), `gallery` (`[{blob, caption}]`) and `bookNotes` (`{bookId: text}`), and it is written by `detail::entryPageText` and read by `detail::readEntryPage`. The export JSON includes it as `page`.
- `Entry::section`, `setSection`, `bookNote` and `setBookNote` (an empty note is removed).
- Templates: `defaultEntryTemplate(kind)` is built in. A world's own template is the setting `entry-template:<world>:<kind>`, one section per line; empty means the built-in one, and a single space means none. `entryTemplate`, `setEntryTemplate` (writing the built-in one clears the setting) and `entrySections(e)` (template order, then any extra written sections).
- `mergeEntries` joins sections and book notes with `joinText`, and adds gallery pictures it doesn't already have.
- The app's entry page shows written sections, a "Still to write" row of chips (each picks `wb_.section`), notes per book in reading order, the gallery (`wb_.picture` selects; the panel has its caption, "Use as the main picture" and "Remove"), and the image boards whose JSON has `"entry": <id>`. Those boards are left out of the sidebar, their back link goes to the entry, and the board panel's "Kept in" moves a board between the sidebar and an entry.
- Panel fields `FSection`, `FBookNote`, `FTemplate` and `isMultiField()`. `focusFieldSoon_` scrolls the panel to a field and focuses it once it's placed.

## 0.32.1: only the author's races

- The "race" facet has no built-in options. `facetChoices` offers the bible's Race entries and race values already written on characters. The panel's Race list ends with "New race…", which creates a Race entry called "New race", opens it, and focuses the Name field with its text selected (`focusFieldSoon_ = FName`).
- The studiocore family tree (`familyOf`, `familyLayout`, `families`, in `family.cpp`) is in, with tests; the app doesn't draw it yet.

## 0.33.0: family trees

- `Library::familyOf(id)` reads "Parent of" and "Child of" (either direction), "Partner of" and "Married to", and "Sibling of". Siblings also include anyone sharing a parent; `familyOf(id, false)` leaves that out, so a parent loop can't recurse.
- `familyLayout(root, limit)`:
  - A breadth-first walk gives each person a generation (partners and siblings 0, parents -1, children +1), up to `limit` people (`cut` says some were left out).
  - Units are a person plus their partners in the same generation.
  - Generations are placed from the eldest down: each unit sorted by the mean x of its members' parents, then pushed right to avoid overlap (0.25 gaps between units).
  - Two passes, from the youngest generation up, move parent units towards the middle of their children, within their neighbours.
  - x is in card widths. `families(world)` returns the connected families of two or more people, named after the most common surname (the last word) and rooted at the eldest; largest first.
- App:
  - The Family section on a character's page (`drawFamilySection`). `addFamilyMember` offers a new character (created, linked, opened with `focusFieldSoon_ = FName`) or an existing one.
  - `wb_.familyRoot` shows `drawFamilyTree`: cards of 176 × 66 scaled to fit, down to 0.66, and wider than that `wb_.treeX` is moved with Shift and the wheel.
  - Partners have one line between them, or two for "Married to". Children are grouped by parent set: a line down from a couple's midpoint (or below a single parent), a bar across, and a line down to each child.
  - A "Family trees" chip row sits on the Characters list.

## 0.34.0: the world bible's last round (names as you write, maps, checks, glossary, magic, spoilers, sharing)

- **Names as you write.**
  - `WebPane::setContextItems` adds NorthRay's items to the editor's WebView2 right-click menu (`ICoreWebView2_11::add_ContextMenuRequested`). The Studio now builds against `libs/third_party/webview2-sdk` (`WebView2MinGW.h` supplies the interface ids).
  - On a right-button mousedown the editor posts `contextWord`: the selection, or the word under the pointer widened to neighbouring capitalised words. Choosing the menu item sets a timer, and `addNameToBible` then asks for the kind.
  - Writing's panel lists `review::nameChecks` Unknown names found in the open chapter (worked out again at most every 4 s), with "Add" and "Not a name" (`names-ok:<book>`, shared with Review).
- **Maps.** `Entry::mapBlob` and `pins` (`{entry, x, y}` from 0 to 1) are kept in the page JSON as `map`. `drawMapSection`: while `wb_.pinning` is set, a click on the map puts the pin at the pointer (`mouseX/mouseY`). `pinMenu` opens, moves or removes a pin. A Maps row of cards sits on the Places and Realms lists.
- **Checks.** `review::continuityChecks(lib, world, book)` (`continuity.cpp`):
  - Detail lines with the same label and different values.
  - Sentences naming exactly one character, compared against that character's Eyes, Hair and Age: colour words near "eyes" or "hair" (grey and gray, gold and golden counted as the same), and "N years old", "N-year-old" or "aged N".
  - Unused entries: no appearance in any book's `appearancesIn`.
  - The Checks page also lists Variant names from `nameChecks` for every book. Dismissals are kept in `checks-ok:<scope>` as `spell|`, `fact|<key>` and `unused|<id>`.
- **Glossary.** `Entry::pronunciation` (page JSON `say`; panel field FAlt, "How it's said") is shown under the name. The Glossary page sorts names and other names, leaving out "the ".
- **Magic and powers.** The new kind `magic` (colour #B3439E / #E37FD0) has its own template and relations. Lore's built-in template is now What it is, Details, History.
- **Spoilers.** `Entry::revealed` maps keys to a book: "entry", "summary", "detail:<label>" or "section:<title>" (page JSON `revealed`). The panel's Spoilers section sets them, and badges show on the page.
- **Sharing** (`studio/bible_export.hpp`).
  - `View` filters a bible: every entry, or for a reader's guide (`readersGuide` and `upTo`) only entries and facts revealed by that book. A reader's guide also leaves out notes and notes per book.
  - `exportWebsite` writes index.html, glossary.html, an `e-<id>.html` page per entry, style.css (light and dark) and images/ (the type is sniffed from the bytes). Names are linked with `findNames`, and map pins are positioned by percentage.
  - `printableSource` builds a `typeset::Source`: a Part per kind, a chapter per entry (matter 2, so it's headed by the name with no number, built with `doc::Builder`), and a Glossary at the back. The app typesets it with the first preset (`chapterSinkLines` at most 3) and writes the PDF.
  - `pictureSize` reads PNG, GIF, JPEG and WebP headers.

## 0.34.1: searchable, scrolling pickers

Choosing who a relationship (or a family member) is with used a plain pop-up list that could run off the screen.
cjui's `showMenu` (and so `trackMenu`) now gives any top-level menu with 13 or more choices a type-to-search field
(letters filter by label, Backspace and Esc clear) and caps it at 14 rows, scrolling by wheel, arrow keys and Page Up/Down,
with small arrows at the edges instead of scroll bars. Every long pop-up list in the suite gets this.

## 0.34.2: question boxes at display scaling; names beside typographer's punctuation

- `cjui::messageBox` sized its window for the display's DPI but drew at 96 DPI (no `Gfx::setDpi`), so above 100%
  scaling the buttons were drawn smaller than, and away from, their click areas: only the keyboard worked. Fixed for
  every app (`d.gfx.setDpi(dpi)` before layout); checked in Wine at 144 DPI.
- Whole-word name matching (`studio/words.hpp`, used by `review::replaceWholeWord` and the mention counts in
  `entries.cpp`) decodes the UTF-8 character on each side instead of treating every non-ASCII byte as a letter, so
  curly quotes, apostrophes (possessives), dashes, ellipses and non-breaking spaces end a name; letters in any script
  still don't. Tested with “Marren, wait!”, Marren’s, Marren—, …Marren…, and not Marrenson, ÉMarren or Marrené.

## Sharing the book being written with NorthRay Capture (0.34.4, 16 Nov 2026)

- summary.json (beside the settings) also gives "book", "series" and "cover" for the book of the chapter in the
  editor: its title, its series line (`seriesLine`, shared with the cover designer: "Book Three of The Alvora
  Chronicles" when it's numbered in a series) and its cover's library file. NorthRay Capture's Book card shows them.
  Nothing leaves the PC.

## 0.35.0: polish (18 Nov 2026)

- **Editor** (`editor/src/editor.js`, bundled to `web/editor.js`):
  - A gliding cursor: `#caret` inside the article, placed from `coordsAtPos` on every selection change, focus, resize
    and view change, with a short transform transition and a breathing animation; `.smooth-caret` hides Windows' caret
    only while it shows (not while selecting, during IME composition, or read-only). Reduced motion turns both off.
  - Kept in the editor's own storage (localStorage; only conveniences): each chapter's scroll (`nr.scroll`, put back on
    load), and earlier versions for undo after reopening (`nr.undo.<chapter>`: the text as opened, then at most every
    90 s while writing and on leaving it; 12 kept, at most about 700 KB per chapter, least recently used chapters let go
    when storage is full). `makeState` replays them oldest first as whole-document replacements, each its own undo step
    (`closeHistory`), so Ctrl+Z steps back through them.
  - "Saved just now / a moment ago / N minutes ago / at 14:05", kept current every 10 s.
  - Style hints are worked out again only after typing pauses for 1.4 s (was 0.35 s).
  - Warm night paper (`paper: "night"` in the view settings, Aa menu): its own palette in either theme.
  - Review's "show it" (`selectComment`) centres the words and lights them up for 2 s (`cm-flash`), resolved or not.
  - Ctrl+K and Ctrl+/ in the text go to the app (`appCommand`); Link is now Ctrl+Alt+K.
- **App**:
  - Ctrl+K (`quickJump`): a searchable cjui menu of the book's chapters and scenes (indented, with words), its bible
    entries (with kinds) and the other books; choosing one opens it in Writing or Worldbuilding.
  - Ctrl+/ (`showShortcuts`): a GDI popup with the shortcuts in three columns, from `shortcutGroups()`, the same list
    the Settings page now draws.
  - The goal bar: today's words against the daily goal, 3 px along the foot of the top bar while writing (gold when
    reached, a tooltip with the numbers).
  - Drop lines (`drawDropLine`, `drawDropInto`): a rounded line with a ring at its start and a soft glow, or the row
    tinted and outlined for "inside"; outline, corkboard and library.
  - Bible cards: 100 px portraits (were 72); the cards are taller.
  - Family tree: Ctrl + wheel zooms about the pointer (`wb_.treeZoom`, 0.45 to 1.8; 0 = fit), and dragging the
    background pans it (`wb_.treePan`).
  - Planning: cards get a band and a wash in their viewpoint character's colour (`povColour`, ten colours chosen by a
    hash of the entry's id); the timeline's lanes show the same colour.
  - The text's right-click menu loses the browser's own items (back, reload, print, save, share, inspect, translate,
    read aloud and the like) and doubled separators.
- **Library**: `appearancesIn` keeps each chapter's result, read again only when its text's length, word count or
  modified time changes or the bible's names do (`appearanceCache_`), so Planning opens a long book without reading
  every chapter each time. Tested in `studiocore_tests` (994 checks).
- Already there: the selection's word count while selecting, focus mode dimming other paragraphs, word counts in the
  outline, and reopening the last book and chapter.
- Tested: the editor in Chromium with Playwright (cursor, scroll per chapter, saved text, night paper, undo after a
  reload); the app in Wine (Ctrl+K and choosing, the shortcuts sheet, the family tree's zoom and pan). Needs the real
  editor on Windows: the right-click menu, and the editor's keys reaching the app.


## 0.52.0: the timeline (20 Nov 2026)

- Core (`planning.hpp`): `nodePlaces`/`setNodePlace` (setting `nodeplaces:<book>`, node -> place or realm entry);
  `timelineEvents`/`setTimelineEvent` (`timeline:<book>`'s new `events` object, entry -> {day}). Outlines read and write
  `Place:` (also Where/Setting/Location), matched to places and realms by name or other name; unknown ones reported.
- App (`drawPlanTimeline`): a toolbar with Lanes (`planPref` "lanes": viewpoint, thread, place, one) and zoom (- Fit +;
  Ctrl+wheel through `zoomTimeline`, keeping the day under the pointer still; `pl_.tlZoom` pixels per unit, 0 to fit).
  Tick steps 1-2-5. With thread lanes a card is drawn in each of its threads' lanes (the reading line uses its first).
  A strip of Story events (the bible's `event` entries placed for this book) with dashed lines through the lanes; a
  flag's menu opens the entry or takes it off. Cards and events not placed wait below as chips.
- Dragging (`planMouseDown/Move/Up`, `pl_.tlDrag.kind`): 0 a card along (whole units from where it started), 1 a card
  chip onto the timeline (day from the pointer, `tlX0`/`tlDay0`; dropped only inside `tlDropArea`, shown with a guide
  and a day pill), 2 an event along, 3 an event chip onto it. The panel's "Where it happens" (`choosePlace`).

## 0.51.0: the corkboard (20 Nov 2026)

- Board preferences in `plan:<book>` beside the target (`planPref`/`setPlanPref`): `layout` rows|columns, `cardSize`
  small|medium|large (190/232/300 wide; Ctrl+wheel), `colourBy` viewpoint|thread|status, as segmented controls under the
  header. `drawPlanBoard` draws every card with one `drawCard` lambda; heights follow the size, and the who line, tags
  and length are placed from the card's foot.
- Editing on the card: `pl_.cardEdit` (FName or FSynopsis) places that field over the title or synopsis of the selected
  card (loading the fields first), focuses it once, and ends when focus leaves it (Enter or Esc, or a click elsewhere),
  committing. `panelField` leaves a field being edited on a card unplaced ("Being edited on the card"). The status word
  and the viewpoint line open menus (`chooseViewpoint`, shared with the panel). The card's parts are hit ids
  `pl:card:<id>|title` etc., so dragging from them still moves the card.
- Columns: a column per part (and one before the first part), scrolled with Shift+wheel (`pl_.boardX`, `boardW`).
  Cards drop above or below each other (`horizontal = false`); a part's head and the rest of its column are `Drop`s with
  the new `into` flag (always inside). The drag indicator follows the drop's orientation.

## 0.50.0: Plot threads, structures and planning as text (20 Nov 2026)

- New portable module `studio/planning.hpp` (src/planning.cpp, tested in `testPlanning`): story structures (eleven built in,
  plus the author's own per world or independent book in setting `structures:world:<id>` / `structures:book:<id>`), the
  book's structure (`structure:<book>`: a beat now holds a list of chapters, the old single id is still read, and `notes`
  per beat), plot threads (`threads:<book>`: threads with id, name, colour, about; and node -> thread ids), and text
  formats for all three with `importPlanning` / `planningText` (one text: "Structure:" blocks, "Thread:" blocks and the
  outline with "Book structure:", "Part:/Chapter:/Scene:" or Markdown headings and the fields Viewpoint, Status, Target,
  Threads, Beat, When). Structures import first, then the outline, then threads, so chapter numbers count new chapters.
- The app's old in-file templates and `structureOf`/`setStructure` are gone; studio_plot.cpp uses the module. New focus
  kinds Thread, Beat and Structure (contextSeries = the book) load and save through `loadPlanFields`/`commitPlanFields`;
  your own structure is edited as text in the panel (FTemplate).
- `nodeShown` applies the status, viewpoint and thread filters on the corkboard, the timeline and the structure bar.
- Cards grow a row of thread tags when the book has threads. Before there's text, chapters without a target count as
  long as the average target on the structure bar.

## 0.49.2: Planning (20 Nov 2026)

- `selectCard`: a second click on the selected card within 450 ms opens it in Writing.
- Structure: the length bar draws "N  Title" where a segment is over 90 px (number only over 26 px) with a tooltip;
  each beat's chapter is a field-like box (surface, rule, drawn chevron, "N · Title" or a faint "Choose a chapter...")
  opening the same menu. Checked in Wine.

## 0.49.1: sprints and targets in the outline (20 Nov 2026)

- Editor: `sprint` {end, minutes, done, base}; `#count` is a button opening `#sprintmenu` (10/15/25/45 minutes, or
  stop); a 1 s tick redraws "mm:ss · +N words"; words are the chapter's now less its words when the sprint reached it,
  plus `done` from earlier chapters (kept when `load` brings another, `base` reset after the new state). The end (or
  stopping) gives a `notice`. Nothing is stored. Tested in the harness across two chapters (+5, then +8).
- Outline (`drawBinder`): a 2 px line under a chapter or scene with `targetWords`, filled towards it (gold at 100%),
  with a tooltip of the numbers. Checked in Wine.

## 0.49.0: writing, calmer (20 Nov 2026)

CJ: too many underlines, typing feel, moving around, the page look. Checked with tools/editor-harness.cjs.

- `writingView` v3: hints default "quiet"; settings below v3 move from "full" to "quiet" once (sent back so the app
  keeps it). `hereLook` puts a node decoration `here` on the top-level block holding the selection; CSS hides style
  hints (not spelling) inside it, so nothing appears under the cursor while typing. Hints are 1 px.
- The load message carries `chapters` (every node of the book in manuscript order with title, kind, depth and words);
  `#chapname` is a button opening `#chapmenu`, which sends `openNode` (the app checks the node is the book's and
  selects it).
- `spacing` (close 1.45, normal 1.65, open 1.9, double 2.2) as `--line`, in Aa.
- `savedText` keeps the last "Saved ..." while a save is pending (only "Saving..." before the first).

## 0.48.5: nested places on the cards, into Writing, an editor harness (20 Nov 2026)

- Cards (Places, and Everything) with nothing found and no type filter: only places whose direct place parent
  (`placePath(...).back()`, a place) is the one looked into (`wb_.facet = "under=<id>"`, roots when none) are shown;
  `wb_.placeKids` counts children for an "N inside ›" button on each card (detail chips leave it room); a path (All
  places › ... › here, and "Open X's page") heads the cards when inside one; `newEntry("place")` while inside adds
  "Located in" it.
- `openLastWriting()`: the Writing tab or Ctrl+W with no book chosen opens `settings_.lastBook`, else the most recently
  modified live book.
- `tools/editor-harness.cjs`: Writing's editor in Chromium with a stand-in WebView2 host (`window.chrome.webview` with
  postMessage recorded and messages delivered), sending `view` (names, book font) and `load` (a chapter from the paste
  fixtures), then screenshots: the page, typing, the Aa menu and the hints list. The native parts of Writing are
  checked in Wine, where WebView2 isn't available.
- The style select is 140 px wide.

## 0.48.4: entry pages (20 Nov 2026)

- `wb_.anchors`: each section of an entry page records where it starts (content y) as it's drawn (Family, In this
  place, Map, Gallery, Members, Relationships, Notes, Appears in); the next frame draws an "On this page" row of links
  under the details (when three or more) that scroll there.
- Relationships: a character's family links to other characters (Parent of ... Foster child of) are left out (they're
  under Family, where the chip menu changes or removes them); the rest sorted by how they read from this entry, then
  by name, the label shown once per group with a rule between groups. Tested in Wine on Ada Wren.

## 0.48.3: the places outline (20 Nov 2026)

- Places' cards view has Cards / Outline (`wb_.placesOutline`). `drawPlacesOutline`: places and realms by their
  `placePath` parent, realms first then A to Z, rows indented 26 px a level with a drawn chevron (`wb_.collapsed`), the
  type facet and the count inside. Rows are `wb:tree:` drag sources and `Drop::KindOf` targets; while dragging, a strip
  at the foot takes the place out of everything. `setKindOf` hands places and realms to `setPlaceIn`, which refuses a
  loop (`placePath` of the new parent), removes the place's up-links (Located in / Part of, and Contains / Includes to
  it) to places and realms, and makes "Located in". Tested in Wine (Akunda dragged into Highcliff and back out).

## 0.48.2: the same on the website (20 Nov 2026)

- `familyLines(lib, view, entry)` (bible_export) builds the labelled lines once; the PDF's Family and the website's
  family box both use it (the website's names are links). Tested on Tom's and Ralph's pages.

## 0.48.1: family by kind in the bible's PDF (20 Nov 2026)

- `printableSource`: a character's Family lists each relative under what they are (from `kinWord` and the gender
  line): Mother / Father / Parent, Wife / Husband / Spouse for a "Married to" link and Partner otherwise, Twin sister,
  Half-brother, Stepson, Adoptive mother, Foster child...; relatives of one kind share a line with a plural label
  (Daughters, Wives, Children). Tested (Tom: Twin sister, Half-sister, Adoptive sister, Father; Ralph: Wives, Stepson,
  Adopted daughter; Lillia: Adoptive father and mother).

## 0.48.0: places and maps (20 Nov 2026)

- **Map viewer** (`studio_world.cpp`: `openMap`, `drawMapView`, `mapMouseDown/Move/Up`, `mapWheel`; state in `wb_.map*`):
  the map fitted to a board under a header (back, "Up to the map of ..." for a place pinned on another map, + Pin,
  Replace), `mapZoom` 1-8 about `mapCX/mapCY` (the map's point at the board's centre, clamped so a zoomed map covers
  the board); the wheel zooms keeping the pointer's point still; dragging the background pans; a pin pressed and moved
  is dragged (saved on release), pressed and released opens `pinMenu`; while pinning, a click places it. Labels are
  placed greedily without overlap (the hovered or dragged one first, always shown); a ring marks a pinned entry with
  its own map. The page's map, its "Open the map" link and the Places list's map cards open it.
- `pinMenu` offers "Open the map of ..." for entries with a map; `pinOnMenu` (the pin chooser) is shared.
- **Place pages**: `placePath` (Located in / Part of / Contains / Includes up through places and realms; Kind of for
  creatures) drawn as links beside the back link; `drawPlaceSection` ("In this place": places inside, Lives in /
  Home of, Born in / Birthplace of, Based in / Base of, Took place in / Scene of; + Place inside makes a place Located
  in this one).
- Tested in Wine: the Highcliff map opened from its card and page, zoomed with the wheel, panned, a pin dragged and
  kept, Fit; a place page's section and a new place inside it with its path.

## 0.47.0: one tree per family, how people joined, replace-all import, the bible site (20 Nov 2026)

From CJ's own Alvora bible PDF: the Smiths shown twice (from Iris through Violet, and from Maria), the Crystallines
twice (from Mah'los, Zah'ra's brother, and from Kion), and partners labelled "married in".

- `families()`: every possible founder (no parents; children or brothers and sisters; not married in) gets a
  bloodline layout; candidates are taken biggest first and one whose own people are more than half in a family already
  taken (or whose founder is) is dropped. Bloodline founders now include each parentless founder's parentless brothers
  and sisters.
- `FamilyLayout::Person::joined` for in-laws: "married" (a "Married to" link to one of the family), "partner", or
  "parent"; the app's cards say Married in / Partner / Other parent, the picture married in / partner / other parent,
  and its key "from another family".
- `paste::clearEntryText` (other names, pronunciation, summary, details, sections, book notes, notes, spoilers, every
  link; name, kind, picture, gallery and map kept) and `importPages(..., replaceAll)`, which clears the matched entries
  before applying any page. App: a choice menu before the import's confirmation when pages match entries, and "Replace
  all its text with what you've copied" (`pasteFromClipboard(true)` -> `pasteEntryPage(..., replaceAll)`); undo as
  before (the entry and links as they were).
- Bible website: `kind-<kind>.html` per kind (A to Z; characters grouped by their gender line when most have one), the
  nav and crumbs pointing there; the home page shows 8 per kind (pictured or summarised first) and "All N ...";
  `kinWord` in the family box; `Site::treeOf` (person -> their family's tree anchor and title, in-laws to their
  partner's) for a "see the tree" link; families.html with chips, anchors and trees at their own width in a
  scrolling frame; the entry's name block (`header.ehead`) above the layout; the phone nav one sliding row.
- Typesetting: a picture that doesn't fit the room left on a page but would at 70% or more is shrunk to stay (a family
  tree under its chapter heading).
- `studio_bible_dump <repo> <out>` (tests/bible_dump.cpp): CJ's paste fixtures plus a family, places and a language,
  shared as a site in every theme and a PDF, for looking over.
- Tested in core (1442 checks: Smith and Crystalline shapes give one family each, partners and marriages labelled,
  replace-all import clearing text and links but keeping the picture, site page counts with kind pages).

## 0.46.0: kinds of family, more typefaces, tidier cards (20 Nov 2026)

CJ: the world bible stays the focus; sibling kinds in the tree and relationships; every part of the bible must import
from the templates; more built-in fonts.

- **Relationships** (`entries.cpp`): "Twin of", "Half-sibling of", "Step-sibling of", "Adoptive sibling of", "Foster
  sibling of" (each its own reverse), and "Adoptive parent of"/"Adopted child of", "Step-parent of"/"Stepchild of",
  "Foster parent of"/"Foster child of"; offered for characters and in the bible export's family box. `relationFor` maps
  the wordings (twin sister, half-brothers, stepmother, stepson, adoptive father, adopted daughter, foster brother...)
  and no longer strips "adoptive"/"step" as prefixes. `blankPage("character")` has a Family line per kind.
- **Family** (`family.cpp`): `Family::how` per relative: "" (birth / full), "adoptive", "step", "foster", "twin",
  "half". A linked sibling takes its link's kind; one found through a parent is half when the birth parents differ, or
  the shared parent's kind; the closest kind wins across parents. `FamilyLayout::parentHow` per (parent, child) and
  `siblings` ({a, b, how, sharedParents}) for twins and brothers and sisters joined only by their own link. Units with
  a person holding two or more partners put them in the middle; twins' units are moved side by side. Bloodline
  founders are followed from founder to founder (partners and other parents with no parents), and `families()` skips a
  founder whose children are already in a family, so a man with two wives gives one family. `FamilyGroup::name` tells
  same-surname families apart by the founder's first name ("The Smith family (Hal's)"); `familytree::title` uses it.
- **Drawing** (app and `family_tree.cpp`): a child hangs from their main parents (birth, else adoptive, foster,
  step); other parents get a dotted elbow line; adopted/step/foster lines are dotted with a small label; twins come
  down from one point (the line runs to that point); unparented sibling pairs get a bracket labelled by kind; each set
  of parents over a row has its line at its own height (`busShift`); key: "adopted, step or foster" (the picture adds
  it only when used, and is at least 700 pt wide with a key).
- **App**: `kinWord` ("twin sister", "stepfather", "adopted son") on the Family section's chips and the tree panel;
  `familyChipMenu` (open, change kind, remove the link: every family link between the two goes, then the chosen one
  is made); `addFamilyMember` asks the kind first.
- **Cards**: the cards view is up to 1,640 px wide (an entry's page keeps 980); columns of at least 290 px; summaries
  through `linesOf(gfx, text, width, font, lines)` (cut at a word with an ellipsis by DirectWrite's own measure, cached)
  under a clip, so they can't run under the detail chips on Windows. Tree card names use it too.
- **Fonts**: 14 more families in `apps/studio/fonts` from Google Fonts (via the @expo-google-fonts packages; OFL, Special
  Elite Apache 2.0; fsType 0): EB Garamond, Libre Baskerville, Lora, Alegreya, Spectral, Source Sans 3, IM Fell English,
  Cinzel, Uncial Antiqua, MedievalSharp, Caveat, Dancing Script, Special Elite, Noto Sans Runic, each with `name.txt`.
  They appear wherever the app's fonts are listed (Typesetting, covers, EPUB); the bible website's `fontDefs`/`fonts()`
  gain them (the @font-face family is the stack's first name). The editor's typeface list adds "Built in" (handwriting,
  typewriter, runes, old print, display) from the app folder on `fonts.nrstudio.localhost` (now in editor.html's
  font-src); words marked with them resolve through `familyFolder` across the app and world folders.
- Tested in core (1430 checks: kinds both ways, half/step worked out, layout kinds, twins adjacent, a filled blank page
  pasted making every kind, wordings, two Smith families named apart, the picture, the built-in families and a site
  with Cinzel and EB Garamond copied in) and in Wine (cards at full width, a sibling changed to Twin from its chip and
  drawn from one point, the founder's tree).

## 0.45.0: family trees reworked, and printed (20 Nov 2026)

- `studio/family_tree.hpp` (`studiocore`, portable, tested): `familytree::title` names a family ("The Yerasuun family" by the surname most of its people share, else "Tobias's family"; "…'s family tree" for one person's own tree). `familytree::render` draws `Library::familyLayout` to a PNG for paper: cards 196 × 80 (picture through `stbi_load_from_memory` or initials, a two-line name in Gentium Book Plus through FreeType, a vector gender sign and one detail), partner lines (double for a marriage), a dashed line joining co-parents who aren't partners, child buses, "married in" for in-laws, an optional heading and key, and `turnWide` turning a tree wider than 1.3 × its height a quarter for a book page.
- `bible_export`: `Options::treeFonts` and `treeFolder`. The printable PDF gets a "Family trees" part (one tree a page at scale 3, turned when wide) when family is included and it isn't a reader's guide (trees show later births); the website gets `families.html` and `images/familytree-<root>.png`, linked from the nav and home page.
- App: the tree sits on a sunken dotted board with a picked-person bar (name chip, + Parent / Partner / Child / Brother or sister), own-tree/whole-family links, Save as a picture… (`IFileSaveDialog`, scale 2), a key and a floating zoom cluster; the same card design as the printed tree; co-parents in a row are always joined.

## 0.44.0: the Library polished (20 Nov 2026)

- `bookStatusColour(BookStatus)` (a colour per status, light and dark) and `statusTag(x, y, status, maxW)`: a
  tinted pill with a dot. `wordsText(n)` ("12,340 words", "No words yet").
- **Shelf** (`bookCard`, rows now cover × 1.5 + 84): the series number, the title, the status tag, the word count,
  and a 3 px bar under the cover towards `bookTarget` (in the ribbon colour once reached).
- **List** (`bookRow`): Title (centred when there's no subtitle), Series (the name, "Book N" beneath, "and N more"
  for a book in several), Status (tag), Words, Changed. `seriesLine` reads "Name · Book N".
- **World and series pages**: `booksTotals` under the description or header: books, words, last changed.
- **Book panel**: an at-a-glance box (words, chapters, last changed; with a target, a bar and "N% of …").
- `panelImage`: an empty picture is a 48 px (wide) or 96 × 60 box reading "No picture yet".
- Tested in Wine: a world page with totals and the list, All books as list (series and numbers, words, tags) and
  shelf (tags, words, the target bar on Dragonfall), the book panel's box, the shorter empty picture boxes.


## 0.43.0: Worldbuilding polished (20 Nov 2026)

CJ asked for the same polish as Writing's bar for each part of the Studio, one at a time; Worldbuilding first.

- **Find in the bible**: a new field `FFind` (cue "Find in the bible: a name, a word"; Esc clears; not committed,
  pasted pages or cleared with the others), `find_`. The cards view filters on every typed word appearing in the
  name, other names, summary or details; while finding, the kind check, family trees, maps and the creature tree are
  hidden and an empty result says so. The box has a drawn magnifier and clear cross.
- **Cards** (250 px minimum, 118 tall): a 72 × 90 picture or a 64 px initials ring; the kind and facets (not gender,
  which is the drawn sign `drawGenderSign`: ring and cross, ring and arrow, or ring and stroke); the name; two lines of
  summary (wrapped, trimmed); up to three detail chips (label faint, value ink) from the first non-facet "Label: value"
  lines that fit.
- **Sidebar**: "ENTRIES" and "THE WORLD" headings; kinds with no entries drawn in the muted ink and without a count.
- **Kind suggestions**: a one-line note ("2 entries may be filed under the wrong kind. Check ›") until opened
  (`wb_.kindsOpen`), with Hide on the open card.
- **Overview panel** (no entry open): the world's name and count, a bar per kind in use (click to filter), the five
  most recently changed entries, and actions (new entry, Import many pages, `copyBlankPageFor(kind)` with the world's
  template, Share the bible). `copyBlankPage` and it share `copyBlankPageText`.
- **Entry panel**: "Undo the paste" stays at the top after a paste; Fill this page and Copy a blank page moved to a
  "PAGES FROM ELSEWHERE" section at the foot. "A creature? (a named one)" became "Kind of creature (for a named
  creature)" with "Not a creature".
- **Everywhere**: `panelChoice` draws a field-like box (surface, rule, hover) with the value left-aligned and a drawn
  chevron, instead of a centred button.
- Tested in Wine: the overview with the summary panel, Characters with the drawn signs and grouping, the find box
  narrowing to two entries and clearing, the kind note opening and hiding, an entry page with the new panel.


## 0.42.0: the writing bar, like a word processor's (20 Nov 2026)

CJ: the bar at the top when writing needed a lot of work, and the font choice smoothing out, based on Word.

- **editor.html**: `#bar` is a ribbon of `.grp` groups (`data-group`, `data-prio`, `data-label`), each a `.row` of tools
  and a `.cap` caption: Style (the paragraph-style select in `.select-wrap` with a drawn chevron), Font (the typeface box
  `#fontbtn.combo` with `#fontname`, B, I, U, Sc, and a new Link button), Paragraph (quote, bullets, numbers), Insert
  (scene break, picture), Review (comment, other words), Edit (undo, redo), View (Aa, and `#morebtn`); on the right
  Chapter (`#hints` as a pill, ‹ `#chapname` ›) and the status (`#count` over `#saved`). Icons are inline SVG drawn
  for the Studio (stroke `currentColor`), so they follow the theme; every tool has `data-label` and `data-key`.
- **editor.css**: the ribbon's look (30 px tools, 6 px radius, pressed tools in the accent tint), the styled select and
  combo, the pill, `#moremenu` and the typeface list (`.fm-item`, `.mm-head`, `.fm-add`), all from the palette
  variables (light, dark, warm night).
- **editor.js**: `fitBar()` (on a ResizeObserver) folds groups below priority 9 from the least used until
  `scrollWidth` fits; `buildMoreMenu()` lists the folded groups' tools by name, icon and shortcut, and runs the hidden
  tool's own mousedown. `selectionFont`/`updateFontBox` show the selection's typeface by name, in its font ("Several
  typefaces" when mixed, the book's typeface by name from the view message's `bookFont`). The typeface list: "This
  book" (the book's typeface), "This world's fonts" (each in its own font, or a note when there are none), and "Add
  fonts to this world…" (`openFonts` to the app, which opens Worldbuilding › Fonts). `setChapName` from load and title.
- **App**: `sendView` adds `bookFont` (the Typesetting design's family name); `openFonts` is handled in Writing.
- Tested in Chromium at 1200 and 760 px, light and dark: the groups and captions, folding into More and a tool run
  from it (a list made), the typeface box naming the selection's font, the list in each font, a font set from it.


## 0.41.0: words in a world's fonts (20 Nov 2026)

CJ: the world's fonts are for use within chapters, for runes, symbols or handwriting.

- **Document**: a `font` mark, `attrs.family` = a world family's id (`[a-z0-9-]{1,64}`, checked by the sanitiser;
  one per text run).
- **Typesetting**: `Span::font`; `Shaper` takes the font folders and opens a marked family once (`familyFor`), picking
  its style by bold and italic; a family that isn't there falls back to the book's and is reported in
  `Book::warnings`. The PDF embeds and subsets whatever faces the display list uses, so nothing else changed.
- **Ebook**: marked words become `<span class="ff ff-<id>">`; `ebook::fontsUsed(src)` lists the ids;
  `ebook::worldFontCss(folders, id, url)` writes an @font-face per style ("NR F <id>") and the class rule. The EPUB
  embeds each under `fonts/<id>/` with its licence when `fontMayBeEmbedded` allows (a note otherwise);
  `EpubOptions::fontsFolder` is now always set by the app. The ebook preview adds the same CSS with URLs from
  `fontUrl()` (the library's fonts host).
- **Editor**: the `font` mark (`span.ff.ff-<id>[data-font]`); the view message carries `fonts` (`editorFonts(book)`:
  id, name and each style's URL on `worldfonts.nrstudio.localhost`, now in editor.html's font-src); `setWorldFonts`
  loads them with the FontFace API and adds a class rule with CSSOM (the CSP keeps out inline styles, not these). The
  toolbar's Ff button (shown when the world has fonts) opens `#fontmenu`: "The book's own typeface" and each family,
  named in its own font; the selection gets the mark (or the stored mark, for what's typed next). `textBlocks` blanks
  marked words, so spelling and hints skip them; marked words have a faint dotted underline while writing.
- Tested in core (1317 checks: the mark kept and a bad id dropped, a page set with the world face in its fonts, the
  missing-font warning, the ebook span and CSS, an EPUB with only the words' font and its licence) and the editor in
  Chromium (the button and menu, the mark applied, the font loaded and used, spelling marks cleared). Inside the app
  the editor needs WebView2, so on Windows.


## 0.40.0: a world's own fonts (20 Nov 2026)

- **studiocore/fonts** (`fonts.hpp`, `fonts.cpp`): font folders are a list, "app fonts|world fonts" (`folders`,
  `joinFolders`, `familyFolder`: the first folder holding a family; ids with path separators refused). A world's own
  live in `<library>/fonts/world-<id>/` (`book-<id>` for an independent book: `scopeFolder`), one folder per family
  with `<Name>-<hash8>-<Style>.ttf` files (the hash keeps the process-wide font cache from serving an older file),
  `name.txt` and `OFL.txt` or `LICENSE.txt`. `inspect(bytes)` reads family (typographic name 16, else the face's),
  style (OS/2 weight 600+ is bold, fsSelection or the face flags for italic), variations (`fvar`), and refuses CFF
  (`OTTO`), WOFF, collections, files without `glyf`, and fsType restricted or bitmap-only. `importFiles` groups by
  family, keeps the weight nearest 400/700 when two files want one style (reporting the other), uses the first style as
  regular when a new family has none, replaces styles of a family already there, never takes a bundled id
  (`crimson-text-2`), and copies a licence beside the font. `familiesIn`, `renameFamily`, `removeFamily`, and
  `sample()` (a line rasterised with the typesetter's shaping into straight RGBA, for previews).
- Everything that sets type takes the list: `openFamily` and `fontFamilies` (name.txt names; duplicates by id once),
  the symbols fallback from the first folder (typesetting and covers), the EPUB's embedded typeface (with
  `LICENSE.txt` when there's no OFL), and the bible website, where `Look.headings/body` may be `family:<id>`: copied
  into the site as `"NR <id>"` with an @font-face per style.
- **App**: `fontFoldersFor(book)` (its world's, or its own when independent), `worldFontFolders(world, book)`;
  `typesetResources(book)`; Typesetting, Publishing (edition typesetting, EPUB, the ebook look), the cover designer and
  Share the bible use them. Worldbuilding › Fonts (`studio_fonts.cpp`, `Focus::Font`): Import fonts (multi-select file
  dialog), a card per family with its styles, licence, and samples (regular, italic, bold) drawn with
  `Gfx::drawPixels` from `fontSamples_` at twice the size; the panel renames (FName), imports more, or removes (listing
  books whose typesetting or cover uses it). The ebook preview serves the library's fonts from
  `worldfonts.nrstudio.localhost` (mapped beside the objects folder; ebook.html's CSP and ebook.js allow full URLs).
  Typesetting's typeface became a list (`side:ts:font`) instead of buttons; the cover's typeface list maps by id.
- Tested in core (1304 checks: inspecting styles and refusals, importing a family of four with its licence beside the
  bundled one, the lists, a cover and the bible website in it, samples, re-import, renaming, removing, two files for
  one style, an EPUB with a world font and a non-OFL licence) and in Wine (importing Gentium's four files and a CFF
  .otf through the dialog, the refusal note, renaming, the samples, the typeface in Typesetting's list and the book
  set in it).


## 0.39.0: many pages at once, templates for every kind (20 Nov 2026)

- **studiocore/entry_paste**: `kindNamed` (kind words and plurals: Characters, people, Bestiary, Magic systems...);
  `splitPages(text, defaultKind)` -> `Batch {kind, name, text}`: a page starts at a `Name:` line or a `# Title` line
  (a `Name:` straight after a title joins it); a kind heading (`# Characters`, `**Places**`, `== Creatures ==`,
  `Realms:`, PLACES) counts when a page follows it or it's a top-level `#` heading, so a page's own "## Magic" stays
  a section; `Kind: X` above or in a page sets that page's kind. With no `Name:` lines at all, headings carry the
  names: the shallowest heading level that isn't a kind or a part of a page is the pages' titles (deeper headings
  become the page's sections). Preambles and rule lines are left out. `looksLikeManyPages`, and
  `documentPagesText(sections)` for Word and OpenDocument files (the importer's sections with `doc::outlineText`,
  new: headings as Markdown, one level deeper than stored, list items as "- "; the importer's "Front matter"
  title dropped).
- `importPages(lib, world, book, pages, order, Import&)`: every page's entry found (name and kind, then name) or made
  first, then each page applied like a paste, so pages naming each other link. `Import` keeps the entries made, those
  updated with their earlier state and links, the entries made for other names, links and unchanged pages.
- Parser: a second "Details" heading is a section (Lore's template has a Details part and a Details section).
- **App** (`studio_paste.cpp`): "Import many pages…" at the foot of the Worldbuilding sidebar (a file: .txt, .md,
  .docx, .odt, UTF-8 or UTF-16; or the clipboard), and Ctrl+V on the cards with several pages. Pages with no kind get
  one asked for once; a confirmation counts pages by kind and those matching existing entries; a backup (and its
  copy) first; `wb_.importUndo` and Ctrl+Z restore updated entries with their links and trash what was made.
- The manual prints `blankPage` for every kind with a `Kind:` line; a test splits all ten joined together.
- Tested in core (1275 checks: every kind's blank page filled and read back, the joined templates, mixed pages with
  headings, Kind: lines, a repeated import changing nothing, Markdown and Word-style heading pages) and in Wine
  (four pages pasted on the cards, the confirmation, the Vale family appearing, Ctrl+Z taking it back). Word
  documents were checked through the importer with python-docx files in both styles.


## 0.38.3: family trees that keep families apart (20 Nov 2026)

- **Library** (`family.cpp`): `familyLayout(root, limit, bloodline)` returns each person with `inLaw`. A person's own
  tree is their ancestors straight up, their brothers and sisters, and the descendants of that generation; a whole
  family (`bloodline`) is its founders (the root, their brothers and sisters, and parentless partners or other parents
  of their children) and everyone descended from them. Partners and other parents who come from elsewhere are added
  as in-laws and never expanded, so two families joined by a marriage stay two trees. `families()` starts a family
  only from founders whose children's other parents have no parents of their own, names it by the surname most of
  its blood members share, and drops one-person groups. Tested in `studiocore_tests` (1164 checks).
- **App** (`studio_world.cpp`): `openFamilyTree(root, bloodline)`; the tree view has a toolbar (open the picked
  person's page, add a parent, partner, child or brother or sister to them, zoom out, fit, zoom in), a link between the
  person's tree and the whole family, in-law cards marked "Married in" with a link to their own family, the picked
  person's lines drawn in the ribbon colour, animated zoom and pan (`famtree:zoom`, `famtree:x`), click to pick and
  double-click (450 ms) to re-root. `drawFamilyPanel` shows the picked person's summary and close family as links.
  Keys while a tree is open: arrows look around, + and − zoom, 0 fits, Esc returns to the root's page.
- Tested in Wine: Alvora's two families shown apart, picking, double-click re-rooting, zoom buttons and the panel.


## 0.38.2: relationships that fill in, your own kinds of relationship, things made, a blank page (19 Nov 2026)

- `relationFor` strips "former", "ex-", "late", "step-" and the like, and knows more wordings (grandparents and other
  relatives as Family, crew of / part of / house / clan as Member of, instructor and tutor as Student of); a wording's
  relationship the kind doesn't offer falls back to the label itself ("Related to" for places).
- Finding a pasted name: by name or other name with "the" ignored, then a fuller form (an entry's whole name of two or
  more words among `nameParts` of the pasted one), never guessing between two.
- Names in Family and Relationships the bible lacks become entries of the relationship's first target kind (a person
  by default) and are linked; Race/Species and Realm/Plane detail values the bible lacks become Race and Realm
  entries. `Report::created` lists them, and the app's `PasteUndo::made` trashes them on Undo. (`others` is looked up
  again after each entry is made: the entries vector moves.)
- `blankPage(kind, sections, books)`: Name, Other names, How it's said, Summary, Details with suggested labels per
  kind, the world's sections, a line per book, Family (characters), Relationships with suggested labels, Notes,
  Spoilers. The parser ignores its blanks: a blank "Name:" doesn't take the next part's line, "Label:" lines without a
  value are skipped in Details, Family and Relationships, headings straight after blank parts still count, and an empty
  Spoilers leaves nothing. "Copy a blank page to fill in" in the entry panel puts it on the clipboard (CRLF).
- **The world's own relationships**: `CustomRelation {label, reverse}` kept as `relations:world:<id>` (or `:book:`).
  `guessReverseRelation` words the other side ("Admires" -> "Admired by" through a participle with irregular verbs;
  "Haunted by" -> "Haunts"; "Guardian of" -> "Has guardian"; otherwise the same both ways). `ensureRelation` adds one
  unless a built-in or existing label (or its other side) matches; `relationReverse(world, book, label)` reads the
  world's own first, then `reverseRelation`, and replaces it in the entry page, the bible export and the paste;
  `renameRelation` changes the list and every link of the world's entries in one transaction; `relationUses` counts.
  A pasted Relationships line `relationFor` doesn't know becomes one (and links like any other).
- Worldbuilding › Relationships (`studio_relations.cpp`, `Focus::Relation`): the world's own kinds with their other
  side and link counts, New kind of relationship, the built-in ones listed for reference; the panel renames (FName) or
  rewords the other side (FAlt), or removes it with its links after asking. Add a relationship lists the world's own
  after the built-in ones, and "A new kind of relationship…".
- Name parts leave out forms of address ("Her Highness" never makes "Her" a name), in the core and the editor.
- Tested in core (1157 checks: the Aaliyah page, a blank page pasted unchanged and partly filled, reverse wordings,
  renaming) and in Wine (Aaliyah pasted with Admires linked; renaming a kind with its link following).

## 0.38.1: pasting replaces, Reset the bible (19 Nov 2026)

- `paste::apply` replaces instead of merging: each part the page has replaces the entry's (name, other names,
  pronunciation, summary, all the details, each section and each matched book's notes, the notes); several headings
  for one template section are still put together. Relationship kinds the page lists (after `relationFor`) lose their
  old links (either direction) and take the pasted ones; kinds it doesn't list are kept. The app's `PasteUndo` keeps
  the links removed as well as added, so Undo puts them back.
- `Library::resetBible(world, book, everything, BibleReset&)`: every live entry trashed in one transaction; with
  `everything`, the settings `conlangs:`, `boards:`, `bible-share:`, `checks-ok:`, `kind-ok:` and each kind's
  `entry-template:` are cleared, their old values kept in the `BibleReset`; `undoBibleReset` restores both. The app
  (`resetBible` in studio_paste.cpp): "Reset the bible…" at the foot of the Worldbuilding sidebar, a choice of entries
  only or everything, a confirmation, a backup (and its copy to the backup folder) first, then Ctrl+Z
  (`wb_.resetUndo`) to take it back. Tested in core (1132 checks) and in Wine (both ways, and undone).

## 0.38.0: sharing the bible, more covers, worlds first (19 Nov 2026)

CJ asked for a nicer bible website with styling chosen before export, languages in the website and PDF, more cover
layouts, worlds on the start page, and races, realms and the like only from what he adds.

- **bible_export**: `Options` gains `pictures`, `family`, `languages`, `glossary` (website and PDF). `Look` (theme,
  mode auto/light/dark, accent, heading and body typefaces, title, tagline, cards, search, kindColours) with
  `toJson`/`lookFromJson` (unknown themes and bad colours fall back). Five themes (`themeDefs`: a light and dark
  palette, default typefaces, capitals for Grimoire; Night sky is dark either way, with a CSS star field) and eight
  typefaces (`fontDefs`: system stacks, and Crimson Text, Gentium Book Plus and Courier Prime copied from the app's
  fonts folder with their OFL and `@font-face`). `websiteCss(look)` builds the stylesheet from CSS variables (kind
  colours as `--k-<kind>`; a custom accent is mixed towards white in dark mode). The site: a sticky header with kinds,
  glossary, languages and a search box (`search.js` holds the names, other names, kinds and summaries; `site.js`
  filters them as you type, arrow keys and Enter, `/` to focus, and filters dictionary tables); the home page's hero
  (banner or an accent gradient), description, kind chips and cards (picture or initials, summary clamped); entry
  pages with a breadcrumb, sections (bulleted lines become lists), notes per book, map with pins, gallery, notes, and
  an infobox aside (portrait, details, family, relationships grouped by label), previous and next of the same kind;
  a glossary with a letter bar; `languages.html` (a section per language, columns only when used, a filter box).
  Responsive below 860 px; print styles. `languagesOf` reads `conlangs:world:<id>`/`conlangs:book:<id>`.
  `printableSource` adds a Languages part (a chapter per language: notes, then each word with pronunciation, part and
  meaning, letter headings over 30 words) and leaves out what's switched off.
- **App** (`studio_share.cpp`): Worldbuilding › Share the bible is a page (`wb_.share`, `Focus::Share`), choices in
  `ShareState` kept as `bible-share:world:<id>` (or `:book:`). Make (segmented), who it's for (chooser: you, or a
  reader's guide per book), what goes in (tick chips), theme cards drawn from `themeColours`, light/dark, colour
  swatches (theme's, world's, eight more; own colour typed in the panel's FAccent), heading and text typefaces, cards
  or lists, search and kind colours; a sketch of the site (`drawSharePreview`: header, banner with the world's banner
  picture, chips, character cards with their pictures) beside the choices when wide, below them otherwise. The panel
  edits the title (FName) and tagline (FDescription). For a PDF: the typesetting preset and typeface.
- **Covers** (`cover.cpp`): seven more layouts (arch, medallion, sidebar, stacked, starfield, ornate, modern), with
  `Canvas::disc`, `ring` (also an arch's straight sides), `diamond`, `glow`, and `drawPicture` taking an `inside`
  mask (arch and circle). The designer lists layouts two to a row for any number.
- **Start page**: `View::Worlds` (`pageWorlds`) is the default view and the first sidebar row: a card per world
  (banner or accent with bands, logo, name, counts of series, books and bible entries, three covers, the description,
  Worldbuilding and Open links) and one for independent books. `restorePlace` reopens Writing only; otherwise the
  library opens on worlds.
- **Facets**: realm and type of place have no built-in options (like race since 0.32.1): the bible's Races and Realms
  and values already written. "New realm or plane…" creates a Realm entry; "Write a new one…" adds a "Type: " line to
  the details and focuses it.
- Tested: core (1120 checks: themes, looks round-tripped and repaired, the site with languages, bundled fonts, search
  data, cards and the tagline; a plain site without the optional parts; the PDF's Languages part); the five themes
  rendered in Chromium at 1280 px light and dark (home, entry, search, languages) with no script errors; all eleven
  covers rendered with and without a picture; Studio in Wine (Your worlds, Share the bible's choices, the sketch
  following theme and tagline).

## 0.37.0: pasting, and choice sections (19 Nov 2026)

CJ asked for better pasting, for world bible sections that can be text or buttons, and to paste a whole character
sheet (his Tom, Sarrah, Light Magic and Telvana pages are test fixtures in `tests/fixtures/paste/`) so the parts go to
the right sections.

- **studiocore/entry_paste** parses pasted text into a `Page` (name, other names, pronunciation, summary, details,
  sections, book notes, relations, notes) and `apply`s it to an entry. Headings: Markdown, a whole line in bold, a
  known word on its own (`slotOf`), "Label:" with nothing after, or a short capitalised line after a boundary (a
  blank line, a sentence's end, a field or another heading), so pages with no blank lines still split. Text before
  the first recognised part (a chat's preamble) is dropped. `looksLikePage` needs two parts, one with a known name, so
  lists and sections with subheadings aren't taken for pages. Applying merges: empty fields filled, text added after
  what's there unless already present, details replaced by label, sections into the template section with that title
  or a part of it ("Appearance" -> "Appearance & personality"), book notes by title, else "Book N" by reading order
  when no title is given (a title that's no book's goes to the notes), Family/Relationships lines into links via
  `relationFor(kind, label)` when the named entry exists (by name, or an other name only one entry has), the rest
  into details. A second paste of the same page changes nothing.
- **Choice sections**: a template line "Title: A | B | C" (`sectionSpec`), with "(any)" after the title for several.
  The section is still keyed by its title and holds text, so changing the options never loses writing;
  `toggleSectionOption` keeps written words that aren't options. `entrySections` maps template lines to titles.
- **App** (`studio_paste.cpp`): every panel box is subclassed for WM_PASTE: a page onto the open entry, else the text
  cleaned (`cleanPastedText`: no-break and zero-width spaces, line breaks to the box's kind, one-line boxes join lines,
  other names with commas). Ctrl+V on an entry or the cards (new entry of a chosen kind), "Fill this page from what
  you've copied", and `PasteUndo` (the entry before, links added, or the entry made) for Ctrl+Z / "Undo the paste".
- **Editor** (`editor/src/paste.js`): `cleanPastedHTML` (Word markup, Google Docs' bold wrapper, and inline styles
  turned into tags, since the page's CSP stops `style` attributes being applied, so ProseMirror's style rules never
  matched pasted bold), `tidySlice` (curly quotes using the character before the cursor, dashes, ellipses, blank
  gap paragraphs and indents gone, "***" lines to scene breaks), `textParagraphs` for plain text. Pasted pictures go
  to the app as base64 (`pasteImage`, 40 MB limit) and into the artwork library, then the text.
- Tested: core (1088 checks: the four fixtures parsed and applied, links made to existing entries by name and other
  name, a second paste changing nothing, choice toggling); the editor's paste in Chromium (Google Docs' wrapper and
  span styles, Word markup, plain text, quotes and scene breaks); Studio in Wine (Ctrl+V on the cards making a
  character from Sarrah's page, a page pasted into a panel box with a link made, Undo the paste and Ctrl+Z, lists
  pasted into one-line boxes, choice and several-choice sections). Pasted pictures and the editor's paste inside the
  app need WebView2, so Windows only.

## 0.36.0: constructed languages (19 Nov 2026)

CJ asked for conlangs to be imported and exported, and for their words not to be marked as misspellings unless they're
misspelt.

- `studio/conlang` (studiocore, tested): `Language` (id, name, notes, words) and `Word` (word, meaning, part,
  pronunciation, notes). Kept per bible scope in the library's settings, `conlangs:world:<id>` or `conlangs:book:<id>`,
  as JSON (`toJson`/`fromJson`: nameless languages and empty words dropped, duplicate ids renumbered; at most 50
  languages, 50,000 words each). `importFile` reads NorthRay's .nrlang (JSON, one language or a "languages" list),
  CSV/TSV/semicolon files (quoted fields; a header row's columns recognised by name: word/conword/lemma/headword,
  meaning/gloss/definition/english/translation, part of speech/pos/class, pronunciation/ipa, notes; otherwise word,
  meaning, part), SFM/MDF lexicons (\lx, \ge/\gn, \de, \ps, \ph, \nt; sniffed in files without an extension) and plain
  lists (`word = meaning`, `word - meaning`, `word: meaning`, tab, or a word alone; a heading line skipped), from UTF-8
  (BOM or not) or UTF-16 with a BOM, up to 32 MB. `exportJson` (.nrlang) and `exportCsv` (UTF-8 with a BOM, CRLF).
  `merge` adds new words and fills gaps in existing ones (matched ignoring ASCII case). `spellData`: whole words,
  hyphenated and multi-word entries also split into parts, and affixes (`-in` an ending, `ve-` a beginning).
- Worldbuilding › Languages (studio_languages.cpp): a sidebar item under Glossary; the page has Import / New language,
  a tab per language, Add a word / Export / Name and notes / Remove, and the words like a dictionary (only rows on screen
  are drawn). Focus::Language (contextSeries "w:<word>" for a word) edits a language (FName, FNotes) or a word (FName,
  FDescription meaning, FGenre part, FAlt pronunciation, FNotes) in the right panel, saved through the usual
  commitFields. Saving sorts words alphabetically and calls sendView so Writing's checker updates at once. Parsed
  languages are cached against the stored JSON (asked for every frame).
- The editor: sendView adds `style.languages` ([{name, words: [{word, meaning} | part], prefixes, suffixes}]).
  spell.js `setConlangs`: a word of a language is known as it is or with one known beginning and up to two endings;
  `conlangNear` finds the nearest language word within 1-3 edits (by length) for a misspelling, and the hint then
  reads "... isn't in the British English dictionary or in Telvani. Did you mean the Telvani word "veyth'ar" (light)?";
  language words near a misspelling join the suggestions.
- Tested: core (1022 checks: every format, round trips, merge, spell data); the editor in Chromium (veyth'ar,
  sel-amar, veveyth'ar and veyth'arin accepted; veythar marked with veyth'ar suggested first; English misspellings as
  before); Studio in Wine (CSV import of 6 words with accents and affixes, editing a word's pronunciation, adding a
  word, a plain list merged into the language, CSV and .nrlang export, everything kept after a restart). Writing's
  spell marks themselves need WebView2, so in the app on Windows only.

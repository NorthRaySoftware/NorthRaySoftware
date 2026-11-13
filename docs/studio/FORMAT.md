# Studio library format (schema version 3)

A library is a **folder**. Organising books never moves files: worlds, series and books are rows in one database, and images are stored once by content.

```
<Library folder>/            default: %USERPROFILE%\Writing Studio Library
  library.db                 SQLite 3 database (WAL journal; library.db-wal / -shm appear while it's open)
  objects/ab/cdef...         images and other files, named by their SHA-256 (identical files are stored once)
  backups/library-YYYYMMDD-HHMMSS.db   a full copy made each time the library is opened (newest 10 kept)
```

Keep the folder out of OneDrive, Dropbox and similar folders: a sync program can copy the database halfway through a write. The Studio warns when it finds one. For copies, use **Settings > Export** (JSON) or copy a file from `backups/`.

## Tables

| Table | Holds |
|---|---|
| `meta` | `format = studio-library`, `schema_version = 7` |
| `worlds` | id, name, description, accent, notes, banner_blob, logo_blob, sort_order, created, modified, deleted |
| `series` | id, world_id (NULL: outside any world), name, description, status, notes, cover_blob, logo_blob, sort_order, timestamps, deleted |
| `books` | id, world_id (NULL: independent), title, subtitle, author, pen_name, synopsis, genre, language, isbn, edition, status, tags (JSON array), cover_blob, word_count, sort_order, timestamps, deleted, trim (3: print trim size id, empty = 6 × 9 in) |
| `editions` | (4) id, book_id, kind (paperback, hardback, large-print, ebook), name (empty: the kind's name), isbn, trim (empty: the book's), design (a preset id; empty: the book's Typesetting design), paper (white, cream), bleed, colour, sort_order, created, modified. Removed for good rather than trashed; they go with their book. |
| `entries` | (5) id, world_id (NULL: an independent book's), book_id, kind (character, place, group, thing, lore, event), name, aliases (JSON array), summary, details ("Label: value" lines), notes, image_blob, sort_order, timestamps, deleted |
| `entry_links` | (5) from_id, to_id, label (a relationship, read from `from`) |
| `snapshots` | (7) id, book_id, name, taken, words |
| `snapshot_nodes` | (7) snapshot_id, node_id, kind, title, content (document JSON), word_count, position |
| `writing_log` | (7) day (local YYYY-MM-DD), book_id, words added that day |
| `memberships` | series_id, book_id, reading_order, chrono_order, number (free text, empty = unnumbered), kind |
| `nodes` (2) | id, book_id, parent_id (NULL: top of the book), kind (`part`, `chapter`, `scene`), title, synopsis, status (`outline`, `draft`, `revised`, `final`), sort_order, content (document JSON, below), word_count, timestamps, deleted, pov_id (6: a viewpoint entry), target_words (6) |
| `artworks` (3) | id, blob (SHA-256, unique: each picture is listed once), name, kind (`illustration`, `map`, `ornament`, `photo`, `diagram`, `other`), width and height in pixels, format, source, notes, timestamps, deleted |
| `node_versions` (2) | id, node_id, saved, word_count, reason (`autosave`, `before restore`), content: earlier text, the newest 40 per node |

- Ids are random UUIDs. Times are Unix seconds (UTC). `deleted` is 0, or the time it went to the Trash.
- Statuses: book `idea, planning, drafting, revising, editing, ready, published, on-hold`; series `planned, ongoing, complete, on-hold`; instalment kind `novel, novella, companion, short-story, collection`.
- A book in a series that belongs to a world is always in that world. Foreign keys cascade only when the Trash is emptied.

## Manuscript text

Each part, chapter and scene keeps its text as JSON in the shape the editor (ProseMirror) uses, limited to the Studio's schema (`libs/studiocore/include/studio/document.hpp`):

- Blocks: `paragraph` (attrs.style: `body`, `first`, `centered`, `epigraph`, `attribution`, `verse`, `letter`, `note`), `heading` (level 1-3), `blockquote`, `bullet_list` / `ordered_list` / `list_item`, `scene_break`, `image` (attrs.blob is the picture's SHA-256 in `objects/`, plus alt and caption, and optionally its placement: `cropX`, `cropY`, `cropW`, `cropH` as fractions of the picture, `width` as a fraction of the text width, `align` `left` or `right`; the stored file is never cropped).
- Inline: `text` with marks (`em`, `strong`, `underline`, `smallcaps`, `strike`, `sup`, `sub`, `code`, `link` with an https or mailto address) and `hard_break`.
- Everything saved is checked against this schema first; anything else is turned into plain text or dropped.

Nesting: parts at the top; chapters at the top or in a part; scenes anywhere above them. Word counts count letters and digits, joined by apostrophes and hyphens.

Versions: when a chapter or scene is saved and its last kept version is more than ten minutes old, the previous text is kept in `node_versions`. Restoring a version keeps the current text as a version first.

## Guarantees

- Every change is one transaction (`BEGIN IMMEDIATE` … `COMMIT`), with `synchronous = FULL`: after a crash or power cut the library is either before or after a change, never halfway.
- On opening: `PRAGMA quick_check`. A damaged database isn't opened; the newest backup can be copied over it.
- A library from a newer schema version is refused and left untouched. Older versions are migrated forward inside a transaction (version 2 added `nodes` and `node_versions`; version 3 added `artworks` and `books.trim`, and lists the pictures already used in text; version 4 added `editions`; version 5 `entries` and `entry_links`; version 6 `nodes.pov_id` and `nodes.target_words`; version 7 `snapshots`, `snapshot_nodes` and `writing_log`; nothing earlier changed meaning).
- Files outside the database (exports, images) are written to a temporary file, flushed to disk, then swapped into place.

## Coming in later versions

Per-book settings are kept in `meta` under `setting:` keys: `setting:typeset:<book id>` (the print design, JSON) and `setting:plan:<book id>` (the book's word target, JSON), `setting:goal:daily` (words a day), `setting:names-ok:<book id>` (names marked as fine, JSON array), `setting:publish-edition:<book id>` (the edition last open in Publishing), `setting:publish:<book id>` (paper, bleed, colour pictures, JSON; from 0.13.0 these move to the book's paperback edition the first time Publishing opens it). Worldbuilding records will be added as new tables through migrations; nothing above changes meaning.

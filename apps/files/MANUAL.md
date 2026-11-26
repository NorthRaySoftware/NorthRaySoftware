# NorthRay Files: Development and Reference Manual

| | |
|---|---|
| Application | NorthRay Files (`NorthRayFiles.exe`) |
| Version at the time of writing | 0.9.4 (`apps/files/VERSION`) |
| Status | **Preview**, released |
| Source | `apps/files/` (C++20, Windows Shell, UI in `libs/cjui`) |
| Architecture notes | `apps/files/docs/ARCHITECTURE.md` |
| Release notes | `shared/metadata/releases/files.json` |
| Manual last checked against code | 26 November 2026 |

Status labels used in this manual:
- **Implemented**: in the code and released.
- **Partial**: some of it is built; the rest is noted.
- **Planned**: agreed with the project director, not built.
- **Proposed**: suggested, not yet agreed.

---

## 1. Overview

**Purpose.** A file manager for Windows in the NorthRay look, being built towards a full alternative to File Explorer, with tools for creative work: colour labels, tags, collections, a gallery view, and pictures of art files.

**Audience.** Anyone managing files on Windows, especially people with folders of art, references and media.

**Philosophy.**
- Every change to files goes through Windows' own file operations (`IFileOperation`), so progress, "replace or skip", undo and the Recycle Bin behave exactly as in File Explorer.
- Labels, tags and collections live in NorthRay's own file and never change the files themselves.
- Online-only files (OneDrive and similar) are never downloaded just by browsing.

**Where it sits.** One of the four suite apps, sharing cjui with the others. Links to Writing Studio and Capture are planned (section 2).

## 2. Features

### Implemented (0.1 to 0.9.4)

- **Interface size** (0.9.3; since 0.9.4 the panes and preview only, the sidebar and toolbar keeping their size): 50–200%, from the menu's Interface size, Ctrl with + / − / 0, or Ctrl and the wheel away from the files (over the files in Icons and Gallery it still sizes the tiles). Kept on this PC (cjui).
**Browsing**
- Sidebar: Home, Desktop, Documents, Downloads, Pictures, Music, Videos, This PC, pinned folders, the Recycle Bin, collections and labels. It can be resized.
- A folder tree under This PC.
- Breadcrumb address bar. Type or paste paths, including UNC and `%USERPROFILE%`. A › after each part lists its subfolders; Backspace goes up.

**Views**
- Details (resizable columns; item counts and folder sizes worked out in the background).
- Icons (Ctrl+wheel changes the size).
- Gallery (Ctrl+Shift+1).
- Each folder remembers its own view and sort order.
- Pictures of images are turned the right way up and fade in. They are cached on disk, up to about 300 MB.

**File operations**
- Copy, cut, paste (two-way with File Explorer); rename (F2); new folder; delete to the Recycle Bin; Shift+Delete deletes for good, after asking.
- More than 50 items to the Recycle Bin asks first.
- Windows' own right-click menu under "More options" (Shift+F10).
- Folders refresh by themselves when anything in them changes.

**Batch rename** (Ctrl+Shift+F2)
- Builds names from a pattern: name, number, date, find and replace, case.
- Previews every name and checks it before anything happens.
- Undo with Ctrl+Z.

**Tabs and panes**
- Tabs (Ctrl+T); two panes side by side (Ctrl+\).
- Drag between panes or onto folders.
- Drag a folder to the sidebar to pin it.
- Tabs, panes and pins are restored next time.

**Drag and drop** with other programs, both ways.

**Recycle Bin** inside Files: Restore, Delete permanently, Empty.

**Search**
- As you type, in the folder and below.
- Phrases, wildcards, `kind:` and `ext:`.
- "Search the whole PC" through the Windows Search index, with a fallback when the index isn't available.

**Preview pane** (Alt+P) and quick look (Space)
- Pictures, the start of text files.
- Photoshop (.psd and .psb), Procreate, Krita (.kra) and OpenRaster pictures.
- Videos and PDFs through Windows' own thumbnail handlers.

**Creative organising**
- Colour labels, tags and collections.
- Searchable with `tag:x`, `#x` and `colour:red`.
- Kept in `labels.json`. They follow renames and moves made inside Files.

**Other**
- Long copies show progress inside the window.
- Light and dark themes.
- The menu links to NorthRay's website.

### Not verified on real Windows

- **Interface size (0.172):** built and cross-compiled, not yet run on Windows (Wine would not start in the build environment).
Behaviour was tested in Wine. Shell-dependent pieces need a check on Windows:
- Windows Search results.
- Thumbnail handlers.
- Recycle Bin details.
- Drag and drop with other programs.

### Planned (the director's answers of 15 and 18 Nov 2026; none in the release notes yet)

- Find duplicate files.
- Convert pictures (PNG, JPG, WebP).
- Resize many pictures at once.
- Make zip files, and browse inside them.
- Compare two folders.
- A picture's colour palette in the preview.
- Font previews.
- Video and audio playback in the preview.
- Rename photos by the date taken.
- Saved searches.
- A Recent files view.
- A disk-space map.
- Send pictures to a Writing Studio board.
- Drag into Capture's NorthRay library.
- Sort Downloads automatically.
- Notes on files.
- Open a folder in Capture as a slideshow.
- Rotate pictures.

**Also planned** (architecture §7k):
- UI Automation.
- Labels that follow moves made outside Files (NTFS file ids).
- An optional File Explorer replacement setting.
- Sorting folders by their size.

### Known limitations

- No screen-reader support for the drawn rows (no UI Automation).
- Labels don't follow files moved by other programs.
- No network locations in the sidebar.

## 3. Interface and user experience

- **Layout:**
  - Toolbar along the top, with back, forward, up, refresh, the breadcrumb, search, the view buttons and ⋯. The caption buttons sit at its end.
  - Night-sky sidebar on the left.
  - The list or tiles in the middle; optional preview pane on the right.
  - Counts and selection at the foot of the sidebar. There is no status bar.
- **Principles:** the suite's palette, calm buttons, themed searchable menus, no scroll bars.
- **Workflows:**
  - Browse, then open.
  - Organise with labels and tags, then find them again with `tag:`.
  - Gather a collection from anywhere.
  - Batch rename a folder of pictures.
  - Compare two folders side by side using two panes.
- **Accessibility:** full keyboard navigation, type to jump to a name, focus rings, WCAG AA contrast for faint text (0.9.1). UI Automation is still to do.

## 4. Technical documentation

| Item | Detail |
|---|---|
| Language | C++20 |
| UI | `libs/cjui` (chosen over WinUI 3: one design system and one toolchain; see the architecture document's §2) |
| Windows APIs | Shell (`IShellItem`, `IFileOperation`, `IContextMenu`, thumbnail handlers), WIC, Windows Search (OLE DB) |
| Build | CMake; MinGW cross-build |

**Structure**

`core/` is portable, unit-tested C++:

| Module | Contents |
|---|---|
| `paths` | Typed and pasted paths, breadcrumbs, valid names |
| `listing` | Entries, visibility, natural sort, sizes, selection |
| `exif` | JPEG orientation |
| `labels` | The labels, tags and collections store |
| Others | Search query parsing and batch-rename patterns |

`src/`:

| File | Contents |
|---|---|
| `shell.*` | Listing on worker threads, icons, type names |
| `operations.*` | Copy, move, rename and delete through `IFileOperation` |
| `files_window.*` | The window |
| `thumbnails.*` | WIC pictures on a background thread, plus the disk cache |
| `search.*` | Folder and whole-PC search |
| `dragdrop.*` | Drag and drop |
| `quick_look.cpp` | Quick look |
| `batch_rename.cpp` | Batch rename |
| `folder_sizes.*` | Folder sizes worked out in the background |
| `settings.*` | `settings.json` |
| `main.cpp` | Start-up. Each launch is its own window; a path on the command line opens there |

**Data storage** (`%LOCALAPPDATA%\NorthRay\Files\`)

| Path | Contents |
|---|---|
| `settings.json` | Views per folder, columns, panes, tabs, pins, sidebar width |
| `labels.json` | Labels, tags and collections |
| `Thumbnails\` | The picture cache |

**External integrations:** Windows Shell, the Windows Search index, and the system's thumbnail and preview handlers.

## 5. Development

**Status:** preview.

**Milestones**
- 0.1: read-only browsing.
- 0.2: file operations.
- 0.3: tabs and panes.
- 0.4: search and preview.
- 0.5: labels, tags, collections, gallery.
- 0.6: Recycle Bin, drag and drop.
- 0.7: batch rename.
- 0.8 and 0.9: polish.
- 0.9.1: calm controls.
- 0.9.2: website link.

**Outstanding:** the planned list above. The director chose batch rename first, then skipped the rest of the Files questions; his round-0 priorities were creative file tools, organising, and looks and speed.

**Technical debt:** the labels key on lower-case paths, so moves made outside Files lose them.

## 6. Maintenance

**Run:** `NorthRayFiles-Setup`, or `build/windows/apps/files/NorthRayFiles.exe [path]`.

**Build:** `tools/build-windows.sh`; for native tests, `ninja -C build/native filescore_tests`.

**Test:** `filescore_tests`, 373 checks, all passing on 26 Nov 2026.

**Troubleshooting**

| Problem | Cause and fix |
|---|---|
| Pictures missing | The Windows codec for that format isn't installed (WebP, HEIC and AVIF depend on Windows) |
| Whole-PC search returns nothing | Windows Search is off; Files falls back to reading the drives |
| Labels lost after a move | The move was made outside Files (known limitation) |

**Implementation notes**
- Never write into users' files for metadata.
- Listing runs on worker threads so huge folders never freeze the window.
- Online-only files keep their normal icon, so browsing never downloads them.

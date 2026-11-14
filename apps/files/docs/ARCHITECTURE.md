# NorthRay Files: architecture

Status: **0.1.1, Version 0.1 (Foundation)**, with picture thumbnails. A working, read-only file browser in the NorthRay design language.
Last updated 14 November 2026. Developed by the NorthRay Software project. Contact: NorthRaySoftware@protonmail.com.

## 1. What 0.1 is

A native Windows window that browses real folders: a night-sky sidebar of places and drives, a toolbar with back,
forward, up and refresh, a breadcrumb address bar you can type or paste into, the folder in Details or Icons view with
Windows' own icons and type names, sorting, single and multiple selection, keyboard navigation, This PC, and clear
loading, empty and error states. Files open in their default app. Since 0.1.1, image files show the picture itself in Icons view (section 4a).

**It never changes a file.** Nothing in 0.1 copies, moves, renames, deletes or writes anywhere but its own settings.
File operations arrive in 0.2 as a separate service (see section 7).

## 2. UI framework: cjui (Win32 + Direct2D), not WinUI 3

The brief named WinUI 3 a leading candidate. It was weighed against the framework the other NorthRay apps use:

| | cjui (Win32 + Direct2D, the suite's own) | WinUI 3 (Windows App SDK) |
|---|---|---|
| NorthRay look | Already built: palette, night sky, aurora band, menus (`cjui::showMenu`), focus rings, animations | Would be rebuilt in XAML styles, a second design system to keep matching |
| Shared with | Launcher, Writing Studio, Browser, Community, CJ Radio | Nothing else in the suite |
| Toolchain | MinGW (the suite's) or MSVC; builds and is tested in the project's workspace | MSVC + C++/WinRT + MSBuild/NuGet; can't be built in the project's workspace |
| Deployment | One static .exe | Windows App SDK runtime (framework package or self-contained ~50 MB) |
| Controls | Drawn by us (list, tree, breadcrumb) | Ready-made, with UI Automation built in |
| Accessibility | Keyboard throughout; **no UI Automation provider yet** (screen readers can't read drawn rows) | Built in |

Chosen: **cjui**, for one design system and one toolchain. The cost is accessibility work: a UI Automation provider for
cjui (list, rows, buttons) is planned for 0.8 and would benefit every NorthRay app. Revisit if a feature needs a WinUI
control we can't reasonably draw.

## 3. Shape

```
apps/files/
  core/                portable C++20 (no Windows calls), unit-tested on Linux (ASan/UBSan) and as a Windows .exe
    paths              typed/pasted paths -> folder paths (drive, UNC, \\?\, file:///, relative), parents, breadcrumbs,
                       extended-length form for API calls, valid names, extensions
    listing            Entry (Win32 attributes), visibility (hidden / protected system), natural sort, sizes, Selection
    exif               a JPEG's EXIF orientation from its first bytes; turning pixels the right way up
  src/
    shell.*            the Windows side: listing on worker threads, drives, known folders, type names and icons
                       (Shell), opening with the default app; nothing that writes
    files_window.*     the window, drawn with cjui
    thumbnails.*       pictures of image files (WIC on a background thread, kept in memory)
    settings.*         settings.json (versioned, defensive, atomic save)
    main.cpp           start-up; each launch is its own window; a path on the command line opens there
  tests/               filescore_tests (208 checks)
```

Later modules (operations service, tabs and panes, search, previews, metadata database) go beside these, with their
logic in `core/` where it can be tested without Windows.

## 4. Windows Shell use (researched; what 0.1 uses)

- **Listing:** `FindFirstFileExW(FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH)` on the extended-length path
  (`\\?\C:\...`, `\\?\UNC\server\share\...`), which lifts MAX_PATH and turns off Win32's name rewriting (trailing dots
  and spaces). Each entry keeps its attributes and reparse tag. On a worker thread; results arrive as `WM_APP_LIST`
  batches (every 4,000 entries or 150 ms), tagged with a request number so stale results are dropped.
- **Type names and icons:** `SHGetFileInfoW(SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_SYSICONINDEX)` by
  extension, cached: the file isn't opened. Only programs, icons and shortcuts (`.exe .ico .lnk .url ...`) have their
  icon read from the file, and never for online-only (cloud) files, which would download them. Icons come from the system
  image list (`SHGetImageList`) and are drawn by Direct2D.
- **Known folders:** `SHGetKnownFolderPath` (Profile as Home, Desktop, Documents, Downloads, Pictures, Music, Videos),
  shown only when they exist. **Drives:** `GetLogicalDrives`, `GetDriveTypeW`, `GetVolumeInformationW`,
  `GetDiskFreeSpaceExW` on a worker, with `SEM_FAILCRITICALERRORS` so an empty card reader never shows a system dialog.
  `WM_DEVICECHANGE` refreshes them.
- **Opening:** `ShellExecuteExW` with the default verb, as a double-click in File Explorer (SmartScreen and Mark of the
  Web apply as usual).
- **Real names:** a typed `c:\filestest` is shown as `C:\FilesTest` (each part's name from `FindFirstFileExW`; not
  `GetFinalPathNameByHandle`, which would resolve junctions and change where you are).
- **Researched for later:** `IShellItem`/PIDLs for virtual locations (Recycle Bin, phones, network), `IFileOperation` for
  copy/move/delete with Windows' own conflict and progress UI and undo, `IContextMenu` for the Shell context menu,
  `IDropTarget`/`IDataObject` (`CF_HDROP`, `CFSTR_SHELLIDLIST`) for drag and drop, `IThumbnailCache` for thumbnails,
  preview handlers (run out of process by `prevhost.exe`), Windows Search (`SystemIndex` via OLE DB),
  `ReadDirectoryChangesW` with overflow -> rescan, and the NTFS USN journal for an optional index.

Not in 0.1: Shell virtual locations. The model already keeps a location as "a folder path or This PC", so a third kind
(a Shell item) can be added without confusing virtual items with real folders.

## 4a. Thumbnails (0.1.1)

- **Decoding:** Windows Imaging Component (`IWICImagingFactory`), Windows' own codecs: JPEG, PNG, GIF, BMP, TIFF, ICO, JPEG XR,
  and WebP, HEIF and AVIF where those Windows codecs are installed. Only `CreateDecoderFromFilename`, a Fant scaler that
  fits the picture in a 128 or 256 pixel square (chosen by DPI; never enlarged) and a conversion to 32-bit RGBA. No
  third-party Shell thumbnail handlers are loaded into the process. For an icon file the largest size is used.
- **The right way up:** the EXIF orientation from WIC's metadata reader (`/app1/ifd/{ushort=274}`), or, when that has no
  answer, from the file's first 128 KB by NorthRay's own parser (`core/exif`, bounds-checked and tested on truncated and
  lying data). The decoded pixels are then turned in code (all eight orientations, tested), not by WIC's flip-rotator.
- **Threading:** one background thread (COM multithreaded). Requests are taken newest first, so what's on screen comes
  first; at most 400 wait, and a new folder drops the rest. Results reach the window as `WM_APP_THUMB`. The thread is
  detached, so a slow file never holds up closing the window.
- **Skipped:** online-only (cloud) files, so browsing never downloads them; empty files; files over 256 MB; pictures over
  400 megapixels. They keep their normal icon, as do files that fail to decode.
- **Memory:** up to 600 pictures, least recently used dropped first, with their Direct2D bitmaps. Keyed by path,
  modified time and size, so a changed file gets a new picture. No disk cache yet (planned with previews in 0.4).
- **Tested** under Wine: JPEG, PNG, BMP and ICO; tall and wide pictures; EXIF orientations 3, 6 and 8 (matching a
  reference library); a broken .jpg (icon); a hidden picture (faded); 400 photos (pressing End shows the last ones at
  once); dark theme; clean exit. WebP isn't available in the simulation, so its icon showed; please check WebP and HEIC
  on a real PC.

## 5. Behaviour and safety (as built)

- Hidden items follow the setting (off by default) and are drawn faded when shown; protected operating system files have
  their own setting with a warning, as in File Explorer. File extensions are always shown.
- Links (junctions, symbolic links) carry a link mark so they aren't mistaken for ordinary folders. 0.1 never walks a
  folder recursively, so link loops can't trap it.
- Errors are explained in plain words: not found, no such drive, drive not ready, access denied (NorthRay Files never
  asks for administrator rights on its own), network unreachable, path too long, and a listing that stopped part-way.
- Typed paths: quotes, `/`, `C:`, UNC, `\\?\`, `file:///` (percent-decoded UTF-8), environment variables, relative
  paths and `..`; never above a root; names Windows doesn't allow are refused with a reason rather than guessed at.
- No telemetry and no logs of paths. Settings: `%LOCALAPPDATA%\NorthRay\Files\settings.json`.

## 6. Tested (14 November 2026)

- `filescore_tests`: 96 checks (paths, sorting, sizes, selection), Linux with AddressSanitizer/UBSan.
- Build: the whole suite with MinGW-w64, no warnings in Files.
- Under Wine (Windows simulation), against a test folder of real files:
  real listing matching the disk; Unicode names (Greek, Japanese, accented; the emoji showed as a box only because Wine
  lacks an emoji font); natural order (chapter 2 before 10); an empty folder; hidden files hidden then shown faded;
  5,000 files listed with the window responsive, End reaching the last in Icons view; Shift and Ctrl selection with the
  status bar's count and size; breadcrumbs and typed paths (`Q:\` -> "There's no Q: drive", `c:/filestest/cover.png` ->
  its folder with the file selected and the real capitalisation); This PC with free space; dark theme; a narrow window
  (sidebar tucked away, columns dropped); clean exit with no process left.
- **Not testable here:** access denied (the simulation runs with full rights), network shares, removable drives coming
  and going, high-DPI scaling on a real display, and screen readers. Please check these on a real PC.

## 7. 0.2: file operations (done 14 November 2026)

- `core/operations`: `uniqueName` ("New folder (2)", "Notes (2).txt"), `copyName` ("cover - Copy.png", "cover - Copy (2).png"), `renameProblem` (forbidden characters, trailing space or dot, reserved device names, a clash with another name; a change of capitals only is allowed), `planTransfer` (never into itself; a move into the folder it's already in is nothing to do) and `isInside`. All are tested.
- `src/operations`:
  - `IFileOperation` with `FOF_ALLOWUNDO | FOFX_ADDUNDORECORD`: `transfer`, `copyAs` (an exact new name for copies made beside the original), `remove` (`FOFX_RECYCLEONDELETE`, or permanent after NorthRay's own question) and `rename`. Where IFileOperation can't be created or queued, they fall back to `SHFileOperationW`.
  - The clipboard writes CF_HDROP with "Preferred DropEffect" and the paths as text, the same formats as File Explorer.
  - `showShellMenu` gets IContextMenu from the parent folder (`GetUIObjectOf`), or the folder background (`CreateViewObject`). `forwardMenuMessage` sends WM_INITMENUPOPUP, DRAWITEM, MEASUREITEM and MENUCHAR on to IContextMenu2/3.
  - `watchFolder` runs `FindFirstChangeNotificationW` on a detached thread and stops when the request moves on. The window waits 350 ms after a change, then lists the folder again quietly (`reloadQuietly`), keeping the old list until the new one is complete and selecting what changed.
  - The Recycle Bin uses `SHQueryRecycleBinW` and `SHEmptyRecycleBinW`. Opening it hands over to File Explorer (`shell:RecycleBinFolder`) until NorthRay can browse it itself.
- Window: Ctrl+C, X and V, F2, Delete and Shift+Delete, Ctrl+Shift+N, Shift+F10. Cut items are drawn faded until they're pasted. The rename field is an EDIT over the name (Enter or focus leaving commits it, Esc cancels).
- Tested under Wine:
  - A new folder named as it was made.
  - A copy beside the original ("cover - Copy.png", selected).
  - Cut, then "Paste into this folder".
  - F2 with the extension left out of the selection.
  - Delete to the Recycle Bin (the sidebar count went to 1), with the next item keeping the focus.
  - A file made by another program appearing by itself.
  - "More options" showing the Shell's menu.
- On real Windows, please check: Windows 11's menu with Open with and Send to, the progress window for a large copy, and undo in File Explorer (Ctrl+Z).

## 7b. 0.3: tabs, panes and the sidebar (done 14 November 2026)

- **Tabs and panes.** Each tab is a `View` (location, history, listing, selection, scroll). The window draws one view at a
  time: `swapView` swaps a tab's `View` with the window's live fields (including cjui's `ScrollState`), so all the 0.1/0.2
  code keeps working on "the current folder". Two panes draw the inactive pane by swapping its tab in, drawing, and
  swapping back (`idPrefix_` keeps its hit ids apart). Background listings carry a request id unique across tabs
  (`nextRequest`), and `viewForRequest` routes a finished listing to whichever tab asked, even if it isn't showing.
  Keys: Ctrl+T, Ctrl+W, Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+\ (two panes), F6 (other pane).
- **Drag and drop** inside Files: onto a folder row/tile, the other pane, or a sidebar folder. Move by default, copy with
  Ctrl or across drives (as File Explorer), through the same `nrf::ops::transfer`. Dragging a folder onto the Pinned
  heading pins it.
- **Pinned folders** (sidebar, `pinFolder`/`unpinFolder`), and tabs, panes and active tabs, persist in settings.json
  (`tabs`, `tabs2`, `activeTab`, `activeTab2`, `twoPanes`, `pinned`).
- **Folder tree** under This PC: drives as roots, sub-folders read on demand (`FindFirstFileExW` with
  `FindExSearchLimitToDirectories`, filtered by the show-hidden setting) and cached by lower-case path; the cache entry is
  dropped when its folder changes or a branch is reopened. It opens itself to the current folder and scrolls it into view
  once the drives and folders on the way have been read. The wheel over the tree scrolls it.
- Tested in Wine: two panes, drag between panes (move), drag to pin, tabs restored after reopening, the tree opening to
  the current folder, navigating from it and collapsing a branch.
- Not done (yet): browsing the Recycle Bin inside Files (it opens File Explorer's Recycle Bin), dragging out to other apps
  (OLE drag source) and dropping in from them.

## 7c. 0.4.0: search (done 14 November 2026)

- Rules in the portable core (`nrf/search.hpp`, tested): `parse` (words, "phrases", wildcards, `kind:`, `ext:`/`.psd`),
  `matches`, `wildcardMatch` (iterative, no blow-up), `kindOf`, `indexSql` (Windows Search SQL with LIKE escaping and
  SCOPE), `relativeFolder`.
- A search is a `Location` with `query` (and `everywhere`), so tabs, history, Back and the title all work as for
  folders. `searchFoldersAsync` walks breadth-first (not through links, only into folders that would be shown), posting
  `ListBatch`es with `Entry::location` set; `searchIndexAsync` asks `Search.CollatorDSO` over OLE DB (IDataInitialize →
  ICommandText → IRowset) and, if that can't be done, walks the ready fixed/removable drives and marks the batches
  `noIndex` so the bar says so. At most 20,000 results.
- Typing narrows the shown results at once (`showHere` filters by the query); more letters on plain words never search
  again, anything else searches again 220 ms after typing pauses, keeping the old results until the first new batch
  (`searchReplace_`).
- Results: a Folder column (relative to the searched folder), and copy/cut/rename/delete/drag work by each result's own
  path; paste and new folder ask for a folder; Windows' menu needs one folder's items.
- Tested in Wine: instant results, narrowing, the folder scope, the whole-PC fallback (Wine has no Windows Search), Esc.
  The index path itself needs real Windows to verify.
- cjui menus with 13+ choices gained a type-to-search field and scrolling (used across the suite).

## 7d. Next: previews and art thumbnails (0.4.x)

Already in the core (tested): `nrf/art.hpp` reads Photoshop's flattened picture (8/16-bit grey, RGB, CMYK; raw or RLE,
box-averaged down) and its JPEG preview resource, and decodes text for previews. Still to do: the preview pane (Alt+P),
Procreate/Krita/OpenRaster previews from their zips (miniz), and video/PDF pictures from Windows' own thumbnail handlers.

## (Earlier plan) 0.2, core file operations

A `FileOperations` service on `IFileOperation` (copy, move, rename, new folder, delete to the Recycle Bin, permanent
delete with explicit confirmation), never overwriting silently, reporting the real result of each item, with Windows'
progress and conflict dialogs at first and a NorthRay operation queue later. Clipboard (`CF_HDROP` + Preferred DropEffect)
and drag and drop with File Explorer. The Shell context menu (`IContextMenu`) behind a "More options" item. Properties.

## 8. Known limitations in 0.1

(Updated for 0.4.0.) No previews yet, thumbnails in Icons view only (none in Details), no network locations
in the sidebar, no column resizing, no drag and drop to or from other apps, and no UI Automation for screen readers.

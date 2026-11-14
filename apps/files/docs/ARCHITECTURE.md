# NorthRay Files: architecture

Status: **0.1.0, Version 0.1 (Foundation)**. A working, read-only file browser in the NorthRay design language.
Last updated 14 November 2026. Developed by the NorthRay Software project. Contact: NorthRaySoftware@protonmail.com.

## 1. What 0.1 is

A native Windows window that browses real folders: a night-sky sidebar of places and drives, a toolbar with back,
forward, up and refresh, a breadcrumb address bar you can type or paste into, the folder in Details or Icons view with
Windows' own icons and type names, sorting, single and multiple selection, keyboard navigation, This PC, and clear
loading, empty and error states. Files open in their default app.

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
  src/
    shell.*            the Windows side: listing on worker threads, drives, known folders, type names and icons
                       (Shell), opening with the default app; nothing that writes
    files_window.*     the window, drawn with cjui
    settings.*         settings.json (versioned, defensive, atomic save)
    main.cpp           start-up; each launch is its own window; a path on the command line opens there
  tests/               filescore_tests (96 checks)
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

## 7. Next: 0.2, core file operations

A `FileOperations` service on `IFileOperation` (copy, move, rename, new folder, delete to the Recycle Bin, permanent
delete with explicit confirmation), never overwriting silently, reporting the real result of each item, with Windows'
progress and conflict dialogs at first and a NorthRay operation queue later. Clipboard (`CF_HDROP` + Preferred DropEffect)
and drag and drop with File Explorer. The Shell context menu (`IContextMenu`) behind a "More options" item. Properties.

## 8. Known limitations in 0.1

One window per launch, no tabs or panes (0.3), no search or previews (0.4), no thumbnails (icons only), no Recycle Bin
or network locations in the sidebar, no folder tree, no column resizing, no automatic refresh when a folder changes
(F5), and no UI Automation for screen readers.

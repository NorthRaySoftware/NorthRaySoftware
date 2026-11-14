// NorthRay Files: changing files, through the Windows Shell's own file operations (IFileOperation), so
// copies, moves, renames and deletes behave exactly as in File Explorer: its progress and "replace or
// skip" windows, the Recycle Bin, and Ctrl+Z in File Explorer. Also the clipboard (files copied here
// paste in File Explorer and the other way round), Windows' own right-click menu ("More options"),
// and watching a folder for changes.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nrf::ops {

struct Result {
    bool ok = true;
    bool cancelled = false;    // the person stopped it (or skipped everything)
    std::wstring message;      // what went wrong, in plain words
};

// Copies or moves `sources` into the folder `destination`. A copy into the folder they're already in
// gets a new name ("cover - Copy.png"), as in File Explorer.
Result transfer(HWND owner, const std::vector<std::wstring>& sources, const std::wstring& destination, bool move);
// Copies each source to an exact new path (a copy beside the original, "cover - Copy.png").
Result copyAs(HWND owner, const std::vector<std::pair<std::wstring, std::wstring>>& fromTo);
// To the Recycle Bin, or (permanently) gone; the caller asks first for permanent deletes.
Result remove(HWND owner, const std::vector<std::wstring>& paths, bool permanently);
Result rename(HWND owner, const std::wstring& path, const std::wstring& newName);
Result newFolder(const std::wstring& path);

// The clipboard, as File Explorer uses it (CF_HDROP and "Preferred DropEffect").
bool putOnClipboard(HWND owner, const std::vector<std::wstring>& paths, bool cut);
struct ClipboardFiles { std::vector<std::wstring> paths; bool cut = false; };
ClipboardFiles readClipboard(HWND owner);
bool clipboardHasFiles();

// Windows' own right-click menu for these items (all in one folder), or for the folder's background
// when `paths` is empty. Runs what's chosen; true if something was.
bool showShellMenu(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& paths, POINT screen, bool extended);
// While that menu is open, its submenus ("Send to", "Open with") need some of the window's messages.
bool forwardMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);

// Watches a folder; posts `message` (wParam = `request`) to `window` when something in it changes.
// Stops when `latest` moves past `request`.
void watchFolder(HWND window, UINT message, std::wstring folder, std::uint64_t request, std::shared_ptr<std::atomic<std::uint64_t>> latest);

// The Recycle Bin: how many items it holds and how big they are; emptying it (Windows asks first).
bool recycleBinInfo(std::uint64_t& items, std::uint64_t& bytes);

// What's in the Recycle Bin (0.6), read through the Shell's own Recycle Bin folder, as File Explorer
// shows it. Slow on a big bin, so it's called on a worker thread (with COM started there).
struct BinItem {
    std::wstring id;          // its name inside the Recycle Bin folder (for restoring or deleting it)
    std::wstring file;        // where its contents are kept now (C:\$Recycle.Bin\...\$R...), for previews
    std::wstring name;        // its name as it was
    std::wstring from;        // the folder it was deleted from
    std::uint64_t size = 0, deleted = 0;   // bytes; FILETIME
    bool folder = false;
};
bool listRecycleBin(std::vector<BinItem>& out, std::wstring& problem);
// Puts items back where they were deleted from ("undelete"), or deletes them for good ("delete";
// Windows asks first). `ids` are BinItem::id values.
Result recycleBinCommand(HWND owner, const std::vector<std::wstring>& ids, bool restore);
Result emptyRecycleBin(HWND owner);

}  // namespace nrf::ops

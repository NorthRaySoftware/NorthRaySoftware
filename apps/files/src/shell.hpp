// NorthRay Files: the Windows side of reading folders. Listing a folder,
// file types and icons from the Windows Shell, the known folders and drives,
// and opening a file with its default app.
//
// Nothing here changes a file. Listing runs on a worker thread and hands its
// results to the window as messages; a newer request makes older results
// stale, so a slow network folder never holds up the window or overwrites a
// newer one.
#pragma once

#include "nrf/listing.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nrf {

// Why a folder couldn't be read, in a form the window can explain.
enum class ListError { None, NotFound, NotAFolder, AccessDenied, NotReady, NetworkUnreachable, PathTooLong, Other };

// One message's worth of a listing (several arrive for a big folder).
struct ListBatch {
    std::uint64_t request = 0;
    std::vector<Entry> entries;
    bool done = false;
    ListError error = ListError::None;
    DWORD code = 0;               // the Windows error, for "Other"
    std::wstring fileToSelect;    // set when the path was a file: show its folder with it selected
    std::wstring canonical;       // the folder's path with each name as it's actually written (first batch)
};

// Drives for This PC.
struct Drive {
    std::wstring root;            // "C:\"
    std::wstring label;           // "Local Disk"; the volume name when it has one
    UINT type = 0;                // DRIVE_FIXED, DRIVE_REMOVABLE, ...
    bool ready = false;           // has media and could be read
    std::uint64_t total = 0, free = 0;
    int icon = -1;
};
struct DriveBatch {
    std::uint64_t request = 0;
    std::vector<Drive> drives;
};

constexpr UINT WM_APP_LIST = WM_APP + 40;     // lParam: ListBatch*
constexpr UINT WM_APP_DRIVES = WM_APP + 41;   // lParam: DriveBatch*

// Lists `folder` on a worker thread, posting batches to `window`.
// `latest` is the newest request number: a worker gives up once it's moved on.
void listFolderAsync(HWND window, std::wstring folder, std::uint64_t request, std::shared_ptr<std::atomic<std::uint64_t>> latest);
void listDrivesAsync(HWND window, std::uint64_t request, std::shared_ptr<std::atomic<std::uint64_t>> latest);

// Known folders, for the sidebar. Only the ones that exist on this PC.
struct Place {
    std::wstring label;
    std::wstring path;
    int icon = -1;
};
std::vector<Place> knownPlaces();

// Opens a file the way double-clicking it in File Explorer does (its default
// app, Windows' own security checks). Returns "" or a plain explanation.
std::wstring openWithDefaultApp(const std::wstring& path, HWND owner);

// Icons from the system image list, as RGBA pixels (cached by index and size).
// `size` is 16, 32 or 48 (pixels, before DPI scaling is chosen by the caller).
struct IconPixels {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
};
const IconPixels* systemIcon(int index, int size);
IconPixels iconPixels(HICON icon);

std::wstring describeListError(ListError e, DWORD code, const std::wstring& path, std::wstring* title);

}  // namespace nrf

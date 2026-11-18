// NorthRay Files: how many items a folder holds and how big it is altogether (0.9), for the Details view.
//
// Worked out on one background thread, newest request first (what's on screen), like the thumbnails. A folder's
// own item count comes first, then its total size, walking every folder inside it without following links
// (junctions and symbolic links), so a loop can't trap it. The walk stops after kMaxEntries or kMaxSeconds and
// the size is then shown as "more than". Only names and sizes are read (FindFirstFileEx), so nothing is opened
// and online-only (cloud) files are never downloaded. Kept in memory by path and the folder's modified time.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace nrf {

constexpr UINT WM_APP_FOLDERSIZE = WM_APP + 43;   // lParam: FolderSizeResult*

struct FolderSize {
    std::uint64_t items = 0;   // directly inside it (hidden ones too)
    std::uint64_t bytes = 0;   // everything inside it, at any depth
    bool counted = false;      // `items` is known
    bool sized = false;        // `bytes` is known
    bool partial = false;      // the walk stopped early: `bytes` is a lower bound
    bool failed = false;       // it couldn't be read (no access)
};

struct FolderSizeResult {
    std::wstring key;
    FolderSize size;
};

class FolderSizes {
public:
    explicit FolderSizes(HWND notify);
    ~FolderSizes();
    FolderSizes(const FolderSizes&) = delete;
    FolderSizes& operator=(const FolderSizes&) = delete;

    // What's known so far (nullptr: nothing yet). Asking queues it; the window gets WM_APP_FOLDERSIZE as
    // the count and then the size arrive.
    const FolderSize* get(const std::wstring& path, std::uint64_t modified);
    const FolderSize* peek(const std::wstring& path, std::uint64_t modified) const;   // without asking
    void received(FolderSizeResult* r);   // on the UI thread
    void forgetQueue();                   // a new folder: what was asked for no longer matters
    void clear();                         // Refresh (F5): work everything out again

    static constexpr std::uint64_t kMaxEntries = 250000;
    static constexpr double kMaxSeconds = 4.0;
    static constexpr std::size_t kMaxKept = 5000;

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
    std::unordered_map<std::wstring, FolderSize> kept_;
};

}  // namespace nrf

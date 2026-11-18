// NorthRay Files' settings (%LOCALAPPDATA%\NorthRay\Files\settings.json).
// Versioned and read defensively like the other NorthRay apps' settings: a
// missing or wrong-typed value falls back to its default, and a file from a
// newer version is never overwritten with less.
#pragma once

#include <string>
#include <vector>

namespace nrf {

enum class Theme { System = 0, Light = 1, Dark = 2 };
enum class ViewMode { Details = 0, Icons = 1, Gallery = 2 };

struct Settings {
    static constexpr int kVersion = 1;
    Theme theme = Theme::System;
    ViewMode view = ViewMode::Details;
    int sortKey = 0;              // nrf::SortKey
    bool sortDescending = false;
    bool showHidden = false;
    bool showProtected = false;   // protected operating system files: off, as in File Explorer
    std::wstring lastFolder;      // opened next time ("" = Home)
    // Tabs open last time, in each pane ("::thispc" for This PC), and which was showing.
    std::vector<std::wstring> tabs[2];
    int activeTab[2] = {0, 0};
    bool twoPanes = false;
    bool previewPane = false;     // the selected item shown beside the list (0.4)
    std::vector<std::wstring> pinned;   // folders pinned to the sidebar, in order
    float iconScale = 1.0f;       // Icons and Gallery tile size (Ctrl + mouse wheel), 0.7..2 (0.8)
    float sidebarWidth = 236;     // dragged by its edge (0.8), 180..420
    // Details columns (0.9), dragged by their left edges in the header; Name takes what's left.
    float dateColW = 170, typeColW = 170, sizeColW = 100;

    // Per-folder view and sort (0.9). `view`, `sortKey` and `sortDescending` above are what's showing now; these are
    // what a folder with nothing remembered opens with (saved as "view", "sortKey", "sortDescending").
    ViewMode baseView = ViewMode::Details;
    int baseSortKey = 0;
    bool baseDescending = false;
    struct FolderView {
        std::wstring path;   // lower case
        ViewMode view = ViewMode::Details;
        int sortKey = 0;
        bool descending = false;
    };
    std::vector<FolderView> folderViews;   // the most recently changed last; at most kMaxFolderViews
    static constexpr std::size_t kMaxFolderViews = 500;
    const FolderView* folderView(const std::wstring& lowerPath) const;
    void rememberFolderView(const std::wstring& lowerPath);   // the view and sort showing now
    void forgetFolderViews();

    bool load(const std::wstring& path);
    bool save(const std::wstring& path) const;
    bool readOnly = false;
};

}  // namespace nrf

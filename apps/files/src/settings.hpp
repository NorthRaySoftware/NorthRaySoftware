// NorthRay Files' settings (%LOCALAPPDATA%\NorthRay\Files\settings.json).
// Versioned and read defensively like the other NorthRay apps' settings: a
// missing or wrong-typed value falls back to its default, and a file from a
// newer version is never overwritten with less.
#pragma once

#include <string>
#include <vector>

namespace nrf {

enum class Theme { System = 0, Light = 1, Dark = 2 };
enum class ViewMode { Details = 0, Icons = 1 };

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
    std::vector<std::wstring> pinned;   // folders pinned to the sidebar, in order

    bool load(const std::wstring& path);
    bool save(const std::wstring& path) const;
    bool readOnly = false;
};

}  // namespace nrf

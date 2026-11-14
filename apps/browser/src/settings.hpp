// NorthRay Browser's own settings (%LOCALAPPDATA%\NorthRay\Browser\settings.json).
// Versioned and read defensively, like the other NorthRay apps' settings: a
// missing or wrong-typed value falls back to its default.
#pragma once

#include <string>

namespace nrb {

enum class Theme { System = 0, Light = 1, Dark = 2 };

struct Settings {
    static constexpr int kVersion = 1;
    Theme theme = Theme::System;
    std::string searchEngine = "duckduckgo";

    // False if the file couldn't be read (the defaults are used). A file from a
    // newer version is read as far as it's understood and never overwritten
    // with less, so going back to an older build loses nothing.
    bool load(const std::wstring& path);
    bool save(const std::wstring& path) const;  // atomic: written beside, then swapped in
    bool readOnly = false;                      // set when the file came from a newer version
};

}  // namespace nrb

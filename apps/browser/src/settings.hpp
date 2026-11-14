// NorthRay Browser's own settings (%LOCALAPPDATA%\NorthRay\Browser\settings.json).
// Versioned and read defensively, like the other NorthRay apps' settings: a
// missing or wrong-typed value falls back to its default.
#pragma once

#include <string>
#include <vector>

namespace nrb {

enum class Theme { System = 0, Light = 1, Dark = 2 };

struct Settings {
    static constexpr int kVersion = 1;
    Theme theme = Theme::System;
    std::string searchEngine = "google";
    bool searchEngineChosen = false;   // picked in the menu (otherwise the default, which may change)
    bool showBookmarksBar = true;
    bool savePasswords = true;   // the engine offers to save passwords after a sign-in
    bool fillForms = true;       // and fills in addresses and other form entries
    bool verticalTabs = false;   // tabs listed down the left side instead of across the top
    bool blockTrackers = true;   // refuse requests to ad and tracking services from other sites
    std::vector<std::string> blockerAllowed;   // sites where they're not blocked ("example.com")
    // The start page
    std::string startBackground;   // a picture's file in the User folder ("" = the night sky)
    bool showClock = true;
    bool showNotes = true;
    std::string weatherPlace;      // "" = no weather
    double weatherLat = 0, weatherLon = 0;
    bool fahrenheit = false;
    bool defaultOffered = false;   // the start page has asked once about being the default browser

    // False if the file couldn't be read (the defaults are used). A file from a
    // newer version is read as far as it's understood and never overwritten
    // with less, so going back to an older build loses nothing.
    bool load(const std::wstring& path);
    bool save(const std::wstring& path) const;  // atomic: written beside, then swapped in
    bool readOnly = false;                      // set when the file came from a newer version
};

}  // namespace nrb

// What NorthRay Launcher knows: the app catalogue (shipped metadata plus the
// online catalogue), where each app is installed, release notes, and the
// user's settings. No drawing here.
#pragma once

#include "cj/registry.hpp"
#include "cjui/packages.hpp"

#include <optional>
#include <string>
#include <vector>

namespace nrl {

enum class ThemeChoice { System, Light, Dark };

struct Settings {
    ThemeChoice theme = ThemeChoice::System;
    std::wstring installRoot;        // where new installs go; empty = default (%LOCALAPPDATA%\NorthRay\Apps)
    bool autoUpdate = true;          // install updates by themselves (apps that aren't running)
    bool keepInTray = true;          // closing the window leaves the launcher in the notification area
    bool startWithWindows = false;   // starts in the notification area when you sign in
    bool showToday = true;           // the Today strip on the home page (writing, recent pictures)
    bool showJumpBack = true;        // Jump back in on the home page and in the tray menu (0.99)
    std::vector<std::string> appOrder;   // the home page's apps in the order they were dragged into (0.97)
};

// The home page's Today strip: what the other NorthRay apps left for it on this PC.
struct Today {
    bool writing = false;            // Writing Studio's summary.json is there and is for today
    long long wordsToday = 0, dailyGoal = 0;
    int streak = 0;
    std::vector<std::wstring> pictures;   // newest first: screenshots and Capture's saved pictures
    // Jump back in (0.99): books written in lately, from Writing Studio's summary.json, newest first.
    struct Book {
        std::string id, chapterId;
        std::wstring title, series, chapter, cover;
        long long words = 0;
    };
    std::vector<Book> books;
    std::string library;             // the Studio library the ids belong to
    // NorthRay Capture's scene collection (settings.json) and its scenes (scenes\<name>.json), in order.
    bool capture = false;
    std::string collection, currentScene;
    std::vector<std::string> scenes;
};
Today readToday();
// The size of everything in a folder (and the folders in it), in bytes.
unsigned long long folderSize(const std::wstring& dir);

// A screenshot shipped with an app's metadata (data/screenshots/<id>.json).
struct Screenshot {
    std::wstring path;
    std::string caption;
};

struct ReleaseNote {
    std::string version;
    std::string date;                // YYYY-MM-DD
    std::vector<std::string> notes;
};

// One downloadable installer, from the online catalogue.
struct Release {
    std::string id;
    std::string version;
    std::string file;                // WritingStudio-Setup-0.59.0.exe
    std::string url;                 // absolute https:// (or http://localhost for testing)
    unsigned long long size = 0;
    std::string sha256;
};

struct AppItem {
    cj::AppEntry entry;
    cjui::InstalledInfo inst;
    std::optional<Release> release;  // what the catalogue offers
    std::vector<ReleaseNote> notes;  // newest first
    std::wstring iconPath;           // PNG shipped with the launcher, if any
    std::vector<Screenshot> screenshots;
    bool updateAvailable() const;
    bool installable() const { return !inst.found && release.has_value(); }
};

class Model {
public:
    void load();                     // registry, installs, release notes, cached catalogue
    void refreshInstalls();          // re-check where apps are installed
    void saveSettings();

    // The online catalogue. Returns "" on success or a message.
    std::wstring applyCatalogue(const std::string& jsonText, const std::string& baseUrl);
    bool catalogueConfigured() const { return !catalogueUrl.empty(); }

    std::wstring launch(const std::string& appId, const std::vector<std::wstring>& args = {});
    std::wstring uninstall(const std::string& appId);

    AppItem* app(const std::string& id);
    std::wstring installDirFor(const std::string& appId) const;
    std::wstring effectiveInstallRoot() const;

    std::vector<AppItem> apps;
    std::optional<Release> launcherRelease;   // NorthRay Launcher itself, if the catalogue lists a newer one
    std::vector<ReleaseNote> launcherNotes;
    Settings settings;
    std::vector<std::string> warnings;
    bool firstRun = false;           // no settings file yet: the welcome is shown

    std::wstring dataDir;            // metadata shipped next to the executable
    std::wstring stateDir;           // %LOCALAPPDATA%\NorthRay\Launcher
    std::string catalogueUrl;        // from community/links.json
    std::string downloadPage;
    std::string contactEmail;
    cj::UtcSeconds catalogueChecked = 0;
    std::wstring catalogueError;

private:
    void loadSettings();
    void loadNotes(const std::string& id, std::vector<ReleaseNote>& out);
};

cj::UtcSeconds nowUtc();
std::wstring exeDir();

}  // namespace nrl

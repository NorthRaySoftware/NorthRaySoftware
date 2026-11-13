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
    bool autoUpdate = false;         // install updates by themselves (apps that aren't running)
};

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
    std::string file;                // CJRadio-Setup-0.4.0.exe
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

    std::wstring launch(const std::string& appId);
    std::wstring uninstall(const std::string& appId);

    AppItem* app(const std::string& id);
    std::wstring installDirFor(const std::string& appId) const;
    std::wstring effectiveInstallRoot() const;

    // CJ Philip's apps (CJ Radio, ...) live in CJ Philip Community, which installs,
    // updates and opens them; NorthRay Launcher leaves them out.
    bool hideCjPhilipApps = false;
    static bool isCjPhilipApp(const cj::AppEntry& e) { return e.associatedCreator == "CJ Philip" && e.id != "cj-philip-community"; }

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

// NorthRay Capture's window: profiles and scene collections (the two names at the top).
//
// A profile is a set of settings: the frame rate, the monitor and second outputs, recording
// (folder, encoder, quality, tracks, the MP4 copy) and streaming (destinations, reconnecting,
// transitions). The window's own settings (theme, low-resource mode, the preview, the workspace) and
// the scene collection stay as they are when the profile changes. The current profile lives in
// settings.json; the others in profiles\<name>.json. Stream keys stay in Credential Manager: a copied
// profile gets its own copy of each key (so removing one in one profile never removes it from
// another), and deleting a profile removes only keys no other profile uses.
//
// A scene collection is a file of scenes and sources (scenes\<name>.json) with its own canvas size.
#include "capture_window.hpp"

#include "log.hpp"
#include "output/credentials.hpp"

#include "ncap/profiles.hpp"

#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <algorithm>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace ncap {

using cjui::MenuItem;
using cjui::narrow;
using cjui::widen;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> namesIn(const std::wstring& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(fs::path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && _wcsicmp(it->path().extension().c_str(), L".json") == 0) out.push_back(narrow(it->path().stem().wstring()));
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

std::string newDestinationId() {
    static std::mt19937 rng{std::random_device{}()};
    const char* hex = "0123456789abcdef";
    std::string id = "d";
    for (int i = 0; i < 10; ++i) id += hex[rng() % 16];
    return id;
}

std::string keyName(const std::string& id) { return "stream/" + id; }

}  // namespace

std::wstring CaptureWindow::profilesFolder() const { return stateFolder_ + L"\\profiles"; }

std::vector<std::string> CaptureWindow::profileNames() const {
    std::vector<std::string> v = namesIn(profilesFolder());
    if (std::none_of(v.begin(), v.end(), [&](const std::string& n) { return _stricmp(n.c_str(), settings_.profile.c_str()) == 0; })) v.push_back(settings_.profile);
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    return v;
}

std::vector<std::string> CaptureWindow::collectionNames() const {
    std::vector<std::string> v = namesIn(stateFolder_ + L"\\scenes");
    if (std::none_of(v.begin(), v.end(), [&](const std::string& n) { return _stricmp(n.c_str(), settings_.sceneCollection.c_str()) == 0; }))
        v.push_back(settings_.sceneCollection);
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    return v;
}

bool CaptureWindow::busyForSwitch(const wchar_t* what) {
    if (!recorder_.active() && !streamer_.active()) return false;
    flash(std::wstring(L"Stop recording and streaming to ") + what + L".", 5);
    return true;
}

// The settings that belong to the window, not to a profile: kept when the profile changes.
static void keepWindowSettings(Settings& to, const Settings& from) {
    to.theme = from.theme;
    to.lowResource = from.lowResource;
    to.previewFps = from.previewFps;
    to.layout = from.layout;
    to.savedLayouts = from.savedLayouts;
    to.sceneCollection = from.sceneCollection;
    to.welcomed = from.welcomed;
    to.studioMode = from.studioMode;
}

void CaptureWindow::applyProfile() {
    engine_.setOutputFps(settings_.outputFps);
    applyMonitor();
    syncAudio();
    applyTransitionFiles();
    designRendered_.clear();
    invalidate();
}

void CaptureWindow::switchProfile(const std::string& name) {
    if (_stricmp(name.c_str(), settings_.profile.c_str()) == 0 || busyForSwitch(L"change the profile")) return;
    CreateDirectoryW(profilesFolder().c_str(), nullptr);
    const std::wstring mine = profilesFolder() + L"\\" + widen(settings_.profile) + L".json";
    if (!settings_.save(mine)) {
        flash(L"This profile couldn't be saved, so it wasn't changed.", 6);
        return;
    }
    Settings next;
    next.load(profilesFolder() + L"\\" + widen(name) + L".json");   // a missing file: the defaults
    keepWindowSettings(next, settings_);
    next.profile = name;
    settings_ = next;
    settings_.save(settingsPath_);
    DeleteFileW((profilesFolder() + L"\\" + widen(name) + L".json").c_str());   // it's the current one now (settings.json)
    applyProfile();
    log::info("Profile changed to \"" + name + "\"");
    flash(L"Profile: " + widen(name), 3);
}

void CaptureWindow::profileMenu(POINT pt) {
    const auto names = profileNames();
    std::vector<MenuItem> items = {MenuItem::item(L"PROFILES: frame rate, recording and streaming settings", 0, {}, false)};
    for (std::size_t i = 0; i < names.size() && i < 100; ++i)
        items.push_back(MenuItem::item(widen(names[i]), 100 + (UINT)i, {}, true, _stricmp(names[i].c_str(), settings_.profile.c_str()) == 0));
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"New profile (a copy of this one)…", 1));
    items.push_back(MenuItem::item(L"Rename this profile…", 2));
    items.push_back(MenuItem::item(L"Delete this profile…", 3, {}, names.size() > 1));
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (cmd >= 100 && cmd < 100 + names.size()) {
        switchProfile(names[cmd - 100]);
        return;
    }
    if (cmd == 1) {
        if (busyForSwitch(L"change the profile")) return;
        askText(L"The new profile's name", widen(profiles::uniqueName(settings_.profile + " copy", names)), [this](const std::wstring& v) {
            const std::string name = narrow(v);
            std::string why;
            if (!profiles::validName(name, profileNames(), why)) {
                flash(L"Not made: " + widen(why) + L".", 5);
                return;
            }
            CreateDirectoryW(profilesFolder().c_str(), nullptr);
            if (!settings_.save(profilesFolder() + L"\\" + widen(settings_.profile) + L".json")) {
                flash(L"This profile couldn't be saved, so a copy wasn't made.", 6);
                return;
            }
            // Its own copy of each stream key, under new names.
            for (auto& d : settings_.destinations) {
                const std::string key = secrets::load(keyName(d.id));
                d.id = newDestinationId();
                if (!key.empty()) secrets::save(keyName(d.id), key);
            }
            settings_.profile = name;
            settings_.save(settingsPath_);
            log::info("Profile \"" + name + "\" made");
            flash(L"Profile “" + v + L"” made from the last one: change what you like in Settings.", 6);
            invalidate();
        });
    } else if (cmd == 2) {
        askText(L"Rename the profile", widen(settings_.profile), [this](const std::wstring& v) {
            const std::string name = narrow(v);
            std::vector<std::string> others = profileNames();
            std::erase_if(others, [&](const std::string& n) { return _stricmp(n.c_str(), settings_.profile.c_str()) == 0; });
            std::string why;
            if (_stricmp(name.c_str(), settings_.profile.c_str()) != 0 && !profiles::validName(name, others, why)) {
                flash(L"Not renamed: " + widen(why) + L".", 5);
                return;
            }
            if (name.empty()) return;
            settings_.profile = name;
            settings_.save(settingsPath_);
            invalidate();
        });
    } else if (cmd == 3 && names.size() > 1) {
        if (busyForSwitch(L"delete the profile")) return;
        const std::string gone = settings_.profile;
        if (cjui::messageBox(hwnd(), L"Delete the profile “" + widen(gone) + L"”? Its settings and the stream keys only it uses are removed. Your "
                                     L"recordings and scenes aren't touched.",
                             L"Profiles", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
            return;
        std::vector<std::string> goneIds;
        for (auto& d : settings_.destinations) goneIds.push_back(d.id);
        std::string to;
        for (auto& n : names)
            if (_stricmp(n.c_str(), gone.c_str()) != 0) {
                to = n;
                break;
            }
        switchProfile(to);
        DeleteFileW((profilesFolder() + L"\\" + widen(gone) + L".json").c_str());
        // Keys no remaining profile uses.
        std::vector<std::string> used;
        for (auto& d : settings_.destinations) used.push_back(d.id);
        for (auto& n : namesIn(profilesFolder())) {
            Settings s;
            if (s.load(profilesFolder() + L"\\" + widen(n) + L".json"))
                for (auto& d : s.destinations) used.push_back(d.id);
        }
        for (auto& id : goneIds)
            if (std::find(used.begin(), used.end(), id) == used.end()) secrets::remove(keyName(id));
        log::info("Profile \"" + gone + "\" deleted");
        flash(L"Deleted the profile “" + widen(gone) + L"”.", 4);
    }
}

void CaptureWindow::switchCollection(const std::string& name, bool copyCurrent) {
    if (busyForSwitch(L"change the scene collection")) return;
    saveScenes();
    const std::wstring dir = stateFolder_ + L"\\scenes";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (copyCurrent) {   // the new one starts as a copy of this one
        std::ofstream f(fs::path(dir + L"\\" + widen(name) + L".json"), std::ios::binary | std::ios::trunc);
        f << scenes_.toJson();
        if (!f.good()) {
            flash(L"The copy couldn't be written.", 5);
            return;
        }
    }
    live_.clear();   // the captures belong to the old collection (ids can repeat between collections)
    scenes_ = SceneCollection();
    settings_.sceneCollection = name;
    settings_.save(settingsPath_);
    loadScenes();
    if (!fs::exists(fs::path(scenesPath_))) {   // a new, empty collection: saved now so it's listed
        std::ofstream f(fs::path(scenesPath_), std::ios::binary | std::ios::trunc);
        f << scenes_.toJson();
        savedChanges_ = scenes_.changes();
    }
    syncSources();
    applyTransitionFiles();
    designRendered_.clear();
    log::info("Scene collection changed to \"" + name + "\" with " + std::to_string(scenes_.scenes().size()) + " scene(s)");
    flash(L"Scenes: " + widen(name), 3);
    invalidate();
}

void CaptureWindow::collectionMenu(POINT pt) {
    const auto names = collectionNames();
    std::vector<MenuItem> items = {MenuItem::item(L"SCENE COLLECTIONS: scenes, sources and the canvas", 0, {}, false)};
    for (std::size_t i = 0; i < names.size() && i < 100; ++i)
        items.push_back(MenuItem::item(widen(names[i]), 100 + (UINT)i, {}, true, _stricmp(names[i].c_str(), settings_.sceneCollection.c_str()) == 0));
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"New, empty collection…", 1));
    items.push_back(MenuItem::item(L"Copy this collection…", 2));
    items.push_back(MenuItem::item(L"Rename this collection…", 3));
    items.push_back(MenuItem::item(L"Delete this collection…", 4, {}, names.size() > 1));
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (cmd >= 100 && cmd < 100 + names.size()) {
        if (_stricmp(names[cmd - 100].c_str(), settings_.sceneCollection.c_str()) != 0) switchCollection(names[cmd - 100], false);
        return;
    }
    if (cmd == 1 || cmd == 2) {
        if (busyForSwitch(L"change the scene collection")) return;
        const bool copy = cmd == 2;
        askText(copy ? L"The copy's name" : L"The new collection's name",
                widen(profiles::uniqueName(copy ? settings_.sceneCollection + " copy" : "Scenes", names)), [this, copy](const std::wstring& v) {
                    const std::string name = narrow(v);
                    std::string why;
                    if (!profiles::validName(name, collectionNames(), why)) {
                        flash(L"Not made: " + widen(why) + L".", 5);
                        return;
                    }
                    switchCollection(name, copy);
                });
    } else if (cmd == 3) {
        askText(L"Rename the scene collection", widen(settings_.sceneCollection), [this](const std::wstring& v) {
            const std::string name = narrow(v);
            std::vector<std::string> others = collectionNames();
            std::erase_if(others, [&](const std::string& n) { return _stricmp(n.c_str(), settings_.sceneCollection.c_str()) == 0; });
            std::string why;
            if (_stricmp(name.c_str(), settings_.sceneCollection.c_str()) != 0 && !profiles::validName(name, others, why)) {
                flash(L"Not renamed: " + widen(why) + L".", 5);
                return;
            }
            if (name.empty() || name == settings_.sceneCollection) return;
            saveScenes();
            const std::wstring to = stateFolder_ + L"\\scenes\\" + widen(name) + L".json";
            if (!MoveFileExW(scenesPath_.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                flash(L"It couldn't be renamed.", 5);
                return;
            }
            scenesPath_ = to;
            settings_.sceneCollection = name;
            settings_.save(settingsPath_);
            invalidate();
        });
    } else if (cmd == 4 && names.size() > 1) {
        if (busyForSwitch(L"delete the scene collection")) return;
        const std::string gone = settings_.sceneCollection;
        if (cjui::messageBox(hwnd(), L"Delete the scene collection “" + widen(gone) + L"”? Its scenes and sources are removed (the files they "
                                     L"show, your recordings and your designs' pictures aren't touched). It goes to the Recycle Bin.",
                             L"Scene collections", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
            return;
        const std::wstring file = scenesPath_;
        std::string to;
        for (auto& n : names)
            if (_stricmp(n.c_str(), gone.c_str()) != 0) {
                to = n;
                break;
            }
        switchCollection(to, false);
        // To the Recycle Bin, so a mistake can be undone.
        std::wstring from = file + L'\0';
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        if (SHFileOperationW(&op) != 0) DeleteFileW(file.c_str());
        flash(L"Deleted “" + widen(gone) + L"” (it's in the Recycle Bin).", 5);
    }
}

}  // namespace ncap

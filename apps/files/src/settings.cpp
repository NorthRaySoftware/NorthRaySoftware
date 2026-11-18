#include "settings.hpp"

#include "cj/json_safe.hpp"
#include "cjui/gfx.hpp"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace nrf {

bool Settings::load(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    readOnly = cj::valueOr(j, "version", 1) > kVersion;
    const int t = cj::valueOr(j, "theme", 0);
    theme = t == 1 ? Theme::Light : t == 2 ? Theme::Dark : Theme::System;
    {
        const int v = cj::valueOr(j, "view", 0);
        view = v == 1 ? ViewMode::Icons : v == 2 ? ViewMode::Gallery : ViewMode::Details;
    }
    sortKey = std::clamp(cj::valueOr(j, "sortKey", 0), 0, 3);
    sortDescending = cj::valueOr(j, "sortDescending", false);
    baseView = view, baseSortKey = sortKey, baseDescending = sortDescending;
    folderViews.clear();
    if (j.contains("folderViews") && j["folderViews"].is_array())
        for (auto& x : j["folderViews"]) {
            if (!x.is_object() || folderViews.size() >= kMaxFolderViews) continue;
            const std::string p = cj::valueOr(x, "path", std::string());
            if (p.empty() || p.size() > 4000) continue;
            FolderView fv;
            fv.path = cjui::widen(p);
            const int v = cj::valueOr(x, "view", 0);
            fv.view = v == 1 ? ViewMode::Icons : v == 2 ? ViewMode::Gallery : ViewMode::Details;
            fv.sortKey = std::clamp(cj::valueOr(x, "sortKey", 0), 0, 3);
            fv.descending = cj::valueOr(x, "descending", false);
            folderViews.push_back(std::move(fv));
        }
    iconScale = std::clamp(cj::valueOr(j, "iconScale", 1.0f), 0.7f, 2.0f);
    sidebarWidth = std::clamp(cj::valueOr(j, "sidebarWidth", 236.0f), 180.0f, 420.0f);
    dateColW = std::clamp(cj::valueOr(j, "dateColW", 170.0f), 90.0f, 360.0f);
    typeColW = std::clamp(cj::valueOr(j, "typeColW", 170.0f), 80.0f, 420.0f);
    sizeColW = std::clamp(cj::valueOr(j, "sizeColW", 100.0f), 64.0f, 220.0f);
    showHidden = cj::valueOr(j, "showHidden", false);
    showProtected = cj::valueOr(j, "showProtected", false);
    lastFolder = cjui::widen(cj::valueOr(j, "lastFolder", ""));
    auto list = [&](const char* key, std::vector<std::wstring>& out, size_t most) {
        out.clear();
        if (j.contains(key) && j[key].is_array())
            for (auto& x : j[key]) if (x.is_string() && out.size() < most && x.get<std::string>().size() < 4000) out.push_back(cjui::widen(x.get<std::string>()));
    };
    list("tabs", tabs[0], 30);
    list("tabs2", tabs[1], 30);
    list("pinned", pinned, 60);
    activeTab[0] = std::clamp(cj::valueOr(j, "activeTab", 0), 0, 29);
    activeTab[1] = std::clamp(cj::valueOr(j, "activeTab2", 0), 0, 29);
    twoPanes = cj::valueOr(j, "twoPanes", false);
    previewPane = cj::valueOr(j, "previewPane", false);
    return true;
}

const Settings::FolderView* Settings::folderView(const std::wstring& lowerPath) const {
    for (auto it = folderViews.rbegin(); it != folderViews.rend(); ++it)
        if (it->path == lowerPath) return &*it;
    return nullptr;
}

void Settings::rememberFolderView(const std::wstring& lowerPath) {
    if (lowerPath.empty()) return;
    folderViews.erase(std::remove_if(folderViews.begin(), folderViews.end(), [&](const FolderView& f) { return f.path == lowerPath; }), folderViews.end());
    // The same as a folder with nothing remembered: nothing to remember.
    if (view != baseView || sortKey != baseSortKey || sortDescending != baseDescending)
        folderViews.push_back({lowerPath, view, sortKey, sortDescending});
    if (folderViews.size() > kMaxFolderViews) folderViews.erase(folderViews.begin(), folderViews.begin() + (long)(folderViews.size() - kMaxFolderViews));
}

void Settings::forgetFolderViews() {
    folderViews.clear();
    baseView = view, baseSortKey = sortKey, baseDescending = sortDescending;
}

bool Settings::save(const std::wstring& path) const {
    if (readOnly) return false;
    nlohmann::json j = {{"version", kVersion},          {"theme", (int)theme},         {"view", (int)baseView},
                        {"sortKey", baseSortKey},       {"sortDescending", baseDescending}, {"iconScale", iconScale}, {"sidebarWidth", sidebarWidth}, {"dateColW", dateColW}, {"typeColW", typeColW}, {"sizeColW", sizeColW},
                        {"showHidden", showHidden},     {"showProtected", showProtected},
                        {"lastFolder", cjui::narrow(lastFolder)}};
    auto list = [](const std::vector<std::wstring>& v) {
        nlohmann::json a = nlohmann::json::array();
        for (auto& x : v) a.push_back(cjui::narrow(x));
        return a;
    };
    j["tabs"] = list(tabs[0]);
    j["tabs2"] = list(tabs[1]);
    j["pinned"] = list(pinned);
    j["activeTab"] = activeTab[0];
    j["activeTab2"] = activeTab[1];
    j["twoPanes"] = twoPanes;
    j["previewPane"] = previewPane;
    {
        nlohmann::json a = nlohmann::json::array();
        for (auto& fv : folderViews)
            a.push_back({{"path", cjui::narrow(fv.path)}, {"view", (int)fv.view}, {"sortKey", fv.sortKey}, {"descending", fv.descending}});
        j["folderViews"] = a;
    }
    const std::wstring tmp = path + L".new";
    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump(2);
        if (!f.good()) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

}  // namespace nrf

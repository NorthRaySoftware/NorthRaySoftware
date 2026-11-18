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
    iconScale = std::clamp(cj::valueOr(j, "iconScale", 1.0f), 0.7f, 2.0f);
    sidebarWidth = std::clamp(cj::valueOr(j, "sidebarWidth", 236.0f), 180.0f, 420.0f);
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

bool Settings::save(const std::wstring& path) const {
    if (readOnly) return false;
    nlohmann::json j = {{"version", kVersion},          {"theme", (int)theme},         {"view", (int)view},
                        {"sortKey", sortKey},           {"sortDescending", sortDescending}, {"iconScale", iconScale}, {"sidebarWidth", sidebarWidth},
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

#include "settings.hpp"

#include "cj/json_safe.hpp"

#include <windows.h>

#include <fstream>
#include <sstream>

namespace nrb {

bool Settings::load(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    const int version = cj::valueOr(j, "version", 1);
    readOnly = version > kVersion;
    const int t = cj::valueOr(j, "theme", 0);
    theme = t == 1 ? Theme::Light : t == 2 ? Theme::Dark : Theme::System;
    searchEngineChosen = cj::valueOr(j, "searchEngineChosen", false);
    // An engine nobody chose is the default's (it was DuckDuckGo before 0.4.0).
    searchEngine = searchEngineChosen ? cj::valueOr(j, "searchEngine", "google") : std::string("google");
    showBookmarksBar = cj::valueOr(j, "showBookmarksBar", true);
    savePasswords = cj::valueOr(j, "savePasswords", true);
    fillForms = cj::valueOr(j, "fillForms", true);
    verticalTabs = cj::valueOr(j, "verticalTabs", false);
    blockTrackers = cj::valueOr(j, "blockTrackers", true);
    startBackground = cj::valueOr(j, "startBackground", std::string());
    if (startBackground.find_first_of("\\/:") != std::string::npos) startBackground.clear();   // a name in the User folder only
    showClock = cj::valueOr(j, "showClock", true);
    showNotes = cj::valueOr(j, "showNotes", true);
    weatherPlace = cj::valueOr(j, "weatherPlace", std::string());
    weatherLat = cj::valueOr(j, "weatherLat", 0.0);
    weatherLon = cj::valueOr(j, "weatherLon", 0.0);
    fahrenheit = cj::valueOr(j, "fahrenheit", false);
    defaultOffered = cj::valueOr(j, "defaultOffered", false);
    popX = cj::valueOr(j, "popX", 0);
    popY = cj::valueOr(j, "popY", 0);
    popW = cj::valueOr(j, "popW", 0);
    popH = cj::valueOr(j, "popH", 0);
    if (popW < 0 || popW > 20000 || popH < 0 || popH > 20000) popW = popH = 0;
    restoreTabs = cj::valueOr(j, "restoreTabs", true);
    askBeforeClosing = cj::valueOr(j, "askBeforeClosing", true);
    session.clear();
    if (j.contains("session") && j["session"].is_array())
        for (const auto& s : j["session"]) {
            if (!s.is_object() || session.size() >= 200) continue;
            SessionTab t{cj::valueOr(s, "url", std::string()), cj::valueOr(s, "title", std::string())};
            if (t.url.empty() || t.url.size() > 8192) continue;
            if (t.title.size() > 1000) t.title.resize(1000);
            session.push_back(std::move(t));
        }
    sessionActive = cj::valueOr(j, "sessionActive", 0);
    siteZoom.clear();
    if (j.contains("siteZoom") && j["siteZoom"].is_object())
        for (auto it = j["siteZoom"].begin(); it != j["siteZoom"].end(); ++it)
            if (it.value().is_number() && it.key().size() < 256 && siteZoom.size() < 2000) {
                const double z = it.value().get<double>();
                if (z >= 0.25 && z <= 5.0) siteZoom[it.key()] = z;
            }
    refBoard = cj::valueOr(j, "refBoard", 0LL);
    auto readMap = [&](const char* key, std::map<std::string, double>& out, double lo, double hi) {
        out.clear();
        if (j.contains(key) && j[key].is_object())
            for (auto it = j[key].begin(); it != j[key].end(); ++it)
                if (it.value().is_number() && it.key().size() < 32 && out.size() < 1000) {
                    const double v = it.value().get<double>();
                    if (v >= lo && v <= hi) out[it.key()] = v;
                }
    };
    readMap("refZoom", refZoom, 0.5, 3.0);
    readMap("refScroll", refScroll, 0.0, 1e7);
    blockerAllowed.clear();
    if (j.contains("blockerAllowed") && j["blockerAllowed"].is_array())
        for (const auto& s : j["blockerAllowed"]) if (s.is_string() && s.get<std::string>().size() < 256) blockerAllowed.push_back(s.get<std::string>());
    return true;
}

bool Settings::save(const std::wstring& path) const {
    if (readOnly) return false;
    nlohmann::json j = {{"version", kVersion}, {"theme", (int)theme}, {"searchEngine", searchEngine},
                         {"showBookmarksBar", showBookmarksBar}, {"savePasswords", savePasswords}, {"fillForms", fillForms}, {"verticalTabs", verticalTabs},
                         {"searchEngineChosen", searchEngineChosen}, {"blockTrackers", blockTrackers}, {"blockerAllowed", blockerAllowed},
                         {"startBackground", startBackground}, {"showClock", showClock}, {"showNotes", showNotes}, {"weatherPlace", weatherPlace},
                         {"weatherLat", weatherLat}, {"weatherLon", weatherLon}, {"fahrenheit", fahrenheit}, {"defaultOffered", defaultOffered},
                         {"popX", popX}, {"popY", popY}, {"popW", popW}, {"popH", popH},
                         {"restoreTabs", restoreTabs}, {"askBeforeClosing", askBeforeClosing}, {"sessionActive", sessionActive}};
    {
        nlohmann::json a = nlohmann::json::array();
        for (const auto& t : session) a.push_back({{"url", t.url}, {"title", t.title}});
        j["session"] = a;
        nlohmann::json z = nlohmann::json::object();
        for (const auto& [host, f] : siteZoom) z[host] = f;
        j["siteZoom"] = z;
        j["refBoard"] = refBoard;
        nlohmann::json rz = nlohmann::json::object(), rs = nlohmann::json::object();
        for (const auto& [k, v] : refZoom) rz[k] = v;
        for (const auto& [k, v] : refScroll) rs[k] = v;
        j["refZoom"] = rz;
        j["refScroll"] = rs;
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

}  // namespace nrb

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
    view = cj::valueOr(j, "view", 0) == 1 ? ViewMode::Icons : ViewMode::Details;
    sortKey = std::clamp(cj::valueOr(j, "sortKey", 0), 0, 3);
    sortDescending = cj::valueOr(j, "sortDescending", false);
    showHidden = cj::valueOr(j, "showHidden", false);
    showProtected = cj::valueOr(j, "showProtected", false);
    lastFolder = cjui::widen(cj::valueOr(j, "lastFolder", ""));
    return true;
}

bool Settings::save(const std::wstring& path) const {
    if (readOnly) return false;
    nlohmann::json j = {{"version", kVersion},          {"theme", (int)theme},         {"view", (int)view},
                        {"sortKey", sortKey},           {"sortDescending", sortDescending},
                        {"showHidden", showHidden},     {"showProtected", showProtected},
                        {"lastFolder", cjui::narrow(lastFolder)}};
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

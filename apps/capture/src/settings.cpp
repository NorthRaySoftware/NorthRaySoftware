#include "settings.hpp"

#include "cj/json_safe.hpp"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ncap {

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
}  // namespace

bool Settings::load(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    readOnly = cj::valueOr(j, "version", 1) > kVersion;
    const int t = cj::valueOr(j, "theme", 0);
    theme = t == 1 ? Theme::Light : t == 2 ? Theme::Dark : Theme::System;
    lowResource = cj::valueOr(j, "lowResource", false);
    const int pf = cj::valueOr(j, "previewFps", 30);
    previewFps = pf == 0 || pf == 10 || pf == 30 || pf == 60 ? pf : 30;
    const int of = cj::valueOr(j, "outputFps", 60);
    outputFps = of == 24 || of == 25 || of == 30 || of == 50 || of == 60 ? of : 60;
    layout = cj::valueOr(j, "layout", std::string());
    if (j.contains("savedLayouts") && j["savedLayouts"].is_object())
        for (auto& [k, v] : j["savedLayouts"].items())
            if (v.is_string() && !k.empty() && k.size() < 80) savedLayouts[k] = v.get<std::string>();
    const std::string sc = cj::valueOr(j, "sceneCollection", std::string("Default"));
    sceneCollection = sc.empty() || sc.find_first_of("\\/:*?\"<>|") != std::string::npos ? "Default" : sc;
    recordingsFolder = widen(cj::valueOr(j, "recordingsFolder", std::string()));
    welcomed = cj::valueOr(j, "welcomed", false);
    return true;
}

bool Settings::save(const std::wstring& path) const {
    if (readOnly) return false;
    nlohmann::json layouts = nlohmann::json::object();
    for (auto& [k, v] : savedLayouts) layouts[k] = v;
    const nlohmann::json j = {{"version", kVersion},        {"theme", (int)theme},       {"lowResource", lowResource},
                              {"previewFps", previewFps},   {"outputFps", outputFps},    {"layout", layout},
                              {"savedLayouts", layouts},    {"sceneCollection", sceneCollection},
                              {"recordingsFolder", narrow(recordingsFolder)}, {"welcomed", welcomed}};
    const std::wstring tmp = path + L".new";
    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump(2);
        if (!f.good()) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

}  // namespace ncap

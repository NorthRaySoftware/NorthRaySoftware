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
    monitorDevice = widen(cj::valueOr(j, "monitorDevice", std::string("default")));
    if (monitorDevice.empty()) monitorDevice = L"default";
    monitorHeadphones = cj::valueOr(j, "monitorHeadphones", true);
    secondDevice = widen(cj::valueOr(j, "secondDevice", std::string("off")));
    if (secondDevice.empty()) secondDevice = L"off";
    const int sb = cj::valueOr(j, "secondBus", 0);
    secondBus = sb >= 0 && sb <= 7 ? sb : 0;
    recordEncoder = widen(cj::valueOr(j, "recordEncoder", std::string()));
    const int rk = cj::valueOr(j, "recordKbps", 20000);
    recordKbps = rk >= 1000 && rk <= 150000 ? rk : 20000;
    recordTracks = (unsigned)cj::valueOr(j, "recordTracks", 0x0F) & 0x3Fu;
    if (!recordTracks) recordTracks = 1;
    recordMp4 = cj::valueOr(j, "recordMp4", true);
    const int ak = cj::valueOr(j, "audioKbps", 160);
    audioKbps = ak == 96 || ak == 128 || ak == 160 || ak == 192 ? ak : 160;
    auto validService = [](const std::string& v) { return v == "twitch" || v == "youtube" || v == "kick" || v == "custom"; };
    auto validKbps = [](int k) { return k >= 500 && k <= 50000; };
    destinations.clear();
    if (auto it = j.find("streamDestinations"); it != j.end() && it->is_array()) {
        for (auto& d : *it) {
            if (!d.is_object() || destinations.size() >= 6) continue;
            StreamDestination sd;
            sd.id = cj::valueOr(d, "id", std::string());
            if (sd.id.empty() || sd.id.size() > 32) continue;
            bool dup = false;
            for (auto& o : destinations) dup |= o.id == sd.id;
            if (dup) continue;
            sd.service = cj::valueOr(d, "service", std::string("custom"));
            if (!validService(sd.service)) sd.service = "custom";
            sd.url = cj::valueOr(d, "url", std::string());
            const int k = cj::valueOr(d, "kbps", 6000);
            sd.kbps = validKbps(k) ? k : 6000;
            sd.enabled = cj::valueOr(d, "enabled", true);
            const int b = cj::valueOr(d, "bus", 0);
            sd.bus = b >= 0 && b <= 6 ? b : 0;
            destinations.push_back(sd);
        }
    } else if (j.contains("streamService")) {   // 0.6: one destination ("main", whose key is already "stream/main")
        StreamDestination sd;
        sd.service = cj::valueOr(j, "streamService", std::string("twitch"));
        if (!validService(sd.service)) sd.service = "twitch";
        sd.url = cj::valueOr(j, "streamUrl", sd.url);
        const int k = cj::valueOr(j, "streamKbps", 6000);
        sd.kbps = validKbps(k) ? k : 6000;
        destinations.push_back(sd);
    }
    if (destinations.empty()) destinations.push_back(StreamDestination{});
    studioMode = cj::valueOr(j, "studioMode", false);
    if (auto t = j.find("transition"); t != j.end() && t->is_object()) transition = transition::fromJson(t->dump());
    const int rm = cj::valueOr(j, "streamRetryMax", 60);
    streamRetryMax = rm == 15 || rm == 30 || rm == 60 || rm == 120 ? rm : 60;
    return true;
}

bool Settings::save(const std::wstring& path) const {
    if (readOnly) return false;
    nlohmann::json layouts = nlohmann::json::object();
    for (auto& [k, v] : savedLayouts) layouts[k] = v;
    nlohmann::json dests = nlohmann::json::array();
    for (auto& d : destinations) dests.push_back({{"id", d.id}, {"service", d.service}, {"url", d.url}, {"kbps", d.kbps}, {"enabled", d.enabled}, {"bus", d.bus}});
    const nlohmann::json j = {{"version", kVersion},        {"theme", (int)theme},       {"lowResource", lowResource},
                              {"previewFps", previewFps},   {"outputFps", outputFps},    {"layout", layout},
                              {"savedLayouts", layouts},    {"sceneCollection", sceneCollection},
                              {"recordingsFolder", narrow(recordingsFolder)}, {"welcomed", welcomed},
                              {"monitorDevice", narrow(monitorDevice)}, {"monitorHeadphones", monitorHeadphones}, {"secondDevice", narrow(secondDevice)}, {"secondBus", secondBus},
                              {"recordEncoder", narrow(recordEncoder)}, {"recordKbps", recordKbps}, {"recordTracks", recordTracks}, {"recordMp4", recordMp4}, {"audioKbps", audioKbps},
                              {"streamDestinations", dests}, {"streamRetryMax", streamRetryMax},
                              {"studioMode", studioMode}, {"transition", nlohmann::json::parse(transition::toJson(transition))}};
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

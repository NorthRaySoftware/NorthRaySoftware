#include "ncap/automation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace ncap::automation {

using nlohmann::json;

std::string normaliseExe(const std::string& in) {
    std::string s = in;
    const auto cut = s.find_last_of("\\/");
    if (cut != std::string::npos) s = s.substr(cut + 1);
    const auto a = s.find_first_not_of(" \t\r\n\"");
    if (a == std::string::npos) return {};
    s = s.substr(a, s.find_last_not_of(" \t\r\n\"") - a + 1);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    if (s.empty() || s == ".exe") return {};
    if (s.size() < 4 || s.compare(s.size() - 4, 4, ".exe") != 0) s += ".exe";
    return s.size() > 260 ? std::string() : s;
}

std::string describe(const Rule& r) {
    const std::string who = r.label.empty() ? r.exe : r.label;
    return "When " + who + (r.when == When::Opens ? " opens" : " closes") + ", switch to " + r.scene + (r.onlyLive ? ", while live or recording" : "");
}

std::string toJson(const std::vector<Rule>& rules) {
    json a = json::array();
    for (auto& r : rules)
        a.push_back({{"exe", r.exe}, {"label", r.label}, {"when", r.when == When::Opens ? "opens" : "closes"}, {"scene", r.scene}, {"on", r.on},
                     {"onlyLive", r.onlyLive}});
    return a.dump();
}

std::vector<Rule> fromJson(const std::string& text) {
    std::vector<Rule> out;
    const json a = json::parse(text, nullptr, false);
    if (!a.is_array()) return out;
    for (auto& j : a) {
        if (!j.is_object() || out.size() >= kMaxRules) continue;
        auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
        Rule r;
        r.exe = normaliseExe(str("exe"));
        r.label = str("label").substr(0, 80);
        r.scene = str("scene");
        if (r.exe.empty() || r.scene.empty()) continue;
        const std::string w = str("when");
        if (w != "opens" && w != "closes") continue;
        r.when = w == "opens" ? When::Opens : When::Closes;
        if (j.contains("on") && j["on"].is_boolean()) r.on = j["on"].get<bool>();
        if (j.contains("onlyLive") && j["onlyLive"].is_boolean()) r.onlyLive = j["onlyLive"].get<bool>();
        out.push_back(r);
    }
    return out;
}

int Watcher::update(const std::set<std::string>& running, const std::vector<Rule>& rules, bool live) {
    if (!primed_) {
        primed_ = true;
        last_ = running;
        return -1;
    }
    int fired = -1;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const Rule& r = rules[i];
        if (!r.on || (r.onlyLive && !live)) continue;
        const bool now = running.count(r.exe) > 0, before = last_.count(r.exe) > 0;
        if ((r.when == When::Opens && now && !before) || (r.when == When::Closes && !now && before)) fired = (int)i;
    }
    last_ = running;
    return fired;
}

}  // namespace ncap::automation

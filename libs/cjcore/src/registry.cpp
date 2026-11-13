#include "cj/registry.hpp"

#include <cstdio>
#include <nlohmann/json.hpp>

namespace cj {

using nlohmann::json;

const char* appStatusLabel(AppStatus s) {
    switch (s) {
        case AppStatus::InDevelopment: return "In development";
        case AppStatus::Preview: return "Preview";
        case AppStatus::Available: return "Available";
        case AppStatus::Retired: return "Retired";
    }
    return "";
}

bool isValidAppId(const std::string& id) {
    if (id.empty() || id.size() > 48 || id.front() == '-' || id.back() == '-') return false;
    char prev = 0;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok || (c == '-' && prev == '-')) return false;
        prev = c;
    }
    return true;
}

static std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

std::optional<AppEntry> parseAppEntry(const std::string& text, std::string& error) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { error = "not valid JSON"; return std::nullopt; }
    AppEntry a;
    a.schema = j.value("schema", 1);
    if (a.schema != 1) { error = "unsupported schema version " + std::to_string(a.schema); return std::nullopt; }
    a.id = str(j, "id");
    if (!isValidAppId(a.id)) { error = "missing or invalid id"; return std::nullopt; }
    a.name = str(j, "name");
    if (a.name.empty()) { error = "missing name"; return std::nullopt; }
    a.tagline = str(j, "tagline");
    a.description = str(j, "description");
    for (const json& f : j.value("features", json::array()))
        if (f.is_string()) a.features.push_back(f.get<std::string>());
    a.category = str(j, "category");
    const std::string st = str(j, "status");
    a.status = st == "available" ? AppStatus::Available
             : st == "preview"   ? AppStatus::Preview
             : st == "retired"   ? AppStatus::Retired
                                 : AppStatus::InDevelopment;
    a.version = str(j, "version");
    a.accent = str(j, "accent");
    if (j.contains("artwork") && j["artwork"].is_object()) {
        a.artworkStyle = str(j["artwork"], "fallbackStyle");
        a.artworkFile = str(j["artwork"], "banner");
        a.iconFile = str(j["artwork"], "icon");
        for (const json& s : j["artwork"].value("screenshots", json::array()))
            if (s.is_string()) a.screenshots.push_back(s.get<std::string>());
    }
    if (j.contains("credits") && j["credits"].is_object()) {
        a.developer = str(j["credits"], "developer");
        a.publisher = str(j["credits"], "publisher");
        a.associatedCreator = str(j["credits"], "associatedCreator");
    }
    if (j.contains("launch") && j["launch"].is_object()) {
        a.executable = str(j["launch"], "executable");
        a.canOpenInsideLauncher = j["launch"].value("canOpenInsideLauncher", false);
        // An executable path must stay inside the app's own install folder.
        if (a.executable.find("..") != std::string::npos || a.executable.find(':') != std::string::npos ||
            (!a.executable.empty() && (a.executable[0] == '/' || a.executable[0] == '\\'))) {
            error = "launch.executable must be a relative path inside the app folder";
            return std::nullopt;
        }
    }
    return a;
}

static std::optional<UtcSeconds> resolveWhen(const json& j, UtcSeconds now) {
    if (j.contains("postedUtc")) return parseUtcIso(j["postedUtc"].get<std::string>());
    if (j.contains("startUk")) {
        auto c = parseLocalIso(j["startUk"].get<std::string>());
        if (c) return ukToUtc(*c);
    }
    // Demonstration data: {"relative": {"daysFromToday": -3, "ukTime": "18:30"}}
    if (j.contains("relative")) {
        const json& r = j["relative"];
        int hh = 12, mm = 0;
        std::sscanf(r.value("ukTime", "12:00").c_str(), "%d:%d", &hh, &mm);
        CivilDateTime today = utcToUk(now);
        CivilDateTime d = civilFromUtc(utcFromCivil({today.year, today.month, today.day, 0, 0, 0}) +
                                       static_cast<UtcSeconds>(r.value("daysFromToday", 0)) * 86400);
        if (r.contains("weekday")) {
            const int wd = weekday(d.year, d.month, d.day);
            d = civilFromUtc(utcFromCivil(d) + ((r["weekday"].get<int>() - wd + 7) % 7) * 86400);
        }
        d.hour = hh;
        d.minute = mm;
        return ukToUtc(d);
    }
    return std::nullopt;
}

std::vector<Announcement> parseAnnouncements(const std::string& text, UtcSeconds now, LoadResult& r) {
    std::vector<Announcement> out;
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) { r.warnings.push_back("Announcements file isn't valid JSON."); return out; }
    const bool demo = doc.value("demo", false);
    for (const json& j : doc.value("announcements", json::array())) {
        Announcement a;
        a.id = str(j, "id");
        a.title = str(j, "title");
        a.body = str(j, "body");
        a.author = str(j, "author");
        a.demo = demo || j.value("demo", false);
        auto t = resolveWhen(j, now);
        if (!t) { r.warnings.push_back("Announcement '" + a.id + "' has no valid date."); continue; }
        a.postedUtc = *t;
        out.push_back(a);
    }
    return out;
}

std::vector<CommunityEvent> parseEvents(const std::string& text, UtcSeconds now, LoadResult& r) {
    std::vector<CommunityEvent> out;
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) { r.warnings.push_back("Events file isn't valid JSON."); return out; }
    const bool demo = doc.value("demo", false);
    for (const json& j : doc.value("events", json::array())) {
        CommunityEvent e;
        e.id = str(j, "id");
        e.title = str(j, "title");
        e.description = str(j, "description");
        e.sourceApp = j.value("source", "community");
        e.durationMinutes = j.value("durationMinutes", 60);
        e.cancelled = j.value("cancelled", false);
        e.demo = demo || j.value("demo", false);
        auto t = resolveWhen(j, now);
        if (!t) { r.warnings.push_back("Event '" + e.id + "' has no valid date."); continue; }
        e.startUtc = *t;
        out.push_back(e);
    }
    return out;
}

std::optional<Profile> parseProfile(const std::string& text, LoadResult& r) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { r.warnings.push_back("Profile file isn't valid JSON."); return std::nullopt; }
    Profile p;
    p.displayName = str(j, "displayName");
    p.username = str(j, "username");
    p.bio = str(j, "bio");
    for (const json& i : j.value("interests", json::array()))
        if (i.is_string()) p.interests.push_back(i.get<std::string>());
    // A file on disk can never claim to be a real online account.
    p.localDemo = true;
    return p;
}

}  // namespace cj

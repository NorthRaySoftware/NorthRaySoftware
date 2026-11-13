#include "cj/station.hpp"
#include "cj/ical.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <nlohmann/json.hpp>

namespace cj::station {

using nlohmann::json;

static std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

static bool isHttpUrl(const std::string& u) {
    return u.rfind("https://", 0) == 0 || u.rfind("http://", 0) == 0;
}

Config parseConfig(const std::string& text, std::vector<std::string>& warnings) {
    Config c;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        warnings.push_back("station.json isn't valid JSON.");
        return c;
    }
    c.name = j.value("name", c.name);
    c.nowPlayingUrl = str(j, "nowPlayingUrl");
    c.streamUrl = str(j, "streamUrl");
    c.recordingsUrl = str(j, "recordingsUrl");
    c.recordingsGithub = githubRepo(str(j, "recordingsGithub"));
    if (!str(j, "recordingsGithub").empty() && c.recordingsGithub.empty())
        warnings.push_back("station.json: recordingsGithub should be \"owner/repository\"; ignored.");
    c.twitchUrl = str(j, "twitchUrl");
    c.youtubeUrl = str(j, "youtubeUrl");
    c.embedTwitchPlayer = j.value("embedTwitchPlayer", true);
    c.pollSeconds = std::clamp(j.value("pollSeconds", 15), 5, 300);
    c.demo = j.value("demo", false);
    for (std::string* u : {&c.nowPlayingUrl, &c.streamUrl, &c.twitchUrl, &c.youtubeUrl}) {
        if (!u->empty() && !isHttpUrl(*u)) {
            warnings.push_back("station.json: '" + *u + "' isn't an http(s) address; ignored.");
            u->clear();
        }
    }
    if (j.contains("calendar") && j["calendar"].is_object()) {
        const json& cal = j["calendar"];
        const std::string ics = str(cal, "icsUrl"), sub = str(cal, "subscribeUrl");
        c.calendarUrl = ical::normaliseUrl(ics);
        if (!ics.empty() && c.calendarUrl.empty())
            warnings.push_back("station.json: the calendar link must be an https:// or webcal:// address; ignored.");
        if (!sub.empty() && sub.rfind("https://", 0) == 0) c.calendarSubscribeUrl = sub;
    }
    if (j.contains("callIn") && j["callIn"].is_object()) {
        const json& ci = j["callIn"];
        c.callIn.enabled = ci.value("enabled", false);
        c.callIn.guestUrl = str(ci, "guestUrl");
        c.callIn.notice = str(ci, "notice");
        c.callIn.minimumAge = ci.value("minimumAge", 16);
        if (!c.callIn.guestUrl.empty() && c.callIn.guestUrl.rfind("https://", 0) != 0) {
            warnings.push_back("station.json: call-in link must start with https://; call-ins disabled.");
            c.callIn.guestUrl.clear();
        }
        if (c.callIn.guestUrl.empty()) c.callIn.enabled = false;
    }
    return c;
}

NowPlaying parseAzuraCastNowPlaying(const std::string& text) {
    NowPlaying np;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) { np.error = "The server's reply wasn't valid JSON."; return np; }
    if (j.is_array()) {
        if (j.empty()) { np.error = "The server lists no stations."; return np; }
        j = j[0];
    }
    if (!j.is_object() || !j.contains("live") || !j["live"].is_object()) {
        np.error = "The server's reply doesn't look like AzuraCast now-playing data.";
        return np;
    }
    np.reachable = true;
    np.stationOnline = j.value("is_online", false);
    const json& live = j["live"];
    np.presenterLive = live.value("is_live", false);
    np.presenter = str(live, "streamer_name");
    if (live.contains("broadcast_start") && live["broadcast_start"].is_number_integer())
        np.liveSinceUtc = live["broadcast_start"].get<long long>();
    if (j.contains("listeners") && j["listeners"].is_object()) {
        const json& l = j["listeners"];
        if (l.contains("current") && l["current"].is_number_integer()) np.listeners = l["current"].get<int>();
        else if (l.contains("total") && l["total"].is_number_integer()) np.listeners = l["total"].get<int>();
    }
    if (j.contains("now_playing") && j["now_playing"].is_object()) {
        const json& n = j["now_playing"];
        if (n.contains("song") && n["song"].is_object()) {
            np.trackText = str(n["song"], "text");
            if (np.trackText.empty()) np.trackText = str(n["song"], "title");
        }
    }
    return np;
}

radio::LiveSignal toLiveSignal(const NowPlaying& np) {
    radio::LiveSignal s;
    s.connected = np.reachable;
    s.onAir = np.reachable && np.presenterLive && np.stationOnline;
    if (s.onAir) {
        s.title = np.trackText.empty() ? std::string{} : np.trackText;
        s.startedUtc = np.liveSinceUtc;
        s.listeners = np.listeners;
    } else {
        s.listeners = np.reachable ? np.listeners : -1;
    }
    return s;
}

CallInService callInService(const std::string& url) {
    for (const char* prefix : {"https://discord.gg/", "https://discord.com/invite/", "https://www.discord.com/invite/",
                               "https://discordapp.com/invite/", "https://discord.com/channels/"})
        if (url.rfind(prefix, 0) == 0 && url.size() > std::string(prefix).size()) return CallInService::Discord;
    return CallInService::Other;
}

std::string twitchChannel(const std::string& url) {
    std::string rest;
    for (const char* prefix : {"https://www.twitch.tv/", "https://twitch.tv/", "https://m.twitch.tv/"}) {
        if (url.rfind(prefix, 0) == 0) { rest = url.substr(std::string(prefix).size()); break; }
    }
    const size_t end = rest.find_first_of("/?#");
    std::string login = rest.substr(0, end);
    if (login.empty() || login.size() > 25) return {};
    for (char& ch : login) {
        if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return {};
    }
    // Twitch's own pages, not channels.
    for (const char* reserved : {"directory", "videos", "settings", "search", "downloads", "p", "jobs", "turbo"})
        if (login == reserved) return {};
    return login;
}

radio::LiveSignal combineLive(const NowPlaying& np, const TwitchStatus& twitch) {
    radio::LiveSignal s = toLiveSignal(np);
    if (s.onAir || !twitch.known) return s;
    if (twitch.online) {
        s.connected = true;
        s.onAir = true;
        s.startedUtc = twitch.sinceUtc;
        s.listeners = -1;  // Twitch viewers aren't CJ Radio listeners
    }
    return s;
}

std::vector<Recording> parseRecordings(const std::string& text, std::vector<std::string>& warnings) {
    std::vector<Recording> out;
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        warnings.push_back("Recordings index isn't valid JSON.");
        return out;
    }
    for (const json& j : doc.value("recordings", json::array())) {
        Recording r;
        r.id = str(j, "id");
        r.file = str(j, "file");
        r.title = str(j, "title");
        r.description = str(j, "description");
        r.presenter = str(j, "presenter");
        r.category = str(j, "category");
        r.durationMinutes = j.value("durationMinutes", 0);
        r.url = str(j, "url");
        r.transcriptUrl = str(j, "transcriptUrl");
        auto t = parseUtcIso(str(j, "recordedUtc"));
        if (r.id.empty() || r.title.empty() || !t || !isHttpUrl(r.url)) {
            warnings.push_back("Recording '" + r.id + "' is missing a title, date or https link; skipped.");
            continue;
        }
        r.recordedUtc = *t;
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.recordedUtc > b.recordedUtc; });
    return out;
}

// ------------------------------------------------------- GitHub recordings

static bool repoChar(char c) { return std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.'; }

std::string githubRepo(const std::string& in) {
    std::string t = in;
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(0, 1);
    for (const char* prefix : {"https://github.com/", "http://github.com/", "https://www.github.com/", "github.com/"})
        if (t.rfind(prefix, 0) == 0) { t = t.substr(std::strlen(prefix)); break; }
    while (!t.empty() && t.back() == '/') t.pop_back();
    if (t.size() > 4 && t.compare(t.size() - 4, 4, ".git") == 0) t = t.substr(0, t.size() - 4);
    const size_t slash = t.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= t.size() || t.find('/', slash + 1) != std::string::npos) return {};
    for (char c : t) if (c != '/' && !repoChar(c)) return {};
    for (const std::string& part : {t.substr(0, slash), t.substr(slash + 1)})
        if (part == "." || part == "..") return {};
    return t;
}

static bool endsWithMp3(const std::string& n) {
    if (n.size() < 5) return false;
    std::string e = n.substr(n.size() - 4);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".mp3";
}

Recording recordingFromFileName(const std::string& fileName, const std::string& url, UtcSeconds fallbackUtc) {
    Recording r;
    r.file = fileName;
    r.id = "file:" + fileName;
    r.url = url;
    std::string stem = endsWithMp3(fileName) ? fileName.substr(0, fileName.size() - 4) : fileName;
    r.recordedUtc = fallbackUtc;
    if (stem.size() >= 10 && parseLocalIso(stem.substr(0, 10) + "T12:00")) {
        r.recordedUtc = *parseUtcIso(stem.substr(0, 10) + "T12:00:00Z");
        stem = stem.substr(10);
        while (!stem.empty() && (stem[0] == ' ' || stem[0] == '_' || stem[0] == '-' || stem[0] == '.')) stem.erase(0, 1);
    }
    for (char& c : stem) if (c == '_') c = ' ';
    r.title = stem.empty() ? "The CJ Philip Show" : stem;
    return r;
}

GithubListing parseGithubContents(const std::string& text) {
    GithubListing out;
    json j = json::parse(text, nullptr, false);
    if (!j.is_array()) return out;
    for (const json& e : j) {
        if (!e.is_object()) continue;
        const std::string name = str(e, "name"), type = str(e, "type"), dl = str(e, "download_url"), path = str(e, "path");
        if (type == "dir") {
            std::string l = name;
            for (char& c : l) c = (char)std::tolower((unsigned char)c);
            if (l == "recordings" || l == "episodes" || l == "audio" || l == "shows") out.folders.push_back(path);
            continue;
        }
        if (type != "file" || dl.rfind("https://", 0) != 0) continue;
        if (name == "recordings.json") { out.indexUrl = dl; continue; }
        if (endsWithMp3(name)) out.recordings.push_back(recordingFromFileName(name, dl, 0));
    }
    return out;
}

std::vector<Recording> parseGithubReleases(const std::string& text) {
    std::vector<Recording> out;
    json j = json::parse(text, nullptr, false);
    if (!j.is_array()) return out;
    for (const json& rel : j) {
        if (!rel.is_object() || rel.value("draft", false)) continue;
        const UtcSeconds published = parseUtcIso(str(rel, "published_at")).value_or(0);
        const std::string relName = str(rel, "name").empty() ? str(rel, "tag_name") : str(rel, "name");
        std::string body = str(rel, "body");
        if (body.size() > 800) body = body.substr(0, 800) + "...";
        std::vector<const json*> mp3s;
        static const json kNone = json::array();
        const json& assets = rel.contains("assets") && rel["assets"].is_array() ? rel["assets"] : kNone;
        for (const json& a : assets)
            if (a.is_object() && endsWithMp3(str(a, "name")) && str(a, "browser_download_url").rfind("https://", 0) == 0) mp3s.push_back(&a);
        for (const json* a : mp3s) {
            Recording r = recordingFromFileName(str(*a, "name"), str(*a, "browser_download_url"), published);
            // One recording per release: the release's own name and notes describe it.
            if (mp3s.size() == 1 && !relName.empty()) {
                Recording named = recordingFromFileName(relName + ".mp3", r.url, r.recordedUtc);
                r.title = named.title;
                if (named.recordedUtc != r.recordedUtc && named.recordedUtc != published) r.recordedUtc = named.recordedUtc;
            }
            r.description = body;
            r.id = "release:" + str(rel, "tag_name") + "/" + r.file;
            out.push_back(r);
        }
    }
    return out;
}

void applyRecordingDetails(std::vector<Recording>& recs, const std::string& indexJson) {
    json j = json::parse(indexJson, nullptr, false);
    if (!j.is_object() || !j.contains("recordings") || !j["recordings"].is_array()) return;
    for (const json& e : j["recordings"]) {
        if (!e.is_object()) continue;
        const std::string file = str(e, "file");
        for (Recording& r : recs) {
            if (file.empty() || r.file != file) continue;
            if (!str(e, "title").empty()) r.title = str(e, "title");
            if (!str(e, "description").empty()) r.description = str(e, "description");
            if (!str(e, "presenter").empty()) r.presenter = str(e, "presenter");
            if (!str(e, "category").empty()) r.category = str(e, "category");
            r.durationMinutes = std::clamp(e.value("durationMinutes", r.durationMinutes), 0, 24 * 60);
            const std::string date = str(e, "date");
            if (date.size() >= 10 && parseLocalIso(date.substr(0, 10) + "T12:00")) r.recordedUtc = *parseUtcIso(date.substr(0, 10) + "T12:00:00Z");
        }
    }
}

std::string recordingsToJson(const std::vector<Recording>& recs) {
    json out = {{"recordings", json::array()}};
    for (const Recording& r : recs)
        out["recordings"].push_back({{"id", r.id}, {"file", r.file}, {"title", r.title}, {"description", r.description},
                                     {"presenter", r.presenter}, {"category", r.category}, {"durationMinutes", r.durationMinutes},
                                     {"url", r.url}, {"transcriptUrl", r.transcriptUrl}, {"recordedUtc", formatUtcIso(r.recordedUtc)}});
    return out.dump(2);
}

}  // namespace cj::station

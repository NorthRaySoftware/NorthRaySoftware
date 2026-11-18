#include "ncap/live.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ncap::live {

using nlohmann::json;

namespace {
// Days from 1970-01-01 (Howard Hinnant's days_from_civil).
std::int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (std::int64_t)doe - 719468;
}
void civilFromDays(std::int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yoe + era * 400) + (m <= 2);
}
int daysIn(int y, int m) {
    static const int n[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : n[(m - 1) % 12];
}
}  // namespace

std::int64_t civilMs(int year, int month, int day, int hour, int minute, int second, int ms) {
    return ((daysFromCivil(year, month, day) * 24 + hour) * 60 + minute) * 60000LL + second * 1000LL + ms;
}

int weekday(int year, int month, int day) {
    const std::int64_t z = daysFromCivil(year, month, day);
    return (int)(((z % 7) + 7 + 4) % 7);
}

bool parseStamp(const std::string& s, Stamp& out) {
    Stamp t;
    char sep = 0;
    int used = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2d%c%2d:%2d%n", &t.year, &t.month, &t.day, &sep, &t.hour, &t.minute, &used) != 6) return false;
    if ((std::size_t)used != s.size() || (sep != 'T' && sep != ' ')) return false;
    if (t.year < 2000 || t.year > 2200 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > daysIn(t.year, t.month) || t.hour < 0 || t.hour > 23 ||
        t.minute < 0 || t.minute > 59)
        return false;
    out = t;
    return true;
}

std::string stampText(const Stamp& s) {
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d", s.year, s.month, s.day, s.hour, s.minute);
    return b;
}

std::string stampWords(const Stamp& s) {
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char b[48];
    std::snprintf(b, sizeof b, "%s %d %s, %02d:%02d", days[weekday(s.year, s.month, s.day)], s.day, months[(s.month + 11) % 12], s.hour, s.minute);
    return b;
}

Stamp addMinutes(const Stamp& s, int minutes) {
    const std::int64_t total = (daysFromCivil(s.year, s.month, s.day) * 24 + s.hour) * 60 + s.minute + minutes;
    const std::int64_t days = total >= 0 ? total / 1440 : -((-total + 1439) / 1440);
    const std::int64_t rest = total - days * 1440;
    Stamp r;
    civilFromDays(days, r.year, r.month, r.day);
    r.hour = (int)(rest / 60);
    r.minute = (int)(rest % 60);
    return r;
}

std::string durationWords(double seconds) {
    const long long s = std::max(0LL, (long long)std::llround(seconds));
    char b[48];
    if (s >= 3600) std::snprintf(b, sizeof b, "%lld h %lld min", s / 3600, s / 60 % 60);
    else if (s >= 60) std::snprintf(b, sizeof b, "%lld min %lld s", s / 60, s % 60);
    else std::snprintf(b, sizeof b, "%lld s", s);
    return b;
}

// --- Collector ---

void Collector::begin(const Stamp& started, const std::string& scene) {
    r_ = Report{};
    r_.started = stampText(started);
    running_ = true;
    scene_.clear();
    bucketSum_ = 0;
    bucketN_ = 0;
    bucketStart_ = 0;
    this->scene(0, scene);
}

void Collector::sample(double t, double kbps, std::uint64_t droppedSoFar) {
    if (!running_) return;
    r_.dropped = std::max(r_.dropped, droppedSoFar);
    r_.seconds = std::max(r_.seconds, t);
    bucketSum_ += std::max(0.0, kbps);
    ++bucketN_;
    if (t - bucketStart_ >= r_.sampleSeconds) {
        if (r_.kbps.size() < kMaxSamples) r_.kbps.push_back((float)(bucketSum_ / bucketN_));
        bucketSum_ = 0;
        bucketN_ = 0;
        bucketStart_ = t;
    }
}

void Collector::closeScene(double t) {
    if (scene_.empty()) return;
    const double d = std::max(0.0, t - sceneSince_);
    for (auto& [name, secs] : r_.scenes)
        if (name == scene_) {
            secs += d;
            return;
        }
    r_.scenes.push_back({scene_, d});
}

void Collector::scene(double t, const std::string& name) {
    if (!running_ || name == scene_) return;
    closeScene(t);
    scene_ = name;
    sceneSince_ = t;
    if (name.empty()) return;
    if (r_.changes.size() < kMaxChanges) r_.changes.push_back({t, name});
    bool known = false;
    for (auto& s : r_.scenes) known = known || s.first == name;
    if (!known) r_.scenes.push_back({name, 0});
}

void Collector::destination(const Report::Destination& d) {
    for (auto& x : r_.destinations)
        if (x.name == d.name) {
            // A destination stopped and started again: the figures add up.
            const double total = x.seconds + d.seconds;
            x.averageKbps = total > 0 ? (x.averageKbps * x.seconds + d.averageKbps * d.seconds) / total : 0;
            x.seconds = total;
            x.dropped += d.dropped;
            x.reconnects += d.reconnects;
            return;
        }
    r_.destinations.push_back(d);
}

Report Collector::finish(double t) {
    if (!running_) return r_;
    r_.seconds = std::max(r_.seconds, t);
    if (bucketN_ && r_.kbps.size() < kMaxSamples) r_.kbps.push_back((float)(bucketSum_ / bucketN_));
    closeScene(r_.seconds);
    scene_.clear();
    running_ = false;
    return r_;
}

// --- JSON ---

std::string toJson(const Report& r) {
    json j;
    j["started"] = r.started;
    j["seconds"] = std::round(r.seconds * 10) / 10;
    j["sampleSeconds"] = r.sampleSeconds;
    json k = json::array();
    for (float v : r.kbps) k.push_back(std::lround(v));
    j["kbps"] = k;
    j["dropped"] = r.dropped;
    j["recorded"] = r.recorded;
    json ds = json::array();
    for (auto& d : r.destinations)
        ds.push_back({{"name", d.name}, {"seconds", std::round(d.seconds)}, {"averageKbps", std::lround(d.averageKbps)}, {"dropped", d.dropped},
                      {"reconnects", d.reconnects}});
    j["destinations"] = ds;
    json sc = json::array();
    for (auto& [n, s] : r.scenes) sc.push_back({{"name", n}, {"seconds", std::round(s)}});
    j["scenes"] = sc;
    json ch = json::array();
    for (auto& [t, n] : r.changes) ch.push_back({{"at", std::round(t)}, {"scene", n}});
    j["changes"] = ch;
    return j.dump();
}

bool fromJson(const std::string& text, Report& out) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("started") || !j["started"].is_string()) return false;
    Report r;
    auto num = [](const json& o, const char* k, double def = 0) { return o.contains(k) && o[k].is_number() ? o[k].get<double>() : def; };
    auto str = [](const json& o, const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); };
    r.started = j["started"].get<std::string>();
    Stamp check;
    if (!parseStamp(r.started, check)) return false;
    r.seconds = std::max(0.0, num(j, "seconds"));
    r.sampleSeconds = std::clamp((int)num(j, "sampleSeconds", 5), 1, 600);
    r.dropped = (std::uint64_t)std::max(0.0, num(j, "dropped"));
    r.recorded = j.contains("recorded") && j["recorded"].is_boolean() && j["recorded"].get<bool>();
    if (j.contains("kbps") && j["kbps"].is_array())
        for (auto& v : j["kbps"])
            if (v.is_number() && r.kbps.size() < Collector::kMaxSamples) r.kbps.push_back(std::max(0.0f, v.get<float>()));
    if (j.contains("destinations") && j["destinations"].is_array())
        for (auto& d : j["destinations"])
            if (d.is_object() && r.destinations.size() < 32)
                r.destinations.push_back({str(d, "name"), std::max(0.0, num(d, "seconds")), std::max(0.0, num(d, "averageKbps")),
                                          (std::uint64_t)std::max(0.0, num(d, "dropped")), (int)std::max(0.0, num(d, "reconnects"))});
    if (j.contains("scenes") && j["scenes"].is_array())
        for (auto& s : j["scenes"])
            if (s.is_object() && r.scenes.size() < 500) r.scenes.push_back({str(s, "name"), std::max(0.0, num(s, "seconds"))});
    if (j.contains("changes") && j["changes"].is_array())
        for (auto& c : j["changes"])
            if (c.is_object() && r.changes.size() < Collector::kMaxChanges) r.changes.push_back({std::max(0.0, num(c, "at")), str(c, "scene")});
    out = std::move(r);
    return true;
}

}  // namespace ncap::live

#include "ncap/plugins.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace ncap::plugins {

const Record* Registry::find(const std::string& file) const {
    for (auto& r : records)
        if (r.file == file) return &r;
    return nullptr;
}
Record* Registry::find(const std::string& file) {
    for (auto& r : records)
        if (r.file == file) return &r;
    return nullptr;
}

namespace {
bool validHash(const std::string& h) {
    return h.size() == 64 && std::all_of(h.begin(), h.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
}  // namespace

std::string toJson(const Registry& r) {
    nlohmann::json a = nlohmann::json::array();
    for (auto& x : r.records) a.push_back({{"file", x.file}, {"sha256", x.sha256}, {"enabled", x.enabled}, {"offReason", x.offReason}});
    return nlohmann::json{{"format", "northray-capture-plugins"}, {"version", 1}, {"plugins", a}}.dump(2);
}

Registry fromJson(const std::string& text) {
    Registry r;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("plugins") || !j["plugins"].is_array()) return r;
    for (auto& e : j["plugins"]) {
        if (!e.is_object()) continue;
        Record x;
        x.file = e.value("file", std::string());
        x.sha256 = lower(e.value("sha256", std::string()));
        x.enabled = e.contains("enabled") && e["enabled"].is_boolean() && e["enabled"].get<bool>();
        x.offReason = e.value("offReason", std::string());
        if (!validFileName(x.file) || r.find(x.file)) continue;
        if (!validHash(x.sha256)) x.enabled = false, x.sha256.clear();
        r.records.push_back(x);
    }
    return r;
}

bool validFileName(const std::string& f) {
    if (f.empty() || f.size() > 128) return false;
    if (f.find_first_of("/\\:*?\"<>|") != std::string::npos) return false;
    for (unsigned char c : f)
        if (c < 32) return false;
    if (f.front() == '.' || f.front() == ' ' || f.back() == ' ') return false;
    const std::string l = lower(f);
    if (l.size() < 5 || l.compare(l.size() - 4, 4, ".dll") != 0) return false;
    // Windows' reserved device names, with or without an extension.
    const std::string stem = l.substr(0, l.find('.'));
    static const std::set<std::string> reserved = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8",
                                                   "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    return !reserved.count(stem);
}

std::string hex(const std::uint8_t* b, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += d[b[i] >> 4];
        s += d[b[i] & 15];
    }
    return s;
}

std::vector<std::string> reconcile(Registry& r, const std::vector<Found>& found) {
    std::vector<std::string> out;
    for (auto& x : r.records) {
        if (!x.enabled) continue;
        auto it = std::find_if(found.begin(), found.end(), [&](const Found& f) { return lower(f.file) == lower(x.file); });
        if (it == found.end()) {
            x.enabled = false;
            x.offReason = "Its file is no longer in the plugins folder.";
        } else if (lower(it->sha256) != x.sha256) {
            x.enabled = false;
            x.offReason = "Its file has changed since you switched it on, so it's off until you check it and switch it on again.";
        } else {
            continue;
        }
        out.push_back(x.file + ": " + x.offReason);
    }
    return out;
}

void enable(Registry& r, const Found& f) {
    Record* x = r.find(f.file);
    if (!x) {
        r.records.push_back({});
        x = &r.records.back();
        x->file = f.file;
    }
    x->sha256 = lower(f.sha256);
    x->enabled = validHash(x->sha256);
    x->offReason.clear();
}

void disable(Registry& r, const std::string& file, const std::string& reason) {
    if (Record* x = r.find(file)) {
        x->enabled = false;
        x->offReason = reason;
    }
}

bool validate(const Info& i, std::string& why) {
    if (i.api != 1) {
        why = "It was made for version " + std::to_string(i.api) + " of the plugin interface; this Capture understands version 1.";
        return false;
    }
    if (!i.hasCreate || !i.hasDestroy || !i.hasSetParam || !i.hasProcess) {
        why = "It doesn't provide everything a plugin needs (create, destroy, set_param and process).";
        return false;
    }
    if (i.name.empty() || i.name.size() > 64) {
        why = "Its name is missing or too long.";
        return false;
    }
    if (i.params.size() > 16) {
        why = "It has more than 16 values to adjust.";
        return false;
    }
    std::set<std::string> keys;
    for (auto& p : i.params) {
        if (p.key.empty() || p.key.size() > 32 || !keys.insert(p.key).second) {
            why = "Its values' keys are missing, too long or repeated.";
            return false;
        }
        if (!std::isfinite(p.min) || !std::isfinite(p.max) || !std::isfinite(p.def) || !(p.min < p.max) || p.def < p.min || p.def > p.max) {
            why = "The range of its value \"" + p.key + "\" doesn't make sense.";
            return false;
        }
    }
    return true;
}

std::string runningToText(const std::vector<std::string>& files) {
    std::string s;
    for (auto& f : files) s += f + "\n";
    return s;
}

std::vector<std::string> runningFromText(const std::string& text) {
    std::vector<std::string> v;
    std::size_t a = 0;
    while (a < text.size()) {
        std::size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string f = text.substr(a, b - a);
        if (!f.empty() && f.back() == '\r') f.pop_back();
        if (validFileName(f)) v.push_back(f);
        a = b + 1;
    }
    return v;
}

std::vector<std::string> disableAfterCrash(Registry& r, const std::vector<std::string>& wereRunning) {
    std::vector<std::string> out;
    for (auto& f : wereRunning)
        if (Record* x = r.find(f); x && x->enabled) {
            x->enabled = false;
            x->offReason = "Capture closed unexpectedly while it was running, so it's off in case it was the cause.";
            out.push_back(f + ": " + x->offReason);
        }
    return out;
}

}  // namespace ncap::plugins

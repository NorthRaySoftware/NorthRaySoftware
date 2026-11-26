#include "ncap/diag.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>

namespace ncap {

namespace {

const char* kMask = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";   // •••

bool keyChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '~' || c == '%' || c == '+' || c == '/' || c == '='; }

void replaceAll(std::string& s, const std::string& what, const std::string& with) {
    if (what.empty()) return;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + with.size())) s.replace(at, what.size(), with);
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

}  // namespace

std::string redact(const std::string& textIn, const std::vector<std::string>& secrets) {
    std::string s = textIn;
    // Known secrets first (longest first, so one inside another goes whole).
    std::vector<std::string> sorted = secrets;
    std::sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const auto& k : sorted)
        if (k.size() >= 4) replaceAll(s, k, kMask);

    // rtmp(s)://host/app/KEY: the part after the application name is the stream key.
    for (const char* scheme : {"rtmp://", "rtmps://"}) {
        for (size_t at = lower(s).find(scheme); at != std::string::npos; at = lower(s).find(scheme, at + 1)) {
            size_t end = at + std::string(scheme).size();
            while (end < s.size() && !std::isspace((unsigned char)s[end]) && s[end] != '"' && s[end] != '\'') ++end;
            const std::string url = s.substr(at, end - at);
            const size_t hostEnd = url.find('/', std::string(scheme).size());
            if (hostEnd == std::string::npos) continue;
            const size_t appEnd = url.find('/', hostEnd + 1);
            if (appEnd == std::string::npos || appEnd + 1 >= url.size()) continue;
            s.replace(at + appEnd + 1, url.size() - appEnd - 1, kMask);
        }
    }

    // ?key=... &token=... and the like, in addresses or text.
    const char* names[] = {"key", "streamkey", "stream_key", "token", "access_token", "refresh_token", "auth", "password", "passwd", "pwd", "secret", "sig", "signature"};
    std::string low = lower(s);
    for (const char* n : names) {
        const std::string pat = std::string(n) + "=";
        for (size_t at = low.find(pat); at != std::string::npos; at = low.find(pat, at + 1)) {
            if (at > 0 && (std::isalnum((unsigned char)low[at - 1]) || low[at - 1] == '_')) continue;   // "apikey=" counts as "key"? no: whole names only
            size_t v = at + pat.size(), e = v;
            while (e < s.size() && s[e] != '&' && !std::isspace((unsigned char)s[e]) && s[e] != '"' && s[e] != '\'' && s[e] != ',') ++e;
            if (e == v || s.compare(v, e - v, kMask) == 0) continue;
            s.replace(v, e - v, kMask);
            low = lower(s);
        }
    }

    // "Bearer xyz" and key-shaped runs ("live_1234_abcd...", "sk_...").
    for (const std::string& pre : {std::string("Bearer "), std::string("bearer ")}) {
        for (size_t at = s.find(pre); at != std::string::npos; at = s.find(pre, at + 1)) {
            size_t v = at + pre.size(), e = v;
            while (e < s.size() && keyChar(s[e])) ++e;
            if (e > v) s.replace(v, e - v, kMask);
        }
    }
    for (const std::string& pre : {std::string("sk_"), std::string("live_")}) {
        for (size_t at = s.find(pre); at != std::string::npos; at = s.find(pre, at + 1)) {
            if (at > 0 && (std::isalnum((unsigned char)s[at - 1]) || s[at - 1] == '_')) continue;   // "sk_live_..." goes whole as sk_
            size_t e = at + pre.size();
            while (e < s.size() && keyChar(s[e])) ++e;
            if (e - at - pre.size() >= 12) s.replace(at, e - at, kMask);
        }
    }
    return s;
}

const char* Layout::panelName(int p) {
    switch (p) {
        case Scenes: return "Scenes";
        case Sources: return "Sources";
        case Mixer: return "Audio mixer";
        case Controls: return "Controls";
        case Chat: return "Chat";
        case Soundboard: return "Soundboard";
        case Notes: return "Notes";
        case Writing: return "Writing";
        default: return "";
    }
}

void Layout::normalise() {
    std::vector<int> o;
    for (int p : order)
        if (p >= 0 && p < kPanels && std::find(o.begin(), o.end(), p) == o.end()) o.push_back(p);
    if (o.empty()) o = {Scenes, Sources, Mixer, Controls};
    order = o;
    // Layouts saved before a panel existed keep their widths; the new panel gets a usual width.
    static const float kDefault[kPanels] = {0.2f, 0.25f, 0.35f, 0.2f, 0.22f, 0.22f, 0.25f, 0.2f};
    if (widths.size() > (size_t)kPanels) widths.resize((size_t)kPanels);
    while (widths.size() < (size_t)kPanels) widths.push_back(kDefault[widths.size()]);
    for (auto& w : widths) w = std::clamp(std::isfinite(w) ? w : 0.25f, 0.08f, 1.0f);
    float sum = 0;
    for (int p : order) sum += widths[(size_t)p];
    for (int p : order) widths[(size_t)p] /= sum;
    dockHeight = std::clamp(std::isfinite(dockHeight) ? dockHeight : 0.34f, 0.15f, 0.8f);
}

bool Layout::shows(int p) const { return std::find(order.begin(), order.end(), p) != order.end(); }

void Layout::hide(int p) {
    if (order.size() <= 1) return;   // one panel always stays
    order.erase(std::remove(order.begin(), order.end(), p), order.end());
    normalise();
}

void Layout::show(int p) {
    if (shows(p) || p < 0 || p >= kPanels) return;
    order.push_back(p);
    widths[(size_t)p] = 1.0f / (float)order.size();
    normalise();
}

void Layout::move(int p, int delta) {
    const auto it = std::find(order.begin(), order.end(), p);
    if (it == order.end()) return;
    const long at = it - order.begin(), to = at + delta;
    if (to < 0 || to >= (long)order.size()) return;
    std::swap(order[(size_t)at], order[(size_t)to]);
}

void Layout::resizeDivider(int index, float fraction) {
    if (index < 0 || index + 1 >= (int)order.size()) return;
    float& a = widths[(size_t)order[(size_t)index]];
    float& b = widths[(size_t)order[(size_t)index + 1]];
    const float total = a + b, minW = 0.08f;
    const float na = std::clamp(a + fraction, minW, total - minW);
    a = na;
    b = total - na;
}

Layout::Arrangement Layout::arrange(float w, float h, float gap, float minW) const {
    Arrangement a;
    const size_t n = order.size();
    if (n == 0 || w <= 0 || h <= 0) return a;
    auto widthOf = [&](int p) { return p >= 0 && (size_t)p < widths.size() ? std::max(widths[(size_t)p], 0.01f) : 0.25f; };
    // Lays out `count` panels from `first` across one row at `y`, sharing `w` by their widths (the last takes what's left).
    auto row = [&](size_t first, size_t count, float y, float rh) {
        float sum = 0;
        for (size_t i = first; i < first + count; ++i) sum += widthOf(order[i]);
        const float usable = w - gap * (float)(count - 1);
        float x = 0;
        for (size_t i = first; i < first + count; ++i) {
            const bool last = i + 1 == first + count;
            const float bw = last ? w - x : std::floor(usable * widthOf(order[i]) / sum);
            a.boxes.push_back({order[i], x, y, bw, rh});
            x += bw + gap;
        }
    };
    row(0, n, 0, h);
    bool fits = true;
    for (const Box& b : a.boxes) fits = fits && b.w >= minW;
    if (fits || n == 1) return a;
    // Too narrow: as many per row as fit at minW, the rows filled evenly.
    a.boxes.clear();
    const size_t fitPerRow = (size_t)std::max(1.0f, std::floor((w + gap) / (std::max(minW, 1.0f) + gap)));
    const size_t rows = (n + fitPerRow - 1) / fitPerRow;
    const float rh = std::floor((h - gap * (float)(rows - 1)) / (float)rows);
    size_t next = 0;
    for (size_t r = 0; r < rows; ++r) {
        const size_t count = (n - next + (rows - r) - 1) / (rows - r);   // what's left, shared over the rows left
        const float y = (float)r * (rh + gap);
        row(next, count, y, r + 1 == rows ? h - y : rh);
        next += count;
    }
    a.rows = (int)rows;
    a.wrapped = rows > 1;
    return a;
}

std::string Layout::toJson() const {
    nlohmann::json j = {{"order", order}, {"widths", widths}, {"dockHeight", dockHeight}, {"preview", previewVisible}};
    return j.dump();
}

Layout Layout::fromJson(const std::string& text) {
    Layout l;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_object()) {
        if (j.contains("order") && j["order"].is_array()) {
            l.order.clear();
            for (auto& v : j["order"]) if (v.is_number_integer()) l.order.push_back(v.get<int>());
        }
        if (j.contains("widths") && j["widths"].is_array()) {
            l.widths.clear();
            for (auto& v : j["widths"]) l.widths.push_back(v.is_number() ? v.get<float>() : 0.25f);
        }
        if (j.contains("dockHeight") && j["dockHeight"].is_number()) l.dockHeight = j["dockHeight"].get<float>();
        if (j.contains("preview") && j["preview"].is_boolean()) l.previewVisible = j["preview"].get<bool>();
    }
    l.normalise();
    return l;
}

}  // namespace ncap

#include "ncap/sourceutil.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>

namespace ncap::sourceutil {

namespace {
int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)std::tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
std::string strip(const std::string& s) {
    std::string o;
    for (char c : s)
        if (!std::isspace((unsigned char)c)) o += c;
    return o;
}
bool number(const std::string& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtof(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}
}  // namespace

bool parseColour(const std::string& text, Rgba& out) {
    std::string s = strip(text);
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    for (char c : s)
        if (hexVal(c) < 0) return false;
    auto byte = [&](std::size_t i) { return (std::uint8_t)(hexVal(s[i]) * 16 + hexVal(s[i + 1])); };
    if (s.size() == 3) {
        out = {(std::uint8_t)(hexVal(s[0]) * 17), (std::uint8_t)(hexVal(s[1]) * 17), (std::uint8_t)(hexVal(s[2]) * 17), 255};
        return true;
    }
    if (s.size() == 6 || s.size() == 8) {
        out = {byte(0), byte(2), byte(4), s.size() == 8 ? byte(6) : (std::uint8_t)255};
        return true;
    }
    return false;
}

std::string colourText(const Rgba& c) {
    char b[16];
    if (c.a == 255) std::snprintf(b, sizeof b, "#%02X%02X%02X", c.r, c.g, c.b);
    else std::snprintf(b, sizeof b, "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
    return b;
}

bool parsePlacement(const std::string& text, Placement& out) {
    std::string t = text;
    std::replace(t.begin(), t.end(), ',', ' ');
    std::istringstream in(t);
    std::vector<std::string> parts;
    std::string p;
    while (in >> p) parts.push_back(p);
    if (parts.empty() || parts.size() == 3 || parts.size() > 4) return false;
    if (parts.size() == 1) return false;
    Placement r;
    if (parts[0] != "-") {
        if (!number(parts[0], r.x)) return false;
        r.hasX = true;
    }
    if (parts[1] != "-") {
        if (!number(parts[1], r.y)) return false;
        r.hasY = true;
    }
    if (parts.size() == 4) {
        if (!number(parts[2], r.w) || !number(parts[3], r.h) || r.w <= 0 || r.h <= 0 || r.w > 100000 || r.h > 100000) return false;
        r.hasSize = true;
    }
    if (std::fabs(r.x) > 100000 || std::fabs(r.y) > 100000) return false;
    out = r;
    return true;
}

std::string placementText(const Transform& t, float sw, float sh) {
    const float w = t.width > 0 ? t.width : sw - t.cropLeft - t.cropRight, h = t.height > 0 ? t.height : sh - t.cropTop - t.cropBottom;
    char b[96];
    std::snprintf(b, sizeof b, "%g, %g, %g, %g", std::round(t.x * 10) / 10, std::round(t.y * 10) / 10, std::round(w * 10) / 10, std::round(h * 10) / 10);
    return b;
}

bool parseDegrees(const std::string& text, float& out) {
    std::string s = strip(text);
    const std::string deg = "\xC2\xB0";   // °
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, deg) == 0) s.resize(s.size() - 2);
    float v = 0;
    if (!number(s, v) || std::fabs(v) > 100000) return false;
    v = std::fmod(v, 360.0f);
    if (v > 180) v -= 360;
    if (v <= -180) v += 360;
    out = v;
    return true;
}

std::vector<std::string> slideOrder(std::vector<std::string> names, bool shuffle, std::uint32_t seed) {
    auto natural = [](const std::string& a, const std::string& b) {
        std::size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
                std::size_t i2 = i, j2 = j;
                while (i2 < a.size() && std::isdigit((unsigned char)a[i2])) ++i2;
                while (j2 < b.size() && std::isdigit((unsigned char)b[j2])) ++j2;
                std::string na = a.substr(i, i2 - i), nb = b.substr(j, j2 - j);
                na.erase(0, std::min(na.find_first_not_of('0'), na.size()));
                nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size()));
                if (na.size() != nb.size()) return na.size() < nb.size();
                if (na != nb) return na < nb;
                i = i2;
                j = j2;
            } else {
                const int ca = std::tolower((unsigned char)a[i]), cb = std::tolower((unsigned char)b[j]);
                if (ca != cb) return ca < cb;
                ++i;
                ++j;
            }
        }
        return a.size() - i < b.size() - j;
    };
    std::sort(names.begin(), names.end(), natural);
    if (shuffle) {
        std::mt19937 rng(seed);
        for (std::size_t k = names.size(); k > 1; --k) std::swap(names[k - 1], names[rng() % k]);
    }
    return names;
}

bool isImageFile(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string e = name.substr(dot + 1);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e == "png" || e == "jpg" || e == "jpeg" || e == "bmp" || e == "gif" || e == "tif" || e == "tiff" || e == "webp" || e == "heic";
}

bool isMediaFile(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string e = name.substr(dot + 1);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    for (const char* k : {"mp4", "m4v", "mov", "mkv", "avi", "wmv", "webm", "mp3", "m4a", "aac", "wav", "wma", "flac"})
        if (e == k) return true;
    return false;
}

std::string clockText(double seconds) {
    const long s = seconds > 0 ? (long)seconds : 0;
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else std::snprintf(b, sizeof b, "%ld:%02ld", s / 60, s % 60);
    return b;
}

int gifDelayMs(int hundredths) { return hundredths <= 1 ? 100 : hundredths * 10; }


TrackInfo trackFromFileName(const std::string& path) {
    std::string n = path.substr(path.find_last_of("\\/") + 1);
    if (const std::size_t dot = n.find_last_of('.'); dot != std::string::npos && dot > 0 && n.size() - dot <= 5) n = n.substr(0, dot);
    for (char& c : n)
        if (c == '_') c = ' ';
    auto trim = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    };
    auto dropNumber = [&](std::string s) {   // "03 ", "03. ", "03 - ", "3-"
        std::size_t i = 0;
        while (i < s.size() && i < 3 && std::isdigit((unsigned char)s[i])) ++i;
        if (i == 0 || i == s.size()) return s;
        std::size_t j = i;
        while (j < s.size() && (s[j] == ' ' || s[j] == '.' || s[j] == '-' || s[j] == ')')) ++j;
        if (j == i) return s;   // "1999 Song": a number that's part of the name
        return trim(s.substr(j));
    };
    n = dropNumber(trim(n));
    while (!n.empty() && (n.back() == '-' || n.back() == ' ')) n.pop_back();   // "Artist - " with no title
    TrackInfo t;
    const std::size_t sep = n.find(" - ");
    if (sep != std::string::npos) {
        t.artist = trim(n.substr(0, sep));
        t.title = trim(n.substr(sep + 3));
    } else {
        t.title = n;
    }
    if (t.title.empty()) std::swap(t.title, t.artist);
    return t;
}

}  // namespace ncap::sourceutil

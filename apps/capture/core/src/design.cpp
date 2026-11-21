#include "ncap/design.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <sstream>

#include "qrcodegen/qrcodegen.hpp"

namespace ncap::design {

using nlohmann::json;
using sourceutil::colourText;
using sourceutil::parseColour;

namespace {
constexpr float kPi = 3.14159265358979f;
const char* kKinds[] = {"text", "rect", "ellipse", "image", "timer", "qr", "icon", "chat"};
const char* kAnims[] = {"none", "fade", "slide-left", "slide-right", "slide-up", "slide-down", "pop", "wipe"};
float clampf(float v, float a, float b) { return std::isfinite(v) ? std::clamp(v, a, b) : a; }
std::uint8_t mix8(std::uint8_t a, std::uint8_t b, float t) { return (std::uint8_t)std::lround(a + (b - a) * t); }

json colourJson(const Rgba& c) { return colourText(c); }
Rgba colourFrom(const json& j, Rgba def) {
    Rgba c;
    return j.is_string() && parseColour(j.get<std::string>(), c) ? c : def;
}

json fillJson(const Fill& f) {
    json j = {{"type", f.type == Fill::None ? "none" : f.type == Fill::Solid ? "solid" : f.type == Fill::Linear ? "linear" : "radial"}};
    if (f.type == Fill::Solid) {
        j["colour"] = colourJson(f.colour);
        if (!f.brand.empty()) j["brand"] = f.brand;
    }
    if (f.type == Fill::Linear || f.type == Fill::Radial) {
        json s = json::array();
        for (auto& st : f.stops) {
            json o = {{"at", st.at}, {"colour", colourJson(st.colour)}};
            if (!st.brand.empty()) o["brand"] = st.brand;
            s.push_back(o);
        }
        j["stops"] = s;
        j["angle"] = f.angle;
    }
    return j;
}

Fill fillFrom(const json& j, const Fill& def) {
    if (!j.is_object()) return def;
    const std::string t = j.value("type", std::string());
    if (t == "none") return Fill::none();
    auto brandOf = [](const json& o) {
        return o.contains("brand") && o["brand"].is_string() ? o["brand"].get<std::string>().substr(0, 16) : std::string();
    };
    if (t == "solid") {
        Fill f = Fill::solid(colourFrom(j.contains("colour") ? j["colour"] : json(), def.colour));
        f.brand = brandOf(j);
        return f;
    }
    if (t == "linear" || t == "radial") {
        std::vector<Stop> stops;
        if (j.contains("stops") && j["stops"].is_array())
            for (auto& s : j["stops"]) {
                if (!s.is_object() || stops.size() >= 16) continue;
                Stop st;
                st.at = s.contains("at") && s["at"].is_number() ? clampf(s["at"].get<float>(), 0, 1) : 0;
                st.colour = colourFrom(s.contains("colour") ? s["colour"] : json(), {255, 255, 255, 255});
                st.brand = brandOf(s);
                stops.push_back(st);
            }
        if (stops.size() < 2) return def;
        std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.at < b.at; });
        const float angle = j.contains("angle") && j["angle"].is_number() ? clampf(j["angle"].get<float>(), -360, 360) : 0;
        Fill f = Fill::linear(stops, angle);
        if (t == "radial") f.type = Fill::Radial;
        return f;
    }
    return def;
}

void corners(const Element& e, float out[8]) {
    const float cx = e.x + e.w / 2, cy = e.y + e.h / 2;
    const float a = e.rotation * kPi / 180, c = std::cos(a), s = std::sin(a);
    const float px[4] = {-e.w / 2, e.w / 2, e.w / 2, -e.w / 2}, py[4] = {-e.h / 2, -e.h / 2, e.h / 2, e.h / 2};
    for (int i = 0; i < 4; ++i) {
        out[i * 2] = cx + px[i] * c - py[i] * s;
        out[i * 2 + 1] = cy + px[i] * s + py[i] * c;
    }
}
}  // namespace

Fill Fill::solid(Rgba c) {
    Fill f;
    f.type = Solid;
    f.colour = c;
    return f;
}
Fill Fill::linear(std::vector<Stop> s, float angle) {
    Fill f;
    f.type = Linear;
    f.stops = std::move(s);
    f.angle = angle;
    return f;
}
Fill Fill::radial(std::vector<Stop> s) {
    Fill f = linear(std::move(s), 0);
    f.type = Radial;
    return f;
}
Fill Fill::none() {
    Fill f;
    f.type = None;
    return f;
}

Rgba colourAt(const Fill& f, float t) {
    if (f.type == Fill::Solid) return f.colour;
    if ((f.type != Fill::Linear && f.type != Fill::Radial) || f.stops.empty()) return {0, 0, 0, 0};
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= f.stops.front().at) return f.stops.front().colour;
    if (t >= f.stops.back().at) return f.stops.back().colour;
    for (std::size_t i = 1; i < f.stops.size(); ++i)
        if (t <= f.stops[i].at) {
            const Stop &a = f.stops[i - 1], &b = f.stops[i];
            const float u = b.at > a.at ? (t - a.at) / (b.at - a.at) : 1.0f;
            return {mix8(a.colour.r, b.colour.r, u), mix8(a.colour.g, b.colour.g, u), mix8(a.colour.b, b.colour.b, u), mix8(a.colour.a, b.colour.a, u)};
        }
    return f.stops.back().colour;
}

namespace {
bool isGradient(const Fill& f) { return (f.type == Fill::Linear || f.type == Fill::Radial) && f.stops.size() >= 2; }
// Where a stop at `at` goes among the others (after any at the same place).
int placeFor(const std::vector<Stop>& stops, float at) {
    int i = 0;
    while (i < (int)stops.size() && stops[(size_t)i].at <= at) ++i;
    return i;
}
}  // namespace

int addStop(Fill& f, float at) {
    if (!isGradient(f) || f.stops.size() >= kMaxStops || !std::isfinite(at)) return -1;
    at = std::clamp(at, 0.0f, 1.0f);
    Stop s;
    s.at = at;
    s.colour = colourAt(f, at);
    const int i = placeFor(f.stops, at);
    f.stops.insert(f.stops.begin() + i, s);
    return i;
}

int moveStop(Fill& f, int index, float at) {
    if (!isGradient(f) || index < 0 || index >= (int)f.stops.size()) return -1;
    if (!std::isfinite(at)) return index;
    Stop s = f.stops[(size_t)index];
    s.at = std::clamp(at, 0.0f, 1.0f);
    f.stops.erase(f.stops.begin() + index);
    const int i = placeFor(f.stops, s.at);
    f.stops.insert(f.stops.begin() + i, s);
    return i;
}

bool removeStop(Fill& f, int index) {
    if (!isGradient(f) || f.stops.size() <= 2 || index < 0 || index >= (int)f.stops.size()) return false;
    f.stops.erase(f.stops.begin() + index);
    return true;
}

int stopNear(const Fill& f, float t, float within) {
    if (!isGradient(f)) return -1;
    int best = -1;
    float bestD = within;
    for (std::size_t i = 0; i < f.stops.size(); ++i) {
        const float d = std::fabs(f.stops[i].at - t);
        if (d <= bestD) best = (int)i, bestD = d;   // `<=`: of stops on top of each other, the later (drawn on top)
    }
    return best;
}

float glowLevel(float coverage, float strength) {
    if (!std::isfinite(coverage) || coverage <= 0) return 0;
    if (coverage >= 1) return 1;
    strength = std::isfinite(strength) ? std::clamp(strength, 0.25f, 4.0f) : 1.0f;
    return 1.0f - std::pow(1.0f - coverage, strength);
}

namespace {
// Felzenszwalb and Huttenlocher's squared distance transform of one line: f in, d out (n values).
void edt1(const float* f, float* d, int n, std::vector<int>& v, std::vector<float>& z) {
    const float kInf = std::numeric_limits<float>::infinity();   // for the boundaries only; f's "far" is a large finite value
    v.assign((size_t)n, 0);
    z.assign((size_t)n + 1, 0);
    int k = 0;
    z[0] = -kInf, z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        float s;
        for (;;) {   // z[0] is -infinity, so this stops at k = 0 at the latest
            const int p = v[(size_t)k];
            s = ((f[q] + (float)q * q) - (f[p] + (float)p * p)) / (2.0f * (q - p));
            if (s > z[(size_t)k]) break;
            --k;
        }
        ++k;
        v[(size_t)k] = q;
        z[(size_t)k] = s;
        z[(size_t)k + 1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[(size_t)k + 1] < (float)q) ++k;
        const int p = v[(size_t)k];
        d[q] = (float)(q - p) * (q - p) + f[p];
    }
}
}  // namespace

std::vector<std::uint8_t> grow(const std::vector<std::uint8_t>& coverage, int w, int h, float radius) {
    if (w <= 0 || h <= 0 || coverage.size() != (size_t)w * h || !std::isfinite(radius) || radius <= 0) return coverage;
    constexpr float kInf = 1e20f;
    std::vector<float> dist((size_t)w * h);
    for (size_t i = 0; i < dist.size(); ++i) dist[i] = coverage[i] >= 128 ? 0.0f : kInf;
    std::vector<float> f((size_t)std::max(w, h)), d((size_t)std::max(w, h));
    std::vector<int> v;
    std::vector<float> z;
    for (int x = 0; x < w; ++x) {   // down each column
        for (int y = 0; y < h; ++y) f[(size_t)y] = dist[(size_t)y * w + x];
        edt1(f.data(), d.data(), h, v, z);
        for (int y = 0; y < h; ++y) dist[(size_t)y * w + x] = d[(size_t)y];
    }
    for (int y = 0; y < h; ++y) {   // then along each row
        float* row = &dist[(size_t)y * w];
        std::copy(row, row + w, f.begin());
        edt1(f.data(), row, w, v, z);
    }
    std::vector<std::uint8_t> out(coverage.size());
    for (size_t i = 0; i < out.size(); ++i) {
        const float cov = std::clamp(radius + 0.5f - std::sqrt(dist[i]), 0.0f, 1.0f);
        out[i] = std::max(coverage[i], (std::uint8_t)std::lround(cov * 255));
    }
    return out;
}

// ---------------------------------------------------------------- pictures (0.16)

const std::vector<FilterLook>& filterLooks() {
    static const std::vector<FilterLook> looks = [] {
        auto tint = [](const char* h, float amount, Filters f) {
            Rgba c;
            sourceutil::parseColour(h, c);
            c.a = 0;
            f.tint = c;
            f.tintAmount = amount;
            return f;
        };
        Filters bw;
        bw.saturation = 0;
        Filters sepia = tint("#704214", 40, bw);
        sepia.contrast = 5;
        Filters warm;
        warm.saturation = 110;
        warm = tint("#FF9A3C", 15, warm);
        Filters cool = tint("#3C8DFF", 15, Filters{});
        Filters faded;
        faded.contrast = -30, faded.brightness = 12, faded.saturation = 80;
        Filters vivid;
        vivid.contrast = 15, vivid.saturation = 140;
        Filters soft;
        soft.blur = 3, soft.brightness = 6;
        Filters moon;
        moon.saturation = 40, moon.brightness = -10;
        moon = tint("#5C7AFF", 25, moon);
        Filters parchment;
        parchment.saturation = 30, parchment.contrast = -10;
        parchment = tint("#C9A66B", 35, parchment);
        return std::vector<FilterLook>{{"Black and white", bw}, {"Sepia", sepia}, {"Warm", warm},   {"Cool", cool},
                                       {"Faded", faded},        {"Vivid", vivid}, {"Soft focus", soft}, {"Moonlight", moon},
                                       {"Parchment", parchment}};
    }();
    return looks;
}

namespace {
// Three box blurs (close to a Gaussian) of premultiplied RGBA, radius r.
void blurRgba(std::vector<float>& p, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<float> t(p.size());
    const float n = 2.0f * r + 1;
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y)
            for (int ch = 0; ch < 4; ++ch) {
                const float* row = &p[(size_t)y * w * 4];
                float sum = 0;
                for (int x = -r; x <= r; ++x) sum += row[(size_t)std::clamp(x, 0, w - 1) * 4 + ch];
                for (int x = 0; x < w; ++x) {
                    t[((size_t)y * w + x) * 4 + ch] = sum / n;
                    sum += row[(size_t)std::min(x + r + 1, w - 1) * 4 + ch] - row[(size_t)std::max(x - r, 0) * 4 + ch];
                }
            }
        for (int x = 0; x < w; ++x)
            for (int ch = 0; ch < 4; ++ch) {
                float sum = 0;
                for (int y = -r; y <= r; ++y) sum += t[((size_t)std::clamp(y, 0, h - 1) * w + x) * 4 + ch];
                for (int y = 0; y < h; ++y) {
                    p[((size_t)y * w + x) * 4 + ch] = sum / n;
                    sum += t[((size_t)std::min(y + r + 1, h - 1) * w + x) * 4 + ch] - t[((size_t)std::max(y - r, 0) * w + x) * 4 + ch];
                }
            }
    }
}
}  // namespace

void applyFilters(std::vector<std::uint8_t>& rgba, int w, int h, const Filters& f, float blurPx) {
    if (w <= 0 || h <= 0 || rgba.size() != (size_t)w * h * 4 || !f.any()) return;
    const int r = std::isfinite(blurPx) && f.blur > 0 ? std::clamp((int)std::lround(blurPx / 2), 0, 400) : 0;
    if (r > 0) {
        std::vector<float> p(rgba.size());
        for (size_t i = 0; i < rgba.size(); i += 4) {
            const float a = rgba[i + 3] / 255.0f;
            p[i] = rgba[i] * a, p[i + 1] = rgba[i + 1] * a, p[i + 2] = rgba[i + 2] * a, p[i + 3] = rgba[i + 3];
        }
        blurRgba(p, w, h, r);
        for (size_t i = 0; i < rgba.size(); i += 4) {
            const float a = p[i + 3];
            rgba[i + 3] = (std::uint8_t)std::clamp(std::lround(a), 0L, 255L);
            for (int k = 0; k < 3; ++k) rgba[i + (size_t)k] = a > 0.5f ? (std::uint8_t)std::clamp(std::lround(p[i + (size_t)k] * 255.0f / a), 0L, 255L) : 0;
        }
    }
    const bool colour = f.brightness != 0 || f.contrast != 0 || f.saturation != 100 || f.tintAmount > 0;
    if (!colour) return;
    const float bright = f.brightness * 2.55f, c = f.contrast * 2.55f;
    const float factor = (259.0f * (c + 255.0f)) / (255.0f * (259.0f - c));
    const float sat = f.saturation / 100.0f, tint = f.tintAmount / 100.0f;
    std::uint8_t lut[256];   // brightness and contrast, the same for every channel
    for (int v = 0; v < 256; ++v) lut[v] = (std::uint8_t)std::clamp(std::lround(factor * (v + bright - 128) + 128), 0L, 255L);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        float ch[3] = {(float)lut[rgba[i]], (float)lut[rgba[i + 1]], (float)lut[rgba[i + 2]]};
        const float l = 0.2126f * ch[0] + 0.7152f * ch[1] + 0.0722f * ch[2];
        const float tc[3] = {(float)f.tint.r, (float)f.tint.g, (float)f.tint.b};
        for (int k = 0; k < 3; ++k) {
            float v = l + (ch[k] - l) * sat;
            v += (tc[k] - v) * tint;
            rgba[i + (size_t)k] = (std::uint8_t)std::clamp(std::lround(v), 0L, 255L);
        }
    }
}

const std::vector<std::string>& maskKeys() {
    static const std::vector<std::string> keys = {"circle", "arch", "hexagon", "diamond", "star", "heart"};
    return keys;
}

std::vector<std::pair<float, float>> maskOutline(const std::string& mask, float w, float h) {
    std::vector<std::pair<float, float>> pts;
    if (!(w > 0) || !(h > 0) || !std::isfinite(w) || !std::isfinite(h)) return pts;
    constexpr float pi = 3.14159265358979f;
    // Stretches points to fill the box exactly (for shapes made in their own units).
    auto fit = [&](std::vector<std::pair<float, float>> raw) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (auto [x, y] : raw) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        for (auto& [x, y] : raw) x = (x - x0) / (x1 - x0) * w, y = (y - y0) / (y1 - y0) * h;
        return raw;
    };
    if (mask == "circle") {
        for (int i = 0; i < 96; ++i) {
            const float a = 2 * pi * i / 96;
            pts.push_back({w / 2 + std::cos(a) * w / 2, h / 2 + std::sin(a) * h / 2});
        }
    } else if (mask == "arch") {   // a doorway: straight sides, a round top (as round as the box allows)
        const float ry = std::min(w / 2, h);
        pts.push_back({0, h});
        for (int i = 0; i <= 48; ++i) {
            const float a = pi + pi * i / 48;   // left, over the top, to the right
            pts.push_back({w / 2 + std::cos(a) * w / 2, ry + std::sin(a) * ry});
        }
        pts.push_back({w, h});
    } else if (mask == "hexagon") {
        pts = {{w * 0.25f, 0}, {w * 0.75f, 0}, {w, h / 2}, {w * 0.75f, h}, {w * 0.25f, h}, {0, h / 2}};
    } else if (mask == "diamond") {
        pts = {{w / 2, 0}, {w, h / 2}, {w / 2, h}, {0, h / 2}};
    } else if (mask == "star") {
        std::vector<std::pair<float, float>> raw;
        for (int i = 0; i < 10; ++i) {
            const float a = -pi / 2 + pi * i / 5, rad = i % 2 ? 0.42f : 1.0f;
            raw.push_back({std::cos(a) * rad, std::sin(a) * rad});
        }
        pts = fit(raw);
    } else if (mask == "heart") {
        std::vector<std::pair<float, float>> raw;
        for (int i = 0; i < 120; ++i) {
            const float t = 2 * pi * i / 120, s = std::sin(t);
            raw.push_back({16 * s * s * s, -(13 * std::cos(t) - 5 * std::cos(2 * t) - 2 * std::cos(3 * t) - std::cos(4 * t))});
        }
        pts = fit(raw);
    }
    return pts;
}

Area pictureArea(float iw, float ih, float bw, float bh, float zoom, float fx, float fy) {
    if (!(iw > 0) || !(ih > 0) || !(bw > 0) || !(bh > 0)) return {0, 0, std::max(0.0f, iw), std::max(0.0f, ih)};
    zoom = std::isfinite(zoom) ? std::clamp(zoom, 1.0f, 8.0f) : 1.0f;
    fx = std::isfinite(fx) ? std::clamp(fx, 0.0f, 1.0f) : 0.5f;
    fy = std::isfinite(fy) ? std::clamp(fy, 0.0f, 1.0f) : 0.5f;
    const float scale = std::max(bw / iw, bh / ih) * zoom;   // picture pixels to box pixels
    const float sw = std::min(iw, bw / scale), sh = std::min(ih, bh / scale);
    const float cx = std::clamp(fx * iw, sw / 2, iw - sw / 2), cy = std::clamp(fy * ih, sh / 2, ih - sh / 2);
    return {cx - sw / 2, cy - sh / 2, sw, sh};
}

void panPicture(float iw, float ih, float bw, float bh, float zoom, float& fx, float& fy, float dx, float dy) {
    if (!(iw > 0) || !(ih > 0) || !(bw > 0) || !(bh > 0) || !std::isfinite(dx) || !std::isfinite(dy)) return;
    // Start from where it shows now (the focus may be past what's reachable), then move against the drag.
    const Area a = pictureArea(iw, ih, bw, bh, zoom, fx, fy);
    const float perBoxX = a.w / bw, perBoxY = a.h / bh;   // picture pixels per box pixel
    const float cx = a.x + a.w / 2 - dx * perBoxX, cy = a.y + a.h / 2 - dy * perBoxY;
    fx = std::clamp(cx, a.w / 2, iw - a.w / 2) / iw;
    fy = std::clamp(cy, a.h / 2, ih - a.h / 2) / ih;
}

const char* key(Kind k) { return kKinds[(int)k]; }
bool parse(const std::string& s, Kind& out) {
    for (int i = 0; i < 8; ++i)
        if (s == kKinds[i]) {
            out = (Kind)i;
            return true;
        }
    return false;
}
const char* key(Anim a) { return kAnims[(int)a]; }
bool parse(const std::string& s, Anim& out) {
    for (int i = 0; i < 8; ++i)
        if (s == kAnims[i]) {
            out = (Anim)i;
            return true;
        }
    return false;
}

const Element* Document::find(const std::string& id) const {
    for (auto& e : elements)
        if (e.id == id) return &e;
    return nullptr;
}
Element* Document::find(const std::string& id) {
    for (auto& e : elements)
        if (e.id == id) return &e;
    return nullptr;
}
int Document::indexOf(const std::string& id) const {
    for (std::size_t i = 0; i < elements.size(); ++i)
        if (elements[i].id == id) return (int)i;
    return -1;
}

std::string toJson(const Document& d) {
    json els = json::array();
    for (auto& e : d.elements) {
        json j = {{"id", e.id},
                  {"name", e.name},
                  {"kind", key(e.kind)},
                  {"x", e.x},
                  {"y", e.y},
                  {"w", e.w},
                  {"h", e.h},
                  {"rotation", e.rotation},
                  {"opacity", e.opacity},
                  {"visible", e.visible},
                  {"locked", e.locked},
                  {"fill", fillJson(e.fill)},
                  {"stroke", colourJson(e.stroke)},
                  {"strokeWidth", e.strokeWidth},
                  {"radius", e.radius},
                  {"shadow", {{"on", e.shadow.on}, {"dx", e.shadow.dx}, {"dy", e.shadow.dy}, {"blur", e.shadow.blur}, {"colour", colourJson(e.shadow.colour)}}},
                  {"anim", key(e.anim)},
                  {"animMs", e.animMs}};
        if (!e.strokeBrand.empty()) j["strokeBrand"] = e.strokeBrand;
        if (!e.group.empty()) j["group"] = e.group;
        if (!e.shadow.brand.empty()) j["shadow"]["brand"] = e.shadow.brand;
        if (!(e.glow == Glow{})) {
            j["glow"] = {{"on", e.glow.on}, {"size", e.glow.size}, {"strength", e.glow.strength}, {"colour", colourJson(e.glow.colour)}};
            if (!e.glow.brand.empty()) j["glow"]["brand"] = e.glow.brand;
        }
        if (e.kind == Kind::Timer) {
            j["timer"] = {{"mode", e.timerMode}, {"seconds", e.timerSeconds}, {"until", e.timerUntil}, {"end", e.timerEnd},
                          {"date", e.timerDate}, {"pulse", e.timerPulse}, {"scene", e.timerScene}};
        }
        if (e.kind == Kind::Qr) j["qr"] = e.qr;
        if (e.kind == Kind::Icon) j["icon"] = e.icon;
        if (e.kind == Kind::Image) {   // 0.16: written only when changed
            json pic = json::object();
            if (e.picZoom != 1 || e.picX != 0.5f || e.picY != 0.5f) pic["zoom"] = e.picZoom, pic["x"] = e.picX, pic["y"] = e.picY;
            if (!e.mask.empty()) pic["mask"] = e.mask;
            if (e.filters.any()) {
                const Filters& f = e.filters;
                pic["filters"] = {{"brightness", f.brightness}, {"contrast", f.contrast}, {"saturation", f.saturation}, {"blur", f.blur},
                                  {"tint", colourJson(f.tint)}, {"tintAmount", f.tintAmount}};
            }
            if (!pic.empty()) j["picture"] = pic;
        }
        if (e.kind == Kind::Chat) {
            j["chat"] = {{"count", e.chatCount}, {"fade", e.chatFade}, {"bubbles", e.chatBubbles}, {"bubble", colourJson(e.chatBubble)},
                         {"bubbleBrand", e.chatBubbleBrand}, {"nameColours", e.chatNameColours}, {"side", e.chatSide}};
        }
        if (e.kind == Kind::Text || e.kind == Kind::Timer || e.kind == Kind::Chat) {
            j["text"] = e.text;
            j["font"] = e.font;
            j["size"] = e.size;
            j["bold"] = e.bold;
            j["italic"] = e.italic;
            j["align"] = e.align;
        }
        if (e.kind == Kind::Image) {
            j["file"] = e.file;
            j["cover"] = e.cover;
        }
        els.push_back(j);
    }
    return json{{"format", "northray-capture-design"}, {"version", 1}, {"name", d.name}, {"width", d.width}, {"height", d.height},
                {"background", fillJson(d.background)}, {"elements", els}, {"nextId", d.nextId}, {"onlyWhilePlaying", d.onlyWhilePlaying},
                {"onlyWhileSprint", d.onlyWhileSprint}, {"onlyCelebrating", d.onlyCelebrating},
                {"grid", d.grid}, {"guidesX", d.guidesX}, {"guidesY", d.guidesY}}
        .dump();
}

Document fromJson(const std::string& text) {
    Document d;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return d;
    auto num = [](const json& o, const char* k, float def, float lo, float hi) {
        return o.contains(k) && o[k].is_number() ? clampf(o[k].get<float>(), lo, hi) : def;
    };
    auto flag = [](const json& o, const char* k, bool def) { return o.contains(k) && o[k].is_boolean() ? o[k].get<bool>() : def; };
    auto str = [](const json& o, const char* k, const std::string& def, std::size_t max) {
        return o.contains(k) && o[k].is_string() ? o[k].get<std::string>().substr(0, max) : def;
    };
    d.name = str(j, "name", d.name, 80);
    d.width = (int)num(j, "width", 1920, 16, 8192);
    d.height = (int)num(j, "height", 1080, 16, 8192);
    if (j.contains("background")) d.background = fillFrom(j["background"], Fill::none());
    d.onlyWhilePlaying = j.contains("onlyWhilePlaying") && j["onlyWhilePlaying"].is_boolean() && j["onlyWhilePlaying"].get<bool>();
    d.onlyWhileSprint = j.contains("onlyWhileSprint") && j["onlyWhileSprint"].is_boolean() && j["onlyWhileSprint"].get<bool>();
    d.onlyCelebrating = j.contains("onlyCelebrating") && j["onlyCelebrating"].is_boolean() && j["onlyCelebrating"].get<bool>();
    d.grid = (int)num(j, "grid", 0, 0, 500);
    for (auto [k, v] : {std::pair{"guidesX", &d.guidesX}, std::pair{"guidesY", &d.guidesY}})
        if (j.contains(k) && j[k].is_array())
            for (auto& g : j[k])
                if (g.is_number() && v->size() < 50) v->push_back(clampf(g.get<float>(), -20000, 20000));
    int maxId = 0;
    if (j.contains("elements") && j["elements"].is_array())
        for (auto& o : j["elements"]) {
            if (!o.is_object() || d.elements.size() >= 200) continue;
            Element e;
            if (!parse(str(o, "kind", "", 16), e.kind)) continue;
            e.id = str(o, "id", "", 32);
            if (e.id.empty() || d.find(e.id)) continue;
            if (e.id.size() > 1 && e.id[0] == 'e') maxId = std::max(maxId, std::atoi(e.id.c_str() + 1));
            e.name = str(o, "name", "", 80);
            e.x = num(o, "x", 0, -20000, 20000);
            e.y = num(o, "y", 0, -20000, 20000);
            e.w = num(o, "w", 100, 1, 20000);
            e.h = num(o, "h", 100, 1, 20000);
            e.rotation = num(o, "rotation", 0, -360, 360);
            e.opacity = num(o, "opacity", 1, 0, 1);
            e.visible = flag(o, "visible", true);
            e.locked = flag(o, "locked", false);
            e.fill = fillFrom(o.contains("fill") ? o["fill"] : json(), e.kind == Kind::Image ? Fill::none() : Fill::solid({255, 255, 255, 255}));
            e.stroke = colourFrom(o.contains("stroke") ? o["stroke"] : json(), {0, 0, 0, 0});
            e.strokeWidth = num(o, "strokeWidth", 0, 0, 200);
            e.strokeBrand = str(o, "strokeBrand", "", 16);
            e.group = str(o, "group", "", 32);
            e.radius = num(o, "radius", 0, 0, 10000);
            if (o.contains("glow") && o["glow"].is_object()) {
                const json& g = o["glow"];
                const Glow def;
                e.glow.on = flag(g, "on", false);
                e.glow.size = num(g, "size", def.size, 1, 120);
                e.glow.strength = num(g, "strength", def.strength, 0.25, 4);
                e.glow.colour = colourFrom(g.contains("colour") ? g["colour"] : json(), def.colour);
                e.glow.brand = str(g, "brand", "", 16);
            }
            if (o.contains("shadow") && o["shadow"].is_object()) {
                const json& s = o["shadow"];
                e.shadow.on = flag(s, "on", false);
                e.shadow.dx = num(s, "dx", 0, -200, 200);
                e.shadow.dy = num(s, "dy", 6, -200, 200);
                e.shadow.blur = num(s, "blur", 12, 0, 100);
                e.shadow.colour = colourFrom(s.contains("colour") ? s["colour"] : json(), {0, 0, 0, 140});
                e.shadow.brand = str(s, "brand", "", 16);
            }
            parse(str(o, "anim", "none", 16), e.anim);
            e.animMs = (int)num(o, "animMs", 500, 0, 5000);
            e.text = str(o, "text", "", 2000);
            e.font = str(o, "font", "Segoe UI", 64);
            e.size = num(o, "size", 64, 4, 1000);
            e.bold = flag(o, "bold", false);
            e.italic = flag(o, "italic", false);
            e.align = str(o, "align", "left", 8);
            if (e.align != "left" && e.align != "centre" && e.align != "right") e.align = "left";
            e.file = str(o, "file", "", 1024);
            e.qr = str(o, "qr", "", 1000);
            e.icon = str(o, "icon", "", 32);
            if (o.contains("timer") && o["timer"].is_object()) {
                const json& t = o["timer"];
                e.timerMode = str(t, "mode", "countdown", 16);
                // Every mode is kept (0.13: the 0.11 modes, golive, remaining and next, were turned back into countdowns here).
                static const char* modes[] = {"countdown", "until", "stopwatch", "live", "clock", "golive", "remaining", "next", "sprint"};
                if (std::none_of(std::begin(modes), std::end(modes), [&](const char* m) { return e.timerMode == m; })) e.timerMode = "countdown";
                e.timerSeconds = (int)num(t, "seconds", 300, 1, 359999);
                e.timerUntil = (int)num(t, "until", 20 * 60, 0, 24 * 60 - 1);
                e.timerEnd = str(t, "end", "Starting now!", 200);
                e.timerDate = flag(t, "date", false);
                e.timerPulse = flag(t, "pulse", true);
                e.timerScene = str(t, "scene", "", 64);
            }
            if (o.contains("chat") && o["chat"].is_object()) {
                const json& c = o["chat"];
                e.chatCount = (int)num(c, "count", 8, 1, 30);
                e.chatFade = (int)num(c, "fade", 0, 0, 3600);
                e.chatBubbles = flag(c, "bubbles", true);
                e.chatBubble = colourFrom(c.contains("bubble") ? c["bubble"] : json(), {11, 26, 46, 210});
                e.chatBubbleBrand = str(c, "bubbleBrand", "panel", 16);
                e.chatNameColours = flag(c, "nameColours", true);
                e.chatSide = str(c, "side", "", 8);
                if (e.chatSide != "left" && e.chatSide != "right" && e.chatSide != "mine") e.chatSide.clear();
            }
            e.cover = flag(o, "cover", true);
            if (o.contains("picture") && o["picture"].is_object()) {
                const json& pic = o["picture"];
                e.picZoom = num(pic, "zoom", 1, 1, 8);
                e.picX = num(pic, "x", 0.5f, 0, 1);
                e.picY = num(pic, "y", 0.5f, 0, 1);
                e.mask = str(pic, "mask", "", 16);
                if (std::find(maskKeys().begin(), maskKeys().end(), e.mask) == maskKeys().end()) e.mask.clear();
                if (pic.contains("filters") && pic["filters"].is_object()) {
                    const json& fj = pic["filters"];
                    Filters& f = e.filters;
                    f.brightness = num(fj, "brightness", 0, -100, 100);
                    f.contrast = num(fj, "contrast", 0, -100, 100);
                    f.saturation = num(fj, "saturation", 100, 0, 200);
                    f.blur = num(fj, "blur", 0, 0, 40);
                    f.tint = colourFrom(fj.contains("tint") ? fj["tint"] : json(), {0, 0, 0, 0});
                    f.tint.a = 0;   // unused: the amount says how much
                    f.tintAmount = num(fj, "tintAmount", 0, 0, 100);
                }
            }
            d.elements.push_back(e);
        }
    d.nextId = std::max((int)num(j, "nextId", 1, 1, 1000000), maxId + 1);
    return d;
}

std::string add(Document& d, Element e) {
    if (d.elements.size() >= 200) return {};
    e.id = "e" + std::to_string(d.nextId++);
    while (d.find(e.id)) e.id = "e" + std::to_string(d.nextId++);
    if (e.name.empty()) {
        static const char* names[] = {"Text", "Rectangle", "Ellipse", "Picture", "Timer", "QR code", "Icon", "Chat"};
        e.name = names[(int)e.kind];
    }
    d.elements.push_back(e);
    return d.elements.back().id;
}

bool remove(Document& d, const std::string& id) {
    const int i = d.indexOf(id);
    if (i < 0) return false;
    d.elements.erase(d.elements.begin() + i);
    return true;
}

std::string duplicate(Document& d, const std::string& id) {
    const Element* e = d.find(id);
    if (!e) return {};
    Element c = *e;
    c.x += 20;
    c.y += 20;
    c.name += " copy";
    const int at = d.indexOf(id);
    const std::string nid = add(d, c);
    if (nid.empty()) return {};
    // Just above the original.
    Element moved = d.elements.back();
    d.elements.pop_back();
    d.elements.insert(d.elements.begin() + at + 1, moved);
    return nid;
}

bool move(Document& d, const std::string& id, int delta) {
    const int i = d.indexOf(id), j = i + delta;
    if (i < 0 || j < 0 || j >= (int)d.elements.size()) return false;
    std::swap(d.elements[(std::size_t)i], d.elements[(std::size_t)j]);
    return true;
}
bool toTop(Document& d, const std::string& id) {
    const int i = d.indexOf(id);
    if (i < 0 || i + 1 == (int)d.elements.size()) return false;
    Element e = d.elements[(std::size_t)i];
    d.elements.erase(d.elements.begin() + i);
    d.elements.push_back(e);
    return true;
}
bool toBottom(Document& d, const std::string& id) {
    const int i = d.indexOf(id);
    if (i <= 0) return false;
    Element e = d.elements[(std::size_t)i];
    d.elements.erase(d.elements.begin() + i);
    d.elements.insert(d.elements.begin(), e);
    return true;
}

std::string hitTest(const Document& d, float x, float y) {
    for (auto it = d.elements.rbegin(); it != d.elements.rend(); ++it) {
        const Element& e = *it;
        if (!e.visible || e.locked) continue;
        const float cx = e.x + e.w / 2, cy = e.y + e.h / 2;
        const float a = -e.rotation * kPi / 180;
        const float lx = (x - cx) * std::cos(a) - (y - cy) * std::sin(a), ly = (x - cx) * std::sin(a) + (y - cy) * std::cos(a);
        if (std::fabs(lx) > e.w / 2 || std::fabs(ly) > e.h / 2) continue;
        if (e.kind == Kind::Ellipse && (lx * lx) / (e.w * e.w / 4) + (ly * ly) / (e.h * e.h / 4) > 1) continue;
        return e.id;
    }
    return {};
}

Snapped snap(const Document& d, const std::string& movingId, Box p, float threshold, bool resizing) {
    return snap(d, std::vector<std::string>{movingId}, p, threshold, resizing);
}

Snapped snap(const Document& d, const std::vector<std::string>& moving, Box p, float threshold, bool resizing) {
    std::vector<float> xs = {0, d.width / 2.0f, (float)d.width}, ys = {0, d.height / 2.0f, (float)d.height};
    xs.insert(xs.end(), d.guidesX.begin(), d.guidesX.end());
    ys.insert(ys.end(), d.guidesY.begin(), d.guidesY.end());
    for (auto& e : d.elements) {
        if (std::find(moving.begin(), moving.end(), e.id) != moving.end() || !e.visible || std::fabs(e.rotation) > 0.01f) continue;
        xs.insert(xs.end(), {e.x, e.x + e.w / 2, e.x + e.w});
        ys.insert(ys.end(), {e.y, e.y + e.h / 2, e.y + e.h});
    }
    Snapped out;
    auto best = [&](const std::vector<float>& lines, const std::vector<float>& mine, float& delta, float& line) {
        bool found = false;
        float bd = threshold + 1;
        for (float l : lines)
            for (float m : mine)
                if (std::fabs(l - m) <= threshold && std::fabs(l - m) < bd) {
                    bd = std::fabs(l - m);
                    delta = l - m;
                    line = l;
                    found = true;
                }
        return found;
    };
    float dx = 0, dy = 0, lx = 0, ly = 0;
    const std::vector<float> mx = resizing ? std::vector<float>{p.x + p.w} : std::vector<float>{p.x, p.x + p.w / 2, p.x + p.w};
    const std::vector<float> my = resizing ? std::vector<float>{p.y + p.h} : std::vector<float>{p.y, p.y + p.h / 2, p.y + p.h};
    const float g = (float)d.grid;
    auto toGrid = [&](float v) { return std::round(v / g) * g; };
    if (best(xs, mx, dx, lx)) {
        if (resizing) p.w = std::max(1.0f, p.w + dx);
        else p.x += dx;
        out.guidesX.push_back(lx);
    } else if (g > 0) {
        if (resizing) p.w = std::max(g, toGrid(p.x + p.w) - p.x);
        else p.x = toGrid(p.x);
    }
    if (best(ys, my, dy, ly)) {
        if (resizing) p.h = std::max(1.0f, p.h + dy);
        else p.y += dy;
        out.guidesY.push_back(ly);
    } else if (g > 0) {
        if (resizing) p.h = std::max(g, toGrid(p.y + p.h) - p.y);
        else p.y = toGrid(p.y);
    }
    out.box = p;
    return out;
}

Box boundsOf(const Document& d, const std::vector<std::string>& ids) {
    Box b;
    bool any = false;
    for (auto& id : ids)
        if (const Element* e = d.find(id)) {
            if (!any) b = {e->x, e->y, e->w, e->h};
            else {
                const float r = std::max(b.x + b.w, e->x + e->w), bt = std::max(b.y + b.h, e->y + e->h);
                b.x = std::min(b.x, e->x);
                b.y = std::min(b.y, e->y);
                b.w = r - b.x;
                b.h = bt - b.y;
            }
            any = true;
        }
    return b;
}

std::string group(Document& d, const std::vector<std::string>& ids) {
    std::vector<std::string> in;
    for (auto& e : d.elements)
        if (std::find(ids.begin(), ids.end(), e.id) != ids.end()) in.push_back(e.id);   // bottom first
    if (in.size() < 2) return {};
    int n = 1;
    std::string gid;
    for (;; ++n) {
        gid = "g" + std::to_string(n);
        if (std::none_of(d.elements.begin(), d.elements.end(), [&](const Element& e) { return e.group == gid; })) break;
    }
    // Gathered just below where the topmost of them is.
    std::vector<Element> taken;
    int top = -1;
    for (int i = 0; i < (int)d.elements.size(); ++i)
        if (std::find(in.begin(), in.end(), d.elements[(size_t)i].id) != in.end()) top = i;
    int insertAt = top + 1;
    for (int i = (int)d.elements.size() - 1; i >= 0; --i)
        if (std::find(in.begin(), in.end(), d.elements[(size_t)i].id) != in.end()) {
            taken.insert(taken.begin(), d.elements[(size_t)i]);
            d.elements.erase(d.elements.begin() + i);
            if (i < insertAt) --insertAt;
        }
    for (auto& e : taken) e.group = gid;
    d.elements.insert(d.elements.begin() + insertAt, taken.begin(), taken.end());
    return gid;
}

void ungroup(Document& d, const std::string& gid) {
    if (gid.empty()) return;
    for (auto& e : d.elements)
        if (e.group == gid) e.group.clear();
}

std::vector<std::string> members(const Document& d, const std::string& gid) {
    std::vector<std::string> out;
    if (gid.empty()) return out;
    for (auto& e : d.elements)
        if (e.group == gid) out.push_back(e.id);
    return out;
}

std::vector<std::string> pick(const Document& d, const std::string& id) {
    const Element* e = d.find(id);
    if (!e) return {};
    if (e->group.empty()) return {id};
    return members(d, e->group);
}

bool align(Document& d, const std::vector<std::string>& ids, const std::string& how, bool toPage) {
    std::vector<Element*> es;
    for (auto& id : ids)
        if (Element* e = d.find(id); e && !e->locked) es.push_back(e);
    if (es.empty()) return false;
    Box b = (toPage || ids.size() < 2) ? Box{0, 0, (float)d.width, (float)d.height} : boundsOf(d, ids);
    // A group lines up as one: its members keep their places within it.
    std::map<std::string, Box> groupBox;
    for (Element* e : es)
        if (!e->group.empty() && !groupBox.count(e->group)) groupBox[e->group] = boundsOf(d, members(d, e->group));
    bool changed = false;
    std::map<std::string, std::pair<float, float>> groupMove;
    for (Element* e : es) {
        const Box own = e->group.empty() ? Box{e->x, e->y, e->w, e->h} : groupBox[e->group];
        float nx = own.x, ny = own.y;
        if (how == "left") nx = b.x;
        else if (how == "centre") nx = b.x + (b.w - own.w) / 2;
        else if (how == "right") nx = b.x + b.w - own.w;
        else if (how == "top") ny = b.y;
        else if (how == "middle") ny = b.y + (b.h - own.h) / 2;
        else if (how == "bottom") ny = b.y + b.h - own.h;
        else return false;
        const float mx = std::round(nx - own.x), my = std::round(ny - own.y);
        if (!e->group.empty()) groupMove[e->group] = {mx, my};
        else if (mx != 0 || my != 0) {
            e->x += mx;
            e->y += my;
            changed = true;
        }
    }
    for (auto& [gid, mv] : groupMove)
        for (auto& e : d.elements)
            if (e.group == gid && (mv.first != 0 || mv.second != 0)) {
                e.x += mv.first;
                e.y += mv.second;
                changed = true;
            }
    return changed;
}

bool distribute(Document& d, const std::vector<std::string>& ids, bool across) {
    std::vector<Element*> es;
    for (auto& id : ids)
        if (Element* e = d.find(id); e && !e->locked) es.push_back(e);
    if (es.size() < 3) return false;
    std::sort(es.begin(), es.end(), [&](Element* a, Element* b) { return across ? a->x < b->x : a->y < b->y; });
    float total = 0;
    for (Element* e : es) total += across ? e->w : e->h;
    const float start = across ? es.front()->x : es.front()->y;
    const float end = across ? es.back()->x + es.back()->w : es.back()->y + es.back()->h;
    const float gap = (end - start - total) / (float)(es.size() - 1);
    float at = start;
    bool changed = false;
    for (Element* e : es) {
        float& pos = across ? e->x : e->y;
        const float want = std::round(at);
        if (pos != want) { pos = want; changed = true; }
        at += (across ? e->w : e->h) + gap;
    }
    return changed;
}

Pose pose(const Element& e, float t, int pageW, int pageH) {
    Pose p;
    if (e.anim == Anim::None || e.animMs <= 0 || t >= e.animMs) return p;
    const float u = std::clamp(t / (float)e.animMs, 0.0f, 1.0f);
    const float k = 1 - (1 - u) * (1 - u) * (1 - u);   // ease out
    switch (e.anim) {
        case Anim::None: break;
        case Anim::Fade: p.opacity = k; break;
        case Anim::SlideLeft: p.dx = -(e.x + e.w) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;          // in from the left edge
        case Anim::SlideRight: p.dx = (pageW - e.x) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;        // from the right
        case Anim::SlideUp: p.dy = (pageH - e.y) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;           // up from the bottom
        case Anim::SlideDown: p.dy = -(e.y + e.h) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;          // down from the top
        case Anim::Pop: {
            // Grows past full size a little, then settles.
            const float s = 1.70158f, v = u - 1;
            p.scale = std::max(0.0f, 1 + (s + 1) * v * v * v + s * v * v);
            p.opacity = std::min(1.0f, u * 4);
            break;
        }
        case Anim::Wipe: p.reveal = k; break;
    }
    return p;
}

bool animating(const Document& d, float t) {
    for (auto& e : d.elements)
        if (e.visible && e.anim != Anim::None && t < e.animMs) return true;
    return false;
}

int entranceMs(const Document& d) {
    int ms = 0;
    for (auto& e : d.elements)
        if (e.visible && e.anim != Anim::None) ms = std::max(ms, e.animMs);
    return ms;
}

std::string fillPlaceholders(const std::string& text, const std::vector<std::pair<std::string, std::string>>& values, bool showMissing) {
    auto fill = [&](std::string part, bool& emptied) {
        bool hadTag = false, allEmpty = true;
        for (auto& [k, v] : values) {
            const std::string tag = "{" + k + "}";
            for (std::size_t at = 0; (at = part.find(tag, at)) != std::string::npos;) {
                hadTag = true;
                const std::string with = v.empty() && showMissing ? tag : v;
                if (!v.empty()) allEmpty = false;
                part.replace(at, tag.size(), with);
                at += with.size();
            }
        }
        std::string trimmed = part;
        trimmed.erase(0, trimmed.find_first_not_of(' '));
        trimmed.erase(trimmed.find_last_not_of(' ') + 1);
        emptied = hadTag && allEmpty && !showMissing && trimmed.empty();
        return part;
    };
    const std::string dot = "·";
    std::string out;
    std::size_t start = 0;
    while (true) {   // line by line
        const std::size_t nl = text.find('\n', start);
        const std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (line.find(dot) == std::string::npos) {
            bool e = false;
            out += fill(line, e);
        } else {
            std::vector<std::string> kept;
            std::size_t s = 0;
            while (true) {
                const std::size_t d = line.find(dot, s);
                std::string part = line.substr(s, d == std::string::npos ? std::string::npos : d - s);
                bool emptied = false;
                part = fill(part, emptied);
                part.erase(0, part.find_first_not_of(' '));
                part.erase(part.find_last_not_of(' ') + 1);
                if (!emptied && !part.empty()) kept.push_back(part);
                if (d == std::string::npos) break;
                s = d + dot.size();
            }
            for (std::size_t i = 0; i < kept.size(); ++i) out += (i ? "   " + dot + "   " : "") + kept[i];
        }
        if (nl == std::string::npos) break;
        out += '\n';
        start = nl + 1;
    }
    return out;
}

// ------------------------------------------------------------------ timers, QR codes, icons

std::string durationText(std::int64_t ms) {
    if (ms < 0) ms = 0;
    const std::int64_t s = ms / 1000, h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    char b[32];
    if (h > 0) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)sec);
    else std::snprintf(b, sizeof b, "%lld:%02lld", (long long)m, (long long)sec);
    return b;
}

TimerShown timerText(const Element& e, const Clock& c) {
    TimerShown t;
    auto counting = [&](std::int64_t remaining) {
        t.remainingMs = std::max<std::int64_t>(0, remaining);
        if (remaining <= 0) {
            t.ended = true;
            t.text = e.timerEnd;
            return;
        }
        t.text = durationText(remaining + 999);   // whole seconds left, rounded up: 0:01 until it's zero
        if (e.timerPulse && remaining <= 10000) {
            // Each second: bigger as it ticks, settling back.
            const float within = (float)(remaining % 1000) / 1000.0f;   // 1 → 0 through the second
            t.pulse = 1 + 0.12f * within * within;
        }
    };
    if (e.timerMode == "countdown") {
        counting((std::int64_t)e.timerSeconds * 1000 - c.elapsedMs);
    } else if (e.timerMode == "until") {
        // To the next time it's that time of day; a time passed less than twelve hours ago has ended.
        std::int64_t diff = (std::int64_t)e.timerUntil * 60 - c.localSeconds;
        if (diff < -12 * 3600) diff += 24 * 3600;
        counting(diff * 1000);
    } else if (e.timerMode == "stopwatch") {
        t.text = durationText(c.elapsedMs);
    } else if (e.timerMode == "live") {
        t.text = c.liveMs < 0 ? "Not live" : durationText(c.liveMs);
    } else if (e.timerMode == "golive") {
        if (c.goLiveInMs < 0) t.text = "No time set";
        else counting(c.goLiveInMs);
    } else if (e.timerMode == "remaining") {
        if (c.plannedMs <= 0) t.text = "No length set";
        else if (c.liveMs < 0) t.text = durationText(c.plannedMs);
        else counting(c.plannedMs - c.liveMs);
    } else if (e.timerMode == "sprint") {
        if (c.sprintLeftMs < 0) t.text = "No sprint";
        else counting(c.sprintLeftMs);
    } else if (e.timerMode == "next") {
        t.text = c.nextText.empty() ? "To be announced" : c.nextText;
    } else {   // clock, 24-hour
        char b[16];
        std::snprintf(b, sizeof b, "%02d:%02d", (c.localSeconds / 3600) % 24, (c.localSeconds / 60) % 60);
        t.text = b;
        if (e.timerDate && !c.dateText.empty()) t.text += "\n" + c.dateText;
    }
    if (!e.text.empty() && !t.ended) {   // words around it: "Starting in {time}"
        std::string w = e.text;
        const std::size_t at = w.find("{time}");
        t.text = at == std::string::npos ? w + " " + t.text : w.replace(at, 6, t.text);
    }
    return t;
}

bool shownNow(const Document& d, const Clock& c) {
    if (d.onlyWhileSprint && c.sprintLeftMs < 0) return false;
    if (d.onlyCelebrating && !(c.celebrateMs >= 0 && c.celebrateMs < kCelebrationMs)) return false;
    return true;
}

bool usesTime(const Document& d) {
    if (d.onlyWhileSprint || d.onlyCelebrating) return true;
    for (auto& e : d.elements)
        if (e.visible && (e.kind == Kind::Timer || (e.kind == Kind::Chat && e.chatFade > 0))) return true;
    return false;
}

bool usesChat(const Document& d) {
    for (auto& e : d.elements)
        if (e.visible && e.kind == Kind::Chat) return true;
    return false;
}

const std::vector<Icon>& icons() {
    // Drawn for NorthRay: simple, filled, 24 x 24.
    static const std::vector<Icon> v = {
        {"star", "Star", "M12 2 L14.9 8.6 L22 9.3 L16.6 14 L18.2 21 L12 17.3 L5.8 21 L7.4 14 L2 9.3 L9.1 8.6 Z"},
        {"heart", "Heart", "M12 21 C5 15.5 2 12.5 2 8.5 C2 5.5 4.3 3.5 7 3.5 C9 3.5 10.8 4.6 12 6.3 C13.2 4.6 15 3.5 17 3.5 C19.7 3.5 22 5.5 22 8.5 C22 12.5 19 15.5 12 21 Z"},
        {"arrow", "Arrow", "M3 10.5 L14 10.5 L14 5 L21.5 12 L14 19 L14 13.5 L3 13.5 Z"},
        {"check", "Tick", "M3.5 12.5 L6 10 L9.5 13.5 L18 5 L20.5 7.5 L9.5 18.5 Z"},
        {"play", "Play", "M7 4 L20 12 L7 20 Z"},
        {"chat", "Chat", "M3 4 L21 4 L21 16 L10 16 L5 20.5 L5.5 16 L3 16 Z"},
        {"music", "Music", "M9 4 L20 2 L20 15.5 C20 17.4 18.4 18.8 16.5 18.8 C14.6 18.8 13.5 17.6 13.5 16.2 C13.5 14.6 14.9 13.4 16.6 13.4 C17.2 13.4 17.7 13.5 18 13.7 L18 6.4 L11 7.7 L11 18 C11 19.9 9.4 21.3 7.5 21.3 C5.6 21.3 4.5 20.1 4.5 18.7 C4.5 17.1 5.9 15.9 7.6 15.9 C8.2 15.9 8.7 16 9 16.2 Z"},
        {"camera", "Camera", "M2 7 L7 7 L9 4 L15 4 L17 7 L22 7 L22 20 L2 20 Z"},
        {"pin", "Place", "M12 22 C7 15.5 4.5 12 4.5 9 C4.5 4.9 7.9 2 12 2 C16.1 2 19.5 4.9 19.5 9 C19.5 12 17 15.5 12 22 Z"},
        {"mail", "Mail", "M2 5 L22 5 L22 19 L2 19 Z"},
        {"bell", "Bell", "M12 2.5 C15.6 2.5 18 5.3 18 9 L18 14 L20.5 17.5 L3.5 17.5 L6 14 L6 9 C6 5.3 8.4 2.5 12 2.5 Z M9.5 19 L14.5 19 C14.5 20.4 13.4 21.5 12 21.5 C10.6 21.5 9.5 20.4 9.5 19 Z"},
        {"calendar", "Calendar", "M3 5 L7 5 L7 2.5 L9 2.5 L9 5 L15 5 L15 2.5 L17 2.5 L17 5 L21 5 L21 21 L3 21 Z"},
        {"clock", "Clock", "O 12 12 10"},
        {"dot", "Dot", "O 12 12 6"},
        {"book", "Book", "M3 4 C6 3.5 9.5 3.8 11.5 5.5 L11.5 20.5 C9.5 19 6 18.7 3 19.2 Z M12.5 5.5 C14.5 3.8 18 3.5 21 4 L21 19.2 C18 18.7 14.5 19 12.5 20.5 Z"},
        {"brush", "Brush", "M20.5 2.5 L22 4 L13 14.5 L10 11.5 Z M9 12.5 L12 15.5 C11.5 18.5 9.5 21 5.5 21 C3.5 21 2.5 20.5 2 20 C4 19.5 4.5 18 5 16 C5.5 14 7 12.5 9 12.5 Z"},
        {"mic", "Microphone", "M9 3.5 C9 2.4 10.3 1.5 12 1.5 C13.7 1.5 15 2.4 15 3.5 L15 11.5 C15 12.6 13.7 13.5 12 13.5 C10.3 13.5 9 12.6 9 11.5 Z M5.5 10 L7 10 C7 13 9.2 15.2 12 15.2 C14.8 15.2 17 13 17 10 L18.5 10 C18.5 13.5 16 16.3 12.8 16.7 L12.8 20 L16 20 L16 22 L8 22 L8 20 L11.2 20 L11.2 16.7 C8 16.3 5.5 13.5 5.5 10 Z"},
        {"globe", "Globe", "O 12 12 10"},
        {"sparkle", "Sparkle", "M12 2 C12.8 7.5 16.5 11.2 22 12 C16.5 12.8 12.8 16.5 12 22 C11.2 16.5 7.5 12.8 2 12 C7.5 11.2 11.2 7.5 12 2 Z"},
        {"moon", "Moon", "M15 2.5 C10 3.5 7 7.5 7 12 C7 16.5 10 20.5 15 21.5 C9 22.5 3 18 3 12 C3 6 9 1.5 15 2.5 Z"},
    };
    return v;
}

const Icon* iconById(const std::string& id) {
    for (auto& i : icons())
        if (id == i.id) return &i;
    return nullptr;
}

std::vector<std::vector<std::pair<float, float>>> iconFigures(const std::string& path) {
    std::vector<std::vector<std::pair<float, float>>> figs;
    std::string spaced;   // "M12 2" → "M 12 2"
    for (char ch : path) {
        if (std::isalpha((unsigned char)ch)) spaced += std::string(" ") + ch + " ";
        else spaced += ch;
    }
    std::stringstream ss(spaced);
    std::string tok;
    std::vector<std::pair<float, float>>* cur = nullptr;
    float lx = 0, ly = 0;
    auto rd = [&](float& v) { return static_cast<bool>(ss >> v); };
    while (ss >> tok) {
        if (tok == "M") {
            float x, y;
            if (!rd(x) || !rd(y)) break;
            figs.push_back({{x, y}});
            cur = &figs.back();
            lx = x, ly = y;
        } else if (tok == "L" && cur) {
            float x, y;
            if (!rd(x) || !rd(y)) break;
            cur->push_back({x, y});
            lx = x, ly = y;
        } else if (tok == "C" && cur) {
            float x1, y1, x2, y2, x, y;
            if (!rd(x1) || !rd(y1) || !rd(x2) || !rd(y2) || !rd(x) || !rd(y)) break;
            for (int i = 1; i <= 12; ++i) {   // flattened into short lines
                const float t = i / 12.0f, u = 1 - t;
                cur->push_back({u * u * u * lx + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x,
                                u * u * u * ly + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y});
            }
            lx = x, ly = y;
        } else if (tok == "O") {
            float cx, cy, r;
            if (!rd(cx) || !rd(cy) || !rd(r)) break;
            figs.push_back({});
            for (int i = 0; i < 48; ++i) figs.back().push_back({cx + r * std::cos(2 * kPi * i / 48), cy + r * std::sin(2 * kPi * i / 48)});
            cur = nullptr;
        } else if (tok == "Z") {
            cur = nullptr;
        }
    }
    return figs;
}

// ------------------------------------------------------------------ templates

namespace {
Rgba hex(const char* h) {
    Rgba c;
    parseColour(h, c);
    return c;
}
Element textEl(const char* name, const char* text, float x, float y, float w, float h, float size, const char* colour, bool bold,
               const char* align = "centre") {
    Element e;
    e.kind = Kind::Text;
    e.name = name;
    e.text = text;
    e.x = x, e.y = y, e.w = w, e.h = h;
    e.size = size;
    e.bold = bold;
    e.align = align;
    e.font = "Segoe UI";
    e.fill = Fill::solid(hex(colour));
    return e;
}
// Writing progress: the chapter's own words, on the CHAPTER label's line at the right.
Element chapterWordsElement() {
    Element e = textEl("Chapter words", "{chapterline}", 50, 40, 1000, 30, 22, "#3FCF8E", true, "right");
    e.anim = Anim::SlideUp;
    return e;
}
Element rectEl(const char* name, float x, float y, float w, float h, Fill f, float radius = 0) {
    Element e;
    e.kind = Kind::Rect;
    e.name = name;
    e.x = x, e.y = y, e.w = w, e.h = h;
    e.fill = f;
    e.radius = radius;
    return e;
}
Element ellipseEl(const char* name, float x, float y, float w, float h, Fill f) {
    Element e = rectEl(name, x, y, w, h, f);
    e.kind = Kind::Ellipse;
    return e;
}
// The NorthRay night with aurora light: a deep gradient and two soft glows.
void auroraPage(Document& d) {
    d.background = Fill::linear({{0, hex("#060B17")}, {0.55f, hex("#0B1A2E")}, {1, hex("#0E2A33")}}, 90);
    add(d, ellipseEl("Aurora (green)", -0.15f * d.width, -0.45f * d.height, 0.85f * d.width, 0.95f * d.height,
                     Fill::radial({{0, hex("#3FCF8E55")}, {0.55f, hex("#3FCF8E22")}, {1, hex("#3FCF8E00")}})));
    add(d, ellipseEl("Aurora (violet)", 0.4f * d.width, -0.35f * d.height, 0.75f * d.width, 0.8f * d.height,
                     Fill::radial({{0, hex("#7A5CFF4A")}, {0.6f, hex("#7A5CFF1C")}, {1, hex("#7A5CFF00")}})));
}
Document holding(const char* name, const char* title, const char* sub) {
    Document d;
    d.name = name;
    auroraPage(d);
    Element t = textEl("Title", title, 160, 380, 1600, 200, 132, "#F4F7FB", true);
    t.shadow = {true, 0, 8, 24, hex("#00000099")};
    t.anim = Anim::Pop;
    add(d, t);
    Element s = textEl("Subtitle", sub, 260, 580, 1400, 80, 44, "#9FB3C8", false);
    s.anim = Anim::Fade;
    s.animMs = 900;
    add(d, s);
    Element bar = rectEl("Accent line", 810, 690, 300, 6, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0), 3);
    bar.anim = Anim::Wipe;
    bar.animMs = 800;
    add(d, bar);
    Element soc = textEl("Socials", "{twitch}   ·   {youtube}   ·   {instagram}", 260, 900, 1400, 60, 34, "#CFE3F0", false);
    add(d, soc);
    return d;
}

// ---- The two looks for the writing, chat and now-playing templates (0.17): aurora (night panels, aurora light, glowing
// titles, gradient labels, round portraits) and bookish (parchment, serif type, ink and gold, arched portraits).
// Aurora's colours are the default brand kit's, so it follows the kit; bookish keeps its own.
constexpr const char* kSerif = "Georgia";
void lookCard(Document& d, bool bk, float x, float y, float w, float h, Anim a, const char* name = "Card") {
    Element c = rectEl(name, x, y, w, h,
                       bk ? Fill::linear({{0, hex("#F7EDD5")}, {1, hex("#EAD7AC")}}, 90) : Fill::linear({{0, hex("#0B1A2EF2")}, {1, hex("#10263EEE")}}, 90),
                       bk ? 12.0f : 28.0f);
    c.stroke = hex(bk ? "#A8853A" : "#3FCF8E44");
    c.strokeWidth = bk ? 3.0f : 2.0f;
    c.shadow = bk ? Shadow{true, 0, 8, 22, hex("#1E140A80"), ""} : Shadow{true, 0, 10, 30, hex("#00000099"), ""};
    c.anim = a;
    add(d, c);
    if (bk) {   // a fine second rule inside the edge, as on a book's title page
        Element r = rectEl("Inner rule", x + 9, y + 9, w - 18, h - 18, Fill::none(), 6);
        r.stroke = hex("#A8853A99");
        r.strokeWidth = 1.5f;
        r.anim = a;
        add(d, r);
    } else {   // aurora light, kept inside the card
        Element g1 = ellipseEl("Aurora light (violet)", x + w * 0.5f, y, w * 0.5f, h, Fill::radial({{0, hex("#7A5CFF30")}, {1, hex("#7A5CFF00")}}));
        g1.anim = a;
        add(d, g1);
        Element g2 = ellipseEl("Aurora light (green)", x, y + h * 0.2f, w * 0.45f, h * 0.8f, Fill::radial({{0, hex("#3FCF8E26")}, {1, hex("#3FCF8E00")}}));
        g2.anim = a;
        add(d, g2);
    }
}
Element lookLabel(bool bk, const char* name, const char* text, float x, float y, float w, float h, Anim a, const char* align = "left") {
    Element e = textEl(name, text, x, y, w, h, bk ? 24.0f : 22.0f, bk ? "#8C6A22" : "#3FCF8E", !bk, align);
    if (bk) {
        e.font = kSerif;
        e.italic = true;
    } else {
        e.fill = Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#22C3E6")}}, 0);   // gradient letters (0.15)
    }
    e.anim = a;
    return e;
}
Element lookTitle(bool bk, const char* name, const char* text, float x, float y, float w, float h, float size, Anim a, const char* align = "left") {
    Element e = textEl(name, text, x, y, w, h, size, bk ? "#2A1C0E" : "#F4F7FB", true, align);
    if (bk) e.font = kSerif;
    else e.glow = {true, 20, 1.0f, hex("#3FCF8E55"), ""};
    e.anim = a;
    return e;
}
Element lookBody(bool bk, const char* name, const char* text, float x, float y, float w, float h, float size, Anim a, const char* align = "left") {
    Element e = textEl(name, text, x, y, w, h, size, bk ? "#4A3622" : "#C9D6E3", false, align);
    if (bk) e.font = kSerif;
    e.anim = a;
    return e;
}
Element lookMuted(bool bk, const char* name, const char* text, float x, float y, float w, float h, float size, Anim a, const char* align = "left") {
    Element e = textEl(name, text, x, y, w, h, size, bk ? "#7A6243" : "#9FB3C8", false, align);
    if (bk) {
        e.font = kSerif;
        e.italic = true;
    }
    e.anim = a;
    return e;
}
// A rule under a heading: aurora's gradient line, or a gold line with a diamond at its middle.
void lookRule(Document& d, bool bk, float x, float y, float w, Anim a) {
    if (bk) {
        Element l = rectEl("Gold rule", x, y, w, 2, Fill::solid(hex("#A8853A")));
        l.anim = a;
        add(d, l);
        Element o = rectEl("Ornament", x + w / 2 - 6, y - 5, 12, 12, Fill::solid(hex("#A8853A")));
        o.rotation = 45;
        o.anim = a;
        add(d, o);
    } else {
        Element l = rectEl("Accent line", x, y, w, 4, Fill::linear({{0, hex("#3FCF8E")}, {0.5f, hex("#22C3E6")}, {1, hex("#7A5CFF")}}, 0), 2);
        l.anim = a == Anim::None ? Anim::Wipe : a;
        add(d, l);
    }
}
Element picEl(const char* name, const char* file, float x, float y, float w, float h, float radius) {
    Element p;
    p.kind = Kind::Image;
    p.name = name;
    p.file = file;
    p.cover = true;
    p.radius = radius;
    p.x = x, p.y = y, p.w = w, p.h = h;
    p.fill = Fill::none();
    return p;
}
// A portrait: round with a glowing ring (aurora) or arched with a gold edge (bookish), over a backing that shows until
// there's a picture.
void lookPortrait(Document& d, bool bk, const char* name, const char* file, float x, float y, float w, float h, Anim a, int ms) {
    Element back = bk ? rectEl("Portrait backing", x, y, w, h, Fill::linear({{0, hex("#D9C394")}, {1, hex("#C2A66E")}}, 90), w / 2)
                      : ellipseEl("Portrait backing", x, y, w, h, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 135));
    back.name = std::string(name) + " backing";
    back.anim = a;
    back.animMs = ms;
    add(d, back);
    Element p = picEl(name, file, x, y, w, h, 0);
    p.mask = bk ? "arch" : "circle";
    p.stroke = hex(bk ? "#A8853A" : "#3FCF8E");
    p.strokeWidth = 4;
    if (bk) p.shadow = {true, 0, 4, 12, hex("#1E140A66"), ""};
    else p.glow = {true, 18, 1.2f, hex("#3FCF8E88"), ""};
    p.anim = a;
    p.animMs = ms;
    add(d, p);
}
Element lookIcon(bool bk, const char* name, const char* icon, float x, float y, float w, float h, Anim a) {
    Element ic;
    ic.kind = Kind::Icon;
    ic.icon = icon;
    ic.name = name;
    ic.x = x, ic.y = y, ic.w = w, ic.h = h;
    ic.fill = bk ? Fill::linear({{0, hex("#D9B45A")}, {1, hex("#9A7424")}}, 90) : Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0);
    if (!bk) ic.glow = {true, 16, 1.0f, hex("#3FCF8E66"), ""};
    ic.anim = a;
    return ic;
}

Document bookCard(bool bk) {
    Document d;
    d.name = bk ? "Book card (bookish)" : "Book card";
    d.width = 900, d.height = 320;
    const Anim a = Anim::SlideLeft;
    lookCard(d, bk, 10, 10, 880, 300, a);
    Element cover = rectEl("Cover (until there is a picture)", 40, 40, 160, 240,
                           bk ? Fill::linear({{0, hex("#6B2E1E")}, {1, hex("#3E1A12")}}, 135) : Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 135), 8);
    cover.shadow = {true, 0, 6, 18, hex("#00000080"), ""};
    cover.anim = a;
    add(d, cover);
    Element ic = lookIcon(bk, "Book", "book", 85, 125, 70, 70, a);
    if (!bk) ic.fill = Fill::solid(hex("#F4F7FB")), ic.glow = {};
    add(d, ic);
    Element pic = picEl("Cover picture", "{cover}", 40, 40, 160, 240, 8);
    if (bk) pic.stroke = hex("#A8853A"), pic.strokeWidth = 2;
    pic.anim = a;
    add(d, pic);
    add(d, lookLabel(bk, "Label", bk ? "Now writing" : "NOW WRITING", 240, 48, 620, 34, a));
    add(d, lookTitle(bk, "Title", "{book}", 240, 86, 620, 120, bk ? 50.0f : 52.0f, a));
    lookRule(d, bk, 240, 214, 220, a);
    Element s = lookMuted(bk, "Series", "{series}", 240, 228, 620, 44, 28, a);
    s.animMs = 700;
    add(d, s);
    return d;
}
Document writingProgress(bool bk) {
    Document d;
    d.name = bk ? "Writing progress (bookish)" : "Writing progress";
    d.width = 1100, d.height = 300;
    const Anim a = Anim::SlideUp;
    lookCard(d, bk, 10, 10, 1080, 280, a);
    add(d, lookLabel(bk, "Label", bk ? "Chapter" : "CHAPTER", 50, 36, 700, 34, a));
    Element cw = lookLabel(bk, "Chapter words", "{chapterline}", 50, 36, 1000, 34, a, "right");
    add(d, cw);
    add(d, lookTitle(bk, "Chapter", "{chapter}", 50, 72, 1000, 70, 50, a));
    lookRule(d, bk, 50, 150, 260, a);
    Element t = lookBody(bk, "Today", "{today}", 50, 164, 1000, 46, 34, a);
    t.animMs = 650;
    add(d, t);
    Element b = lookMuted(bk, "Book and streak", "{bookline}  ·  {streakline}", 50, 218, 1000, 40, 26, a);
    b.animMs = 750;
    add(d, b);
    return d;
}
Document chapterCard(bool bk) {
    Document d;
    d.name = bk ? "Chapter card (bookish)" : "Chapter card";
    d.width = 1100, d.height = 320;
    const Anim a = Anim::SlideUp;
    lookCard(d, bk, 10, 10, 1080, 300, a);
    Element pic = picEl("Place picture", "{placepic}", bk ? 830 : 800, bk ? 36 : 40, bk ? 200 : 250, bk ? 248 : 240, 18);
    if (bk) {
        pic.mask = "arch";
        pic.stroke = hex("#A8853A"), pic.strokeWidth = 4;
        pic.shadow = {true, 0, 4, 12, hex("#1E140A66"), ""};
    } else {
        pic.glow = {true, 16, 1.0f, hex("#7A5CFF66"), ""};
    }
    pic.anim = a;
    add(d, pic);
    add(d, lookLabel(bk, "Status", "{status}", 50, 36, 720, 34, a));
    add(d, lookTitle(bk, "Chapter", "{chapter}", 50, 72, 740, 70, 50, a));
    lookRule(d, bk, 50, 148, 240, a);
    Element where = lookBody(bk, "Where and when", "{place}  ·  {storyday}", 50, 160, 740, 42, 30, a);
    where.animMs = 650;
    add(d, where);
    Element pov = lookMuted(bk, "Viewpoint", "{viewpoint}", 50, 206, 740, 40, 26, a);
    pov.animMs = 700;
    add(d, pov);
    Element t = lookMuted(bk, "Target", "{targetline}", 50, 250, 740, 40, 26, a);
    t.animMs = 750;
    add(d, t);
    return d;
}
Document characterCard(bool bk) {
    Document d;
    d.name = bk ? "Character card (bookish)" : "Character card";
    d.width = 760, d.height = 260;
    const Anim a = Anim::SlideLeft;
    lookCard(d, bk, 10, 10, 740, 240, a);
    if (bk) lookPortrait(d, bk, "Portrait", "{viewpointpic}", 50, 30, 150, 200, a, 500);
    else lookPortrait(d, bk, "Portrait", "{viewpointpic}", 42, 42, 176, 176, a, 500);
    add(d, lookLabel(bk, "Label", bk ? "Through the eyes of" : "THROUGH THE EYES OF", 250, 70, 480, 34, a));
    Element n = lookTitle(bk, "Name", "{viewpoint}", 250, 106, 480, 80, 50, a);
    n.animMs = 650;
    add(d, n);
    lookRule(d, bk, 250, 188, 200, a);
    return d;
}
Document castList(bool bk) {
    Document d;
    d.name = bk ? "Cast list (bookish)" : "Cast list";
    d.width = 1200, d.height = bk ? 340.0f : 300;
    const Anim a = Anim::SlideUp;
    lookCard(d, bk, 10, 10, 1180, d.height - 20, a);
    add(d, lookLabel(bk, "Label", bk ? "In this chapter" : "IN THIS CHAPTER", 40, 26, 1120, 34, a));
    for (int i = 0; i < 6; ++i) {
        const float x = 40 + i * 188.0f;
        const std::string n = std::to_string(i + 1);
        const std::string name = "Portrait " + n, pic = "{cast" + n + "pic}", who = "{cast" + n + "}", nameEl = "Name " + n;
        if (bk) lookPortrait(d, bk, name.c_str(), pic.c_str(), x + 33, 72, 106, 150, a, 500 + i * 80);
        else lookPortrait(d, bk, name.c_str(), pic.c_str(), x + 24, 76, 124, 124, a, 500 + i * 80);
        Element t = bk ? lookBody(bk, nameEl.c_str(), who.c_str(), x, 236, 172, 70, 22, a, "centre")
                       : textEl(nameEl.c_str(), who.c_str(), x, 210, 172, 60, 22, "#F4F7FB", true, "centre");
        t.anim = a;
        t.animMs = 500 + i * 80;
        add(d, t);
    }
    return d;
}
Document sprintTimer(bool bk) {
    Document d;
    d.name = bk ? "Sprint timer (bookish)" : "Sprint timer";
    d.width = 700, d.height = 220;
    d.onlyWhileSprint = true;
    const Anim a = Anim::Pop;
    lookCard(d, bk, 10, 10, 680, 200, a);
    add(d, lookLabel(bk, "Label", bk ? "A writing sprint" : "WRITING SPRINT", 40, 30, 620, 34, a));
    Element t;
    t.kind = Kind::Timer;
    t.name = "Time left";
    t.timerMode = "sprint";
    t.timerEnd = bk ? "Pens down!" : "Time!";
    t.x = 40, t.y = 64, t.w = 620, t.h = 86;
    t.size = 72;
    t.bold = true;
    t.align = "left";
    if (bk) {
        t.font = kSerif;
        t.fill = Fill::solid(hex("#2A1C0E"));
    } else {
        t.fill = Fill::linear({{0, hex("#F4F7FB")}, {1, hex("#BFF3DC")}}, 90);
        t.glow = {true, 22, 1.2f, hex("#3FCF8E66"), ""};
    }
    t.anim = a;
    add(d, t);
    add(d, lookMuted(bk, "Words", "{sprintline}", 40, 154, 620, 40, 28, a));
    return d;
}
Document celebration(bool bk) {
    Document d;
    d.name = bk ? "Celebration (bookish)" : "Celebration";
    d.width = 1000, d.height = 260;
    d.onlyCelebrating = true;
    Element glow = rectEl("Glow", 20, 20, 960, 220, Fill::radial({{0, hex(bk ? "#E8C46A99" : "#3FCF8E99")}, {1, hex(bk ? "#E8C46A00" : "#3FCF8E00")}}), 110);
    glow.anim = Anim::Fade;
    add(d, glow);
    if (bk) {
        lookCard(d, bk, 80, 60, 840, 140, Anim::Pop, "Banner");
    } else {
        Element card = rectEl("Banner", 80, 60, 840, 140, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0), 70);
        card.shadow = {true, 0, 10, 26, hex("#00000099"), ""};
        card.anim = Anim::Pop;
        add(d, card);
    }
    Element ic = lookIcon(bk, "Sparkle", bk ? "star" : "sparkle", 112, 90, 80, 80, Anim::Pop);
    if (!bk) ic.fill = Fill::solid(hex("#FFFFFF")), ic.glow = {true, 14, 1.0f, hex("#FFFFFF88"), ""};
    add(d, ic);
    Element t = textEl("Words", "{celebration}", 204, 90, 690, 80, 40, bk ? "#2A1C0E" : "#FFFFFF", true, "left");
    if (bk) t.font = kSerif, t.italic = true;
    t.anim = Anim::Pop;
    t.animMs = 650;
    add(d, t);
    return d;
}
Document chatBox(bool bk) {
    Document d;
    d.name = bk ? "Chat box (bookish)" : "Chat box";
    d.width = 600, d.height = 760;
    lookCard(d, bk, 0, 0, 600, 64, Anim::None, "Header");
    Element ic = lookIcon(bk, "Chat icon", "chat", 22, 14, 36, 36, Anim::None);
    add(d, ic);
    add(d, lookLabel(bk, "Label", bk ? "From the reading room" : "LIVE CHAT", 72, 15, 500, 34, Anim::None));
    Element e;
    e.kind = Kind::Chat;
    e.name = "Chat";
    e.x = 0, e.y = 80, e.w = 600, e.h = 680;
    e.size = 28;
    e.chatCount = 8;
    e.chatSide = "left";
    if (bk) {
        e.font = kSerif;
        e.fill = Fill::solid(hex("#2A1C0E"));
        e.chatBubble = hex("#F7EDD5F0");
        e.chatBubbleBrand = "";
    } else {
        e.fill = Fill::solid(hex("#F4F7FB"));
    }
    add(d, e);
    return d;
}
Document nowPlaying(bool bk) {
    Document d;
    d.name = bk ? "Now playing (bookish)" : "Now playing";
    d.width = 760, d.height = 150;
    d.onlyWhilePlaying = true;
    const Anim a = Anim::SlideLeft;
    lookCard(d, bk, 10, 10, 740, 130, a);
    add(d, lookIcon(bk, "Music", "music", 40, 35, 80, 80, a));
    add(d, lookLabel(bk, "Label", bk ? "Now playing" : "NOW PLAYING", 145, 22, 580, 34, a));
    add(d, lookTitle(bk, "Song", "{song}", 145, 52, 580, 48, 38, a));
    add(d, lookMuted(bk, "Artist", "{artist}", 145, 98, 580, 34, 26, a));
    return d;
}

// ---- The bookish look for the holding screens and the rest of the stream (0.18).
// A full page of paper: warm parchment, darker towards the edges, inside a gold double frame.
void bookishPage(Document& d) {
    const float W = (float)d.width, H = (float)d.height;
    d.background = Fill::linear({{0, hex("#F5E8C8")}, {1, hex("#E4CD9A")}}, 90);
    add(d, ellipseEl("Paper light", W * 0.1f, H * 0.05f, W * 0.8f, H * 0.9f, Fill::radial({{0, hex("#FFF8E655")}, {1, hex("#FFF8E600")}})));
    const float m = std::min(W, H) * 0.037f;
    Element outer = rectEl("Gold frame", m, m, W - 2 * m, H - 2 * m, Fill::none(), 8);
    outer.stroke = hex("#A8853A");
    outer.strokeWidth = 4;
    add(d, outer);
    Element inner = rectEl("Inner rule", m + 14, m + 14, W - 2 * m - 28, H - 2 * m - 28, Fill::none(), 4);
    inner.stroke = hex("#A8853A99");
    inner.strokeWidth = 1.5f;
    add(d, inner);
    for (auto [cx, cy] : std::vector<std::pair<float, float>>{{m + 7, m + 7}, {W - m - 7, m + 7}, {m + 7, H - m - 7}, {W - m - 7, H - m - 7}}) {
        Element o = rectEl("Corner ornament", cx - 9, cy - 9, 18, 18, Fill::solid(hex("#A8853A")));
        o.rotation = 45;
        add(d, o);
    }
}
Document holdingBookish(const char* name, const char* title, const char* sub) {
    Document d;
    d.name = std::string(name) + " (bookish)";
    bookishPage(d);
    Element above = lookLabel(true, "Above", "{name}", 260, 290, 1400, 60, Anim::Fade, "centre");
    above.size = 40;   // a full page: larger than a card's label
    add(d, above);
    Element t = lookTitle(true, "Title", title, 160, 360, 1600, 200, 124, Anim::Pop, "centre");
    add(d, t);
    lookRule(d, true, 760, 590, 400, Anim::Fade);
    Element s = lookMuted(true, "Subtitle", sub, 260, 620, 1400, 80, 44, Anim::Fade, "centre");
    s.animMs = 900;
    add(d, s);
    add(d, lookMuted(true, "Socials", "{twitch}   ·   {youtube}   ·   {instagram}", 260, 880, 1400, 60, 32, Anim::None, "centre"));
    return d;
}
Document lowerThirdBookish() {
    Document d;
    d.name = "Lower third (bookish)";
    d.width = 1000, d.height = 180;
    lookCard(d, true, 20, 30, 900, 120, Anim::SlideLeft, "Bar");
    add(d, lookTitle(true, "Name", "{name}", 60, 42, 820, 58, 46, Anim::SlideLeft));
    Element r = lookMuted(true, "Role", "Author · Artist · Broadcaster", 60, 100, 820, 40, 28, Anim::SlideLeft);
    r.animMs = 700;
    add(d, r);
    return d;
}
Document facecamBookish() {
    Document d;
    d.name = "Facecam frame (bookish)";
    d.width = 680, d.height = 420;
    // "Frame" first, with the same opening as the aurora frame: scene templates fit the camera into it.
    Element f = rectEl("Frame", 20, 20, 640, 360, Fill::none(), 12);
    f.stroke = hex("#A8853A");
    f.strokeWidth = 10;
    f.shadow = {true, 0, 6, 18, hex("#1E140A88"), ""};
    add(d, f);
    Element in = rectEl("Inner rule", 32, 32, 616, 336, Fill::none(), 6);
    in.stroke = hex("#F5E8C8AA");
    in.strokeWidth = 2;
    add(d, in);
    lookCard(d, true, 40, 350, 260, 56, Anim::None, "Name tag");
    add(d, lookTitle(true, "Name", "{name}", 40, 358, 260, 40, 26, Anim::None, "centre"));
    return d;
}
Document socialsBookish() {
    Document d;
    d.name = "Socials bar (bookish)";
    d.width = 1200, d.height = 90;
    lookCard(d, true, 4, 10, 1192, 70, Anim::None, "Bar");
    add(d, lookBody(true, "Handles", "{twitch}   ·   {youtube}   ·   {instagram}", 30, 22, 1140, 46, 30, Anim::None, "centre"));
    return d;
}
Document twitchPanelBookish() {
    Document d;
    d.name = "Twitch panel (bookish)";
    d.width = 320, d.height = 100;
    d.background = Fill::linear({{0, hex("#F5E8C8")}, {1, hex("#E4CD9A")}}, 90);
    Element edge = rectEl("Gold edge", 4, 4, 312, 92, Fill::none(), 4);
    edge.stroke = hex("#A8853A");
    edge.strokeWidth = 3;
    add(d, edge);
    add(d, lookTitle(true, "Label", "About me", 0, 24, 320, 52, 38, Anim::None, "centre"));
    return d;
}
Document showTitleBookish() {
    Document d;
    d.name = "Show title (bookish)";
    d.width = 1100, d.height = 220;
    lookCard(d, true, 10, 10, 1080, 200, Anim::Fade);
    Element pill = rectEl("On air", 40, 40, 170, 48, Fill::linear({{0, hex("#9B3A2C")}, {1, hex("#6E2219")}}, 90), 24);
    pill.stroke = hex("#D9B45A");
    pill.strokeWidth = 2;
    pill.anim = Anim::Pop;
    add(d, pill);
    Element oa = textEl("On air text", "ON AIR", 40, 44, 170, 40, 24, "#F5E3B0", true);
    oa.font = kSerif;
    add(d, oa);
    add(d, lookIcon(true, "Microphone", "mic", 960, 50, 100, 100, Anim::Fade));
    add(d, lookTitle(true, "Show", "{show}", 40, 96, 900, 66, 54, Anim::SlideLeft));
    Element e = lookMuted(true, "Episode", "{episode}", 40, 158, 900, 40, 28, Anim::SlideLeft);
    e.animMs = 700;
    add(d, e);
    return d;
}
}  // namespace

bool addChapterWords(Document& d) {
    if (d.name != "Writing progress") return false;
    const Element* label = nullptr;
    for (const Element& e : d.elements) {
        if (e.kind == Kind::Text && (e.text.find("{chapterline}") != std::string::npos || e.text.find("{chapterwords}") != std::string::npos))
            return false;   // it has one (or they've put it somewhere of their own)
        if (e.kind == Kind::Text && e.name == "Label" && e.text == "CHAPTER") label = &e;
    }
    if (!label) return false;   // changed past recognising: left alone
    Element e = chapterWordsElement();
    e.x = label->x, e.y = label->y, e.w = label->w, e.h = label->h;
    e.size = label->size;
    e.font = label->font;   // brand fonts are in the name ("brand:heading")
    e.fill = label->fill;   // with its brand link
    e.bold = label->bold;
    e.italic = label->italic;
    e.anim = label->anim;
    e.animMs = label->animMs;
    return !add(d, e).empty();
}

const std::vector<Template>& templates() {
    static const std::vector<Template> t = [] {
        std::vector<Template> v;
        v.push_back({"starting-soon", "Starting Soon", "Holding screens", holding("Starting Soon", "Starting soon", "Grab a drink, we'll be live in a moment")});
        v.push_back({"be-right-back", "Be Right Back", "Holding screens", holding("Be Right Back", "Be right back", "Don't go anywhere")});
        v.push_back({"ending", "Ending", "Holding screens", holding("Ending", "Thanks for watching", "See you next stream")});
        {
            Document d;
            d.name = "Lower third";
            d.width = 1000;
            d.height = 180;
            Element bg = rectEl("Bar", 20, 30, 900, 120, Fill::linear({{0, hex("#0B1A2EF0")}, {1, hex("#10213AE0")}}, 0), 18);
            bg.shadow = {true, 0, 8, 20, hex("#00000080")};
            bg.anim = Anim::SlideLeft;
            add(d, bg);
            Element acc = rectEl("Accent", 20, 30, 12, 120, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 90), 6);
            acc.anim = Anim::SlideLeft;
            add(d, acc);
            Element n = textEl("Name", "{name}", 60, 44, 820, 56, 46, "#F4F7FB", true, "left");
            n.anim = Anim::SlideLeft;
            add(d, n);
            Element r = textEl("Role", "Author · Artist · Broadcaster", 60, 100, 820, 40, 28, "#9FB3C8", false, "left");
            r.anim = Anim::SlideLeft;
            r.animMs = 700;
            add(d, r);
            v.push_back({"lower-third", "Lower third", "On stream", d});
        }
        {
            Document d;
            d.name = "Facecam frame";
            d.width = 680;
            d.height = 420;
            Element f = rectEl("Frame", 20, 20, 640, 360, Fill::none(), 28);
            f.stroke = hex("#3FCF8E");
            f.strokeWidth = 8;
            f.shadow = {true, 0, 0, 24, hex("#3FCF8E88")};
            add(d, f);
            Element tag = rectEl("Name tag", 40, 352, 260, 52, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0), 26);
            add(d, tag);
            add(d, textEl("Name", "{name}", 40, 358, 260, 40, 28, "#06101C", true));
            v.push_back({"facecam-frame", "Facecam frame", "On stream", d});
        }
        {
            Document d;
            d.name = "Socials bar";
            d.width = 1200;
            d.height = 90;
            add(d, rectEl("Bar", 0, 10, 1200, 70, Fill::solid(hex("#0B1A2ED0")), 35));
            add(d, textEl("Handles", "{twitch}   ·   {youtube}   ·   {instagram}", 30, 22, 1140, 46, 30, "#E8F1F7", false));
            v.push_back({"socials-bar", "Socials bar", "On stream", d});
        }
        {
            Document d;
            d.name = "Twitch panel";
            d.width = 320;
            d.height = 100;
            d.background = Fill::linear({{0, hex("#0B1A2E")}, {1, hex("#13304A")}}, 0);
            add(d, rectEl("Accent", 0, 92, 320, 8, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0)));
            add(d, textEl("Label", "ABOUT ME", 0, 26, 320, 48, 36, "#F4F7FB", true));
            v.push_back({"twitch-panel", "Twitch panel (320 × 100)", "Panels", d});
        }
        {
            Document d = holding("Offline banner", "Offline", "{schedule}");
            d.name = "Offline banner";
            v.push_back({"offline-banner", "Offline banner (1920 × 1080)", "Panels", d});
        }
        {
            Document d;
            d.name = "Logo watermark";
            d.width = 400;
            d.height = 400;
            Element e;
            e.kind = Kind::Image;
            e.name = "Logo";
            e.x = 0, e.y = 0, e.w = 400, e.h = 400;
            e.file = kBrandLogo;
            e.cover = false;
            e.fill = Fill::none();
            e.opacity = 0.6f;
            add(d, e);
            v.push_back({"logo-watermark", "Logo watermark (your brand logo)", "On stream", d});
        }
        // The writing, chat and now-playing templates in two looks (0.17): aurora (the same ids as before, so scene
        // templates still find them) and bookish.
        struct Pair {
            const char *id, *name, *bookName;
            Document (*make)(bool);
            const char* group;
        };
        static const Pair pairs[] = {
            {"chat-box", "Chat box (your Twitch chat, live)", "Chat box (parchment cards)", chatBox, "On stream"},
            {"now-playing", "Now playing (your music's title and artist)", "Now playing", nowPlaying, "On stream"},
            {"book-card", "Book card (your book's title and series)", "Book card", bookCard, "Writing"},
            {"writing-progress", "Writing progress (chapter and today's words)", "Writing progress", writingProgress, "Writing"},
            {"chapter-card", "Chapter card (status, place, story day, target)", "Chapter card", chapterCard, "Writing"},
            {"character-card", "Character card (the chapter's viewpoint character)", "Character card", characterCard, "Writing"},
            {"cast-list", "Cast list (who's in this chapter)", "Cast list", castList, "Writing"},
            {"sprint-timer", "Sprint timer (shows during a writing sprint)", "Sprint timer", sprintTimer, "Writing"},
            {"celebration", "Celebration (daily goal reached, sprint done)", "Celebration", celebration, "Writing"},
        };
        static std::vector<std::string> bookIds;   // kept alive: Template holds const char*
        bookIds.clear();
        for (auto& pr : pairs) bookIds.push_back(std::string(pr.id) + "-bookish");
        for (std::size_t i = 0; i < std::size(pairs); ++i) {
            v.push_back({pairs[i].id, pairs[i].name, pairs[i].group, pairs[i].make(false)});
            v.push_back({bookIds[i].c_str(), pairs[i].bookName, "Bookish (parchment, serif, ink and gold)", pairs[i].make(true)});
        }
        // And the rest of a stream in the bookish look (0.18).
        const char* bk = "Bookish (parchment, serif, ink and gold)";
        v.push_back({"starting-soon-bookish", "Starting Soon", bk, holdingBookish("Starting Soon", "Starting soon", "Settle in with a cup of tea")});
        v.push_back({"be-right-back-bookish", "Be Right Back", bk, holdingBookish("Be Right Back", "Be right back", "Keep your place, I'll return shortly")});
        v.push_back({"ending-bookish", "Ending", bk, holdingBookish("Ending", "Thank you for reading along", "Until the next chapter")});
        {
            Document d = holdingBookish("Offline banner", "Offline", "{schedule}");
            v.push_back({"offline-banner-bookish", "Offline banner (1920 × 1080)", bk, d});
        }
        v.push_back({"lower-third-bookish", "Lower third", bk, lowerThirdBookish()});
        v.push_back({"facecam-frame-bookish", "Facecam frame", bk, facecamBookish()});
        v.push_back({"socials-bar-bookish", "Socials bar", bk, socialsBookish()});
        v.push_back({"show-title-bookish", "Show title", bk, showTitleBookish()});
        v.push_back({"twitch-panel-bookish", "Twitch panel (320 × 100)", bk, twitchPanelBookish()});
        {   // A radio show's title and episode, with an on-air badge.
            Document d;
            d.name = "Show title";
            d.width = 1100;
            d.height = 220;
            Element card = rectEl("Card", 10, 10, 1080, 200, Fill::linear({{0, hex("#0B1A2EF0")}, {1, hex("#10213AE0")}}, 0), 30);
            card.shadow = {true, 0, 8, 20, hex("#00000080")};
            card.anim = Anim::Fade;
            add(d, card);
            Element pill = rectEl("On air", 40, 40, 170, 48, Fill::solid(hex("#E0566A")), 24);
            pill.anim = Anim::Pop;
            add(d, pill);
            add(d, textEl("On air text", "ON AIR", 40, 44, 170, 40, 24, "#FFFFFF", true));
            Element ic;
            ic.kind = Kind::Icon;
            ic.icon = "mic";
            ic.name = "Microphone";
            ic.x = 960, ic.y = 50, ic.w = 100, ic.h = 100;
            ic.fill = Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0);
            ic.anim = Anim::Fade;
            add(d, ic);
            Element t = textEl("Show", "{show}", 40, 96, 900, 66, 54, "#F4F7FB", true, "left");
            t.anim = Anim::SlideLeft;
            add(d, t);
            Element e = textEl("Episode", "{episode}", 40, 158, 900, 40, 28, "#9FB3C8", false, "left");
            e.anim = Anim::SlideLeft;
            e.animMs = 700;
            add(d, e);
            v.push_back({"show-title", "Show title (your radio show and episode)", "On stream", d});
        }
        // Keep each group together (the watermark was added last).
        std::stable_sort(v.begin(), v.end(), [](const Template& x, const Template& y) {
            auto rank = [](const char* g) {
                const std::string s(g);
                return s == "Holding screens" ? 0 : s == "On stream" ? 1 : s == "Writing" ? 2 : s.rfind("Bookish", 0) == 0 ? 3 : 4;
            };
            return rank(x.group) < rank(y.group);
        });
        const Brand b = defaultBrand();
        for (auto& x : v) linkToBrand(x.doc, b);
        return v;
    }();
    return t;
}

std::vector<std::vector<bool>> qrModules(const std::string& text) {
    std::vector<std::vector<bool>> m;
    if (text.empty() || text.size() > 1000) return m;
    try {
        const qrcodegen::QrCode q = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int n = q.getSize();
        m.assign((std::size_t)n, std::vector<bool>((std::size_t)n));
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) m[(std::size_t)y][(std::size_t)x] = q.getModule(x, y);
    } catch (...) {
        m.clear();
    }
    return m;
}


// ------------------------------------------------------------------ brand kit

const std::vector<std::string>& brandKeys() {
    static const std::vector<std::string> k = {"primary", "secondary", "text", "soft", "panel", "night"};
    return k;
}

Brand defaultBrand() {
    Brand b;
    b.colours = {{"primary", "Main colour", hex("#3FCF8E")}, {"secondary", "Second colour", hex("#7A5CFF")},
                 {"text", "Text", hex("#F4F7FB")},          {"soft", "Soft text", hex("#9FB3C8")},
                 {"panel", "Panels", hex("#0B1A2E")},       {"night", "Background", hex("#060B17")}};
    return b;
}

std::string toJson(const Brand& b) {
    json c = json::object();
    for (auto& x : b.colours) c[x.key] = colourText(x.colour);
    json f = json::array();
    for (auto& x : b.fontFiles) f.push_back(x);
    return json{{"format", "northray-brand"}, {"version", 1}, {"colours", c}, {"headingFont", b.headingFont},
                {"bodyFont", b.bodyFont}, {"fontFiles", f}, {"logo", b.logo},
                {"stinger", b.stinger}, {"stingerCutMs", b.stingerCutMs}, {"intro", b.intro}, {"outro", b.outro}}
        .dump(2);
}

Brand brandFromJson(const std::string& text) {
    Brand b = defaultBrand();
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return b;
    if (j.contains("colours") && j["colours"].is_object())
        for (auto& x : b.colours) {
            Rgba c;
            if (j["colours"].contains(x.key) && j["colours"][x.key].is_string() && parseColour(j["colours"][x.key].get<std::string>(), c)) {
                c.a = 255;   // brand colours are solid; designs choose how see-through
                x.colour = c;
            }
        }
    auto font = [&](const char* k, std::string& out) {
        if (j.contains(k) && j[k].is_string()) {
            const std::string v = j[k].get<std::string>().substr(0, 64);
            if (!v.empty() && v.rfind("brand:", 0) != 0) out = v;
        }
    };
    font("headingFont", b.headingFont);
    font("bodyFont", b.bodyFont);
    if (j.contains("fontFiles") && j["fontFiles"].is_array())
        for (auto& x : j["fontFiles"])
            if (x.is_string() && b.fontFiles.size() < 16 && !x.get<std::string>().empty() && x.get<std::string>().size() <= 1024)
                b.fontFiles.push_back(x.get<std::string>());
    if (j.contains("logo") && j["logo"].is_string() && j["logo"].get<std::string>().size() <= 1024) b.logo = j["logo"].get<std::string>();
    auto path = [&](const char* k, std::string& out) {
        if (j.contains(k) && j[k].is_string() && j[k].get<std::string>().size() <= 1024) out = j[k].get<std::string>();
    };
    path("stinger", b.stinger);
    path("intro", b.intro);
    path("outro", b.outro);
    if (j.contains("stingerCutMs") && j["stingerCutMs"].is_number_integer()) b.stingerCutMs = std::clamp(j["stingerCutMs"].get<int>(), 0, 10000);
    return b;
}

const BrandColour* brandColour(const Brand& b, const std::string& key) {
    for (auto& x : b.colours)
        if (x.key == key) return &x;
    return nullptr;
}

namespace {
// Every linkable colour in a design: (the colour, its link).
template <class D, class F>
void eachColour(D& d, F f) {
    auto fill = [&](auto& fl) {
        if (fl.type == Fill::Solid) f(fl.colour, fl.brand);
        if (fl.type == Fill::Linear || fl.type == Fill::Radial)
            for (auto& st : fl.stops) f(st.colour, st.brand);
    };
    fill(d.background);
    for (auto& e : d.elements) {
        fill(e.fill);
        f(e.stroke, e.strokeBrand);
        f(e.shadow.colour, e.shadow.brand);
        if (e.glow.on) f(e.glow.colour, e.glow.brand);   // an unused glow isn't linked by linkToBrand
        if (e.kind == Kind::Chat) f(e.chatBubble, e.chatBubbleBrand);
    }
}
}  // namespace

Document applyBrand(const Document& d, const Brand& b) {
    Document out = d;
    eachColour(out, [&](Rgba& c, std::string& link) {
        if (link.empty()) return;
        if (const BrandColour* x = brandColour(b, link)) c = {x->colour.r, x->colour.g, x->colour.b, c.a};
    });
    for (auto& e : out.elements) {
        if (e.font == kBrandHeading) e.font = b.headingFont;
        else if (e.font == kBrandBody) e.font = b.bodyFont;
        if (e.file == kBrandLogo) e.file = b.logo;
    }
    return out;
}

void linkToBrand(Document& d, const Brand& b) {
    eachColour(d, [&](Rgba& c, std::string& link) {
        if (!link.empty() || c.a == 0) return;
        for (auto& x : b.colours)
            if (x.colour.r == c.r && x.colour.g == c.g && x.colour.b == c.b) {
                link = x.key;
                return;
            }
    });
    for (auto& e : d.elements)
        if ((e.kind == Kind::Text || e.kind == Kind::Timer || e.kind == Kind::Chat) && e.font == "Segoe UI") e.font = e.bold ? kBrandHeading : kBrandBody;
}

bool usesBrand(const Document& d) {
    bool any = false;
    eachColour(d, [&](const Rgba&, const std::string& link) { any = any || !link.empty(); });
    for (auto& e : d.elements) any = any || e.font == kBrandHeading || e.font == kBrandBody || e.file == kBrandLogo;
    return any;
}

Document unlinkBrand(const Document& d, const Brand& b) {
    Document out = applyBrand(d, b);
    eachColour(out, [](Rgba&, std::string& link) { link.clear(); });
    return out;
}

}  // namespace ncap::design

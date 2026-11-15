#include "ncap/border.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace ncap::border {

namespace {
constexpr float kPi = 3.14159265358979f;
using sourceutil::colourText;
using sourceutil::parseColour;
float clampf(float v, float a, float b) { return std::isfinite(v) ? std::clamp(v, a, b) : a; }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
}  // namespace

std::string toJson(const Style& s) {
    return nlohmann::json{{"on", s.on},
                          {"width", s.width},
                          {"roundness", s.roundness},
                          {"colour", colourText(s.colour)},
                          {"gradient", s.gradient},
                          {"colour2", colourText(s.colour2)},
                          {"angle", s.angle},
                          {"glow", s.glow},
                          {"glowColour", colourText(s.glowColour)},
                          {"glowOpacity", s.glowOpacity},
                          {"offsetX", s.offsetX},
                          {"offsetY", s.offsetY}}
        .dump();
}

Style fromJson(const std::string& text) {
    Style s;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return s;
    auto b = [&](const char* k, bool& out) {
        if (j.contains(k) && j[k].is_boolean()) out = j[k].get<bool>();
    };
    auto n = [&](const char* k, float& out, float lo, float hi) {
        if (j.contains(k) && j[k].is_number()) out = clampf(j[k].get<float>(), lo, hi);
    };
    auto c = [&](const char* k, Rgba& out) {
        Rgba v;
        if (j.contains(k) && j[k].is_string() && parseColour(j[k].get<std::string>(), v)) out = v;
    };
    b("on", s.on);
    b("gradient", s.gradient);
    n("width", s.width, 0, 60);
    n("roundness", s.roundness, 0, 100);
    n("angle", s.angle, -360, 360);
    n("glow", s.glow, 0, 120);
    n("glowOpacity", s.glowOpacity, 0, 100);
    n("offsetX", s.offsetX, -60, 60);
    n("offsetY", s.offsetY, -60, 60);
    c("colour", s.colour);
    c("colour2", s.colour2);
    c("glowColour", s.glowColour);
    return s;
}

float radius(const Style& s, float w, float h) { return std::max(0.0f, std::min(w, h) * 0.5f * s.roundness / 100.0f); }

float distance(float x, float y, float w, float h, float r) {
    r = std::clamp(r, 0.0f, std::min(w, h) * 0.5f);
    // From the centre, folded into one quarter (the standard rounded-box distance).
    const float qx = std::fabs(x - w * 0.5f) - (w * 0.5f - r);
    const float qy = std::fabs(y - h * 0.5f) - (h * 0.5f - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

float coverage(float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); }

float margin(const Style& s) {
    if (!s.on || s.glow <= 0 || s.glowOpacity <= 0) return 0;
    return std::ceil(s.glow + std::max(std::fabs(s.offsetX), std::fabs(s.offsetY)));
}

Rgba ringColour(const Style& s, float x, float y, float w, float h) {
    if (!s.gradient) return s.colour;
    // Position along the direction, 0 at one corner of the item and 1 at the opposite one.
    const float a = s.angle * kPi / 180.0f;
    const float dx = std::cos(a), dy = std::sin(a);
    const float half = (std::fabs(dx) * w + std::fabs(dy) * h) * 0.5f;
    const float t = half > 0 ? std::clamp(((x - w * 0.5f) * dx + (y - h * 0.5f) * dy) / (2 * half) + 0.5f, 0.0f, 1.0f) : 0.0f;
    Rgba c;
    c.r = (std::uint8_t)std::lround(lerp(s.colour.r, s.colour2.r, t));
    c.g = (std::uint8_t)std::lround(lerp(s.colour.g, s.colour2.g, t));
    c.b = (std::uint8_t)std::lround(lerp(s.colour.b, s.colour2.b, t));
    c.a = (std::uint8_t)std::lround(lerp(s.colour.a, s.colour2.a, t));
    return c;
}

std::vector<std::uint8_t> render(const Style& s, int w, int h, int& outW, int& outH) {
    const int m = (int)margin(s);
    outW = std::max(0, w) + 2 * m;
    outH = std::max(0, h) + 2 * m;
    std::vector<std::uint8_t> px((std::size_t)outW * (std::size_t)outH * 4, 0);
    if (w <= 0 || h <= 0) return px;
    const float r = radius(s, (float)w, (float)h);
    const float ir = std::max(0.0f, r - s.width);
    for (int py = 0; py < outH; ++py)
        for (int pxl = 0; pxl < outW; ++pxl) {
            const float x = (float)(pxl - m) + 0.5f, y = (float)(py - m) + 0.5f;
            // The glow: the shape, moved by the offset, fading out over `glow` pixels.
            float gr = 0, gg = 0, gb = 0, ga = 0;
            if (m > 0) {
                const float d = distance(x - s.offsetX, y - s.offsetY, (float)w, (float)h, r);
                float a = d <= 0 ? 1.0f : 1.0f - std::clamp(d / s.glow, 0.0f, 1.0f);
                a = a * a * (3 - 2 * a);   // smooth
                ga = a * s.glowOpacity / 100.0f * s.glowColour.a / 255.0f;
                gr = s.glowColour.r, gg = s.glowColour.g, gb = s.glowColour.b;
            }
            // The item's own shape covers the glow (the camera shows there); the ring is drawn over it.
            const float inside = coverage(distance(x, y, (float)w, (float)h, r));
            ga *= 1 - inside;
            float ra = 0;
            Rgba rc{};
            if (s.width > 0) {
                const float inner = coverage(distance(x - s.width, y - s.width, (float)w - 2 * s.width, (float)h - 2 * s.width, ir));
                rc = ringColour(s, x, y, (float)w, (float)h);
                ra = inside * (1 - (w > 2 * s.width && h > 2 * s.width ? inner : 0.0f)) * rc.a / 255.0f;
            }
            // Ring over glow ("over" in straight alpha).
            const float oa = ra + ga * (1 - ra);
            std::uint8_t* o = &px[((std::size_t)py * (std::size_t)outW + (std::size_t)pxl) * 4];
            if (oa > 0) {
                o[0] = (std::uint8_t)std::lround((rc.r * ra + gr * ga * (1 - ra)) / oa);
                o[1] = (std::uint8_t)std::lround((rc.g * ra + gg * ga * (1 - ra)) / oa);
                o[2] = (std::uint8_t)std::lround((rc.b * ra + gb * ga * (1 - ra)) / oa);
                o[3] = (std::uint8_t)std::lround(oa * 255);
            }
        }
    return px;
}

const std::vector<Preset>& presets() {
    static const std::vector<Preset> p = [] {
        std::vector<Preset> v;
        Style s;
        s.on = true;
        Style clean = s;
        clean.width = 6;
        clean.roundness = 10;
        v.push_back({"Clean white", clean, false});
        Style neon = s;
        neon.width = 5;
        neon.roundness = 14;
        neon.gradient = true;
        neon.colour = {0, 229, 255, 255};
        neon.colour2 = {255, 61, 214, 255};
        neon.glow = 28;
        neon.glowColour = {120, 80, 255, 255};
        neon.glowOpacity = 70;
        v.push_back({"Neon glow", neon, false});
        Style soft = s;
        soft.width = 0;
        soft.roundness = 8;
        soft.glow = 30;
        soft.glowOpacity = 55;
        soft.offsetX = 0;
        soft.offsetY = 10;
        v.push_back({"Soft shadow, no ring", soft, false});
        Style circle = s;
        circle.width = 6;
        circle.roundness = 100;
        circle.glow = 18;
        circle.glowOpacity = 45;
        v.push_back({"Circle", circle, true});
        Style gold = s;
        gold.width = 8;
        gold.roundness = 6;
        gold.gradient = true;
        gold.colour = {255, 214, 120, 255};
        gold.colour2 = {176, 120, 32, 255};
        gold.angle = 90;
        gold.glow = 16;
        gold.glowOpacity = 50;
        gold.offsetY = 6;
        v.push_back({"Gold frame", gold, false});
        Style thin = s;
        thin.width = 2;
        thin.roundness = 0;
        thin.colour = {255, 255, 255, 200};
        v.push_back({"Thin and square", thin, false});
        return v;
    }();
    return p;
}

}  // namespace ncap::border

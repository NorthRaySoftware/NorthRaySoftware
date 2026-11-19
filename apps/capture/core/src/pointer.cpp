#include "ncap/pointer.hpp"

#include "ncap/placement.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace ncap::pointer {

using nlohmann::json;

std::string toJson(const Style& s) {
    return json{{"on", s.on},
                {"size", s.size},
                {"strength", s.strength},
                {"clicks", s.clicks},
                {"colour", sourceutil::colourText(s.colour)},
                {"second", sourceutil::colourText(s.second)}}
        .dump();
}

Style fromJson(const std::string& text) {
    Style s;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return s;
    auto num = [&](const char* k, float& out, float lo, float hi) {
        if (j.contains(k) && j[k].is_number() && std::isfinite(j[k].get<double>())) out = std::clamp(j[k].get<float>(), lo, hi);
    };
    auto flag = [&](const char* k, bool& out) {
        if (j.contains(k) && j[k].is_boolean()) out = j[k].get<bool>();
    };
    auto colour = [&](const char* k, Rgba& out) {
        Rgba c;
        if (j.contains(k) && j[k].is_string() && sourceutil::parseColour(j[k].get<std::string>(), c)) out = c;
    };
    flag("on", s.on);
    flag("clicks", s.clicks);
    num("size", s.size, 12, 240);
    num("strength", s.strength, 0, 100);
    colour("colour", s.colour);
    colour("second", s.second);
    return s;
}

bool toCanvas(const Transform& t, float sw, float sh, float areaLeft, float areaTop, float areaW, float areaH, float dx, float dy, float& cx, float& cy,
              float& scale) {
    if (sw <= 0 || sh <= 0 || areaW <= 0 || areaH <= 0) return false;
    if (dx < areaLeft || dy < areaTop || dx >= areaLeft + areaW || dy >= areaTop + areaH) return false;
    // Desktop to source pixels (the picture may be a different size from the area, e.g. scaled).
    const float px = (dx - areaLeft) * sw / areaW, py = (dy - areaTop) * sh / areaH;
    const float vw = sw - t.cropLeft - t.cropRight, vh = sh - t.cropTop - t.cropBottom;
    if (vw <= 0 || vh <= 0) return false;
    float u = (px - t.cropLeft) / vw, v = (py - t.cropTop) / vh;
    if (u < 0 || v < 0 || u > 1 || v > 1) return false;
    if (t.flipH) u = 1 - u;
    if (t.flipV) v = 1 - v;
    Vec2 c[4];
    cornersOf(t, sw, sh, c);
    cx = c[0].x + u * (c[1].x - c[0].x) + v * (c[3].x - c[0].x);
    cy = c[0].y + u * (c[1].y - c[0].y) + v * (c[3].y - c[0].y);
    scale = std::hypot(c[1].x - c[0].x, c[1].y - c[0].y) / vw;
    return true;
}

RingLook ringAt(const Style& s, long long age) {
    if (age < 0 || age >= kRingMs) return {s.size, 0};
    const float p = (float)age / kRingMs;
    const float ease = 1 - (1 - p) * (1 - p);   // quick out, slowing
    return {s.size * (0.5f + 1.5f * ease), 0.9f * (1 - p)};
}

void Rings::press(long long now, float x, float y, bool right) {
    prune(now);
    if (rings_.size() >= 16) rings_.erase(rings_.begin());
    rings_.push_back({now, x, y, right});
}

void Rings::prune(long long now) {
    rings_.erase(std::remove_if(rings_.begin(), rings_.end(), [now](const Ring& r) { return now - r.startMs >= kRingMs || now < r.startMs; }), rings_.end());
}

namespace {
std::vector<unsigned char> picture(int n, float (*alphaAt)(float)) {
    n = std::clamp(n, 4, 1024);
    std::vector<unsigned char> px((std::size_t)n * n * 4);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float dx = (x + 0.5f) / n * 2 - 1, dy = (y + 0.5f) / n * 2 - 1;
            const float a = std::clamp(alphaAt(std::sqrt(dx * dx + dy * dy)), 0.0f, 1.0f);
            unsigned char* p = &px[((std::size_t)y * n + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)std::lround(a * 255);
        }
    return px;
}
}  // namespace

std::vector<unsigned char> discPixels(int n) {
    // Full in the middle, fading from 60% of the radius to the edge.
    return picture(n, [](float r) { return r >= 1 ? 0.0f : r <= 0.6f ? 1.0f : 1 - (r - 0.6f) / 0.4f; });
}

std::vector<unsigned char> ringPixels(int n) {
    // A band between 78% and 96% of the radius, soft on both sides.
    return picture(n, [](float r) {
        const float d = std::fabs(r - 0.87f);
        return d >= 0.1f ? 0.0f : d <= 0.05f ? 1.0f : 1 - (d - 0.05f) / 0.05f;
    });
}

}  // namespace ncap::pointer

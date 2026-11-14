#include "ncap/transition.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace ncap::transition {

namespace {
const char* kKinds[] = {"cut", "fade", "slide", "swipe", "luma"};
const char* kEasings[] = {"linear", "ease-in-out", "ease-out"};
const char* kDirections[] = {"left", "right", "up", "down"};
const char* kPatterns[] = {"horizontal", "vertical", "diagonal", "circle", "clock", "blinds"};
template <typename E, std::size_t N>
bool parseOf(const std::string& s, const char* (&names)[N], E& out) {
    for (std::size_t i = 0; i < N; ++i)
        if (s == names[i]) {
            out = (E)i;
            return true;
        }
    return false;
}
constexpr float kPi = 3.14159265358979f;
}  // namespace

const char* key(Kind k) { return kKinds[(int)k]; }
const char* key(Easing e) { return kEasings[(int)e]; }
const char* key(Direction d) { return kDirections[(int)d]; }
const char* key(Pattern p) { return kPatterns[(int)p]; }
bool parse(const std::string& s, Kind& out) { return parseOf(s, kKinds, out); }
bool parse(const std::string& s, Easing& out) { return parseOf(s, kEasings, out); }
bool parse(const std::string& s, Direction& out) { return parseOf(s, kDirections, out); }
bool parse(const std::string& s, Pattern& out) { return parseOf(s, kPatterns, out); }

std::string toJson(const Settings& s) {
    return nlohmann::json{{"kind", key(s.kind)},           {"durationMs", s.durationMs}, {"easing", key(s.easing)},
                          {"direction", key(s.direction)}, {"pattern", key(s.pattern)},  {"softness", s.softness},
                          {"invert", s.invert}}
        .dump();
}

Settings fromJson(const std::string& text) {
    Settings s;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return s;
    auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    parse(str("kind"), s.kind);
    parse(str("easing"), s.easing);
    parse(str("direction"), s.direction);
    parse(str("pattern"), s.pattern);
    if (j.contains("durationMs") && j["durationMs"].is_number_integer()) s.durationMs = std::clamp(j["durationMs"].get<int>(), 50, 10000);
    if (j.contains("softness") && j["softness"].is_number()) s.softness = std::clamp(j["softness"].get<float>(), 0.0f, 0.5f);
    if (j.contains("invert") && j["invert"].is_boolean()) s.invert = j["invert"].get<bool>();
    return s;
}

float ease(Easing e, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (e) {
    case Easing::Linear: return t;
    case Easing::EaseOut: return 1 - (1 - t) * (1 - t) * (1 - t);
    case Easing::EaseInOut: return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
    }
    return t;
}

Offsets offsets(Kind k, Direction d, float p, float w, float h) {
    Offsets o;
    p = std::clamp(p, 0.0f, 1.0f);
    // The unit vector the new scene travels along, and how far it has to go.
    float dx = 0, dy = 0, dist = 0;
    switch (d) {
    case Direction::Left: dx = -1, dist = w; break;
    case Direction::Right: dx = 1, dist = w; break;
    case Direction::Up: dy = -1, dist = h; break;
    case Direction::Down: dy = 1, dist = h; break;
    }
    // The new scene starts one canvas away, opposite its direction, and arrives at 0.
    o.toX = -dx * dist * (1 - p);
    o.toY = -dy * dist * (1 - p);
    if (k == Kind::Slide) {   // the old scene is pushed out the same way
        o.fromX = dx * dist * p;
        o.fromY = dy * dist * p;
    }
    return o;
}

std::vector<std::uint8_t> pattern(Pattern p, int w, int h) {
    std::vector<std::uint8_t> out((std::size_t)std::max(0, w) * (std::size_t)std::max(0, h));
    if (w <= 0 || h <= 0) return out;
    const float cx = (w - 1) / 2.0f, cy = (h - 1) / 2.0f;
    const float maxR = std::sqrt(cx * cx + cy * cy);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float u = w > 1 ? (float)x / (float)(w - 1) : 0, v = h > 1 ? (float)y / (float)(h - 1) : 0;
            float l = 0;
            switch (p) {
            case Pattern::Horizontal: l = u; break;
            case Pattern::Vertical: l = v; break;
            case Pattern::Diagonal: l = (u + v) / 2; break;
            case Pattern::Circle: l = maxR > 0 ? std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / maxR : 0; break;
            case Pattern::Clock: {   // from 12 o'clock, clockwise
                float a = std::atan2(x - cx, -(y - cy));   // 0 at the top, increasing clockwise
                if (a < 0) a += 2 * kPi;
                l = a / (2 * kPi);
                break;
            }
            case Pattern::Blinds: l = std::fmod(u * 8, 1.0f); break;   // eight vertical blinds
            }
            out[(std::size_t)y * (std::size_t)w + (std::size_t)x] = (std::uint8_t)std::lround(std::clamp(l, 0.0f, 1.0f) * 255);
        }
    return out;
}

float lumaAlpha(float luma, float progress, float softness, bool invert) {
    if (invert) luma = 1 - luma;
    softness = std::clamp(softness, 0.0f, 0.5f);
    // Spread the threshold so that at 0 nothing shows and at 1 everything does, even with a soft edge.
    const float t = progress * (1 + softness) - softness;
    if (softness <= 0) return luma < t || progress >= 1 ? 1.0f : 0.0f;
    const float a = (t + softness - luma) / softness;
    return std::clamp(a, 0.0f, 1.0f);
}

}  // namespace ncap::transition

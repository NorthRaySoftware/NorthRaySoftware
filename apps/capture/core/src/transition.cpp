#include "ncap/transition.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ncap::transition {

namespace {
const char* kKinds[] = {"cut", "fade", "slide", "swipe", "luma", "stinger", "shader", "aurora"};
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
                          {"invert", s.invert},          {"stingerFile", s.stingerFile}, {"cutMs", s.cutMs},
                          {"shaderFile", s.shaderFile}}
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
    s.stingerFile = str("stingerFile");
    s.shaderFile = str("shaderFile");
    if (j.contains("cutMs") && j["cutMs"].is_number_integer()) s.cutMs = std::clamp(j["cutMs"].get<int>(), 0, 60000);
    return s;
}

namespace {
// Along the sweep (0 where it starts, 1 where it ends) and across it (0..1), for a point.
void sweepAxes(float u, float v, Direction d, float& along, float& across) {
    switch (d) {
    case Direction::Left: along = 1 - u, across = v; break;
    case Direction::Right: along = u, across = v; break;
    case Direction::Up: along = 1 - v, across = u; break;
    case Direction::Down: along = v, across = u; break;
    }
}
float smooth(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
bool hexRgb(const std::string& h, float out[3]) {
    if (h.size() != 7 || h[0] != '#') return false;
    for (int i = 0; i < 3; ++i) {
        const std::string part = h.substr(1 + i * 2, 2);
        char* end = nullptr;
        const long v = std::strtol(part.c_str(), &end, 16);
        if (!end || *end) return false;
        out[i] = (float)v / 255.0f;
    }
    return true;
}
}  // namespace

AuroraPixel aurora(float u, float v, float p, Direction d) {
    float along, across;
    sweepAxes(u, v, d, along, across);
    // A slanted, gently waving front: tilted across the picture, rippling as it goes.
    const float s = along * 0.8f + 0.1f + 0.12f * (across - 0.5f) + 0.025f * std::sin(across * 11.0f + p * 7.0f);
    const float c = -0.3f + p * 1.6f;   // the band's centre: off one edge at 0, off the other at 1
    AuroraPixel px;
    px.to = 1 - smooth(c - 0.06f, c + 0.02f, s);
    const float k = (s - c) / 0.09f;
    px.light = std::exp(-k * k) * (0.75f + 0.25f * std::sin(across * 37.0f + p * 23.0f));
    return px;
}

std::string auroraShader(Direction d, const std::string& colour1, const std::string& colour2) {
    float a[3] = {0.247f, 0.812f, 0.557f}, b[3] = {0.478f, 0.361f, 1.0f};
    hexRgb(colour1, a);
    hexRgb(colour2, b);
    const char* axes = d == Direction::Left ? "1 - uv.x, uv.y" : d == Direction::Right ? "uv.x, uv.y" : d == Direction::Up ? "1 - uv.y, uv.x" : "uv.y, uv.x";
    char buf[1800];
    std::snprintf(buf, sizeof buf, R"(// NorthRay's aurora sweep (built in; the same sums as transition::aurora).
float4 transition(float2 uv, float progress)
{
    float2 ax = float2(%s);   // along the sweep, across it
    float s = ax.x * 0.8 + 0.1 + 0.12 * (ax.y - 0.5) + 0.025 * sin(ax.y * 11.0 + progress * 7.0);
    float c = -0.3 + progress * 1.6;
    float toAmount = 1 - smoothstep(c - 0.06, c + 0.02, s);
    float k = (s - c) / 0.09;
    float light = exp(-k * k) * (0.75 + 0.25 * sin(ax.y * 37.0 + progress * 23.0));
    float3 col = lerp(float3(%.4f, %.4f, %.4f), float3(%.4f, %.4f, %.4f), saturate(ax.y + 0.3 * sin(progress * 3.0)));
    float4 scene = lerp(sampleFrom(uv), sampleTo(uv), toAmount);
    return float4(saturate(scene.rgb + col * light * 0.9), 1);
}
)",
                  axes, a[0], a[1], a[2], b[0], b[1], b[2]);
    return buf;
}

std::string shaderSource(const std::string& code) {
    return R"(cbuffer C : register(b0) { float4 pos_[4]; float4 uv_[4]; float4 colour_; float4 flags_; };
Texture2D fromTex : register(t0);
Texture2D toTex : register(t1);
SamplerState smp_ : register(s0);
struct V_ { float4 p : SV_Position; float2 t : TEXCOORD0; };
static const float2 resolution = colour_.xy;
float4 sampleFrom(float2 uv) { return fromTex.Sample(smp_, uv); }
float4 sampleTo(float2 uv) { return toTex.Sample(smp_, uv); }
#line 1 "transition"
)" + code + R"(
float4 ps(V_ i) : SV_Target { float4 c = transition(i.t, saturate(flags_.w)); return float4(c.rgb, 1); }
)";
}

std::string exampleShader() {
    return R"(// A NorthRay Capture transition (HLSL). Capture calls transition() for every pixel while the
// scenes change: uv is the pixel's place on the canvas (0,0 top-left to 1,1 bottom-right) and
// progress runs from 0 (the old scene) to 1 (the new one). sampleFrom(uv) and sampleTo(uv) give the
// two scenes' colours there; resolution is the canvas size in pixels.
//
// This one is a circle that grows from the centre, with a soft rim.
float4 transition(float2 uv, float progress)
{
    float2 d = (uv - 0.5) * float2(resolution.x / resolution.y, 1);
    float radius = progress * 1.2;
    float edge = smoothstep(radius - 0.04, radius, length(d));
    return lerp(sampleTo(uv), sampleFrom(uv), edge);
}
)";
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

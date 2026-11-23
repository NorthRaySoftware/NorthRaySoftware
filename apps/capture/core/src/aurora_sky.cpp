#include "ncap/aurora_sky.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ncap::aurora_sky {

using nlohmann::json;

const std::vector<std::string>& styles() {
    static const std::vector<std::string> v = {"aurora", "bookish", "fantasy", "scifi", "cosy", "minimal", "radio", "steampunk"};
    return v;
}

std::string toJson(const Settings& s) {
    return json{{"colour1", sourceutil::colourText(s.colour1)},
                {"colour2", sourceutil::colourText(s.colour2)},
                {"night", sourceutil::colourText(s.night)},
                {"colour3", sourceutil::colourText(s.colour3)},
                {"style", s.style},
                {"speed", s.speed},
                {"brightness", s.brightness},
                {"stars", s.stars},
                {"followBrand", s.followBrand},
                {"wave", s.wave},
                {"width", s.width},
                {"height", s.height}}
        .dump();
}

Settings fromJson(const std::string& text) {
    Settings s;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return s;
    auto colour = [&](const char* k, Rgba& out) {
        Rgba c;
        if (j.contains(k) && j[k].is_string() && sourceutil::parseColour(j[k].get<std::string>(), c)) out = {c.r, c.g, c.b, 255};
    };
    colour("colour1", s.colour1);
    colour("colour2", s.colour2);
    colour("night", s.night);
    colour("colour3", s.colour3);
    if (j.contains("style") && j["style"].is_string())
        for (auto& st : styles())
            if (j["style"].get<std::string>() == st) s.style = st;
    auto num = [&](const char* k, float lo, float hi, float& out) {
        if (j.contains(k) && j[k].is_number() && std::isfinite(j[k].get<double>())) out = std::clamp(j[k].get<float>(), lo, hi);
    };
    num("speed", 0.25f, 3.0f, s.speed);
    num("brightness", 0.3f, 1.5f, s.brightness);
    if (j.contains("stars") && j["stars"].is_boolean()) s.stars = j["stars"].get<bool>();
    if (j.contains("followBrand") && j["followBrand"].is_boolean()) s.followBrand = j["followBrand"].get<bool>();
    if (j.contains("wave") && j["wave"].is_boolean()) s.wave = j["wave"].get<bool>();
    auto size = [&](const char* k, int& out) {
        if (j.contains(k) && j[k].is_number_integer()) out = std::clamp(j[k].get<int>(), 16, 4096);
    };
    size("width", s.width);
    size("height", s.height);
    return s;
}

namespace {
float frac(float x) { return x - std::floor(x); }
float sat(float x) { return std::clamp(x, 0.0f, 1.0f); }
float smoothstep(float a, float b, float x) {
    const float t = sat((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}
Rgb lin(const Rgba& c) { return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f}; }
}  // namespace

float bandAt(const Pulse& p, float u) {
    if (!p.bands || p.count <= 0) return 0;
    const float x = std::clamp(u * p.count - 0.5f, 0.0f, (float)(p.count - 1));
    const int i = (int)x;
    const float f = x - i;
    const float a = p.bands[i], b = p.bands[std::min(i + 1, p.count - 1)];
    return a + (b - a) * f;
}

float waveLine(int k, float u, float t, float band) {
    const float ph = k * 2.1f;
    const float amp = 0.035f + 0.26f * band;   // quiet: a gentle swell; loud: tall
    return 0.56f + (k - 1) * 0.05f + amp * std::sin(u * (9.0f + 2.5f * k) - t * (1.1f + 0.35f * k) + ph) +
           0.015f * std::sin(u * 23.0f + t * 0.8f + ph);
}

namespace {
Rgb styled(const Settings& s, float u, float v, float seconds, int w, int h, const Pulse* pulse);
std::string styledShader(const Settings& s, bool pulse);
}  // namespace

// The sums (kept in step with the HLSL in shader()).
Rgb sample(const Settings& s, float u, float v, float seconds, int w, int h, const Pulse* pulse) {
    if (s.style != "aurora") return styled(s, u, v, seconds, w, h, pulse);
    const float band = pulse ? bandAt(*pulse, u) : 0;
    const float loud = pulse ? sat(pulse->level) : 0;
    const float t = seconds * s.speed;
    const Rgb night = lin(s.night), c1 = lin(s.colour1), c2 = lin(s.colour2);
    // The sky: the night colour, darker towards the top.
    const float sky = 0.55f + 0.45f * v;
    Rgb out{night.r * sky, night.g * sky, night.b * sky};
    float glow = 0;
    Rgb light{0, 0, 0};
    const bool wave = pulse && s.wave;
    // The wave follows the bands smoothed along the screen (neighbours blended), so it swells rather than kinks.
    const float wb = wave ? (bandAt(*pulse, u - 0.06f) + 2 * bandAt(*pulse, u - 0.03f) + 3 * band + 2 * bandAt(*pulse, u + 0.03f) +
                             bandAt(*pulse, u + 0.06f)) / 9
                          : 0;
    for (int k = 0; wave && k < 3; ++k) {
        // A ribbon: a bright thin line, a soft halo, and faint rays rising from it like an aurora's.
        const float d = waveLine(k, u, t, wb) - v;   // the line is below when positive
        const float thick = 0.007f + 0.016f * wb + 0.006f * loud;
        const float core = std::exp(-(d * d) / (thick * thick));
        const float halo = std::exp(-std::fabs(d) * 12.0f);
        const float rays = d > 0 ? std::exp(-d * 6.0f) * (0.55f + 0.45f * std::sin(u * 60.0f + t * 0.5f + k)) : 0.0f;
        const float amount = (1.3f * core + 0.5f * halo + 0.4f * rays) * (0.8f - 0.15f * k) * (0.45f + 1.2f * wb);
        const float m = sat(0.5f + 0.5f * std::sin(u * 2.2f + t * 0.09f + k * 1.3f));
        light.r += (c1.r + (c2.r - c1.r) * m) * amount;
        light.g += (c1.g + (c2.g - c1.g) * m) * amount;
        light.b += (c1.b + (c2.b - c1.b) * m) * amount;
        glow += amount;
    }
    for (int k = 0; !wave && k < 3; ++k) {
        const float ph = k * 1.7f;
        // The curtain's lower edge, rippling slowly.
        float edge = 0.62f - 0.13f * k + 0.06f * std::sin(u * 2.3f + t * 0.11f + ph) + 0.03f * std::sin(u * 5.1f - t * 0.17f + ph * 2);
        if (pulse) edge = edge + 0.12f - 0.3f * band;   // quiet: low; loud: risen
        const float d = edge - v;   // above the edge when positive
        const float curtain = d > 0 ? std::exp(-d * 5.5f) : std::exp(d * 45.0f);
        const float rays = 0.6f + 0.4f * std::sin(u * 40.0f + 3.0f * std::sin(u * 7.0f + t * 0.3f + ph) + t * 0.2f);
        float amount = curtain * rays * (0.55f - 0.12f * k);
        if (pulse) amount *= 0.25f + 1.4f * band;
        const float m = sat(0.5f + 0.5f * std::sin(u * 1.7f + t * 0.07f + ph));
        light.r += (c1.r + (c2.r - c1.r) * m) * amount;
        light.g += (c1.g + (c2.g - c1.g) * m) * amount;
        light.b += (c1.b + (c2.b - c1.b) * m) * amount;
        glow += amount;
    }
    const float bright = s.brightness * (pulse ? 0.75f + 0.5f * loud : 1.0f);
    out.r += light.r * bright;
    out.g += light.g * bright;
    out.b += light.b * bright;
    if (s.stars) {
        // A star in about one 2×2-pixel cell in 600, in the upper part, hidden where the aurora is bright.
        const float cx = std::floor(u * w / 2), cy = std::floor(v * h / 2);
        const float n = frac(std::sin(cx * 12.9898f + cy * 78.233f) * 43758.5453f);
        if (n > 0.9983f) {
            const float twinkle = 0.55f + 0.45f * std::sin(t * 1.3f + n * 6283.0f);
            const float star = twinkle * (1 - smoothstep(0.35f, 0.7f, v)) * (1 - sat(glow * 1.6f)) * 0.8f;
            out.r += star, out.g += star, out.b += star;
        }
    }
    return {sat(out.r), sat(out.g), sat(out.b)};
}

std::string shader(const Settings& s, bool pulse) {
    if (s.style != "aurora") return styledShader(s, pulse);
    const Rgb c1 = lin(s.colour1), c2 = lin(s.colour2), n = lin(s.night);
    char buf[6400];
    std::snprintf(buf, sizeof buf, R"(// NorthRay Capture's aurora sky (built in; the same sums as aurora_sky::sample).
cbuffer C : register(b0) { float4 pos_[4]; float4 uv_[4]; float4 colour_; float4 flags_; };
struct V_ { float4 p : SV_Position; float2 t : TEXCOORD0; };
Texture2D bandsTex : register(t0);   // the aurora pulse: red the band, green the loudness
SamplerState smp : register(s0);
static const int PULSE = %d;
static const int WAVE = %d;
float4 ps(V_ i) : SV_Target
{
    float u = i.t.x, v = i.t.y;
    float2 bl = PULSE ? bandsTex.SampleLevel(smp, float2(u, 0.5), 0).rg : float2(0, 0);
    float band = bl.x, loud = saturate(bl.y);
    float t = flags_.w * %.4f;
    float3 night = float3(%.4f, %.4f, %.4f), c1 = float3(%.4f, %.4f, %.4f), c2 = float3(%.4f, %.4f, %.4f);
    float3 col = night * (0.55 + 0.45 * v);
    float glow = 0;
    float3 light = 0;
    if (WAVE) {
        float wb = 0;
        [unroll] for (int j = -2; j <= 2; ++j) wb += bandsTex.SampleLevel(smp, float2(u + j * 0.03, 0.5), 0).r * (3 - abs(j));
        wb /= 9;
        [unroll] for (int k = 0; k < 3; ++k) {
            float ph = k * 2.1;
            float amp = 0.035 + 0.26 * wb;
            float centre = 0.56 + (k - 1) * 0.05 + amp * sin(u * (9.0 + 2.5 * k) - t * (1.1 + 0.35 * k) + ph) + 0.015 * sin(u * 23.0 + t * 0.8 + ph);
            float d = centre - v;
            float thick = 0.007 + 0.016 * wb + 0.006 * loud;
            float core = exp(-(d * d) / (thick * thick));
            float halo = exp(-abs(d) * 12.0);
            float rays = d > 0 ? exp(-d * 6.0) * (0.55 + 0.45 * sin(u * 60.0 + t * 0.5 + k)) : 0.0;
            float amount = (1.3 * core + 0.5 * halo + 0.4 * rays) * (0.8 - 0.15 * k) * (0.45 + 1.2 * wb);
            light += lerp(c1, c2, saturate(0.5 + 0.5 * sin(u * 2.2 + t * 0.09 + k * 1.3))) * amount;
            glow += amount;
        }
    }
    [unroll] for (int k = 0; k < (WAVE ? 0 : 3); ++k) {
        float ph = k * 1.7;
        float edge = 0.62 - 0.13 * k + 0.06 * sin(u * 2.3 + t * 0.11 + ph) + 0.03 * sin(u * 5.1 - t * 0.17 + ph * 2);
        if (PULSE) edge = edge + 0.12 - 0.3 * band;
        float d = edge - v;
        float curtain = d > 0 ? exp(-d * 5.5) : exp(d * 45.0);
        float rays = 0.6 + 0.4 * sin(u * 40.0 + 3.0 * sin(u * 7.0 + t * 0.3 + ph) + t * 0.2);
        float amount = curtain * rays * (0.55 - 0.12 * k);
        if (PULSE) amount *= 0.25 + 1.4 * band;
        light += lerp(c1, c2, saturate(0.5 + 0.5 * sin(u * 1.7 + t * 0.07 + ph))) * amount;
        glow += amount;
    }
    col += light * %.4f * (PULSE ? 0.75 + 0.5 * loud : 1.0);
    if (%d) {
        float2 cell = floor(float2(u, v) * colour_.xy / 2);
        float n = frac(sin(cell.x * 12.9898 + cell.y * 78.233) * 43758.5453);
        if (n > 0.9983) {
            float star = (0.55 + 0.45 * sin(t * 1.3 + n * 6283.0)) * (1 - smoothstep(0.35, 0.7, v)) * (1 - saturate(glow * 1.6)) * 0.8;
            col += star;
        }
    }
    return float4(saturate(col), 1);
}
)",
                  pulse ? 1 : 0, pulse && s.wave ? 1 : 0, s.speed, n.r, n.g, n.b, c1.r, c1.g, c1.b, c2.r, c2.g, c2.b, s.brightness, s.stars ? 1 : 0);
    return buf;
}

// ---- The other styles' skies (0.21). Each style's sky, pulse and wave are their own pictures; `styled` is their sums
// in C++ and `styledShader` the same sums in HLSL (kept in step by hand, and checked by the tests).
namespace {
struct V3 {
    float r = 0, g = 0, b = 0;
};
V3 v3(const Rgb& c) { return {c.r, c.g, c.b}; }
V3 operator+(V3 a, V3 b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
V3 operator-(V3 a, V3 b) { return {a.r - b.r, a.g - b.g, a.b - b.b}; }
V3 operator*(V3 a, float k) { return {a.r * k, a.g * k, a.b * k}; }
V3 mixv(V3 a, V3 b, float t) { return a + (b - a) * t; }
float h2(float x, float y) { return frac(std::sin(x * 12.9898f + y * 78.233f) * 43758.5453f); }
// Particles in cells of `cell` pixels, the cells moved by (dx, dy) pixels: one particle in a share `density` of the cells,
// `smin`..`smax` pixels across (a soft round spot). `r` is the particle's own random number.
float motes(float px, float py, float cell, float dx, float dy, float density, float smin, float smax, float t, float& r) {
    const float qx = (px + dx) / cell, qy = (py + dy) / cell;
    const float cx = std::floor(qx), cy = std::floor(qy), fx = qx - cx, fy = qy - cy;
    r = h2(cx, cy);
    const float r2 = h2(cx + 17.0f, cy + 17.0f);
    if (r < 1.0f - density) return 0;
    const float ox = 0.2f + 0.6f * r2 + 0.12f * std::sin(t * 0.7f + r * 6.2832f), oy = 0.2f + 0.6f * frac(r * 7.13f);
    const float d = std::hypot(fx - ox, fy - oy) * cell;
    const float size = smin + (smax - smin) * r2;
    return std::exp(-(d * d) / (size * size));
}

// An angle from a direction, by a polynomial (the same in HLSL: some shader compilers lack atan2).
float atan2p(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float a = std::min(ax, ay) / std::max(std::max(ax, ay), 1e-6f);
    const float sq = a * a;
    float r = ((-0.0464964749f * sq + 0.15931422f) * sq - 0.327622764f) * sq * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    if (y < 0) r = -r;
    return r;
}
// A gear seen from the front (steampunk): how much of the pixel it covers, and `rim`, the light along its edge.
float gearAt(float u, float v, float w, float h, float cx, float cy, float r, float n, float rot, float& rim) {
    const float dx = (u - cx) * w / h, dy = v - cy;
    const float d = std::sqrt(dx * dx + dy * dy), a = atan2p(dy, dx);
    const float R = r * (1 + 0.09f * smoothstep(-0.3f, 0.3f, std::sin(a * n + rot)));
    const float aa = 1.5f / h;
    float cover = sat((R - d) / aa + 0.5f) * sat((d - r * 0.22f) / aa + 0.5f);
    if (d > r * 0.35f && d < r * 0.78f && std::sin(a * 5.0f + rot * 5.0f / n) < -0.25f) cover = 0;
    rim = std::exp(-std::fabs(d - R * 0.96f) * h / 4.0f);
    return cover;
}

Rgb styled(const Settings& s, float u, float v, float seconds, int w, int h, const Pulse* pulse) {
    const bool P = pulse != nullptr, W = P && s.wave;
    const float band = P ? bandAt(*pulse, u) : 0, loud = P ? sat(pulse->level) : 0;
    auto bandAtU = [&](float x) { return P ? bandAt(*pulse, x) : 0.0f; };
    const float wb = W ? (bandAtU(u - 0.06f) + 2 * bandAtU(u - 0.03f) + 3 * band + 2 * bandAtU(u + 0.03f) + bandAtU(u + 0.06f)) / 9 : 0;
    const float t = seconds * s.speed;
    const V3 night = v3(lin(s.night)), c1 = v3(lin(s.colour1)), c2 = v3(lin(s.colour2)), c3 = v3(lin(s.colour3));
    const float bright = s.brightness * (P ? 0.75f + 0.5f * loud : 1.0f);
    const float px = u * w, py = v * h;
    const int ST = s.style == "bookish" ? 1 : s.style == "fantasy" ? 2 : s.style == "scifi" ? 3 : s.style == "cosy" ? 4 : s.style == "radio" ? 6 : s.style == "steampunk" ? 7 : 5;
    V3 col;
    float r = 0;
    if (ST == 1) {   // bookish: a leather-dark room, drifting ink, gold dust; book spines; calligraphy
        const float dx = u - 0.5f, dy = v - 0.5f;
        const float vg = 1 - sat((dx * dx + dy * dy) * 1.6f);
        col = night * (0.55f + 0.65f * vg);
        const float ink = sat(0.5f + 0.5f * std::sin(u * 3.1f + t * 0.05f + std::sin(v * 4.3f - t * 0.04f) * 1.5f)) *
                          sat(0.5f + 0.5f * std::sin(v * 2.7f + t * 0.03f + std::sin(u * 5.1f) * 1.2f));
        col = col + c2 * (ink * 0.35f) + c1 * (std::exp(-(1 - v) * 3.0f) * 0.12f);
        if (P && !W) {
            const float n = 20, i = std::floor(u * n), cu = u * n - i;
            const float b = bandAtU((i + 0.5f) / n), top = 1 - (0.12f + 0.45f * b);
            if (cu > 0.07f && cu < 0.93f && v > top) {
                V3 spine = mixv(c2, c1, 0.25f + 0.5f * b) * (0.55f + 0.45f * std::sin(cu * 3.1416f));
                if (v < top + 0.012f || std::fabs(v - (top + 0.05f)) < 0.004f || std::fabs(v - 0.95f) < 0.004f) spine = c1;
                col = mixv(col, spine * bright, 0.9f);
            }
        }
        for (int k = 0; W && k < 3; ++k) {
            const float d = waveLine(k, u, t, wb) - v;
            const float thick = (0.003f + 0.009f * wb) * (0.55f + 0.45f * std::sin(u * 7.0f + k * 1.9f + t * 0.3f));
            const float amount = (1.2f * std::exp(-(d * d) / (thick * thick)) + 0.25f * std::exp(-std::fabs(d) * 25.0f)) * (0.85f - 0.2f * k) * (0.5f + 1.1f * wb);
            col = col + c1 * (amount * bright);
        }
        if (s.stars) {
            const float e = motes(px, py, 60, t * w * 0.003f, -t * h * 0.008f, 0.35f, 1.2f, 2.6f, t, r);
            col = col + c1 * (e * (0.5f + 0.5f * std::sin(t * 1.7f + r * 50.0f)) * 1.1f);
        }
    } else if (ST == 2) {   // fantasy: embers and mist; flames; flame ribbons
        col = night * (0.35f + 0.65f * v);
        col = col + c1 * (std::exp(-(1 - v) * 4.0f) * (0.6f + 0.15f * std::sin(u * 3.0f + t * 0.2f)) * 0.5f * (P ? 0.7f + 0.8f * loud : 1.0f));
        const float mist = sat(0.5f + 0.5f * std::sin(u * 4.0f + t * 0.15f + std::sin(v * 9.0f + t * 0.1f))) * std::exp(-std::fabs(v - 0.8f) * 6.0f);
        col = col + V3{0.55f, 0.5f, 0.52f} * (mist * 0.2f);
        if (P && !W) {
            const float hgt = 0.06f + 0.5f * band;
            const float yy = (1 - v) + 0.035f * std::sin(v * 30.0f - t * 6.0f + u * 50.0f) + 0.025f * std::sin(v * 17.0f - t * 4.0f + u * 23.0f);
            float fl = sat(1 - yy / hgt);
            fl = fl * fl;
            col = col + mixv(c3, c1, sat(yy / hgt)) * (fl * (0.6f + 0.9f * band) * bright);
        }
        for (int k = 0; W && k < 3; ++k) {
            const float d = waveLine(k, u, t, wb) - v;
            const float thick = 0.006f + 0.014f * wb + 0.005f * loud;
            const float flick = d > 0 ? std::exp(-d * 9.0f) * (0.5f + 0.5f * std::sin(u * 80.0f + t * 3.0f + k * 2.0f)) : 0.0f;
            const float amount = (1.3f * std::exp(-(d * d) / (thick * thick)) + 0.45f * std::exp(-std::fabs(d) * 14.0f) + 0.5f * flick) * (0.8f - 0.15f * k) * (0.45f + 1.2f * wb);
            col = col + mixv(c3, c1, sat(0.5f + 0.5f * std::sin(u * 2.6f + t * 0.2f + k))) * (amount * bright);
        }
        if (s.stars) {
            const float e = motes(px, py, 70, 0, t * h * 0.05f, 0.25f + (P ? 0.35f * loud : 0.0f), 1.2f, 3.2f, t, r);
            col = col + mixv(c3, c1, frac(r * 3.7f)) * (e * (0.55f + 0.45f * std::sin(t * 4.0f + r * 40.0f)) * smoothstep(0.05f, 0.9f, v) * 1.4f * bright);
        }
    } else if (ST == 3) {   // scifi: a grid horizon and a scan beam; an equaliser; an oscilloscope
        const float hz = 0.62f;
        col = v < hz ? night * (0.5f + 0.5f * v) : night * 0.6f;
        if (v < hz) col = col + c2 * (std::exp(-(hz - v) * 6.0f) * 0.35f);
        const float sp = frac(t * 0.05f);
        col = col + c1 * (std::exp(-((v - sp) * (v - sp)) / 0.0003f) * 0.18f);
        if (v > hz) {
            const float p = v - hz + 0.0005f;
            const float X = (u - 0.5f) / p * 1.2f, Z = 0.25f / p - t * 0.8f;
            const float sx = std::min(1.5f / w / p * 1.2f + 0.0001f, 0.25f), sz = std::min(1.5f / h / (p * p) * 0.25f + 0.0001f, 0.25f);
            const float fx = frac(X + 0.5f) - 0.5f, fz = frac(Z + 0.5f) - 0.5f;
            const float gl = std::min(std::exp(-(fx * fx) / (sx * sx)) + std::exp(-(fz * fz) / (sz * sz)), 1.0f);
            col = col + c1 * (gl * sat(p * 12.0f) * 0.55f * (P ? 0.7f + 0.8f * loud : 1.0f));
        }
        col = col + c1 * (std::exp(-std::fabs(v - hz) * 90.0f) * 0.9f + std::exp(-std::fabs(v - hz) * 18.0f) * 0.25f);
        if (P && !W) {
            const float n = 28, i = std::floor(u * n), cu = u * n - i;
            const float b = bandAtU((i + 0.5f) / n), hgt = 0.03f + 0.5f * b;
            if (cu > 0.18f && cu < 0.82f) {
                if (v < hz && v > hz - hgt) col = col + mixv(c1, c3, sat((hz - v) / 0.55f)) * ((frac((hz - v) / 0.022f) < 0.72f ? 1.0f : 0.15f) * 0.9f * bright);
                if (v >= hz && v < hz + hgt * 0.35f) col = col + c1 * (0.18f * bright);
            }
        }
        for (int k = 0; W && k < 3; ++k) {
            const float centre = 0.40f + (k - 1) * 0.07f + (0.01f + 0.2f * wb) * std::sin(u * (14.0f + 3.0f * k) - t * (2.0f + 0.5f * k) + k * 2.1f);
            const float d = centre - v, sig = 1.5f / h + 0.002f * wb;
            const float amount = (std::exp(-(d * d) / (sig * sig)) + 0.35f * std::exp(-std::fabs(d) * 30.0f)) * (0.6f + 0.8f * wb);
            col = col + (k == 0 ? c1 : k == 1 ? c3 : c2) * (amount * bright);
        }
        if (s.stars && v < hz) {
            const float cx = std::floor(u * w / 2), cy = std::floor(v * h / 2);
            const float n = frac(std::sin(cx * 12.9898f + cy * 78.233f) * 43758.5453f);
            if (n > 0.9983f) {
                const float star = (0.55f + 0.45f * std::sin(t * 1.3f + n * 6283.0f)) * (1 - smoothstep(0.35f, 0.6f, v)) * 0.7f;
                col = col + V3{star, star, star};
            }
        }
    } else if (ST == 4) {   // cosy: lamplight, bokeh and fireflies; a warm glow; soft ribbons
        col = night * (0.6f + 0.6f * v);
        const float lx = u - 0.22f, ly = (v - 0.28f) * h / w;
        col = col + c1 * (std::exp(-(lx * lx + ly * ly) * 5.0f) * 0.35f);
        const float bk = motes(px, py, 200, t * w * 0.004f, -t * h * 0.006f, 0.45f, 12, 22, t * 0.4f, r);
        col = col + mixv(c1, c3, frac(r * 5.3f)) * (bk * 0.3f * (P ? 0.6f + 1.6f * band : 1.0f));
        if (P && !W) col = col + c1 * (std::exp(-(1 - v) * 5.0f) * (0.2f + 0.9f * loud) * 0.6f);
        for (int k = 0; W && k < 3; ++k) {
            const float d = waveLine(k, u, t * 0.6f, wb) - v;
            const float thick = 0.03f + 0.03f * wb;
            const float amount = std::exp(-(d * d) / (thick * thick)) * (0.35f + 0.9f * wb) * (0.7f - 0.15f * k);
            col = col + mixv(c1, c3, sat(0.5f + 0.5f * std::sin(u * 2.0f + t * 0.1f + k))) * (amount * bright);
        }
        if (s.stars) {
            float r2 = 0;
            const float dxf = std::sin(t * 0.3f) * 20.0f, dyf = -t * h * 0.01f;
            const float core = motes(px, py, 110, dxf, dyf, 0.3f, 1.6f, 2.6f, t, r2);
            const float halo = motes(px, py, 110, dxf, dyf, 0.3f, 6.0f, 9.0f, t, r2);
            col = col + (c1 * 0.4f + V3{0.6f, 0.6f, 0.6f}) * ((core + 0.35f * halo) * (0.5f + 0.5f * std::sin(t * 2.2f + r2 * 30.0f)) * 1.2f);
        }
    } else if (ST == 6) {   // radio: waves from a transmitter and static; valve glow behind a grille; an AM carrier
        const float dx = (u - 0.5f) * w / h, dy = v - 1.05f, dist = std::sqrt(dx * dx + dy * dy);
        col = night * (0.5f + 0.6f * v);
        col = col + c1 * (std::exp(-dist * 2.2f) * 0.25f);
        const float ring = frac(dist * 3.0f - t * 0.15f);
        col = col + c1 * (std::exp(-((ring - 0.5f) * (ring - 0.5f)) / 0.0015f) * std::exp(-dist * 0.9f) * 0.45f * (P ? 0.6f + 0.8f * loud : 1.0f));
        if (P && !W) {
            const float n = 24, i = std::floor(u * n), cu = u * n - i;
            const float b = bandAtU((i + 0.5f) / n), top = 1 - (0.05f + 0.5f * b);
            const float gx = frac(px / 10.0f) - 0.5f, gy = frac(py / 10.0f) - 0.5f;
            const float grill = std::sqrt(gx * gx + gy * gy) < 0.32f ? 1.0f : 0.25f;
            if (cu > 0.12f && cu < 0.88f && v > top) col = col + mixv(c1, c2, sat((v - top) / 0.5f)) * (0.9f * grill * bright * (0.6f + 0.4f * std::sin(cu * 3.1416f)));
            if (v > 0.1f && v < 0.14f) {
                col = col + c3 * 0.12f;
                if (frac(u * 40.0f) < 0.06f) col = col + c1 * 0.6f;
            }
            const float nx = 0.2f + 0.6f * loud;
            if (v > 0.07f && v < 0.17f && std::fabs(u - nx) * w < 2.0f) col = col + c2 * 1.2f;
        }
        if (W) {
            const float env = 0.02f + 0.25f * wb;
            const float y = 0.55f + env * std::sin(u * 160.0f - t * 8.0f);
            const float d = y - v, sig = 1.5f / h;
            col = col + c1 * ((std::exp(-(d * d) / (sig * sig)) + 0.3f * std::exp(-std::fabs(d) * 40.0f)) * (0.6f + 0.6f * wb) * bright);
            const float e1 = std::fabs(v - (0.55f - env)), e2 = std::fabs(v - (0.55f + env));
            col = col + c3 * (sat(1 - std::min(e1, e2) * h / 1.5f) * 0.5f);
        }
        if (s.stars) {   // a little static
            const float n = h2(std::floor(px / 2) + std::floor(t * 20.0f), std::floor(py / 2));
            if (n > 0.996f) col = col + c3 * 0.14f;
        }
    } else if (ST == 7) {   // steampunk: turning gears, steam and sparks; copper pipes; brass ribbons
        col = night * (0.5f + 0.6f * v);
        const float gears[3][5] = {{0.16f, 0.78f, 0.30f, 14, 0.6f}, {0.84f, 0.28f, 0.22f, 11, -0.6f}, {0.55f, 1.05f, 0.38f, 18, 0.6f}};
        for (auto& g : gears) {
            float rim = 0;
            const float cover = gearAt(u, v, (float)w, (float)h, g[0], g[1], g[2], g[3], t * g[4], rim);
            col = mixv(col, c2 * 0.5f + c1 * (rim * 0.5f), cover * 0.75f * (P ? 0.6f : 1.0f));
        }
        const float mist = sat(0.5f + 0.5f * std::sin(u * 3.3f + t * 0.2f + std::sin(v * 7.0f - t * 0.15f))) * std::exp(-std::fabs(v - 0.7f) * 5.0f);
        col = col + V3{0.6f, 0.58f, 0.55f} * (mist * 0.15f);
        if (P && !W) {
            const float n = 16, i = std::floor(u * n), cu = u * n - i;
            const float b = bandAtU((i + 0.5f) / n), top = 1 - (0.08f + 0.5f * b);
            if (cu > 0.25f && cu < 0.75f && v > top) {
                const float shade = 0.25f + 0.75f * std::cos((cu - 0.5f) * 6.2832f);
                col = mixv(col, (v < top + 0.015f ? c3 * shade : mixv(c2, c1, shade)) * bright, 0.95f);
            }
            const float puff = std::exp(-((v - (top - 0.04f)) * (v - (top - 0.04f))) / 0.0012f) * b * std::exp(-((cu - 0.5f) * (cu - 0.5f)) / 0.08f);
            col = col + V3{0.7f, 0.68f, 0.65f} * (puff * 0.5f);
        }
        for (int k = 0; W && k < 3; ++k) {
            const float d = waveLine(k, u, t, wb) - v;
            const float thick = 0.006f + 0.012f * wb;
            const float amount = (1.2f * std::exp(-(d * d) / (thick * thick)) + 0.4f * std::exp(-std::fabs(d) * 16.0f)) * (0.8f - 0.15f * k) * (0.45f + 1.2f * wb);
            col = col + mixv(c3, c1, k * 0.5f) * (amount * bright);
        }
        if (s.stars) {
            const float e = motes(px, py, 80, 0, t * h * 0.04f, 0.2f, 1.0f, 2.2f, t, r);
            col = col + c3 * (e * (0.55f + 0.45f * std::sin(t * 5.0f + r * 30.0f)) * smoothstep(0.1f, 0.9f, v) * 1.3f);
        }
    } else {   // minimal: soft bands; a row of bars; a single line
        col = night;
        const float s1 = frac(u * 0.55f + v * 0.85f - t * 0.015f), s2 = frac(u * 0.4f - v * 0.7f + t * 0.011f + 0.37f);
        const float b1 = smoothstep(0.3f, 0.5f, s1) * (1 - smoothstep(0.5f, 0.7f, s1));
        const float b2 = smoothstep(0.3f, 0.5f, s2) * (1 - smoothstep(0.5f, 0.7f, s2));
        col = col + (c1 - night) * (b1 * 0.09f) + (c2 - night) * (b2 * 0.06f);
        if (P && !W) {
            const float n = 40, i = std::floor(u * n), cu = u * n - i;
            const float b = bandAtU((i + 0.5f) / n), hh = 0.008f + 0.22f * b;
            const float in = sat((hh - std::fabs(v - 0.5f)) * h / 1.5f + 0.5f) * sat((0.18f - std::fabs(cu - 0.5f)) * w / n / 1.5f + 0.5f);
            col = mixv(col, c1, in * sat(bright));
        }
        if (W) {
            const float c = 0.5f + (0.01f + 0.22f * wb) * std::sin(u * 8.0f - t * 1.5f);
            const float c2l = 0.5f + (0.01f + 0.22f * wb) * std::sin(u * 8.0f - t * 1.5f - 0.6f);
            col = mixv(col, c2, sat(1 - std::fabs(c2l - v) * h / 2.0f) * 0.35f);
            col = mixv(col, c1, sat(1 - std::fabs(c - v) * h / 3.0f));
        }
    }
    return {sat(col.r), sat(col.g), sat(col.b)};
}

std::string num(float x) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", x);
    return b;
}
std::string f3(const Rgb& c) { return "float3(" + num(c.r) + ", " + num(c.g) + ", " + num(c.b) + ")"; }
void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (std::size_t i = s.find(from); i != std::string::npos; i = s.find(from, i + to.size())) s.replace(i, from.size(), to);
}

std::string styledShader(const Settings& s, bool pulse) {
    const int ST = s.style == "bookish" ? 1 : s.style == "fantasy" ? 2 : s.style == "scifi" ? 3 : s.style == "cosy" ? 4 : s.style == "radio" ? 6 : s.style == "steampunk" ? 7 : 5;
    std::string src = R"(// NorthRay Capture's $NAME sky (built in; the same sums as aurora_sky::sample).
cbuffer C : register(b0) { float4 pos_[4]; float4 uv_[4]; float4 colour_; float4 flags_; };
struct V_ { float4 p : SV_Position; float2 t : TEXCOORD0; };
Texture2D bandsTex : register(t0);
SamplerState smp : register(s0);
static const int PULSE = $PULSE;
static const int WAVE = $WAVE;
static const int STARS = $STARS;
float h2_(float x, float y) { return frac(sin(x * 12.9898 + y * 78.233) * 43758.5453); }
float bandAt_(float x) { return PULSE ? bandsTex.SampleLevel(smp, float2(x, 0.5), 0).r : 0; }
float waveLine_(int k, float u, float t, float b)
{
    float ph = k * 2.1;
    float amp = 0.035 + 0.26 * b;
    return 0.56 + (k - 1) * 0.05 + amp * sin(u * (9.0 + 2.5 * k) - t * (1.1 + 0.35 * k) + ph) + 0.015 * sin(u * 23.0 + t * 0.8 + ph);
}
float motes_(float px, float py, float cell, float dx, float dy, float density, float smin, float smax, float t, out float r)
{
    float qx = (px + dx) / cell, qy = (py + dy) / cell;
    float cx = floor(qx), cy = floor(qy), fx = qx - cx, fy = qy - cy;
    r = h2_(cx, cy);
    float r2 = h2_(cx + 17.0, cy + 17.0);
    if (r < 1.0 - density) return 0;
    float ox = 0.2 + 0.6 * r2 + 0.12 * sin(t * 0.7 + r * 6.2832), oy = 0.2 + 0.6 * frac(r * 7.13);
    float d = length(float2(fx - ox, fy - oy)) * cell;
    float size = smin + (smax - smin) * r2;
    return exp(-(d * d) / (size * size));
}
float atan2p_(float y, float x)
{
    float ax = abs(x), ay = abs(y);
    float a = min(ax, ay) / max(max(ax, ay), 1e-6);
    float sq = a * a;
    float r = ((-0.0464964749 * sq + 0.15931422) * sq - 0.327622764) * sq * a + a;
    if (ay > ax) r = 1.57079637 - r;
    if (x < 0) r = 3.14159274 - r;
    if (y < 0) r = -r;
    return r;
}
float gearAt_(float u, float v, float w, float h, float cx, float cy, float r, float n, float rot, out float rim)
{
    float dx = (u - cx) * w / h, dy = v - cy;
    float d = sqrt(dx * dx + dy * dy), a = atan2p_(dy, dx);
    float R = r * (1 + 0.09 * smoothstep(-0.3, 0.3, sin(a * n + rot)));
    float aa = 1.5 / h;
    float cover = saturate((R - d) / aa + 0.5) * saturate((d - r * 0.22) / aa + 0.5);
    if (d > r * 0.35 && d < r * 0.78 && sin(a * 5.0 + rot * 5.0 / n) < -0.25) cover = 0;
    rim = exp(-abs(d - R * 0.96) * h / 4.0);
    return cover;
}
float4 ps(V_ i) : SV_Target
{
    float u = i.t.x, v = i.t.y;
    float w = colour_.x, h = colour_.y;
    float2 bl = PULSE ? bandsTex.SampleLevel(smp, float2(u, 0.5), 0).rg : float2(0, 0);
    float band = bl.x, loud = saturate(bl.y);
    float wb = WAVE ? (bandAt_(u - 0.06) + 2 * bandAt_(u - 0.03) + 3 * band + 2 * bandAt_(u + 0.03) + bandAt_(u + 0.06)) / 9 : 0;
    float t = flags_.w * $SPEED;
    float3 night = $NIGHT, c1 = $C1, c2 = $C2, c3 = $C3;
    float bright = $BRIGHT * (PULSE ? 0.75 + 0.5 * loud : 1.0);
    float px = u * w, py = v * h;
    float3 col = 0;
    float r = 0;
$BODY
    return float4(saturate(col), 1);
}
)";
    std::string body;
    if (ST == 1) body = R"(
    float dx = u - 0.5, dy = v - 0.5;
    float vg = 1 - saturate((dx * dx + dy * dy) * 1.6);
    col = night * (0.55 + 0.65 * vg);
    float ink = saturate(0.5 + 0.5 * sin(u * 3.1 + t * 0.05 + sin(v * 4.3 - t * 0.04) * 1.5)) * saturate(0.5 + 0.5 * sin(v * 2.7 + t * 0.03 + sin(u * 5.1) * 1.2));
    col += c2 * (ink * 0.35) + c1 * (exp(-(1 - v) * 3.0) * 0.12);
    if (PULSE && !WAVE) {
        float n = 20, ix = floor(u * n), cu = u * n - ix;
        float b = bandAt_((ix + 0.5) / n), top = 1 - (0.12 + 0.45 * b);
        if (cu > 0.07 && cu < 0.93 && v > top) {
            float3 spine = lerp(c2, c1, 0.25 + 0.5 * b) * (0.55 + 0.45 * sin(cu * 3.1416));
            if (v < top + 0.012 || abs(v - (top + 0.05)) < 0.004 || abs(v - 0.95) < 0.004) spine = c1;
            col = lerp(col, spine * bright, 0.9);
        }
    }
    [unroll] for (int k = 0; k < (WAVE ? 3 : 0); ++k) {
        float d = waveLine_(k, u, t, wb) - v;
        float thick = (0.003 + 0.009 * wb) * (0.55 + 0.45 * sin(u * 7.0 + k * 1.9 + t * 0.3));
        float amount = (1.2 * exp(-(d * d) / (thick * thick)) + 0.25 * exp(-abs(d) * 25.0)) * (0.85 - 0.2 * k) * (0.5 + 1.1 * wb);
        col += c1 * (amount * bright);
    }
    if (STARS) {
        float e = motes_(px, py, 60, t * w * 0.003, -t * h * 0.008, 0.35, 1.2, 2.6, t, r);
        col += c1 * (e * (0.5 + 0.5 * sin(t * 1.7 + r * 50.0)) * 1.1);
    }
)";
    else if (ST == 2) body = R"(
    col = night * (0.35 + 0.65 * v);
    col += c1 * (exp(-(1 - v) * 4.0) * (0.6 + 0.15 * sin(u * 3.0 + t * 0.2)) * 0.5 * (PULSE ? 0.7 + 0.8 * loud : 1.0));
    float mist = saturate(0.5 + 0.5 * sin(u * 4.0 + t * 0.15 + sin(v * 9.0 + t * 0.1))) * exp(-abs(v - 0.8) * 6.0);
    col += float3(0.55, 0.5, 0.52) * (mist * 0.2);
    if (PULSE && !WAVE) {
        float hgt = 0.06 + 0.5 * band;
        float yy = (1 - v) + 0.035 * sin(v * 30.0 - t * 6.0 + u * 50.0) + 0.025 * sin(v * 17.0 - t * 4.0 + u * 23.0);
        float fl = saturate(1 - yy / hgt);
        fl = fl * fl;
        col += lerp(c3, c1, saturate(yy / hgt)) * (fl * (0.6 + 0.9 * band) * bright);
    }
    [unroll] for (int k = 0; k < (WAVE ? 3 : 0); ++k) {
        float d = waveLine_(k, u, t, wb) - v;
        float thick = 0.006 + 0.014 * wb + 0.005 * loud;
        float flick = d > 0 ? exp(-d * 9.0) * (0.5 + 0.5 * sin(u * 80.0 + t * 3.0 + k * 2.0)) : 0.0;
        float amount = (1.3 * exp(-(d * d) / (thick * thick)) + 0.45 * exp(-abs(d) * 14.0) + 0.5 * flick) * (0.8 - 0.15 * k) * (0.45 + 1.2 * wb);
        col += lerp(c3, c1, saturate(0.5 + 0.5 * sin(u * 2.6 + t * 0.2 + k))) * (amount * bright);
    }
    if (STARS) {
        float e = motes_(px, py, 70, 0, t * h * 0.05, 0.25 + (PULSE ? 0.35 * loud : 0.0), 1.2, 3.2, t, r);
        col += lerp(c3, c1, frac(r * 3.7)) * (e * (0.55 + 0.45 * sin(t * 4.0 + r * 40.0)) * smoothstep(0.05, 0.9, v) * 1.4 * bright);
    }
)";
    else if (ST == 3) body = R"(
    float hz = 0.62;
    col = v < hz ? night * (0.5 + 0.5 * v) : night * 0.6;
    if (v < hz) col += c2 * (exp(-(hz - v) * 6.0) * 0.35);
    float sp = frac(t * 0.05);
    col += c1 * (exp(-((v - sp) * (v - sp)) / 0.0003) * 0.18);
    if (v > hz) {
        float p = v - hz + 0.0005;
        float gx = (u - 0.5) / p * 1.2, gz = 0.25 / p - t * 0.8;
        float sx = min(1.5 / w / p * 1.2 + 0.0001, 0.25), sz = min(1.5 / h / (p * p) * 0.25 + 0.0001, 0.25);
        float fx = frac(gx + 0.5) - 0.5, fz = frac(gz + 0.5) - 0.5;
        float gl = min(exp(-(fx * fx) / (sx * sx)) + exp(-(fz * fz) / (sz * sz)), 1.0);
        col += c1 * (gl * saturate(p * 12.0) * 0.55 * (PULSE ? 0.7 + 0.8 * loud : 1.0));
    }
    col += c1 * (exp(-abs(v - hz) * 90.0) * 0.9 + exp(-abs(v - hz) * 18.0) * 0.25);
    if (PULSE && !WAVE) {
        float n = 28, ix = floor(u * n), cu = u * n - ix;
        float b = bandAt_((ix + 0.5) / n), hgt = 0.03 + 0.5 * b;
        if (cu > 0.18 && cu < 0.82) {
            if (v < hz && v > hz - hgt) col += lerp(c1, c3, saturate((hz - v) / 0.55)) * ((frac((hz - v) / 0.022) < 0.72 ? 1.0 : 0.15) * 0.9 * bright);
            if (v >= hz && v < hz + hgt * 0.35) col += c1 * (0.18 * bright);
        }
    }
    [unroll] for (int k = 0; k < (WAVE ? 3 : 0); ++k) {
        float centre = 0.40 + (k - 1) * 0.07 + (0.01 + 0.2 * wb) * sin(u * (14.0 + 3.0 * k) - t * (2.0 + 0.5 * k) + k * 2.1);
        float d = centre - v, sig = 1.5 / h + 0.002 * wb;
        float amount = (exp(-(d * d) / (sig * sig)) + 0.35 * exp(-abs(d) * 30.0)) * (0.6 + 0.8 * wb);
        col += (k == 0 ? c1 : (k == 1 ? c3 : c2)) * (amount * bright);
    }
    if (STARS && v < hz) {
        float cx = floor(u * w / 2), cy = floor(v * h / 2);
        float n = frac(sin(cx * 12.9898 + cy * 78.233) * 43758.5453);
        if (n > 0.9983) {
            float star = (0.55 + 0.45 * sin(t * 1.3 + n * 6283.0)) * (1 - smoothstep(0.35, 0.6, v)) * 0.7;
            col += star;
        }
    }
)";
    else if (ST == 4) body = R"(
    col = night * (0.6 + 0.6 * v);
    float lx = u - 0.22, ly = (v - 0.28) * h / w;
    col += c1 * (exp(-(lx * lx + ly * ly) * 5.0) * 0.35);
    float bk = motes_(px, py, 200, t * w * 0.004, -t * h * 0.006, 0.45, 12, 22, t * 0.4, r);
    col += lerp(c1, c3, frac(r * 5.3)) * (bk * 0.3 * (PULSE ? 0.6 + 1.6 * band : 1.0));
    if (PULSE && !WAVE) col += c1 * (exp(-(1 - v) * 5.0) * (0.2 + 0.9 * loud) * 0.6);
    [unroll] for (int k = 0; k < (WAVE ? 3 : 0); ++k) {
        float d = waveLine_(k, u, t * 0.6, wb) - v;
        float thick = 0.03 + 0.03 * wb;
        float amount = exp(-(d * d) / (thick * thick)) * (0.35 + 0.9 * wb) * (0.7 - 0.15 * k);
        col += lerp(c1, c3, saturate(0.5 + 0.5 * sin(u * 2.0 + t * 0.1 + k))) * (amount * bright);
    }
    if (STARS) {
        float r2 = 0;
        float dxf = sin(t * 0.3) * 20.0, dyf = -t * h * 0.01;
        float core = motes_(px, py, 110, dxf, dyf, 0.3, 1.6, 2.6, t, r2);
        float halo = motes_(px, py, 110, dxf, dyf, 0.3, 6.0, 9.0, t, r2);
        col += (c1 * 0.4 + float3(0.6, 0.6, 0.6)) * ((core + 0.35 * halo) * (0.5 + 0.5 * sin(t * 2.2 + r2 * 30.0)) * 1.2);
    }
)";
    else if (ST == 6) body = R"(
    float dx = (u - 0.5) * w / h, dy = v - 1.05, dist = sqrt(dx * dx + dy * dy);
    col = night * (0.5 + 0.6 * v);
    col += c1 * (exp(-dist * 2.2) * 0.25);
    float ring = frac(dist * 3.0 - t * 0.15);
    col += c1 * (exp(-((ring - 0.5) * (ring - 0.5)) / 0.0015) * exp(-dist * 0.9) * 0.45 * (PULSE ? 0.6 + 0.8 * loud : 1.0));
    if (PULSE && !WAVE) {
        float n = 24, ix = floor(u * n), cu = u * n - ix;
        float b = bandAt_((ix + 0.5) / n), top = 1 - (0.05 + 0.5 * b);
        float gx = frac(px / 10.0) - 0.5, gy = frac(py / 10.0) - 0.5;
        float grill = sqrt(gx * gx + gy * gy) < 0.32 ? 1.0 : 0.25;
        if (cu > 0.12 && cu < 0.88 && v > top) col += lerp(c1, c2, saturate((v - top) / 0.5)) * (0.9 * grill * bright * (0.6 + 0.4 * sin(cu * 3.1416)));
        if (v > 0.1 && v < 0.14) {
            col += c3 * 0.12;
            if (frac(u * 40.0) < 0.06) col += c1 * 0.6;
        }
        float nx = 0.2 + 0.6 * loud;
        if (v > 0.07 && v < 0.17 && abs(u - nx) * w < 2.0) col += c2 * 1.2;
    }
    if (WAVE) {
        float env = 0.02 + 0.25 * wb;
        float y = 0.55 + env * sin(u * 160.0 - t * 8.0);
        float d = y - v, sig = 1.5 / h;
        col += c1 * ((exp(-(d * d) / (sig * sig)) + 0.3 * exp(-abs(d) * 40.0)) * (0.6 + 0.6 * wb) * bright);
        float e1 = abs(v - (0.55 - env)), e2 = abs(v - (0.55 + env));
        col += c3 * (saturate(1 - min(e1, e2) * h / 1.5) * 0.5);
    }
    if (STARS) {
        float n = h2_(floor(px / 2) + floor(t * 20.0), floor(py / 2));
        if (n > 0.996) col += c3 * 0.14;
    }
)";
    else if (ST == 7) body = R"(
    col = night * (0.5 + 0.6 * v);
    float rim = 0;
    float cover = gearAt_(u, v, w, h, 0.16, 0.78, 0.30, 14, t * 0.6, rim);
    col = lerp(col, c2 * 0.5 + c1 * (rim * 0.5), cover * 0.75 * (PULSE ? 0.6 : 1.0));
    cover = gearAt_(u, v, w, h, 0.84, 0.28, 0.22, 11, t * -0.6, rim);
    col = lerp(col, c2 * 0.5 + c1 * (rim * 0.5), cover * 0.75 * (PULSE ? 0.6 : 1.0));
    cover = gearAt_(u, v, w, h, 0.55, 1.05, 0.38, 18, t * 0.6, rim);
    col = lerp(col, c2 * 0.5 + c1 * (rim * 0.5), cover * 0.75 * (PULSE ? 0.6 : 1.0));
    float mist = saturate(0.5 + 0.5 * sin(u * 3.3 + t * 0.2 + sin(v * 7.0 - t * 0.15))) * exp(-abs(v - 0.7) * 5.0);
    col += float3(0.6, 0.58, 0.55) * (mist * 0.15);
    if (PULSE && !WAVE) {
        float n = 16, ix = floor(u * n), cu = u * n - ix;
        float b = bandAt_((ix + 0.5) / n), top = 1 - (0.08 + 0.5 * b);
        if (cu > 0.25 && cu < 0.75 && v > top) {
            float shade = 0.25 + 0.75 * cos((cu - 0.5) * 6.2832);
            col = lerp(col, (v < top + 0.015 ? c3 * shade : lerp(c2, c1, shade)) * bright, 0.95);
        }
        float puff = exp(-((v - (top - 0.04)) * (v - (top - 0.04))) / 0.0012) * b * exp(-((cu - 0.5) * (cu - 0.5)) / 0.08);
        col += float3(0.7, 0.68, 0.65) * (puff * 0.5);
    }
    [unroll] for (int k = 0; k < (WAVE ? 3 : 0); ++k) {
        float d = waveLine_(k, u, t, wb) - v;
        float thick = 0.006 + 0.012 * wb;
        float amount = (1.2 * exp(-(d * d) / (thick * thick)) + 0.4 * exp(-abs(d) * 16.0)) * (0.8 - 0.15 * k) * (0.45 + 1.2 * wb);
        col += lerp(c3, c1, k * 0.5) * (amount * bright);
    }
    if (STARS) {
        float e = motes_(px, py, 80, 0, t * h * 0.04, 0.2, 1.0, 2.2, t, r);
        col += c3 * (e * (0.55 + 0.45 * sin(t * 5.0 + r * 30.0)) * smoothstep(0.1, 0.9, v) * 1.3);
    }
)";
    else body = R"(
    col = night;
    float s1 = frac(u * 0.55 + v * 0.85 - t * 0.015), s2 = frac(u * 0.4 - v * 0.7 + t * 0.011 + 0.37);
    float b1 = smoothstep(0.3, 0.5, s1) * (1 - smoothstep(0.5, 0.7, s1));
    float b2 = smoothstep(0.3, 0.5, s2) * (1 - smoothstep(0.5, 0.7, s2));
    col += (c1 - night) * (b1 * 0.09) + (c2 - night) * (b2 * 0.06);
    if (PULSE && !WAVE) {
        float n = 40, ix = floor(u * n), cu = u * n - ix;
        float b = bandAt_((ix + 0.5) / n), hh = 0.008 + 0.22 * b;
        float cover = saturate((hh - abs(v - 0.5)) * h / 1.5 + 0.5) * saturate((0.18 - abs(cu - 0.5)) * w / n / 1.5 + 0.5);
        col = lerp(col, c1, cover * saturate(bright));
    }
    if (WAVE) {
        float c = 0.5 + (0.01 + 0.22 * wb) * sin(u * 8.0 - t * 1.5);
        float c2l = 0.5 + (0.01 + 0.22 * wb) * sin(u * 8.0 - t * 1.5 - 0.6);
        col = lerp(col, c2, saturate(1 - abs(c2l - v) * h / 2.0) * 0.35);
        col = lerp(col, c1, saturate(1 - abs(c - v) * h / 3.0));
    }
)";
    replaceAll(src, "$BODY", body);
    replaceAll(src, "$NAME", s.style);
    replaceAll(src, "$PULSE", pulse ? "1" : "0");
    replaceAll(src, "$WAVE", pulse && s.wave ? "1" : "0");
    replaceAll(src, "$STARS", s.stars ? "1" : "0");
    replaceAll(src, "$SPEED", num(s.speed));
    replaceAll(src, "$BRIGHT", num(s.brightness));
    replaceAll(src, "$NIGHT", f3(lin(s.night)));
    replaceAll(src, "$C1", f3(lin(s.colour1)));
    replaceAll(src, "$C2", f3(lin(s.colour2)));
    replaceAll(src, "$C3", f3(lin(s.colour3)));
    return src;
}
}  // namespace
}  // namespace ncap::aurora_sky

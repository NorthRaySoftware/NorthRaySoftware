#include "ncap/aurora_sky.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ncap::aurora_sky {

using nlohmann::json;

std::string toJson(const Settings& s) {
    return json{{"colour1", sourceutil::colourText(s.colour1)},
                {"colour2", sourceutil::colourText(s.colour2)},
                {"night", sourceutil::colourText(s.night)},
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

// The sums (kept in step with the HLSL in shader()).
Rgb sample(const Settings& s, float u, float v, float seconds, int w, int h, const Pulse* pulse) {
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
            float line = 0.56 + (k - 1) * 0.05 + amp * sin(u * (9.0 + 2.5 * k) - t * (1.1 + 0.35 * k) + ph) + 0.015 * sin(u * 23.0 + t * 0.8 + ph);
            float d = line - v;
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

}  // namespace ncap::aurora_sky

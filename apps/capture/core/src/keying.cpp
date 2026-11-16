#include "ncap/keying.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace ncap::keying {

using nlohmann::json;

std::string toJson(const Key& k) {
    return json{{"on", k.on}, {"colour", sourceutil::colourText(k.colour)}, {"similarity", k.similarity}, {"smoothness", k.smoothness}, {"spill", k.spill}}
        .dump();
}

Key fromJson(const std::string& text) {
    Key k;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return k;
    if (j.contains("on") && j["on"].is_boolean()) k.on = j["on"].get<bool>();
    if (j.contains("colour") && j["colour"].is_string()) {
        Rgba c;
        if (sourceutil::parseColour(j["colour"].get<std::string>(), c)) {
            c.a = 255;
            k.colour = c;
        }
    }
    auto num = [&](const char* key, float& out) {
        if (j.contains(key) && j[key].is_number() && std::isfinite(j[key].get<double>())) out = std::clamp(j[key].get<float>(), 0.0f, 100.0f);
    };
    num("similarity", k.similarity);
    num("smoothness", k.smoothness);
    num("spill", k.spill);
    return k;
}

namespace {
void cbcr(float r, float g, float b, float& cb, float& cr) {
    // BT.709
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    cb = (b - y) / 1.8556f;
    cr = (r - y) / 1.5748f;
}
float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / std::max(e1 - e0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
}  // namespace

Params params(const Key& k) {
    Params p{};
    cbcr(k.colour.r / 255.0f, k.colour.g / 255.0f, k.colour.b / 255.0f, p.cb, p.cr);
    p.sim = k.similarity / 100.0f * 0.4f;
    p.smooth = k.smoothness / 100.0f * 0.3f + 0.001f;
    p.spill = k.spill / 100.0f;
    return p;
}

Out apply(float r, float g, float b, const Params& p) {
    float cb, cr;
    cbcr(r, g, b, cb, cr);
    const float d = std::hypot(cb - p.cb, cr - p.cr);
    Out o{r, g, b, smoothstep(p.sim, p.sim + p.smooth, d)};
    // Spill: near the key colour (within twice the clear range), pull towards grey.
    const float near = 1 - smoothstep(p.sim, p.sim * 2 + p.smooth + 0.05f, d);
    const float pull = near * p.spill;
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    o.r = r + (y - r) * pull, o.g = g + (y - g) * pull, o.b = b + (y - b) * pull;
    return o;
}

}  // namespace ncap::keying

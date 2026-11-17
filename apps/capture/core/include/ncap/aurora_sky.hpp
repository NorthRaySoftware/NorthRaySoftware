// NorthRay Capture: the aurora sky, a live animated background drawn on the graphics card (a source of its
// own; holding scenes, Starting Soon). A night sky in the brand kit's darkest colour, three curtains of
// aurora light that drift and ripple in its main and second colours (sharp along the bottom, fading
// upwards, with fine rays), and a few faint stars that twinkle. Portable and unit-tested: `sample` is the
// picture's sums in C++, and `shader` writes the same sums as HLSL for the GPU.
#pragma once

#include "ncap/colour.hpp"

#include <string>

namespace ncap::aurora_sky {

using sourceutil::Rgba;

struct Settings {
    Rgba colour1{63, 207, 142, 255};    // the main curtain colour (the brand's primary)
    Rgba colour2{122, 92, 255, 255};    // the second (the brand's secondary)
    Rgba night{7, 12, 24, 255};         // the sky (the brand's night)
    float speed = 1.0f;                 // 0.25 (slow) .. 3 (lively)
    float brightness = 1.0f;            // 0.3 .. 1.5
    bool stars = true;
    bool followBrand = true;            // the colours follow the brand kit
    // The aurora pulse's style: false the curtains rise with the music; true (the aurora wave) three ribbons of
    // light flow across the screen like a sound wave, swelling where their frequency band is loud.
    bool wave = false;
    int width = 1920, height = 1080;    // the picture's size
    bool operator==(const Settings&) const = default;
};
std::string toJson(const Settings& s);
Settings fromJson(const std::string& json);   // missing or bad values take the defaults, others clamped

struct Rgb {
    float r, g, b;   // 0..1
};
// The aurora pulse (the sound visualiser): the curtains rise and brighten with the music, each across
// the screen following its own frequency band (low on the left, high on the right), and the whole sky
// brightens with the loudness. `bands` are 0..1 (core/visualiser), read between neighbours the way the
// graphics card's linear sampling of an n×1 texture does.
struct Pulse {
    const float* bands = nullptr;
    int count = 0;
    float level = 0;
};
float bandAt(const Pulse& p, float u);

// The aurora wave (Settings::wave): ribbon k's middle line at u, 0..1 down the picture. Exposed for the tests.
float waveLine(int k, float u, float t, float band);
// The colour at (u, v) (0..1, v down) after `seconds`, for a `w`×`h` picture (stars are per pixel).
Rgb sample(const Settings& s, float u, float v, float seconds, int w, int h, const Pulse* pulse = nullptr);
// The pixel shader (HLSL, "ps"): `flags.w` is the time in seconds, `colour.xy` the size (Gpu::drawWith).
// With `pulse`, it reads the bands from t0 (an n×1 8-bit texture: red the band, green the loudness).
std::string shader(const Settings& s, bool pulse = false);

}  // namespace ncap::aurora_sky

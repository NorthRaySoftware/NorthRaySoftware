// NorthRay Capture: green screen (a colour key) on a scene item. Portable and unit-tested; the GPU's
// frame shader (video/gpu.cpp) does the same sums per pixel.
//
// Colours are compared by their colour alone, not their brightness (Cb and Cr of BT.709 YCbCr), so a
// shadow on the screen is still "the screen". Within `similarity` of the key colour a pixel is clear;
// it fades in over `smoothness` (soft hair and edges); and green light bouncing onto you (spill) is
// taken out by pulling pixels near the key colour towards grey.
#pragma once

#include "ncap/colour.hpp"

#include <string>

namespace ncap::keying {

using sourceutil::Rgba;

struct Key {
    bool on = false;
    Rgba colour{0, 177, 64, 255};   // a typical green screen
    float similarity = 40;          // % (0-100): how near the key colour is clear
    float smoothness = 8;           // % (0-100): how softly the edge fades in
    float spill = 50;               // % (0-100): how much of the key colour's tint to take out
    bool operator==(const Key&) const = default;
};

std::string toJson(const Key& k);           // a JSON object
Key fromJson(const std::string& json);      // missing or bad values take the defaults, others clamped

// The shader's constants: the key's Cb, Cr and the three thresholds in its units.
struct Params {
    float cb, cr, sim, smooth, spill;
};
Params params(const Key& k);

// One pixel (0..1 each): how opaque it stays, and its colour after the spill is taken out.
struct Out {
    float r, g, b, a;
};
Out apply(float r, float g, float b, const Params& p);

// Presets: the usual green and blue screens.
constexpr Rgba kGreen{0, 177, 64, 255}, kBlue{0, 71, 187, 255};

}  // namespace ncap::keying

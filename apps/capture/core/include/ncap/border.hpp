// NorthRay Capture: borders for scene items (the Camera Border maker). A border is part of an item:
// its corners rounded (up to a circle), a ring of colour (plain or a two-colour gradient) round the
// outside of its edge (the picture keeps its full size), and a glow or shadow around that. The video engine draws it on the graphics card with
// the same sums as here; render() makes the same picture on the processor for "Save as PNG".
#pragma once

#include "ncap/colour.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ncap::border {

using sourceutil::Rgba;

struct Style {
    bool on = false;
    float width = 6;             // the ring's thickness, canvas pixels (0-60)
    float roundness = 12;        // the corners: % of half the shorter side (100 = a circle or a pill)
    Rgba colour{255, 255, 255, 255};
    bool gradient = false;       // colour → colour2 across the item
    Rgba colour2{0, 200, 255, 255};
    float angle = 45;            // the gradient's direction, degrees clockwise from left-to-right
    float glow = 0;              // the glow or shadow's size, canvas pixels (0-120)
    Rgba glowColour{0, 0, 0, 255};
    float glowOpacity = 60;      // %
    float offsetX = 0, offsetY = 0;   // moves the glow: a drop shadow (-60..60)
    bool operator==(const Style&) const = default;
};

std::string toJson(const Style& s);           // a JSON object
Style fromJson(const std::string& json);      // missing or bad values take the defaults, others clamped

// The corner radius in pixels for an item of w×h.
float radius(const Style& s, float w, float h);
// Signed distance from (x, y) (pixels from the item's top-left) to the edge of a w×h rectangle with
// corners of radius r: negative inside.
float distance(float x, float y, float w, float h, float r);
// How much of the pixel centred at (x, y) is inside the shape (1-pixel anti-aliasing).
float coverage(float d);
// How far the ring and glow reach outside the item, in pixels (0 when it's off).
float margin(const Style& s);

// The border's picture for an item of w×h, with margin() all round (as if on), as straight-alpha
// RGBA rows: the glow, then the ring, and the item's own area left clear (where the camera shows).
std::vector<std::uint8_t> render(const Style& s, int w, int h, int& outW, int& outH);
// The colour of the ring at (x, y) within a w×h area: the plain colour, or the gradient's.
Rgba ringColour(const Style& s, float x, float y, float w, float h);

struct Preset {
    const char* name;
    Style style;
    bool square;   // also make the item square (a circle needs one)
};
const std::vector<Preset>& presets();

}  // namespace ncap::border

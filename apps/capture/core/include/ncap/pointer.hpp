// NorthRay Capture: highlighting the pointer and its clicks on a screen capture (0.12). Portable and tested;
// the video engine draws what these work out.
//
// A scene item showing a display, a window or part of the display can have a soft halo round the pointer and a
// ring that spreads out from each click (left in the main colour, right in the second). The pointer's place comes
// from Windows in desktop pixels; `toCanvas` turns it into the canvas through the item's area, crop, mirroring,
// size and rotation, and says when it's outside what the item shows.
#pragma once

#include "ncap/pointer_style.hpp"
#include "ncap/scenes.hpp"

#include <string>
#include <vector>

namespace ncap::pointer {

using sourceutil::Rgba;

std::string toJson(const Style& s);
Style fromJson(const std::string& json);   // missing or bad values take the defaults, others clamped

// A desktop point inside `area` (the desktop rectangle the source's picture shows, at the picture's size sw × sh)
// to the canvas. False if it's outside the area or cropped away. `scale` is how many canvas pixels one source
// pixel is (for sizing the halo).
bool toCanvas(const Transform& t, float sw, float sh, float areaLeft, float areaTop, float areaW, float areaH, float desktopX, float desktopY,
              float& canvasX, float& canvasY, float& scale);

// Click rings: each lasts kRingMs, growing from half the halo's size to twice it and fading out.
constexpr int kRingMs = 450;
struct Ring {
    long long startMs = 0;
    float x = 0, y = 0;   // where it was clicked, desktop pixels
    bool right = false;
};
struct RingLook {
    float diameter, alpha;   // in source pixels, 0..1
};
RingLook ringAt(const Style& s, long long ageMs);   // alpha 0 once it's over

// Keeps the rings for one item: press() on a button going down, prune() drops finished ones (at most 16 kept).
class Rings {
public:
    void press(long long nowMs, float x, float y, bool right);
    void prune(long long nowMs);
    const std::vector<Ring>& all() const { return rings_; }

private:
    std::vector<Ring> rings_;
};

// The halo and ring pictures (straight-alpha BGRA, n × n): a soft disc fading out at the edge, and a ring with
// soft edges. White, so the engine can draw them in any colour.
std::vector<unsigned char> discPixels(int n);
std::vector<unsigned char> ringPixels(int n);

}  // namespace ncap::pointer

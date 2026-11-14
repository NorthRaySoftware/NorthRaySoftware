// NorthRay Capture: transitions between scenes (system 5), the portable part: the kinds and their
// settings, easing, where the two scenes sit during a slide or swipe, and the luma wipe patterns.
// The compositor (src/video/engine.cpp) draws them; everything here is unit-tested.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncap::transition {

enum class Kind { Cut, Fade, Slide, Swipe, LumaWipe };
enum class Easing { Linear, EaseInOut, EaseOut };
enum class Direction { Left, Right, Up, Down };   // where the new scene moves towards
enum class Pattern { Horizontal, Vertical, Diagonal, Circle, Clock, Blinds };

struct Settings {
    Kind kind = Kind::Fade;
    int durationMs = 300;
    Easing easing = Easing::EaseInOut;
    Direction direction = Direction::Left;
    Pattern pattern = Pattern::Horizontal;
    float softness = 0.12f;   // luma wipe: the width of the soft edge (0 hard .. 0.5)
    bool invert = false;      // luma wipe: run the pattern the other way
};

const char* key(Kind k);
const char* key(Easing e);
const char* key(Direction d);
const char* key(Pattern p);
bool parse(const std::string& s, Kind& out);
bool parse(const std::string& s, Easing& out);
bool parse(const std::string& s, Direction& out);
bool parse(const std::string& s, Pattern& out);

std::string toJson(const Settings& s);
Settings fromJson(const std::string& json);   // anything missing or unknown keeps its default

// 0..1 → 0..1.
float ease(Easing e, float t);

// During a slide (both scenes move) or a swipe (the new one moves over the old one), the offset of
// each scene's canvas, in canvas pixels, at eased progress p.
struct Offsets {
    float fromX = 0, fromY = 0, toX = 0, toY = 0;
};
Offsets offsets(Kind k, Direction d, float p, float canvasW, float canvasH);

// A luma wipe's pattern: one byte per pixel; the new scene shows where pattern < progress
// (with a soft edge), so 0 is revealed first and 255 last.
std::vector<std::uint8_t> pattern(Pattern p, int w, int h);

// What one pixel of a luma wipe shows of the new scene (0..1), as the shader computes it.
float lumaAlpha(float luma, float progress, float softness, bool invert);

}  // namespace ncap::transition

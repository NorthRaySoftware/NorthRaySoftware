// NorthRay Capture: small portable pieces for the picture sources made in the app (images, text,
// colours, slideshows) and for typing an item's position exactly. Unit-tested.
#pragma once

#include "ncap/colour.hpp"
#include "ncap/scenes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ncap::sourceutil {


// "x, y, width, height" (commas or spaces; width and height may be left off, and "x" and "y" may
// be "-" for "as it is"). Sizes must be positive. Canvas pixels.
struct Placement {
    bool hasX = false, hasY = false, hasSize = false;
    float x = 0, y = 0, w = 0, h = 0;
};
bool parsePlacement(const std::string& text, Placement& out);
std::string placementText(const Transform& t, float sourceW, float sourceH);
// "45", "-90.5", "45°": degrees; false otherwise. Brought into -180..180.
bool parseDegrees(const std::string& text, float& out);

// The order to show a folder's images in: by name, numbers counted as numbers ("2" before "10"),
// any case; or shuffled with `seed` (the same seed, the same order).
std::vector<std::string> slideOrder(std::vector<std::string> names, bool shuffle, std::uint32_t seed);
bool isImageFile(const std::string& name);
bool isMediaFile(const std::string& name);   // video or sound: mp4, m4v, mov, mkv, avi, wmv, webm, mp3, m4a, aac, wav, wma, flac
// "1:05" or "1:02:03" for a number of seconds.
std::string clockText(double seconds);   // by its extension: png, jpg, jpeg, bmp, gif, tif(f), webp, heic

// A GIF frame's delay in hundredths of a second, as browsers show it: 0 or 1 means 10 (a tenth of a second).
int gifDelayMs(int hundredths);

}  // namespace ncap::sourceutil

// NorthRay Capture: a colour as the settings write it ("#RRGGBB" or "#RRGGBBAA"). Defined in sourceutil.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace ncap::sourceutil {

struct Rgba {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
    bool operator==(const Rgba&) const = default;
};
// "#RRGGBB", "#RRGGBBAA", "#RGB" (case and spaces don't matter); false if it isn't one.
bool parseColour(const std::string& text, Rgba& out);
std::string colourText(const Rgba& c);   // "#RRGGBB", or "#RRGGBBAA" when not opaque

}  // namespace ncap::sourceutil

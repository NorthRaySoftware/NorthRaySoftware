// The pointer highlight's settings on a scene item (see ncap/pointer.hpp).
#pragma once

#include "ncap/colour.hpp"

namespace ncap::pointer {

using sourceutil::Rgba;

struct Style {
    bool on = false;
    float size = 48;                         // the halo's diameter in source pixels (12-240)
    float strength = 35;                     // % opacity of the halo's middle (0 = clicks only)
    bool clicks = true;                      // rings from clicks
    Rgba colour{255, 214, 64, 255};          // the halo and left clicks
    Rgba second{122, 92, 255, 255};          // right clicks
    bool operator==(const Style&) const = default;
};

}  // namespace ncap::pointer

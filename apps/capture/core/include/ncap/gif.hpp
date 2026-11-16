// NorthRay Capture: animated GIFs (designs' entrances, to post or share). Portable and unit-tested
// (and checked by decoding with Pillow).
//
// One palette for the whole animation (median cut over the frames' colours, up to 255 colours plus
// one see-through entry when any pixel is mostly transparent), each frame mapped to its nearest colour
// with light ordered dithering (stable from frame to frame, so it doesn't crawl), LZW-compressed, and
// set to loop forever. A frame that hasn't changed just lengthens the one before.
#pragma once

#include <cstdint>
#include <vector>

namespace ncap::gif {

struct Frame {
    std::vector<std::uint8_t> bgra;   // w x h, straight alpha
    int delayMs = 40;
};

// The GIF's bytes ("" if the size is impossible). Delays are rounded to GIF's hundredths of a second
// (at least 2, as browsers slow anything shorter).
std::vector<std::uint8_t> encode(int w, int h, const std::vector<Frame>& frames, bool dither = true);

}  // namespace ncap::gif

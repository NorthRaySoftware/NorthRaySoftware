// NorthRay Capture: drawing an overlay design (core/design) into a picture, with Direct2D and
// DirectWrite on the processor (a WIC bitmap), so the video source, the designer's canvas and PNG
// export all get exactly the same picture.
#pragma once

#include "ncap/design.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ncap::video {

struct DesignPicture {
    int w = 0, h = 0;
    std::vector<std::uint8_t> bgra;   // straight (not premultiplied) alpha
};

using Placeholders = std::vector<std::pair<std::string, std::string>>;

// Draws `d` at `scale` (1 = its own size), `tMs` into its entrances (a large number for the finished
// look). Placeholders ("{twitch}") in text are filled from `values`. False with a reason if it can't.
// `showMissing`: details not set yet show as their {tag} (the designer); otherwise they're left out.
bool renderDesign(const design::Document& d, float tMs, float scale, const Placeholders& values, DesignPicture& out, std::wstring& error,
                  bool showMissing = false);

// Saves a picture as a PNG.
bool savePng(const std::wstring& path, const DesignPicture& p);

// The values every design's placeholders are filled from (set by the window from Settings; any thread).
void setDesignValues(const Placeholders& values);
Placeholders designValues(unsigned* version = nullptr);

}  // namespace ncap::video

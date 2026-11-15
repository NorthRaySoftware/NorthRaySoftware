// NorthRay Capture: drawing an overlay design (core/design) into a picture, with Direct2D and
// DirectWrite on the processor (a WIC bitmap), so the video source, the designer's canvas and PNG
// export all get exactly the same picture.
#pragma once

#include "ncap/chat.hpp"
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
// `clock` is what timers show (design::timerText).
bool renderDesign(const design::Document& d, float tMs, float scale, const Placeholders& values, DesignPicture& out, std::wstring& error,
                  bool showMissing = false, const design::Clock& clock = {});
// The clock now for timers (local time and date; time live from setLiveTime), with `elapsedMs` given.
design::Clock clockNow(std::int64_t elapsedMs);
// How long the stream has been live (the longest destination), or -1 when it isn't; set by the window.
void setLiveTime(std::int64_t ms);

// Saves a picture as a PNG.
bool savePng(const std::wstring& path, const DesignPicture& p);

// The values every design's placeholders are filled from (set by the window from Settings; any thread).
void setDesignValues(const Placeholders& values);
Placeholders designValues(unsigned* version = nullptr);   // the version also changes with the brand kit

// The brand kit every design is drawn with (set by the window; any thread).
void setDesignBrand(const design::Brand& b);
// The live chat messages chat elements show (set by the chat connection; any thread).
void setDesignChat(std::vector<chat::Message> messages);
std::vector<chat::Message> designChat();
design::Brand designBrand();
// The font families designs can use (Windows' and the kit's font files), sorted; `error` says if some
// font files couldn't be used.
std::vector<std::wstring> brandFontFamilies(std::wstring& error);
// The family names in one font file (empty if it isn't a font Windows can read).
std::vector<std::wstring> fontFileFamilies(const std::wstring& file);

}  // namespace ncap::video

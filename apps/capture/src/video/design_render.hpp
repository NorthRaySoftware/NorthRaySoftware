// NorthRay Capture: drawing an overlay design (core/design) into a picture, with Direct2D and
// DirectWrite on the processor (a WIC bitmap), so the video source, the designer's canvas and PNG
// export all get exactly the same picture.
#pragma once

#include "ncap/chat.hpp"
#include "ncap/design.hpp"
#include "ncap/sourceutil.hpp"

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
// The stream's plan, for the go-live, time-left and next-stream timers (set by the window; any thread):
// `goLiveAt` is live::stampMs of the time set to go live (-1: none), `plannedMs` the stream's length (-1: none),
// `nextText` the next stream in words ("" none).
void setStreamPlan(std::int64_t goLiveAt, std::int64_t plannedMs, const std::string& nextText);

// From Writing Studio (0.13), set by the window (any thread): a writing sprint's end (milliseconds since 1970, -1 none),
// a celebration starting now with its words ({celebration}), and the book's world colour that brand-linked designs take
// in place of the kit's main colour while that book is written ("" none).
void setWritingSprint(std::int64_t endsAtEpochMs);
void celebrate(const std::string& text);
void setWorldColour(const std::string& hex);

// Writing card parts switched off live (0.20), from the main window's Writing panel and hotkeys (any thread).
void setWritingHidden(const std::vector<std::string>& parts);
std::vector<std::string> writingHidden();

// Saves a picture as a PNG.
bool savePng(const std::wstring& path, const DesignPicture& p);

// The values every design's placeholders are filled from (set by the window from Settings; any thread).
void setDesignValues(const Placeholders& values);
Placeholders designValues(unsigned* version = nullptr);   // the version also changes with the brand kit

// The brand kit every design is drawn with (set by the window; any thread).
void setDesignBrand(const design::Brand& b);
// The live chat messages chat elements show (set by the chat connection; any thread).
void setDesignChat(std::vector<chat::Message> messages);
// What's playing (a music file in a media source): designs fill in {song} and {artist}. The latest
// track started wins; a source clears it when it goes away. `owner` tells sources apart.
void setNowPlaying(const void* owner, const sourceutil::TrackInfo& track);
void clearNowPlaying(const void* owner);
sourceutil::TrackInfo nowPlaying();
std::vector<chat::Message> designChat();
design::Brand designBrand();
// The font families designs can use (Windows' and the kit's font files), sorted; `error` says if some
// font files couldn't be used.
std::vector<std::wstring> brandFontFamilies(std::wstring& error);
// The family names in one font file (empty if it isn't a font Windows can read).
std::vector<std::wstring> fontFileFamilies(const std::wstring& file);

}  // namespace ncap::video

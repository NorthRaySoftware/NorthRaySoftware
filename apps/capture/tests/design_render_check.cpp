// Not a test by itself: draws every design template with sample details to PNGs, for looking at (0.17).
//   capture_design_render_check.exe <folder> [picture]
// The picture (any image file) stands in for the cover, portraits and the place.
#include "video/design_render.hpp"

#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace ncap;

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: capture_design_render_check <folder> [picture]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring folder = argv[1];
    std::string pic;
    if (argc > 2) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, nullptr, 0, nullptr, nullptr);
        pic.resize((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, pic.data(), n, nullptr, nullptr);
        pic.resize(pic.size() - 1);
    }
    video::Placeholders v = {{"name", "Night Owl"},
                             {"twitch", "twitch.tv/nightowl"},
                             {"book", "The Envoy of Telvana"},
                             {"series", "Book One of The Alvora Chronicles"},
                             {"chapter", "Chapter 12: Ashes and Embers"},
                             {"chapterline", "3,214 words"},
                             {"today", "1,240 of 2,000 words today"},
                             {"bookline", "48,312 words in the book"},
                             {"streakline", "4-day streak"},
                             {"status", "Revised"},
                             {"targetline", "3,214 of 4,000 words"},
                             {"viewpoint", "Ada Wren"},
                             {"place", "Highcliff"},
                             {"storyday", "Day 4, at dawn"},
                             {"sprintline", "312 words this sprint"},
                             {"streamline", "640 words this stream"},
                             {"celebration", "Daily goal reached: 2,000 words!"},
                             {"song", "Night Sky"},
                             {"artist", "Aurora Band"},
                             {"cover", pic},
                             {"viewpointpic", pic},
                             {"placepic", pic}};
    const char* cast[] = {"Ada Wren", "Tom", "Sarrah", "The Grey Wanderer", "Mira", "Old Kess", "Bluebolt", "Maren", "Zornov", "Lillia"};
    for (int i = 0; i < 10; ++i) {   // more than fit: the Writing card's cast row scrolls (0.20)
        v.push_back({"cast" + std::to_string(i + 1), cast[i]});
        v.push_back({"cast" + std::to_string(i + 1) + "pic", pic});
    }
    // A song playing and some chat.
    static int owner = 0;
    sourceutil::TrackInfo track;
    track.title = "Night Sky";
    track.artist = "Aurora Band";
    video::setNowPlaying(&owner, track);
    video::setDesignChat(chat::samples((std::int64_t)GetTickCount64()));
    // A sprint running and a celebration just fired, so those designs show.
    design::Clock clock = video::clockNow(0);
    clock.sprintLeftMs = 7 * 60 * 1000 + 31000;
    clock.celebrateMs = 2000;
    clock.elapsedMs = 3000;   // three seconds into the cast row's scrolling
    int bad = 0;
    for (auto& t : design::templates()) {
        video::DesignPicture out;
        std::wstring err;
        if (!video::renderDesign(t.doc, 10000, 1, v, out, err, false, clock)) {
            std::fwprintf(stderr, L"%hs: %ls\n", t.id, err.c_str());
            ++bad;
            continue;
        }
        const std::wstring path = folder + L"\\" + std::wstring(t.id, t.id + std::strlen(t.id)) + L".png";
        if (!video::savePng(path, out)) ++bad;
    }
    std::printf("%d templates, %d failed\n", (int)design::templates().size(), bad);
    return bad ? 1 : 0;
}

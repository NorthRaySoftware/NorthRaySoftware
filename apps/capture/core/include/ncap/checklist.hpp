// NorthRay Capture: the pre-live checklist (optional; Settings › Streaming). When it's on, Start streaming
// first shows what Capture can check for itself, from a snapshot of the window's state, next to your own
// list to tick ("Phone on silent"). Nothing here stops you going live; problems are named so you can
// decide. Portable and unit-tested.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncap::checklist {

// Your own list: a few starting suggestions, and tidying what's typed (trimmed, no blanks or repeats,
// at most kMaxItems of at most kMaxChars bytes each).
constexpr std::size_t kMaxItems = 12, kMaxChars = 80;
std::vector<std::string> defaultItems();
std::vector<std::string> tidy(const std::vector<std::string>& items);

struct Destination {
    std::string name;   // "Twitch"
    bool hasKey = true;
    int bus = 0;        // the sound it carries: 0 the Stream mix, 1-6 Track 1-6
    bool hasServer = true;   // its server address is set (Kick and custom ones need one typed in)
};

struct Mic {
    std::string name;
    bool muted = false;
    std::uint32_t buses = 1;   // bit n: in bus n (0 the Stream mix, 1-6 the tracks)
    double silentFor = -1;     // seconds since it last had sound; < 0: no sound since Capture started
    bool working = true;       // the device is there and capturing
};

struct Snapshot {
    std::vector<Destination> destinations;   // the ones switched on
    std::string scene;
    bool sceneEmpty = false;
    std::vector<Mic> mics;
    bool encodersKnown = true, hardwareEncoder = true;
    bool recording = false;
};

enum class State { Ok, Waiting, Warn, Problem };
struct Check {
    State state;
    std::string text;
};

std::vector<Check> run(const Snapshot& s);
bool anyProblem(const std::vector<Check>& checks);   // a Problem or a Warn

// How long without sound before "say something" becomes a warning about the mic.
constexpr double kQuietSeconds = 20;

}  // namespace ncap::checklist

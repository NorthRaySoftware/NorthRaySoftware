// NorthRay Capture: where each audio source goes, and the checks that stop sound leaking into a
// broadcast (system 6, the rules; the mixer applies them). Portable and unit-tested.
//
// Every audio source can go to any of: the stream, recording tracks 1-6, and the monitor (your
// headphones). Muting or turning a source down in Capture never changes the application's own
// volume in Windows.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncap::routing {

enum Bus : std::uint32_t {
    Stream = 1u << 0,
    Track1 = 1u << 1, Track2 = 1u << 2, Track3 = 1u << 3, Track4 = 1u << 4, Track5 = 1u << 5, Track6 = 1u << 6,
    Monitor = 1u << 7,
};
constexpr std::uint32_t kAllTracks = Track1 | Track2 | Track3 | Track4 | Track5 | Track6;
inline std::uint32_t track(int n) { return n >= 1 && n <= 6 ? (1u << n) : 0; }

enum class Kind {
    Input,         // a microphone or line in or capture card audio: a capture device
    Desktop,       // everything an output device plays (loopback), minus `excluded` apps
    Application,   // one application's sound (its process tree)
};

struct AudioSource {
    std::string id, name;
    Kind kind = Kind::Input;
    std::string device;                  // Input: the capture device; Desktop: the output device ("" = default)
    std::string app;                     // Application: the executable, lower case ("minecraft.exe")
    std::vector<std::string> excluded;   // Desktop: apps left out ("spotify.exe")
    std::uint32_t buses = Stream | Track1;
    bool muted = false;
    float volume = 1.0f;                 // linear gain, 0..4 (+12 dB)
    // Whether it reaches a bus at all (not muted, not silent, and routed there).
    bool reaches(std::uint32_t bus) const { return !muted && volume > 0.0f && (buses & bus) != 0; }
};

// What's running and playing sound now (from Windows' audio sessions), to know which apps a desktop
// capture would also carry.
struct PlayingApp {
    std::string app;      // executable, lower case
    std::string device;   // the output device it plays to ("" = default)
};

struct Warning {
    enum Type {
        LeakThroughDesktop,   // an app kept off the stream still reaches it through desktop audio
        CapturedTwice,        // the same app (or device) reaches one bus from two sources
        MonitorFeedback,      // monitoring desktop audio plays it back into what's being captured
        EchoOnMicrophone,     // monitored sound may be picked up by a microphone (speakers, not headphones)
    } type;
    std::vector<std::string> sources;   // the sources involved (ids)
    std::string app;                    // the application, when it's about one
    std::string message;                // in plain words, with what to do
};

// The checks, for the given sources and the apps playing now. `monitorDevice` is the output the
// monitor bus plays to ("" = default); `monitorIsHeadphones` says whether it's headphones.
std::vector<Warning> analyse(const std::vector<AudioSource>& sources, const std::vector<PlayingApp>& playing,
                             const std::string& monitorDevice, bool monitorIsHeadphones);

// The path a source's sound takes, for the routing diagnostics view: "Spotify → (muted for the
// stream) · Monitor → Headphones".
std::string describeRoute(const AudioSource& s);

// "minecraft.exe" from "C:\Games\Minecraft\Minecraft.exe" or "Minecraft.EXE".
std::string exeKey(const std::string& pathOrName);

}  // namespace ncap::routing

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
        OutputFeedback,       // the second output plays desktop audio back onto the device it's captured from
        OutputsShareDevice,   // the monitor and the second output play to the same device, so both are heard mixed
    } type;
    std::vector<std::string> sources;   // the sources involved (ids)
    std::string app;                    // the application, when it's about one
    std::string message;                // in plain words, with what to do
};

// The checks, for the given sources and the apps playing now. `monitorDevice` is the output the
// monitor bus plays to ("" = default); `monitorIsHeadphones` says whether it's headphones.
// `secondDevice` and `secondBus` are the second output (a line out to a streaming PC, say) when it's
// on: the device and the bus it plays (one bit of Bus); "" and 0 when it's off.
std::vector<Warning> analyse(const std::vector<AudioSource>& sources, const std::vector<PlayingApp>& playing,
                             const std::string& monitorDevice, bool monitorIsHeadphones, const std::string& secondDevice = "",
                             std::uint32_t secondBus = 0);

// Ready-made places for a source's sound, set from its menu in the mixer.
enum class Preset {
    StreamAndRecord,   // the stream, track 1 and its kind's own track (as a new source starts)
    RecordOnly,        // track 1 and its own track, never the stream
    StreamOnly,        // the stream alone, not in recordings
    MonitorOnly,       // only you hear it
};
// The buses for a preset, keeping whether it's monitored (except MonitorOnly, which is just that).
std::uint32_t applyPreset(Preset p, Kind k, std::uint32_t current);
// The preset the buses match, if any.
bool matchPreset(std::uint32_t buses, Kind k, Preset& out);

// The path a source's sound takes, for the routing diagnostics view: "Spotify → (muted for the
// stream) · Monitor → Headphones".
std::string describeRoute(const AudioSource& s);

// Where a new source goes until it's changed: the stream, track 1 (everything), and a track of its
// own kind so it can be edited separately afterwards (microphones 2, desktop audio 3, applications 4).
std::uint32_t defaultBuses(Kind k);
// The bus's name in the interface: "Stream", "Track 3", "Monitor".
std::string busLabel(std::uint32_t bus);
// The apps a desktop audio source carries right now (playing on its device, not left out), sorted.
std::vector<std::string> carriedApps(const AudioSource& desktop, const std::vector<PlayingApp>& playing);

// "minecraft.exe" from "C:\Games\Minecraft\Minecraft.exe" or "Minecraft.EXE".
std::string exeKey(const std::string& pathOrName);

}  // namespace ncap::routing

// NorthRay Capture: the audio mixer and router (system 6). Every 10 ms it takes a block from each
// sound source, applies its volume and mute, measures it, and adds it to each bus it's routed to:
//
//   Stream       what viewers hear
//   Track 1-6    the recording's separate tracks (track 1: everything, by default)
//   Monitor      what you hear in your headphones, played on the monitor device
//
// A source that isn't routed to a bus contributes nothing to it at all (not "turned down": absent),
// which is what keeps a private app out of the broadcast.
//
// The mixer keeps its own steady clock; a source with nothing new (a loopback while nothing plays)
// adds silence, and a source that has run ahead is trimmed so its delay stays small.
#pragma once

#include "audio/capture.hpp"

#include "ncap/routing.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ncap::audio {

constexpr int kBuses = 8;   // 0 Stream, 1-6 Track 1-6, 7 Monitor
inline std::uint32_t busBit(int index) { return 1u << index; }   // matches routing::Bus

struct MixSource {
    Id id;
    std::shared_ptr<Capture> capture;
    float gain = 1;
    bool muted = false;
    std::uint32_t buses = routing::Stream | routing::Track1;
    bool syncTone = false;   // no capture: a 1 kHz beep for 50 ms at each whole second of the clock,
                             // in step with the sync test pattern's flash (checking sync in a recording)
};

struct Levels {
    float left = 0, right = 0;   // the loudest samples since last asked (after volume and mute)
};

// Plays the monitor bus on an output device (event-driven WASAPI render, shared mode).
class MonitorOutput {
public:
    ~MonitorOutput() { stop(); }
    void start(const std::wstring& deviceId);   // "" or "default": Windows' default output
    void stop();
    void push(const float* frames, std::size_t count);   // the mixer thread
    std::wstring status() const;

private:
    void run(std::wstring device);
    Ring ring_{kRate / 4};
    std::thread thread_;
    std::atomic<bool> quit_{false};
    mutable std::mutex m_;
    std::wstring status_;
};

// A bus's sound handed to a recording or a stream: 10 ms blocks in a ring, with the time (QPC
// ticks) the first block was mixed, so the reader can time every sample after it by counting.
struct Tap {
    explicit Tap(int bus) : bus(bus) {}
    const int bus;
    Ring ring{kRate * 2};   // 2 s
    std::atomic<std::int64_t> firstQpc{0};
    std::atomic<std::uint64_t> overflowed{0};   // frames dropped because the reader fell behind
};

class Mixer {
public:
    ~Mixer() { stop(); }
    void start();
    void stop();
    void setSources(std::vector<MixSource> sources);
    // The monitor device ("" or "default" for Windows' default; "off" for none).
    void setMonitorDevice(const std::wstring& device);
    std::wstring monitorStatus() const { return monitor_.status(); }
    // Per source; "#bus0".."#bus7" for the buses.
    std::map<Id, Levels> takeLevels();
    static Id busKey(int index) { return "#bus" + std::to_string(index); }
    // Saves `seconds` of a source's own sound (before volume and mute), or of a bus (`busKey`), as a
    // WAV file: the proof that a source or a mix holds only what it should. False if one is already
    // being saved.
    bool startClip(const Id& sourceOrBus, const std::wstring& path, int seconds);
    std::wstring takeFinishedClip(bool* ok = nullptr);   // once, when a clip is done
    // Recordings and streams read buses through taps, from the next block on.
    void addTap(std::shared_ptr<Tap> tap);
    void removeTap(const std::shared_ptr<Tap>& tap);

private:
    void run();
    void block();
    void clipWrite(const Id& key, const float* frames);

    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::mutex m_;
    std::vector<MixSource> sources_;
    std::map<Id, Levels> levels_;
    MonitorOutput monitor_;
    std::wstring monitorDevice_ = L"off";
    // A test clip in progress (mixer thread), and the one that's finished (for the UI).
    Id clipKey_;
    std::FILE* clip_ = nullptr;
    std::wstring clipPath_, finishedClip_;
    bool finishedOk_ = false;
    std::uint32_t clipFrames_ = 0, clipWanted_ = 0;
    std::vector<float> buf_;
    std::array<std::vector<float>, kBuses> bus_;
    std::vector<std::shared_ptr<Tap>> taps_;
    std::int64_t blockQpc_ = 0;   // when the block being mixed was due
};

}  // namespace ncap::audio

// NorthRay Capture: the audio mixer (system 6, first part). Every 10 ms it takes a block from each
// sound source, applies its volume and mute, measures it, and adds it to the mix. In Milestone 4 the
// one mix becomes the stream mix, the recording tracks and the monitor (headphones) mix.
//
// The mixer keeps its own steady clock; a source with nothing new (a loopback while nothing
// plays) adds silence, and a source that has run ahead is trimmed so its delay stays small.
#pragma once

#include "audio/capture.hpp"

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ncap::audio {

struct MixSource {
    Id id;
    std::shared_ptr<Capture> capture;
    float gain = 1;
    bool muted = false;
};

struct Levels {
    float left = 0, right = 0;   // the loudest samples since last asked (after volume and mute)
};

class Mixer {
public:
    ~Mixer() { stop(); }
    void start();
    void stop();
    void setSources(std::vector<MixSource> sources);
    std::map<Id, Levels> takeLevels();   // per source, and "" for the mix
    // Saves `seconds` of one source's own sound (before volume and mute) as a WAV file: the proof
    // that a source holds only what it should. False if one is already being saved.
    bool startClip(const Id& source, const std::wstring& path, int seconds);
    // The file of a clip that has finished (once), or "".
    std::wstring takeFinishedClip(bool* ok = nullptr);

private:
    void run();
    void block();

    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::mutex m_;
    std::vector<MixSource> sources_;
    std::map<Id, Levels> levels_;
    // A test clip in progress (mixer thread), and the one that's finished (for the UI).
    Id clipSource_;
    std::FILE* clip_ = nullptr;
    std::wstring clipPath_, finishedClip_;
    bool finishedOk_ = false;
    std::uint32_t clipFrames_ = 0, clipWanted_ = 0;
    std::vector<float> buf_, mix_;
};

}  // namespace ncap::audio

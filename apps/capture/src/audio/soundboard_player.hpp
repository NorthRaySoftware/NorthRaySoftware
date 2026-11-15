// NorthRay Capture: a soundboard source's sound. Its buttons' sounds (the starter pack, made in
// core/soundboard, or your own files decoded once with Media Foundation into 48 kHz stereo, up to 30
// seconds each) play through `Voices` into a ring the mixer reads, so the soundboard is an ordinary
// channel in the mixer (volume, mute, auto-duck, filters, where it's sent).
#pragma once

#include "ncap/audio.hpp"
#include "ncap/soundboard.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ncap::audio {

struct SoundboardSound {
    std::string name;
    std::string starter;   // a starter-pack id, or
    std::string file;      // a sound file
    bool operator==(const SoundboardSound&) const = default;
};

class SoundboardPlayer {
public:
    SoundboardPlayer();
    ~SoundboardPlayer();
    std::shared_ptr<Ring> ring() const { return ring_; }
    void setSounds(const std::vector<SoundboardSound>& sounds);   // loads files in the background
    void play(std::size_t i);
    void stopAll() { voices_.stopAll(); }
    std::size_t playing() const { return voices_.playing(); }
    // Why a sound can't play ("" if it can, or is still loading).
    std::wstring problem(std::size_t i) const;

private:
    void run();
    std::shared_ptr<Ring> ring_;
    soundboard::Voices voices_;
    mutable std::mutex m_;
    std::vector<SoundboardSound> sounds_;
    std::vector<soundboard::Voices::Pcm> pcm_;
    std::vector<std::wstring> problems_;
    std::thread thread_, loader_;
    std::atomic<bool> quit_{false};
    std::atomic<unsigned> generation_{0};
};

}  // namespace ncap::audio

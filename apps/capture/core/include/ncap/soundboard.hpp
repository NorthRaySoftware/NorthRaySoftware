// NorthRay Capture: the soundboard's portable parts (unit-tested).
//
// The starter pack is made here, in code: short sounds synthesised from tones and noise (a chime, a
// fanfare, a drum roll...), so they're NorthRay's own, free to use on stream with nothing to credit.
// Your own files are decoded elsewhere (Media Foundation) into the same form: 48 kHz stereo floats.
// `Voices` plays any number of sounds at once, mixed into the soundboard's channel.
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ncap::soundboard {

constexpr int kRate = 48000;   // stereo, interleaved floats

struct Starter {
    const char* id;
    const char* name;
};
const std::vector<Starter>& starterPack();
// The sound for a starter id (empty if unknown). Peaks below 0.9, starts and ends at silence.
std::vector<float> synth(const std::string& id);

// Sounds playing at once. `play` and `stop` from any thread; `mix` from the audio thread.
class Voices {
public:
    using Pcm = std::shared_ptr<const std::vector<float>>;
    // `restart`: if this sound is already playing, start it again rather than adding another copy.
    void play(const Pcm& pcm, float gain = 1.0f, bool restart = true);
    void stopAll();
    // Adds every playing sound into `out` (frames x 2) and drops finished ones.
    void mix(float* out, std::size_t frames);
    std::size_t playing() const;

private:
    struct Voice {
        Pcm pcm;
        std::size_t at = 0;   // frames played
        float gain = 1;
    };
    mutable std::mutex m_;
    std::vector<Voice> v_;
};

}  // namespace ncap::soundboard

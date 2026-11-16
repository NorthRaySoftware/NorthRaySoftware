// NorthRay Capture: the sound visualiser's analysis (the radio show's aurora pulse). Takes the mixed sound
// (mono, any rate) and gives a few frequency bands, spaced like hearing (logarithmically), each 0..1 with a
// fast rise and a slower fall so they pulse rather than flicker, plus the overall loudness. Portable and
// unit-tested; the window feeds it the Stream mix and the aurora pulse source draws from it.
#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <vector>

namespace ncap::visualiser {

constexpr int kFft = 2048;   // samples per analysis (about 43 ms at 48 kHz)
constexpr int kMaxBands = 64;

struct Settings {
    int bands = 32;            // 4..64
    float lowHz = 40, highHz = 16000;
    float floorDb = -70;       // quieter than this shows as nothing
    float riseMs = 30, fallMs = 350;
};

class Analyser {
public:
    explicit Analyser(int sampleRate = 48000, Settings s = {});
    // Mono samples (-1..1) in arrival order; analysis happens every kFft / 2 samples.
    void push(const float* samples, std::size_t n);
    // The bands now (0..1) and the loudness (0..1, from the RMS level over floorDb..0 dB).
    const std::vector<float>& bands() const { return smooth_; }
    float level() const { return level_; }
    int sampleRate() const { return rate_; }

private:
    void analyse();
    int rate_;
    Settings s_;
    std::vector<float> ring_;
    std::size_t filled_ = 0, sinceLast_ = 0, pos_ = 0;
    std::vector<float> window_;
    std::vector<std::complex<float>> buf_;
    std::vector<int> edges_;   // FFT bins: band i covers [edges_[i], edges_[i+1])
    std::vector<float> smooth_;
    float level_ = 0;
};

// In-place radix-2 FFT (size a power of two).
void fft(std::vector<std::complex<float>>& a);

}  // namespace ncap::visualiser

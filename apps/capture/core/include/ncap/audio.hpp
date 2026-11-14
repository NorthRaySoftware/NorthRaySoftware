// NorthRay Capture: sound as numbers (part of systems 3 and 6). Portable and unit-tested.
//
// Every source is brought to one format on capture (Windows converts): 48 kHz, two channels,
// 32-bit float, interleaved. The mixer works in 10 ms blocks of that.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ncap::audio {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kBlock = kRate / 100;   // frames in 10 ms

constexpr float kSilenceDb = -100;   // what "no sound at all" reads as

float dbToGain(float db);            // -100 dB and below: 0
float gainToDb(float gain);          // 0: -100 dB

// The volume fader: position 0..1 to gain, on a curve that gives the top of the fader most of
// the room (where people adjust), like a mixing desk: 1 = 0 dB, 0.5 ≈ -18 dB, 0 = silence.
float faderToGain(float position);
float gainToFader(float gain);
std::wstring dbText(float db);       // "-6.0 dB", "0.0 dB", "-∞ dB"

// The loudest sample of one channel in an interleaved stereo block.
float peakOf(const float* interleaved, std::size_t frames, int channel);
// dst += src * gain, for `samples` floats.
void mixInto(float* dst, const float* src, std::size_t samples, float gain);

// A level meter as people read it: it jumps up at once, falls back at 20 dB a second, and keeps
// a line at the highest level for 1.5 s. Fed peaks (gain, not dB) with the time in seconds.
class Meter {
public:
    void feed(float peak, double now);
    float level(double now) const;   // dB
    float hold(double now) const;    // dB
    bool clipped(double now) const;  // reached 0 dB in the last 1.5 s

private:
    float levelDb_ = kSilenceDb, holdDb_ = kSilenceDb;
    double levelAt_ = 0, holdAt_ = -10, clipAt_ = -10;
};

// One writer thread and one reader thread pass interleaved stereo frames through this, without
// locks. Writing more than there's room for drops the newest (the reader is behind and catches up
// with `skip`).
class Ring {
public:
    explicit Ring(std::size_t frames);
    std::size_t write(const float* frames, std::size_t count);   // returns frames written
    std::size_t writeSilence(std::size_t count);
    std::size_t read(float* out, std::size_t count);              // returns frames read
    std::size_t available() const;
    void skip(std::size_t count);                                  // reader side: drop the oldest
    std::size_t capacity() const { return size_; }

private:
    std::vector<float> data_;
    std::size_t size_;
    std::atomic<std::size_t> head_{0}, tail_{0};   // frames written / read, ever
};

// The 44-byte header of a WAV file of 32-bit float samples.
std::vector<std::uint8_t> wavHeader(int rate, int channels, std::uint32_t dataBytes);

}  // namespace ncap::audio

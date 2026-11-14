#include "ncap/audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ncap::audio {

float dbToGain(float db) { return db <= kSilenceDb ? 0.0f : std::pow(10.0f, db / 20.0f); }

float gainToDb(float gain) {
    if (gain <= 0.00001f) return kSilenceDb;
    return std::max(kSilenceDb, 20.0f * std::log10(gain));
}

float faderToGain(float p) {
    p = std::clamp(p, 0.0f, 1.0f);
    return p * p * p;
}

float gainToFader(float g) { return g <= 0 ? 0.0f : std::min(1.0f, std::cbrt(g)); }

std::wstring dbText(float db) {
    if (db <= kSilenceDb + 0.05f) return L"-∞ dB";
    wchar_t b[32];
    const float r = std::round(db * 10) / 10;
    swprintf(b, 32, L"%.1f dB", r == 0 ? 0.0f : r);   // never "-0.0"
    return b;
}

float peakOf(const float* s, std::size_t frames, int channel) {
    float p = 0;
    for (std::size_t i = 0; i < frames; ++i) p = std::max(p, std::fabs(s[i * kChannels + (std::size_t)channel]));
    return p;
}

void mixInto(float* dst, const float* src, std::size_t samples, float gain) {
    if (gain == 0) return;
    for (std::size_t i = 0; i < samples; ++i) dst[i] += src[i] * gain;
}

// ---------------------------------------------------------------- Meter

namespace {
constexpr float kFall = 20;        // dB a second
constexpr double kHold = 1.5;      // seconds
}  // namespace

void Meter::feed(float peak, double now) {
    const float db = gainToDb(peak);
    if (db >= level(now)) {
        levelDb_ = db;
        levelAt_ = now;
    }
    if (db >= hold(now) || now - holdAt_ > kHold) {
        holdDb_ = db;
        holdAt_ = now;
    }
    if (peak >= 0.999f) clipAt_ = now;
}

float Meter::level(double now) const {
    const float fallen = levelDb_ - kFall * (float)std::max(0.0, now - levelAt_);
    return std::max(kSilenceDb, fallen);
}

float Meter::hold(double now) const { return now - holdAt_ <= kHold ? holdDb_ : level(now); }

bool Meter::clipped(double now) const { return now - clipAt_ <= kHold; }

// ---------------------------------------------------------------- Ring

Ring::Ring(std::size_t frames) : data_(frames * kChannels), size_(frames) {}

std::size_t Ring::available() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }

std::size_t Ring::write(const float* frames, std::size_t count) {
    const std::size_t head = head_.load(std::memory_order_relaxed), tail = tail_.load(std::memory_order_acquire);
    count = std::min(count, size_ - (head - tail));
    for (std::size_t done = 0; done < count;) {
        const std::size_t at = (head + done) % size_, run = std::min(count - done, size_ - at);
        if (frames) std::memcpy(&data_[at * kChannels], frames + done * kChannels, run * kChannels * sizeof(float));
        else std::memset(&data_[at * kChannels], 0, run * kChannels * sizeof(float));
        done += run;
    }
    head_.store(head + count, std::memory_order_release);
    return count;
}

std::size_t Ring::writeSilence(std::size_t count) { return write(nullptr, count); }

std::size_t Ring::read(float* out, std::size_t count) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed), head = head_.load(std::memory_order_acquire);
    count = std::min(count, head - tail);
    for (std::size_t done = 0; done < count;) {
        const std::size_t at = (tail + done) % size_, run = std::min(count - done, size_ - at);
        std::memcpy(out + done * kChannels, &data_[at * kChannels], run * kChannels * sizeof(float));
        done += run;
    }
    tail_.store(tail + count, std::memory_order_release);
    return count;
}

void Ring::skip(std::size_t count) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed), head = head_.load(std::memory_order_acquire);
    tail_.store(tail + std::min(count, head - tail), std::memory_order_release);
}

// ---------------------------------------------------------------- WAV

std::vector<std::uint8_t> wavHeader(int rate, int channels, std::uint32_t dataBytes) {
    std::vector<std::uint8_t> h;
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) h.push_back((std::uint8_t)(v >> (8 * i)));
    };
    auto put16 = [&](std::uint16_t v) {
        h.push_back((std::uint8_t)v);
        h.push_back((std::uint8_t)(v >> 8));
    };
    auto tag = [&](const char* t) { h.insert(h.end(), t, t + 4); };
    tag("RIFF");
    put32(36 + dataBytes);
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(3);   // IEEE float
    put16((std::uint16_t)channels);
    put32((std::uint32_t)rate);
    put32((std::uint32_t)(rate * channels * 4));
    put16((std::uint16_t)(channels * 4));
    put16(32);
    tag("data");
    put32(dataBytes);
    return h;
}

}  // namespace ncap::audio

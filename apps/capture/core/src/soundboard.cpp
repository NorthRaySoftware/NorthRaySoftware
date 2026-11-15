#include "ncap/soundboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ncap::soundboard {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Buf {
    std::vector<float> s;   // stereo
    explicit Buf(double seconds) : s((std::size_t)(seconds * kRate) * 2, 0.0f) {}
    std::size_t frames() const { return s.size() / 2; }
    void add(std::size_t frame, float l, float r) {
        if (frame < frames()) {
            s[frame * 2] += l;
            s[frame * 2 + 1] += r;
        }
    }
};

// A tone with a quick attack and an exponential decay, a little brightness from two harmonics.
void tone(Buf& b, double start, double freq, double length, double amp, double decay, double pan = 0, double bright = 0.3) {
    const std::size_t s0 = (std::size_t)(start * kRate), n = (std::size_t)(length * kRate);
    const float l = (float)(amp * std::sqrt(0.5 * (1 - pan))), r = (float)(amp * std::sqrt(0.5 * (1 + pan)));
    for (std::size_t i = 0; i < n; ++i) {
        const double t = (double)i / kRate;
        const double env = std::min(1.0, t / 0.004) * std::exp(-t * decay);
        const double v = std::sin(2 * kPi * freq * t) + bright * std::sin(4 * kPi * freq * t) * std::exp(-t * decay * 2) +
                         bright * 0.5 * std::sin(6 * kPi * freq * t) * std::exp(-t * decay * 3);
        b.add(s0 + i, (float)(v * env) * l, (float)(v * env) * r);
    }
}

// Deterministic noise (the same sound every time).
struct Noise {
    std::uint32_t x = 0x12345678;
    float next() {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return (float)((double)x / 4294967295.0 * 2 - 1);
    }
};

// A burst of filtered noise: a drum hit, a clap, a whoosh.
void noise(Buf& b, Noise& nz, double start, double length, double amp, double decay, double lowpass, double pan = 0) {
    const std::size_t s0 = (std::size_t)(start * kRate), n = (std::size_t)(length * kRate);
    const double a = std::exp(-2 * kPi * lowpass / kRate);
    double y = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = (double)i / kRate;
        const double env = std::min(1.0, t / 0.002) * std::exp(-t * decay);
        y = (1 - a) * nz.next() + a * y;
        const float v = (float)(y * env * amp * 3);
        b.add(s0 + i, v * (float)std::sqrt(0.5 * (1 - pan)), v * (float)std::sqrt(0.5 * (1 + pan)));
    }
}

// The sound made safe: below 0.9 at its loudest, faded at the ends, trailing silence trimmed.
std::vector<float> finish(Buf& b) {
    float peak = 0;
    for (float v : b.s) peak = std::max(peak, std::fabs(v));
    const float g = peak > 0 ? 0.85f / peak : 1;
    for (float& v : b.s) v *= g;
    std::size_t end = b.frames();
    while (end > 0 && std::fabs(b.s[(end - 1) * 2]) < 1e-4f && std::fabs(b.s[(end - 1) * 2 + 1]) < 1e-4f) --end;
    b.s.resize(end * 2);
    const std::size_t fade = std::min<std::size_t>(240, end / 4);   // 5 ms
    for (std::size_t i = 0; i < fade; ++i) {
        const float k = (float)i / (float)fade;
        b.s[i * 2] *= k, b.s[i * 2 + 1] *= k;
        b.s[(end - 1 - i) * 2] *= k, b.s[(end - 1 - i) * 2 + 1] *= k;
    }
    return std::move(b.s);
}

double note(int semitonesFromA4) { return 440.0 * std::pow(2.0, semitonesFromA4 / 12.0); }

}  // namespace

const std::vector<Starter>& starterPack() {
    static const std::vector<Starter> p = {
        {"chime", "Chime"},       {"ding-dong", "Ding-dong"}, {"fanfare", "Fanfare"}, {"level-up", "Level up"},
        {"drum-roll", "Drum roll"}, {"ba-dum-tss", "Ba-dum-tss"}, {"whoosh", "Whoosh"},   {"applause", "Applause"},
        {"buzzer", "Wrong answer"}, {"sparkle", "Sparkle"},      {"heartbeat", "Heartbeat"}, {"page-turn", "Page turn"},
    };
    return p;
}

std::vector<float> synth(const std::string& id) {
    Noise nz;
    if (id == "chime") {
        Buf b(2.2);
        tone(b, 0, note(15), 2.0, 0.5, 2.2, -0.2);    // C6
        tone(b, 0.12, note(19), 2.0, 0.45, 2.2, 0.2);  // E6
        tone(b, 0.24, note(22), 2.0, 0.4, 2.0);        // G6
        return finish(b);
    }
    if (id == "ding-dong") {
        Buf b(2.4);
        tone(b, 0, note(7), 1.4, 0.6, 2.0, -0.1, 0.5);    // E5
        tone(b, 0.55, note(3), 1.8, 0.6, 1.8, 0.1, 0.5);  // C5
        return finish(b);
    }
    if (id == "fanfare") {
        Buf b(2.0);
        const int notes[] = {-2, 2, 5, 10};   // G4 B4 D5 G5
        const double at[] = {0, 0.14, 0.28, 0.42}, len[] = {0.14, 0.14, 0.14, 1.4};
        for (int i = 0; i < 4; ++i)
            for (int h = 1; h <= 4; ++h) tone(b, at[i], note(notes[i]) * h, len[i] + 0.05, 0.35 / h, i == 3 ? 2.0 : 8.0, 0, 0);
        return finish(b);
    }
    if (id == "level-up") {
        Buf b(1.2);
        const int notes[] = {3, 7, 10, 15, 19, 22};
        for (int i = 0; i < 6; ++i) {
            const double t0 = i * 0.07;
            const std::size_t s0 = (std::size_t)(t0 * kRate), n = (std::size_t)((i == 5 ? 0.6 : 0.09) * kRate);
            const double f = note(notes[i]);
            for (std::size_t k = 0; k < n; ++k) {   // square-ish, like an old console
                const double t = (double)k / kRate;
                const double v = (std::sin(2 * kPi * f * t) > 0 ? 1 : -1) * 0.18 * std::exp(-t * (i == 5 ? 4 : 10));
                b.add(s0 + k, (float)v, (float)v);
            }
        }
        return finish(b);
    }
    if (id == "drum-roll") {
        Buf b(2.6);
        for (double t = 0; t < 1.9; t += 0.045) noise(b, nz, t, 0.08, 0.25 + 0.25 * t / 1.9, 40, 3000, std::sin(t * 20) * 0.2);
        noise(b, nz, 1.95, 0.6, 0.8, 6, 6000);   // the cymbal at the end
        tone(b, 1.95, 60, 0.4, 0.7, 10, 0, 0);    // and a bass drum
        return finish(b);
    }
    if (id == "ba-dum-tss") {
        Buf b(1.6);
        tone(b, 0, 180, 0.25, 0.6, 18, -0.2, 0.2);
        noise(b, nz, 0, 0.12, 0.3, 30, 2500, -0.2);
        tone(b, 0.18, 120, 0.3, 0.6, 14, 0.2, 0.2);
        noise(b, nz, 0.18, 0.12, 0.3, 30, 2500, 0.2);
        noise(b, nz, 0.42, 1.1, 0.7, 4, 9000, 0.3);   // tss
        return finish(b);
    }
    if (id == "whoosh") {
        Buf b(1.0);
        const std::size_t n = (std::size_t)(0.9 * kRate);
        double y = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double t = (double)i / n;
            const double cut = 300 + 5000 * std::sin(kPi * t);   // the filter opens and closes
            const double a = std::exp(-2 * kPi * cut / kRate);
            y = (1 - a) * nz.next() + a * y;
            const double env = std::sin(kPi * t);
            const double pan = -0.8 + 1.6 * t;   // left to right
            b.add(i, (float)(y * env * std::sqrt(0.5 * (1 - pan))), (float)(y * env * std::sqrt(0.5 * (1 + pan))));
        }
        return finish(b);
    }
    if (id == "applause") {
        Buf b(3.2);
        std::uint32_t seed = 7;
        for (int i = 0; i < 260; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double t = 0.05 + (double)(seed % 1000) / 1000.0 * 2.6;
            seed = seed * 1664525u + 1013904223u;
            const double pan = ((double)(seed % 1000) / 1000.0) * 1.6 - 0.8;
            const double fade = t > 2.0 ? std::max(0.0, 1 - (t - 2.0) / 0.7) : std::min(1.0, t / 0.3);
            noise(b, nz, t, 0.03, 0.25 * fade, 120, 2500 + (seed % 3000), pan);
        }
        return finish(b);
    }
    if (id == "buzzer") {
        Buf b(1.0);
        const std::size_t n = (std::size_t)(0.8 * kRate);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = (double)i / kRate;
            const double f = 150 - 30 * t;
            const double saw = 2 * (t * f - std::floor(t * f + 0.5));
            const double env = std::min(1.0, t / 0.01) * (t > 0.7 ? (0.8 - t) / 0.1 : 1);
            b.add(i, (float)(saw * env * 0.4), (float)(saw * env * 0.4));
        }
        return finish(b);
    }
    if (id == "sparkle") {
        Buf b(1.6);
        std::uint32_t seed = 3;
        for (int i = 0; i < 14; ++i) {
            seed = seed * 1664525u + 1013904223u;
            tone(b, i * 0.05, note(24 + (int)(seed % 12)), 0.6, 0.25, 7, ((int)(seed >> 8) % 100) / 60.0 - 0.8, 0.1);
        }
        return finish(b);
    }
    if (id == "heartbeat") {
        Buf b(2.0);
        for (int beat = 0; beat < 2; ++beat) {
            tone(b, beat * 0.9, 55, 0.25, 0.8, 16, 0, 0);
            tone(b, beat * 0.9 + 0.22, 48, 0.3, 0.6, 14, 0, 0);
        }
        return finish(b);
    }
    if (id == "page-turn") {
        Buf b(0.7);
        noise(b, nz, 0, 0.35, 0.3, 9, 4000, -0.3);
        noise(b, nz, 0.18, 0.4, 0.4, 12, 7000, 0.3);
        return finish(b);
    }
    return {};
}

void Voices::play(const Pcm& pcm, float gain, bool restart) {
    if (!pcm || pcm->empty()) return;
    std::lock_guard<std::mutex> l(m_);
    if (restart)
        for (auto& v : v_)
            if (v.pcm == pcm) {
                v.at = 0;
                v.gain = gain;
                return;
            }
    if (v_.size() >= 32) v_.erase(v_.begin());   // the oldest makes way
    v_.push_back({pcm, 0, gain});
}

void Voices::stopAll() {
    std::lock_guard<std::mutex> l(m_);
    v_.clear();
}

std::size_t Voices::playing() const {
    std::lock_guard<std::mutex> l(m_);
    return v_.size();
}

void Voices::mix(float* out, std::size_t frames) {
    std::lock_guard<std::mutex> l(m_);
    for (auto& v : v_) {
        const std::size_t total = v.pcm->size() / 2;
        const std::size_t n = std::min(frames, total - std::min(total, v.at));
        const float* src = v.pcm->data() + v.at * 2;
        for (std::size_t i = 0; i < n * 2; ++i) out[i] += src[i] * v.gain;
        v.at += n;
    }
    std::erase_if(v_, [](const Voice& v) { return v.at >= v.pcm->size() / 2; });
}

}  // namespace ncap::soundboard

#include "ncap/visualiser.hpp"

#include <algorithm>
#include <cmath>

namespace ncap::visualiser {

namespace {
constexpr float kPi = 3.14159265358979f;
}

void fft(std::vector<std::complex<float>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2 * kPi / (float)len;
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

Analyser::Analyser(int sampleRate, Settings s) : rate_(std::clamp(sampleRate, 8000, 192000)), s_(s) {
    s_.bands = std::clamp(s_.bands, 4, kMaxBands);
    s_.highHz = std::clamp(s_.highHz, 200.0f, rate_ / 2.0f);
    s_.lowHz = std::clamp(s_.lowHz, 10.0f, s_.highHz / 4);
    ring_.assign(kFft, 0);
    window_.resize(kFft);
    for (int i = 0; i < kFft; ++i) window_[(size_t)i] = 0.5f - 0.5f * std::cos(2 * kPi * i / (kFft - 1));   // Hann
    buf_.resize(kFft);
    // Band edges spaced logarithmically from lowHz to highHz, each at least one bin wide.
    const float binHz = (float)rate_ / kFft;
    edges_.resize((size_t)s_.bands + 1);
    for (int i = 0; i <= s_.bands; ++i) {
        const float hz = s_.lowHz * std::pow(s_.highHz / s_.lowHz, (float)i / s_.bands);
        edges_[(size_t)i] = std::max(1, (int)std::lround(hz / binHz));
    }
    for (int i = 1; i <= s_.bands; ++i) edges_[(size_t)i] = std::max(edges_[(size_t)i], edges_[(size_t)i - 1] + 1);
    for (auto& e : edges_) e = std::min(e, kFft / 2);
    smooth_.assign((size_t)s_.bands, 0);
}

void Analyser::push(const float* samples, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const float x = std::isfinite(samples[i]) ? std::clamp(samples[i], -1.0f, 1.0f) : 0.0f;
        ring_[pos_] = x;
        pos_ = (pos_ + 1) % kFft;
        filled_ = std::min<std::size_t>(filled_ + 1, kFft);
        if (++sinceLast_ >= kFft / 2) {
            sinceLast_ = 0;
            analyse();
        }
    }
}

void Analyser::analyse() {
    // The last kFft samples, oldest first, windowed.
    double sq = 0;
    for (int i = 0; i < kFft; ++i) {
        const float x = ring_[(pos_ + (size_t)i) % kFft];
        sq += (double)x * x;
        buf_[(size_t)i] = {x * window_[(size_t)i], 0};
    }
    fft(buf_);
    // Each band's strongest bin, in dB relative to a full-scale sine (amplitude 1 gives about 0 dB).
    const float norm = 4.0f / kFft;   // the Hann window's coherent gain (0.5) and the one-sided spectrum (2)
    const float dt = (float)(kFft / 2) / rate_ * 1000;   // ms since the last analysis
    const float up = 1 - std::exp(-dt / std::max(1.0f, s_.riseMs)), down = 1 - std::exp(-dt / std::max(1.0f, s_.fallMs));
    for (int b = 0; b < s_.bands; ++b) {
        float peak = 0;
        for (int k = edges_[(size_t)b]; k < std::max(edges_[(size_t)b + 1], edges_[(size_t)b] + 1) && k < kFft / 2; ++k)
            peak = std::max(peak, std::abs(buf_[(size_t)k]) * norm);
        const float db = 20 * std::log10(std::max(peak, 1e-9f));
        const float v = std::clamp((db - s_.floorDb) / -s_.floorDb, 0.0f, 1.0f);
        float& sm = smooth_[(size_t)b];
        sm += (v - sm) * (v > sm ? up : down);
    }
    const float rms = (float)std::sqrt(sq / kFft);
    const float ldb = 20 * std::log10(std::max(rms, 1e-9f));
    const float lv = std::clamp((ldb - s_.floorDb) / -s_.floorDb, 0.0f, 1.0f);
    level_ += (lv - level_) * (lv > level_ ? up : down);
}

}  // namespace ncap::visualiser

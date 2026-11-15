#include "ncap/dsp.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace ncap::dsp {

namespace {
constexpr float kRate = 48000.0f;
constexpr float kPi = 3.14159265358979f;
const char* kKeys[] = {"gain", "noise-gate", "noise-suppression", "expander", "compressor", "limiter", "equaliser", "balance", "delay", "plugin"};
const char* kLabels[] = {"Gain", "Noise gate", "Noise suppression", "Expander", "Compressor", "Limiter", "Equaliser", "Balance", "Delay", "Plugin"};

// Time constant for an attack or release of `ms`.
float coeff(float ms) { return ms <= 0 ? 0.0f : std::exp(-1.0f / (ms * 0.001f * kRate)); }
}  // namespace

float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }
float linearToDb(float lin) { return lin <= 1e-6f ? -120.0f : 20.0f * std::log10(lin); }

const char* key(Type t) { return kKeys[(int)t]; }
const char* label(Type t) { return kLabels[(int)t]; }
bool parse(const std::string& k, Type& out) {
    for (int i = 0; i <= kTypes; ++i)
        if (k == kKeys[i]) {
            out = (Type)i;
            return true;
        }
    return false;
}

const std::vector<Param>& params(Type t) {
    static const std::vector<Param> p[kTypes + 1] = {
        {{"gain", "Gain", -30, 30, 0, "dB"}},
        {{"open", "Opens above", -80, 0, -36, "dB"}, {"close", "Closes below", -80, 0, -42, "dB"}, {"attack", "Attack", 0.1f, 100, 2, "ms"},
         {"hold", "Hold", 0, 1000, 150, "ms"}, {"release", "Release", 5, 2000, 150, "ms"}},
        {{"strength", "Reduction", 3, 40, 18, "dB"}},
        {{"threshold", "Below", -80, 0, -45, "dB"}, {"ratio", "Ratio", 1, 20, 3, ":1"}, {"attack", "Attack", 0.1f, 100, 5, "ms"},
         {"release", "Release", 5, 2000, 100, "ms"}},
        {{"threshold", "Above", -60, 0, -18, "dB"}, {"ratio", "Ratio", 1, 20, 3, ":1"}, {"attack", "Attack", 0.1f, 100, 6, "ms"},
         {"release", "Release", 5, 2000, 60, "ms"}, {"makeup", "Make-up gain", 0, 30, 0, "dB"}},
        {{"ceiling", "Ceiling", -20, 0, -1, "dB"}, {"release", "Release", 5, 1000, 60, "ms"}},
        {{"low", "Low (100 Hz)", -15, 15, 0, "dB"}, {"mid", "Middle", -15, 15, 0, "dB"}, {"midFreq", "Middle at", 200, 6000, 1000, "Hz"},
         {"high", "High (8 kHz)", -15, 15, 0, "dB"}},
        {{"balance", "Balance", -100, 100, 0, "%"}},
        {{"delay", "Delay", 0, 2000, 0, "ms"}},
        {},
    };
    return p[(int)t];
}

float Filter::get(const std::string& k) const {
    auto it = values.find(k);
    if (it != values.end()) return it->second;
    for (auto& p : params(type))
        if (k == p.key) return p.def;
    return 0;
}

Filter Filter::make(Type t) {
    Filter f;
    f.type = t;
    for (auto& p : params(t)) f.values[p.key] = p.def;
    return f;
}

std::string toJson(const Chain& c) {
    nlohmann::json a = nlohmann::json::array();
    for (auto& f : c) {
        nlohmann::json v = nlohmann::json::object();
        for (auto& [k, x] : f.values) v[k] = x;
        nlohmann::json e = {{"type", key(f.type)}, {"enabled", f.enabled}, {"values", v}};
        if (f.type == Type::Plugin) e["plugin"] = f.plugin;
        a.push_back(e);
    }
    return a.dump();
}

Chain fromJson(const std::string& text) {
    Chain c;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) return c;
    for (auto& e : j) {
        if (!e.is_object() || !e.contains("type") || !e["type"].is_string() || c.size() >= 16) continue;
        Type t;
        if (!parse(e["type"].get<std::string>(), t)) continue;
        Filter f = Filter::make(t);
        f.enabled = !e.contains("enabled") || !e["enabled"].is_boolean() || e["enabled"].get<bool>();
        if (t == Type::Plugin) {
            // Its values are the plugin's own; the host clamps them to the plugin's ranges.
            if (!e.contains("plugin") || !e["plugin"].is_string()) continue;
            f.plugin = e["plugin"].get<std::string>();
            if (f.plugin.empty() || f.plugin.size() > 128 || f.plugin.find_first_of("/\\:") != std::string::npos) continue;
            if (e.contains("values") && e["values"].is_object())
                for (auto& [k, v] : e["values"].items())
                    if (v.is_number() && k.size() <= 32 && f.values.size() < 32 && std::isfinite(v.get<float>())) f.values[k] = v.get<float>();
        } else if (e.contains("values") && e["values"].is_object())
            for (auto& p : params(t))
                if (e["values"].contains(p.key) && e["values"][p.key].is_number()) f.values[p.key] = std::clamp(e["values"][p.key].get<float>(), p.min, p.max);
        c.push_back(f);
    }
    return c;
}

std::string describe(const Filter& f) {
    char b[96];
    switch (f.type) {
        case Type::Gain: std::snprintf(b, sizeof b, "%+.1f dB", f.get("gain")); break;
        case Type::NoiseGate: std::snprintf(b, sizeof b, "opens above %.0f dB", f.get("open")); break;
        case Type::NoiseSuppression: std::snprintf(b, sizeof b, "up to %.0f dB quieter", f.get("strength")); break;
        case Type::Expander: std::snprintf(b, sizeof b, "below %.0f dB, %.1f:1", f.get("threshold"), f.get("ratio")); break;
        case Type::Compressor: std::snprintf(b, sizeof b, "above %.0f dB, %.1f:1", f.get("threshold"), f.get("ratio")); break;
        case Type::Limiter: std::snprintf(b, sizeof b, "ceiling %.1f dB", f.get("ceiling")); break;
        case Type::Equaliser: std::snprintf(b, sizeof b, "%+.0f / %+.0f / %+.0f dB", f.get("low"), f.get("mid"), f.get("high")); break;
        case Type::Balance: {
            const float v = f.get("balance");
            if (std::fabs(v) < 0.5f) std::snprintf(b, sizeof b, "centre");
            else std::snprintf(b, sizeof b, "%.0f%% %s", std::fabs(v), v < 0 ? "left" : "right");
            break;
        }
        case Type::Delay: std::snprintf(b, sizeof b, "%.0f ms", f.get("delay")); break;
        case Type::Plugin: std::snprintf(b, sizeof b, "%s", f.plugin.c_str()); break;
    }
    return std::string(label(f.type)) + ": " + b;
}

// ------------------------------------------------------------------ the stages

class Stage {
public:
    virtual ~Stage() = default;
    virtual void process(float* x, std::size_t n) = 0;   // interleaved stereo
    virtual int latency() const { return 0; }
    virtual float reductionDb() const { return 0; }
};

namespace {

class GainStage : public Stage {
public:
    explicit GainStage(const Filter& f) : g_(dbToLinear(f.get("gain"))) {}
    void process(float* x, std::size_t n) override {
        for (std::size_t i = 0; i < n * 2; ++i) x[i] *= g_;
    }

private:
    float g_;
};

// A level follower on the louder channel: fast up (attack), slow down (release), in linear terms.
struct Follower {
    float env = 0, att = 0, rel = 0;
    void set(float attackMs, float releaseMs) {
        att = coeff(attackMs);
        rel = coeff(releaseMs);
    }
    float step(float in) {
        const float c = in > env ? att : rel;
        env = in + c * (env - in);
        return env;
    }
};

class GateStage : public Stage {
public:
    explicit GateStage(const Filter& f) {
        open_ = dbToLinear(f.get("open"));
        close_ = dbToLinear(std::min(f.get("close"), f.get("open")));
        att_ = coeff(f.get("attack"));
        rel_ = coeff(f.get("release"));
        holdFrames_ = (int)(f.get("hold") * 0.001f * kRate);
        det_.set(0.5f, 20);
    }
    void process(float* x, std::size_t n) override {
        float minGain = 1;
        for (std::size_t i = 0; i < n; ++i) {
            const float level = det_.step(std::max(std::fabs(x[i * 2]), std::fabs(x[i * 2 + 1])));
            if (level >= open_) {
                isOpen_ = true;
                held_ = 0;
            } else if (isOpen_ && level < close_) {
                if (++held_ > holdFrames_) isOpen_ = false;
            }
            const float target = isOpen_ ? 1.0f : 0.0f;
            const float c = target > gain_ ? att_ : rel_;
            gain_ = target + c * (gain_ - target);
            x[i * 2] *= gain_;
            x[i * 2 + 1] *= gain_;
            minGain = std::min(minGain, gain_);
        }
        reduction_ = linearToDb(minGain);
    }
    float reductionDb() const override { return std::max(-60.0f, reduction_); }

private:
    float open_, close_, att_, rel_, gain_ = 0, reduction_ = 0;
    int holdFrames_, held_ = 0;
    bool isOpen_ = false;
    Follower det_;
};

// Compressor and expander: a gain computer in dB on a smoothed level.
class DynamicsStage : public Stage {
public:
    DynamicsStage(const Filter& f, bool expand) : expand_(expand) {
        threshold_ = f.get("threshold");
        ratio_ = std::max(1.0f, f.get("ratio"));
        makeup_ = expand ? 0.0f : f.get("makeup");
        att_ = coeff(f.get("attack"));
        rel_ = coeff(f.get("release"));
        level_.set(0.5f, 30);   // the level: follows peaks quickly, falls over a wave or two
    }
    void process(float* x, std::size_t n) override {
        float most = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const float levelDb = linearToDb(level_.step(std::max(std::fabs(x[i * 2]), std::fabs(x[i * 2 + 1]))));
            float wantDb = 0;   // gain change wanted (negative: quieter)
            if (!expand_ && levelDb > threshold_) wantDb = (threshold_ - levelDb) * (1 - 1 / ratio_);
            if (expand_ && levelDb < threshold_) wantDb = std::max(-60.0f, (levelDb - threshold_) * (ratio_ - 1));
            // Smooth the gain in dB: towards more reduction at the attack speed, back at the release.
            const float c = wantDb < gainDb_ ? att_ : rel_;
            gainDb_ = wantDb + c * (gainDb_ - wantDb);
            const float g = dbToLinear(gainDb_ + makeup_);
            x[i * 2] *= g;
            x[i * 2 + 1] *= g;
            most = std::min(most, gainDb_);
        }
        reduction_ = most;
    }
    float reductionDb() const override { return reduction_; }

private:
    bool expand_;
    float threshold_, ratio_, makeup_, att_, rel_, gainDb_ = 0, reduction_ = 0;
    Follower level_;
};

// A look-ahead peak limiter: the sound is delayed 1.5 ms so the gain is already down when a peak arrives.
class LimiterStage : public Stage {
public:
    explicit LimiterStage(const Filter& f) : line_((std::size_t)kAhead * 2, 0.0f) {
        ceiling_ = dbToLinear(f.get("ceiling"));
        rel_ = coeff(f.get("release"));
    }
    void process(float* x, std::size_t n) override {
        float most = 1;
        for (std::size_t i = 0; i < n; ++i) {
            // The gain needed for the sample entering the look-ahead window.
            const float peak = std::max(std::fabs(x[i * 2]), std::fabs(x[i * 2 + 1]));
            const float need = peak > ceiling_ ? ceiling_ / peak : 1.0f;
            // Hold the smallest gain needed over the window; move down at once (over the window), up at the release.
            needs_[pos_] = need;
            float target = 1;
            for (float v : needs_) target = std::min(target, v);
            if (target < gain_) gain_ = target;
            else gain_ = target + rel_ * (gain_ - target);
            // Out: the delayed sample, at the gain.
            const float l = line_[pos_ * 2], r = line_[pos_ * 2 + 1];
            line_[pos_ * 2] = x[i * 2];
            line_[pos_ * 2 + 1] = x[i * 2 + 1];
            pos_ = (pos_ + 1) % kAhead;
            x[i * 2] = std::clamp(l * gain_, -ceiling_, ceiling_);   // never over, whatever happens
            x[i * 2 + 1] = std::clamp(r * gain_, -ceiling_, ceiling_);
            most = std::min(most, gain_);
        }
        reduction_ = linearToDb(most);
    }
    int latency() const override { return kAhead; }
    float reductionDb() const override { return reduction_; }

private:
    static constexpr int kAhead = 72;   // 1.5 ms
    std::vector<float> line_;
    float needs_[kAhead] = {};
    std::size_t pos_ = 0;
    float ceiling_, rel_, gain_ = 1, reduction_ = 0;
};

// RBJ biquads.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] = {0, 0}, z2[2] = {0, 0};
    float run(float in, int ch) {
        const float out = b0 * in + z1[ch];
        z1[ch] = b1 * in - a1 * out + z2[ch];
        z2[ch] = b2 * in - a2 * out;
        return out;
    }
    void set(float nb0, float nb1, float nb2, float na0, float na1, float na2) {
        b0 = nb0 / na0;
        b1 = nb1 / na0;
        b2 = nb2 / na0;
        a1 = na1 / na0;
        a2 = na2 / na0;
    }
    void lowShelf(float f, float db) {
        const float A = std::pow(10.0f, db / 40), w = 2 * kPi * f / kRate, cw = std::cos(w), sw = std::sin(w);
        const float alpha = sw / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * alpha;
        set(A * ((A + 1) - (A - 1) * cw + sa), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sa), (A + 1) + (A - 1) * cw + sa,
            -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sa);
    }
    void highShelf(float f, float db) {
        const float A = std::pow(10.0f, db / 40), w = 2 * kPi * f / kRate, cw = std::cos(w), sw = std::sin(w);
        const float alpha = sw / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * alpha;
        set(A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sa), (A + 1) - (A - 1) * cw + sa,
            2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa);
    }
    void peak(float f, float db, float q) {
        const float A = std::pow(10.0f, db / 40), w = 2 * kPi * f / kRate, cw = std::cos(w), alpha = std::sin(w) / (2 * q);
        set(1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
    }
};

class EqStage : public Stage {
public:
    explicit EqStage(const Filter& f) {
        low_.lowShelf(100, f.get("low"));
        mid_.peak(f.get("midFreq"), f.get("mid"), 1.0f);
        high_.highShelf(8000, f.get("high"));
    }
    void process(float* x, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch) {
                float v = x[i * 2 + (std::size_t)ch];
                v = high_.run(mid_.run(low_.run(v, ch), ch), ch);
                x[i * 2 + (std::size_t)ch] = v;
            }
    }

private:
    Biquad low_, mid_, high_;
};

class BalanceStage : public Stage {
public:
    explicit BalanceStage(const Filter& f) {
        // Constant power: centre leaves both as they are; full left silences the right (and lifts nothing).
        const float b = std::clamp(f.get("balance") / 100.0f, -1.0f, 1.0f);
        l_ = b > 0 ? std::cos(b * kPi / 2) : 1.0f;
        r_ = b < 0 ? std::cos(-b * kPi / 2) : 1.0f;
    }
    void process(float* x, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) {
            x[i * 2] *= l_;
            x[i * 2 + 1] *= r_;
        }
    }

private:
    float l_, r_;
};

class DelayStage : public Stage {
public:
    explicit DelayStage(const Filter& f) : frames_((std::size_t)(f.get("delay") * 0.001f * kRate + 0.5f)), line_(frames_ * 2 + 2, 0.0f) {}
    void process(float* x, std::size_t n) override {
        if (!frames_) return;
        for (std::size_t i = 0; i < n * 2; ++i) {
            const float out = line_[pos_];
            line_[pos_] = x[i];
            x[i] = out;
            pos_ = (pos_ + 1) % (frames_ * 2);
        }
    }
    int latency() const override { return (int)frames_; }

private:
    std::size_t frames_;
    std::vector<float> line_;
    std::size_t pos_ = 0;
};

// Noise suppression: short-time spectra (512-point, half overlapping, Hann windows), a noise level
// per frequency that follows the quietest the sound has been lately, and a gain per frequency that
// lowers what's at or near that level (by up to `strength` dB), smoothed so it doesn't warble.
class NoiseStage : public Stage {
public:
    explicit NoiseStage(const Filter& f) {
        floor_ = dbToLinear(-f.get("strength"));
        for (int i = 0; i < kN; ++i) window_[i] = 0.5f - 0.5f * std::cos(2 * kPi * (float)i / kN);   // periodic Hann: sums to 1 at 50% overlap
        for (auto& ch : ch_) {
            ch.in.assign(kN, 0.0f);
            ch.out.assign(kN, 0.0f);
            ch.noise.assign(kN / 2 + 1, 0.0f);
            ch.gain.assign(kN / 2 + 1, 1.0f);
            ch.smooth.assign(kN / 2 + 1, 0.0f);
        }
    }
    void process(float* x, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) {
            for (int c = 0; c < 2; ++c) {
                Channel& ch = ch_[c];
                ch.in[(std::size_t)(kHop + fill_)] = x[i * 2 + (std::size_t)c];
                x[i * 2 + (std::size_t)c] = ch.out[(std::size_t)fill_];
            }
            if (++fill_ == kHop) {
                fill_ = 0;
                for (auto& ch : ch_) frame(ch);
            }
        }
    }
    int latency() const override { return kN; }

private:
    static constexpr int kN = 512, kHop = 256;
    struct Channel {
        std::vector<float> in, out, noise, gain, smooth;
        bool primed = false;
        int frames = 0;
    };
    static void fft(std::vector<std::complex<float>>& a, bool inverse) {
        const std::size_t n = a.size();
        for (std::size_t i = 1, j = 0; i < n; ++i) {
            std::size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (std::size_t len = 2; len <= n; len <<= 1) {
            const float ang = 2 * kPi / (float)len * (inverse ? 1.0f : -1.0f);
            const std::complex<float> wl(std::cos(ang), std::sin(ang));
            for (std::size_t i = 0; i < n; i += len) {
                std::complex<float> w(1);
                for (std::size_t k = 0; k < len / 2; ++k) {
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
        if (inverse)
            for (auto& v : a) v /= (float)n;
    }
    void frame(Channel& ch) {
        std::vector<std::complex<float>> spec(kN);
        for (int i = 0; i < kN; ++i) spec[(std::size_t)i] = ch.in[(std::size_t)i] * window_[i];
        fft(spec, false);
        ++ch.frames;
        for (int k = 0; k <= kN / 2; ++k) {
            // This frequency's power, smoothed over a few frames (single frames of noise jump about).
            float& ps = ch.smooth[(std::size_t)k];
            const float p = std::norm(spec[(std::size_t)k]);
            ps = ch.primed ? ps * 0.75f + p * 0.25f : p;
            // The noise level: the quietest the smoothed power has been, rising slowly (about 4 dB a second)
            // so it follows noise that gets louder.
            float& nz = ch.noise[(std::size_t)k];
            if (!ch.primed || ps < nz) nz = ps;
            else nz *= 1.005f;
            // The minimum sits below the noise's average: counted three times over, steady noise falls to
            // the floor while anything clearly above it passes.
            const float snr = ps / std::max(nz * 3.0f, 1e-12f);
            const float g = snr <= 1 ? floor_ : std::max(floor_, 1 - 1 / snr);
            float& sm = ch.gain[(std::size_t)k];
            sm = g > sm ? sm * 0.3f + g * 0.7f : sm * 0.7f + g * 0.3f;   // open quickly, close gently
            spec[(std::size_t)k] *= sm;
            if (k > 0 && k < kN / 2) spec[(std::size_t)(kN - k)] *= sm;
        }
        ch.primed = ch.frames > 2;
        fft(spec, true);
        // Overlap-add: the first half finishes what the last frame started; shift the input along.
        for (int i = 0; i < kHop; ++i) ch.out[(std::size_t)i] = ch.out[(std::size_t)(i + kHop)] + spec[(std::size_t)i].real();
        for (int i = kHop; i < kN; ++i) ch.out[(std::size_t)i] = spec[(std::size_t)i].real();
        std::copy(ch.in.begin() + kHop, ch.in.end(), ch.in.begin());
    }
    float window_[kN];
    float floor_;
    Channel ch_[2];
    int fill_ = 0;
};

}  // namespace

namespace {
ExternalFactory gFactory = nullptr;
class ExternalStage : public Stage {
public:
    explicit ExternalStage(std::unique_ptr<External> e) : e_(std::move(e)) {}
    void process(float* x, std::size_t n) override { e_->process(x, n); }
    int latency() const override { return e_->latency(); }

private:
    std::unique_ptr<External> e_;
};
}  // namespace

void setExternalFactory(ExternalFactory f) { gFactory = f; }

Processor::Processor(const Chain& chain) {
    for (auto& f : chain) {
        if (!f.enabled) continue;
        switch (f.type) {
            case Type::Gain: stages_.push_back(std::make_unique<GainStage>(f)); break;
            case Type::NoiseGate: stages_.push_back(std::make_unique<GateStage>(f)); break;
            case Type::NoiseSuppression: stages_.push_back(std::make_unique<NoiseStage>(f)); break;
            case Type::Expander: stages_.push_back(std::make_unique<DynamicsStage>(f, true)); break;
            case Type::Compressor: stages_.push_back(std::make_unique<DynamicsStage>(f, false)); break;
            case Type::Limiter: stages_.push_back(std::make_unique<LimiterStage>(f)); break;
            case Type::Equaliser: stages_.push_back(std::make_unique<EqStage>(f)); break;
            case Type::Balance: stages_.push_back(std::make_unique<BalanceStage>(f)); break;
            case Type::Delay: stages_.push_back(std::make_unique<DelayStage>(f)); break;
            case Type::Plugin: {
                std::unique_ptr<External> e = gFactory ? gFactory(f.plugin, f.values) : nullptr;
                if (e) stages_.push_back(std::make_unique<ExternalStage>(std::move(e)));
                else notRunning_.push_back(f.plugin);
                break;
            }
        }
    }
    for (auto& st : stages_) latency_ += st->latency();
}

Processor::~Processor() = default;

void Processor::process(float* frames, std::size_t count) {
    float red = 0;
    for (auto& s : stages_) {
        s->process(frames, count);
        red = std::min(red, s->reductionDb());
    }
    reduction_ = red;
}



std::pair<float, float> Ducker::block(float triggerPeak, float blockMs, const DuckSettings& st) {
    const float low = dbToLinear(std::clamp(st.amountDb, -60.0f, 0.0f));
    const bool loud = linearToDb(triggerPeak) > st.thresholdDb;
    quietMs_ = loud ? 0 : quietMs_ + blockMs;
    target_ = (loud || quietMs_ < (float)std::max(0, st.holdMs)) ? low : 1.0f;
    const float start = gain_;
    // Move in decibels at a steady rate: the whole way down in attackMs, back up in releaseMs.
    const float span = std::max(0.5f, -linearToDb(low));
    const float now = linearToDb(gain_), want = linearToDb(target_);
    float next = now;
    if (want < now) next = std::max(want, now - span * blockMs / (float)std::max(1, st.attackMs));
    else if (want > now) next = std::min(want, now + span * blockMs / (float)std::max(1, st.releaseMs));
    gain_ = target_ >= 0.999f && next > -0.05f ? 1.0f : dbToLinear(next);
    return {start, gain_};
}

}  // namespace ncap::dsp

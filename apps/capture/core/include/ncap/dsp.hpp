// NorthRay Capture: per-source audio filters (system 7, "Audio Processing"). Portable and tested.
//
// Each sound source has a chain of filters, applied in order to its 48 kHz stereo sound before its
// volume, routing and meters. The chain is kept as JSON in the source's settings; the mixer runs a
// Processor built from it (stateful: envelopes, delay lines), rebuilt when the chain changes.
#pragma once

#include <utility>

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ncap::dsp {

// The built-in filters, then Plugin: an audio-effect plugin the user has switched on (plugins.hpp).
enum class Type { Gain, NoiseGate, NoiseSuppression, Expander, Compressor, Limiter, Equaliser, Balance, Delay, Plugin };
const char* key(Type t);                       // "gain", "noise-gate"... "plugin"
const char* label(Type t);                     // "Noise gate"
bool parse(const std::string& key, Type& out);
constexpr int kTypes = 9;                      // the built-in ones (Plugin is after them)

// One adjustable value of a filter: its key, its words, its range and default, and its unit.
struct Param {
    const char* key;
    const char* label;
    float min, max, def;
    const char* unit;   // "dB", "ms", ":1", "Hz", "%", ""
};
const std::vector<Param>& params(Type t);    // none for Plugin: a plugin describes its own

struct Filter {
    Type type = Type::Gain;
    bool enabled = true;
    std::map<std::string, float> values;   // missing ones take the default
    std::string plugin;                    // Plugin: its file name ("tremolo.dll")
    float get(const std::string& k) const;
    static Filter make(Type t);            // with every default
};

using Chain = std::vector<Filter>;
std::string toJson(const Chain& c);        // a JSON array
Chain fromJson(const std::string& json);   // unknown filters and values dropped, values clamped
std::string describe(const Filter& f);     // "Compressor: -18 dB, 3:1"

class Stage;   // one filter's running state

// A plugin's running instance, made by whoever hosts plugins (the app); the chain only runs it.
class External {
public:
    virtual ~External() = default;
    virtual void process(float* frames, std::size_t count) = 0;   // interleaved stereo, 48 kHz, in place
    virtual int latency() const { return 0; }
};
// Makes an instance of `plugin` with these values (by the plugin's own keys), or nullptr when the plugin
// isn't switched on or can't run: the filter then passes sound through unchanged.
using ExternalFactory = std::unique_ptr<External> (*)(const std::string& plugin, const std::map<std::string, float>& values);
void setExternalFactory(ExternalFactory f);

class Processor {
public:
    explicit Processor(const Chain& chain);
    ~Processor();
    // Interleaved stereo at 48 kHz, in place.
    void process(float* frames, std::size_t count);
    bool empty() const { return stages_.empty(); }
    int latencyFrames() const { return latency_; }   // what the chain adds (noise suppression, look-ahead, a delay, plugins); fixed when made
    // Gain reduction of the last block, in dB (0 none), for showing what a compressor or gate is doing.
    float reductionDb() const { return reduction_; }
    // Plugins in the chain that aren't running (switched off, missing or refused), by file name.
    const std::vector<std::string>& notRunning() const { return notRunning_; }

private:
    std::vector<std::unique_ptr<Stage>> stages_;
    float reduction_ = 0;
    int latency_ = 0;
    std::vector<std::string> notRunning_;
};

// Helpers the tests use too.
float dbToLinear(float db);
float linearToDb(float lin);   // -120 for silence

// Auto-duck: one source (music) made quieter while another (the microphone) has sound. Each 10 ms
// block the mixer gives it the trigger's loudest sample; it gives back the gain for the block's start
// and end, so the change is a smooth ramp (no clicks). Above the threshold it goes down to `amountDb`
// over `attackMs`; after the trigger has been quiet for `holdMs` it comes back up over `releaseMs`.
struct DuckSettings {
    float amountDb = -12;      // how much quieter (negative)
    float thresholdDb = -38;   // the trigger counts as "talking" above this
    int attackMs = 80, holdMs = 400, releaseMs = 700;
    bool operator==(const DuckSettings&) const = default;
};
class Ducker {
public:
    // `triggerPeak` linear (0..1+); `blockMs` the block's length. Returns {gain at start, gain at end}.
    std::pair<float, float> block(float triggerPeak, float blockMs, const DuckSettings& s);
    float gainDb() const { return linearToDb(gain_); }
    bool ducking() const { return target_ < 0.999f; }

private:
    float gain_ = 1, target_ = 1;
    float quietMs_ = 1e9f;   // how long the trigger has been quiet
};

}  // namespace ncap::dsp

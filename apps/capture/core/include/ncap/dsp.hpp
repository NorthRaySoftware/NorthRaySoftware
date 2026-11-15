// NorthRay Capture: per-source audio filters (system 7, "Audio Processing"). Portable and tested.
//
// Each sound source has a chain of filters, applied in order to its 48 kHz stereo sound before its
// volume, routing and meters. The chain is kept as JSON in the source's settings; the mixer runs a
// Processor built from it (stateful: envelopes, delay lines), rebuilt when the chain changes.
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ncap::dsp {

enum class Type { Gain, NoiseGate, NoiseSuppression, Expander, Compressor, Limiter, Equaliser, Balance, Delay };
const char* key(Type t);                       // "gain", "noise-gate"...
const char* label(Type t);                     // "Noise gate"
bool parse(const std::string& key, Type& out);
constexpr int kTypes = 9;

// One adjustable value of a filter: its key, its words, its range and default, and its unit.
struct Param {
    const char* key;
    const char* label;
    float min, max, def;
    const char* unit;   // "dB", "ms", ":1", "Hz", "%", ""
};
const std::vector<Param>& params(Type t);

struct Filter {
    Type type = Type::Gain;
    bool enabled = true;
    std::map<std::string, float> values;   // missing ones take the default
    float get(const std::string& k) const;
    static Filter make(Type t);            // with every default
};

using Chain = std::vector<Filter>;
std::string toJson(const Chain& c);        // a JSON array
Chain fromJson(const std::string& json);   // unknown filters and values dropped, values clamped
std::string describe(const Filter& f);     // "Compressor: -18 dB, 3:1"

class Stage;   // one filter's running state

class Processor {
public:
    explicit Processor(const Chain& chain);
    ~Processor();
    // Interleaved stereo at 48 kHz, in place.
    void process(float* frames, std::size_t count);
    bool empty() const { return stages_.empty(); }
    int latencyFrames() const;   // what the chain adds (noise suppression, the limiter's look-ahead, a delay)
    // Gain reduction of the last block, in dB (0 none), for showing what a compressor or gate is doing.
    float reductionDb() const { return reduction_; }

private:
    std::vector<std::unique_ptr<Stage>> stages_;
    float reduction_ = 0;
};

// Helpers the tests use too.
float dbToLinear(float db);
float linearToDb(float lin);   // -120 for silence

}  // namespace ncap::dsp

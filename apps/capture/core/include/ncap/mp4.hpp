// NorthRay Capture: an MP4 copy of a recording (CJ: "automatic MP4 after recording, keep the MKV").
// Portable and unit-tested.
//
// Recordings are written as MKV because a crash or power cut can't spoil them. Afterwards this makes
// an MP4 beside it without re-encoding: it reads the MKV (one H.264 video track, AAC audio tracks; a
// file whose recording was cut short works too) and copies every frame into an MP4 with the index at
// the front ("fast start": it plays while it downloads), each audio track named, the first one
// playing by default and the rest as alternatives, and the tracks kept in step with edit lists.
// Markers become chapters (Nero-style "chpl", which players and editors read).
//
// Reading is in two passes over the file (find the frames, then copy them), so files of any size
// work without loading them into memory.
#pragma once

#include "ncap/mkv.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ncap::mp4 {

using media::Bytes;

// Where the MKV is read from: a file on Windows, memory in the tests.
class Input {
public:
    virtual ~Input() = default;
    virtual std::uint64_t size() const = 0;
    virtual bool read(std::uint64_t offset, std::uint8_t* out, std::size_t n) = 0;
};

class MemoryInput : public Input {
public:
    explicit MemoryInput(const Bytes& b) : b_(b) {}
    std::uint64_t size() const override { return b_.size(); }
    bool read(std::uint64_t offset, std::uint8_t* out, std::size_t n) override;

private:
    const Bytes& b_;
};

struct Chapter {
    std::int64_t us = 0;
    std::string title;
};

// What the MKV holds (the first pass).
struct Track {
    int number = 0;        // Matroska's track number
    bool video = false;
    std::string codec;     // V_MPEG4/ISO/AVC, A_AAC
    std::string name;
    Bytes priv;            // avcC / AudioSpecificConfig
    int width = 0, height = 0, rate = 48000, channels = 2;
};
struct Frame {
    int track = 0;         // index into tracks
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
    std::int64_t us = 0;
    bool key = false;
};
struct Contents {
    std::vector<Track> tracks;
    std::vector<Frame> frames;   // in file order
    bool cutShort = false;       // the recording ended without its index (a crash): still fine
};
bool scan(Input& in, Contents& out, std::string& error);

struct Result {
    bool ok = false;
    std::string error;           // in words, for the person
    std::size_t videoFrames = 0, audioTracks = 0;
    std::int64_t durationUs = 0;
};
// Writes the MP4. `progress` (0..1) is called now and then; returning false stops (cancelled).
Result remux(Input& in, mkv::Sink& out, const std::vector<Chapter>& chapters = {},
             const std::function<bool(double)>& progress = {});

// The MP4's name beside the MKV: "x.mkv" -> "x.mp4".
std::string mp4NameFor(const std::string& mkvPath);

}  // namespace ncap::mp4

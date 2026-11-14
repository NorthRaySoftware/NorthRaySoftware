// NorthRay Capture: the Matroska (MKV) writer for recordings (system 9). Portable and unit-tested.
//
// Written so that a recording survives a crash or a power cut: the segment and each cluster start
// with "size unknown", which Matroska allows, so everything written so far plays. When the
// recording ends normally, the sizes, the duration and an index of keyframes (cues, for fast
// seeking) are filled in.
//
// One H.264 video track and up to six AAC audio tracks (the recording tracks), each with a name.
#pragma once

#include "ncap/media.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ncap::mkv {

using media::Bytes;

// Where the bytes go: a file on Windows, memory in the tests.
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool write(const std::uint8_t* data, std::size_t size) = 0;
    virtual bool writeAt(std::uint64_t offset, const std::uint8_t* data, std::size_t size) = 0;   // patching earlier bytes
    virtual std::uint64_t position() const = 0;
};

class MemorySink : public Sink {
public:
    bool write(const std::uint8_t* d, std::size_t n) override;
    bool writeAt(std::uint64_t at, const std::uint8_t* d, std::size_t n) override;
    std::uint64_t position() const override { return bytes.size(); }
    Bytes bytes;
};

struct VideoTrack {
    int width = 0, height = 0;
    double fps = 60;
    Bytes avcC;   // from media::avcC
};

struct AudioTrack {
    std::string name;   // "Track 1", "Microphone"
    int rate = 48000, channels = 2;
    Bytes config;       // media::aacConfig
};

class Writer {
public:
    // Writes the header: the EBML head, the segment (size unknown), the seek head, the info and the
    // tracks. Video is track number 1, audio tracks 2, 3...
    bool begin(Sink* sink, const VideoTrack& video, const std::vector<AudioTrack>& audio, const std::string& app);
    // `data`: length-prefixed NAL units (video) or one raw AAC frame (audio). Times in microseconds,
    // never earlier than a frame already written on the same track.
    bool video(std::int64_t ptsUs, bool keyframe, const Bytes& data);
    bool audio(int track, std::int64_t ptsUs, const Bytes& data);   // track 0..5
    // The cues, the duration and the sizes. The file is complete after this.
    bool finish();
    std::uint64_t bytesWritten() const { return sink_ ? sink_->position() : 0; }
    std::int64_t durationUs() const { return lastUs_; }

private:
    bool block(int trackNumber, std::int64_t us, bool keyframe, const Bytes& data);
    bool openCluster(std::int64_t ms);
    bool closeCluster();

    Sink* sink_ = nullptr;
    std::uint64_t segmentData_ = 0;   // where the segment's contents start (positions are relative to it)
    std::uint64_t segmentSize_ = 0;   // the segment's size field
    std::uint64_t durationAt_ = 0;    // the duration's float
    std::uint64_t cuesSeekAt_ = 0;    // the seek head's position for the cues
    std::uint64_t clusterStart_ = 0, clusterSize_ = 0;
    bool inCluster_ = false;
    std::int64_t clusterMs_ = 0, lastUs_ = 0;
    struct Cue { std::int64_t ms; std::uint64_t cluster; };
    std::vector<Cue> cues_;
    bool ok_ = true;
};

}  // namespace ncap::mkv

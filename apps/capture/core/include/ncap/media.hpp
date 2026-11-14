// NorthRay Capture: encoded media as bytes (part of systems 7 and 9). Portable and unit-tested.
//
//   H.264: encoders hand out Annex B (start codes); Matroska and MP4 want each NAL unit with a
//          length in front, plus an avcC record holding the stream's SPS and PPS.
//   AAC:   the AudioSpecificConfig that tells a player the profile, rate and channels.
//   Interleaving: packets from the video encoder and each audio track arrive at different times;
//          a file wants them in time order, so they wait here until it's safe to write each.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace ncap::media {

using Bytes = std::vector<std::uint8_t>;

// ---------------------------------------------------------------- H.264

// The NAL units in an Annex B stream (each without its start code).
std::vector<std::pair<const std::uint8_t*, std::size_t>> nalUnits(const std::uint8_t* data, std::size_t size);
inline int nalType(const std::uint8_t* nal) { return nal[0] & 0x1F; }
// Annex B to length-prefixed (4-byte big-endian lengths). SPS, PPS and access unit delimiters are
// left out when `dropParameterSets` (they live in the avcC record instead).
Bytes annexBToLengthPrefixed(const std::uint8_t* data, std::size_t size, bool dropParameterSets);
bool containsKeyframe(const std::uint8_t* annexB, std::size_t size);   // an IDR slice
// The first SPS and PPS in an Annex B buffer (empty if missing).
bool findParameterSets(const std::uint8_t* annexB, std::size_t size, Bytes& sps, Bytes& pps);
// The avcC (AVCDecoderConfigurationRecord) for Matroska's CodecPrivate and MP4's avcC box.
Bytes avcC(const Bytes& sps, const Bytes& pps);

// ---------------------------------------------------------------- AAC

// AAC-LC AudioSpecificConfig (2 bytes) for a rate and channel count; empty if the rate isn't one
// AAC has an index for.
Bytes aacConfig(int rate, int channels);

// ---------------------------------------------------------------- interleaving

struct Packet {
    int stream = 0;                 // 0 = video, 1.. = audio tracks
    std::int64_t pts = 0;           // microseconds from the start of the recording
    std::int64_t dts = 0;           // decode order (video with B-frames; equal to pts otherwise)
    bool keyframe = false;
    Bytes data;
};

// Releases packets in decode-time order. A packet is released once every stream that's still
// sending has something later queued (or `maxWaitUs` has passed for a stream that's gone quiet),
// so a file is never written out of order and nothing waits forever.
class Interleaver {
public:
    explicit Interleaver(int streams, std::int64_t maxWaitUs = 1000000);
    void push(Packet p);
    // The next packet that can be written now, if any.
    bool pop(Packet& out);
    // At the end: everything left, in order.
    bool drain(Packet& out);
    std::size_t queued() const;

private:
    std::vector<std::deque<Packet>> q_;
    std::vector<std::int64_t> last_;   // the newest dts seen per stream
    std::int64_t maxWait_;
};

// File names: "2026-11-14 21-55-03" style stamps, and a name that doesn't exist yet among `taken`.
std::string recordingName(int year, int month, int day, int hour, int minute, int second);

}  // namespace ncap::media

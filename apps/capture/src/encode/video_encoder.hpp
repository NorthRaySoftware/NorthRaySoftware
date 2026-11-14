// NorthRay Capture: the H.264 video encoder (system 7), through a Media Foundation transform:
// a hardware encoder where the PC has one (NVIDIA NVENC, AMD AMF, Intel Quick Sync), otherwise
// Microsoft's software encoder. Only encoders Windows actually offers are used.
//
// The canvas (BGRA) is turned into NV12 on the graphics card by the D3D11 video processor, then:
//   hardware  the NV12 texture goes straight to the encoder (no copy through the CPU);
//   software  it's copied to memory first (the encoder runs on the CPU anyway).
// The encoder runs on its own thread; the video thread only converts and hands over, so a slow
// encoder never holds up composition. Frames the encoder can't keep up with are dropped and counted.
#pragma once

#include "video/gpu.hpp"

#include "ncap/media.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ncap::encode {

struct VideoSettings {
    int width = 1920, height = 1080, fps = 60;
    int bitrateKbps = 12000;
    bool constantBitrate = false;   // CBR for streaming; VBR (quality) for recording
    int keyframeSeconds = 2;
    std::wstring encoder;           // the encoder's name as Windows lists it; "" = the best available
};

class VideoEncoder {
public:
    ~VideoEncoder() { close(); }
    // Video thread (the device's). False with a reason in plain words.
    bool open(video::Gpu& gpu, const VideoSettings& s, std::wstring& error);
    // Video thread: `canvas` is the composed frame (BGRA); `ptsUs` from the recording's start.
    void submit(ID3D11Texture2D* canvas, std::int64_t ptsUs);
    // Any thread: the next encoded packet (Annex B, as the encoder made it).
    bool take(media::Packet& out);
    // Stops taking frames, encodes what's queued and the encoder's last frames; `take` still works.
    void finish();
    void close();   // finish, and free everything

    const std::wstring& name() const { return name_; }
    bool hardware() const { return hardware_; }
    std::uint64_t dropped() const { return dropped_.load(); }
    std::wstring error() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::wstring name_;
    bool hardware_ = false;
    std::atomic<std::uint64_t> dropped_{0};
};

// The H.264 encoders on this PC, best first (hardware before software).
struct EncoderInfo {
    std::wstring name;
    bool hardware = false;
};
std::vector<EncoderInfo> listH264Encoders();

}  // namespace ncap::encode

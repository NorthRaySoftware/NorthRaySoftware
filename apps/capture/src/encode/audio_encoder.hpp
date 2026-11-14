// NorthRay Capture: the AAC audio encoder (system 7), Windows' own AAC encoder transform. One per
// recording track. Takes 48 kHz stereo float, gives raw AAC-LC frames (1024 samples each).
#pragma once

#include "ncap/media.hpp"

#include <windows.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <deque>
#include <string>
#include <vector>

namespace ncap::encode {

class AudioEncoder {
public:
    bool open(int bitrateKbps, std::wstring& error);   // 96, 128, 160 or 192
    // Samples from `ptsUs` on (interleaved stereo float). Calls must be continuous.
    bool encode(const float* frames, std::size_t count, std::int64_t ptsUs);
    bool finish();                       // the encoder's last frames
    bool take(media::Packet& out, int stream);
    media::Bytes config() const;         // the AudioSpecificConfig

private:
    bool pull(int stream);
    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    std::deque<media::Packet> out_;
    std::vector<std::int16_t> pcm_;
    std::int64_t samplesIn_ = 0, firstUs_ = 0;
    bool started_ = false;
    DWORD bufferSize_ = 0;
    media::Bytes config_;
};

}  // namespace ncap::encode

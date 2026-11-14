// NorthRay Capture: streaming to one destination over RTMP or RTMPS (system 8).
//
// The canvas goes to an H.264 encoder at a constant bitrate (what platforms ask for), the Stream
// mix to AAC, and both to the server as they're made. If the connection drops, the stream keeps
// encoding and reconnects (after 2, 4, 8... up to 60 seconds) without being asked; a wrong stream
// key isn't retried. If the network can't keep up, whole groups of frames are dropped (from the
// oldest keyframe on) rather than the stream falling ever further behind, and counted.
#pragma once

#include "audio/mixer.hpp"
#include "encode/audio_encoder.hpp"
#include "encode/video_encoder.hpp"
#include "video/engine.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ncap::output {

struct StreamSettings {
    std::string name;     // "Twitch"
    std::string url;      // rtmp(s)://host[:port]/app
    std::string key;      // never logged
    encode::VideoSettings video;
    int audioKbps = 160;
};

struct StreamStatus {
    enum State { Off, Connecting, Live, Reconnecting, Failed } state = Off;
    std::wstring message;          // in plain words: why it's reconnecting, why it failed
    double seconds = 0;            // live time (this connection and earlier ones)
    double kbps = 0;               // sent in the last second
    std::uint64_t droppedFrames = 0;
    int attempt = 0;               // reconnection attempts so far
    int retryIn = 0;               // seconds until the next attempt
    std::wstring encoder;
    bool hardware = false;
};

class Streamer {
public:
    ~Streamer() { stop(); }
    bool start(video::Engine& engine, audio::Mixer& mixer, const StreamSettings& s, std::wstring& error);
    void stop();
    bool active() const { return running_.load(); }
    StreamStatus status() const;

private:
    void run();
    void frame(ID3D11Texture2D* canvas, LONGLONG qpc);
    void setState(StreamStatus::State s, const std::wstring& message);

    video::Engine* engine_ = nullptr;
    audio::Mixer* mixer_ = nullptr;
    StreamSettings s_;
    std::unique_ptr<encode::VideoEncoder> video_;
    std::unique_ptr<encode::AudioEncoder> audio_;
    std::shared_ptr<audio::Tap> tap_;
    std::thread thread_;
    std::atomic<bool> running_{false}, stopping_{false};
    LONGLONG t0_ = 0, freq_ = 1;
    std::int64_t lastFrame_ = -1;
    std::mutex frameM_;
    mutable std::mutex m_;
    StreamStatus st_;
};

}  // namespace ncap::output

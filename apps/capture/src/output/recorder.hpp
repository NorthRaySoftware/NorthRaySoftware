// NorthRay Capture: recording to a file (system 9). The canvas goes to the H.264 encoder at the
// output frame rate, each chosen recording track (a mixer bus) to its own AAC encoder, and the
// packets, put in time order, into a Matroska file that stays playable if the PC crashes.
//
// Timing: picture and sound are both timed against the same clock (QueryPerformanceCounter) from
// the moment recording starts. Frames sit on a constant grid at the output rate (a frame the PC
// couldn't make in time is counted as dropped, not stretched); sound is timed from its first block
// by counting samples, so it can't drift against the file's own timeline.
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
#include <vector>

namespace ncap::output {

struct RecordSettings {
    std::wstring folder;                 // created if missing
    encode::VideoSettings video;
    std::vector<int> tracks = {1};       // recording tracks 1-6, in the file's order
    std::vector<std::string> trackNames; // shown by editors and players ("Track 1 (everything)")
    int audioKbps = 160;
    std::string profile = "Default";   // names the file: "<profile> 2026-11-15 14-00-05"
};

struct RecordStatus {
    bool active = false;
    std::wstring path;
    double seconds = 0;
    std::uint64_t bytes = 0;
    std::uint64_t droppedFrames = 0;     // composition was late (the PC was busy)
    std::uint64_t encoderDropped = 0;    // the encoder couldn't keep up
    std::uint64_t frames = 0;
    std::wstring encoder;
    bool hardware = false;
    std::wstring error;                  // why it stopped early, or a warning ("disk nearly full")
    std::uint64_t freeBytes = 0;         // on the recording's drive
};

class Recorder {
public:
    ~Recorder() { stop(); }
    bool start(video::Engine& engine, audio::Mixer& mixer, const RecordSettings& s, std::wstring& error);
    void stop();   // finishes the file (waits for the encoders to finish)
    RecordStatus status() const;
    bool active() const { return running_.load(); }
    // The file of a recording that has just finished (once), or "".
    std::wstring takeFinished(bool* ok = nullptr, std::wstring* problem = nullptr);

private:
    void run();
    void frame(ID3D11Texture2D* canvas, LONGLONG qpc);

    video::Engine* engine_ = nullptr;
    audio::Mixer* mixer_ = nullptr;
    RecordSettings s_;
    std::unique_ptr<encode::VideoEncoder> video_;
    std::vector<std::unique_ptr<encode::AudioEncoder>> audio_;
    std::vector<std::shared_ptr<audio::Tap>> taps_;
    std::thread thread_;
    std::atomic<bool> running_{false}, stopping_{false};
    LONGLONG t0_ = 0, freq_ = 1;
    std::int64_t lastFrame_ = -1;
    std::mutex frameM_;
    mutable std::mutex m_;
    RecordStatus st_;
    std::wstring finished_, finishedProblem_;
    bool finishedOk_ = false;
};

}  // namespace ncap::output

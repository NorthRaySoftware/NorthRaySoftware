// Plays an MP3 stream (the live Icecast mount or a recording) over HTTPS.
//
//   network thread -> compressed bytes -> decoder thread -> PCM ring buffer
//   -> audio device callback (never blocks) -> speakers
//
// Live streams reconnect automatically if the connection drops. Recordings
// end with State::Finished.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

namespace cjr {

class StreamPlayer {
public:
    enum class State { Stopped, Connecting, Buffering, Playing, Reconnecting, Finished, Error };

    StreamPlayer();
    ~StreamPlayer();

    // Posts `message` to `notify` whenever the state changes.
    void setNotify(HWND hwnd, UINT message) { notify_ = hwnd; message_ = message; }

    void play(const std::string& url, bool live);
    void stop();
    // Pausing fades out over a few milliseconds and holds the position;
    // resuming fades back in. (Live streams are resumed at the live point by
    // the app, not here.)
    void setPaused(bool p) { paused_ = p; if (notify_) PostMessageW(notify_, message_, 0, 0); }
    bool paused() const { return paused_.load(); }

    void setVolume(float v);      // 0..1, applied smoothly
    float volume() const { return volume_.load(); }
    void setMuted(bool m) { muted_ = m; }
    bool muted() const { return muted_.load(); }

    State state() const { return state_.load(); }
    std::wstring error() const;
    bool live() const { return live_; }
    std::string url() const;
    float level() const { return level_.load(); }  // recent peak, 0..1, for the meter
    double secondsPlayed() const;

    // Audio-thread callback. Not for general use.
    void fill(float* out, unsigned frameCount);

private:
    struct Impl;
    void setState(State s, const std::wstring& err = {});
    void networkLoop(std::string url, bool live, unsigned generation);
    void fileLoop(const std::string& path);
    void decodeLoop(unsigned generation);
    void shutdownThreads();

    Impl* impl_;
    HWND notify_ = nullptr;
    UINT message_ = 0;

    std::atomic<State> state_{State::Stopped};
    std::atomic<float> volume_{0.8f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> paused_{false};
    std::atomic<float> level_{0};
    std::atomic<bool> cancel_{false};
    std::atomic<unsigned> generation_{0};
    bool live_ = false;
    std::string url_;
    std::wstring error_;
    mutable std::mutex stateMutex_;

    // Compressed bytes from the network.
    std::mutex bytesMutex_;
    std::condition_variable bytesCv_;
    std::deque<unsigned char> bytes_;
    bool netDone_ = false;

    std::thread net_, decode_;
};

}  // namespace cjr

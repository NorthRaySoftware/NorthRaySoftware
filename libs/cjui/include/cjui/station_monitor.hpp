// Polls the CJ Radio server (AzuraCast now-playing API) on a background thread
// and tells a window when the answer changes.
#pragma once

#include "cj/station.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace cjui {

class StationMonitor {
public:
    ~StationMonitor() { stop(); }

    // Posts `message` to `notify` whenever a poll completes.
    void start(const cj::station::Config& config, HWND notify, UINT message);
    void stop();
    void pollNow();

    cj::station::NowPlaying latest() const;
    bool hasResult() const { return polled_.load(); }

private:
    void run();

    cj::station::Config config_;
    HWND notify_ = nullptr;
    UINT message_ = 0;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false, pollRequested_ = false;
    cj::station::NowPlaying latest_;
    std::atomic<bool> polled_{false};
};

}  // namespace cjui

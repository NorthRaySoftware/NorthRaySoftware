#include "cjui/station_monitor.hpp"

#include "cjui/http.hpp"

#include <chrono>

namespace cjui {

void StationMonitor::start(const cj::station::Config& config, HWND notify, UINT message) {
    stop();
    config_ = config;
    notify_ = notify;
    message_ = message;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = false;
        latest_ = {};
    }
    polled_ = false;
    if (!config_.serverConfigured()) return;  // nothing to poll; status stays "not set up"
    thread_ = std::thread([this] { run(); });
}

void StationMonitor::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void StationMonitor::pollNow() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pollRequested_ = true;
    }
    wake_.notify_all();
}

cj::station::NowPlaying StationMonitor::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void StationMonitor::run() {
    for (;;) {
        HttpResult r = httpGet(config_.nowPlayingUrl, 8000);
        cj::station::NowPlaying np;
        if (r.ok()) {
            np = cj::station::parseAzuraCastNowPlaying(r.body);
        } else {
            np.error = std::string(r.error.begin(), r.error.end());
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = np;
        }
        polled_ = true;
        if (notify_) PostMessageW(notify_, message_, 0, 0);

        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::seconds(config_.pollSeconds), [this] { return stop_ || pollRequested_; });
        if (stop_) return;
        pollRequested_ = false;
    }
}

}  // namespace cjui

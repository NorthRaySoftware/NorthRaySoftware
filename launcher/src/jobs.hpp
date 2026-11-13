// Background installs and updates: download -> check size and SHA-256 ->
// run the app's own setup silently into its folder. One job runs at a time;
// the rest wait in a queue. Progress is posted to the window as messages.
#pragma once

#include "model.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace nrl {

struct JobState {
    enum Stage { Queued, Downloading, Verifying, Installing, Done, Failed, Cancelled } stage = Queued;
    unsigned long long done = 0, total = 0;
    std::wstring message;    // failure reason, shown to the user
    bool update = false;
};

struct JobRequest {
    std::string appId;           // "launcher" for NorthRay Launcher itself
    Release release;
    std::wstring installDir;     // empty for the launcher: its own installer decides
    bool update = false;
};

class Jobs {
public:
    ~Jobs();
    void start(HWND notify, UINT message, const std::wstring& downloadDir);
    void stop();
    void enqueue(const JobRequest& r);
    void cancel(const std::string& appId);
    bool busy(const std::string& appId) const;
    bool anyBusy() const;
    JobState state(const std::string& appId) const;
    void forget(const std::string& appId);  // clear a finished/failed state

private:
    void run();
    void setState(const std::string& id, const JobState& s, bool notify = true);
    bool download(const JobRequest& r, const std::wstring& path, std::wstring& err);
    bool install(const JobRequest& r, const std::wstring& path, std::wstring& err);

    HWND notify_ = nullptr;
    UINT message_ = 0;
    std::wstring downloadDir_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<JobRequest> queue_;
    std::map<std::string, JobState> states_;
    std::string current_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> cancelCurrent_{false};
    DWORD lastPost_ = 0;
};

}  // namespace nrl

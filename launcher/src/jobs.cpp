#include "jobs.hpp"

#include "cjui/gfx.hpp"
#include "cjui/http.hpp"
#include "cjui/packages.hpp"

#include <cstdio>
#include <map>

namespace nrl {

Jobs::~Jobs() { stop(); }

void Jobs::start(HWND notify, UINT message, const std::wstring& downloadDir) {
    notify_ = notify;
    message_ = message;
    downloadDir_ = downloadDir;
    CreateDirectoryW(downloadDir_.c_str(), nullptr);
    quit_ = false;
    worker_ = std::thread([this] { run(); });
}

void Jobs::stop() {
    quit_ = true;
    cancelCurrent_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Jobs::enqueue(const JobRequest& r) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& q : queue_) if (q.appId == r.appId) return;
        if (current_ == r.appId) return;
        queue_.push_back(r);
        JobState s;
        s.stage = JobState::Queued;
        s.total = r.release.size;
        s.update = r.update;
        states_[r.appId] = s;
    }
    cv_.notify_all();
    if (notify_) PostMessageW(notify_, message_, 0, 0);
}

void Jobs::cancel(const std::string& appId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = queue_.begin(); it != queue_.end(); ++it)
        if (it->appId == appId) { queue_.erase(it); states_.erase(appId); return; }
    // Only a download can be stopped; once its setup runs, it finishes.
    if (current_ == appId && states_[appId].stage == JobState::Downloading) cancelCurrent_ = true;
}

bool Jobs::busy(const std::string& appId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(appId);
    return it != states_.end() && (it->second.stage == JobState::Queued || it->second.stage == JobState::Downloading ||
                                   it->second.stage == JobState::Verifying || it->second.stage == JobState::Installing);
}

bool Jobs::anyBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !current_.empty() || !queue_.empty();
}

JobState Jobs::state(const std::string& appId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(appId);
    return it == states_.end() ? JobState{JobState::Done, 0, 0, L"", false} : it->second;
}

void Jobs::forget(const std::string& appId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(appId);
    if (it != states_.end() && (it->second.stage == JobState::Done || it->second.stage == JobState::Failed ||
                                it->second.stage == JobState::Cancelled))
        states_.erase(it);
}

void Jobs::setState(const std::string& id, const JobState& s, bool notify) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        states_[id] = s;
    }
    if (notify && notify_) PostMessageW(notify_, message_, 0, 0);
}

void Jobs::run() {
    while (!quit_) {
        JobRequest r;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            r = queue_.front();
            queue_.pop_front();
            current_ = r.appId;
        }
        cancelCurrent_ = false;
        const std::wstring path = downloadDir_ + L"\\" + cjui::widen(r.release.file);
        std::wstring err;
        JobState s;
        s.update = r.update;
        s.total = r.release.size;
        bool ok = download(r, path, err);
        if (ok) {
            s.stage = JobState::Verifying;
            s.done = s.total;
            setState(r.appId, s);
            // The file must be exactly what the catalogue describes before it's run.
            const std::string hash = cjui::sha256File(path);
            if (hash != r.release.sha256) {
                ok = false;
                err = L"The download didn't match its published checksum, so it wasn't installed. Try again later.";
            }
        }
        if (ok) {
            s.stage = JobState::Installing;
            setState(r.appId, s);
            ok = install(r, path, err);
        }
        DeleteFileW(path.c_str());
        DeleteFileW((path + L".part").c_str());
        s.stage = ok ? JobState::Done : cancelCurrent_ ? JobState::Cancelled : JobState::Failed;
        s.message = ok ? L"" : err;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_.clear();
        }
        setState(r.appId, s);
    }
}

bool Jobs::download(const JobRequest& r, const std::wstring& path, std::wstring& err) {
    const std::wstring part = path + L".part";
    FILE* f = _wfopen(part.c_str(), L"wb");
    if (!f) { err = L"Couldn't save the download (is the disk full?)."; return false; }
    JobState s;
    s.stage = JobState::Downloading;
    s.total = r.release.size;
    s.update = r.update;
    setState(r.appId, s);
    unsigned long long got = 0;
    bool tooBig = false, writeFailed = false;
    std::string contentType;
    const int status = cjui::httpStream(
        r.release.url,
        [&](const uint8_t* data, size_t n) {
            if (got + n > r.release.size) { tooBig = true; return false; }  // never more than announced
            if (std::fwrite(data, 1, n, f) != n) { writeFailed = true; return false; }
            got += n;
            const DWORD t = GetTickCount();
            if (t - lastPost_ > 120) {
                lastPost_ = t;
                s.done = got;
                setState(r.appId, s);
            }
            return true;
        },
        cancelCurrent_, err, &contentType);
    std::fclose(f);
    if (cancelCurrent_) { err = L"Cancelled."; return false; }
    if (writeFailed) { err = L"Couldn't save the download (is the disk full?)."; return false; }
    if (tooBig) { err = L"The download was larger than the catalogue says, so it was stopped."; return false; }
    if (status != 200) {
        if (err.empty()) err = status == 404 ? L"The installer wasn't found on the server." : L"The download didn't complete.";
        return false;
    }
    if (got != r.release.size) { err = L"The download was incomplete. Check your connection and try again."; return false; }
    DeleteFileW(path.c_str());
    if (!MoveFileW(part.c_str(), path.c_str())) { err = L"Couldn't prepare the downloaded installer."; return false; }
    return true;
}

bool Jobs::install(const JobRequest& r, const std::wstring& path, std::wstring& err) {
    // Apps install silently into the chosen folder. NorthRay Launcher's own
    // setup is shown normally (it has to replace this program).
    std::wstring cmd = L"\"" + path + L"\"";
    if (r.appId != "launcher") cmd += L" /S /D=" + r.installDir;  // NSIS: /D must be last and unquoted
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(path.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        err = L"Windows couldn't run the installer (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    CloseHandle(pi.hThread);
    if (r.appId == "launcher") {
        CloseHandle(pi.hProcess);
        if (notify_) PostMessageW(notify_, WM_CLOSE, 0, 0);  // let the setup replace the launcher
        return true;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (wait != WAIT_OBJECT_0) { err = L"The installer didn't finish in time."; return false; }
    if (code != 0) {
        err = code == 2 ? L"The app is open, so its files couldn't be replaced. Close it and try again."
                        : L"The installer reported a problem (code " + std::to_wstring(code) + L").";
        return false;
    }
    return true;
}

}  // namespace nrl

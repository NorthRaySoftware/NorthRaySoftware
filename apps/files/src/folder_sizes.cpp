#include "folder_sizes.hpp"

#include "nrf/paths.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace nrf {

namespace {

struct Job {
    std::wstring key, path;
    std::uint64_t generation = 0;
};

// Calls `each` for every entry directly in `folder` (not . and ..); false if it couldn't be opened.
template <typename F>
bool eachEntry(const std::wstring& folder, F&& each) {
    std::wstring pattern = extendedPath(folder);
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        if (!each(fd)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

std::wstring join(const std::wstring& folder, const wchar_t* name) {
    std::wstring p = folder;
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    return p + name;
}

}  // namespace

struct FolderSizes::Shared {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::set<std::wstring> pending;
    std::uint64_t generation = 0;
    bool stop = false;
    HWND notify = nullptr;

    // Posts a result unless the window has moved on; false if it has (stop working on it).
    bool post(const Job& job, const FolderSize& size, bool last) {
        std::lock_guard<std::mutex> lock(m);
        if (last) pending.erase(job.key);
        if (stop || job.generation != generation) return false;
        auto* r = new FolderSizeResult{job.key, size};
        if (!PostMessageW(notify, WM_APP_FOLDERSIZE, 0, reinterpret_cast<LPARAM>(r))) {
            delete r;
            return false;
        }
        return true;
    }
    bool cancelled(const Job& job) {
        std::lock_guard<std::mutex> lock(m);
        return stop || job.generation != generation;
    }
};

FolderSizes::FolderSizes(HWND notify) : shared_(std::make_shared<Shared>()) {
    shared_->notify = notify;
    std::shared_ptr<Shared> s = shared_;
    std::thread([s] {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(s->m);
                s->cv.wait(lock, [&] { return s->stop || !s->jobs.empty(); });
                if (s->stop) break;
                job = std::move(s->jobs.back());   // the newest first: what's on screen now
                s->jobs.pop_back();
            }
            FolderSize fs;
            // Its own items first: quick, and shown at once.
            std::vector<std::wstring> folders;
            fs.failed = !eachEntry(job.path, [&](const WIN32_FIND_DATAW& fd) {
                ++fs.items;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) folders.push_back(join(job.path, fd.cFileName));
                } else {
                    fs.bytes += ((std::uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                }
                return true;
            });
            fs.counted = !fs.failed;
            if (fs.failed || folders.empty()) {
                fs.sized = !fs.failed;
                s->post(job, fs, true);
                continue;
            }
            if (!s->post(job, fs, false)) {
                std::lock_guard<std::mutex> lock(s->m);
                s->pending.erase(job.key);
                continue;
            }
            // Then everything inside, breadth first, never through links.
            const auto start = std::chrono::steady_clock::now();
            std::uint64_t seen = fs.items;
            bool stopped = false, gaveUp = false;
            while (!folders.empty() && !stopped) {
                const std::wstring dir = std::move(folders.back());
                folders.pop_back();
                eachEntry(dir, [&](const WIN32_FIND_DATAW& fd) {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) folders.push_back(join(dir, fd.cFileName));
                    } else {
                        fs.bytes += ((std::uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                    }
                    if (++seen % 2048 == 0) {
                        if (s->cancelled(job)) { stopped = gaveUp = true; return false; }
                        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                        if (seen >= kMaxEntries || secs >= kMaxSeconds) { stopped = true; return false; }
                    }
                    return true;
                });
            }
            fs.sized = true;
            fs.partial = stopped;
            if (gaveUp) {
                std::lock_guard<std::mutex> lock(s->m);
                s->pending.erase(job.key);
                continue;
            }
            s->post(job, fs, true);
        }
    }).detach();   // never holds up closing the window
}

FolderSizes::~FolderSizes() {
    std::lock_guard<std::mutex> lock(shared_->m);
    shared_->stop = true;
    shared_->jobs.clear();
    shared_->cv.notify_all();
}

const FolderSize* FolderSizes::get(const std::wstring& path, std::uint64_t modified) {
    const std::wstring key = path + L"|" + std::to_wstring(modified);
    auto it = kept_.find(key);
    if (it != kept_.end() && (it->second.sized || it->second.failed)) return &it->second;
    std::lock_guard<std::mutex> lock(shared_->m);
    if (!shared_->pending.count(key)) {
        shared_->pending.insert(key);
        shared_->jobs.push_back({key, path, shared_->generation});
        while (shared_->jobs.size() > 400) {
            shared_->pending.erase(shared_->jobs.front().key);
            shared_->jobs.pop_front();
        }
        shared_->cv.notify_one();
    }
    return it != kept_.end() ? &it->second : nullptr;
}

const FolderSize* FolderSizes::peek(const std::wstring& path, std::uint64_t modified) const {
    auto it = kept_.find(path + L"|" + std::to_wstring(modified));
    return it != kept_.end() ? &it->second : nullptr;
}

void FolderSizes::received(FolderSizeResult* r) {
    if (kept_.size() >= kMaxKept && !kept_.count(r->key)) kept_.clear();
    kept_[r->key] = r->size;
}

void FolderSizes::forgetQueue() {
    std::lock_guard<std::mutex> lock(shared_->m);
    ++shared_->generation;
    for (const Job& j : shared_->jobs) shared_->pending.erase(j.key);
    shared_->jobs.clear();
}

void FolderSizes::clear() {
    forgetQueue();
    kept_.clear();
}

}  // namespace nrf

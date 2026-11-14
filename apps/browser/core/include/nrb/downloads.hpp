// NorthRay Browser's downloads: the list on the Downloads page (northray://downloads, Ctrl+J),
// kept in a small SQLite database (%LOCALAPPDATA%\NorthRay\Browser\User\downloads.db) so finished
// downloads are still listed next time. The files themselves are wherever they were saved.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace nrb {

enum class DownloadState { Running = 0, Paused = 1, Done = 2, Cancelled = 3, Failed = 4 };

struct Download {
    std::int64_t id = 0;
    std::string url;            // where it came from
    std::string path;           // where it's saved (UTF-8)
    std::string mime;
    std::int64_t received = 0;
    std::int64_t total = -1;    // -1: the site didn't say
    DownloadState state = DownloadState::Running;
    std::string reason;         // why it failed, in words
    std::int64_t started = 0, finished = 0;   // Unix seconds
    std::int64_t estimatedEnd = 0;            // Unix seconds, 0 = unknown
    std::string name() const;   // the file's name, from its path
};

class DownloadStore {
public:
    static constexpr std::size_t kKeep = 200;   // the newest are kept in the list

    // Opens (creating) the list at `path` (":memory:" for tests). Downloads that were still running
    // when the browser last closed are marked as failed ("Stopped when the browser closed").
    static std::unique_ptr<DownloadStore> open(const std::string& path, std::string& error);
    ~DownloadStore();
    DownloadStore(const DownloadStore&) = delete;
    DownloadStore& operator=(const DownloadStore&) = delete;

    std::int64_t add(Download d);                 // returns the id
    // Updates it in memory; written to the file when its state changes (not on every byte).
    void update(const Download& d);
    const Download* find(std::int64_t id) const;
    const std::vector<Download>& all() const { return list_; }   // newest first
    bool remove(std::int64_t id);
    std::size_t clearFinished();                  // everything not running or paused
    int active() const;                           // running or paused
    // All running downloads together, 0..1; -1 when a size isn't known.
    double progress() const;

private:
    DownloadStore() = default;
    void write(const Download& d);
    sqlite3* db_ = nullptr;
    std::vector<Download> list_;
};

// "1.2 MB", "820 KB", "15 bytes".
std::string formatBytes(std::int64_t n);
// The line under a download's name: "4.2 MB of 12 MB, 3 seconds left", "Done, 12 MB", "Cancelled".
std::string downloadStatus(const Download& d, std::int64_t now);

}  // namespace nrb

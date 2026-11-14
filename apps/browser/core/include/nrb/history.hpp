// NorthRay Browser's history: the pages opened, one SQLite database per profile
// (%LOCALAPPDATA%\NorthRay\Browser\User\history.db), kept apart from the
// engine's browsing data. Kept for 90 days. Portable, so it's tested on its own.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace nrb {

struct Visit {
    std::int64_t id = 0;
    std::string url;
    std::string title;
    std::int64_t at = 0;    // Unix seconds
};

// Whether an address goes in the history: web pages and files, never NorthRay's
// own pages, about:, data:, blob: or javascript:.
bool historyUrl(const std::string& url);

class HistoryStore {
public:
    static constexpr int kSchemaVersion = 1;
    static constexpr std::int64_t kKeepSeconds = 90LL * 24 * 3600;

    // Opens (creating if needed) the database at `path` (UTF-8; ":memory:" for tests).
    // A damaged file is set aside as "<path>.damaged" and a new one started.
    static std::unique_ptr<HistoryStore> open(const std::string& path, std::string& error);
    ~HistoryStore();
    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;

    // A page was opened at `at` (0 = now). The same address again within 30
    // minutes (a reload, back and forth) moves its latest visit instead of adding one.
    // Returns the visit's id (0 if not kept).
    std::int64_t add(const std::string& url, const std::string& title, std::int64_t at = 0);
    // The page's title arrived (or changed) after it opened.
    void setTitle(std::int64_t visit, const std::string& title);
    // Newest first; `query` matches title or address (any case); only visits before `before` (0 = any).
    std::vector<Visit> list(const std::string& query = {}, std::int64_t before = 0, std::size_t limit = 500) const;
    std::size_t count() const;
    bool remove(std::int64_t visit);
    std::size_t removeUrl(const std::string& url);        // every visit to it
    std::size_t clearSince(std::int64_t since);           // visits at or after `since` (0 = everything)
    std::size_t prune(std::int64_t now = 0);              // older than kKeepSeconds

private:
    HistoryStore() = default;
    sqlite3* db_ = nullptr;
};

}  // namespace nrb

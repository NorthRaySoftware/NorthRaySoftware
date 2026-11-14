// NorthRay Browser's bookmarks: one SQLite database per profile
// (%LOCALAPPDATA%\NorthRay\Browser\User\bookmarks.db), kept apart from the
// engine's disposable browsing data, so clearing that never touches them.
//
// Two fixed folders: the Bookmarks bar (shown under the toolbar and on the
// start page) and Other bookmarks. Folders nest. Positions within a folder are
// kept 0..n-1. Portable (SQLite only), so it's tested on its own.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace nrb {

struct Bookmark {
    std::int64_t id = 0;
    std::int64_t parent = 0;       // 0 for the two fixed folders
    bool folder = false;
    std::string title;
    std::string url;               // empty for folders
    int position = 0;
    std::int64_t created = 0;      // Unix seconds
    int iconW = 0, iconH = 0;      // the site icon, RGBA, when one was seen
    std::vector<unsigned char> icon;
};

// Whether an address can be bookmarked: http, https, file and NorthRay's own
// pages. Never javascript: (a bookmark that runs code on whatever page is open).
bool bookmarkableUrl(const std::string& url);

class BookmarkStore {
public:
    static constexpr std::int64_t kBar = 1;     // Bookmarks bar
    static constexpr std::int64_t kOther = 2;   // Other bookmarks
    static constexpr int kSchemaVersion = 1;

    // Opens (creating if needed) the database at `path` (UTF-8; ":memory:" for
    // tests). A damaged file is set aside as "<path>.damaged" and a new one
    // started; `notice` then says so in plain words.
    static std::unique_ptr<BookmarkStore> open(const std::string& path, std::string& error, std::string* notice = nullptr);
    ~BookmarkStore();
    BookmarkStore(const BookmarkStore&) = delete;
    BookmarkStore& operator=(const BookmarkStore&) = delete;

    std::vector<Bookmark> children(std::int64_t folder) const;
    std::optional<Bookmark> get(std::int64_t id) const;
    std::optional<Bookmark> findUrl(const std::string& url) const;   // the first with exactly this address
    // Every bookmark (not folders) whose title or address contains `query` (any case), newest first.
    std::vector<Bookmark> search(const std::string& query, std::size_t limit = 200) const;
    std::size_t count() const;   // bookmarks, not folders

    // position -1 = at the end. Returns 0 on failure (lastError() says why).
    std::int64_t add(std::int64_t parent, const std::string& title, const std::string& url, int position = -1);
    std::int64_t addFolder(std::int64_t parent, const std::string& title, int position = -1);
    bool rename(std::int64_t id, const std::string& title);
    bool setUrl(std::int64_t id, const std::string& url);
    bool setIcon(std::int64_t id, int w, int h, const std::vector<unsigned char>& rgba);
    bool move(std::int64_t id, std::int64_t parent, int position);
    bool remove(std::int64_t id);   // a folder takes its contents with it; the fixed folders can't go
    bool isFolder(std::int64_t id) const;

    // The Netscape bookmark file that Chrome, Edge and Firefox export and import.
    std::string exportHtml() const;
    // Adds the file's bookmarks (and folders) into `parent`; returns how many bookmarks were added.
    int importHtml(const std::string& html, std::int64_t parent);

    // A copy of the database beside it ("<path>.backup"), at most once a day.
    bool backupIfDue(std::int64_t nowUnix);

    const std::string& lastError() const { return error_; }

private:
    BookmarkStore() = default;
    bool migrate();
    bool renumber(std::int64_t parent);
    bool insideFolder(std::int64_t id, std::int64_t folder) const;   // id is folder, or within it
    std::int64_t insert(std::int64_t parent, bool folder, const std::string& title, const std::string& url, int position,
                        std::int64_t created);
    sqlite3* db_ = nullptr;
    std::string path_;
    mutable std::string error_;
};

}  // namespace nrb

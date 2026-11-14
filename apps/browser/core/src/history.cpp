#include "nrb/history.hpp"

#include <sqlite3.h>

#include <cctype>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace nrb {

namespace {

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) { if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) s_ = nullptr; }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    explicit operator bool() const { return s_ != nullptr; }
    Stmt& bind(int i, std::int64_t v) { sqlite3_bind_int64(s_, i, v); return *this; }
    Stmt& bind(int i, const std::string& v) { sqlite3_bind_text(s_, i, v.data(), (int)v.size(), SQLITE_TRANSIENT); return *this; }
    bool step() { return s_ && sqlite3_step(s_) == SQLITE_ROW; }
    bool run() { return s_ && sqlite3_step(s_) == SQLITE_DONE; }
    std::int64_t i64(int c) const { return sqlite3_column_int64(s_, c); }
    std::string text(int c) const {
        const auto* t = sqlite3_column_text(s_, c);
        return t ? std::string(reinterpret_cast<const char*>(t), (size_t)sqlite3_column_bytes(s_, c)) : std::string();
    }

private:
    sqlite3_stmt* s_ = nullptr;
};

std::int64_t now() { return (std::int64_t)std::time(nullptr); }

std::string likePattern(const std::string& q) {
    std::string p = "%";
    for (char c : q) {
        if (c == '%' || c == '_' || c == '\\') p += '\\';
        p += (char)std::tolower((unsigned char)c);
    }
    return p + "%";
}

}  // namespace

bool historyUrl(const std::string& url) {
    auto starts = [&](const char* p) {
        const size_t n = std::char_traits<char>::length(p);
        if (url.size() < n) return false;
        for (size_t i = 0; i < n; ++i) if (std::tolower((unsigned char)url[i]) != p[i]) return false;
        return true;
    };
    return (starts("http://") || starts("https://") || starts("file:")) && url.size() < 8192;
}

std::unique_ptr<HistoryStore> HistoryStore::open(const std::string& path, std::string& error) {
    auto tryOpen = [&](std::unique_ptr<HistoryStore>& s) -> bool {
        s.reset(new HistoryStore());
        if (sqlite3_open_v2(path.c_str(), &s->db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            error = s->db_ ? sqlite3_errmsg(s->db_) : "couldn't open the history";
            return false;
        }
        sqlite3_busy_timeout(s->db_, 3000);
        {
            Stmt check(s->db_, "PRAGMA quick_check");
            if (!check || !check.step() || check.text(0) != "ok") { error = "damaged"; return false; }
        }
        if (path != ":memory:") sqlite3_exec(s->db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
        Stmt v(s->db_, "PRAGMA user_version");
        const int version = v.step() ? (int)v.i64(0) : 0;
        if (version > kSchemaVersion) { error = "The history was made by a newer NorthRay Browser."; return false; }
        if (version < 1) {
            char* msg = nullptr;
            if (sqlite3_exec(s->db_,
                             "CREATE TABLE IF NOT EXISTS visits(id INTEGER PRIMARY KEY, url TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', at INTEGER NOT NULL);"
                             "CREATE INDEX IF NOT EXISTS visits_at ON visits(at);"
                             "CREATE INDEX IF NOT EXISTS visits_url ON visits(url);"
                             "PRAGMA user_version=1;",
                             nullptr, nullptr, &msg) != SQLITE_OK) {
                error = msg ? msg : "database error";
                sqlite3_free(msg);
                return false;
            }
        }
        return true;
    };
    std::unique_ptr<HistoryStore> s;
    if (tryOpen(s)) { s->prune(); return s; }
    if (path == ":memory:" || error.find("newer") != std::string::npos) return nullptr;
    // Damaged: keep it aside and start again (history is a convenience, not a record).
    s.reset();
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(std::u8string(path.begin(), path.end()));
    fs::rename(p, fs::path(std::u8string(path.begin(), path.end()) + u8".damaged"), ec);
    fs::remove(fs::path(std::u8string(path.begin(), path.end()) + u8"-wal"), ec);
    fs::remove(fs::path(std::u8string(path.begin(), path.end()) + u8"-shm"), ec);
    if (tryOpen(s)) return s;
    return nullptr;
}

HistoryStore::~HistoryStore() {
    if (db_) sqlite3_close_v2(db_);
}

std::int64_t HistoryStore::add(const std::string& url, const std::string& title, std::int64_t at) {
    if (!historyUrl(url)) return 0;
    if (!at) at = now();
    {
        Stmt last(db_, "SELECT id, at FROM visits WHERE url = ? ORDER BY at DESC LIMIT 1");
        last.bind(1, url);
        if (last.step() && at - last.i64(1) < 30 * 60 && at >= last.i64(1)) {
            const std::int64_t id = last.i64(0);
            Stmt up(db_, title.empty() ? "UPDATE visits SET at = ? WHERE id = ?" : "UPDATE visits SET at = ?, title = ? WHERE id = ?");
            up.bind(1, at);
            if (title.empty()) up.bind(2, id); else up.bind(2, title).bind(3, id);
            up.run();
            return id;
        }
    }
    Stmt ins(db_, "INSERT INTO visits(url, title, at) VALUES(?, ?, ?)");
    ins.bind(1, url).bind(2, title).bind(3, at);
    return ins.run() ? sqlite3_last_insert_rowid(db_) : 0;
}

void HistoryStore::setTitle(std::int64_t visit, const std::string& title) {
    if (!visit || title.empty()) return;
    Stmt up(db_, "UPDATE visits SET title = ? WHERE id = ?");
    up.bind(1, title).bind(2, visit).run();
}

std::vector<Visit> HistoryStore::list(const std::string& query, std::int64_t before, std::size_t limit) const {
    std::vector<Visit> out;
    Stmt s(db_, "SELECT id, url, title, at FROM visits WHERE (? = '' OR lower(title) LIKE ? ESCAPE '\\' OR lower(url) LIKE ? ESCAPE '\\') "
                "AND (? = 0 OR at < ?) ORDER BY at DESC, id DESC LIMIT ?");
    const std::string pat = likePattern(query);
    s.bind(1, query).bind(2, pat).bind(3, pat).bind(4, before).bind(5, before).bind(6, (std::int64_t)limit);
    while (s.step()) out.push_back({s.i64(0), s.text(1), s.text(2), s.i64(3)});
    return out;
}

std::size_t HistoryStore::count() const {
    Stmt s(db_, "SELECT COUNT(*) FROM visits");
    return s.step() ? (std::size_t)s.i64(0) : 0;
}

bool HistoryStore::remove(std::int64_t visit) {
    Stmt s(db_, "DELETE FROM visits WHERE id = ?");
    return s.bind(1, visit).run() && sqlite3_changes(db_) > 0;
}

std::size_t HistoryStore::removeUrl(const std::string& url) {
    Stmt s(db_, "DELETE FROM visits WHERE url = ?");
    return s.bind(1, url).run() ? (std::size_t)sqlite3_changes(db_) : 0;
}

std::size_t HistoryStore::clearSince(std::int64_t since) {
    Stmt s(db_, "DELETE FROM visits WHERE at >= ?");
    const std::size_t n = s.bind(1, since).run() ? (std::size_t)sqlite3_changes(db_) : 0;
    if (since == 0) sqlite3_exec(db_, "VACUUM", nullptr, nullptr, nullptr);   // nothing left behind in the file
    return n;
}

std::size_t HistoryStore::prune(std::int64_t t) {
    if (!t) t = now();
    Stmt s(db_, "DELETE FROM visits WHERE at < ?");
    return s.bind(1, t - kKeepSeconds).run() ? (std::size_t)sqlite3_changes(db_) : 0;
}

}  // namespace nrb

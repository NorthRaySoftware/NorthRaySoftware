#include "nrb/downloads.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace nrb {

namespace {

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) { if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) s_ = nullptr; }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
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

bool finishedState(DownloadState s) { return s != DownloadState::Running && s != DownloadState::Paused; }

}  // namespace

std::string Download::name() const {
    const size_t slash = path.find_last_of("\\/");
    std::string n = slash == std::string::npos ? path : path.substr(slash + 1);
    if (n.empty()) {
        // No file yet: the last part of the address.
        std::string u = url.substr(0, url.find_first_of("?#"));
        const size_t s = u.find_last_of('/');
        n = s == std::string::npos ? u : u.substr(s + 1);
    }
    return n.empty() ? std::string("download") : n;
}

std::unique_ptr<DownloadStore> DownloadStore::open(const std::string& path, std::string& error) {
    std::unique_ptr<DownloadStore> s(new DownloadStore());
    if (sqlite3_open_v2(path.c_str(), &s->db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        error = s->db_ ? sqlite3_errmsg(s->db_) : "couldn't open the downloads list";
        return nullptr;
    }
    sqlite3_busy_timeout(s->db_, 3000);
    char* msg = nullptr;
    if (sqlite3_exec(s->db_,
                     "CREATE TABLE IF NOT EXISTS downloads(id INTEGER PRIMARY KEY, url TEXT NOT NULL, path TEXT NOT NULL, mime TEXT NOT NULL DEFAULT '',"
                     " received INTEGER NOT NULL DEFAULT 0, total INTEGER NOT NULL DEFAULT -1, state INTEGER NOT NULL, reason TEXT NOT NULL DEFAULT '',"
                     " started INTEGER NOT NULL, finished INTEGER NOT NULL DEFAULT 0);",
                     nullptr, nullptr, &msg) != SQLITE_OK) {
        error = msg ? msg : "couldn't set up the downloads list";
        sqlite3_free(msg);
        return nullptr;
    }
    // What was still going when the browser closed has stopped.
    sqlite3_exec(s->db_, "UPDATE downloads SET state=4, reason='Stopped when the browser closed' WHERE state IN (0,1);", nullptr, nullptr, nullptr);
    Stmt q(s->db_, "SELECT id,url,path,mime,received,total,state,reason,started,finished FROM downloads ORDER BY started DESC, id DESC");
    while (q.step()) {
        Download d;
        d.id = q.i64(0);
        d.url = q.text(1);
        d.path = q.text(2);
        d.mime = q.text(3);
        d.received = q.i64(4);
        d.total = q.i64(5);
        d.state = (DownloadState)std::clamp<std::int64_t>(q.i64(6), 0, 4);
        d.reason = q.text(7);
        d.started = q.i64(8);
        d.finished = q.i64(9);
        s->list_.push_back(std::move(d));
    }
    // Only the newest are kept.
    if (s->list_.size() > kKeep) {
        const std::int64_t cut = s->list_[kKeep - 1].id;
        Stmt del(s->db_, "DELETE FROM downloads WHERE id<?");
        del.bind(1, cut).run();
        s->list_.resize(kKeep);
    }
    return s;
}

DownloadStore::~DownloadStore() { sqlite3_close(db_); }

void DownloadStore::write(const Download& d) {
    Stmt u(db_, "UPDATE downloads SET url=?,path=?,mime=?,received=?,total=?,state=?,reason=?,finished=? WHERE id=?");
    u.bind(1, d.url).bind(2, d.path).bind(3, d.mime).bind(4, d.received).bind(5, d.total).bind(6, (std::int64_t)d.state).bind(7, d.reason)
        .bind(8, d.finished).bind(9, d.id).run();
}

std::int64_t DownloadStore::add(Download d) {
    if (!d.started) d.started = (std::int64_t)std::time(nullptr);
    Stmt ins(db_, "INSERT INTO downloads(url,path,mime,received,total,state,reason,started,finished) VALUES(?,?,?,?,?,?,?,?,?)");
    ins.bind(1, d.url).bind(2, d.path).bind(3, d.mime).bind(4, d.received).bind(5, d.total).bind(6, (std::int64_t)d.state).bind(7, d.reason)
        .bind(8, d.started).bind(9, d.finished);
    if (!ins.run()) return 0;
    d.id = sqlite3_last_insert_rowid(db_);
    list_.insert(list_.begin(), d);
    if (list_.size() > kKeep) {
        Stmt del(db_, "DELETE FROM downloads WHERE id=?");
        del.bind(1, list_.back().id).run();
        list_.pop_back();
    }
    return d.id;
}

void DownloadStore::update(const Download& d) {
    for (Download& x : list_)
        if (x.id == d.id) {
            const bool stateChanged = x.state != d.state || x.path != d.path;
            x = d;
            if (finishedState(d.state) && !x.finished) x.finished = (std::int64_t)std::time(nullptr);
            if (stateChanged) write(x);
            return;
        }
}

const Download* DownloadStore::find(std::int64_t id) const {
    for (const Download& d : list_) if (d.id == id) return &d;
    return nullptr;
}

bool DownloadStore::remove(std::int64_t id) {
    const auto it = std::find_if(list_.begin(), list_.end(), [&](const Download& d) { return d.id == id; });
    if (it == list_.end() || !finishedState(it->state)) return false;
    Stmt del(db_, "DELETE FROM downloads WHERE id=?");
    del.bind(1, id).run();
    list_.erase(it);
    return true;
}

std::size_t DownloadStore::clearFinished() {
    const std::size_t before = list_.size();
    list_.erase(std::remove_if(list_.begin(), list_.end(), [](const Download& d) { return finishedState(d.state); }), list_.end());
    sqlite3_exec(db_, "DELETE FROM downloads WHERE state NOT IN (0,1);", nullptr, nullptr, nullptr);
    return before - list_.size();
}

int DownloadStore::active() const {
    int n = 0;
    for (const Download& d : list_) if (!finishedState(d.state)) ++n;
    return n;
}

double DownloadStore::progress() const {
    std::int64_t got = 0, total = 0;
    bool any = false;
    for (const Download& d : list_) {
        if (d.state != DownloadState::Running) continue;
        if (d.total <= 0) return -1;
        any = true;
        got += d.received;
        total += d.total;
    }
    return any && total > 0 ? std::clamp((double)got / (double)total, 0.0, 1.0) : -1;
}

std::string formatBytes(std::int64_t n) {
    char b[32];
    if (n < 0) return "unknown size";
    if (n < 1000) std::snprintf(b, sizeof b, "%lld bytes", (long long)n);
    else if (n < 1000 * 1000) std::snprintf(b, sizeof b, "%.0f KB", n / 1000.0);
    else if (n < 1000LL * 1000 * 1000) std::snprintf(b, sizeof b, n < 10 * 1000 * 1000 ? "%.1f MB" : "%.0f MB", n / 1e6);
    else std::snprintf(b, sizeof b, "%.1f GB", n / 1e9);
    return b;
}

std::string downloadStatus(const Download& d, std::int64_t now) {
    switch (d.state) {
        case DownloadState::Done: return "Done, " + formatBytes(d.total > 0 ? d.total : d.received);
        case DownloadState::Cancelled: return "Cancelled";
        case DownloadState::Failed: return d.reason.empty() ? std::string("Failed") : "Failed: " + d.reason;
        case DownloadState::Paused:
            return "Paused, " + formatBytes(d.received) + (d.total > 0 ? " of " + formatBytes(d.total) : std::string());
        case DownloadState::Running: break;
    }
    std::string s = formatBytes(d.received) + (d.total > 0 ? " of " + formatBytes(d.total) : std::string());
    if (d.estimatedEnd > now) {
        const std::int64_t left = d.estimatedEnd - now;
        if (left < 60) s += ", " + std::to_string(left) + (left == 1 ? " second left" : " seconds left");
        else if (left < 3600) s += ", " + std::to_string((left + 30) / 60) + ((left + 30) / 60 == 1 ? " minute left" : " minutes left");
        else s += ", " + std::to_string((left + 1800) / 3600) + ((left + 1800) / 3600 == 1 ? " hour left" : " hours left");
    }
    return s;
}

}  // namespace nrb

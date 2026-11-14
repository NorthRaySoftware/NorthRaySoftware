#include "nrb/references.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cmath>
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

std::int64_t now() { return (std::int64_t)std::time(nullptr); }

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i, 80);
}

}  // namespace

std::unique_ptr<ReferenceStore> ReferenceStore::open(const std::string& path, std::string& error) {
    std::unique_ptr<ReferenceStore> s(new ReferenceStore());
    if (sqlite3_open_v2(path.c_str(), &s->db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        error = s->db_ ? sqlite3_errmsg(s->db_) : "couldn't open the reference boards";
        return nullptr;
    }
    sqlite3_busy_timeout(s->db_, 3000);
    char* msg = nullptr;
    if (sqlite3_exec(s->db_,
                     "PRAGMA foreign_keys=ON;"
                     "CREATE TABLE IF NOT EXISTS boards(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE, made INTEGER NOT NULL);"
                     "CREATE TABLE IF NOT EXISTS pictures(id INTEGER PRIMARY KEY, board INTEGER NOT NULL REFERENCES boards(id) ON DELETE CASCADE,"
                     " file TEXT NOT NULL, source TEXT NOT NULL DEFAULT '', page TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',"
                     " width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0, added INTEGER NOT NULL);"
                     "CREATE TABLE IF NOT EXISTS colours(id INTEGER PRIMARY KEY, board INTEGER NOT NULL REFERENCES boards(id) ON DELETE CASCADE,"
                     " rgb INTEGER NOT NULL, page TEXT NOT NULL DEFAULT '', added INTEGER NOT NULL);",
                     nullptr, nullptr, &msg) != SQLITE_OK) {
        error = msg ? msg : "couldn't set up the reference boards";
        sqlite3_free(msg);
        return nullptr;
    }
    if (!s->firstBoard()) s->addBoard("References");
    return s;
}

ReferenceStore::~ReferenceStore() { sqlite3_close(db_); }

std::vector<RefBoard> ReferenceStore::boards() const {
    std::vector<RefBoard> out;
    Stmt q(db_, "SELECT b.id, b.name, (SELECT COUNT(*) FROM pictures p WHERE p.board=b.id) FROM boards b ORDER BY b.made, b.id");
    while (q.step()) out.push_back({q.i64(0), q.text(1), (int)q.i64(2)});
    return out;
}

std::int64_t ReferenceStore::addBoard(const std::string& nameIn) {
    const std::string name = trimmed(nameIn);
    if (name.empty()) return 0;
    Stmt ins(db_, "INSERT INTO boards(name, made) VALUES(?, ?)");
    ins.bind(1, name).bind(2, now());
    return ins.run() ? sqlite3_last_insert_rowid(db_) : 0;
}

bool ReferenceStore::renameBoard(std::int64_t id, const std::string& nameIn) {
    const std::string name = trimmed(nameIn);
    if (name.empty()) return false;
    Stmt u(db_, "UPDATE boards SET name=? WHERE id=?");
    return u.bind(1, name).bind(2, id).run() && sqlite3_changes(db_) == 1;
}

std::vector<std::string> ReferenceStore::removeBoard(std::int64_t id) {
    std::vector<std::string> files;
    {
        Stmt q(db_, "SELECT file FROM pictures WHERE board=? AND file NOT IN (SELECT file FROM pictures WHERE board<>?)");
        q.bind(1, id).bind(2, id);
        while (q.step()) files.push_back(q.text(0));
    }
    Stmt del(db_, "DELETE FROM boards WHERE id=?");
    del.bind(1, id).run();
    if (!firstBoard()) addBoard("References");   // there's always a board
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

std::int64_t ReferenceStore::firstBoard() const {
    Stmt q(db_, "SELECT id FROM boards ORDER BY made, id LIMIT 1");
    return q.step() ? q.i64(0) : 0;
}

std::int64_t ReferenceStore::addPicture(const RefPicture& p) {
    Stmt ins(db_, "INSERT INTO pictures(board,file,source,page,title,width,height,added) VALUES(?,?,?,?,?,?,?,?)");
    ins.bind(1, p.board).bind(2, p.file).bind(3, p.source).bind(4, p.page).bind(5, p.title).bind(6, (std::int64_t)p.width)
        .bind(7, (std::int64_t)p.height).bind(8, p.added ? p.added : now());
    return ins.run() ? sqlite3_last_insert_rowid(db_) : 0;
}

std::vector<RefPicture> ReferenceStore::pictures(std::int64_t board) const {
    std::vector<RefPicture> out;
    Stmt q(db_, "SELECT id,board,file,source,page,title,width,height,added FROM pictures WHERE board=? ORDER BY added DESC, id DESC");
    q.bind(1, board);
    while (q.step())
        out.push_back({q.i64(0), q.i64(1), q.text(2), q.text(3), q.text(4), q.text(5), (int)q.i64(6), (int)q.i64(7), q.i64(8)});
    return out;
}

const std::string ReferenceStore::pictureFile(std::int64_t id) const {
    Stmt q(db_, "SELECT file FROM pictures WHERE id=?");
    q.bind(1, id);
    return q.step() ? q.text(0) : std::string();
}

bool ReferenceStore::movePicture(std::int64_t id, std::int64_t board) {
    Stmt u(db_, "UPDATE pictures SET board=? WHERE id=?");
    return u.bind(1, board).bind(2, id).run() && sqlite3_changes(db_) == 1;
}

std::string ReferenceStore::removePicture(std::int64_t id) {
    const std::string file = pictureFile(id);
    Stmt del(db_, "DELETE FROM pictures WHERE id=?");
    del.bind(1, id).run();
    // The file stays if another board still has the same picture.
    Stmt q(db_, "SELECT COUNT(*) FROM pictures WHERE file=?");
    q.bind(1, file);
    return q.step() && q.i64(0) == 0 ? file : std::string();
}

bool ReferenceStore::hasSource(std::int64_t board, const std::string& source) const {
    Stmt q(db_, "SELECT 1 FROM pictures WHERE board=? AND source=? LIMIT 1");
    q.bind(1, board).bind(2, source);
    return q.step();
}

std::int64_t ReferenceStore::addColour(const RefColour& c) {
    {
        Stmt q(db_, "SELECT id FROM colours WHERE board=? AND rgb=?");
        q.bind(1, c.board).bind(2, (std::int64_t)(c.rgb & 0xFFFFFF));
        if (q.step()) return q.i64(0);
    }
    Stmt ins(db_, "INSERT INTO colours(board,rgb,page,added) VALUES(?,?,?,?)");
    ins.bind(1, c.board).bind(2, (std::int64_t)(c.rgb & 0xFFFFFF)).bind(3, c.page).bind(4, c.added ? c.added : now());
    return ins.run() ? sqlite3_last_insert_rowid(db_) : 0;
}

std::vector<RefColour> ReferenceStore::colours(std::int64_t board) const {
    std::vector<RefColour> out;
    Stmt q(db_, "SELECT id,board,rgb,page,added FROM colours WHERE board=? ORDER BY added DESC, id DESC");
    q.bind(1, board);
    while (q.step()) out.push_back({q.i64(0), q.i64(1), (std::uint32_t)q.i64(2), q.text(3), q.i64(4)});
    return out;
}

bool ReferenceStore::removeColour(std::int64_t id) {
    Stmt del(db_, "DELETE FROM colours WHERE id=?");
    return del.bind(1, id).run() && sqlite3_changes(db_) == 1;
}

std::string hexColour(std::uint32_t rgb) {
    char b[8];
    std::snprintf(b, sizeof b, "#%06X", (unsigned)(rgb & 0xFFFFFF));
    return b;
}

std::vector<std::uint32_t> mainColours(const std::uint8_t* rgba, int w, int h, int count) {
    if (!rgba || w <= 0 || h <= 0 || count <= 0) return {};
    // A histogram of 4-bit-per-channel shades, from up to about 60,000 pixels.
    struct Bin { std::int64_t n = 0, r = 0, g = 0, b = 0; };
    std::vector<Bin> bins(4096);
    const std::int64_t total = (std::int64_t)w * h;
    const int step = std::max(1, (int)std::sqrt((double)total / 60000.0));
    for (int y = 0; y < h; y += step)
        for (int x = 0; x < w; x += step) {
            const std::uint8_t* p = rgba + ((size_t)y * w + x) * 4;
            if (p[3] < 128) continue;
            Bin& b = bins[((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4)];
            ++b.n; b.r += p[0]; b.g += p[1]; b.b += p[2];
        }
    std::vector<int> order;
    for (int i = 0; i < 4096; ++i) if (bins[i].n) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return bins[a].n != bins[b].n ? bins[a].n > bins[b].n : a < b; });
    std::vector<std::uint32_t> out;
    std::vector<std::array<double, 3>> picked;
    for (int i : order) {
        const Bin& b = bins[i];
        const std::array<double, 3> c{(double)b.r / b.n, (double)b.g / b.n, (double)b.b / b.n};
        bool near = false;
        for (const auto& p : picked)
            if (std::sqrt((c[0] - p[0]) * (c[0] - p[0]) + (c[1] - p[1]) * (c[1] - p[1]) + (c[2] - p[2]) * (c[2] - p[2])) < 48) near = true;
        if (near) continue;
        picked.push_back(c);
        out.push_back(((std::uint32_t)std::lround(c[0]) << 16) | ((std::uint32_t)std::lround(c[1]) << 8) | (std::uint32_t)std::lround(c[2]));
        if ((int)out.size() >= count) break;
    }
    return out;
}

}  // namespace nrb

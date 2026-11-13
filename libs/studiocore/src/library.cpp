#include "studio/library.hpp"

#include "internal.hpp"
#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <random>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace studio {
using detail::Stmt;

// ------------------------------------------------------------------ enums

namespace {
template <class E, size_t N>
struct EnumTable { std::array<std::pair<E, const char*>, N> ids; std::array<const char*, N> labels; };

const EnumTable<BookStatus, 8> kBook{{{{BookStatus::Idea, "idea"}, {BookStatus::Planning, "planning"},
                                       {BookStatus::Drafting, "drafting"}, {BookStatus::Revising, "revising"},
                                       {BookStatus::Editing, "editing"}, {BookStatus::ReadyToPublish, "ready"},
                                       {BookStatus::Published, "published"}, {BookStatus::OnHold, "on-hold"}}},
                                     {"Idea", "Planning", "Drafting", "Revising", "Editing", "Ready to publish", "Published", "On hold"}};
const EnumTable<SeriesStatus, 4> kSeries{{{{SeriesStatus::Planned, "planned"}, {SeriesStatus::Ongoing, "ongoing"},
                                           {SeriesStatus::Complete, "complete"}, {SeriesStatus::OnHold, "on-hold"}}},
                                         {"Planned", "Ongoing", "Complete", "On hold"}};
const EnumTable<Instalment, 5> kKind{{{{Instalment::Novel, "novel"}, {Instalment::Novella, "novella"},
                                       {Instalment::Companion, "companion"}, {Instalment::ShortStory, "short-story"},
                                       {Instalment::Collection, "collection"}}},
                                     {"Novel", "Novella", "Companion story", "Short story", "Collection"}};

template <class E, size_t N> const char* idOf(const EnumTable<E, N>& t, E v) {
    for (auto& [e, s] : t.ids) if (e == v) return s;
    return t.ids[0].second;
}
template <class E, size_t N> const char* labelOf(const EnumTable<E, N>& t, E v) {
    for (size_t i = 0; i < N; ++i) if (t.ids[i].first == v) return t.labels[i];
    return t.labels[0];
}
template <class E, size_t N> std::optional<E> fromId(const EnumTable<E, N>& t, const std::string& s) {
    for (auto& [e, id] : t.ids) if (s == id) return e;
    return std::nullopt;
}
}  // namespace

const char* toString(BookStatus s) { return idOf(kBook, s); }
const char* toString(SeriesStatus s) { return idOf(kSeries, s); }
const char* toString(Instalment k) { return idOf(kKind, k); }
const char* label(BookStatus s) { return labelOf(kBook, s); }
const char* label(SeriesStatus s) { return labelOf(kSeries, s); }
const char* label(Instalment k) { return labelOf(kKind, k); }
std::optional<BookStatus> bookStatusFrom(const std::string& s) { return fromId(kBook, s); }
std::optional<SeriesStatus> seriesStatusFrom(const std::string& s) { return fromId(kSeries, s); }
std::optional<Instalment> instalmentFrom(const std::string& s) { return fromId(kKind, s); }

// ---------------------------------------------------------------- helpers

static fs::path u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
static std::string fromPath(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string newId() {
    static std::mt19937_64 rng{std::random_device{}() ^ (std::uint64_t)std::time(nullptr)};
    unsigned char b[16];
    for (int i = 0; i < 16; i += 8) {
        const std::uint64_t v = rng();
        std::memcpy(b + i, &v, 8);
    }
    b[6] = (b[6] & 0x0F) | 0x40;  // version 4
    b[8] = (b[8] & 0x3F) | 0x80;  // variant
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += hex[b[i] >> 4];
        out += hex[b[i] & 15];
    }
    return out;
}

Time now() { return (Time)std::time(nullptr); }

std::string sha256Hex(const std::string& data) {
    static const std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg = data;
    const std::uint64_t bits = (std::uint64_t)data.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56) msg += (char)0;
    for (int i = 7; i >= 0; --i) msg += (char)((bits >> (i * 8)) & 0xFF);
    auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t)(unsigned char)msg[off + i * 4] << 24 | (std::uint32_t)(unsigned char)msg[off + i * 4 + 1] << 16 |
                   (std::uint32_t)(unsigned char)msg[off + i * 4 + 2] << 8 | (std::uint32_t)(unsigned char)msg[off + i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}

bool atomicWrite(const std::string& path, const std::string& bytes, std::string& error) {
    const fs::path target = u8(path);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    const fs::path tmp = target.parent_path() / (target.filename().u8string() + u8".tmp-" + std::u8string(u8"w"));
#ifdef _WIN32
    FILE* f = _wfopen(tmp.wstring().c_str(), L"wb");
#else
    FILE* f = std::fopen(tmp.string().c_str(), "wb");
#endif
    if (!f) { error = "Couldn't create " + fromPath(tmp); return false; }
    const bool wrote = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    bool flushed = std::fflush(f) == 0;
#ifdef _WIN32
    flushed = flushed && _commit(_fileno(f)) == 0;
#else
    flushed = flushed && fsync(fileno(f)) == 0;
#endif
    std::fclose(f);
    if (!wrote || !flushed) {
        fs::remove(tmp, ec);
        error = "Couldn't write " + path + " (is the disk full?)";
        return false;
    }
#ifdef _WIN32
    // Antivirus and search indexers sometimes hold files briefly: retry.
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (MoveFileExW(tmp.wstring().c_str(), target.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        Sleep(50 * (attempt + 1));
    }
    fs::remove(tmp, ec);
    error = "Couldn't replace " + path + ". Another program may have it open.";
    return false;
#else
    fs::rename(tmp, target, ec);
    if (ec) { error = "Couldn't replace " + path + ": " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
#endif
}

bool looksLikeSyncFolder(const std::string& path) {
    std::string p = path;
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (const char* s : {"onedrive", "dropbox", "google drive", "googledrive", "icloud", "box sync", "pcloud", "mega"})
        if (p.find(s) != std::string::npos) return true;
    return false;
}

static std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// ------------------------------------------------------------ SQLite glue

namespace {

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS worlds (
  id TEXT PRIMARY KEY, name TEXT NOT NULL, description TEXT NOT NULL DEFAULT '', accent TEXT NOT NULL DEFAULT '',
  notes TEXT NOT NULL DEFAULT '', banner_blob TEXT NOT NULL DEFAULT '', logo_blob TEXT NOT NULL DEFAULT '',
  sort_order REAL NOT NULL DEFAULT 0, created INTEGER NOT NULL, modified INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS series (
  id TEXT PRIMARY KEY, world_id TEXT REFERENCES worlds(id) ON DELETE CASCADE, name TEXT NOT NULL,
  description TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'planned', notes TEXT NOT NULL DEFAULT '',
  cover_blob TEXT NOT NULL DEFAULT '', logo_blob TEXT NOT NULL DEFAULT '', sort_order REAL NOT NULL DEFAULT 0,
  created INTEGER NOT NULL, modified INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS books (
  id TEXT PRIMARY KEY, world_id TEXT REFERENCES worlds(id) ON DELETE CASCADE, title TEXT NOT NULL,
  subtitle TEXT NOT NULL DEFAULT '', author TEXT NOT NULL DEFAULT '', pen_name TEXT NOT NULL DEFAULT '',
  synopsis TEXT NOT NULL DEFAULT '', genre TEXT NOT NULL DEFAULT '', language TEXT NOT NULL DEFAULT 'en-GB',
  isbn TEXT NOT NULL DEFAULT '', edition TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'idea',
  tags TEXT NOT NULL DEFAULT '[]', cover_blob TEXT NOT NULL DEFAULT '', word_count INTEGER NOT NULL DEFAULT 0,
  sort_order REAL NOT NULL DEFAULT 0, created INTEGER NOT NULL, modified INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS memberships (
  series_id TEXT NOT NULL REFERENCES series(id) ON DELETE CASCADE,
  book_id TEXT NOT NULL REFERENCES books(id) ON DELETE CASCADE,
  reading_order REAL NOT NULL DEFAULT 0, chrono_order REAL NOT NULL DEFAULT 0,
  number TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL DEFAULT 'novel',
  PRIMARY KEY (series_id, book_id));
CREATE INDEX IF NOT EXISTS memberships_book ON memberships(book_id);
-- Schema 2: manuscripts.
CREATE TABLE IF NOT EXISTS nodes (
  id TEXT PRIMARY KEY, book_id TEXT NOT NULL REFERENCES books(id) ON DELETE CASCADE,
  parent_id TEXT REFERENCES nodes(id) ON DELETE CASCADE, kind TEXT NOT NULL, title TEXT NOT NULL DEFAULT '',
  synopsis TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'draft', sort_order REAL NOT NULL DEFAULT 0,
  content TEXT NOT NULL DEFAULT '', word_count INTEGER NOT NULL DEFAULT 0,
  created INTEGER NOT NULL, modified INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS nodes_book ON nodes(book_id);
CREATE INDEX IF NOT EXISTS nodes_parent ON nodes(parent_id);
CREATE TABLE IF NOT EXISTS node_versions (
  id INTEGER PRIMARY KEY AUTOINCREMENT, node_id TEXT NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
  saved INTEGER NOT NULL, word_count INTEGER NOT NULL DEFAULT 0, reason TEXT NOT NULL DEFAULT '', content TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS node_versions_node ON node_versions(node_id, saved);
-- Schema 3: the artwork library (books.trim is added by the upgrade step).
CREATE TABLE IF NOT EXISTS artworks (
  id TEXT PRIMARY KEY, blob TEXT NOT NULL UNIQUE, name TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL DEFAULT 'illustration',
  width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0, format TEXT NOT NULL DEFAULT '',
  source TEXT NOT NULL DEFAULT '', notes TEXT NOT NULL DEFAULT '',
  created INTEGER NOT NULL, modified INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0);
-- Schema 4: editions of a book.
CREATE TABLE IF NOT EXISTS editions (
  id TEXT PRIMARY KEY, book_id TEXT NOT NULL REFERENCES books(id) ON DELETE CASCADE, kind TEXT NOT NULL DEFAULT 'paperback',
  name TEXT NOT NULL DEFAULT '', isbn TEXT NOT NULL DEFAULT '', trim TEXT NOT NULL DEFAULT '', design TEXT NOT NULL DEFAULT '',
  paper TEXT NOT NULL DEFAULT 'white', bleed INTEGER NOT NULL DEFAULT 0, colour INTEGER NOT NULL DEFAULT 0,
  sort_order REAL NOT NULL DEFAULT 0, created INTEGER NOT NULL, modified INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS editions_book ON editions(book_id);
-- Schema 5: worldbuilding.
CREATE TABLE IF NOT EXISTS entries (
  id TEXT PRIMARY KEY, world_id TEXT REFERENCES worlds(id) ON DELETE CASCADE, book_id TEXT REFERENCES books(id) ON DELETE CASCADE,
  kind TEXT NOT NULL DEFAULT 'character', name TEXT NOT NULL DEFAULT '', aliases TEXT NOT NULL DEFAULT '[]',
  summary TEXT NOT NULL DEFAULT '', details TEXT NOT NULL DEFAULT '', notes TEXT NOT NULL DEFAULT '', image_blob TEXT NOT NULL DEFAULT '',
  sort_order REAL NOT NULL DEFAULT 0, created INTEGER NOT NULL, modified INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS entries_world ON entries(world_id);
CREATE TABLE IF NOT EXISTS entry_links (
  from_id TEXT NOT NULL REFERENCES entries(id) ON DELETE CASCADE, to_id TEXT NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
  label TEXT NOT NULL DEFAULT '', PRIMARY KEY (from_id, to_id, label));
-- Schema 7: editorial.
CREATE TABLE IF NOT EXISTS snapshots (
  id TEXT PRIMARY KEY, book_id TEXT NOT NULL REFERENCES books(id) ON DELETE CASCADE, name TEXT NOT NULL DEFAULT '',
  taken INTEGER NOT NULL, words INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS snapshot_nodes (
  snapshot_id TEXT NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE, node_id TEXT NOT NULL, kind TEXT NOT NULL,
  title TEXT NOT NULL DEFAULT '', content TEXT NOT NULL DEFAULT '', word_count INTEGER NOT NULL DEFAULT 0, position INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (snapshot_id, node_id));
-- Schema 8: comments.
CREATE TABLE IF NOT EXISTS comments (
  id TEXT PRIMARY KEY, node_id TEXT NOT NULL REFERENCES nodes(id) ON DELETE CASCADE, body TEXT NOT NULL DEFAULT '',
  quote TEXT NOT NULL DEFAULT '', created INTEGER NOT NULL, modified INTEGER NOT NULL, resolved INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS comments_node ON comments(node_id);
CREATE TABLE IF NOT EXISTS writing_log (
  day TEXT NOT NULL, book_id TEXT NOT NULL REFERENCES books(id) ON DELETE CASCADE, words INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (day, book_id));
)SQL";
}  // namespace

bool Library::exec(const char* sql) {
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        error_ = msg ? msg : "database error";
        sqlite3_free(msg);
        return false;
    }
    return true;
}
bool Library::begin() { return exec("BEGIN IMMEDIATE"); }
bool Library::commit() {
    if (exec("COMMIT")) return true;
    rollback();
    return false;
}
void Library::rollback() {
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    load();  // the in-memory copy must match what's on disk
}

// ---------------------------------------------------------------- opening

std::unique_ptr<Library> Library::open(const std::string& folder, OpenResult& r) {
    r = {};
    std::error_code ec;
    const fs::path dir = u8(folder);
    fs::create_directories(dir / "objects", ec);
    fs::create_directories(dir / "backups", ec);
    if (!fs::is_directory(dir)) { r.error = "Couldn't create the library folder " + folder; return nullptr; }
    const fs::path dbPath = dir / "library.db";
    const bool existed = fs::exists(dbPath);

    std::unique_ptr<Library> lib(new Library);
    lib->folder_ = folder;
    if (sqlite3_open_v2(fromPath(dbPath).c_str(), &lib->db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        r.error = "Couldn't open the library database: " + std::string(lib->db_ ? sqlite3_errmsg(lib->db_) : "unknown error");
        return nullptr;
    }
    sqlite3_busy_timeout(lib->db_, 3000);
    if (!lib->exec("PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL; PRAGMA synchronous = FULL;")) {
        r.error = "The library database couldn't be prepared: " + lib->error_;
        return nullptr;
    }
    if (existed) {
        Stmt q(lib->db_, "PRAGMA quick_check");
        if (!q || !q.row() || q.text(0) != "ok") {
            r.error = "The library database is damaged. Restore the newest copy from the backups folder (" +
                      fromPath(dir / "backups") + ").";
            return nullptr;
        }
        Stmt v(lib->db_, "SELECT value FROM meta WHERE key = 'schema_version'");
        if (v && v.row()) {
            const int found = std::atoi(v.text(0).c_str());
            if (found > kSchemaVersion) {
                r.error = "This library was made by a newer version of the Studio. Update the Studio to open it.";
                return nullptr;
            }
        }
    }
    // Create or migrate. Version 1 is the first format; later versions add steps here.
    int foundVersion = 0;
    if (existed) {
        Stmt v(lib->db_, "SELECT value FROM meta WHERE key = 'schema_version'");
        if (v && v.row()) foundVersion = std::atoi(v.text(0).c_str());
    }
    if (!lib->begin() || !lib->exec(kSchema)) { r.error = "Couldn't set up the library: " + lib->error_; return nullptr; }
    {
        // Columns added to existing tables: schema 3 (a book's trim size), schema 6 (planning).
        struct Add { const char* table; const char* column; const char* sql; };
        static const Add adds[] = {
            {"books", "trim", "ALTER TABLE books ADD COLUMN trim TEXT NOT NULL DEFAULT ''"},
            {"nodes", "pov_id", "ALTER TABLE nodes ADD COLUMN pov_id TEXT NOT NULL DEFAULT ''"},
            {"nodes", "target_words", "ALTER TABLE nodes ADD COLUMN target_words INTEGER NOT NULL DEFAULT 0"},
        };
        for (const Add& a : adds) {
            bool has = false;
            Stmt c(lib->db_, (std::string("PRAGMA table_info(") + a.table + ")").c_str());
            while (c && c.row()) if (c.text(1) == a.column) has = true;
            if (!has && !lib->exec(a.sql)) {
                lib->rollback();
                r.error = "Couldn't upgrade the library: " + lib->error_;
                return nullptr;
            }
        }
    }
    {
        Stmt s(lib->db_, "INSERT INTO meta(key, value) VALUES ('format', 'studio-library'), ('schema_version', ?1) "
                         "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
        s.bind(1, std::to_string(kSchemaVersion));
        if (!s.run() || !lib->commit()) { r.error = "Couldn't set up the library: " + lib->error_; return nullptr; }
    }
    if (!lib->load()) { r.error = "Couldn't read the library: " + lib->error_; return nullptr; }
    // Upgrading from before the artwork library: list the pictures already in the text.
    if (existed && foundVersion < 3 && !lib->registerArtworksInText())
        r.notes.push_back("Some pictures couldn't be added to the artwork library: " + lib->error_);

    r.created = !existed;
    if (existed) {
        std::string berr;
        if (lib->backup(berr)) r.notes.push_back("Made a backup copy of the library.");
        else r.notes.push_back("Couldn't make a backup copy: " + berr);
    }
    if (looksLikeSyncFolder(folder))
        r.notes.push_back("This library is in a cloud-sync folder. Sync programs can copy a database while it's being written; "
                          "it's safer to keep the library outside it and use Export on the Settings page for copies.");
    r.ok = true;
    return lib;
}

Library::~Library() {
    if (db_) {
        sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr, nullptr);
        sqlite3_close_v2(db_);
    }
}

int Library::schemaVersion() const {
    Stmt v(db_, "SELECT value FROM meta WHERE key = 'schema_version'");
    return v && v.row() ? std::atoi(v.text(0).c_str()) : 0;
}

bool Library::load() {
    worlds_.clear(); series_.clear(); books_.clear(); memberships_.clear();
    {
        Stmt q(db_, "SELECT id, name, description, accent, notes, banner_blob, logo_blob, sort_order, created, modified, deleted "
                    "FROM worlds ORDER BY sort_order, created");
        if (!q) { error_ = sqlite3_errmsg(db_); return false; }
        while (q.row()) {
            World w;
            w.id = q.text(0); w.name = q.text(1); w.description = q.text(2); w.accent = q.text(3); w.notes = q.text(4);
            w.bannerBlob = q.text(5); w.logoBlob = q.text(6); w.order = q.dbl(7);
            w.created = q.i64(8); w.modified = q.i64(9); w.deleted = q.i64(10);
            worlds_.push_back(std::move(w));
        }
    }
    {
        Stmt q(db_, "SELECT id, world_id, name, description, status, notes, cover_blob, logo_blob, sort_order, created, modified, deleted "
                    "FROM series ORDER BY sort_order, created");
        if (!q) { error_ = sqlite3_errmsg(db_); return false; }
        while (q.row()) {
            Series s;
            s.id = q.text(0); s.worldId = q.text(1); s.name = q.text(2); s.description = q.text(3);
            s.status = seriesStatusFrom(q.text(4)).value_or(SeriesStatus::Planned);
            s.notes = q.text(5); s.coverBlob = q.text(6); s.logoBlob = q.text(7); s.order = q.dbl(8);
            s.created = q.i64(9); s.modified = q.i64(10); s.deleted = q.i64(11);
            series_.push_back(std::move(s));
        }
    }
    {
        Stmt q(db_, "SELECT id, world_id, title, subtitle, author, pen_name, synopsis, genre, language, isbn, edition, status, tags, "
                    "cover_blob, word_count, sort_order, created, modified, deleted, trim FROM books ORDER BY sort_order, created");
        if (!q) { error_ = sqlite3_errmsg(db_); return false; }
        while (q.row()) {
            Book b;
            b.id = q.text(0); b.worldId = q.text(1); b.title = q.text(2); b.subtitle = q.text(3); b.author = q.text(4);
            b.penName = q.text(5); b.synopsis = q.text(6); b.genre = q.text(7); b.language = q.text(8); b.isbn = q.text(9);
            b.edition = q.text(10); b.status = bookStatusFrom(q.text(11)).value_or(BookStatus::Idea);
            json t = json::parse(q.text(12), nullptr, false);
            if (t.is_array())
                for (auto& x : t) if (x.is_string()) b.tags.push_back(x.get<std::string>());
            b.coverBlob = q.text(13); b.wordCount = q.i64(14); b.order = q.dbl(15);
            b.created = q.i64(16); b.modified = q.i64(17); b.deleted = q.i64(18); b.trim = q.text(19);
            books_.push_back(std::move(b));
        }
    }
    {
        Stmt q(db_, "SELECT series_id, book_id, reading_order, chrono_order, number, kind FROM memberships");
        if (!q) { error_ = sqlite3_errmsg(db_); return false; }
        while (q.row()) {
            Membership m;
            m.seriesId = q.text(0); m.bookId = q.text(1); m.readingOrder = q.dbl(2); m.chronoOrder = q.dbl(3);
            m.number = q.text(4); m.kind = instalmentFrom(q.text(5)).value_or(Instalment::Novel);
            memberships_.push_back(std::move(m));
        }
    }
    return loadNodes() && loadArtworks() && loadEditions() && loadEntries() && loadComments();
}

// ---------------------------------------------------------------- lookups

const World* Library::world(const Id& id) const {
    for (auto& w : worlds_) if (w.id == id) return &w;
    return nullptr;
}
const Series* Library::seriesById(const Id& id) const {
    for (auto& s : series_) if (s.id == id) return &s;
    return nullptr;
}
const Book* Library::book(const Id& id) const {
    for (auto& b : books_) if (b.id == id) return &b;
    return nullptr;
}
World* Library::worldMut(const Id& id) { return const_cast<World*>(world(id)); }
Series* Library::seriesMut(const Id& id) { return const_cast<Series*>(seriesById(id)); }
Book* Library::bookMut(const Id& id) { return const_cast<Book*>(book(id)); }

bool Library::seriesVisible(const Series& s) const {
    if (s.deleted) return false;
    if (s.worldId.empty()) return true;
    const World* w = world(s.worldId);
    return w && !w->deleted;
}
bool Library::bookVisible(const Book& b) const {
    if (b.deleted) return false;
    if (b.worldId.empty()) return true;
    const World* w = world(b.worldId);
    return w && !w->deleted;
}

std::vector<const World*> Library::liveWorlds() const {
    std::vector<const World*> out;
    for (auto& w : worlds_) if (!w.deleted) out.push_back(&w);
    return out;
}
std::vector<const Series*> Library::seriesIn(const Id& worldId) const {
    std::vector<const Series*> out;
    for (auto& s : series_) if (s.worldId == worldId && seriesVisible(s)) out.push_back(&s);
    return out;
}
std::vector<const Book*> Library::standaloneBooksIn(const Id& worldId) const {
    std::vector<const Book*> out;
    for (auto& b : books_) {
        if (b.worldId != worldId || !bookVisible(b)) continue;
        bool inSeries = false;
        for (auto& m : memberships_) {
            if (m.bookId != b.id) continue;
            const Series* s = seriesById(m.seriesId);
            if (s && seriesVisible(*s)) { inSeries = true; break; }
        }
        if (!inSeries) out.push_back(&b);
    }
    return out;
}
std::vector<const Book*> Library::booksInSeries(const Id& seriesId, bool chronological) const {
    std::vector<std::pair<double, const Book*>> v;
    for (auto& m : memberships_) {
        if (m.seriesId != seriesId) continue;
        const Book* b = book(m.bookId);
        if (b && bookVisible(*b)) v.push_back({chronological ? m.chronoOrder : m.readingOrder, b});
    }
    std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<const Book*> out;
    for (auto& [o, b] : v) out.push_back(b);
    return out;
}
std::vector<const Series*> Library::seriesOfBook(const Id& bookId) const {
    std::vector<const Series*> out;
    for (auto& m : memberships_) {
        if (m.bookId != bookId) continue;
        const Series* s = seriesById(m.seriesId);
        if (s && seriesVisible(*s)) out.push_back(s);
    }
    return out;
}
const Membership* Library::membership(const Id& seriesId, const Id& bookId) const {
    for (auto& m : memberships_) if (m.seriesId == seriesId && m.bookId == bookId) return &m;
    return nullptr;
}
std::vector<const Book*> Library::liveBooks() const {
    std::vector<const Book*> out;
    for (auto& b : books_) if (bookVisible(b)) out.push_back(&b);
    return out;
}

// ------------------------------------------------------------------ saving

bool Library::saveWorld(const World& w) {
    Stmt s(db_, "INSERT INTO worlds(id, name, description, accent, notes, banner_blob, logo_blob, sort_order, created, modified, deleted) "
                "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) ON CONFLICT(id) DO UPDATE SET name=excluded.name, "
                "description=excluded.description, accent=excluded.accent, notes=excluded.notes, banner_blob=excluded.banner_blob, "
                "logo_blob=excluded.logo_blob, sort_order=excluded.sort_order, modified=excluded.modified, deleted=excluded.deleted");
    s.bind(1, w.id).bind(2, w.name).bind(3, w.description).bind(4, w.accent).bind(5, w.notes).bind(6, w.bannerBlob)
        .bind(7, w.logoBlob).bind(8, w.order).bind(9, (std::int64_t)w.created).bind(10, (std::int64_t)w.modified)
        .bind(11, (std::int64_t)w.deleted);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}
bool Library::saveSeries(const Series& x) {
    Stmt s(db_, "INSERT INTO series(id, world_id, name, description, status, notes, cover_blob, logo_blob, sort_order, created, modified, deleted) "
                "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12) ON CONFLICT(id) DO UPDATE SET world_id=excluded.world_id, "
                "name=excluded.name, description=excluded.description, status=excluded.status, notes=excluded.notes, "
                "cover_blob=excluded.cover_blob, logo_blob=excluded.logo_blob, sort_order=excluded.sort_order, "
                "modified=excluded.modified, deleted=excluded.deleted");
    s.bind(1, x.id).bindIdOrNull(2, x.worldId).bind(3, x.name).bind(4, x.description).bind(5, std::string(toString(x.status)))
        .bind(6, x.notes).bind(7, x.coverBlob).bind(8, x.logoBlob).bind(9, x.order).bind(10, (std::int64_t)x.created)
        .bind(11, (std::int64_t)x.modified).bind(12, (std::int64_t)x.deleted);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}
bool Library::saveBook(const Book& b) {
    Stmt s(db_, "INSERT INTO books(id, world_id, title, subtitle, author, pen_name, synopsis, genre, language, isbn, edition, status, "
                "tags, cover_blob, word_count, sort_order, created, modified, deleted, trim) VALUES "
                "(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20) ON CONFLICT(id) DO UPDATE SET "
                "world_id=excluded.world_id, title=excluded.title, subtitle=excluded.subtitle, author=excluded.author, "
                "pen_name=excluded.pen_name, synopsis=excluded.synopsis, genre=excluded.genre, language=excluded.language, "
                "isbn=excluded.isbn, edition=excluded.edition, status=excluded.status, tags=excluded.tags, "
                "cover_blob=excluded.cover_blob, word_count=excluded.word_count, sort_order=excluded.sort_order, "
                "modified=excluded.modified, deleted=excluded.deleted, trim=excluded.trim");
    s.bind(1, b.id).bindIdOrNull(2, b.worldId).bind(3, b.title).bind(4, b.subtitle).bind(5, b.author).bind(6, b.penName)
        .bind(7, b.synopsis).bind(8, b.genre).bind(9, b.language).bind(10, b.isbn).bind(11, b.edition)
        .bind(12, std::string(toString(b.status))).bind(13, json(b.tags).dump()).bind(14, b.coverBlob)
        .bind(15, (std::int64_t)b.wordCount).bind(16, b.order).bind(17, (std::int64_t)b.created)
        .bind(18, (std::int64_t)b.modified).bind(19, (std::int64_t)b.deleted).bind(20, b.trim);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}
bool Library::saveMembership(const Membership& m) {
    Stmt s(db_, "INSERT INTO memberships(series_id, book_id, reading_order, chrono_order, number, kind) VALUES (?1,?2,?3,?4,?5,?6) "
                "ON CONFLICT(series_id, book_id) DO UPDATE SET reading_order=excluded.reading_order, "
                "chrono_order=excluded.chrono_order, number=excluded.number, kind=excluded.kind");
    s.bind(1, m.seriesId).bind(2, m.bookId).bind(3, m.readingOrder).bind(4, m.chronoOrder).bind(5, m.number)
        .bind(6, std::string(toString(m.kind)));
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}
bool Library::deleteMembership(const Id& seriesId, const Id& bookId) {
    Stmt s(db_, "DELETE FROM memberships WHERE series_id = ?1 AND book_id = ?2");
    s.bind(1, seriesId).bind(2, bookId);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    memberships_.erase(std::remove_if(memberships_.begin(), memberships_.end(),
                                      [&](auto& m) { return m.seriesId == seriesId && m.bookId == bookId; }),
                       memberships_.end());
    return true;
}

// Runs `body` in a transaction; on any failure everything is rolled back.
#define STUDIO_TX(body)                  \
    do {                                 \
        if (!begin()) return false;      \
        if (!(body)) { rollback(); return false; } \
        return commit();                 \
    } while (0)

// ---------------------------------------------------------------- creating

Id Library::createWorld(const std::string& name) {
    World w;
    w.id = newId();
    w.name = trimmed(name).empty() ? "New world" : trimmed(name);
    w.created = w.modified = now();
    double maxOrder = 0;
    for (auto& x : worlds_) maxOrder = std::max(maxOrder, x.order);
    w.order = nextOrder(maxOrder);
    if (!begin()) return {};
    if (!saveWorld(w)) { rollback(); return {}; }
    if (!commit()) return {};
    worlds_.push_back(w);
    return w.id;
}

Id Library::createSeries(const std::string& name, const Id& worldId) {
    if (!worldId.empty() && !world(worldId)) { error_ = "That world doesn't exist."; return {}; }
    Series s;
    s.id = newId();
    s.worldId = worldId;
    s.name = trimmed(name).empty() ? "New series" : trimmed(name);
    s.created = s.modified = now();
    double maxOrder = 0;
    for (auto& x : series_) if (x.worldId == worldId) maxOrder = std::max(maxOrder, x.order);
    s.order = nextOrder(maxOrder);
    if (!begin()) return {};
    if (!saveSeries(s)) { rollback(); return {}; }
    if (!commit()) return {};
    series_.push_back(s);
    return s.id;
}

Id Library::createBook(const std::string& title, const Id& worldId, const Id& seriesId) {
    Id w = worldId;
    if (!seriesId.empty()) {
        const Series* s = seriesById(seriesId);
        if (!s) { error_ = "That series doesn't exist."; return {}; }
        if (!s->worldId.empty()) w = s->worldId;
    }
    if (!w.empty() && !world(w)) { error_ = "That world doesn't exist."; return {}; }
    Book b;
    b.id = newId();
    b.worldId = w;
    b.title = trimmed(title).empty() ? "Untitled book" : trimmed(title);
    b.created = b.modified = now();
    double maxOrder = 0;
    for (auto& x : books_) if (x.worldId == w) maxOrder = std::max(maxOrder, x.order);
    b.order = nextOrder(maxOrder);
    if (!begin()) return {};
    if (!saveBook(b)) { rollback(); return {}; }
    books_.push_back(b);
    if (!seriesId.empty()) {
        Membership m;
        m.seriesId = seriesId;
        m.bookId = b.id;
        double maxR = 0, maxC = 0;
        int numbered = 0;
        for (auto& x : memberships_) if (x.seriesId == seriesId) {
            maxR = std::max(maxR, x.readingOrder); maxC = std::max(maxC, x.chronoOrder);
            if (!x.number.empty()) ++numbered;
        }
        m.readingOrder = maxR + 1;
        m.chronoOrder = maxC + 1;
        m.number = std::to_string(numbered + 1);
        if (!saveMembership(m)) { rollback(); return {}; }
        memberships_.push_back(m);
    }
    if (!commit()) return {};
    return b.id;
}

// ----------------------------------------------------------------- editing

bool Library::updateWorld(const World& in) {
    World* w = worldMut(in.id);
    if (!w) { error_ = "That world no longer exists."; return false; }
    World next = in;
    next.name = trimmed(in.name).empty() ? w->name : trimmed(in.name);
    next.created = w->created; next.order = w->order; next.deleted = w->deleted; next.modified = now();
    if (!begin()) return false;
    if (!saveWorld(next)) { rollback(); return false; }
    if (!commit()) return false;
    *w = next;
    return true;
}
bool Library::updateSeries(const Series& in) {
    Series* s = seriesMut(in.id);
    if (!s) { error_ = "That series no longer exists."; return false; }
    Series next = in;
    next.name = trimmed(in.name).empty() ? s->name : trimmed(in.name);
    next.worldId = s->worldId;  // moving between worlds goes through moveSeriesToWorld
    next.created = s->created; next.order = s->order; next.deleted = s->deleted; next.modified = now();
    if (!begin()) return false;
    if (!saveSeries(next)) { rollback(); return false; }
    if (!commit()) return false;
    *s = next;
    return true;
}
bool Library::updateBook(const Book& in) {
    Book* b = bookMut(in.id);
    if (!b) { error_ = "That book no longer exists."; return false; }
    Book next = in;
    next.title = trimmed(in.title).empty() ? b->title : trimmed(in.title);
    next.worldId = b->worldId;  // moving goes through moveBookToWorld
    next.created = b->created; next.order = b->order; next.deleted = b->deleted; next.modified = now();
    std::vector<std::string> tags;
    for (auto& t : in.tags) {
        const std::string x = trimmed(t);
        if (!x.empty() && std::find(tags.begin(), tags.end(), x) == tags.end()) tags.push_back(x);
    }
    next.tags = tags;
    if (!begin()) return false;
    if (!saveBook(next)) { rollback(); return false; }
    if (!commit()) return false;
    *b = next;
    return true;
}
bool Library::updateMembership(const Membership& in) {
    Membership* m = const_cast<Membership*>(membership(in.seriesId, in.bookId));
    if (!m) { error_ = "That book isn't in that series."; return false; }
    Membership next = *m;
    next.number = trimmed(in.number);
    next.kind = in.kind;
    if (!begin()) return false;
    if (!saveMembership(next)) { rollback(); return false; }
    if (!commit()) return false;
    *m = next;
    return true;
}

// ---------------------------------------------------------------- ordering

bool Library::reorderWorlds(const std::vector<Id>& ids) {
    if (!begin()) return false;
    double o = 1;
    for (auto& id : ids) {
        World* w = worldMut(id);
        if (!w) continue;
        World next = *w;
        next.order = o++;
        if (!saveWorld(next)) { rollback(); return false; }
        *w = next;
    }
    if (!commit()) return false;
    std::stable_sort(worlds_.begin(), worlds_.end(), [](auto& a, auto& b) { return a.order < b.order; });
    return true;
}
bool Library::reorderSeries(const std::vector<Id>& ids) {
    if (!begin()) return false;
    double o = 1;
    for (auto& id : ids) {
        Series* s = seriesMut(id);
        if (!s) continue;
        Series next = *s;
        next.order = o++;
        if (!saveSeries(next)) { rollback(); return false; }
        *s = next;
    }
    if (!commit()) return false;
    std::stable_sort(series_.begin(), series_.end(), [](auto& a, auto& b) { return a.order < b.order; });
    return true;
}
bool Library::reorderStandaloneBooks(const std::vector<Id>& ids) {
    if (!begin()) return false;
    double o = 1;
    for (auto& id : ids) {
        Book* b = bookMut(id);
        if (!b) continue;
        Book next = *b;
        next.order = o++;
        if (!saveBook(next)) { rollback(); return false; }
        *b = next;
    }
    if (!commit()) return false;
    std::stable_sort(books_.begin(), books_.end(), [](auto& a, auto& b) { return a.order < b.order; });
    return true;
}
bool Library::reorderSeriesBooks(const Id& seriesId, const std::vector<Id>& bookIds, bool chronological) {
    // If the series still uses the automatic numbering (1, 2, 3... in reading
    // order), the numbers follow a new reading order. Custom numbers ("2.5",
    // gaps, unnumbered companions) are never touched.
    bool autoNumbered = !chronological;
    if (autoNumbered) {
        int expect = 1;
        for (const Book* b : booksInSeries(seriesId, false)) {
            const Membership* m = membership(seriesId, b->id);
            if (!m || m->number != std::to_string(expect++)) { autoNumbered = false; break; }
        }
    }
    if (!begin()) return false;
    double o = 1;
    for (auto& id : bookIds) {
        Membership* m = const_cast<Membership*>(membership(seriesId, id));
        if (!m) continue;
        Membership next = *m;
        (chronological ? next.chronoOrder : next.readingOrder) = o;
        if (autoNumbered) next.number = std::to_string((int)o);
        ++o;
        if (!saveMembership(next)) { rollback(); return false; }
        *m = next;
    }
    return commit();
}

// ------------------------------------------------------------------ moving

std::vector<const Series*> Library::seriesLostByMove(const Id& bookId, const Id& toWorldId) const {
    std::vector<const Series*> out;
    for (auto& m : memberships_) {
        if (m.bookId != bookId) continue;
        const Series* s = seriesById(m.seriesId);
        if (s && !s->worldId.empty() && s->worldId != toWorldId) out.push_back(s);
    }
    return out;
}

bool Library::moveBookToWorldTx(Book& b, const Id& worldId) {
    std::vector<Id> lost;
    for (const Series* s : seriesLostByMove(b.id, worldId)) lost.push_back(s->id);
    for (auto& sid : lost) if (!deleteMembership(sid, b.id)) return false;
    Book next = b;
    next.worldId = worldId;
    next.modified = now();
    double maxOrder = 0;
    for (auto& x : books_) if (x.worldId == worldId) maxOrder = std::max(maxOrder, x.order);
    next.order = nextOrder(maxOrder);
    if (!saveBook(next)) return false;
    b = next;
    return true;
}

bool Library::moveBookToWorld(const Id& bookId, const Id& worldId) {
    Book* b = bookMut(bookId);
    if (!b) { error_ = "That book no longer exists."; return false; }
    if (!worldId.empty() && !world(worldId)) { error_ = "That world doesn't exist."; return false; }
    if (b->worldId == worldId) return true;
    STUDIO_TX(moveBookToWorldTx(*b, worldId));
}

bool Library::addBookToSeries(const Id& bookId, const Id& seriesId) {
    Book* b = bookMut(bookId);
    const Series* s = seriesById(seriesId);
    if (!b || !s) { error_ = "That book or series no longer exists."; return false; }
    if (membership(seriesId, bookId)) return true;
    if (!begin()) return false;
    if (!s->worldId.empty() && s->worldId != b->worldId && !moveBookToWorldTx(*b, s->worldId)) { rollback(); return false; }
    Membership m;
    m.seriesId = seriesId;
    m.bookId = bookId;
    double maxR = 0, maxC = 0;
    int numbered = 0;
    for (auto& x : memberships_) if (x.seriesId == seriesId) {
        maxR = std::max(maxR, x.readingOrder); maxC = std::max(maxC, x.chronoOrder);
        if (!x.number.empty()) ++numbered;
    }
    m.readingOrder = maxR + 1;
    m.chronoOrder = maxC + 1;
    m.number = std::to_string(numbered + 1);
    if (!saveMembership(m)) { rollback(); return false; }
    if (!commit()) return false;
    memberships_.push_back(m);
    return true;
}

bool Library::removeBookFromSeries(const Id& bookId, const Id& seriesId) {
    if (!membership(seriesId, bookId)) return true;
    STUDIO_TX(deleteMembership(seriesId, bookId));
}

bool Library::makeStandalone(const Id& bookId) {
    if (!book(bookId)) { error_ = "That book no longer exists."; return false; }
    std::vector<Id> ids;
    for (auto& m : memberships_) if (m.bookId == bookId) ids.push_back(m.seriesId);
    if (ids.empty()) return true;
    if (!begin()) return false;
    for (auto& sid : ids) if (!deleteMembership(sid, bookId)) { rollback(); return false; }
    return commit();
}

bool Library::moveSeriesToWorld(const Id& seriesId, const Id& worldId) {
    Series* s = seriesMut(seriesId);
    if (!s) { error_ = "That series no longer exists."; return false; }
    if (!worldId.empty() && !world(worldId)) { error_ = "That world doesn't exist."; return false; }
    if (s->worldId == worldId) return true;
    const Id oldWorld = s->worldId;
    if (!begin()) return false;
    Series next = *s;
    next.worldId = worldId;
    next.modified = now();
    double maxOrder = 0;
    for (auto& x : series_) if (x.worldId == worldId) maxOrder = std::max(maxOrder, x.order);
    next.order = nextOrder(maxOrder);
    if (!saveSeries(next)) { rollback(); return false; }
    *s = next;
    // Its books that lived in the old world come along.
    std::vector<Id> members;
    for (auto& m : memberships_) if (m.seriesId == seriesId) members.push_back(m.bookId);
    for (auto& bid : members) {
        Book* b = bookMut(bid);
        if (b && b->worldId == oldWorld && !moveBookToWorldTx(*b, worldId)) { rollback(); return false; }
    }
    return commit();
}

// ------------------------------------------------------------------- trash

bool Library::trashWorld(const Id& id) {
    World* w = worldMut(id);
    if (!w) return false;
    World next = *w;
    next.deleted = now();
    if (!begin()) return false;
    if (!saveWorld(next)) { rollback(); return false; }
    if (!commit()) return false;
    *w = next;
    return true;
}
bool Library::trashSeries(const Id& id) {
    Series* s = seriesMut(id);
    if (!s) return false;
    Series next = *s;
    next.deleted = now();
    if (!begin()) return false;
    if (!saveSeries(next)) { rollback(); return false; }
    if (!commit()) return false;
    *s = next;
    return true;
}
bool Library::trashBook(const Id& id) {
    Book* b = bookMut(id);
    if (!b) return false;
    Book next = *b;
    next.deleted = now();
    if (!begin()) return false;
    if (!saveBook(next)) { rollback(); return false; }
    if (!commit()) return false;
    *b = next;
    return true;
}

bool Library::restore(const Id& id) {
    if (World* w = worldMut(id)) {
        World next = *w;
        next.deleted = 0;
        if (!begin()) return false;
        if (!saveWorld(next)) { rollback(); return false; }
        if (!commit()) return false;
        *w = next;
        return true;
    }
    if (Series* s = seriesMut(id)) {
        Series next = *s;
        next.deleted = 0;
        // Its world is still in the Trash: bring the series back on its own.
        const World* w = next.worldId.empty() ? nullptr : world(next.worldId);
        if (w && w->deleted) next.worldId.clear();
        if (!begin()) return false;
        if (!saveSeries(next)) { rollback(); return false; }
        if (!commit()) return false;
        *s = next;
        return true;
    }
    if (Book* b = bookMut(id)) {
        if (!begin()) return false;
        const World* w = b->worldId.empty() ? nullptr : world(b->worldId);
        if (w && w->deleted && !moveBookToWorldTx(*b, {})) { rollback(); return false; }
        Book next = *b;
        next.deleted = 0;
        if (!saveBook(next)) { rollback(); return false; }
        if (!commit()) return false;
        *b = next;
        return true;
    }
    if (ManuscriptNode* n = nodeMut(id)) {
        // Restores the node and everything trashed with it (same time stamp).
        const Time when = n->deleted;
        std::vector<Id> sub;
        collectSubtree(id, sub);
        if (!begin()) return false;
        for (const Id& x : sub) {
            ManuscriptNode next = *nodeMut(x);
            if (next.deleted != when) continue;
            next.deleted = 0;
            if (x == id) {
                // Its parent is still in the Trash, or gone: bring it back at the top level.
                const ManuscriptNode* p = next.parentId.empty() ? nullptr : node(next.parentId);
                if (p && p->deleted) next.parentId.clear();
            }
            if (!saveNode(next)) { rollback(); return false; }
            *nodeMut(x) = next;
        }
        if (!updateBookWordCountTx(n->bookId) || !commit()) return false;
        return load();
    }
    if (const Artwork* a = artwork(id)) return !registerArtwork(a->blob, a->name, a->source).empty();
    if (Entry* e = entryMut(id)) {
        Entry next = *e;
        next.deleted = 0;
        // Its world or book is still in the Trash: it comes back with it, not on its own.
        if (!saveEntry(next)) return false;
        *e = next;
        return true;
    }
    error_ = "Nothing to restore.";
    return false;
}

Library::TrashCounts Library::trashCounts() const {
    TrashCounts c;
    for (auto& w : worlds_) if (w.deleted) ++c.worlds;
    for (auto& s : series_) if (s.deleted) ++c.series;
    for (auto& b : books_) if (b.deleted) ++c.books;
    c.entries = (int)trashedEntries().size();
    return c;
}

bool Library::emptyTrash() {
    // Worlds take their series and books with them (ON DELETE CASCADE); memberships follow.
    if (!begin()) return false;
    if (!exec("DELETE FROM nodes WHERE deleted != 0;"
              "DELETE FROM books WHERE deleted != 0 OR world_id IN (SELECT id FROM worlds WHERE deleted != 0);"
              "DELETE FROM series WHERE deleted != 0 OR world_id IN (SELECT id FROM worlds WHERE deleted != 0);"
              "DELETE FROM worlds WHERE deleted != 0;"
              "DELETE FROM artworks WHERE deleted != 0;"
              "DELETE FROM entries WHERE deleted != 0;")) {
        rollback();
        return false;
    }
    if (!commit()) return false;
    return load();
}

// ---------------------------------------------------------------- settings

std::string Library::setting(const std::string& key) const {
    Stmt q(db_, "SELECT value FROM meta WHERE key = ?1");
    q.bind(1, "setting:" + key);
    return q && q.row() ? q.text(0) : std::string();
}

bool Library::setSetting(const std::string& key, const std::string& value) {
    Stmt s(db_, "INSERT INTO meta(key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bind(1, "setting:" + key).bind(2, value);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

// ------------------------------------------------------------------- blobs

std::string Library::blobPath(const std::string& hash) const {
    if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos) return {};
    return fromPath(u8(folder_) / "objects" / hash.substr(0, 2) / hash.substr(2));
}

std::string Library::storeBlob(const std::string& bytes) {
    const std::string hash = sha256Hex(bytes);
    const std::string path = blobPath(hash);
    std::error_code ec;
    if (fs::exists(u8(path), ec)) return hash;  // already stored: identical content is kept once
    std::string err;
    if (!atomicWrite(path, bytes, err)) { error_ = err; return {}; }
    return hash;
}

// ------------------------------------------------------------ export, backup

bool Library::exportJson(const std::string& path, std::string& err) const {
    json j;
    j["format"] = "studio-library-export";
    j["formatVersion"] = 1;
    j["schemaVersion"] = kSchemaVersion;
    j["exportedUtc"] = now();
    j["note"] = "Library structure, book details and manuscript text. Images aren't included; they stay in the library's objects folder.";
    for (auto& w : worlds_)
        j["worlds"].push_back({{"id", w.id}, {"name", w.name}, {"description", w.description}, {"accent", w.accent},
                               {"notes", w.notes}, {"bannerBlob", w.bannerBlob}, {"logoBlob", w.logoBlob}, {"order", w.order},
                               {"created", w.created}, {"modified", w.modified}, {"deleted", w.deleted}});
    for (auto& s : series_)
        j["series"].push_back({{"id", s.id}, {"worldId", s.worldId}, {"name", s.name}, {"description", s.description},
                               {"status", toString(s.status)}, {"notes", s.notes}, {"coverBlob", s.coverBlob},
                               {"logoBlob", s.logoBlob}, {"order", s.order}, {"created", s.created},
                               {"modified", s.modified}, {"deleted", s.deleted}});
    for (auto& b : books_)
        j["books"].push_back({{"id", b.id}, {"worldId", b.worldId}, {"title", b.title}, {"subtitle", b.subtitle},
                              {"author", b.author}, {"penName", b.penName}, {"synopsis", b.synopsis}, {"genre", b.genre},
                              {"language", b.language}, {"isbn", b.isbn}, {"edition", b.edition},
                              {"status", toString(b.status)}, {"tags", b.tags}, {"coverBlob", b.coverBlob}, {"trim", b.trim},
                              {"wordCount", b.wordCount}, {"order", b.order}, {"created", b.created},
                              {"modified", b.modified}, {"deleted", b.deleted}});
    for (auto& m : memberships_)
        j["memberships"].push_back({{"seriesId", m.seriesId}, {"bookId", m.bookId}, {"readingOrder", m.readingOrder},
                                    {"chronoOrder", m.chronoOrder}, {"number", m.number}, {"kind", toString(m.kind)}});
    for (auto& n : nodes_) {
        json c = json::parse(nodeContent(n.id), nullptr, false);
        j["nodes"].push_back({{"id", n.id}, {"bookId", n.bookId}, {"parentId", n.parentId}, {"kind", toString(n.kind)},
                              {"title", n.title}, {"synopsis", n.synopsis}, {"status", toString(n.status)},
                              {"order", n.order}, {"wordCount", n.wordCount}, {"created", n.created},
                              {"modified", n.modified}, {"deleted", n.deleted}, {"povId", n.povId}, {"targetWords", n.targetWords},
                              {"content", c.is_discarded() ? json() : c}});
    }
    for (auto& a : artworks_)
        j["artworks"].push_back({{"id", a.id}, {"blob", a.blob}, {"name", a.name}, {"kind", a.kind}, {"width", a.width},
                                 {"height", a.height}, {"format", a.format}, {"source", a.source}, {"notes", a.notes},
                                 {"created", a.created}, {"modified", a.modified}, {"deleted", a.deleted}});
    for (auto& e : editions_)
        j["editions"].push_back({{"id", e.id}, {"bookId", e.bookId}, {"kind", e.kind}, {"name", e.name}, {"isbn", e.isbn},
                                 {"trim", e.trim}, {"design", e.design}, {"paper", e.paper}, {"bleed", e.bleed},
                                 {"colour", e.colour}, {"order", e.order}, {"created", e.created}, {"modified", e.modified}});
    for (auto& e : entries_)
        j["entries"].push_back({{"id", e.id}, {"worldId", e.worldId}, {"bookId", e.bookId}, {"kind", e.kind}, {"name", e.name},
                                {"aliases", e.aliases}, {"summary", e.summary}, {"details", e.details}, {"notes", e.notes},
                                {"imageBlob", e.imageBlob}, {"order", e.order}, {"created", e.created}, {"modified", e.modified},
                                {"deleted", e.deleted}});
    for (auto& l : links_) j["entryLinks"].push_back({{"from", l.from}, {"to", l.to}, {"label", l.label}});
    for (auto& c : comments_)
        j["comments"].push_back({{"id", c.id}, {"nodeId", c.nodeId}, {"body", c.body}, {"quote", c.quote}, {"created", c.created},
                                 {"modified", c.modified}, {"resolved", c.resolved}});
    for (const char* k : {"worlds", "series", "books", "memberships", "nodes", "artworks", "editions", "entries", "entryLinks", "comments"})
        if (!j.contains(k)) j[k] = json::array();
    return atomicWrite(path, j.dump(2), err);
}

bool Library::backup(std::string& err, int keep) {
    const fs::path dir = u8(folder_) / "backups";
    std::error_code ec;
    fs::create_directories(dir, ec);
    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    fs::path target = dir / ("library-" + std::string(stamp) + ".db");
    for (int n = 2; fs::exists(target, ec); ++n) target = dir / ("library-" + std::string(stamp) + "-" + std::to_string(n) + ".db");
    // VACUUM INTO writes a complete, consistent copy, even while the library is open.
    Stmt s(db_, "VACUUM INTO ?1");
    s.bind(1, fromPath(target));
    if (!s || !s.run()) { err = sqlite3_errmsg(db_); return false; }
    // Keep the newest `keep` copies.
    std::vector<fs::path> all;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".db" && e.path().filename().string().rfind("library-", 0) == 0) all.push_back(e.path());
    std::sort(all.begin(), all.end());
    while ((int)all.size() > keep) { fs::remove(all.front(), ec); all.erase(all.begin()); }
    return true;
}

}  // namespace studio

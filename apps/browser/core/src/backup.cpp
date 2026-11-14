#include "nrb/backup.hpp"

#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include "miniz.h"

#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace nrb::backup {

namespace {

constexpr int kVersion = 1;
constexpr const char* kApp = "NorthRay Browser";
constexpr std::uint64_t kMostBytes = 2048ull * 1024 * 1024;   // a backup bigger than this isn't one of ours

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p.c_str(), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto n = in.tellg();
    if (n < 0 || (std::uint64_t)n > kMostBytes) return false;
    out.resize((std::size_t)n);
    in.seekg(0);
    if (n > 0) in.read(out.data(), n);
    return (bool)in;
}

bool writeFile(const fs::path& p, const void* data, std::size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(static_cast<const char*>(data), (std::streamsize)n);
    out.close();
    return !out.fail();
}

// Database files that belong together: moving one means moving its journal too.
const char* kSidecars[] = {"-wal", "-shm", "-journal"};

std::string u8(const fs::path& p) {
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

}  // namespace

bool snapshotDatabase(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::remove(to, ec);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(u8(from).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = db ? sqlite3_errmsg(db) : "couldn't open";
        sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 3000);
    char* msg = nullptr;
    const std::string target = u8(to);
    std::string sql = "VACUUM INTO '";
    for (char c : target) sql += c == '\'' ? std::string("''") : std::string(1, c);
    sql += "'";
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) error = msg ? msg : "the copy failed";
    sqlite3_free(msg);
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

bool checkDatabase(const fs::path& file, const std::string& table, std::string& error) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(u8(file).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = "it couldn't be opened";
        sqlite3_close(db);
        return false;
    }
    bool ok = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        const char* r = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        ok = r && std::string(r) == "ok";
    }
    sqlite3_finalize(st);
    if (!ok) error = "it's damaged";
    if (ok) {
        ok = false;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, table.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(st) == SQLITE_ROW;
        }
        sqlite3_finalize(st);
        if (!ok) error = "it isn't the right kind of database";
    }
    sqlite3_close(db);
    return ok;
}

bool allowedEntry(const std::string& name) {
    auto plainName = [](const std::string& n) {
        if (n.empty() || n == "." || n == ".." || n.size() > 200) return false;
        for (char c : n)
            if (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 32) return false;
        return n[0] != '.';
    };
    if (name == "manifest.json" || name == "settings.json") return true;
    if (name == "User/bookmarks.db" || name == "User/history.db" || name == "User/references.db" || name == "User/start-notes.txt") return true;
    const std::string bg = "User/start-background-", refs = "User/references/";
    if (name.compare(0, bg.size(), bg) == 0) return plainName(name.substr(5));
    if (name.compare(0, refs.size(), refs) == 0) return plainName(name.substr(refs.size()));
    return false;
}

bool make(const fs::path& stateDir, const fs::path& out, const Options& options, std::string& error, Manifest* madeOut) {
    std::error_code ec;
    const fs::path user = stateDir / "User";
    const fs::path work = stateDir / "Backup-work";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);

    struct Item { std::string name; fs::path file; };
    std::vector<Item> items;
    if (fs::exists(stateDir / "settings.json")) items.push_back({"settings.json", stateDir / "settings.json"});
    const char* dbs[] = {"bookmarks.db", "history.db", "references.db"};
    for (const char* db : dbs) {
        if (std::string(db) == "history.db" && !options.history) continue;
        if (!fs::exists(user / db)) continue;
        if (!snapshotDatabase(user / db, work / db, error)) {
            error = std::string(db) + " couldn't be copied: " + error;
            fs::remove_all(work, ec);
            return false;
        }
        items.push_back({std::string("User/") + db, work / db});
    }
    if (fs::exists(user / "start-notes.txt")) items.push_back({"User/start-notes.txt", user / "start-notes.txt"});
    if (fs::is_directory(user, ec))
        for (const auto& e : fs::directory_iterator(user, ec)) {
            const std::string n = u8(e.path().filename());
            if (e.is_regular_file() && n.rfind("start-background-", 0) == 0 && allowedEntry("User/" + n)) items.push_back({"User/" + n, e.path()});
        }
    if (fs::is_directory(user / "references", ec))
        for (const auto& e : fs::directory_iterator(user / "references", ec)) {
            const std::string n = u8(e.path().filename());
            if (e.is_regular_file() && allowedEntry("User/references/" + n)) items.push_back({"User/references/" + n, e.path()});
        }

    Manifest m;
    m.version = kVersion;
    m.app = kApp;
    m.kind = "backup";
    m.appVersion = options.appVersion;
    m.made = options.made;
    m.history = options.history && fs::exists(work / "history.db");
    m.files = (int)items.size();
    nlohmann::json j = {{"version", m.version}, {"app", m.app},         {"kind", m.kind},  {"appVersion", m.appVersion},
                        {"made", m.made},       {"history", m.history}, {"files", m.files}};
    const std::string manifest = j.dump(1);

    mz_zip_archive z{};
    if (!mz_zip_writer_init_heap(&z, 0, 1 << 20)) { error = "out of memory"; return false; }
    bool ok = mz_zip_writer_add_mem(&z, "manifest.json", manifest.data(), manifest.size(), MZ_BEST_COMPRESSION);
    std::uint64_t total = 0;
    for (const Item& it : items) {
        std::string data;
        if (!readFile(it.file, data)) { error = it.name + " couldn't be read"; ok = false; break; }
        total += data.size();
        if (total > kMostBytes) { error = "it would be bigger than 2 GB"; ok = false; break; }
        // Pictures are compressed already.
        const bool picture = it.name.rfind("User/references/", 0) == 0 || it.name.rfind("User/start-background-", 0) == 0;
        if (!mz_zip_writer_add_mem(&z, it.name.c_str(), data.data(), data.size(), picture ? MZ_NO_COMPRESSION : MZ_DEFAULT_COMPRESSION)) {
            error = it.name + " couldn't be added";
            ok = false;
            break;
        }
    }
    void* buf = nullptr;
    size_t size = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&z, &buf, &size);
    if (ok) {
        const fs::path tmp = out.string() + ".part";
        ok = writeFile(tmp, buf, size);
        if (ok) {
            fs::rename(tmp, out, ec);
            if (ec) { fs::remove(out, ec); fs::rename(tmp, out, ec); }
            ok = !ec;
        }
        if (!ok) { error = "the file couldn't be saved there"; fs::remove(tmp, ec); }
    }
    if (buf) mz_free(buf);
    mz_zip_writer_end(&z);
    fs::remove_all(work, ec);
    if (ok && madeOut) *madeOut = m;
    return ok;
}

bool unpack(const fs::path& zipFile, const fs::path& stateDir, Manifest& m, std::string& error) {
    std::string bytes;
    if (!readFile(zipFile, bytes)) { error = "The file couldn't be read."; return false; }
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, bytes.data(), bytes.size(), 0)) { error = "That isn't a NorthRay Browser backup (it isn't a zip file)."; return false; }
    bool ok = false;
    const fs::path staging = stateDir / "Restore";
    std::error_code ec;
    do {
        size_t n = 0;
        const int mi = mz_zip_reader_locate_file(&z, "manifest.json", nullptr, 0);
        void* p = mi >= 0 ? mz_zip_reader_extract_to_heap(&z, (mz_uint)mi, &n, 0) : nullptr;
        if (!p) { error = "That isn't a NorthRay Browser backup."; break; }
        const nlohmann::json j = nlohmann::json::parse(std::string(static_cast<char*>(p), n), nullptr, false);
        mz_free(p);
        if (j.is_discarded() || !j.is_object() || j.value("app", "") != kApp || j.value("kind", "") != "backup") {
            error = "That isn't a NorthRay Browser backup.";
            break;
        }
        m.version = j.value("version", 0);
        m.app = kApp;
        m.kind = "backup";
        m.appVersion = j.value("appVersion", "");
        m.made = j.value("made", "");
        m.history = j.value("history", false);
        m.files = j.value("files", 0);
        if (m.version < 1 || m.version > kVersion) { error = "That backup was made by a newer NorthRay Browser. Update this one first."; break; }

        fs::remove_all(staging, ec);
        fs::create_directories(staging, ec);
        bool all = true;
        const mz_uint count = mz_zip_reader_get_num_files(&z);
        for (mz_uint i = 0; i < count && all; ++i) {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory) continue;
            const std::string name = st.m_filename;
            if (name == "manifest.json" || !allowedEntry(name)) continue;   // anything else isn't ours: left out
            if (st.m_uncomp_size > kMostBytes) { all = false; error = "Part of that backup is too big to be real."; break; }
            size_t got = 0;
            void* data = mz_zip_reader_extract_to_heap(&z, i, &got, 0);
            if (!data) { all = false; error = "That backup is damaged (" + name + " couldn't be read)."; break; }
            const bool written = writeFile(staging / fs::path(name), data, got);
            mz_free(data);
            if (!written) { all = false; error = "The backup couldn't be unpacked (there may not be enough space)."; }
        }
        if (!all) break;
        // The databases must open and be what they say they are.
        struct Db { const char* name; const char* table; };
        const Db dbs[] = {{"bookmarks.db", "bookmarks"}, {"history.db", "visits"}, {"references.db", "boards"}};
        for (const Db& d : dbs) {
            const fs::path f = staging / "User" / d.name;
            std::string why;
            if (fs::exists(f) && !checkDatabase(f, d.table, why)) {
                all = false;
                error = std::string("That backup's ") + d.name + " can't be used: " + why + ".";
                break;
            }
        }
        if (!all) break;
        std::ofstream ready((staging / "ready").c_str(), std::ios::binary);
        ready << j.dump();
        ok = (bool)ready;
        if (!ok) error = "The backup couldn't be unpacked.";
    } while (false);
    mz_zip_reader_end(&z);
    if (!ok) fs::remove_all(staging, ec);
    return ok;
}

bool pending(const fs::path& stateDir) { return fs::exists(stateDir / "Restore" / "ready"); }

void cancelPending(const fs::path& stateDir) {
    std::error_code ec;
    fs::remove_all(stateDir / "Restore", ec);
}

bool applyPending(const fs::path& stateDir, std::string& error, fs::path* keptIn, const std::string& stampIn) {
    if (!pending(stateDir)) return true;
    std::error_code ec;
    const fs::path staging = stateDir / "Restore";
    const std::string stamp = stampIn.empty() ? "earlier" : stampIn;
    fs::path keep = stateDir / ("Before restore " + stamp);
    for (int k = 2; fs::exists(keep) && k < 100; ++k) keep = stateDir / ("Before restore " + stamp + " (" + std::to_string(k) + ")");

    // What's replaced: settings.json, each file in User, and the references folder as a whole.
    std::vector<fs::path> units;   // relative to stateDir
    if (fs::exists(staging / "settings.json")) units.push_back("settings.json");
    if (fs::is_directory(staging / "User", ec))
        for (const auto& e : fs::directory_iterator(staging / "User", ec)) units.push_back(fs::path("User") / e.path().filename());

    struct Move { fs::path from, to; };
    std::vector<Move> done;
    auto move = [&](const fs::path& from, const fs::path& to) {
        fs::create_directories(to.parent_path(), ec);
        fs::rename(from, to, ec);
        if (ec) return false;
        done.push_back({from, to});
        return true;
    };
    bool ok = true;
    for (const fs::path& u : units) {
        // The current one (and its journal files) step aside...
        std::vector<fs::path> current = {u};
        for (const char* s : kSidecars) current.push_back(u.string() + s);
        for (const fs::path& c : current)
            if (fs::exists(stateDir / c) && !move(stateDir / c, keep / c)) { ok = false; break; }
        if (!ok) break;
        // ...and the backup's takes its place.
        if (!move(staging / u, stateDir / u)) { ok = false; break; }
    }
    if (!ok) {
        // Put everything back as it was.
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            std::error_code e2;
            fs::rename(it->to, it->from, e2);
        }
        error = "The restore couldn't be finished, so nothing was changed.";
        return false;
    }
    fs::remove_all(staging, ec);
    if (keptIn) *keptIn = fs::exists(keep) ? keep : fs::path();
    return true;
}

}  // namespace nrb::backup

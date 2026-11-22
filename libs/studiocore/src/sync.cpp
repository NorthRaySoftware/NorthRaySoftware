// Keeping a world in a repository (0.61). See sync.hpp.
#include "studio/sync.hpp"
#include "studio/document.hpp"
#include "studio/fonts.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <ctime>

#include <nlohmann/json.hpp>

namespace studio {

using nlohmann::json;
using detail::Stmt;
using detail::u8path;
using detail::fromPath;
namespace fs = std::filesystem;

namespace {

// One row as {column: value}, keys sorted, so the same row always gives the same bytes.
json rowJson(sqlite3_stmt* s) {
    json j = json::object();
    const int n = sqlite3_column_count(s);
    for (int i = 0; i < n; ++i) {
        const char* name = sqlite3_column_name(s, i);
        switch (sqlite3_column_type(s, i)) {
            case SQLITE_INTEGER: j[name] = (std::int64_t)sqlite3_column_int64(s, i); break;
            case SQLITE_FLOAT: j[name] = sqlite3_column_double(s, i); break;
            case SQLITE_NULL: j[name] = nullptr; break;
            default: {
                const unsigned char* t = sqlite3_column_text(s, i);
                j[name] = t ? std::string(reinterpret_cast<const char*>(t), (size_t)sqlite3_column_bytes(s, i)) : std::string();
            }
        }
    }
    return j;
}

std::string bytesOf(const json& j) { return j.dump(1, ' ', false, json::error_handler_t::replace) + "\n"; }

bool isHex64(const std::string& s) { return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos; }

// Every 64-hex-digit run in the text (pictures are named by their SHA-256).
void hashesIn(const std::string& text, std::set<std::string>& out) {
    size_t i = 0;
    auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    while (i < text.size()) {
        if (!hex(text[i])) { ++i; continue; }
        size_t j = i;
        while (j < text.size() && hex(text[j])) ++j;
        if (j - i == 64) out.insert(text.substr(i, 64));
        i = j;
    }
}

std::string settingFileName(const std::string& key) {
    std::string out;
    static const char* digits = "0123456789abcdef";
    for (unsigned char c : key) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_') out += (char)c;
        else { out += '~'; out += digits[c >> 4]; out += digits[c & 15]; }
    }
    if (out.size() > 180) out = out.substr(0, 140) + "~" + sha256Hex(key).substr(0, 16);
    return out;
}

std::string readAll(const std::string& path) {
    std::ifstream f(u8path(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Columns {
    std::vector<std::string> names, pk;
};
Columns columnsOf(sqlite3* db, const char* table) {
    Columns c;
    Stmt q(db, (std::string("PRAGMA table_info(") + table + ")").c_str());
    while (q.row()) {
        c.names.push_back(q.text(1));
        if (q.i64(5) > 0) c.pk.push_back(q.text(1));
    }
    return c;
}

bool bindJson(sqlite3_stmt* s, int i, const json& v) {
    if (v.is_null()) return sqlite3_bind_null(s, i) == SQLITE_OK;
    if (v.is_boolean()) return sqlite3_bind_int64(s, i, v.get<bool>() ? 1 : 0) == SQLITE_OK;
    if (v.is_number_integer() || v.is_number_unsigned()) return sqlite3_bind_int64(s, i, v.get<std::int64_t>()) == SQLITE_OK;
    if (v.is_number_float()) return sqlite3_bind_double(s, i, v.get<double>()) == SQLITE_OK;
    if (v.is_string()) {
        const std::string& t = v.get_ref<const std::string&>();
        return sqlite3_bind_text(s, i, t.c_str(), (int)t.size(), SQLITE_TRANSIENT) == SQLITE_OK;
    }
    return false;
}

// Inserts the row, or updates it in place (never delete-and-insert, which would take its children with it).
bool upsert(sqlite3* db, const char* table, const json& row, std::string& error) {
    if (!row.is_object()) { error = std::string("A ") + table + " file isn't a row."; return false; }
    const Columns c = columnsOf(db, table);
    std::vector<std::string> cols;
    for (auto& n : c.names) if (row.contains(n)) cols.push_back(n);
    for (auto& k : c.pk) if (!row.contains(k)) { error = std::string("A ") + table + " row has no " + k + "."; return false; }
    std::string sql = std::string("INSERT INTO ") + table + " (";
    for (size_t i = 0; i < cols.size(); ++i) sql += (i ? "," : "") + std::string("\"") + cols[i] + "\"";
    sql += ") VALUES (";
    for (size_t i = 0; i < cols.size(); ++i) sql += (i ? ",?" : "?");
    sql += ")";
    std::string set;
    for (auto& n : cols)
        if (std::find(c.pk.begin(), c.pk.end(), n) == c.pk.end()) set += (set.empty() ? "" : ",") + std::string("\"") + n + "\"=excluded.\"" + n + "\"";
    sql += " ON CONFLICT(";
    for (size_t i = 0; i < c.pk.size(); ++i) sql += (i ? "," : "") + std::string("\"") + c.pk[i] + "\"";
    sql += set.empty() ? ") DO NOTHING" : ") DO UPDATE SET " + set;
    Stmt s(db, sql.c_str());
    if (!s) { error = sqlite3_errmsg(db); return false; }
    for (size_t i = 0; i < cols.size(); ++i)
        if (!bindJson(s.s, (int)i + 1, row[cols[i]])) { error = std::string("A ") + table + " row has an odd value in " + cols[i] + "."; return false; }
    if (!s.run()) { error = sqlite3_errmsg(db); return false; }
    return true;
}

std::string str(const json& j, const char* k) { return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); }

// Splits "a/b/c" into its parts.
std::vector<std::string> parts(const std::string& p) {
    std::vector<std::string> out;
    size_t at = 0;
    while (true) {
        const size_t s = p.find('/', at);
        out.push_back(p.substr(at, s == std::string::npos ? std::string::npos : s - at));
        if (s == std::string::npos) break;
        at = s + 1;
    }
    return out;
}

bool endsWith(const std::string& s, const char* e) {
    const size_t n = std::strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}
std::string stem(const std::string& file) { return endsWith(file, ".json") ? file.substr(0, file.size() - 5) : file; }

}  // namespace

// ---------------------------------------------------------------------------------------- a world as files

std::map<std::string, Library::WorldFile> Library::worldFiles(const Id& w) const {
    std::map<std::string, WorldFile> out;
    if (w.empty()) return out;
    auto put = [&](const std::string& path, const json& j) { out[path].bytes = bytesOf(j); };
    std::vector<Id> books, series, nodes, entries;
    {
        Stmt q(db_, "SELECT * FROM worlds WHERE id = ?1");
        q.bind(1, w);
        if (!q.row()) return out;
        put("world.json", rowJson(q.s));
    }
    {
        Stmt q(db_, "SELECT * FROM series WHERE world_id = ?1");
        q.bind(1, w);
        while (q.row()) { json j = rowJson(q.s); series.push_back(str(j, "id")); put("series/" + series.back() + ".json", j); }
    }
    {
        Stmt q(db_, "SELECT * FROM books WHERE world_id = ?1");
        q.bind(1, w);
        while (q.row()) { json j = rowJson(q.s); books.push_back(str(j, "id")); put("books/" + books.back() + "/book.json", j); }
    }
    {
        Stmt q(db_, "SELECT m.* FROM memberships m WHERE m.series_id IN (SELECT id FROM series WHERE world_id = ?1) "
                    "AND m.book_id IN (SELECT id FROM books WHERE world_id = ?1)");
        q.bind(1, w);
        while (q.row()) { json j = rowJson(q.s); put("memberships/" + str(j, "series_id") + "+" + str(j, "book_id") + ".json", j); }
    }
    for (const Id& b : books) {
        {
            Stmt q(db_, "SELECT * FROM nodes WHERE book_id = ?1");
            q.bind(1, b);
            while (q.row()) { json j = rowJson(q.s); nodes.push_back(str(j, "id")); put("books/" + b + "/nodes/" + nodes.back() + ".json", j); }
        }
        {
            Stmt q(db_, "SELECT * FROM editions WHERE book_id = ?1");
            q.bind(1, b);
            while (q.row()) { json j = rowJson(q.s); put("books/" + b + "/editions/" + str(j, "id") + ".json", j); }
        }
        {
            Stmt q(db_, "SELECT * FROM snapshots WHERE book_id = ?1");
            q.bind(1, b);
            while (q.row()) {
                json j = rowJson(q.s);
                json ns = json::array();
                Stmt n(db_, "SELECT * FROM snapshot_nodes WHERE snapshot_id = ?1 ORDER BY position, node_id");
                n.bind(1, str(j, "id"));
                while (n.row()) ns.push_back(rowJson(n.s));
                j["nodes"] = ns;
                put("books/" + b + "/snapshots/" + str(j, "id") + ".json", j);
            }
        }
        {
            Stmt q(db_, "SELECT c.* FROM comments c JOIN nodes n ON n.id = c.node_id WHERE n.book_id = ?1");
            q.bind(1, b);
            while (q.row()) { json j = rowJson(q.s); put("books/" + b + "/comments/" + str(j, "id") + ".json", j); }
        }
        {
            Stmt q(db_, "SELECT * FROM writing_log WHERE book_id = ?1 ORDER BY day");
            q.bind(1, b);
            json days = json::array();
            while (q.row()) days.push_back(rowJson(q.s));
            if (!days.empty()) put("books/" + b + "/log.json", days);
        }
    }
    {
        Stmt q(db_, "SELECT * FROM entries WHERE world_id = ?1");
        q.bind(1, w);
        while (q.row()) { json j = rowJson(q.s); entries.push_back(str(j, "id")); put("entries/" + entries.back() + ".json", j); }
    }
    for (const Id& e : entries) {
        Stmt q(db_, "SELECT to_id, label FROM entry_links WHERE from_id = ?1 ORDER BY to_id, label");
        q.bind(1, e);
        json links = json::array();
        while (q.row()) links.push_back({{"to", q.text(0)}, {"label", q.text(1)}});
        if (!links.empty()) put("links/" + e + ".json", links);
    }
    {
        // Settings naming the world, one of its books or series (boards, plans, typesetting...); never the sync state.
        std::vector<Id> ids = books;
        ids.push_back(w);
        ids.insert(ids.end(), series.begin(), series.end());
        Stmt q(db_, "SELECT key, value FROM meta WHERE key LIKE 'setting:%'");
        while (q.row()) {
            const std::string key = q.text(0);
            if (key.rfind("setting:sync:", 0) == 0) continue;
            if (std::none_of(ids.begin(), ids.end(), [&](const Id& id) { return !id.empty() && key.find(id) != std::string::npos; })) continue;
            put("settings/" + settingFileName(key.substr(8)) + ".json", json{{"key", key.substr(8)}, {"value", q.text(1)}});
        }
    }
    // The pictures all this names, and the artwork rows for them.
    std::set<std::string> hashes;
    for (auto& [p, f] : out) hashesIn(f.bytes, hashes);
    {
        Stmt q(db_, "SELECT * FROM artworks");
        while (q.row()) {
            json j = rowJson(q.s);
            if (hashes.count(str(j, "blob"))) put("artworks/" + str(j, "id") + ".json", j);
        }
    }
    for (const std::string& h : hashes) {
        const std::string path = blobPath(h);
        std::error_code ec;
        if (!path.empty() && fs::is_regular_file(u8path(path), ec)) out["objects/" + h.substr(0, 2) + "/" + h.substr(2)].diskPath = path;
    }
    // The world's own fonts.
    const std::string fontDir = fonts::scopeFolder(folder_, w, {});
    std::error_code ec;
    if (!fontDir.empty() && fs::is_directory(u8path(fontDir), ec))
        for (auto& f : fs::recursive_directory_iterator(u8path(fontDir), ec)) {
            if (!f.is_regular_file(ec)) continue;
            const std::string rel = fromPath(fs::relative(f.path(), u8path(fontDir), ec));
            std::string r = rel;
            std::replace(r.begin(), r.end(), '\\', '/');
            if (sync::validPath("fonts/" + r)) out["fonts/" + r].diskPath = fromPath(f.path());
        }
    return out;
}

bool Library::reload() {
    const bool ok = load();
    entryAtSize_ = (std::size_t)-1;
    linksAtValid_ = false;
    appearanceCache_.clear();
    return ok;
}

bool Library::applyWorldFiles(const Id& w, const std::map<std::string, std::optional<std::string>>& changes,
                              const std::vector<std::pair<Id, std::string>>& keepVersions, std::string& error) {
    if (w.empty()) { error = "No world."; return false; }
    // Files outside the database first: pictures (checked against their names) and fonts.
    const std::string fontDir = fonts::scopeFolder(folder_, w, {});
    for (auto& [path, bytes] : changes) {
        if (!sync::validPath(path)) continue;
        if (path.rfind("objects/", 0) == 0) {
            if (!bytes) continue;   // a picture may be used elsewhere: never removed
            const std::string h = path.substr(8, 2) + path.substr(11);
            if (sha256Hex(*bytes) != h) { error = "A picture from the repository isn't what its name says."; return false; }
            if (storeBlob(*bytes).empty()) { error = error_; return false; }
        } else if (path.rfind("fonts/", 0) == 0) {
            const fs::path target = u8path(fontDir) / u8path(path.substr(6));
            std::error_code ec;
            if (!bytes) { fs::remove(target, ec); continue; }
            fs::create_directories(target.parent_path(), ec);
            std::string err;
            if (!atomicWrite(fromPath(target), *bytes, err)) { error = err; return false; }
        }
    }

    if (!begin()) { error = error_; return false; }
    exec("PRAGMA defer_foreign_keys = ON");
    auto fail = [&](const std::string& why) { rollback(); error = why; return false; };
    auto bookInWorld = [&](const Id& b) {
        Stmt q(db_, "SELECT world_id FROM books WHERE id = ?1");
        q.bind(1, b);
        return q.row() && q.text(0) == w;
    };
    auto entryInWorld = [&](const Id& e) {
        Stmt q(db_, "SELECT world_id FROM entries WHERE id = ?1");
        q.bind(1, e);
        return q.row() && q.text(0) == w;
    };
    std::vector<Id> worldIds = {w};
    // Pass 1: the world, series, books; pass 2: what hangs off books; pass 3: entries and their links, settings.
    for (int pass = 1; pass <= 3; ++pass) {
        for (auto& [path, bytes] : changes) {
            if (!bytes || !sync::validPath(path) || path.rfind("objects/", 0) == 0 || path.rfind("fonts/", 0) == 0) continue;
            const auto p = parts(path);
            const json j = json::parse(*bytes, nullptr, false);
            if (j.is_discarded()) return fail("The file " + path + " in the repository isn't readable.");
            const std::string top = p[0];
            if (pass == 1) {
                if (path == "world.json") {
                    if (str(j, "id") != w) return fail("The repository holds a different world.");
                    if (!upsert(db_, "worlds", j, error)) return fail(error);
                } else if (top == "series" && p.size() == 2) {
                    if (str(j, "id") != stem(p[1]) || str(j, "world_id") != w) return fail("A series file belongs to another world.");
                    if (!upsert(db_, "series", j, error)) return fail(error);
                } else if (top == "books" && p.size() == 3 && p[2] == "book.json") {
                    if (str(j, "id") != p[1] || str(j, "world_id") != w) return fail("A book file belongs to another world.");
                    if (!upsert(db_, "books", j, error)) return fail(error);
                }
            } else if (pass == 2) {
                if (top == "memberships" && p.size() == 2) {
                    if (!bookInWorld(str(j, "book_id"))) return fail("A series entry names a book from another world.");
                    if (!upsert(db_, "memberships", j, error)) return fail(error);
                } else if (top == "books" && p.size() == 4 && bookInWorld(p[1])) {
                    const std::string& b = p[1];
                    if (p[2] == "nodes" || p[2] == "editions" || p[2] == "snapshots") {
                        if (str(j, "id") != stem(p[3]) || str(j, "book_id") != b) return fail("A file in " + path + " belongs elsewhere.");
                        if (p[2] == "snapshots") {
                            json row = j;
                            row.erase("nodes");
                            if (!upsert(db_, "snapshots", row, error)) return fail(error);
                            Stmt d(db_, "DELETE FROM snapshot_nodes WHERE snapshot_id = ?1");
                            d.bind(1, str(j, "id"));
                            d.run();
                            if (j.contains("nodes") && j["nodes"].is_array())
                                for (const json& n : j["nodes"]) {
                                    if (str(n, "snapshot_id") != str(j, "id")) continue;
                                    if (!upsert(db_, "snapshot_nodes", n, error)) return fail(error);
                                }
                        } else if (!upsert(db_, p[2] == "nodes" ? "nodes" : "editions", j, error)) return fail(error);
                    } else if (p[2] == "comments") {
                        if (str(j, "id") != stem(p[3])) return fail("A comment file is misnamed.");
                        Stmt q(db_, "SELECT book_id FROM nodes WHERE id = ?1");
                        q.bind(1, str(j, "node_id"));
                        if (!q.row() || q.text(0) != b) continue;   // its chapter isn't here (yet): left for the next sync
                        if (!upsert(db_, "comments", j, error)) return fail(error);
                    }
                } else if (top == "books" && p.size() == 3 && p[2] == "log.json" && bookInWorld(p[1]) && j.is_array()) {
                    Stmt d(db_, "DELETE FROM writing_log WHERE book_id = ?1");
                    d.bind(1, p[1]);
                    d.run();
                    for (const json& day : j)
                        if (str(day, "book_id") == p[1] && !upsert(db_, "writing_log", day, error)) return fail(error);
                } else if (top == "entries" && p.size() == 2) {
                    if (str(j, "id") != stem(p[1]) || str(j, "world_id") != w) return fail("An entry file belongs to another world.");
                    if (!upsert(db_, "entries", j, error)) return fail(error);
                } else if (top == "artworks" && p.size() == 2) {
                    if (str(j, "id") != stem(p[1]) || !isHex64(str(j, "blob"))) return fail("An artwork file is misnamed.");
                    // One row per picture: a picture already listed here under another id keeps its row.
                    Stmt q(db_, "SELECT id FROM artworks WHERE blob = ?1");
                    q.bind(1, str(j, "blob"));
                    if (q.row() && q.text(0) != str(j, "id")) continue;
                    if (!upsert(db_, "artworks", j, error)) return fail(error);
                }
            } else {
                if (top == "links" && p.size() == 2 && j.is_array()) {
                    const Id from = stem(p[1]);
                    if (!entryInWorld(from)) continue;
                    Stmt d(db_, "DELETE FROM entry_links WHERE from_id = ?1");
                    d.bind(1, from);
                    d.run();
                    for (const json& l : j) {
                        const std::string to = str(l, "to");
                        if (!entryInWorld(to)) continue;   // linked to an entry not here: dropped rather than broken
                        Stmt s(db_, "INSERT OR IGNORE INTO entry_links(from_id, to_id, label) VALUES (?1, ?2, ?3)");
                        s.bind(1, from).bind(2, to).bind(3, str(l, "label"));
                        if (!s.run()) return fail(sqlite3_errmsg(db_));
                    }
                } else if (top == "settings" && p.size() == 2) {
                    const std::string key = str(j, "key");
                    if (key.empty() || key.rfind("sync:", 0) == 0 || settingFileName(key) + ".json" != p[1]) continue;
                    bool ours = key.find(w) != std::string::npos;
                    if (!ours) {
                        Stmt q(db_, "SELECT id FROM books WHERE world_id = ?1 UNION SELECT id FROM series WHERE world_id = ?1");
                        q.bind(1, w);
                        while (!ours && q.row()) ours = !q.text(0).empty() && key.find(q.text(0)) != std::string::npos;
                    }
                    if (!ours) continue;
                    Stmt s(db_, "INSERT INTO meta(key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
                    s.bind(1, "setting:" + key).bind(2, str(j, "value"));
                    if (!s.run()) return fail(sqlite3_errmsg(db_));
                }
            }
        }
    }
    // Things gone from the other computer (children first). The world itself is never removed this way.
    auto removeRow = [&](const char* sql, const std::string& id) {
        Stmt d(db_, sql);
        d.bind(1, id);
        return d.run();
    };
    for (int pass = 1; pass <= 2; ++pass)
        for (auto& [path, bytes] : changes) {
            if (bytes || !sync::validPath(path)) continue;
            const auto p = parts(path);
            if (pass == 1) {
                if (p[0] == "links" && p.size() == 2 && entryInWorld(stem(p[1]))) removeRow("DELETE FROM entry_links WHERE from_id = ?1", stem(p[1]));
                else if (p[0] == "books" && p.size() == 4 && bookInWorld(p[1])) {
                    const Id id = stem(p[3]);
                    if (p[2] == "comments") removeRow("DELETE FROM comments WHERE id = ?1", id);
                    else if (p[2] == "snapshots") removeRow("DELETE FROM snapshots WHERE id = ?1 AND book_id IN (SELECT id FROM books)", id);
                    else if (p[2] == "editions") removeRow("DELETE FROM editions WHERE id = ?1", id);
                    else if (p[2] == "nodes") {
                        Stmt d(db_, "DELETE FROM nodes WHERE id = ?1 AND book_id = ?2");
                        d.bind(1, id).bind(2, p[1]);
                        d.run();
                    }
                } else if (p[0] == "books" && p.size() == 3 && p[2] == "log.json" && bookInWorld(p[1]))
                    removeRow("DELETE FROM writing_log WHERE book_id = ?1", p[1]);
                else if (p[0] == "memberships" && p.size() == 2) {
                    const std::string s = stem(p[1]);
                    const size_t plus = s.find('+');
                    if (plus != std::string::npos && bookInWorld(s.substr(plus + 1))) {
                        Stmt d(db_, "DELETE FROM memberships WHERE series_id = ?1 AND book_id = ?2");
                        d.bind(1, s.substr(0, plus)).bind(2, s.substr(plus + 1));
                        d.run();
                    }
                } else if (p[0] == "settings" && p.size() == 2) {
                    // Found by its file name: the setting's own key is in the file that's gone.
                    Stmt q(db_, "SELECT key FROM meta WHERE key LIKE 'setting:%'");
                    std::vector<std::string> gone;
                    while (q.row()) if (settingFileName(q.text(0).substr(8)) + ".json" == p[1] && q.text(0).rfind("setting:sync:", 0) != 0) gone.push_back(q.text(0));
                    for (auto& k : gone) removeRow("DELETE FROM meta WHERE key = ?1", k);
                }
            } else {
                if (p[0] == "entries" && p.size() == 2 && entryInWorld(stem(p[1]))) removeRow("DELETE FROM entries WHERE id = ?1", stem(p[1]));
                else if (p[0] == "books" && p.size() == 3 && p[2] == "book.json" && bookInWorld(p[1])) removeRow("DELETE FROM books WHERE id = ?1", p[1]);
                else if (p[0] == "series" && p.size() == 2) {
                    Stmt d(db_, "DELETE FROM series WHERE id = ?1 AND world_id = ?2");
                    d.bind(1, stem(p[1])).bind(2, w);
                    d.run();
                }
            }
        }
    for (auto& [node, content] : keepVersions) {
        Stmt q(db_, "SELECT n.id FROM nodes n JOIN books b ON b.id = n.book_id WHERE n.id = ?1 AND b.world_id = ?2");
        q.bind(1, node).bind(2, w);
        if (q.row() && !addVersionTx(node, content, doc::wordCount(content), "From another computer")) return fail(error_);
    }
    if (!commit()) { error = error_.empty() ? std::string(sqlite3_errmsg(db_)) : error_; rollback(); return false; }
    if (!reload()) { error = error_; return false; }
    return true;
}

namespace sync {

// ------------------------------------------------------------------------------------------- hashing

std::string sha1Hex(const std::string& data) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = data;
    const std::uint64_t bits = (std::uint64_t)data.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; --i) m += (char)((bits >> (i * 8)) & 0xff);
    auto rol = [](std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t wv[80];
        for (int i = 0; i < 16; ++i)
            wv[i] = ((std::uint32_t)(unsigned char)m[off + i * 4] << 24) | ((std::uint32_t)(unsigned char)m[off + i * 4 + 1] << 16) |
                    ((std::uint32_t)(unsigned char)m[off + i * 4 + 2] << 8) | (std::uint32_t)(unsigned char)m[off + i * 4 + 3];
        for (int i = 16; i < 80; ++i) wv[i] = rol(wv[i - 3] ^ wv[i - 8] ^ wv[i - 14] ^ wv[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const std::uint32_t t = rol(a, 5) + f + e + k + wv[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (std::uint32_t v : h)
        for (int i = 7; i >= 0; --i) out += hex[(v >> (i * 4)) & 15];
    return out;
}

std::string gitBlobSha(const std::string& bytes) {
    std::string head = "blob " + std::to_string(bytes.size());
    head += '\0';
    return sha1Hex(head + bytes);
}

std::string base64Encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const std::uint32_t v = (unsigned char)in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const std::uint32_t v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::string base64Decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    std::uint32_t acc = 0;
    int n = 0, pad = 0;
    for (char c : in) {
        if (c == '\n' || c == '\r' || c == ' ') continue;
        if (c == '=') { ++pad; continue; }
        if (pad) return {};
        const int v = val(c);
        if (v < 0) return {};
        acc = (acc << 6) | (std::uint32_t)v;
        if (++n == 4) {
            out += (char)((acc >> 16) & 0xff); out += (char)((acc >> 8) & 0xff); out += (char)(acc & 0xff);
            acc = 0; n = 0;
        }
    }
    if (n == 2) out += (char)((acc >> 4) & 0xff);
    else if (n == 3) { out += (char)((acc >> 10) & 0xff); out += (char)((acc >> 2) & 0xff); }
    else if (n == 1) return {};
    return out;
}

bool validPath(const std::string& path) {
    if (path.empty() || path.size() > 400 || path.front() == '/' || path.back() == '/') return false;
    const auto p = parts(path);
    for (auto& s : p) {
        if (s.empty() || s == "." || s == ".." || s.size() > 200) return false;
        for (unsigned char c : s)
            if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '~' || c == '+' || c == ' ' || c >= 0x80)) return false;
    }
    const std::string& top = p[0];
    if (path == "world.json") return true;
    if (top == "objects") return p.size() == 3 && p[1].size() == 2 && p[2].size() == 62 && isHex64(p[1] + p[2]);
    if (top == "fonts") return p.size() >= 2 && p.size() <= 4;
    if (top == "README.md" && p.size() == 1) return false;
    if (p.size() == 2 && (top == "series" || top == "entries" || top == "links" || top == "artworks" || top == "memberships" || top == "settings"))
        return endsWith(p[1], ".json");
    if (top == "books") {
        if (p.size() == 3) return p[2] == "book.json" || p[2] == "log.json";
        if (p.size() == 4) return (p[2] == "nodes" || p[2] == "editions" || p[2] == "snapshots" || p[2] == "comments") && endsWith(p[3], ".json");
    }
    return false;
}

bool parseRepoLink(const std::string& linkIn, std::string& owner, std::string& repo) {
    std::string s = linkIn;
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(0, 1);
    for (const char* pre : {"https://", "http://", "git@", "www."}) if (s.rfind(pre, 0) == 0) s = s.substr(std::strlen(pre));
    if (s.rfind("github.com:", 0) == 0) s = s.substr(11);
    else if (s.rfind("github.com/", 0) == 0) s = s.substr(11);
    else if (s.find("://") != std::string::npos || s.find('.') < s.find('/')) return false;   // another site
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (endsWith(s, ".git")) s.resize(s.size() - 4);
    const size_t slash = s.find('/');
    if (slash == std::string::npos) return false;
    owner = s.substr(0, slash);
    repo = s.substr(slash + 1);
    if (const size_t more = repo.find('/'); more != std::string::npos) repo = repo.substr(0, more);   // ".../tree/main"
    static const std::regex ok(R"([A-Za-z0-9][A-Za-z0-9._-]{0,99})");
    return std::regex_match(owner, ok) && std::regex_match(repo, ok);
}

// ------------------------------------------------------------------------------------------- in memory

bool MemoryRemote::info(RepoInfo& out, std::string&) {
    out.isPrivate = isPrivate;
    out.defaultBranch = "main";
    return true;
}
bool MemoryRemote::head(RemoteState& out, std::string&) {
    out = {};
    out.commit = commitId;
    for (auto& [p, b] : files) out.files[p] = gitBlobSha(b);
    return true;
}
bool MemoryRemote::getBlob(const std::string& sha, std::string& bytes, std::string& error) {
    for (auto& [p, b] : files)
        if (gitBlobSha(b) == sha) { bytes = b; return true; }
    error = "No such file.";
    return false;
}
bool MemoryRemote::commit(const RemoteState& parent, const std::map<std::string, std::optional<std::string>>& changes, const std::string&,
                          std::string& newCommit, bool& moved, std::string&) {
    moved = parent.commit != commitId;
    if (moved) return false;
    for (auto& [p, b] : changes) {
        if (b) files[p] = *b;
        else files.erase(p);
    }
    commitId = "c" + std::to_string(++commits);
    newCommit = commitId;
    return true;
}

// ------------------------------------------------------------------------------------------- GitHub

GitHubRemote::GitHubRemote(std::string owner, std::string repo, std::string token, Http http, std::string branch)
    : owner_(std::move(owner)), repo_(std::move(repo)), token_(std::move(token)), branch_(std::move(branch)), http_(std::move(http)) {}

HttpResponse GitHubRemote::call(const std::string& method, const std::string& path, const std::string& body) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
        {"User-Agent", "NorthRay-Writing-Studio"},
        {"Authorization", "Bearer " + token_}};
    if (!body.empty()) headers.push_back({"Content-Type", "application/json"});
    if (!http_) return {0, {}, "No way to reach GitHub."};
    return http_(method, apiBase + "/repos/" + owner_ + "/" + repo_ + path, headers, body);
}

std::string GitHubRemote::describe(const HttpResponse& r, const std::string& what) const {
    if (r.status == 0) return "Couldn't reach GitHub" + (r.error.empty() ? std::string(".") : ": " + r.error);
    if (r.status == 401) return "GitHub didn't accept the token. It may have expired or been revoked.";
    if (r.status == 403 || r.status == 429) {
        const json j = json::parse(r.body, nullptr, false);
        const std::string m = str(j, "message");
        if (m.find("rate limit") != std::string::npos) return "GitHub's limit on requests was reached. Try again in an hour.";
        return "The token isn't allowed to " + what + ". It needs Contents: Read and write on " + owner_ + "/" + repo_ + ".";
    }
    if (r.status == 404) return "GitHub can't find " + owner_ + "/" + repo_ + ", or the token can't see it.";
    const json j = json::parse(r.body, nullptr, false);
    const std::string m = str(j, "message");
    return "GitHub couldn't " + what + " (" + std::to_string(r.status) + (m.empty() ? "" : ": " + m.substr(0, 200)) + ").";
}

bool GitHubRemote::info(RepoInfo& out, std::string& error) {
    const HttpResponse r = call("GET", "");
    if (r.status != 200) { error = describe(r, "read the repository"); return false; }
    const json j = json::parse(r.body, nullptr, false);
    out.isPrivate = j.is_object() && j.value("private", false);
    out.defaultBranch = str(j, "default_branch");
    if (out.defaultBranch.empty()) out.defaultBranch = "main";
    if (branch_.empty()) branch_ = out.defaultBranch;
    return true;
}

bool GitHubRemote::head(RemoteState& out, std::string& error) {
    out = {};
    if (branch_.empty()) { RepoInfo i; if (!info(i, error)) return false; }
    HttpResponse r = call("GET", "/git/ref/heads/" + branch_);
    if (r.status == 404 || r.status == 409) return true;   // an empty repository
    if (r.status != 200) { error = describe(r, "read the repository"); return false; }
    json j = json::parse(r.body, nullptr, false);
    out.commit = j.is_object() && j.contains("object") ? str(j["object"], "sha") : std::string();
    if (out.commit.empty()) { error = "GitHub's answer didn't make sense."; return false; }
    r = call("GET", "/git/commits/" + out.commit);
    if (r.status != 200) { error = describe(r, "read the repository"); return false; }
    j = json::parse(r.body, nullptr, false);
    out.tree = j.is_object() && j.contains("tree") ? str(j["tree"], "sha") : std::string();
    r = call("GET", "/git/trees/" + out.tree + "?recursive=1");
    if (r.status != 200) { error = describe(r, "read the repository"); return false; }
    j = json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("tree") || !j["tree"].is_array()) { error = "GitHub's answer didn't make sense."; return false; }
    if (j.value("truncated", false)) { error = "The repository has more files than GitHub will list at once."; return false; }
    for (auto& t : j["tree"])
        if (str(t, "type") == "blob") out.files[str(t, "path")] = str(t, "sha");
    return true;
}

bool GitHubRemote::getBlob(const std::string& sha, std::string& bytes, std::string& error) {
    const HttpResponse r = call("GET", "/git/blobs/" + sha);
    if (r.status != 200) { error = describe(r, "send a file"); return false; }
    const json j = json::parse(r.body, nullptr, false);
    const std::string content = str(j, "content");
    if (str(j, "encoding") != "base64") { error = "GitHub sent a file in a way Writing Studio doesn't read."; return false; }
    bytes = base64Decode(content);
    if (gitBlobSha(bytes) != sha) { error = "A file from GitHub came through damaged."; return false; }
    return true;
}

bool GitHubRemote::ensureBranch(std::string& error) {
    // The git data API can't write to an empty repository, so the first commit is a short README.
    const std::string readme =
        "# A NorthRay Writing Studio world\n\nThis repository holds a world from NorthRay Writing Studio: its books, chapters, bible, "
        "plans and pictures. Open it in Writing Studio (Worldbuilding, then Sync) on each computer you write on, rather than "
        "changing these files here.\n";
    const json body = {{"message", "Start the world's repository"}, {"content", base64Encode(readme)}};
    const HttpResponse r = call("PUT", "/contents/README.md", body.dump());
    if (r.status != 201 && r.status != 200) { error = describe(r, "write to the repository"); return false; }
    return true;
}

bool GitHubRemote::commit(const RemoteState& parentIn, const std::map<std::string, std::optional<std::string>>& changes,
                          const std::string& message, std::string& newCommit, bool& moved, std::string& error) {
    moved = false;
    RemoteState parent = parentIn;
    if (parent.commit.empty()) {
        if (!ensureBranch(error)) return false;
        if (!head(parent, error)) return false;
        if (parent.commit.empty()) { error = "GitHub didn't start the repository."; return false; }
    }
    json tree = json::array();
    for (auto& [path, bytes] : changes) {
        if (!bytes) {
            if (parent.files.count(path)) tree.push_back({{"path", path}, {"mode", "100644"}, {"type", "blob"}, {"sha", nullptr}});
            continue;
        }
        const json b = {{"content", base64Encode(*bytes)}, {"encoding", "base64"}};
        const HttpResponse r = call("POST", "/git/blobs", b.dump());
        if (r.status != 201) { error = describe(r, "store a file"); return false; }
        const std::string sha = str(json::parse(r.body, nullptr, false), "sha");
        if (sha != gitBlobSha(*bytes)) { error = "GitHub stored a file differently from what was sent."; return false; }
        tree.push_back({{"path", path}, {"mode", "100644"}, {"type", "blob"}, {"sha", sha}});
    }
    if (tree.empty()) { newCommit = parent.commit; return true; }
    HttpResponse r = call("POST", "/git/trees", json{{"base_tree", parent.tree}, {"tree", tree}}.dump());
    if (r.status != 201) { error = describe(r, "write to the repository"); return false; }
    const std::string treeSha = str(json::parse(r.body, nullptr, false), "sha");
    r = call("POST", "/git/commits", json{{"message", message}, {"tree", treeSha}, {"parents", json::array({parent.commit})}}.dump());
    if (r.status != 201) { error = describe(r, "write to the repository"); return false; }
    const std::string c = str(json::parse(r.body, nullptr, false), "sha");
    r = call("PATCH", "/git/refs/heads/" + branch_, json{{"sha", c}, {"force", false}}.dump());
    if (r.status == 422 || r.status == 409) { moved = true; return false; }
    if (r.status != 200) { error = describe(r, "write to the repository"); return false; }
    newCommit = c;
    return true;
}

// ------------------------------------------------------------------------------------------- state

State loadState(const Library& lib, const Id& world) {
    State s;
    const json j = json::parse(lib.setting("sync:world:" + world), nullptr, false);
    if (!j.is_object()) return s;
    s.repo = str(j, "repo");
    s.branch = str(j, "branch");
    s.commit = str(j, "commit");
    s.lastSync = j.value("lastSync", (std::int64_t)0);
    if (j.contains("files") && j["files"].is_object())
        for (auto& [p, sha] : j["files"].items()) if (sha.is_string()) s.files[p] = sha.get<std::string>();
    return s;
}

bool saveState(Library& lib, const Id& world, const State& s) {
    json j = {{"repo", s.repo}, {"branch", s.branch}, {"commit", s.commit}, {"lastSync", s.lastSync}, {"files", s.files}};
    return lib.setSetting("sync:world:" + world, j.dump());
}

bool forget(Library& lib, const Id& world) { return lib.setSetting("sync:world:" + world, ""); }

// ------------------------------------------------------------------------------------------- syncing

namespace {

std::int64_t modifiedOf(const std::string& bytes) {
    const json j = json::parse(bytes, nullptr, false);
    return j.is_object() && j.contains("modified") && j["modified"].is_number() ? j["modified"].get<std::int64_t>() : -1;
}

struct Resolved {
    std::string bytes;
    std::optional<std::pair<Id, std::string>> keepVersion;   // a chapter's other text, kept here as a saved version
    std::string note;
};

// Both sides changed the same file.
Resolved resolve(const std::string& path, const std::string& here, const std::string& there) {
    Resolved r;
    const auto p = parts(path);
    if (p[0] == "links") {   // links: everything either side has
        json a = json::parse(here, nullptr, false), b = json::parse(there, nullptr, false);
        std::set<std::pair<std::string, std::string>> all;
        for (const json* x : {&a, &b})
            if (x->is_array()) for (auto& l : *x) all.insert({str(l, "to"), str(l, "label")});
        json out = json::array();
        for (auto& [to, label] : all) out.push_back({{"to", to}, {"label", label}});
        r.bytes = bytesOf(out);
        return r;
    }
    if (p[0] == "books" && p.size() == 3 && p[2] == "log.json") {   // words written: the most for each day
        std::map<std::string, json> days;
        for (const std::string* s : {&here, &there}) {
            const json a = json::parse(*s, nullptr, false);
            if (!a.is_array()) continue;
            for (auto& d : a) {
                const std::string day = str(d, "day");
                auto it = days.find(day);
                if (it == days.end() || d.value("words", (std::int64_t)0) > it->second.value("words", (std::int64_t)0)) days[day] = d;
            }
        }
        json out = json::array();
        for (auto& [d, row] : days) out.push_back(row);
        r.bytes = bytesOf(out);
        return r;
    }
    if (p[0] == "objects") { r.bytes = there; return r; }   // the same picture by definition
    const std::int64_t mh = modifiedOf(here), mt = modifiedOf(there);
    const bool hereWins = mh >= 0 && mt >= 0 ? mh > mt : p[0] == "settings";
    r.bytes = hereWins ? here : there;
    const json loser = json::parse(hereWins ? there : here, nullptr, false);
    const json winner = json::parse(r.bytes, nullptr, false);
    if (p[0] == "books" && p.size() == 4 && p[2] == "nodes") {
        const std::string other = str(loser, "content");
        if (!other.empty() && other != str(winner, "content")) r.keepVersion = std::make_pair(str(winner, "id"), other);
        const std::string t = str(winner, "title");
        r.note = "\xE2\x80\x9C" + (t.empty() ? std::string("A chapter") : t) +
                 "\xE2\x80\x9D was changed on both computers. The newer text is kept; the other is a saved version (Review, Changes).";
    } else if (p[0] == "entries") {
        r.note = "\xE2\x80\x9C" + str(winner, "name") + "\xE2\x80\x9D was changed on both computers. The newer is kept.";
    } else if (p[0] == "settings") {
        r.note = "A setting (" + str(winner, "key").substr(0, 60) + ") was changed on both computers. This computer's is kept.";
    }
    return r;
}

}  // namespace

bool syncWorld(Library& lib, const Id& worldIn, Remote& remote, State& state, Report& report, std::string& error) {
    report = {};
    Id world = worldIn;
    for (int attempt = 0; attempt < 4; ++attempt) {
        RemoteState rs;
        if (!remote.head(rs, error)) return false;
        if (world.empty()) {   // opening a world from the repository
            auto it = rs.files.find("world.json");
            if (it == rs.files.end()) { error = "That repository doesn't hold a Writing Studio world yet."; return false; }
            std::string bytes;
            if (!remote.getBlob(it->second, bytes, error)) return false;
            world = str(json::parse(bytes, nullptr, false), "id");
            if (world.empty()) { error = "The repository's world can't be read."; return false; }
            if (lib.world(world)) { error = "That world is already in this library."; return false; }
            state.files.clear();
            state.commit.clear();
        }
        report.world = world;
        const auto local = lib.worldFiles(world);
        std::map<std::string, std::string> here;   // path -> sha
        auto bytesHere = [&](const std::string& p) -> std::string {
            const auto& f = local.at(p);
            return f.diskPath.empty() ? f.bytes : readAll(f.diskPath);
        };
        for (auto& [p, f] : local) {
            if (!f.diskPath.empty()) {
                std::error_code ec;
                const auto size = fs::file_size(u8path(f.diskPath), ec);
                if (ec) continue;
                if (size > kMaxFileBytes) { report.skipped.push_back(p); continue; }
                if (p.rfind("objects/", 0) == 0 && state.files.count(p)) { here[p] = state.files.at(p); continue; }   // named by content: unchanged
                here[p] = gitBlobSha(readAll(f.diskPath));
            } else {
                here[p] = gitBlobSha(f.bytes);
            }
        }
        std::set<std::string> paths;
        for (auto& [p, s] : here) paths.insert(p);
        for (auto& [p, s] : rs.files) if (validPath(p)) paths.insert(p);
        for (auto& [p, s] : state.files) paths.insert(p);

        std::map<std::string, std::optional<std::string>> toHere, toThere;
        std::map<std::string, std::string> merged;   // path -> sha after this sync
        std::vector<std::pair<Id, std::string>> versions;
        std::vector<std::string> notes;
        for (const std::string& p : paths) {
            auto get = [](const std::map<std::string, std::string>& m, const std::string& k) {
                auto it = m.find(k);
                return it == m.end() ? std::string() : it->second;
            };
            const std::string b = get(state.files, p), l = get(here, p), r = validPath(p) ? get(rs.files, p) : std::string();
            if (l == r) { if (!l.empty()) merged[p] = l; continue; }
            if (l == b) {   // changed there only
                if (r.empty()) { toHere[p] = std::nullopt; continue; }
                std::string bytes;
                if (!remote.getBlob(r, bytes, error)) return false;
                toHere[p] = std::move(bytes);
                merged[p] = r;
            } else if (r == b) {   // changed here only
                if (l.empty()) { toThere[p] = std::nullopt; continue; }
                toThere[p] = bytesHere(p);
                merged[p] = l;
            } else if (l.empty() || r.empty()) {   // removed on one side, changed on the other: kept
                if (l.empty()) {
                    std::string bytes;
                    if (!remote.getBlob(r, bytes, error)) return false;
                    toHere[p] = std::move(bytes);
                    merged[p] = r;
                } else {
                    toThere[p] = bytesHere(p);
                    merged[p] = l;
                }
            } else {
                std::string there;
                if (!remote.getBlob(r, there, error)) return false;
                const std::string mine = bytesHere(p);
                Resolved res = resolve(p, mine, there);
                if (res.keepVersion) versions.push_back(*res.keepVersion);
                if (!res.note.empty()) notes.push_back(res.note);
                const std::string sha = gitBlobSha(res.bytes);
                if (sha != l) toHere[p] = res.bytes;
                if (sha != r) toThere[p] = res.bytes;
                merged[p] = sha;
            }
        }
        // Here first (a failure leaves the repository as it was), then there.
        if (!toHere.empty() || !versions.empty()) {
            if (!lib.applyWorldFiles(world, toHere, versions, error)) return false;
        }
        std::string newCommit = rs.commit;
        if (!toThere.empty()) {
            bool moved = false;
            const World* wd = lib.world(world);
            if (!remote.commit(rs, toThere, "Writing Studio: " + (wd ? wd->name : std::string("a world")), newCommit, moved, error)) {
                if (moved) {   // another computer synced in between: go again from what's there now
                    for (auto& [p, s] : merged) if (toHere.count(p)) state.files[p] = s;   // what was taken stays taken
                    for (auto& [p, v] : toHere) if (!v) state.files.erase(p);
                    report.received += (int)std::count_if(toHere.begin(), toHere.end(), [](auto& x) { return (bool)x.second; });
                    report.removedHere += (int)std::count_if(toHere.begin(), toHere.end(), [](auto& x) { return !x.second; });
                    error.clear();
                    continue;
                }
                return false;
            }
        }
        for (auto& [p, v] : toHere) (v ? report.received : report.removedHere)++;
        for (auto& [p, v] : toThere) (v ? report.sent : report.removedThere)++;
        report.conflicts.insert(report.conflicts.end(), notes.begin(), notes.end());
        state.files = merged;
        state.commit = newCommit;
        state.lastSync = (std::int64_t)std::time(nullptr);
        saveState(lib, world, state);
        return true;
    }
    error = "Another computer kept syncing at the same moment. Try again.";
    return false;
}

}  // namespace sync
}  // namespace studio

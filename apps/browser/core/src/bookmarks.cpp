#include "nrb/bookmarks.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <system_error>

namespace nrb {

namespace fs = std::filesystem;

namespace {

fs::path fsPath(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

// A prepared statement that finalises itself.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) { if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) s_ = nullptr; }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    explicit operator bool() const { return s_ != nullptr; }
    Stmt& bind(int i, std::int64_t v) { sqlite3_bind_int64(s_, i, v); return *this; }
    Stmt& bind(int i, const std::string& v) { sqlite3_bind_text(s_, i, v.data(), (int)v.size(), SQLITE_TRANSIENT); return *this; }
    Stmt& bindBlob(int i, const std::vector<unsigned char>& v) {
        if (v.empty()) sqlite3_bind_null(s_, i);
        else sqlite3_bind_blob(s_, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
        return *this;
    }
    bool step() { return s_ && sqlite3_step(s_) == SQLITE_ROW; }
    bool run() { return s_ && sqlite3_step(s_) == SQLITE_DONE; }
    std::int64_t i64(int c) const { return sqlite3_column_int64(s_, c); }
    std::string text(int c) const {
        const auto* t = sqlite3_column_text(s_, c);
        return t ? std::string(reinterpret_cast<const char*>(t), (size_t)sqlite3_column_bytes(s_, c)) : std::string();
    }
    std::vector<unsigned char> blob(int c) const {
        const auto* b = static_cast<const unsigned char*>(sqlite3_column_blob(s_, c));
        return b ? std::vector<unsigned char>(b, b + sqlite3_column_bytes(s_, c)) : std::vector<unsigned char>();
    }

private:
    sqlite3_stmt* s_ = nullptr;
};

bool exec(sqlite3* db, const char* sql, std::string& error) {
    char* msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) == SQLITE_OK) return true;
    error = msg ? msg : "database error";
    sqlite3_free(msg);
    return false;
}

constexpr const char* kColumns = "id, parent, folder, title, url, position, created, icon_w, icon_h, icon";

Bookmark readRow(const Stmt& s) {
    Bookmark b;
    b.id = s.i64(0);
    b.parent = s.i64(1);
    b.folder = s.i64(2) != 0;
    b.title = s.text(3);
    b.url = s.text(4);
    b.position = (int)s.i64(5);
    b.created = s.i64(6);
    b.iconW = (int)s.i64(7);
    b.iconH = (int)s.i64(8);
    b.icon = s.blob(9);
    if ((size_t)b.iconW * (size_t)b.iconH * 4 != b.icon.size()) { b.icon.clear(); b.iconW = b.iconH = 0; }
    return b;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string htmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

void appendUtf8(std::string& o, unsigned long cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else {
        o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    }
}

std::string htmlUnescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { o += s[i]; continue; }
        const std::string e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") o += '&';
        else if (e == "lt") o += '<';
        else if (e == "gt") o += '>';
        else if (e == "quot") o += '"';
        else if (e == "apos") o += '\'';
        else if (e == "nbsp") appendUtf8(o, 0xA0);
        else if (e.size() > 1 && e[0] == '#') {
            const bool hex = e[1] == 'x' || e[1] == 'X';
            const std::string digits = e.substr(hex ? 2 : 1);
            if (digits.empty() || digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") != std::string::npos) {
                o += s.substr(i, semi - i + 1);
            } else {
                appendUtf8(o, std::stoul(digits, nullptr, hex ? 16 : 10));
            }
        } else {
            o += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return o;
}

// The value of an attribute in a tag's text (`<A HREF="..." ...>`), or "".
std::string attribute(const std::string& tag, const std::string& name) {
    const std::string l = lower(tag);
    size_t at = 0;
    while ((at = l.find(name, at)) != std::string::npos) {
        const bool startOk = at > 0 && std::isspace((unsigned char)l[at - 1]);
        size_t p = at + name.size();
        while (p < l.size() && std::isspace((unsigned char)l[p])) ++p;
        if (startOk && p < l.size() && l[p] == '=') {
            ++p;
            while (p < l.size() && std::isspace((unsigned char)l[p])) ++p;
            if (p < l.size() && (l[p] == '"' || l[p] == '\'')) {
                const char q = l[p];
                const size_t end = l.find(q, p + 1);
                if (end == std::string::npos) return {};
                return htmlUnescape(tag.substr(p + 1, end - p - 1));
            }
            size_t end = p;
            while (end < l.size() && !std::isspace((unsigned char)l[end]) && l[end] != '>') ++end;
            return htmlUnescape(tag.substr(p, end - p));
        }
        at = p;
    }
    return {};
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

}  // namespace

bool bookmarkableUrl(const std::string& url) {
    const std::string l = lower(url);
    return l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0 || l.rfind("file:///", 0) == 0 || l.rfind("northray://", 0) == 0;
}

std::unique_ptr<BookmarkStore> BookmarkStore::open(const std::string& path, std::string& error, std::string* notice) {
    auto tryOpen = [&](std::unique_ptr<BookmarkStore>& out) -> int {   // 0 ok, 1 damaged, 2 other failure
        out.reset(new BookmarkStore());
        out->path_ = path;
        if (sqlite3_open_v2(path.c_str(), &out->db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            error = out->db_ ? sqlite3_errmsg(out->db_) : "couldn't open the bookmarks";
            return 2;
        }
        sqlite3_busy_timeout(out->db_, 3000);
        {
            Stmt check(out->db_, "PRAGMA quick_check");   // finalised before anything else runs
            if (!check || !check.step() || check.text(0) != "ok") return 1;
        }
        if (path != ":memory:" && !exec(out->db_, "PRAGMA journal_mode=WAL", error)) return 1;
        exec(out->db_, "PRAGMA foreign_keys=ON", error);
        if (!out->migrate()) { error = out->error_; return out->error_.find("newer") != std::string::npos ? 2 : 1; }
        return 0;
    };
    std::unique_ptr<BookmarkStore> s;
    const int r = tryOpen(s);
    if (r == 0) return s;
    if (r == 2 || path == ":memory:") return nullptr;
    // Damaged: keep the file for anyone who wants it, then use the backup, or start again.
    s.reset();
    std::error_code ec;
    const fs::path p = fsPath(path);
    fs::rename(p, fsPath(path + ".damaged"), ec);
    fs::remove(fsPath(path + "-wal"), ec);
    fs::remove(fsPath(path + "-shm"), ec);
    const fs::path backup = fsPath(path + ".backup");
    const bool restored = fs::exists(backup, ec) && fs::copy_file(backup, p, fs::copy_options::overwrite_existing, ec);
    if (tryOpen(s) != 0) {
        if (!restored) return nullptr;
        s.reset();
        fs::remove(p, ec);
        if (tryOpen(s) != 0) return nullptr;
        if (notice) *notice = "Your bookmarks file was damaged and its backup couldn't be read, so bookmarks start again. The damaged file is kept beside it.";
        return s;
    }
    if (notice)
        *notice = restored ? "Your bookmarks file was damaged, so it was restored from its most recent daily backup."
                           : "Your bookmarks file was damaged, so bookmarks start again. The damaged file is kept beside it.";
    return s;
}

BookmarkStore::~BookmarkStore() {
    if (db_) sqlite3_close_v2(db_);
}

bool BookmarkStore::migrate() {
    Stmt v(db_, "PRAGMA user_version");
    const int version = v.step() ? (int)v.i64(0) : 0;
    if (version > kSchemaVersion) {
        error_ = "These bookmarks were saved by a newer NorthRay Browser.";
        return false;
    }
    if (version == kSchemaVersion) return true;
    // Version 1: the bookmarks table, the two fixed folders and a little metadata.
    const char* v1 =
        "BEGIN;"
        "CREATE TABLE IF NOT EXISTS bookmarks ("
        "  id INTEGER PRIMARY KEY,"
        "  parent INTEGER REFERENCES bookmarks(id) ON DELETE CASCADE,"
        "  folder INTEGER NOT NULL DEFAULT 0,"
        "  title TEXT NOT NULL DEFAULT '',"
        "  url TEXT NOT NULL DEFAULT '',"
        "  position INTEGER NOT NULL DEFAULT 0,"
        "  created INTEGER NOT NULL DEFAULT 0,"
        "  icon_w INTEGER NOT NULL DEFAULT 0, icon_h INTEGER NOT NULL DEFAULT 0, icon BLOB);"
        "CREATE INDEX IF NOT EXISTS bookmarks_parent ON bookmarks(parent, position);"
        "CREATE INDEX IF NOT EXISTS bookmarks_url ON bookmarks(url);"
        "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);"
        "INSERT OR IGNORE INTO bookmarks(id, parent, folder, title, position) VALUES (1, NULL, 1, 'Bookmarks bar', 0);"
        "INSERT OR IGNORE INTO bookmarks(id, parent, folder, title, position) VALUES (2, NULL, 1, 'Other bookmarks', 1);"
        "PRAGMA user_version = 1;"
        "COMMIT;";
    if (!exec(db_, v1, error_)) {
        std::string ignored;
        exec(db_, "ROLLBACK", ignored);
        return false;
    }
    return true;
}

std::vector<Bookmark> BookmarkStore::children(std::int64_t folder) const {
    std::vector<Bookmark> out;
    Stmt s(db_, (std::string("SELECT ") + kColumns + " FROM bookmarks WHERE parent = ? ORDER BY position, id").c_str());
    s.bind(1, folder);
    while (s.step()) out.push_back(readRow(s));
    return out;
}

std::optional<Bookmark> BookmarkStore::get(std::int64_t id) const {
    Stmt s(db_, (std::string("SELECT ") + kColumns + " FROM bookmarks WHERE id = ?").c_str());
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return readRow(s);
}

std::optional<Bookmark> BookmarkStore::findUrl(const std::string& url) const {
    if (url.empty()) return std::nullopt;
    Stmt s(db_, (std::string("SELECT ") + kColumns + " FROM bookmarks WHERE folder = 0 AND url = ? ORDER BY id LIMIT 1").c_str());
    s.bind(1, url);
    if (!s.step()) return std::nullopt;
    return readRow(s);
}

std::vector<Bookmark> BookmarkStore::search(const std::string& query, std::size_t limit) const {
    std::vector<Bookmark> out;
    // Matching is done here rather than with LIKE, so any character can be searched for.
    const std::string q = lower(trim(query));
    Stmt s(db_, (std::string("SELECT ") + kColumns + " FROM bookmarks WHERE folder = 0 ORDER BY created DESC, id DESC").c_str());
    while (s.step() && out.size() < limit) {
        Bookmark b = readRow(s);
        if (q.empty() || lower(b.title).find(q) != std::string::npos || lower(b.url).find(q) != std::string::npos)
            out.push_back(std::move(b));
    }
    return out;
}

std::size_t BookmarkStore::count() const {
    Stmt s(db_, "SELECT COUNT(*) FROM bookmarks WHERE folder = 0");
    return s.step() ? (std::size_t)s.i64(0) : 0;
}

bool BookmarkStore::isFolder(std::int64_t id) const {
    Stmt s(db_, "SELECT folder FROM bookmarks WHERE id = ?");
    s.bind(1, id);
    return s.step() && s.i64(0) != 0;
}

std::int64_t BookmarkStore::insert(std::int64_t parent, bool folder, const std::string& title, const std::string& url,
                                   int position, std::int64_t created) {
    if (!isFolder(parent)) { error_ = "That folder doesn't exist."; return 0; }
    std::int64_t n = 0;
    {
        Stmt c(db_, "SELECT COUNT(*) FROM bookmarks WHERE parent = ?");
        c.bind(1, parent);
        if (c.step()) n = c.i64(0);
    }
    const std::int64_t pos = position < 0 || position > n ? n : position;
    std::string err;
    if (!exec(db_, "SAVEPOINT ins", err)) { error_ = err; return 0; }
    Stmt shift(db_, "UPDATE bookmarks SET position = position + 1 WHERE parent = ? AND position >= ?");
    shift.bind(1, parent).bind(2, pos);
    Stmt s(db_, "INSERT INTO bookmarks(parent, folder, title, url, position, created) VALUES (?, ?, ?, ?, ?, ?)");
    s.bind(1, parent).bind(2, folder ? 1 : 0).bind(3, title).bind(4, url).bind(5, pos).bind(6, created);
    if (!shift.run() || !s.run()) {
        error_ = sqlite3_errmsg(db_);
        exec(db_, "ROLLBACK TO ins; RELEASE ins", err);
        return 0;
    }
    exec(db_, "RELEASE ins", err);
    return sqlite3_last_insert_rowid(db_);
}

std::int64_t BookmarkStore::add(std::int64_t parent, const std::string& title, const std::string& url, int position) {
    if (!bookmarkableUrl(url)) { error_ = "That address can't be bookmarked."; return 0; }
    return insert(parent, false, title.empty() ? url : title, url, position, (std::int64_t)std::time(nullptr));
}

std::int64_t BookmarkStore::addFolder(std::int64_t parent, const std::string& title, int position) {
    return insert(parent, true, title.empty() ? "New folder" : title, "", position, (std::int64_t)std::time(nullptr));
}

bool BookmarkStore::rename(std::int64_t id, const std::string& title) {
    if (id == kBar || id == kOther) { error_ = "The fixed folders keep their names."; return false; }
    Stmt s(db_, "UPDATE bookmarks SET title = ? WHERE id = ?");
    s.bind(1, trim(title).empty() ? std::string("Untitled") : trim(title)).bind(2, id);
    return s.run() && sqlite3_changes(db_) == 1;
}

bool BookmarkStore::setUrl(std::int64_t id, const std::string& url) {
    if (!bookmarkableUrl(url)) { error_ = "That address can't be bookmarked."; return false; }
    Stmt s(db_, "UPDATE bookmarks SET url = ? WHERE id = ? AND folder = 0");
    s.bind(1, url).bind(2, id);
    return s.run() && sqlite3_changes(db_) == 1;
}

bool BookmarkStore::setIcon(std::int64_t id, int w, int h, const std::vector<unsigned char>& rgba) {
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || (size_t)w * (size_t)h * 4 != rgba.size()) return false;
    Stmt s(db_, "UPDATE bookmarks SET icon_w = ?, icon_h = ?, icon = ? WHERE id = ?");
    s.bind(1, w).bind(2, h).bindBlob(3, rgba).bind(4, id);
    return s.run();
}

bool BookmarkStore::renumber(std::int64_t parent) {
    std::vector<std::int64_t> ids;
    {
        Stmt s(db_, "SELECT id FROM bookmarks WHERE parent = ? ORDER BY position, id");
        s.bind(1, parent);
        while (s.step()) ids.push_back(s.i64(0));
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        Stmt u(db_, "UPDATE bookmarks SET position = ? WHERE id = ?");
        u.bind(1, (std::int64_t)i).bind(2, ids[i]);
        if (!u.run()) return false;
    }
    return true;
}

bool BookmarkStore::insideFolder(std::int64_t id, std::int64_t folder) const {
    // Walks up from `folder`; true when it meets `id` (moving a folder into itself).
    std::int64_t at = folder;
    for (int guard = 0; at != 0 && guard < 10000; ++guard) {
        if (at == id) return true;
        Stmt s(db_, "SELECT parent FROM bookmarks WHERE id = ?");
        s.bind(1, at);
        if (!s.step()) return false;
        at = s.i64(0);
    }
    return false;
}

bool BookmarkStore::move(std::int64_t id, std::int64_t parent, int position) {
    const auto b = get(id);
    if (!b || id == kBar || id == kOther) { error_ = "That can't be moved."; return false; }
    if (!isFolder(parent)) { error_ = "That folder doesn't exist."; return false; }
    if (b->folder && insideFolder(id, parent)) { error_ = "A folder can't go inside itself."; return false; }
    std::string err;
    if (!exec(db_, "SAVEPOINT mv", err)) { error_ = err; return false; }
    bool ok = true;
    {
        // Take it out (to the very end of the old folder), then put it in place.
        Stmt out(db_, "UPDATE bookmarks SET parent = ?, position = -1 WHERE id = ?");
        out.bind(1, parent).bind(2, id);
        ok = out.run() && renumber(b->parent);
    }
    if (ok) {
        std::vector<std::int64_t> ids;
        {
            Stmt s(db_, "SELECT id FROM bookmarks WHERE parent = ? AND id != ? ORDER BY position, id");
            s.bind(1, parent).bind(2, id);
            while (s.step()) ids.push_back(s.i64(0));
        }
        const size_t at = position < 0 || (size_t)position > ids.size() ? ids.size() : (size_t)position;
        ids.insert(ids.begin() + (std::ptrdiff_t)at, id);
        for (size_t i = 0; i < ids.size() && ok; ++i) {
            Stmt u(db_, "UPDATE bookmarks SET position = ? WHERE id = ?");
            u.bind(1, (std::int64_t)i).bind(2, ids[i]);
            ok = u.run();
        }
    }
    if (!ok) {
        error_ = sqlite3_errmsg(db_);
        exec(db_, "ROLLBACK TO mv; RELEASE mv", err);
        return false;
    }
    exec(db_, "RELEASE mv", err);
    return true;
}

bool BookmarkStore::remove(std::int64_t id) {
    const auto b = get(id);
    if (!b) { error_ = "That bookmark is already gone."; return false; }
    if (id == kBar || id == kOther) { error_ = "The fixed folders can't be deleted."; return false; }
    std::string err;
    if (!exec(db_, "SAVEPOINT rm", err)) { error_ = err; return false; }
    Stmt s(db_, "DELETE FROM bookmarks WHERE id = ?");   // a folder's contents go with it (ON DELETE CASCADE)
    s.bind(1, id);
    if (!s.run() || !renumber(b->parent)) {
        error_ = sqlite3_errmsg(db_);
        exec(db_, "ROLLBACK TO rm; RELEASE rm", err);
        return false;
    }
    exec(db_, "RELEASE rm", err);
    return true;
}

std::string BookmarkStore::exportHtml() const {
    std::string o =
        "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
        "<!-- Exported by NorthRay Browser. Chrome, Edge and Firefox can import this file. -->\n"
        "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
        "<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n";
    std::function<void(std::int64_t, int)> folder = [&](std::int64_t id, int depth) {
        const std::string pad((size_t)depth * 4, ' ');
        for (const Bookmark& b : children(id)) {
            if (b.folder) {
                o += pad + "<DT><H3 ADD_DATE=\"" + std::to_string(b.created) + "\">" + htmlEscape(b.title) + "</H3>\n";
                o += pad + "<DL><p>\n";
                folder(b.id, depth + 1);
                o += pad + "</DL><p>\n";
            } else {
                o += pad + "<DT><A HREF=\"" + htmlEscape(b.url) + "\" ADD_DATE=\"" + std::to_string(b.created) + "\">" +
                     htmlEscape(b.title) + "</A>\n";
            }
        }
    };
    o += "    <DT><H3 PERSONAL_TOOLBAR_FOLDER=\"true\">Bookmarks bar</H3>\n    <DL><p>\n";
    folder(kBar, 2);
    o += "    </DL><p>\n    <DT><H3>Other bookmarks</H3>\n    <DL><p>\n";
    folder(kOther, 2);
    o += "    </DL><p>\n</DL><p>\n";
    return o;
}

int BookmarkStore::importHtml(const std::string& html, std::int64_t parent) {
    if (!isFolder(parent)) { error_ = "That folder doesn't exist."; return 0; }
    std::string err;
    if (!exec(db_, "BEGIN", err)) { error_ = err; return 0; }
    std::vector<std::int64_t> stack{parent};
    std::int64_t pendingFolder = 0;   // an <H3> folder waiting for its <DL>
    bool seenTopList = false;
    int added = 0;
    size_t i = 0;
    while ((i = html.find('<', i)) != std::string::npos) {
        const size_t close = html.find('>', i);
        if (close == std::string::npos) break;
        const std::string tag = html.substr(i + 1, close - i - 1);
        const std::string name = lower(tag.substr(0, tag.find_first_of(" \t\r\n")));
        if (name == "dl") {
            if (!seenTopList) { seenTopList = true; }             // the file's outer list is `parent` itself
            else stack.push_back(pendingFolder ? pendingFolder : stack.back());
            pendingFolder = 0;
        } else if (name == "/dl") {
            if (stack.size() > 1) stack.pop_back();
        } else if (name == "h3" || name == "a") {
            const std::string endTag = name == "h3" ? "</h3" : "</a";
            const std::string lowRest = lower(html.substr(close + 1, 4096));
            const size_t end = lowRest.find(endTag);
            const std::string text = trim(htmlUnescape(end == std::string::npos ? std::string() : html.substr(close + 1, end)));
            const std::string added_ = attribute(tag, "add_date");
            const std::int64_t when = added_.empty() || added_.find_first_not_of("0123456789") != std::string::npos
                                          ? (std::int64_t)std::time(nullptr)
                                          : std::stoll(added_.substr(0, 12));
            if (name == "h3") {
                // Another browser's bookmarks bar goes into ours rather than a folder of that name.
                if (!attribute(tag, "personal_toolbar_folder").empty()) pendingFolder = kBar;
                else if (stack.size() == 1 && text == "Other bookmarks") pendingFolder = kOther;
                else pendingFolder = insert(stack.back(), true, text.empty() ? "Imported" : text, "", -1, when);
            } else {
                const std::string url = attribute(tag, "href");
                if (bookmarkableUrl(url) && insert(stack.back(), false, text.empty() ? url : text, url, -1, when)) ++added;
            }
            if (end != std::string::npos) { i = close + 1 + end; continue; }
        }
        i = close + 1;
    }
    if (!exec(db_, "COMMIT", err)) { error_ = err; exec(db_, "ROLLBACK", err); return 0; }
    return added;
}

bool BookmarkStore::backupIfDue(std::int64_t nowUnix) {
    if (path_ == ":memory:") return false;
    {
        Stmt s(db_, "SELECT value FROM meta WHERE key = 'last_backup'");
        if (s.step()) {
            const std::string v = s.text(0);
            if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos && nowUnix - std::stoll(v) < 24 * 3600)
                return false;
        }
    }
    const std::string tmp = path_ + ".backup.new";
    std::error_code ec;
    fs::remove(fsPath(tmp), ec);
    Stmt v(db_, "VACUUM INTO ?");
    v.bind(1, tmp);
    if (!v.run()) { error_ = sqlite3_errmsg(db_); return false; }
    fs::rename(fsPath(tmp), fsPath(path_ + ".backup"), ec);
    if (ec) { error_ = ec.message(); return false; }
    Stmt m(db_, "INSERT OR REPLACE INTO meta(key, value) VALUES ('last_backup', ?)");
    m.bind(1, std::to_string(nowUnix));
    return m.run();
}

}  // namespace nrb

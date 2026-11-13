// Worldbuilding entries and their relationships (schema 5), and finding where
// they're mentioned in the manuscripts.
#include "studio/document.hpp"
#include "studio/library.hpp"

#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace studio {

using namespace detail;
using nlohmann::json;

const std::vector<std::string>& entryKinds() {
    static const std::vector<std::string> k{"character", "place", "group", "thing", "lore", "event"};
    return k;
}
const char* entryKindLabel(const std::string& k) {
    if (k == "place") return "Place";
    if (k == "group") return "Group";
    if (k == "thing") return "Thing";
    if (k == "lore") return "Lore";
    if (k == "event") return "Event";
    return "Character";
}
const char* entryKindPlural(const std::string& k) {
    if (k == "place") return "Places";
    if (k == "group") return "Groups";
    if (k == "thing") return "Things";
    if (k == "lore") return "Lore";
    if (k == "event") return "Events";
    return "Characters";
}
static std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::string knownEntryKind(const std::string& k) {
    for (auto& x : entryKinds()) if (x == k) return k;
    return "character";
}

const std::vector<Relation>& relations() {
    static const std::vector<Relation> r{
        {"Family", "Family"},        {"Parent of", "Child of"},    {"Child of", "Parent of"},  {"Sibling of", "Sibling of"},
        {"Partner of", "Partner of"}, {"Friend of", "Friend of"},  {"Ally of", "Ally of"},     {"Rival of", "Rival of"},
        {"Enemy of", "Enemy of"},    {"Mentor of", "Student of"},  {"Member of", "Has member"}, {"Leader of", "Led by"},
        {"Lives in", "Home of"},     {"Located in", "Contains"},   {"Owns", "Owned by"},       {"Made", "Made by"},
        {"Took part in", "Involved"}, {"Related to", "Related to"},
    };
    return r;
}
std::string reverseRelation(const std::string& label) {
    for (auto& r : relations()) if (label == r.label) return r.reverse;
    return label;
}

// ----------------------------------------------------------------- storage

bool Library::loadEntries() {
    entries_.clear();
    links_.clear();
    {
        Stmt q(db_, "SELECT id, world_id, book_id, kind, name, aliases, summary, details, notes, image_blob, sort_order, created, modified, deleted "
                    "FROM entries ORDER BY name COLLATE NOCASE");
        if (!q) { error_ = sqlite3_errmsg(db_); return false; }
        while (q.row()) {
            Entry e;
            e.id = q.text(0); e.worldId = q.text(1); e.bookId = q.text(2); e.kind = knownEntryKind(q.text(3)); e.name = q.text(4);
            const json a = json::parse(q.text(5), nullptr, false);
            if (a.is_array()) for (auto& x : a) if (x.is_string()) e.aliases.push_back(x.get<std::string>());
            e.summary = q.text(6); e.details = q.text(7); e.notes = q.text(8); e.imageBlob = q.text(9);
            e.order = q.dbl(10); e.created = q.i64(11); e.modified = q.i64(12); e.deleted = q.i64(13);
            entries_.push_back(std::move(e));
        }
    }
    Stmt q(db_, "SELECT from_id, to_id, label FROM entry_links");
    if (!q) { error_ = sqlite3_errmsg(db_); return false; }
    while (q.row()) links_.push_back({q.text(0), q.text(1), q.text(2)});
    return true;
}

bool Library::saveEntry(const Entry& e) {
    Stmt s(db_, "INSERT INTO entries(id, world_id, book_id, kind, name, aliases, summary, details, notes, image_blob, sort_order, created, "
                "modified, deleted) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14) ON CONFLICT(id) DO UPDATE SET "
                "kind=excluded.kind, name=excluded.name, aliases=excluded.aliases, summary=excluded.summary, details=excluded.details, "
                "notes=excluded.notes, image_blob=excluded.image_blob, sort_order=excluded.sort_order, modified=excluded.modified, "
                "deleted=excluded.deleted");
    s.bindIdOrNull(2, e.worldId).bindIdOrNull(3, e.bookId);
    s.bind(1, e.id).bind(4, e.kind).bind(5, e.name).bind(6, json(e.aliases).dump()).bind(7, e.summary).bind(8, e.details)
        .bind(9, e.notes).bind(10, e.imageBlob).bind(11, e.order).bind(12, (std::int64_t)e.created).bind(13, (std::int64_t)e.modified)
        .bind(14, (std::int64_t)e.deleted);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

Entry* Library::entryMut(const Id& id) {
    for (auto& e : entries_) if (e.id == id) return &e;
    return nullptr;
}
const Entry* Library::entry(const Id& id) const {
    for (auto& e : entries_) if (e.id == id) return &e;
    return nullptr;
}

std::vector<const Entry*> Library::entriesOf(const Id& worldId, const Id& bookId) const {
    std::vector<const Entry*> out;
    for (auto& e : entries_)
        if (!e.deleted && (worldId.empty() ? e.worldId.empty() && e.bookId == bookId : e.worldId == worldId)) out.push_back(&e);
    std::stable_sort(out.begin(), out.end(), [](const Entry* a, const Entry* b) {
        std::string x = a->name, y = b->name;
        for (auto& c : x) c = (char)std::tolower((unsigned char)c);
        for (auto& c : y) c = (char)std::tolower((unsigned char)c);
        return x < y;
    });
    return out;
}

std::vector<const Entry*> Library::trashedEntries() const {
    std::vector<const Entry*> out;
    for (auto& e : entries_) {
        if (!e.deleted) continue;
        // Inside a trashed world or book, it's in the Trash with that (and comes back with it).
        const World* w = e.worldId.empty() ? nullptr : world(e.worldId);
        const Book* b = e.bookId.empty() ? nullptr : book(e.bookId);
        if ((w && w->deleted) || (b && b->deleted)) continue;
        out.push_back(&e);
    }
    return out;
}

Id Library::createEntry(const Id& worldId, const Id& bookId, const std::string& kind, const std::string& name) {
    if (worldId.empty() ? !book(bookId) : !world(worldId)) { error_ = "No such world or book."; return {}; }
    Entry e;
    e.id = newId();
    e.worldId = worldId;
    if (worldId.empty()) e.bookId = bookId;
    e.kind = knownEntryKind(kind);
    e.name = name;
    e.created = e.modified = now();
    if (!saveEntry(e)) return {};
    entries_.push_back(e);
    return e.id;
}

bool Library::updateEntry(const Entry& in) {
    Entry* e = entryMut(in.id);
    if (!e) { error_ = "No such entry."; return false; }
    Entry n = in;
    n.worldId = e->worldId;
    n.bookId = e->bookId;
    n.kind = knownEntryKind(n.kind);
    n.created = e->created;
    n.deleted = e->deleted;
    n.modified = now();
    // Other names: trimmed, no blanks, no repeats, not the name itself.
    std::vector<std::string> al;
    for (auto a : n.aliases) {
        a = trimmed(a);
        if (!a.empty() && a != n.name && std::find(al.begin(), al.end(), a) == al.end()) al.push_back(a);
    }
    n.aliases = al;
    if (!saveEntry(n)) return false;
    *e = n;
    return true;
}

bool Library::trashEntry(const Id& id) {
    Entry* e = entryMut(id);
    if (!e) return false;
    Entry n = *e;
    n.deleted = now();
    if (!saveEntry(n)) return false;
    *e = n;
    return true;
}

std::vector<EntryLink> Library::linksOf(const Id& id) const {
    std::vector<EntryLink> out;
    for (auto& l : links_) {
        if (l.from != id && l.to != id) continue;
        const Entry* other = entry(l.from == id ? l.to : l.from);
        if (other && !other->deleted) out.push_back(l);
    }
    return out;
}

bool Library::addLink(const Id& from, const Id& to, const std::string& label) {
    if (from == to || !entry(from) || !entry(to)) { error_ = "Those can't be linked."; return false; }
    for (auto& l : links_) if (l.from == from && l.to == to && l.label == label) return true;
    Stmt s(db_, "INSERT OR IGNORE INTO entry_links(from_id, to_id, label) VALUES (?1, ?2, ?3)");
    s.bind(1, from).bind(2, to).bind(3, label);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    links_.push_back({from, to, label});
    return true;
}

bool Library::removeLink(const Id& from, const Id& to, const std::string& label) {
    Stmt s(db_, "DELETE FROM entry_links WHERE from_id = ?1 AND to_id = ?2 AND label = ?3");
    s.bind(1, from).bind(2, to).bind(3, label);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    links_.erase(std::remove_if(links_.begin(), links_.end(), [&](const EntryLink& l) { return l.from == from && l.to == to && l.label == label; }),
                 links_.end());
    return true;
}

// ---------------------------------------------------------------- mentions

namespace {

// A word character for matching whole names: letters, digits, and any byte of a
// non-ASCII character (so "Café" isn't matched inside "Cafés").
bool wordByte(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

// Cuts at a character boundary, never inside a UTF-8 sequence.
size_t back(const std::string& s, size_t i) { while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i; return i; }

}  // namespace

namespace {
// How often any of `names` appears in `text` as a whole word, and where first.
int countNames(const std::string& text, const std::vector<std::string>& names, size_t* firstAt = nullptr, size_t* firstLen = nullptr) {
    int count = 0;
    size_t first = std::string::npos, len = 0;
    for (const std::string& name : names) {
        size_t at = 0;
        while ((at = text.find(name, at)) != std::string::npos) {
            const bool startOk = at == 0 || !wordByte((unsigned char)text[at - 1]);
            const size_t end = at + name.size();
            const bool endOk = end >= text.size() || !wordByte((unsigned char)text[end]);
            if (startOk && endOk) {
                ++count;
                if (at < first) { first = at; len = name.size(); }
            }
            at = end;
        }
    }
    if (firstAt) *firstAt = first;
    if (firstLen) *firstLen = len;
    return count;
}
}  // namespace

std::map<Id, std::vector<Id>> Library::appearancesIn(const Id& bookId) const {
    std::map<Id, std::vector<Id>> out;
    const Book* b = book(bookId);
    if (!b) return out;
    const auto entries = b->worldId.empty() ? entriesOf({}, bookId) : entriesOf(b->worldId);
    std::vector<std::vector<std::string>> names;
    for (const Entry* e : entries) {
        std::vector<std::string> n;
        if (trimmed(e->name).size() >= 2) n.push_back(trimmed(e->name));
        for (auto& a : e->aliases) if (a.size() >= 2) n.push_back(a);
        names.push_back(std::move(n));
    }
    for (const ManuscriptNode* n : manuscriptOf(bookId)) {
        const std::string text = doc::plainText(nodeContent(n->id));
        std::vector<std::pair<int, Id>> found;
        for (size_t i = 0; i < entries.size(); ++i)
            if (!names[i].empty())
                if (const int c = countNames(text, names[i]); c > 0) found.push_back({c, entries[i]->id});
        std::stable_sort(found.begin(), found.end(), [](auto& x, auto& y) { return x.first > y.first; });
        for (auto& f : found) out[n->id].push_back(f.second);
    }
    return out;
}

std::vector<Mention> Library::mentionsOf(const Entry& e) const {
    std::vector<Mention> out;
    std::vector<std::string> names;
    if (trimmed(e.name).size() >= 2) names.push_back(trimmed(e.name));
    for (auto& a : e.aliases) if (a.size() >= 2) names.push_back(a);
    if (names.empty()) return out;
    std::vector<const Book*> books;
    if (e.worldId.empty()) { if (const Book* b = book(e.bookId)) books.push_back(b); }
    else for (const Book* b : liveBooks()) if (b->worldId == e.worldId) books.push_back(b);
    for (const Book* b : books)
        for (const ManuscriptNode* n : manuscriptOf(b->id)) {
            const std::string text = doc::plainText(nodeContent(n->id));
            Mention m;
            m.bookId = b->id;
            m.nodeId = n->id;
            size_t first = std::string::npos, firstLen = 0;
            m.count = countNames(text, names, &first, &firstLen);
            if (!m.count) continue;
            // Some text either side, on one line.
            size_t a = first > 70 ? back(text, first - 70) : 0;
            size_t z = std::min(text.size(), first + firstLen + 90);
            z = back(text, z);
            std::string snip = text.substr(a, z - a);
            for (auto& c : snip) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            std::string tidy;
            for (char c : snip) if (!(c == ' ' && !tidy.empty() && tidy.back() == ' ')) tidy += c;
            m.snippet = (a > 0 ? "…" : "") + trimmed(tidy) + (z < text.size() ? "…" : "");
            out.push_back(std::move(m));
        }
    return out;
}

}  // namespace studio

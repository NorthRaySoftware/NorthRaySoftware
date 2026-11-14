// Worldbuilding entries and their relationships (schema 5), and finding where
// they're mentioned in the manuscripts.
#include "studio/document.hpp"
#include "studio/library.hpp"

#include "internal.hpp"

#include <algorithm>
#include <set>
#include <cctype>
#include <nlohmann/json.hpp>

namespace studio {

using namespace detail;
using nlohmann::json;

const std::vector<std::string>& entryKinds() {
    static const std::vector<std::string> k{"character", "race", "creature", "place", "realm", "group", "thing", "lore", "event"};
    return k;
}
const char* entryKindLabel(const std::string& k) {
    if (k == "realm") return "Realm or plane";
    if (k == "race") return "Race";
    if (k == "creature") return "Creature";
    if (k == "place") return "Place";
    if (k == "group") return "Group";
    if (k == "thing") return "Thing";
    if (k == "lore") return "Lore";
    if (k == "event") return "Event";
    return "Character";
}
const char* entryKindPlural(const std::string& k) {
    if (k == "realm") return "Realms and planes";
    if (k == "race") return "Races";
    if (k == "creature") return "Creatures";
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

// Every relationship and how it reads from the other side. Both directions are listed, so either
// can be chosen and the other entry shows the reverse.
const std::vector<Relation>& relations() {
    static const std::vector<Relation> r{
        // People
        {"Family", "Family"},          {"Parent of", "Child of"},       {"Child of", "Parent of"},     {"Sibling of", "Sibling of"},
        {"Partner of", "Partner of"},  {"Married to", "Married to"},    {"Loves", "Loved by"},          {"Loved by", "Loves"},
        {"Friend of", "Friend of"},    {"Ally of", "Ally of"},          {"Rival of", "Rival of"},      {"Enemy of", "Enemy of"},
        {"Mentor of", "Student of"},   {"Student of", "Mentor of"},     {"Serves", "Served by"},       {"Served by", "Serves"},
        {"Member of", "Has member"},   {"Has member", "Member of"},     {"Leader of", "Led by"},       {"Led by", "Leader of"},
        {"Rules", "Ruled by"},         {"Ruled by", "Rules"},           {"Lives in", "Home of"},       {"Home of", "Lives in"},
        {"Born in", "Birthplace of"},  {"Birthplace of", "Born in"},    {"Founded", "Founded by"},     {"Founded by", "Founded"},
        // Creatures
        {"Rides", "Ridden by"},        {"Ridden by", "Rides"},          {"Tames", "Tamed by"},         {"Tamed by", "Tames"},
        {"Hunts", "Hunted by"},        {"Hunted by", "Hunts"},          {"Native to", "Native home of"}, {"Native home of", "Native to"},
        {"Kind of", "Has kind"},       {"Has kind", "Kind of"},
        // Places and things
        {"Located in", "Contains"},    {"Contains", "Located in"},      {"Borders", "Borders"},        {"Capital of", "Has capital"},
        {"Has capital", "Capital of"}, {"Road to", "Road to"},          {"Trades with", "Trades with"}, {"At war with", "At war with"},
        {"Based in", "Base of"},       {"Base of", "Based in"},         {"Part of", "Includes"},       {"Includes", "Part of"},
        {"Owns", "Owned by"},          {"Owned by", "Owns"},            {"Made", "Made by"},           {"Made by", "Made"},
        {"Guards", "Guarded by"},      {"Guarded by", "Guards"},        {"Named after", "Gave its name to"},
        {"Gave its name to", "Named after"},
        // Lore and events
        {"Believes in", "Believed by"}, {"Believed by", "Believes in"}, {"Practises", "Practised by"}, {"Practised by", "Practises"},
        {"Comes from", "Origin of"},   {"Origin of", "Comes from"},     {"Explains", "Explained by"},  {"Explained by", "Explains"},
        {"Took part in", "Involved"},  {"Involved", "Took part in"},    {"Took place in", "Scene of"}, {"Scene of", "Took place in"},
        {"Caused", "Caused by"},       {"Caused by", "Caused"},         {"Led to", "Followed from"},   {"Followed from", "Led to"},
        {"Related to", "Related to"},
    };
    return r;
}

// The relationships that make sense from an entry of this kind, each with the kinds it usually links to.
const std::vector<RelationChoice>& relationsFor(const std::string& kind) {
    static const std::vector<RelationChoice> character{
        {"Family", "character"}, {"Parent of", "character"}, {"Child of", "character"}, {"Sibling of", "character"},
        {"Partner of", "character"}, {"Married to", "character"}, {"Loves", "character"}, {"Friend of", "character"},
        {"Ally of", "character group"}, {"Rival of", "character group"}, {"Enemy of", "character group"},
        {"Mentor of", "character"}, {"Student of", "character"}, {"Serves", "character group"},
        {"Member of", "group race"}, {"Leader of", "group race"}, {"Rules", "place group realm"}, {"Lives in", "place realm"}, {"Born in", "place realm"},
        {"Founded", "place group"}, {"Owns", "thing place"}, {"Made", "thing"}, {"Believes in", "lore"}, {"Practises", "lore"},
        {"Rides", "creature"}, {"Tames", "creature"}, {"Hunts", "creature character"},
        {"Took part in", "event"}, {"Caused", "event"}, {"Related to", ""}};
    static const std::vector<RelationChoice> realm{
        {"Contains", "place realm"}, {"Part of", "realm"}, {"Includes", "realm"}, {"Borders", "realm"}, {"Ruled by", "character group race"},
        {"Home of", "character race creature group"}, {"Native home of", "creature race"}, {"Origin of", "lore thing character race"},
        {"Road to", "realm place"}, {"At war with", "realm group race"}, {"Ally of", "realm group race"}, {"Scene of", "event"},
        {"Explained by", "lore"}, {"Related to", ""}};
    static const std::vector<RelationChoice> race{
        {"Has member", "character"}, {"Led by", "character group"}, {"Native to", "place"}, {"Lives in", "place"},
        {"Rules", "place"}, {"Ally of", "race group place"}, {"Rival of", "race group"}, {"Enemy of", "race group place"},
        {"Kind of", "race"}, {"Has kind", "race"}, {"Comes from", "place lore"}, {"Believes in", "lore"}, {"Practises", "lore"},
        {"Made by", "character race"}, {"Took part in", "event"}, {"Related to", ""}};
    static const std::vector<RelationChoice> creature{
        {"Native to", "place"}, {"Lives in", "place"}, {"Ridden by", "character"}, {"Tamed by", "character group"},
        {"Hunts", "creature character"}, {"Hunted by", "character group creature"}, {"Guards", "place thing character"},
        {"Serves", "character group"}, {"Made by", "character group"}, {"Kind of", "creature"}, {"Has kind", "creature"},
        {"Parent of", "creature"}, {"Child of", "creature"}, {"Enemy of", "creature character group"},
        {"Ally of", "creature character group"}, {"Comes from", "place lore"}, {"Took part in", "event"}, {"Related to", ""}};
    static const std::vector<RelationChoice> place{
        {"Located in", "place realm"}, {"Contains", "place thing"}, {"Borders", "place"}, {"Capital of", "place"},
        {"Road to", "place"}, {"Part of", "place"}, {"Home of", "character group"}, {"Birthplace of", "character"},
        {"Ruled by", "character group"}, {"Owned by", "character group"}, {"Founded by", "character group"},
        {"Base of", "group"}, {"Guarded by", "character group thing"}, {"Trades with", "place group"},
        {"At war with", "place group"}, {"Ally of", "place group"}, {"Named after", ""}, {"Scene of", "event"},
        {"Native home of", "creature"}, {"Origin of", "lore thing character creature"}, {"Related to", ""}};
    static const std::vector<RelationChoice> group{
        {"Has member", "character"}, {"Led by", "character"}, {"Based in", "place"}, {"Rules", "place group"},
        {"Ally of", "group character place"}, {"Rival of", "group character"}, {"Enemy of", "group character place"},
        {"Serves", "character group"}, {"Part of", "group"}, {"Includes", "group"}, {"Founded by", "character"},
        {"Owns", "thing place"}, {"Believes in", "lore"}, {"Practises", "lore"}, {"Took part in", "event"},
        {"Caused", "event"}, {"Related to", ""}};
    static const std::vector<RelationChoice> thing{
        {"Owned by", "character group"}, {"Made by", "character group"}, {"Located in", "place"},
        {"Guarded by", "character group"}, {"Guards", "place thing"}, {"Part of", "thing"}, {"Includes", "thing"},
        {"Comes from", "place lore"}, {"Named after", ""}, {"Took part in", "event"}, {"Caused", "event"},
        {"Related to", ""}};
    static const std::vector<RelationChoice> lore{
        {"Believed by", "character group place"}, {"Practised by", "character group"}, {"Comes from", "place group event"},
        {"Explains", ""}, {"Explained by", "lore"}, {"Part of", "lore"}, {"Includes", "lore"}, {"Origin of", "thing character"},
        {"Related to", ""}};
    static const std::vector<RelationChoice> event{
        {"Involved", "character group thing"}, {"Took place in", "place"}, {"Caused by", "character group event thing"},
        {"Led to", "event"}, {"Followed from", "event"}, {"Part of", "event"}, {"Includes", "event"},
        {"Origin of", "lore thing"}, {"Related to", ""}};
    if (kind == "realm") return realm;
    if (kind == "race") return race;
    if (kind == "creature") return creature;
    if (kind == "place") return place;
    if (kind == "group") return group;
    if (kind == "thing") return thing;
    if (kind == "lore") return lore;
    if (kind == "event") return event;
    return character;
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
// Longer names first, and a place counts once ("Maren Holt" isn't also "Maren" and "Holt").
int countNames(const std::string& text, const std::vector<std::string>& namesIn, size_t* firstAt = nullptr, size_t* firstLen = nullptr) {
    std::vector<std::string> names = namesIn;
    std::stable_sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    std::vector<std::pair<size_t, size_t>> taken;
    int count = 0;
    size_t first = std::string::npos, len = 0;
    for (const std::string& name : names) {
        if (name.empty()) continue;
        size_t at = 0;
        while ((at = text.find(name, at)) != std::string::npos) {
            const bool startOk = at == 0 || !wordByte((unsigned char)text[at - 1]);
            const size_t end = at + name.size();
            const bool endOk = end >= text.size() || !wordByte((unsigned char)text[end]);
            bool overlaps = false;
            for (auto& [a, z] : taken) if (at < z && end > a) { overlaps = true; break; }
            if (startOk && endOk && !overlaps) {
                ++count;
                taken.push_back({at, end});
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

// The parts of a character's full name that also stand for them: the first and last names on
// their own, and runs that include one of them ("Snarf Jill Grave": Snarf, Grave, Snarf Jill,
// Jill Grave, Snarf Grave; not Jill alone). Joining words and titles don't count, and a part
// must start with a capital (or a non-ASCII letter). Same rule as the editor's links.
std::vector<std::string> nameParts(const std::string& full) {
    static const std::set<std::string> particles = {"of", "the", "de", "da", "di", "du", "del", "della", "der", "den", "van", "von", "le",
                                                    "la", "al", "el", "bin", "bint", "ibn", "ap", "mac", "y", "and"};
    static const std::set<std::string> titles = {"mr", "mrs", "ms", "miss", "dr", "sir", "dame", "lord", "lady", "king", "queen", "prince",
                                                 "princess", "captain", "capt", "general", "father", "mother", "brother", "sister",
                                                 "master", "mistress", "saint", "st", "professor", "prof"};
    std::vector<std::string> words;
    for (size_t i = 0; i < full.size();) {
        while (i < full.size() && std::isspace((unsigned char)full[i])) ++i;
        size_t j = i;
        while (j < full.size() && !std::isspace((unsigned char)full[j])) ++j;
        if (j > i) words.push_back(full.substr(i, j - i));
        i = j;
    }
    std::vector<std::string> out;
    if (words.size() < 2) return out;
    std::string whole;
    for (auto& w : words) whole += (whole.empty() ? "" : " ") + w;
    auto lower = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); if (!s.empty() && s.back() == '.') s.pop_back(); return s; };
    auto isName = [&](const std::string& w) {
        const unsigned char c = (unsigned char)w[0];
        return (std::isupper(c) || c >= 0x80) && !particles.count(lower(w)) && !titles.count(lower(w));
    };
    auto add = [&](const std::string& s) { if (s.size() >= 2 && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s); };
    size_t firstAt = words.size(), lastAt = words.size();
    for (size_t i = 0; i < words.size(); ++i) if (isName(words[i])) { if (firstAt == words.size()) firstAt = i; lastAt = i; }
    for (size_t a = 0; a < words.size(); ++a)
        for (size_t z = a; z < words.size(); ++z) {
            if (a == 0 && z == words.size() - 1) continue;
            if (!isName(words[a]) || !isName(words[z])) continue;
            if (a != firstAt && z != lastAt) continue;   // a middle name alone isn't used for the person
            std::string p;
            for (size_t k = a; k <= z; ++k) p += (k > a ? " " : "") + words[k];
            add(p);
        }
    // Skipping middle names too: "Albus Dumbledore", "Albus Brian Dumbledore" (in order, with the first or last).
    std::vector<std::string> named;
    for (auto& w : words) if (isName(w)) named.push_back(w);
    if (named.size() > 8) named.erase(named.begin() + 4, named.end() - 4);
    const size_t k = named.size();
    if (k >= 2)
        for (unsigned mask = 1; mask < (1u << k); ++mask) {
            if (!(mask & 1u) && !(mask & (1u << (k - 1)))) continue;
            std::string p;
            for (size_t i = 0; i < k; ++i) if (mask & (1u << i)) p += (p.empty() ? "" : " ") + named[i];
            if (p != whole) add(p);
        }
    return out;
}

// Every name an entry goes by in the text: its name, other names, and for a character the parts of them.
std::vector<std::string> searchNames(const Entry& e) {
    std::vector<std::string> n;
    if (trimmed(e.name).size() >= 2) n.push_back(trimmed(e.name));
    for (auto& a : e.aliases) if (a.size() >= 2) n.push_back(a);
    if (e.kind == "character") {
        const size_t given = n.size();
        for (size_t i = 0; i < given; ++i)
            for (auto& p : nameParts(n[i])) if (std::find(n.begin(), n.end(), p) == n.end()) n.push_back(p);
    }
    // Either apostrophe: the bible's Zah'mes is the text's Zah’mes, and the other way round.
    {
        const size_t given = n.size();
        auto swapAll = [](std::string t, const std::string& from, const std::string& to) {
            for (size_t at = t.find(from); at != std::string::npos; at = t.find(from, at + to.size())) t.replace(at, from.size(), to);
            return t;
        };
        for (size_t i = 0; i < given; ++i) {
            for (const std::string& v : {swapAll(n[i], "'", "\xE2\x80\x99"), swapAll(n[i], "\xE2\x80\x99", "'")})
                if (v != n[i] && std::find(n.begin(), n.end(), v) == n.end()) n.push_back(v);
        }
    }
    // Shouted: "JACKSON!" (ASCII letters upper-cased; others are left as they are).
    const size_t plain = n.size();
    for (size_t i = 0; i < plain; ++i) {
        std::string up = n[i];
        for (auto& c : up) c = (char)std::toupper((unsigned char)c);
        if (up != n[i] && std::find(n.begin(), n.end(), up) == n.end()) n.push_back(up);
    }
    return n;
}

std::vector<NameSpan> findNames(const std::string& text, const std::vector<const Entry*>& entries) {
    // Every (name, entry) pair, longest names first.
    std::vector<std::pair<std::string, Id>> all;
    for (const Entry* e : entries)
        for (auto& n : searchNames(*e)) all.push_back({n, e->id});
    std::stable_sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
    std::vector<NameSpan> out;
    for (auto& [name, id] : all) {
        size_t at = 0;
        while ((at = text.find(name, at)) != std::string::npos) {
            const size_t end = at + name.size();
            const bool whole = (at == 0 || !wordByte((unsigned char)text[at - 1])) && (end >= text.size() || !wordByte((unsigned char)text[end]));
            if (whole) {
                bool merged = false, overlaps = false;
                for (auto& s : out) {
                    if (s.start == at && s.end == end) {   // the same words: another entry with this name
                        if (std::find(s.ids.begin(), s.ids.end(), id) == s.ids.end()) s.ids.push_back(id);
                        merged = true;
                        break;
                    }
                    if (at < s.end && end > s.start) { overlaps = true; break; }
                }
                if (!merged && !overlaps) out.push_back({at, end, {id}});
            }
            at = end;
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.start < b.start; });
    return out;
}

std::map<Id, std::vector<Id>> Library::appearancesIn(const Id& bookId) const {
    std::map<Id, std::vector<Id>> out;
    const Book* b = book(bookId);
    if (!b) return out;
    const auto entries = b->worldId.empty() ? entriesOf({}, bookId) : entriesOf(b->worldId);
    std::vector<std::vector<std::string>> names;
    for (const Entry* e : entries) names.push_back(searchNames(*e));
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
    const std::vector<std::string> names = searchNames(e);
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

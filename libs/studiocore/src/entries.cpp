// Worldbuilding entries and their relationships (schema 5), and finding where
// they're mentioned in the manuscripts.
#include "studio/words.hpp"
#include "studio/document.hpp"
#include "studio/library.hpp"

#include "internal.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <set>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <cctype>
#include <nlohmann/json.hpp>

namespace studio {

using namespace detail;
using nlohmann::json;

const std::vector<std::string>& entryKinds() {
    static const std::vector<std::string> k{"character", "race", "creature", "place", "realm", "group", "thing", "lore", "magic", "event"};
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
    if (k == "magic") return "Magic or power";
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
    if (k == "magic") return "Magic and powers";
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
        {"Twin of", "Twin of"},        {"Half-sibling of", "Half-sibling of"}, {"Step-sibling of", "Step-sibling of"},
        {"Adoptive sibling of", "Adoptive sibling of"}, {"Foster sibling of", "Foster sibling of"},
        {"Adoptive parent of", "Adopted child of"}, {"Adopted child of", "Adoptive parent of"},
        {"Step-parent of", "Stepchild of"}, {"Stepchild of", "Step-parent of"},
        {"Foster parent of", "Foster child of"}, {"Foster child of", "Foster parent of"},
        {"Partner of", "Partner of"},  {"Married to", "Married to"},    {"Engaged to", "Engaged to"},  {"Former partner of", "Former partner of"},
        {"Triplet of", "Triplet of"},    {"Loves", "Loved by"},          {"Loved by", "Loves"},
        {"Friend of", "Friend of"},    {"Ally of", "Ally of"},          {"Rival of", "Rival of"},      {"Enemy of", "Enemy of"},
        {"Mentor of", "Student of"},   {"Student of", "Mentor of"},     {"Serves", "Served by"},       {"Served by", "Serves"},
        {"Member of", "Has member"},   {"Has member", "Member of"},     {"Leader of", "Led by"},       {"Led by", "Leader of"},
        {"Rules", "Ruled by"},         {"Ruled by", "Rules"},           {"Lives in", "Home of"},       {"Home of", "Lives in"},
        {"From", "Hometown of"},       {"Hometown of", "From"},
        {"Born in", "Birthplace of"},  {"Birthplace of", "Born in"},    {"Founded", "Founded by"},     {"Founded by", "Founded"},
        // Creatures
        {"Rides", "Ridden by"},        {"Ridden by", "Rides"},          {"Tames", "Tamed by"},         {"Tamed by", "Tames"},
        {"Hunts", "Hunted by"},        {"Hunted by", "Hunts"},          {"Native to", "Native home of"}, {"Native home of", "Native to"},
        {"Kind of", "Has kind"},       {"Has kind", "Kind of"},
        // Places and things
        {"Located in", "Contains"},    {"Contains", "Located in"},      {"Borders", "Borders"},        {"Capital of", "Has capital"},
        {"Has capital", "Capital of"}, {"Road to", "Road to"},          {"Trades with", "Trades with"}, {"At war with", "At war with"},
        {"Based in", "Base of"},       {"Base of", "Based in"},         {"Part of", "Includes"},       {"Includes", "Part of"},
        {"Used", "Used by"},           {"Used by", "Used"},
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
        {"Twin of", "character"}, {"Triplet of", "character"}, {"Half-sibling of", "character"}, {"Step-sibling of", "character"},
        {"Adoptive sibling of", "character"}, {"Foster sibling of", "character"},
        {"Adoptive parent of", "character"}, {"Adopted child of", "character"}, {"Step-parent of", "character"},
        {"Stepchild of", "character"}, {"Foster parent of", "character"}, {"Foster child of", "character"},
        {"Partner of", "character"}, {"Married to", "character"}, {"Engaged to", "character"}, {"Former partner of", "character"},
        {"Loves", "character"}, {"Friend of", "character"},
        {"Ally of", "character group"}, {"Rival of", "character group"}, {"Enemy of", "character group"},
        {"Mentor of", "character"}, {"Student of", "character"}, {"Serves", "character group"},
        {"Member of", "group race"}, {"Leader of", "group race"}, {"Rules", "place group realm"}, {"From", "place realm"}, {"Lives in", "place realm"}, {"Born in", "place realm"},
        {"Founded", "place group"}, {"Owns", "thing place"}, {"Used", "thing magic"}, {"Made", "thing"}, {"Believes in", "lore"}, {"Practises", "lore magic"},
        {"Rides", "creature"}, {"Tames", "creature"}, {"Hunts", "creature character"},
        {"Took part in", "event"}, {"Caused", "event"}, {"Related to", ""}};
    static const std::vector<RelationChoice> realm{
        {"Contains", "place realm"}, {"Part of", "realm"}, {"Includes", "realm"}, {"Borders", "realm"}, {"Ruled by", "character group race"},
        {"Home of", "character race creature group"}, {"Hometown of", "character"}, {"Native home of", "creature race"}, {"Origin of", "lore thing character race"},
        {"Road to", "realm place"}, {"At war with", "realm group race"}, {"Ally of", "realm group race"}, {"Scene of", "event"},
        {"Founded by", "character group race"},
        {"Explained by", "lore"}, {"Related to", ""}};
    static const std::vector<RelationChoice> race{
        {"Has member", "character"}, {"Led by", "character group"}, {"Native to", "place"}, {"Lives in", "place"},
        {"Rules", "place"}, {"Ally of", "race group place"}, {"Rival of", "race group"}, {"Enemy of", "race group place"},
        {"Kind of", "race"}, {"Has kind", "race"}, {"Comes from", "place lore"}, {"Founded", "place group realm"}, {"Origin of", "lore thing magic"}, {"Believes in", "lore"}, {"Practises", "lore"},
        {"Made by", "character race"}, {"Took part in", "event"}, {"Related to", ""}};
    static const std::vector<RelationChoice> creature{
        {"Native to", "place"}, {"Lives in", "place"}, {"Ridden by", "character"}, {"Tamed by", "character group"},
        {"Hunts", "creature character"}, {"Hunted by", "character group creature"}, {"Guards", "place thing character"},
        {"Serves", "character group"}, {"Made by", "character group"}, {"Kind of", "creature"}, {"Has kind", "creature"},
        {"Parent of", "creature"}, {"Child of", "creature"}, {"Sibling of", "creature"}, {"Twin of", "creature"},
        {"Triplet of", "creature"}, {"Half-sibling of", "creature"}, {"Partner of", "creature"}, {"Former partner of", "creature"},
        {"Mentor of", "creature character"}, {"Used by", "character group"}, {"Origin of", "lore thing"}, {"Enemy of", "creature character group"},
        {"Ally of", "creature character group"}, {"Comes from", "place lore"}, {"Took part in", "event"}, {"Related to", ""}};
    static const std::vector<RelationChoice> place{
        {"Located in", "place realm"}, {"Contains", "place thing"}, {"Borders", "place"}, {"Capital of", "place"},
        {"Road to", "place"}, {"Part of", "place"}, {"Home of", "character group"}, {"Hometown of", "character"}, {"Birthplace of", "character"},
        {"Ruled by", "character group"}, {"Owned by", "character group"}, {"Founded by", "character group"},
        {"Base of", "group"}, {"Guarded by", "character group thing"}, {"Trades with", "place group"},
        {"At war with", "place group"}, {"Ally of", "place group"}, {"Named after", ""}, {"Scene of", "event"},
        {"Native home of", "creature"}, {"Origin of", "lore thing character creature"}, {"Related to", ""}};
    static const std::vector<RelationChoice> group{
        {"Has member", "character"}, {"Led by", "character"}, {"Based in", "place"}, {"Rules", "place group"},
        {"Ally of", "group character place"}, {"Rival of", "group character"}, {"Enemy of", "group character place"},
        {"Serves", "character group"}, {"Part of", "group"}, {"Includes", "group"}, {"Founded by", "character"},
        {"Founded", "place group"}, {"Used", "thing magic"}, {"Mentor of", "character"}, {"Origin of", "lore thing"},
        {"Owns", "thing place"}, {"Believes in", "lore"}, {"Practises", "lore"}, {"Took part in", "event"},
        {"Caused", "event"}, {"Related to", ""}};
    static const std::vector<RelationChoice> thing{
        {"Owned by", "character group"}, {"Made by", "character group"}, {"Used by", "character group creature"}, {"Located in", "place"},
        {"Guarded by", "character group"}, {"Guards", "place thing"}, {"Part of", "thing"}, {"Includes", "thing"},
        {"Comes from", "place lore"}, {"Named after", ""}, {"Took part in", "event"}, {"Caused", "event"},
        {"Origin of", "lore thing event"}, {"Scene of", "event"}, {"Related to", ""}};
    static const std::vector<RelationChoice> lore{
        {"Believed by", "character group place"}, {"Practised by", "character group"}, {"Comes from", "place group event"},
        {"Explains", ""}, {"Explained by", "lore"}, {"Part of", "lore"}, {"Includes", "lore"}, {"Origin of", "thing character"},
        {"Founded by", "character group"}, {"Used by", "character group"},
        {"Related to", ""}};
    static const std::vector<RelationChoice> magic{
        {"Practised by", "character group race"}, {"Used by", "character group race creature"}, {"Origin of", "thing lore"},
        {"Comes from", "place realm lore thing event"}, {"Part of", "magic lore"},
        {"Includes", "magic"}, {"Explained by", "lore"}, {"Made", "thing"}, {"Enemy of", "magic group"}, {"Related to", ""}};
    static const std::vector<RelationChoice> event{
        {"Involved", "character group thing"}, {"Took place in", "place"}, {"Caused by", "character group event thing"},
        {"Led to", "event"}, {"Followed from", "event"}, {"Part of", "event"}, {"Includes", "event"},
        {"Origin of", "lore thing"}, {"Scene of", "event"}, {"Founded", "place group"}, {"Related to", ""}};
    if (kind == "realm") return realm;
    if (kind == "race") return race;
    if (kind == "creature") return creature;
    if (kind == "place") return place;
    if (kind == "realm") return realm;
    if (kind == "group") return group;
    if (kind == "thing") return thing;
    if (kind == "lore") return lore;
    if (kind == "magic") return magic;
    if (kind == "event") return event;
    return character;
}

bool isFamilyRelation(const std::string& l) {
    static const std::set<std::string> family{"Parent of", "Child of", "Partner of", "Married to", "Engaged to", "Former partner of",
                                              "Sibling of", "Twin of", "Triplet of", "Half-sibling of", "Step-sibling of",
                                              "Adoptive sibling of", "Foster sibling of", "Adoptive parent of", "Adopted child of",
                                              "Step-parent of", "Stepchild of", "Foster parent of", "Foster child of"};
    return family.count(l) > 0;
}

std::string reverseRelation(const std::string& label) {
    for (auto& r : relations()) if (label == r.label) return r.reverse;
    return label;
}

// ----------------------------------------------------------------- entry pages

namespace {
const std::string kNone;
}

const std::string& Entry::section(const std::string& title) const {
    for (auto& s : sections) if (s.first == title) return s.second;
    return kNone;
}

void Entry::setSection(const std::string& title, const std::string& text) {
    for (auto& s : sections) if (s.first == title) { s.second = text; return; }
    if (!text.empty()) sections.emplace_back(title, text);
}

const Id& Entry::revealedIn(const std::string& key) const {
    for (auto& r : revealed) if (r.first == key) return r.second;
    return kNone;
}

void Entry::setRevealedIn(const std::string& key, const Id& book) {
    for (size_t i = 0; i < revealed.size(); ++i)
        if (revealed[i].first == key) {
            if (book.empty()) revealed.erase(revealed.begin() + (long)i); else revealed[i].second = book;
            return;
        }
    if (!book.empty()) revealed.emplace_back(key, book);
}

const std::string& Entry::bookNote(const Id& book) const {
    for (auto& n : bookNotes) if (n.first == book) return n.second;
    return kNone;
}

void Entry::setBookNote(const Id& book, const std::string& text) {
    for (size_t i = 0; i < bookNotes.size(); ++i)
        if (bookNotes[i].first == book) {
            if (text.empty()) bookNotes.erase(bookNotes.begin() + (long)i); else bookNotes[i].second = text;
            return;
        }
    if (!text.empty() && !book.empty()) bookNotes.emplace_back(book, text);
}

const std::vector<std::string>& defaultEntryTemplate(const std::string& kind) {
    static const std::vector<std::string> character{"Appearance & personality", "Backstory & goals", "Abilities & powers", "Age, birth & status"};
    static const std::vector<std::string> creature{"Appearance", "Habitat & range", "Behaviour & diet", "Abilities", "Life cycle"};
    static const std::vector<std::string> race{"Appearance", "Homeland", "Culture & customs", "Language & names", "Abilities", "History"};
    static const std::vector<std::string> place{"Description", "History", "Who lives there", "Notable spots"};
    static const std::vector<std::string> realm{"Description", "Rulers & peoples", "Lands within", "History"};
    static const std::vector<std::string> group{"Purpose & beliefs", "Leaders & members", "History", "Symbols & colours"};
    static const std::vector<std::string> thing{"Description", "Origin", "Powers & uses", "Where it is now"};
    static const std::vector<std::string> lore{"What it is", "Details", "History"};
    static const std::vector<std::string> magic{"How it works", "Rules", "Costs & limits", "Who can use it", "What it can't do", "History"};
    static const std::vector<std::string> event{"What happened", "Causes", "Who was there", "What came of it"};
    static const std::vector<std::string> none;
    if (kind == "character") return character;
    if (kind == "creature") return creature;
    if (kind == "race") return race;
    if (kind == "place") return place;
    if (kind == "realm") return realm;
    if (kind == "group") return group;
    if (kind == "thing") return thing;
    if (kind == "lore") return lore;
    if (kind == "magic") return magic;
    if (kind == "event") return event;
    return none;
}

std::vector<std::string> parseEntryTemplate(const std::string& lines) {
    std::vector<std::string> out;
    std::istringstream in(lines);
    std::string l;
    while (std::getline(in, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
        size_t i = 0;
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t' || l[i] == '-' || l[i] == '*')) ++i;
        l = l.substr(i);
        if (l.empty() || std::find(out.begin(), out.end(), l) != out.end()) continue;
        out.push_back(l.substr(0, sectionSpec(l).options.empty() ? 80 : 600));
    }
    return out;
}

namespace {
std::string optionKey(const std::string& s) {
    std::string out;
    for (char c : trimmed(s)) out += (char)std::tolower((unsigned char)c);
    return out;
}
std::vector<std::string> pickedParts(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text + ",") {
        if (c == ',' || c == '\n') { if (!trimmed(cur).empty()) out.push_back(trimmed(cur)); cur.clear(); }
        else cur += c;
    }
    return out;
}
}  // namespace

SectionSpec sectionSpec(const std::string& line) {
    SectionSpec s;
    const size_t colon = line.find(':');
    const size_t bar = line.find('|');
    if (colon == std::string::npos || bar == std::string::npos || bar < colon) { s.title = trimmed(line); return s; }
    s.title = trimmed(line.substr(0, colon));
    std::string rest = line.substr(colon + 1) + "|";
    std::string cur;
    for (char c : rest) {
        if (c == '|') {
            const std::string o = trimmed(cur);
            if (!o.empty() && o.size() <= 60 && s.options.size() < 24 &&
                std::none_of(s.options.begin(), s.options.end(), [&](const std::string& x) { return optionKey(x) == optionKey(o); }))
                s.options.push_back(o);
            cur.clear();
        } else cur += c;
    }
    // "(any)" or "(several)" after the title: more than one can be picked.
    for (const char* mark : {"(any)", "(several)", "(pick any)", "(many)"}) {
        const std::string m = mark;
        if (s.title.size() > m.size() && optionKey(s.title.substr(s.title.size() - m.size())) == m) {
            s.many = true;
            s.title = trimmed(s.title.substr(0, s.title.size() - m.size()));
        }
    }
    if (s.title.empty()) s.title = trimmed(line);
    if (s.options.size() < 2) { s.options.clear(); s.many = false; s.title = trimmed(line); }
    return s;
}

bool sectionOptionPicked(const SectionSpec& spec, const std::string& text, const std::string& option) {
    if (!spec.many) return optionKey(text) == optionKey(option);
    for (auto& p : pickedParts(text)) if (optionKey(p) == optionKey(option)) return true;
    return false;
}

std::string toggleSectionOption(const SectionSpec& spec, const std::string& text, const std::string& option) {
    if (!spec.many) return sectionOptionPicked(spec, text, option) ? std::string() : option;
    const bool was = sectionOptionPicked(spec, text, option);
    std::vector<std::string> parts = pickedParts(text), out;
    for (auto& o : spec.options) {
        const bool on = optionKey(o) == optionKey(option) ? !was : sectionOptionPicked(spec, text, o);
        if (on) out.push_back(o);
    }
    for (auto& p : parts)   // written words that aren't one of the options stay
        if (std::none_of(spec.options.begin(), spec.options.end(), [&](const std::string& o) { return optionKey(o) == optionKey(p); })) out.push_back(p);
    std::string r;
    for (auto& o : out) r += (r.empty() ? "" : ", ") + o;
    return r;
}

std::string entryTemplateText(const std::vector<std::string>& sections) {
    std::string s;
    for (auto& t : sections) { if (!s.empty()) s += "\n"; s += t; }
    return s;
}

std::vector<std::string> Library::entryTemplate(const Id& worldId, const std::string& kind) const {
    const std::string own = setting("entry-template:" + worldId + ":" + kind);
    if (!own.empty()) return parseEntryTemplate(own);
    return defaultEntryTemplate(kind);
}

bool Library::setEntryTemplate(const Id& worldId, const std::string& kind, const std::vector<std::string>& sections) {
    // Empty means "back to the built-in one"; a single space keeps an intentionally empty template.
    const bool same = sections == defaultEntryTemplate(kind);
    return setSetting("entry-template:" + worldId + ":" + kind, same ? std::string() : sections.empty() ? std::string(" ") : entryTemplateText(sections));
}

std::vector<std::pair<std::string, std::string>> Library::entrySections(const Entry& e) const {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& t : entryTemplate(e.worldId, e.kind)) {
        const std::string title = sectionSpec(t).title;
        if (std::none_of(out.begin(), out.end(), [&](auto& o) { return o.first == title; })) out.emplace_back(title, e.section(title));
    }
    for (auto& s : e.sections)
        if (!s.second.empty() && std::none_of(out.begin(), out.end(), [&](auto& o) { return o.first == s.first; })) out.push_back(s);
    return out;
}

SectionSpec Library::entrySectionSpec(const Entry& e, const std::string& title) const {
    for (auto& t : entryTemplate(e.worldId, e.kind)) {
        SectionSpec s = sectionSpec(t);
        if (s.title == title) return s;
    }
    SectionSpec s;
    s.title = title;
    return s;
}

namespace detail {

std::string entryPageText(const Entry& e) {
    json j = json::object();
    json sec = json::array();
    for (auto& s : e.sections) if (!s.second.empty()) sec.push_back({s.first, s.second});
    if (!sec.empty()) j["sections"] = sec;
    json g = json::array();
    for (auto& p : e.gallery) {
        json o{{"blob", p.blob}};
        if (!p.caption.empty()) o["caption"] = p.caption;
        g.push_back(o);
    }
    if (!g.empty()) j["gallery"] = g;
    if (!e.pronunciation.empty()) j["say"] = e.pronunciation;
    if (!e.revealed.empty()) {
        json r = json::object();
        for (auto& [k, b] : e.revealed) if (!b.empty()) r[k] = b;
        if (!r.empty()) j["revealed"] = r;
    }
    if (!e.mapBlob.empty()) {
        json pins = json::array();
        for (auto& p : e.pins) if (!p.entry.empty()) pins.push_back({{"entry", p.entry}, {"x", p.x}, {"y", p.y}});
        j["map"] = {{"blob", e.mapBlob}, {"pins", pins}};
    }
    json n = json::object();
    for (auto& b : e.bookNotes) if (!b.second.empty()) n[b.first] = b.second;
    if (!n.empty()) j["bookNotes"] = n;
    return j.dump();
}

void readEntryPage(Entry& e, const std::string& text) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    if (auto s = j.find("sections"); s != j.end() && s->is_array())
        for (auto& x : *s)
            if (x.is_array() && x.size() == 2 && x[0].is_string() && x[1].is_string()) e.sections.emplace_back(x[0].get<std::string>(), x[1].get<std::string>());
    if (auto g = j.find("gallery"); g != j.end() && g->is_array())
        for (auto& x : *g)
            if (x.is_object() && x.value("blob", std::string()).size())
                e.gallery.push_back({x.value("blob", std::string()), x.value("caption", std::string())});
    e.pronunciation = j.value("say", std::string());
    if (auto r = j.find("revealed"); r != j.end() && r->is_object())
        for (auto it = r->begin(); it != r->end(); ++it) if (it->is_string()) e.revealed.emplace_back(it.key(), it->get<std::string>());
    if (auto m = j.find("map"); m != j.end() && m->is_object()) {
        e.mapBlob = m->value("blob", std::string());
        if (auto ps = m->find("pins"); ps != m->end() && ps->is_array())
            for (auto& p : *ps)
                if (p.is_object() && p.value("entry", std::string()).size()) {
                    const double x = p.value("x", 0.5), y = p.value("y", 0.5);
                    e.pins.push_back({p.value("entry", std::string()), std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)});
                }
    }
    if (auto n = j.find("bookNotes"); n != j.end() && n->is_object())
        for (auto it = n->begin(); it != n->end(); ++it) if (it->is_string()) e.bookNotes.emplace_back(it.key(), it->get<std::string>());
}

}  // namespace detail

// ----------------------------------------------------------------- storage

bool Library::loadEntries() {
    entries_.clear();
    links_.clear();
    entryAt_.clear();
    entryAtSize_ = (std::size_t)-1;
    linksChanged();
    {
        Stmt q(db_, "SELECT id, world_id, book_id, kind, name, aliases, summary, details, notes, image_blob, sort_order, created, modified, deleted, page "
                    "FROM entries ORDER BY name COLLATE NOCASE");
        if (!q) { error_ = sqlite3_errmsg(db_); return false; }
        while (q.row()) {
            Entry e;
            e.id = q.text(0); e.worldId = q.text(1); e.bookId = q.text(2); e.kind = knownEntryKind(q.text(3)); e.name = q.text(4);
            const json a = json::parse(q.text(5), nullptr, false);
            if (a.is_array()) for (auto& x : a) if (x.is_string()) e.aliases.push_back(x.get<std::string>());
            e.summary = q.text(6); e.details = q.text(7); e.notes = q.text(8); e.imageBlob = q.text(9);
            e.order = q.dbl(10); e.created = q.i64(11); e.modified = q.i64(12); e.deleted = q.i64(13);
            detail::readEntryPage(e, q.text(14));
            entries_.push_back(std::move(e));
        }
    }
    Stmt q(db_, "SELECT from_id, to_id, label FROM entry_links");
    if (!q) { error_ = sqlite3_errmsg(db_); return false; }
    while (q.row()) links_.push_back({q.text(0), q.text(1), q.text(2)});
    linksChanged();
    return true;
}

bool Library::saveEntry(const Entry& e) {
    Stmt s(db_, "INSERT INTO entries(id, world_id, book_id, kind, name, aliases, summary, details, notes, image_blob, sort_order, created, "
                "modified, deleted, page) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15) ON CONFLICT(id) DO UPDATE SET "
                "kind=excluded.kind, name=excluded.name, aliases=excluded.aliases, summary=excluded.summary, details=excluded.details, "
                "notes=excluded.notes, image_blob=excluded.image_blob, sort_order=excluded.sort_order, modified=excluded.modified, "
                "deleted=excluded.deleted, page=excluded.page");
    s.bind(15, detail::entryPageText(e));
    s.bindIdOrNull(2, e.worldId).bindIdOrNull(3, e.bookId);
    s.bind(1, e.id).bind(4, e.kind).bind(5, e.name).bind(6, json(e.aliases).dump()).bind(7, e.summary).bind(8, e.details)
        .bind(9, e.notes).bind(10, e.imageBlob).bind(11, e.order).bind(12, (std::int64_t)e.created).bind(13, (std::int64_t)e.modified)
        .bind(14, (std::int64_t)e.deleted);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    return true;
}

void Library::indexEntries() const {
    if (entryAtSize_ == entries_.size()) return;
    entryAt_.clear();
    entryAt_.reserve(entries_.size() * 2);
    for (std::size_t i = 0; i < entries_.size(); ++i) entryAt_.emplace(entries_[i].id, i);
    entryAtSize_ = entries_.size();
}

Entry* Library::entryMut(const Id& id) {
    return const_cast<Entry*>(entry(id));
}
const Entry* Library::entry(const Id& id) const {
    if (id.empty()) return nullptr;
    indexEntries();
    auto it = entryAt_.find(id);
    if (it != entryAt_.end() && it->second < entries_.size() && entries_[it->second].id == id) return &entries_[it->second];
    // Not indexed (or the index is out of step): look the slow way, and index again next time.
    for (auto& e : entries_) if (e.id == id) { entryAtSize_ = (std::size_t)-1; return &e; }
    return nullptr;
}

std::vector<const Entry*> Library::entriesOf(const Id& worldId, const Id& bookId) const {
    // Sorted by name ignoring case; the lower-case names are made once rather than at every comparison.
    std::vector<std::pair<std::string, const Entry*>> keyed;
    for (auto& e : entries_)
        if (!e.deleted && (worldId.empty() ? e.worldId.empty() && e.bookId == bookId : e.worldId == worldId)) {
            std::string k = e.name;
            for (auto& c : k) c = (char)std::tolower((unsigned char)c);
            keyed.emplace_back(std::move(k), &e);
        }
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<const Entry*> out;
    out.reserve(keyed.size());
    for (auto& [k, e] : keyed) out.push_back(e);
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

bool Library::resetBible(const Id& worldId, const Id& bookId, bool everything, BibleReset& out) {
    out = {};
    const Id bookScope = worldId.empty() ? bookId : Id{};
    if (worldId.empty() ? !book(bookId) : !world(worldId)) { error_ = "No such world or book."; return false; }
    std::vector<std::string> keys;
    if (everything) {
        const std::string scope = worldId.empty() ? "book:" + bookId : "world:" + worldId;
        const std::string sid = worldId.empty() ? bookId : worldId;
        keys = {"conlangs:" + scope, "boards:" + scope, "bible-share:" + scope, "checks-ok:" + sid, "kind-ok:" + sid};
        for (const auto& k : entryKinds()) keys.push_back("entry-template:" + worldId + ":" + k);
    }
    if (!begin()) return false;
    for (const Entry* e : entriesOf(worldId, bookScope)) {
        Entry n = *e;
        n.deleted = now();
        if (!saveEntry(n)) { rollback(); return false; }
        out.trashed.push_back(n.id);
    }
    for (const auto& k : keys) {
        const std::string v = setting(k);
        if (v.empty()) continue;
        out.settings.push_back({k, v});
        if (!setSetting(k, std::string())) { rollback(); return false; }
    }
    if (!commit()) return false;
    const Time t = now();
    for (const Id& id : out.trashed) if (Entry* e = entryMut(id)) e->deleted = t;
    return true;
}

bool Library::undoBibleReset(const BibleReset& reset) {
    bool ok = true;
    for (const Id& id : reset.trashed) ok = restore(id) && ok;
    for (const auto& [k, v] : reset.settings) ok = setSetting(k, v) && ok;
    return ok;
}

// ----------------------------------------------------------------- the world's own relationships

namespace {
std::string relKey(const Id& worldId, const Id& bookId) { return worldId.empty() ? "relations:book:" + bookId : "relations:world:" + worldId; }
std::string lowerAscii(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string capitalised(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == ':')) s.pop_back();
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}
// Irregular verbs: third person -> past participle.
const std::vector<std::pair<const char*, const char*>>& irregular() {
    static const std::vector<std::pair<const char*, const char*>> v = {
        {"teaches", "taught"}, {"leads", "led"}, {"holds", "held"}, {"keeps", "kept"}, {"fights", "fought"}, {"seeks", "sought"},
        {"sees", "seen"}, {"knows", "known"}, {"hides", "hidden"}, {"bears", "borne"}, {"bites", "bitten"}, {"binds", "bound"},
        {"brings", "brought"}, {"builds", "built"}, {"buys", "bought"}, {"catches", "caught"}, {"chooses", "chosen"}, {"feeds", "fed"},
        {"finds", "found"}, {"forgives", "forgiven"}, {"forsakes", "forsaken"}, {"gives", "given"}, {"hears", "heard"}, {"loses", "lost"},
        {"makes", "made"}, {"meets", "met"}, {"pays", "paid"}, {"rides", "ridden"}, {"rules", "ruled"}, {"sells", "sold"}, {"sends", "sent"},
        {"shoots", "shot"}, {"slays", "slain"}, {"steals", "stolen"}, {"strikes", "struck"}, {"swears", "sworn"}, {"takes", "taken"},
        {"tells", "told"}, {"wins", "won"}, {"writes", "written"}, {"breaks", "broken"}, {"forgets", "forgotten"}, {"betrays", "betrayed"},
        {"has", "had"}, {"does", "done"}, {"bests", "bested"}, {"saves", "saved"}, {"slew", "slain"}, {"killed", "killed"}};
    return v;
}
std::string participle(const std::string& verb3) {   // "admires" -> "admired"
    const std::string v = lowerAscii(verb3);
    for (auto& [a, b] : irregular()) if (v == a) return b;
    std::string base = v;
    if (base.size() > 4 && base.compare(base.size() - 3, 3, "ies") == 0) return base.substr(0, base.size() - 3) + "ied";
    if (base.size() > 4 && (base.compare(base.size() - 4, 4, "ches") == 0 || base.compare(base.size() - 4, 4, "shes") == 0 ||
                            base.compare(base.size() - 3, 3, "xes") == 0 || base.compare(base.size() - 3, 3, "sses") == 0))
        base = base.substr(0, base.size() - 2);
    else if (!base.empty() && base.back() == 's') base.pop_back();
    if (!base.empty() && base.back() == 'e') return base + "d";
    return base + "ed";
}
std::string thirdPerson(const std::string& part) {   // "haunted" -> "haunts"
    const std::string p = lowerAscii(part);
    for (auto& [a, b] : irregular()) if (p == b) return a;
    if (p.size() > 4 && p.compare(p.size() - 3, 3, "ied") == 0) return p.substr(0, p.size() - 3) + "ies";
    if (p.size() > 3 && p.compare(p.size() - 2, 2, "ed") == 0) {
        std::string stem = p.substr(0, p.size() - 2);
        // "deceived", "guided", "raised", "loved", "hated": the stem wants its e back.
        static const char* withE[] = {"deceiv", "guid", "rais", "lov", "hat", "creat", "admir", "ador", "serv", "sav", "chas", "us",
                                      "bless", "curs", "despis", "nam", "tam", "receiv", "believ", "rescu", "captur", "forg"};
        for (const char* w : withE) if (stem == w) return stem + "es";
        const char last = stem.empty() ? 0 : stem.back();
        if (last == 'v' || last == 'z' || (last == 'c' && stem.size() > 2)) return stem + "es";
        if (stem.size() > 2 && stem[stem.size() - 1] == stem[stem.size() - 2] && std::string("bdgmnprt").find(last) != std::string::npos)
            stem.pop_back();   // "stabbed" -> "stabs"
        if (last == 's' || last == 'x' || (stem.size() > 1 && (stem.compare(stem.size() - 2, 2, "ch") == 0 || stem.compare(stem.size() - 2, 2, "sh") == 0)))
            return stem + "es";
        return stem + "s";
    }
    return p;
}
}  // namespace

std::string guessReverseRelation(const std::string& labelIn) {
    const std::string label = capitalised(labelIn);
    if (label.empty()) return label;
    std::vector<std::string> words;
    {
        std::istringstream in(label);
        for (std::string w; in >> w;) words.push_back(w);
    }
    const std::string first = lowerAscii(words[0]), last = lowerAscii(words.back());
    // "Haunted by" -> "Haunts"; "Guided by" -> "Guides".
    if (words.size() >= 2 && last == "by") {
        std::string out = capitalised(thirdPerson(first));
        for (size_t i = 1; i + 1 < words.size(); ++i) out += " " + words[i];
        return out;
    }
    // "Guardian of" -> "Has guardian"; "Heir of" -> "Has heir".
    if (words.size() >= 2 && last == "of") {
        std::string noun;
        for (size_t i = 0; i + 1 < words.size(); ++i) noun += (noun.empty() ? "" : " ") + lowerAscii(words[i]);
        return "Has " + noun;
    }
    if (first == "has" && words.size() >= 2) {
        std::string noun;
        for (size_t i = 1; i < words.size(); ++i) noun += (noun.empty() ? "" : " ") + lowerAscii(words[i]);
        return capitalised(noun) + " of";
    }
    // "Admires" -> "Admired by"; "Watches over" -> "Watched over by"; "Is loyal to" stays as it is.
    if (first.size() > 2 && first.back() == 's' && first != "is" && first != "was") {
        std::string out = capitalised(participle(first));
        for (size_t i = 1; i < words.size(); ++i) out += " " + words[i];
        return out + " by";
    }
    return label;   // the same both ways ("Rivals", "Twin")
}

std::vector<CustomRelation> Library::customRelations(const Id& worldId, const Id& bookId) const {
    std::vector<CustomRelation> out;
    const json j = json::parse(setting(relKey(worldId, bookId)), nullptr, false);
    if (!j.is_array()) return out;
    for (auto& r : j)
        if (r.is_object() && r.contains("label") && r["label"].is_string()) {
            CustomRelation c{capitalised(r["label"].get<std::string>()), r.contains("reverse") && r["reverse"].is_string() ? capitalised(r["reverse"].get<std::string>()) : std::string()};
            if (c.label.empty() || c.label.size() > 60) continue;
            // One the bible has come to know itself (0.57: "Used by") is no longer the world's own.
            if (std::any_of(relations().begin(), relations().end(), [&](const Relation& b) { return lowerAscii(b.label) == lowerAscii(c.label); })) continue;
            if (c.reverse.empty()) c.reverse = guessReverseRelation(c.label);
            if (std::none_of(out.begin(), out.end(), [&](const CustomRelation& x) { return lowerAscii(x.label) == lowerAscii(c.label); })) out.push_back(c);
        }
    return out;
}

bool Library::setCustomRelations(const Id& worldId, const Id& bookId, const std::vector<CustomRelation>& list) {
    json j = json::array();
    for (auto& c : list) if (!capitalised(c.label).empty()) j.push_back({{"label", capitalised(c.label)}, {"reverse", capitalised(c.reverse)}});
    return setSetting(relKey(worldId, bookId), j.empty() ? std::string() : j.dump());
}

std::string Library::ensureRelation(const Id& worldId, const Id& bookId, const std::string& labelIn) {
    const std::string label = capitalised(labelIn);
    if (label.empty()) return label;
    for (auto& r : relations()) {
        if (lowerAscii(r.label) == lowerAscii(label)) return r.label;
    }
    auto list = customRelations(worldId, bookId);
    for (auto& c : list) {
        if (lowerAscii(c.label) == lowerAscii(label)) return c.label;
        if (lowerAscii(c.reverse) == lowerAscii(label)) return c.reverse;   // its other side, already known
    }
    list.push_back({label, guessReverseRelation(label)});
    setCustomRelations(worldId, bookId, list);
    return label;
}

std::string Library::relationReverse(const Id& worldId, const Id& bookId, const std::string& label) const {
    for (auto& c : customRelations(worldId, bookId)) {
        if (c.label == label) return c.reverse;
        if (c.reverse == label) return c.label;
    }
    return reverseRelation(label);
}

int Library::relationUses(const Id& worldId, const Id& bookId, const std::string& label) const {
    std::set<Id> mine;
    for (const Entry* e : entriesOf(worldId, worldId.empty() ? bookId : Id{})) mine.insert(e->id);
    int n = 0;
    for (auto& l : links_) if (l.label == label && mine.count(l.from)) ++n;
    return n;
}

bool Library::renameRelation(const Id& worldId, const Id& bookId, const std::string& from, const CustomRelation& to) {
    const std::string label = capitalised(to.label);
    if (label.empty()) { error_ = "A relationship needs a name."; return false; }
    std::set<Id> mine;
    for (const Entry* e : entriesOf(worldId, worldId.empty() ? bookId : Id{})) mine.insert(e->id);
    if (!begin()) return false;
    auto list = customRelations(worldId, bookId);
    bool found = false;
    for (auto& c : list)
        if (c.label == from) { c = {label, capitalised(to.reverse).empty() ? guessReverseRelation(label) : capitalised(to.reverse)}; found = true; }
    if (!found) list.push_back({label, capitalised(to.reverse).empty() ? guessReverseRelation(label) : capitalised(to.reverse)});
    if (!setCustomRelations(worldId, bookId, list)) { rollback(); return false; }
    if (label != from) {
        for (auto& l : links_) {
            if (l.label != from || !mine.count(l.from)) continue;
            Stmt s(db_, "UPDATE OR IGNORE entry_links SET label = ?4 WHERE from_id = ?1 AND to_id = ?2 AND label = ?3");
            s.bind(1, l.from).bind(2, l.to).bind(3, from).bind(4, label);
            if (!s.run()) { error_ = sqlite3_errmsg(db_); rollback(); return false; }
        }
    }
    if (!commit()) return false;
    if (label != from) for (auto& l : links_) if (l.label == from && mine.count(l.from)) l.label = label;
    return true;
}

std::vector<EntryLink> Library::linksOf(const Id& id) const {
    if (!linksAtValid_) {
        linksAt_.clear();
        for (std::size_t i = 0; i < links_.size(); ++i) {
            linksAt_[links_[i].from].push_back(i);
            if (links_[i].to != links_[i].from) linksAt_[links_[i].to].push_back(i);
        }
        linksAtValid_ = true;
    }
    std::vector<EntryLink> out;
    auto it = linksAt_.find(id);
    if (it == linksAt_.end()) return out;
    for (std::size_t i : it->second) {
        if (i >= links_.size()) continue;
        const EntryLink& l = links_[i];
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
    linksChanged();
    return true;
}

bool Library::removeLink(const Id& from, const Id& to, const std::string& label) {
    Stmt s(db_, "DELETE FROM entry_links WHERE from_id = ?1 AND to_id = ?2 AND label = ?3");
    s.bind(1, from).bind(2, to).bind(3, label);
    if (!s.run()) { error_ = sqlite3_errmsg(db_); return false; }
    links_.erase(std::remove_if(links_.begin(), links_.end(), [&](const EntryLink& l) { return l.from == from && l.to == to && l.label == label; }),
                 links_.end());
    linksChanged();
    return true;
}

// ---------------------------------------------------------------- tidying

namespace {

// A name as compared for "the same entry": lower case, curly apostrophes straight, spaces squeezed.
std::string nameKey(const std::string& n) {
    std::string out;
    bool sp = false;
    for (size_t i = 0; i < n.size(); ++i) {
        if (n.compare(i, 3, "\xE2\x80\x99") == 0 || n.compare(i, 3, "\xE2\x80\x98") == 0) { out += '\''; i += 2; sp = false; continue; }
        const unsigned char c = (unsigned char)n[i];
        if (c == ' ' || c == '\t') { if (!sp && !out.empty()) out += ' '; sp = true; continue; }
        out += (char)std::tolower(c);
        sp = false;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string facetLabel(const std::string& line) {
    const size_t c = line.find(':');
    if (c == std::string::npos || c == 0 || c > 24) return {};
    std::string l = line.substr(0, c);
    for (auto& ch : l) ch = (char)std::tolower((unsigned char)ch);
    return l;
}

// Two blocks of text together: the second's lines that the first doesn't have, after it. Facet lines
// ("Gender: Female") only when the first has no line with that label.
std::string joinText(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty() || a == b) return a;
    std::vector<std::string> la;
    std::istringstream ia(a);
    for (std::string l; std::getline(ia, l);) la.push_back(l);
    std::string extra;
    std::istringstream ib(b);
    for (std::string l; std::getline(ib, l);) {
        if (l.empty() || std::find(la.begin(), la.end(), l) != la.end()) continue;
        const std::string f = facetLabel(l);
        if (!f.empty() && std::any_of(la.begin(), la.end(), [&](const std::string& x) { return facetLabel(x) == f; })) continue;
        extra += (extra.empty() ? "" : "\n") + l;
    }
    if (extra.empty()) return a;
    return a + "\n" + extra;
}

int richness(const Entry& e, size_t links) {
    return (int)(e.summary.size() + e.details.size() + e.notes.size()) + (e.imageBlob.empty() ? 0 : 200) + (int)e.aliases.size() * 20 + (int)links * 40
         + [&] { int n = 0; for (auto& x : e.sections) n += (int)x.second.size(); for (auto& x : e.bookNotes) n += (int)x.second.size(); return n; }()
         + (int)e.gallery.size() * 100;
}

}  // namespace

const Entry* Library::findEntryNamed(const Id& worldId, const Id& bookId, const std::string& name, const std::string& kind) const {
    const std::string k = nameKey(name);
    for (const Entry* e : entriesOf(worldId, bookId))
        if ((kind.empty() || e->kind == kind) && nameKey(e->name) == k) return e;
    return nullptr;
}

bool Library::mergeEntries(const Id& keepId, const Id& otherId) {
    Entry* keep = entryMut(keepId);
    const Entry* other = entry(otherId);
    if (!keep || !other || keepId == otherId) { error_ = "Those entries can't be merged."; return false; }
    if (!begin()) return false;
    Entry k = *keep;
    if (nameKey(other->name) != nameKey(k.name)) k.aliases.push_back(other->name);
    for (const auto& a : other->aliases) k.aliases.push_back(a);
    std::vector<std::string> al;
    for (auto& a : k.aliases)
        if (nameKey(a) != nameKey(k.name) && std::none_of(al.begin(), al.end(), [&](const std::string& x) { return nameKey(x) == nameKey(a); })) al.push_back(a);
    k.aliases = al;
    k.summary = joinText(k.summary, other->summary);
    k.details = joinText(k.details, other->details);
    k.notes = joinText(k.notes, other->notes);
    if (k.imageBlob.empty()) k.imageBlob = other->imageBlob;
    for (auto& sct : other->sections) k.setSection(sct.first, joinText(k.section(sct.first), sct.second));
    for (auto& p : other->gallery)
        if (std::none_of(k.gallery.begin(), k.gallery.end(), [&](const Entry::Picture& q) { return q.blob == p.blob; })) k.gallery.push_back(p);
    for (auto& n : other->bookNotes) k.setBookNote(n.first, joinText(k.bookNote(n.first), n.second));
    if (k.mapBlob.empty()) { k.mapBlob = other->mapBlob; k.pins = other->pins; }
    if (k.pronunciation.empty()) k.pronunciation = other->pronunciation;
    for (auto& r : other->revealed) if (k.revealedIn(r.first).empty()) k.setRevealedIn(r.first, r.second);
    k.modified = now();
    if (!saveEntry(k)) { rollback(); return false; }
    // Its links, now the kept entry's (none to itself, none twice).
    std::vector<EntryLink> moved;
    for (const EntryLink& l : links_) {
        if (l.from != otherId && l.to != otherId) continue;
        EntryLink n{l.from == otherId ? keepId : l.from, l.to == otherId ? keepId : l.to, l.label};
        if (n.from != n.to) moved.push_back(n);
    }
    {
        Stmt d(db_, "DELETE FROM entry_links WHERE from_id = ?1 OR to_id = ?1");
        d.bind(1, otherId);
        if (!d.run()) { error_ = sqlite3_errmsg(db_); rollback(); return false; }
    }
    for (const EntryLink& n : moved) {
        Stmt s(db_, "INSERT OR IGNORE INTO entry_links(from_id, to_id, label) VALUES (?1, ?2, ?3)");
        s.bind(1, n.from).bind(2, n.to).bind(3, n.label);
        if (!s.run()) { error_ = sqlite3_errmsg(db_); rollback(); return false; }
    }
    {
        Stmt p(db_, "UPDATE nodes SET pov_id = ?1 WHERE pov_id = ?2");
        p.bind(1, keepId).bind(2, otherId);
        if (!p.run()) { error_ = sqlite3_errmsg(db_); rollback(); return false; }
    }
    Entry o = *other;
    o.deleted = now();
    if (!saveEntry(o)) { rollback(); return false; }
    if (!commit()) return false;
    // In memory too.
    *entryMut(keepId) = k;
    *entryMut(otherId) = o;
    links_.erase(std::remove_if(links_.begin(), links_.end(), [&](const EntryLink& l) { return l.from == otherId || l.to == otherId; }), links_.end());
    linksChanged();
    for (const EntryLink& n : moved)
        if (std::none_of(links_.begin(), links_.end(), [&](const EntryLink& l) { return l.from == n.from && l.to == n.to && l.label == n.label; }))
            { links_.push_back(n); linksChanged(); }
    for (auto& n : nodes_) if (n.povId == otherId) n.povId = keepId;
    return true;
}

Library::TidyReport Library::tidyBible(const Id& worldId) {
    TidyReport r;
    if (!world(worldId)) return r;
    // Entries kept with a book of this world (from before it joined): into the world's bible.
    for (auto& e : entries_) {
        if (e.deleted || !e.worldId.empty() || e.bookId.empty()) continue;
        const Book* b = book(e.bookId);
        if (!b || b->worldId != worldId) continue;
        Stmt s(db_, "UPDATE entries SET world_id = ?1, book_id = NULL WHERE id = ?2");
        s.bind(1, worldId).bind(2, e.id);
        if (!s.run()) { error_ = sqlite3_errmsg(db_); continue; }
        e.worldId = worldId;
        e.bookId.clear();
        ++r.moved;
    }
    // Copies: the same name and kind, or a character and a creature of one name (one entry that's both:
    // the character keeps the creature's "Kind of", so it's listed under its breed).
    for (bool again = true; again;) {
        again = false;
        // Only entries whose names match can be copies: grouped by name first (0.72), so a big bible isn't compared
        // pair by pair (600 entries took 1.4 s every time a library opened).
        std::map<std::string, std::vector<const Entry*>> byName;
        for (const Entry* e : entriesOf(worldId))
            if (std::string k = nameKey(e->name); !k.empty()) byName[k].push_back(e);
        for (auto it = byName.begin(); it != byName.end() && !again; ++it) {
          const auto& all = it->second;
          if (all.size() < 2) continue;
          for (size_t i = 0; i < all.size() && !again; ++i)
            for (size_t j = i + 1; j < all.size() && !again; ++j) {
                const Entry* a = all[i];
                const Entry* b = all[j];
                const bool sameKind = a->kind == b->kind;
                const bool charCreature = (a->kind == "character" && b->kind == "creature") || (a->kind == "creature" && b->kind == "character");
                if (!sameKind && !charCreature) continue;
                const Entry* keep = a;
                const Entry* drop = b;
                if (charCreature) {
                    if (a->kind != "character") std::swap(keep, drop);
                } else if (richness(*b, linksOf(b->id).size()) > richness(*a, linksOf(a->id).size()) ||
                           (richness(*b, linksOf(b->id).size()) == richness(*a, linksOf(a->id).size()) && b->created < a->created)) {
                    std::swap(keep, drop);
                }
                const std::string name = keep->name;
                if (mergeEntries(keep->id, drop->id)) {
                    ++r.merged;
                    if (std::find(r.mergedNames.begin(), r.mergedNames.end(), name) == r.mergedNames.end()) r.mergedNames.push_back(name);
                    again = true;   // the list changed
                }
            }
        }
    }
    return r;
}

std::vector<Library::KindSuggestion> Library::kindSuggestions(const Id& worldId, const Id& bookId) const {
    std::vector<KindSuggestion> out;
    const auto all = entriesOf(worldId, bookId);
    auto suggest = [&](const Entry* e, const std::string& kind, const std::string& why) {
        if (e->kind == kind) return;
        for (auto& s : out) if (s.entry == e->id) return;
        out.push_back({e->id, kind, why});
    };
    // Every "Race:" value in use, by its name key, with the entries giving it in the bible's order (read once, 0.72:
    // comparing every entry with every other took a fifth of a second on each change in a big bible).
    std::map<std::string, std::vector<const Entry*>> raceOf;
    for (const Entry* o : all) {
        std::istringstream in(o->details);
        for (std::string l; std::getline(in, l);)
            if (facetLabel(l) == "race") {
                std::string v = l.substr(l.find(':') + 1);
                while (!v.empty() && v.front() == ' ') v.erase(0, 1);
                raceOf[nameKey(v)].push_back(o);
            }
    }
    for (const Entry* e : all) {
        // Used as another entry's race ("Race: Demon").
        if (e->kind != "race")
            if (auto it = raceOf.find(nameKey(e->name)); it != raceOf.end())
                for (const Entry* o : it->second)
                    if (o != e) { suggest(e, "race", o->name + "'s race is " + e->name + ", so it reads like a race (a people)."); break; }
        // Something is "a kind of" it: a species or breed.
        if (e->kind == "character")
            for (const auto& l : links_)
                if (l.to == e->id && l.label == "Kind of")
                    if (const Entry* o = entry(l.from); o && !o->deleted && o->kind == "creature")
                        suggest(e, "creature", o->name + " is a kind of " + e->name + ", so it reads like a creature (a species or breed).");
        // A people's name: "Fluxians", "Ironfolk".
        if (e->kind == "character" && e->name.find(' ') == std::string::npos) {
            const std::string k = nameKey(e->name);
            auto ends = [&](const char* s) { const size_t n = std::strlen(s); return k.size() > n + 2 && k.compare(k.size() - n, n, s) == 0; };
            if (ends("ians") || ends("ites") || ends("folk") || ends("kin") || ends("ese") || ends("ans"))
                suggest(e, "race", e->name + " sounds like the name of a people.");
        }
    }
    return out;
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
            const size_t end = at + name.size();
            const bool startOk = words::wholeWord(text, at, name.size()), endOk = true;
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
                                                 "master", "mistress", "saint", "st", "professor", "prof",
                                                 // forms of address: "Her Highness" never stands for someone as "Her" (0.38.2)
                                                 "her", "his", "your", "my", "their", "our", "highness", "majesty", "lordship", "ladyship",
                                                 "grace", "excellency", "honour", "honor", "eminence", "holiness"};
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

// The runs of ASCII letters and digits in a text, in lower case: a quick test of which names could be in it. Anything
// else (spaces, punctuation, any byte of a non-ASCII character) separates them, the same way for names and text, so a
// name found by countNames always has its first such run among them.
static std::unordered_set<std::string> asciiWords(const std::string& text) {
    std::unordered_set<std::string> out;
    std::string cur;
    for (unsigned char ch : text) {
        if (ch < 0x80 && std::isalnum(ch)) cur += (char)std::tolower(ch);
        else if (!cur.empty()) { out.insert(cur); cur.clear(); }
    }
    if (!cur.empty()) out.insert(cur);
    return out;
}

// Could one of these names be in a text with these words? Only "no" for certain: every ASCII run of a name that's
// there is among the text's; a name with none is always checked.
static bool mayAppear(const std::vector<std::string>& names, const std::unordered_set<std::string>& present) {
    for (const std::string& nm : names) {
        bool all = true;
        std::string cur;
        for (size_t i = 0; i <= nm.size() && all; ++i) {
            const unsigned char ch = i < nm.size() ? (unsigned char)nm[i] : ' ';
            if (ch < 0x80 && std::isalnum(ch)) cur += (char)std::tolower(ch);
            else if (!cur.empty()) { all = present.count(cur) > 0; cur.clear(); }
        }
        if (all) return true;
    }
    return false;
}

std::map<Id, std::vector<Id>> Library::appearancesIn(const Id& bookId) const {
    std::map<Id, std::vector<Id>> out;
    const Book* b = book(bookId);
    if (!b) return out;
    const auto entries = b->worldId.empty() ? entriesOf({}, bookId) : entriesOf(b->worldId);
    std::vector<std::vector<std::string>> names;
    for (const Entry* e : entries) names.push_back(searchNames(*e));
    // The bible's names, as one number: a change to any of them means every chapter is looked at again.
    std::uint64_t sig = 1469598103934665603ull;
    auto mix = [&](const std::string& s) { for (unsigned char c : s) sig = (sig ^ c) * 1099511628211ull; sig = (sig ^ 0xff) * 1099511628211ull; };
    for (size_t i = 0; i < entries.size(); ++i) { mix(entries[i]->id); for (const auto& nm : names[i]) mix(nm); }
    // What was found in earlier sessions (0.72), kept in the library's cache folder (not the database, so it's never
    // synced or backed up): a long book's Planning and the bible open without reading every chapter again.
    const std::filesystem::path cacheFile = u8path(folder_) / "cache" / ("appearances-" + bookId + ".json");
    if (appearanceLoaded_.insert(bookId).second) {
        std::ifstream in(cacheFile, std::ios::binary);
        const json j = in ? json::parse(in, nullptr, false) : json();
        if (j.is_object() && j.value("names", "") == std::to_string(sig) && j.contains("nodes") && j["nodes"].is_object())
            for (auto& [nid, v] : j["nodes"].items()) {
                if (!v.is_array() || v.size() != 4 || appearanceCache_.count(nid)) continue;
                AppearanceCache c;
                c.length = v[0].get<std::int64_t>();
                c.words = v[1].get<std::int64_t>();
                c.modified = v[2].get<Time>();
                c.names = sig;
                for (auto& f : v[3]) if (f.is_array() && f.size() == 2) c.found.push_back({f[0].get<int>(), f[1].get<std::string>()});
                appearanceCache_[nid] = std::move(c);
            }
    }
    bool changed = false;
    for (const ManuscriptNode* n : manuscriptOf(bookId)) {
        std::int64_t length = 0, words = 0;
        Time modified = 0;
        {
            Stmt q(db_, "SELECT length(content), word_count, modified FROM nodes WHERE id = ?1");
            q.bind(1, n->id);
            if (q && q.row()) { length = q.i64(0); words = q.i64(1); modified = q.i64(2); }
        }
        AppearanceCache& c = appearanceCache_[n->id];
        if (c.length != length || c.words != words || c.modified != modified || c.names != sig) {
            const std::string text = doc::plainText(nodeContent(n->id));
            // The text's words once (0.72), so only entries one of whose names starts with a word that's there are
            // counted: a chapter isn't read again for each of hundreds of entries.
            const std::unordered_set<std::string> present = asciiWords(text);
            std::vector<std::pair<int, Id>> found;
            for (size_t i = 0; i < entries.size(); ++i)
                if (!names[i].empty() && mayAppear(names[i], present))
                    if (const int k = countNames(text, names[i]); k > 0) found.push_back({k, entries[i]->id});
            std::stable_sort(found.begin(), found.end(), [](auto& x, auto& y) { return x.first > y.first; });
            c = {length, words, modified, sig, std::move(found)};
            changed = true;
        }
        for (auto& f : c.found) out[n->id].push_back(f.second);
    }
    if (changed) {   // kept for next time; a write that fails only costs the next session a re-read
        json j{{"names", std::to_string(sig)}, {"nodes", json::object()}};
        for (const ManuscriptNode* n : manuscriptOf(bookId))
            if (auto it = appearanceCache_.find(n->id); it != appearanceCache_.end()) {
                json f = json::array();
                for (auto& [k, id] : it->second.found) f.push_back({k, id});
                j["nodes"][n->id] = {it->second.length, it->second.words, it->second.modified, f};
            }
        std::error_code ec;
        std::filesystem::create_directories(cacheFile.parent_path(), ec);
        std::filesystem::path tmp = cacheFile;
        tmp += ".tmp";
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            if (o) o << j.dump();
        }
        std::filesystem::rename(tmp, cacheFile, ec);
        if (ec) std::filesystem::remove(tmp, ec);
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

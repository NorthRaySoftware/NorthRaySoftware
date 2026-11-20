// The Worldbuilding workspace: a world's (or an independent book's) bible of
// characters, places, groups, things, lore and events. The sidebar filters by
// kind; the middle shows the entries as cards, or one entry with its details,
// relationships and where it appears in the manuscripts; the right-hand panel
// edits it.
#include "studio_app.hpp"
#include "studio/family_tree.hpp"
#include "cjui/dialog.hpp"
#include "studio/bible_export.hpp"
#include "cjui/menu.hpp"

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace nrs {

using nlohmann::json;

using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }

static int popupAt(HWND owner, const std::vector<std::wstring>& items, int checked = -1) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i)
        AppendMenuW(m, MF_STRING | ((int)i == checked ? MF_CHECKED : 0), 1 + (UINT)i, items[i].c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = (int)cjui::trackMenu(m, pt, owner);
    DestroyMenu(m);
    return cmd - 1;
}

static std::wstring upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

// `text` cut to at most `lines` lines at `width` (at a word, with an ellipsis), measured the way the screen will draw
// it. Kept per text and width, since cards ask every frame.
static std::wstring linesOf(cjui::Gfx& g, const std::wstring& text, float width, Font f, int lines) {
    static std::map<std::wstring, std::wstring> cache;
    std::wstring key = std::to_wstring((int)width) + L"|" + std::to_wstring((int)f) + L"|" + std::to_wstring(lines) + L"|" + text;
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    if (cache.size() > 4000) cache.clear();
    const float one = g.measureHeight(L"Ag", 10000, f);
    const float limit = one * lines + one * 0.5f;
    std::wstring flat = text;
    for (auto& c : flat) if (c == L'\n' || c == L'\r' || c == L'\t') c = L' ';
    std::wstring out = flat;
    if (g.measureHeight(flat, width, f) > limit) {
        std::vector<size_t> cuts;   // word ends
        for (size_t i = 1; i < flat.size(); ++i) if (flat[i] == L' ' && flat[i - 1] != L' ') cuts.push_back(i);
        size_t lo = 0, hi = cuts.size();   // the most words that fit with "…"
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (g.measureHeight(flat.substr(0, cuts[mid - 1]) + L"\u2026", width, f) <= limit) lo = mid; else hi = mid - 1;
        }
        out = lo == 0 ? std::wstring() : flat.substr(0, cuts[lo - 1]);
        while (!out.empty() && (out.back() == L',' || out.back() == L';' || out.back() == L':' || out.back() == L' ')) out.pop_back();
        out += L"\u2026";
    }
    cache[key] = out;
    return out;
}

static std::wstring initials(const std::string& name) {
    std::wstring out;
    bool start = true;
    for (wchar_t c : w(name)) {
        if (iswspace(c)) { start = true; continue; }
        if (start && iswalnum(c) && out.size() < 2) out += (wchar_t)towupper(c);
        start = false;
    }
    return out.empty() ? L"?" : out;
}

// ------------------------------------------------------------- gender and place types
// Kept as a line of the entry's details ("Gender: Female", "Type: City"), so what writers have
// already written counts, and it's still plain text they can edit.

// Each kind's colour (the editor's links and name cards use the same ones).
static cjui::Color kindColor(const std::string& kind, bool dark) {
    struct K { const char* k; const char* light; const char* dark; };
    static const K ks[] = {{"character", "#3E7BD6", "#7AA9F0"}, {"creature", "#C26A2E", "#E69A5F"}, {"place", "#2E9A6C", "#5BC99A"}, {"race", "#6B8E23", "#A5C76A"}, {"realm", "#5B5FC7", "#9A9DF0"},
                           {"group", "#7D5BD0", "#AE93F2"},     {"thing", "#B08A1E", "#E0BE55"},    {"lore", "#1E97A6", "#55C6D4"},
                           {"magic", "#B3439E", "#E37FD0"},     {"event", "#C9476A", "#F07C98"}};
    for (auto& x : ks) if (kind == x.k) return cjui::hexColor(dark ? x.dark : x.light);
    return cjui::hexColor(dark ? "#7AA9F0" : "#3E7BD6");
}

struct Facet {
    std::string key;                    // "gender"
    std::wstring title;                 // "Gender or sex"
    std::string written;                // the label a new line gets: "Gender"
    std::vector<std::string> labels;    // labels it's read from (lower case)
    std::vector<std::wstring> options;  // offered in the chooser
    bool openList;                      // options the world has written come first (races, realms)
};
static const std::vector<Facet>& allFacets() {
    static const std::vector<Facet> f = {
        {"gender", L"Gender or sex", "Gender", {"gender", "sex", "gender/sex", "gender or sex"},
         {L"Female", L"Male", L"Non-binary", L"Genderfluid", L"Agender", L"Intersex", L"Not applicable", L"Unknown", L"Other"}, false},
        // Races: only the author's own (the bible's Races, and races written on characters), never a built-in list.
        {"race", L"Race", "Race", {"race", "species", "people", "kin"}, {}, true},
        // Realms and types of place: likewise only what the author has made or written (0.38).
        {"realm", L"Realm or plane", "Realm", {"realm", "plane", "world of origin", "realm/plane"}, {}, true},
        {"type", L"Type of place", "Type", {"type", "kind of place", "place type"}, {}, true},
        {"ctype", L"Type of creature", "Type", {"type", "kind of creature", "creature type"},
         {L"Beast", L"Monster", L"Dragon", L"Undead", L"Spirit", L"Animal", L"Bird", L"Sea creature", L"Insect", L"Plant",
          L"Construct", L"Other"}, false},
    };
    return f;
}
static const Facet* findFacet(const std::string& key) {
    for (auto& f : allFacets()) if (f.key == key) return &f;
    return nullptr;
}
// The facets an entry of this kind has, in the order they're shown.
static std::vector<const Facet*> facetsFor(const std::string& kind) {
    std::vector<std::string> keys;
    if (kind == "character") keys = {"gender", "race", "realm"};
    else if (kind == "creature") keys = {"realm"};   // what kind of creature it is: "Kind of" another creature
    else if (kind == "place") keys = {"type", "realm"};
    std::vector<const Facet*> out;
    for (auto& k : keys) out.push_back(findFacet(k));
    return out;
}
static std::string trimmedCopy(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}
static bool isFacetLine(const std::string& line, const Facet& f, size_t& colon) {
    colon = line.find(':');
    if (colon == std::string::npos) return false;
    std::string label = trimmedCopy(line.substr(0, colon));
    for (auto& c : label) c = (char)std::tolower((unsigned char)c);
    return std::find(f.labels.begin(), f.labels.end(), label) != f.labels.end();
}
// The entry's value for a facet, as written ("" when not set).
static std::string facetOf(const Entry& e, const Facet& f) {
    size_t at = 0;
    while (at < e.details.size()) {
        size_t nl = e.details.find('\n', at);
        if (nl == std::string::npos) nl = e.details.size();
        const std::string line = e.details.substr(at, nl - at);
        size_t colon;
        if (isFacetLine(line, f, colon)) {
            std::string v = trimmedCopy(line.substr(colon + 1));
            if (!v.empty()) v[0] = (char)std::toupper((unsigned char)v[0]);
            return v;
        }
        at = nl + 1;
    }
    return {};
}
// Sets (or, with "", removes) the facet's line, keeping the others where they are.
static std::string withFacet(const Entry& e, const Facet& f, const std::string& value) {
    std::string out;
    bool done = false;
    size_t at = 0;
    while (at < e.details.size()) {
        size_t nl = e.details.find('\n', at);
        if (nl == std::string::npos) nl = e.details.size();
        const std::string line = e.details.substr(at, nl - at);
        size_t colon;
        if (isFacetLine(line, f, colon)) {
            if (!done && !value.empty()) out += trimmedCopy(line.substr(0, colon)) + ": " + value + "\n";
            done = true;
        } else {
            out += line + "\n";
        }
        at = nl + 1;
    }
    if (!done && !value.empty()) out = f.written + ": " + value + "\n" + out;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}
// The choices for a facet: for open lists, what this world already uses comes first.
static std::vector<std::wstring> facetChoices(const Facet& f, const std::vector<const Entry*>& scope, const std::string& kind) {
    std::vector<std::wstring> out;
    // Race and realm: the bible's own Races and Realms come first.
    if (f.key == "race" || f.key == "realm")
        for (const Entry* x : scope)
            if (x->kind == f.key && std::find(out.begin(), out.end(), widen(x->name)) == out.end()) out.push_back(widen(x->name));
    if (f.openList)
        for (const Entry* x : scope)
            if (x->kind == kind) {
                const std::wstring v = widen(facetOf(*x, f));
                if (!v.empty() && std::find(out.begin(), out.end(), v) == out.end() &&
                    std::find(f.options.begin(), f.options.end(), v) == f.options.end())
                    out.push_back(v);
            }
    std::sort(out.begin(), out.end());
    for (auto& o : f.options) if (std::find(out.begin(), out.end(), o) == out.end()) out.push_back(o);
    return out;
}
// wb_.facet is "key=value" ("key=-" for not set).
static bool facetMatches(const Entry& e, const std::string& filter) {
    if (filter.empty()) return true;
    const size_t eq = filter.find('=');
    const Facet* f = findFacet(filter.substr(0, eq));
    if (!f || eq == std::string::npos) return true;
    const std::string v = facetOf(e, *f), want = filter.substr(eq + 1);
    return want == "-" ? v.empty() : v == want;
}

// ------------------------------------------------------------- the bible by book

// The world's books in reading order: each series in turn (its reading order), then books in no series.
std::vector<Id> booksInReadingOrder(const Library& lib, const Id& world, const Id& book) {
    std::vector<Id> out;
    if (world.empty()) { if (!book.empty()) out.push_back(book); return out; }
    for (const Series* s : lib.seriesIn(world))
        for (const studio::Book* b : lib.booksInSeries(s->id))
            if (std::find(out.begin(), out.end(), b->id) == out.end()) out.push_back(b->id);
    for (const studio::Book* b : lib.standaloneBooksIn(world))
        if (std::find(out.begin(), out.end(), b->id) == out.end()) out.push_back(b->id);
    return out;
}

// Gender groups: a sign and a colour each (the cards keep the same layout).
static std::wstring genderSign(const std::wstring& g) {
    if (g == L"Female") return L"\u2640";
    if (g == L"Male") return L"\u2642";
    if (g == L"Non-binary" || g == L"Genderfluid" || g == L"Agender" || g == L"Intersex") return L"\u26A7";
    return L"";
}
// The sign drawn (not every font has the characters): a ring, with a cross below (female), an arrow up and right
// (male), or a line up with a cross bar and a stroke (⚧ for the others). `s` is the ring's diameter.
static void drawGenderSign(cjui::Gfx& gfx, float cx, float cy, float s, const std::wstring& g, cjui::Color c) {
    const float r = s / 2, t = std::max(1.4f, s / 9);
    if (g == L"Female") {
        cy -= s * 0.28f;
        gfx.circle(cx, cy, r, c, false, t);
        gfx.line(cx, cy + r, cx, cy + r + s * 0.6f, c, t);
        gfx.line(cx - s * 0.28f, cy + r + s * 0.32f, cx + s * 0.28f, cy + r + s * 0.32f, c, t);
    } else if (g == L"Male") {
        cx -= s * 0.18f; cy += s * 0.18f;
        gfx.circle(cx, cy, r, c, false, t);
        const float a = 0.7071f, sx = cx + r * a, sy = cy - r * a, ex = sx + s * 0.42f, ey = sy - s * 0.42f;
        gfx.line(sx, sy, ex, ey, c, t);
        gfx.line(ex - s * 0.34f, ey, ex, ey, c, t);
        gfx.line(ex, ey, ex, ey + s * 0.34f, c, t);
    } else if (!genderSign(g).empty()) {
        cy += s * 0.12f;
        gfx.circle(cx, cy, r, c, false, t);
        gfx.line(cx, cy - r, cx, cy - r - s * 0.6f, c, t);
        gfx.line(cx - s * 0.24f, cy - r - s * 0.3f, cx + s * 0.24f, cy - r - s * 0.3f, c, t);
    }
}
static cjui::Color genderColor(const std::wstring& g, const cjui::Palette& p) {
    const char* light = "#7A7F8C"; const char* dark = "#A6ABB8";
    if (g == L"Female") { light = "#B4507A"; dark = "#E68AB0"; }
    else if (g == L"Male") { light = "#3C72C4"; dark = "#7EA8EE"; }
    else if (g == L"Non-binary" || g == L"Genderfluid" || g == L"Agender") { light = "#8A6A1C"; dark = "#D9B95C"; }
    else if (g == L"Intersex") { light = "#6E4FB8"; dark = "#A992EC"; }
    return cjui::hexColor(p.dark ? dark : light);
}

// What an entry sits under in the bible: the place a place is in ("Located in" / "Part of" from it, or
// "Contains" / "Includes" to it), or the creature a creature is a kind of (Dragon > Glimmerwyrm > Bluebolt).
static Id parentPlace(const Library& lib, const Entry& e) {
    if (e.kind != "place" && e.kind != "creature") return {};
    const bool place = e.kind == "place";
    for (const EntryLink& l : lib.linksOf(e.id)) {
        const bool up = place ? (l.from == e.id && (l.label == "Located in" || l.label == "Part of")) ||
                                    (l.to == e.id && (l.label == "Contains" || l.label == "Includes"))
                              : (l.from == e.id && l.label == "Kind of") || (l.to == e.id && l.label == "Has kind");
        if (!up) continue;
        const Id other = l.from == e.id ? l.to : l.from;
        const Entry* o = lib.entry(other);
        if (o && !o->deleted && o->kind == e.kind) return other;
    }
    return {};
}
// The places (or realms) an entry sits in, outermost first: "Elandroth › Aquiera" for a city of Aquiera (0.48).
static std::vector<const Entry*> placePath(const Library& lib, const Entry& e) {
    std::vector<const Entry*> path;
    const Entry* x = &e;
    std::set<Id> seen{e.id};
    for (int guard = 0; guard < 12; ++guard) {
        const Entry* up = nullptr;
        for (const EntryLink& l : lib.linksOf(x->id)) {
            const bool isUp = (l.from == x->id && (l.label == "Located in" || l.label == "Part of" || (x->kind == "creature" && l.label == "Kind of"))) ||
                              (l.to == x->id && (l.label == "Contains" || l.label == "Includes" || (x->kind == "creature" && l.label == "Has kind")));
            if (!isUp) continue;
            const Entry* o = lib.entry(l.from == x->id ? l.to : l.from);
            if (o && !o->deleted && !seen.count(o->id) && (o->kind == "place" || o->kind == "realm" || (x->kind == "creature" && o->kind == "creature"))) { up = o; break; }
        }
        if (!up) break;
        seen.insert(up->id);
        path.insert(path.begin(), up);
        x = up;
    }
    return path;
}
// Whether `e` is `root` or sits somewhere under it.
static bool isUnder(const Library& lib, const Entry& e, const Id& root) {
    const Entry* x = &e;
    for (int guard = 0; x && guard < 16; ++guard) {
        if (x->id == root) return true;
        const Id p = parentPlace(lib, *x);
        x = p.empty() ? nullptr : lib.entry(p);
    }
    return false;
}

// ------------------------------------------------------------- opening

// The world (or independent book) the user is working in, from wherever they are.
bool StudioApp::worldbuildingScope(Id& worldId, Id& bookId) const {
    worldId.clear();
    bookId.clear();
    auto fromBook = [&](const Id& b) {
        const studio::Book* bk = lib_->book(b);
        if (!bk) return false;
        if (!bk->worldId.empty()) worldId = bk->worldId; else bookId = bk->id;
        return true;
    };
    if (ws_ == Workspace::Worldbuilding && (!wb_.world.empty() || !wb_.book.empty())) { worldId = wb_.world; bookId = wb_.book; return true; }
    if (ws_ == Workspace::Writing && fromBook(writingBook_)) return true;
    if (ws_ == Workspace::Typesetting && fromBook(ts_.book)) return true;
    if (ws_ == Workspace::Preview && fromBook(pv_.book)) return true;
    if (ws_ == Workspace::Publishing && fromBook(pb_.book)) return true;
    if (focus_.kind == Focus::World && lib_->world(focus_.id)) { worldId = focus_.id; return true; }
    if (focus_.kind == Focus::Series)
        if (const Series* s = lib_->seriesById(focus_.id); s && !s->worldId.empty()) { worldId = s->worldId; return true; }
    if (focus_.kind == Focus::Book && fromBook(focus_.id)) return true;
    if (view_ == View::World && lib_->world(viewId_)) { worldId = viewId_; return true; }
    if (view_ == View::Book && fromBook(viewId_)) return true;
    if (!wb_.world.empty() && lib_->world(wb_.world)) { worldId = wb_.world; return true; }
    if (!wb_.book.empty() && lib_->book(wb_.book)) { bookId = wb_.book; return true; }
    // Only one world: that one.
    Id only;
    int n = 0;
    for (auto& x : lib_->worlds()) if (!x.deleted) { only = x.id; ++n; }
    if (n == 1) { worldId = only; return true; }
    return false;
}

void StudioApp::openWorldbuilding(const Id& worldId, const Id& bookId) {
    if (!lib_) return;
    if (worldId.empty() ? !lib_->book(bookId) : !lib_->world(worldId)) return;
    commitFields();
    if (ws_ == Workspace::Writing && editor_.ready()) editor_.send(R"({"type":"flush"})");
    if (wb_.world != worldId || wb_.book != bookId) {
        wb_ = {};
        wb_.world = worldId;
        wb_.book = bookId;
    }
    ws_ = Workspace::Worldbuilding;
    if (const Entry* e = lib_->entry(wb_.entry); e && !e->deleted) openEntry(e->id);
    else {
        wb_.entry.clear();
        setFocus({});
    }
    invalidate();
}

void StudioApp::openEntry(const Id& id) {
    commitFields();
    wb_.board.clear();   // out of any image board
    wb_.item.clear();
    wb_.familyRoot.clear();
    wb_.pinning.clear();
    wb_.mapView.clear();
    wb_.checks = false;
    wb_.glossary = false;
    wb_.languages = false;
    wb_.share = false;
    wb_.relationsPage = false;
    wb_.fontsPage = false;
    if (wb_.entry != id) {
        wb_.panelScroll = 0;
        wb_.section.clear();
        wb_.editTemplate = false;
        wb_.picture = -1;
    }
    wb_.entry = id;
    wb_.scroll = 0;
    wb_.mentionsFor.clear();   // looked up again
    setFocus(id.empty() ? Focus{} : Focus{Focus::Entry, id, {}});
    invalidate();
}

void StudioApp::newEntry(const std::string& kindIn) {
    std::string kind = kindIn;
    if (kind.empty()) {
        std::vector<std::wstring> items;
        for (auto& k : entryKinds()) items.push_back(w(entryKindLabel(k)));
        const int i = popupAt(hwnd(), items);
        if (i < 0) return;
        kind = entryKinds()[i];
    }
    const Id id = lib_->createEntry(wb_.world, wb_.book, kind, std::string("New ") + [&] {
        std::string l = entryKindLabel(kind);
        for (auto& c : l) c = (char)std::tolower((unsigned char)c);
        return l;
    }());
    if (id.empty()) { flash(w("Couldn't create the entry: " + lib_->lastError()), 8); return; }
    openEntry(id);
    // Straight into naming it.
    loadFields();
    if (fields_[FName]) { SetFocus(fields_[FName]); SendMessageW(fields_[FName], EM_SETSEL, 0, -1); }
}

// "+ Add members…": tick characters (and creatures) in and out of a group or race; the list stays open
// for the next one until Esc or a click elsewhere.
void StudioApp::addMembers() {
    const Entry* g = lib_->entry(wb_.entry);
    if (!g) return;
    const Id gid = g->id;
    for (;;) {
        std::vector<const Entry*> people;
        for (const Entry* x : lib_->entriesOf(wb_.world, wb_.book))
            if (x->id != gid && (x->kind == "character" || x->kind == "creature")) people.push_back(x);
        if (people.empty()) { flash(L"Add some characters to the bible first.", 5); return; }
        std::stable_partition(people.begin(), people.end(), [](const Entry* x) { return x->kind == "character"; });
        auto memberLink = [&](const Entry* x, EntryLink* out) {
            for (const EntryLink& l : lib_->linksOf(gid))
                if ((l.to == gid && l.from == x->id && l.label == "Member of") || (l.from == gid && l.to == x->id && l.label == "Has member")) {
                    if (out) *out = l;
                    return true;
                }
            return false;
        };
        HMENU m = CreatePopupMenu();
        bool creatures = false;
        for (size_t i = 0; i < people.size(); ++i) {
            if (people[i]->kind == "creature" && !creatures && i > 0) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); creatures = true; }
            AppendMenuW(m, MF_STRING | (memberLink(people[i], nullptr) ? MF_CHECKED : 0), 1 + (UINT)i, w(people[i]->name).c_str());
        }
        POINT pt;
        GetCursorPos(&pt);
        const int i = (int)cjui::trackMenu(m, pt, hwnd()) - 1;
        DestroyMenu(m);
        if (i < 0) break;
        EntryLink l;
        if (memberLink(people[(size_t)i], &l)) lib_->removeLink(l.from, l.to, l.label);
        else lib_->addLink(people[(size_t)i]->id, gid, "Member of");
        invalidate();
        UpdateWindow(hwnd());
    }
}

void StudioApp::addRelationship() {
    const Entry* e = lib_->entry(wb_.entry);
    if (!e) return;
    std::vector<const Entry*> others;
    for (const Entry* x : lib_->entriesOf(wb_.world, wb_.book)) if (x->id != e->id) others.push_back(x);
    if (others.empty()) { flash(L"Add another entry first, then link the two.", 5); return; }
    // The relationships that suit this kind of entry, then the entries that suit that relationship
    // (the usual kinds first, the rest after a line).
    const auto& builtIn = relationsFor(e->kind);
    struct Choice { std::string label, targets; };
    std::vector<Choice> choices;
    for (auto& r : builtIn) choices.push_back({r.label, r.targets});
    const auto own = lib_->customRelations(wb_.world, wb_.world.empty() ? wb_.book : Id{});
    HMENU rm = CreatePopupMenu();
    for (size_t i = 0; i < choices.size(); ++i) AppendMenuW(rm, MF_STRING, 1 + (UINT)i, w(choices[i].label).c_str());
    if (!own.empty()) AppendMenuW(rm, MF_SEPARATOR, 0, nullptr);
    for (auto& c : own) {
        choices.push_back({c.label, ""});
        AppendMenuW(rm, MF_STRING, (UINT)choices.size(), w(c.label).c_str());
    }
    AppendMenuW(rm, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(rm, MF_STRING, 9000, L"A new kind of relationship\u2026");
    POINT rp;
    GetCursorPos(&rp);
    const int pick = (int)cjui::trackMenu(rm, rp, hwnd());
    DestroyMenu(rm);
    if (pick == 9000) { newRelation(); return; }
    const int li = pick - 1;
    if (li < 0 || li >= (int)choices.size()) return;
    const std::string targets = std::string(" ") + choices[li].targets + " ";
    auto fits = [&](const Entry* x) { return targets == "  " || targets.find(" " + x->kind + " ") != std::string::npos; };
    std::stable_partition(others.begin(), others.end(), fits);
    const size_t good = (size_t)std::count_if(others.begin(), others.end(), fits);
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < others.size(); ++i) {
        if (i == good && good > 0 && good < others.size()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 1 + (UINT)i, (w(others[i]->name) + L"   (" + w(entryKindLabel(others[i]->kind)) + L")").c_str());
    }
    POINT pt;
    GetCursorPos(&pt);
    const int oi = (int)cjui::trackMenu(m, pt, hwnd()) - 1;
    DestroyMenu(m);
    if (oi < 0) return;
    if (!lib_->addLink(e->id, others[oi]->id, choices[li].label)) flash(w("Couldn't link them: " + lib_->lastError()), 6);
    invalidate();
}

// ---------------------------------------------------------------- drawing

// Text whose words include the bible's names, drawn with those names as links (like a wiki):
// a click opens the entry, or offers a choice when several share the name. Wraps by words;
// a newline starts a new line. Returns the height used.
float StudioApp::drawLinkedText(const std::string& text, const Rect& r, Font f, Color c, const Id& self, const std::string& hitPrefix) {
    std::vector<const Entry*> others;
    for (const Entry* e : wb_.scopeEntries) if (e->id != self) others.push_back(e);
    const auto spans = findNames(text, others);
    struct Tok { std::wstring s; int span = -1; bool space = false, newline = false; };
    std::vector<Tok> toks;
    auto addPiece = [&](const std::string& piece, int span) {
        const std::wstring wpc = w(piece);
        std::wstring cur;
        auto push = [&](bool sp) { if (!cur.empty()) { toks.push_back({cur, span, sp, false}); cur.clear(); } };
        for (wchar_t ch : wpc) {
            if (ch == L'\r') continue;
            if (ch == L'\n') { push(false); toks.push_back({L"", -1, false, true}); continue; }
            const bool sp = ch == L' ' || ch == L'\t';
            if (!cur.empty() && (cur.back() == L' ') != sp) push(!sp);
            cur += sp ? L' ' : ch;
        }
        if (!cur.empty()) toks.push_back({cur, span, !cur.empty() && cur[0] == L' ', false});
    };
    size_t at = 0;
    for (size_t i = 0; i < spans.size(); ++i) {
        if (spans[i].start > at) addPiece(text.substr(at, spans[i].start - at), -1);
        addPiece(text.substr(spans[i].start, spans[i].end - spans[i].start), (int)i);
        at = spans[i].end;
    }
    if (at < text.size()) addPiece(text.substr(at), -1);
    const float lineH = gfx_.measureHeight(L"Ag", 1000, f);
    float x = r.x, y = r.y;
    for (size_t k = 0; k < toks.size(); ++k) {
        const Tok& t = toks[k];
        if (t.newline) { x = r.x; y += lineH; continue; }
        if (t.space && x == r.x) continue;   // no space at the start of a line
        const float tw = gfx_.measureWidth(t.s, f);
        if (!t.space && x > r.x && x + tw > r.right()) { x = r.x; y += lineH; }
        if (t.space) { x += tw; continue; }
        if (t.span < 0) {
            gfx_.text(t.s, {x, y, tw + 2, lineH}, f, c, {Align::Left, false});
        } else {
            const auto& ids = spans[(size_t)t.span].ids;
            const std::string id = hitPrefix + std::to_string(t.span) + ":" + std::to_string(k);
            addHit(id, {x, y, tw, lineH}, [this, ids] { openEntryChoice(ids); }, false);
            const float hv = hoverAmount(id);
            gfx_.text(t.s, {x, y, tw + 2, lineH}, f, pal_.ribbon.mix(pal_.ink, 0.25f * hv), {Align::Left, false});
            // A dotted underline (dashed when several entries share the name).
            const float uy = y + lineH - 2;
            const float dash = ids.size() > 1 ? 4.0f : 1.5f, gap = ids.size() > 1 ? 3.0f : 2.0f;
            for (float ux = x; ux < x + tw; ux += dash + gap)
                gfx_.line(ux, uy, std::min(x + tw, ux + dash), uy, pal_.ribbon.withAlpha(0.55f + 0.45f * hv), 1.0f);
        }
        x += tw;
    }
    return y + lineH - r.y;
}

void StudioApp::openEntryChoice(const std::vector<Id>& ids) {
    if (ids.empty()) return;
    if (ids.size() == 1) { openEntry(ids[0]); return; }
    std::vector<cjui::MenuItem> items;
    for (size_t i = 0; i < ids.size(); ++i)
        if (const Entry* e = lib_->entry(ids[i]))
            items.push_back(cjui::MenuItem::item(w(e->name) + L"  (" + w(entryKindLabel(e->kind)) + L")", (UINT)(i + 1)));
    POINT pt;
    GetCursorPos(&pt);
    const UINT c = cjui::showMenu(hwnd(), items, pt, pal_);
    if (c && c <= ids.size()) openEntry(ids[c - 1]);
}

void StudioApp::drawWorldSidebar(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), pal_.rule);
    gfx_.pushClip(r);
    setHitClip(r);
    const float x = r.x + 10, wd = r.w - 20, tx0 = x + 12, cw = wd - 24;
    float y = r.y + 14 - wb_.sideScroll;
    linkText("side:wb:library", tx0, y, L"‹ Library", [this] {
        ws_ = Workspace::Library;
        if (!wb_.world.empty()) goTo(View::World, wb_.world); else goTo(View::Book, wb_.book);
    });
    y += 30;
    gfx_.text(L"WORLDBUILDING", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 20;
    std::wstring name;
    if (const World* wo = lib_->world(wb_.world)) name = w(wo->name);
    else if (const studio::Book* b = lib_->book(wb_.book)) name = w(b->title);
    y += gfx_.text(name, {tx0, y, cw, 90}, Font::SerifTitle, pal_.ink) + 2;
    const auto all = lib_->entriesOf(wb_.world, wb_.book);
    gfx_.text(wb_.world.empty() ? L"An independent book" : L"A world", {tx0, y, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 30;
    button("side:wb:new", {tx0, y, cw, 32}, wb_.kind.empty() ? L"+ New entry" : L"+ New " + [&] {
        std::wstring l = w(entryKindLabel(wb_.kind));
        for (auto& c : l) c = (wchar_t)towlower(c);
        return l;
    }(), [this] { newEntry(wb_.kind); }, true);
    y += 48;
    // Kinds.
    auto row = [&](const std::string& kind, const std::wstring& label, int count) {
        const std::string id = "side:wb:kind:" + kind;
        const Rect rr{x, y, wd, 30};
        addHit(id, rr, [this, kind] {
            wb_.kind = kind;
            wb_.facet.clear();
            wb_.scroll = 0;
            openEntry({});
        });
        const bool sel = wb_.kind == kind && wb_.facet.empty() && wb_.entry.empty() && wb_.board.empty() && !wb_.checks && !wb_.glossary && !wb_.languages && !wb_.share && !wb_.relationsPage && !wb_.fontsPage && wb_.familyRoot.empty();
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        if (!kind.empty()) gfx_.circle(tx0 + 4, rr.y + rr.h / 2, 3.5f, kindColor(kind, pal_.dark), true);
        const float lx = kind.empty() ? tx0 : tx0 + 16;
        // Kinds with nothing in them yet are quieter (0.43).
        gfx_.text(label, {lx, rr.y, cw - 30 - (lx - tx0), rr.h}, sel ? Font::BodyStrong : Font::Body, count || sel ? pal_.ink : pal_.inkMuted,
                  {Align::Left, false, true, true});
        if (count) gfx_.text(std::to_wstring(count), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
        if (focused(id)) focusRing(rr);
        y += 32;
    };
    gfx_.text(L"ENTRIES", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 22;
    row("", L"Everything", (int)all.size());
    for (auto& k : entryKinds()) {
        int n = 0;
        for (auto* e : all) if (e->kind == k) ++n;
        row(k, w(entryKindPlural(k)), n);
        // Creatures: the top-level kinds (Dragon, ...), each with everything under it.
        if (wb_.kind == k && k == "creature" && n > 0) {
            std::vector<std::pair<const Entry*, int>> roots;
            for (auto* e : all)
                if (e->kind == "creature" && parentPlace(*lib_, *e).empty()) {
                    int under = 0;
                    for (auto* o : all) if (o->kind == "creature" && o != e && isUnder(*lib_, *o, e->id)) ++under;
                    if (under) roots.push_back({e, under + 1});
                }
            if (!roots.empty()) {
                gfx_.text(L"KINDS", {tx0 + 14, y + 4, cw - 14, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
                y += 22;
                for (auto& [e, cnt] : roots) {
                    const std::string filter = "under=" + e->id;
                    const std::string id = "side:wb:under:" + e->id;
                    const Rect rr{x + 14, y, wd - 14, 26};
                    addHit(id, rr, [this, filter] { wb_.kind = "creature"; wb_.facet = wb_.facet == filter ? std::string() : filter; wb_.scroll = 0; openEntry({}); });
                    const bool sel = wb_.facet == filter && wb_.entry.empty() && wb_.board.empty();
                    const float hv = hoverAmount(id);
                    if (sel) gfx_.fillRounded(rr, 3, pal_.surface);
                    else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
                    gfx_.text(w(e->name), {rr.x + 12, rr.y, rr.w - 50, rr.h}, sel ? Font::SmallStrong : Font::Small, sel ? pal_.ink : pal_.inkMuted,
                              {Align::Left, false, true, true});
                    gfx_.text(std::to_wstring(cnt), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
                    if (focused(id)) focusRing(rr);
                    y += 27;
                }
                y += 4;
            }
        }
        // The selected kind's entries by gender, race, realm or type: the values in use, with counts.
        if (wb_.kind == k && n > 0)
            for (const Facet* f : facetsFor(k)) {
                std::map<std::string, int> counts;
                int unset = 0;
                for (auto* e : all) if (e->kind == k) { const std::string v = facetOf(*e, *f); if (v.empty()) ++unset; else ++counts[v]; }
                if (counts.empty()) continue;   // nothing set yet: no list to show
                gfx_.text(upper(f->title), {tx0 + 14, y + 4, cw - 14, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
                y += 22;
                auto sub = [&](const std::string& value, const std::wstring& label, int count) {
                    const std::string filter = f->key + "=" + value;
                    const std::string id = "side:wb:facet:" + k + ":" + filter;
                    const Rect rr{x + 14, y, wd - 14, 26};
                    addHit(id, rr, [this, k, filter] { wb_.kind = k; wb_.facet = wb_.facet == filter ? std::string() : filter; wb_.scroll = 0; openEntry({}); });
                    const bool sel = wb_.facet == filter && wb_.entry.empty() && wb_.board.empty();
                    const float hv = hoverAmount(id);
                    if (sel) gfx_.fillRounded(rr, 3, pal_.surface);
                    else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
                    gfx_.text(label, {rr.x + 12, rr.y, rr.w - 50, rr.h}, sel ? Font::SmallStrong : Font::Small, sel ? pal_.ink : pal_.inkMuted,
                              {Align::Left, false, true, true});
                    gfx_.text(std::to_wstring(count), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
                    if (focused(id)) focusRing(rr);
                    y += 27;
                };
                for (auto& o : f->options) if (auto it = counts.find(narrow(o)); it != counts.end()) { sub(it->first, o, it->second); counts.erase(it); }
                for (auto& [v, cnt] : counts) sub(v, w(v), cnt);
                if (unset) sub("-", L"Not set", unset);
                y += 4;
            }
    }
    y += 18;
    gfx_.text(L"THE WORLD", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 22;
    // Glossary and checks.
    {
        const std::string id = "side:wb:glossary";
        const Rect rr{x, y, wd, 30};
        addHit(id, rr, [this] { openEntry({}); wb_.glossary = true; wb_.scroll = 0; invalidate(); });
        const bool sel = wb_.glossary;
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(L"Glossary", {tx0, rr.y, cw - 30, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
        if (focused(id)) focusRing(rr);
        y += 32;
    }
    {
        const std::string id = "side:wb:languages";
        const Rect rr{x, y, wd, 30};
        addHit(id, rr, [this] { openLanguages(); });
        const bool sel = wb_.languages;
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(L"Languages", {tx0, rr.y, cw - 30, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
        const std::size_t n = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{}).size();
        if (n) gfx_.text(std::to_wstring(n), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
        if (focused(id)) focusRing(rr);
        y += 32;
    }
    {
        const std::string id = "side:wb:relations";
        const Rect rr{x, y, wd, 30};
        addHit(id, rr, [this] { openRelations({}); });
        const bool sel = wb_.relationsPage;
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(L"Relationships", {tx0, rr.y, cw - 30, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
        const std::size_t n = lib_->customRelations(wb_.world, wb_.world.empty() ? wb_.book : Id{}).size();
        if (n) gfx_.text(std::to_wstring(n), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
        if (focused(id)) focusRing(rr);
        y += 32;
    }
    {
        const std::string id = "side:wb:fonts";
        const Rect rr{x, y, wd, 30};
        addHit(id, rr, [this] { openFonts({}); });
        const bool sel = wb_.fontsPage;
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(L"Fonts", {tx0, rr.y, cw - 30, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
        const std::size_t n = studio::fonts::familiesIn(ownFontsFolder(wb_.world, wb_.book)).size();
        if (n) gfx_.text(std::to_wstring(n), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
        if (focused(id)) focusRing(rr);
        y += 32;
    }
    {
        const std::string id = "side:wb:share";
        const Rect rr{x, y, wd, 30};
        addHit(id, rr, [this] { shareBible(); });
        const bool sel = wb_.share;
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(L"Share the bible", {tx0, rr.y, cw - 30, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
        if (focused(id)) focusRing(rr);
        y += 32;
    }
    {
        const std::string id = "side:wb:checks";
        const Rect rr{x, y, wd, 30};
        addHit(id, rr, [this] { openEntry({}); wb_.checks = true; wb_.scroll = 0; invalidate(); });
        const bool sel = wb_.checks;
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(L"Checks", {tx0, rr.y, cw - 30, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
        if (focused(id)) focusRing(rr);
        y += 44;
    }
    // Image boards.
    gfx_.text(L"IMAGE BOARDS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    linkText("side:wb:newboard", tx0 + cw - gfx_.measureWidth(L"+ New", Font::Small), y - 2, L"+ New", [this] { newBoard(); });
    y += 24;
    {
        const json bj = boardsData();
        int shown = 0;
        for (auto& bd : bj["boards"]) {
            if (!bd.value("entry", "").empty()) continue;   // kept on an entry's page
            ++shown;
            const std::string bid = bd.value("id", "");
            const std::string id = "side:wb:board:" + bid;
            const Rect rr{x, y, wd, 30};
            addHit(id, rr, [this, bid] {
                commitFields();
                wb_.board = bid;
                wb_.item.clear();
                wb_.entry.clear();
                wb_.scroll = 0;
                wb_.panelScroll = 0;
                setFocus({Focus::Board, bid, {}});
                invalidate();
            });
            const bool sel = wb_.board == bid;
            const float hv = hoverAmount(id);
            if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, pal_.ribbon); }
            else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
            gfx_.text(w(bd.value("name", "Board")), {tx0, rr.y, cw - 30, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink,
                      {Align::Left, false, true, true});
            gfx_.text(std::to_wstring(bd["items"].size()), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
            if (focused(id)) focusRing(rr);
            y += 32;
        }
        if (!shown) {
            y += gfx_.text(L"Pictures for this world: maps, places, moods. A character's own boards are on their page.", {tx0, y, cw, 40}, Font::Small, pal_.inkFaint) + 6;
        }
    }
    y += 20;
    // The books it covers.
    gfx_.text(L"BOOKS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 22;
    std::vector<const studio::Book*> books;
    if (wb_.world.empty()) { if (const studio::Book* b = lib_->book(wb_.book)) books.push_back(b); }
    else for (const studio::Book* b : lib_->liveBooks()) if (b->worldId == wb_.world) books.push_back(b);
    for (const studio::Book* b : books) {
        linkText("side:wb:book:" + b->id, tx0, y, w(b->title), [this, id = b->id] { openWriting(id); });
        y += 24;
    }
    if (books.empty()) { gfx_.text(L"No books yet.", {tx0, y, cw, 18}, Font::Small, pal_.inkFaint, {Align::Left, false}); y += 24; }
    // Starting again (0.38.1), out of the way at the foot.
    y += 18;
    linkText("side:wb:import", tx0, y, L"Import many pages\u2026", [this] { importManyPages(false); });
    y += 30;
    linkText("side:wb:reset", tx0, y, L"Reset the bible\u2026", [this] { resetBible(); });
    y += 30;
    wb_.sideContent = y + wb_.sideScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawWorldPage(const Rect& r) {
    wb_.scopeEntries = lib_->entriesOf(wb_.world, wb_.book);
    if (!wb_.board.empty()) { drawBoard(r); return; }
    if (!wb_.familyRoot.empty()) { drawFamilyTree(r); return; }
    if (!wb_.mapView.empty()) { drawMapView(r); return; }
    if (wb_.share) { drawShare(r); return; }
    if (wb_.relationsPage) { drawRelations(r); return; }
    if (wb_.fontsPage) { drawFonts(r); return; }
    if (wb_.checks) { drawChecks(r); return; }
    if (wb_.glossary) { drawGlossary(r); return; }
    if (wb_.languages) { drawLanguages(r); return; }
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const Entry* e = wb_.entry.empty() ? nullptr : lib_->entry(wb_.entry);
    if (e && e->deleted) { e = nullptr; wb_.entry.clear(); }
    // The cards use the page's whole width (0.46); one entry's page keeps a readable measure.
    const float cw = std::min(r.w - pad * 2, e ? 980.0f : 1640.0f);
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;

    if (!e) {
        // ---- All entries (or one kind), as cards.
        std::wstring scopeName;
        if (const World* wo = lib_->world(wb_.world)) scopeName = w(wo->name);
        else if (const studio::Book* b = lib_->book(wb_.book)) scopeName = w(b->title);
        std::vector<const Entry*> list;
        // "Find in the bible" (0.43): every word typed must be in the name, other names, summary or details.
        std::vector<std::string> findWords;
        {
            std::string cur;
            for (char ch : find_ + " ") {
                if (ch == ' ' || ch == ',') { if (!cur.empty()) findWords.push_back(cur); cur.clear(); }
                else cur += (char)std::tolower((unsigned char)ch);
            }
        }
        auto found = [&](const Entry& x) {
            if (findWords.empty()) return true;
            std::string hay = x.name + "\n" + x.summary + "\n" + x.details;
            for (auto& a : x.aliases) hay += "\n" + a;
            for (auto& c : hay) c = (char)std::tolower((unsigned char)c);
            for (auto& fw : findWords) if (hay.find(fw) == std::string::npos) return false;
            return true;
        };
        for (const Entry* x : lib_->entriesOf(wb_.world, wb_.book)) {
            if (!found(*x)) continue;
            if (!wb_.kind.empty() && x->kind != wb_.kind) continue;
            if (wb_.facet.rfind("under=", 0) == 0) { if (!isUnder(*lib_, *x, wb_.facet.substr(6))) continue; }
            else if (!facetMatches(*x, wb_.facet)) continue;
            list.push_back(x);
        }
        std::wstring title = wb_.kind.empty() ? scopeName : w(entryKindPlural(wb_.kind));
        if (const size_t eq = wb_.facet.find('='); eq != std::string::npos) {
            const Facet* f = findFacet(wb_.facet.substr(0, eq));
            const std::string v = wb_.facet.substr(eq + 1);
            if (f) title += L": " + (v == "-" ? f->title + L" not set" : w(v));
            else if (wb_.facet.rfind("under=", 0) == 0) if (const Entry* r = lib_->entry(v)) title += L": " + w(r->name);
        }
        std::wstring sub = std::to_wstring(list.size()) + (list.size() == 1 ? L" entry" : L" entries");
        if (!findWords.empty()) sub += L" found";
        if (!wb_.kind.empty()) sub += L" in " + scopeName;
        y += pageHeader(m, L"WORLDBUILDING", title, sub);
        {
            // The find box, and for characters how they're grouped, on one row.
            const Rect fr{m.x, y - 6, std::min(320.0f, cw * 0.5f), 32};
            gfx_.fillRounded(fr, 6, pal_.surface);
            gfx_.strokeRounded(fr, 6, GetFocus() == fields_[FFind] ? pal_.ribbon : pal_.rule);
            {
                // A magnifier, drawn.
                const float gx = fr.x + 15, gy = fr.y + fr.h / 2 - 1;
                gfx_.circle(gx, gy, 5, pal_.inkFaint, false, 1.4f);
                gfx_.line(gx + 3.6f, gy + 3.6f, gx + 7.5f, gy + 7.5f, pal_.inkFaint, 1.6f);
            }
            placeField(FFind, {fr.x + 22, fr.y, fr.w - 22 - (find_.empty() ? 0 : 26), fr.h});
            if (!find_.empty()) {
                // Clear: a drawn cross (not every font has one).
                const Rect xr{fr.right() - 28, fr.y + 4, 24, fr.h - 8};
                addHit("wb:findclear", xr, [this] { setFieldText(FFind, ""); find_.clear(); invalidate(); });
                const float hv = hoverAmount("wb:findclear");
                if (hv > 0.01f) gfx_.fillRounded(xr, 4, pal_.sunken.withAlpha(hv));
                const float cx = xr.x + xr.w / 2, cy = xr.y + xr.h / 2;
                gfx_.line(cx - 4, cy - 4, cx + 4, cy + 4, pal_.inkMuted, 1.4f);
                gfx_.line(cx - 4, cy + 4, cx + 4, cy - 4, pal_.inkMuted, 1.4f);
            }
            if (wb_.kind == "character" && !list.empty() && fr.right() + 380 <= m.right()) {
                const float gx = m.right() - 300;
                gfx_.text(L"Group by", {gx - 70, fr.y, 66, fr.h}, Font::Small, pal_.inkMuted, {Align::Right, false, false, true});
                const int at = wb_.groupBy == "race" ? 1 : wb_.groupBy == "none" ? 2 : 0;
                segmented("wb:groupby", {gx, fr.y + 2, 300, 28}, {L"Gender or sex", L"Race", L"Nothing"}, at, [this](int i) {
                    wb_.groupBy = i == 1 ? "race" : i == 2 ? "none" : "gender";
                    invalidate();
                });
            } else if (wb_.kind == "character" && !list.empty()) {
                y += 42;
                gfx_.text(L"Group by", {m.x, y - 4, 70, 28}, Font::Small, pal_.inkMuted, {Align::Left, false, false, true});
                const int at = wb_.groupBy == "race" ? 1 : wb_.groupBy == "none" ? 2 : 0;
                segmented("wb:groupby", {m.x + 72, y - 4, 300, 28}, {L"Gender or sex", L"Race", L"Nothing"}, at, [this](int i) {
                    wb_.groupBy = i == 1 ? "race" : i == 2 ? "none" : "gender";
                    invalidate();
                });
            }
            y += 46;
        }
        if (findWords.empty()) y = drawKindSuggestions(m, y);
        if (wb_.kind == "character" && wb_.facet.empty() && findWords.empty()) {
            // The families in the bible, each opening its tree.
            const auto fams = lib_->families(wb_.world, wb_.book);
            if (!fams.empty()) {
                gfx_.text(L"FAMILY TREES", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
                y += 26;
                float lx = m.x;
                for (const auto& fg : fams) {
                    const Entry* rootE = lib_->entry(fg.root);
                    if (!rootE) continue;
                    const std::wstring nm = w(fg.name) +
                                            L"  \u00B7  " + std::to_wstring(fg.people);
                    const float lw2 = gfx_.measureWidth(nm, Font::Body) + 30;
                    if (lx + lw2 > m.right() && lx > m.x) { lx = m.x; y += 36; }
                    const Rect chip{lx, y, lw2, 30};
                    const std::string id = "wb:fam:" + fg.root;
                    addHit(id, chip, [this, rid = fg.root] { openFamilyTree(rid, true); });
                    const float hv = hoverAmount(id);
                    gfx_.fillRounded(chip, 15, pal_.surface.mix(pal_.sunken, 0.4f * hv));
                    gfx_.strokeRounded(chip, 15, pal_.rule.mix(pal_.ribbon, hv));
                    gfx_.text(nm, chip, Font::Body, pal_.ink, {Align::Center, false, true, true});
                    if (focused(id)) focusRing(chip);
                    lx += lw2 + 8;
                }
                y += 52;
            }
        }
        if ((wb_.kind == "place" || wb_.kind == "realm") && wb_.facet.empty() && findWords.empty()) {
            // The bible's maps, each opening its place's page at the map.
            std::vector<const Entry*> maps;
            for (const Entry* x : lib_->entriesOf(wb_.world, wb_.book)) if (!x->mapBlob.empty() && (x->kind == "place" || x->kind == "realm")) maps.push_back(x);
            if (!maps.empty()) {
                gfx_.text(L"MAPS", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
                y += 26;
                const float tw = 200, th = 120, gap = 12;
                float lx = m.x;
                for (const Entry* x : maps) {
                    if (lx + tw > m.right() && lx > m.x) { lx = m.x; y += th + 34 + gap; }
                    const Rect tr{lx, y, tw, th};
                    const std::string id = "wb:mapcard:" + x->id;
                    addHit(id, {tr.x, tr.y, tr.w, tr.h + 26}, [this, eid = x->id] { openMap(eid); });
                    const float hv = hoverAmount(id);
                    softShadow(tr, 4, 0.3f + 0.6f * hv);
                    if (!gfx_.drawImage(w(lib_->blobPath(x->mapBlob)), tr, true)) gfx_.fillRounded(tr, 4, pal_.sunken);
                    gfx_.text(w(x->name) + L"  \u00B7  " + std::to_wstring(x->pins.size()) + (x->pins.size() == 1 ? L" pin" : L" pins"),
                              {tr.x, tr.bottom() + 6, tw, 20}, Font::SmallStrong, pal_.ink, {Align::Left, false, true});
                    if (focused(id)) focusRing(tr);
                    lx += tw + gap;
                }
                y += th + 34 + 20;
            }
        }
        if (wb_.kind == "creature" && findWords.empty() && (wb_.facet.empty() || wb_.facet.rfind("under=", 0) == 0)) {
            y = drawCreatureTree(m, y);
        } else if (list.empty() && !findWords.empty()) {
            y += gfx_.text(L"Nothing in the bible matches \u201C" + w(find_) + L"\u201D" + (wb_.kind.empty() ? L"." : L" among the " + w(entryKindPlural(wb_.kind)) + L"."),
                           {m.x, y, std::min(cw, 640.0f), 60}, Font::Body, pal_.inkMuted) + 16;
        } else if (list.empty()) {
            const std::wstring hint = wb_.kind.empty()
                ? L"Keep track of who and what is in your books: characters, places, groups, things, lore and events. Each entry has "
                  L"its details, notes and relationships, and lists the chapters it appears in."
                : L"None yet.";
            y += gfx_.text(hint, {m.x, y, std::min(cw, 640.0f), 80}, Font::Body, pal_.inkMuted) + 16;
            button("wb:first", {m.x, y, 180, 34}, wb_.kind.empty() ? L"Add the first entry" : L"Add one", [this] { newEntry(wb_.kind); }, true);
            y += 50;
        } else {
            // Each entry's first book, worked out again when the library has changed.
            const std::string key = wb_.world + "|" + wb_.book + "|" + std::to_string(lib_->changesMade());
            if (key != wb_.firstBookKey) {
                wb_.firstBookKey = key;
                wb_.bookOrder = booksInReadingOrder(*lib_, wb_.world, wb_.book);
                wb_.firstBook.clear();
                for (size_t bi = 0; bi < wb_.bookOrder.size(); ++bi)
                    for (auto& [node, ids] : lib_->appearancesIn(wb_.bookOrder[bi]))
                        for (const Id& eid : ids) wb_.firstBook.emplace(eid, bi);
            }
            // Places inside other places follow them (depth-first), and go in their outermost place's book.
            std::map<Id, Id> parentOf;
            std::set<Id> inList;
            for (const Entry* x : list) inList.insert(x->id);
            for (const Entry* x : list) if (Id p = parentPlace(*lib_, *x); !p.empty() && inList.count(p)) parentOf[x->id] = p;
            auto rootOf = [&](Id id) { for (int guard = 0; guard < 32 && parentOf.count(id); ++guard) id = parentOf[id]; return id; };
            struct Item { const Entry* e; int depth; };
            std::map<size_t, std::vector<Item>> groups;   // book index (npos: in no book yet) -> entries
            std::set<Id> placed;
            std::function<void(const Entry*, int, size_t)> addTree = [&](const Entry* x, int depth, size_t g) {
                if (!placed.insert(x->id).second || depth > 8) return;
                groups[g].push_back({x, depth});
                for (const Entry* c : list) if (auto it = parentOf.find(c->id); it != parentOf.end() && it->second == x->id) addTree(c, depth + 1, g);
            };
            for (const Entry* x : list) {
                if (parentOf.count(x->id)) continue;   // comes with its parent
                const auto it = wb_.firstBook.find(rootOf(x->id));
                addTree(x, 0, it == wb_.firstBook.end() ? std::string::npos : it->second);
            }
            for (const Entry* x : list) if (!placed.count(x->id)) addTree(x, 0, std::string::npos);   // a loop of "Located in"

            const float gap = 14;
            const int cols = std::max(1, (int)((cw + gap) / (290 + gap)));   // 0.46: a little wider, more room for the summary
            const float cardW = (cw - gap * (cols - 1)) / cols, cardH = 118;   // 0.43: picture, two lines, details
            const bool headings = groups.size() > 1 || (groups.size() == 1 && groups.begin()->first != std::string::npos && wb_.bookOrder.size() > 1);
            for (auto& [g, items] : groups) {
            if (headings) {
                std::wstring head = L"Not in a book yet", note;
                if (g != std::string::npos && g < wb_.bookOrder.size())
                    if (const studio::Book* b = lib_->book(wb_.bookOrder[g])) {
                        head = w(b->title);
                        const auto ss = lib_->seriesOfBook(b->id);
                        if (!ss.empty()) {
                            note = w(ss.front()->name);
                            if (const Membership* mb = lib_->membership(ss.front()->id, b->id); mb && !mb->number.empty())
                                note += L", book " + w(mb->number);
                        }
                    }
                gfx_.text(upper(head), {m.x, y + 4, cw, 18}, Font::Label, pal_.inkMuted, {Align::Left, false, true});
                if (!note.empty()) gfx_.text(note, {m.x, y + 4, cw, 18}, Font::Small, pal_.inkFaint, {Align::Right, false, true});
                gfx_.line(m.x, y + 26.5f, m.right(), y + 26.5f, pal_.rule);
                y += 38;
            }
            // Characters: by gender or sex within the book (the listed order, then anything else, then not set).
            std::vector<std::pair<std::wstring, std::vector<Item>>> subs;
            std::string subKind;   // how the sub-headings look: "gender", "race", "creature"
            if (wb_.kind == "creature" && wb_.facet.rfind("under=", 0) != 0) {
                // Creatures by their top-level kind (Dragon: the dragon, its types and the dragons themselves).
                subKind = "creature";
                std::vector<std::wstring> order;
                std::map<std::wstring, std::vector<Item>> by;
                for (auto& it : items) {
                    const Id r = rootOf(it.e->id);
                    bool family = r != it.e->id;
                    if (!family) for (auto& o : items) if (o.e != it.e && rootOf(o.e->id) == it.e->id) family = true;
                    const Entry* re = lib_->entry(r);
                    const std::wstring label = family && re ? w(re->name) : L"Other creatures";
                    if (!by.count(label)) order.push_back(label);
                    by[label].push_back(it);
                }
                std::stable_partition(order.begin(), order.end(), [](const std::wstring& l) { return l != L"Other creatures"; });
                for (auto& l : order) subs.push_back({l, by[l]});
                if (subs.size() == 1 && subs[0].first == L"Other creatures") subs[0].first.clear();
            } else if (wb_.kind == "character" && wb_.groupBy == "race" && wb_.facet.rfind("race=", 0) != 0) {
                subKind = "race";
                const Facet* rf = findFacet("race");
                std::map<std::wstring, std::vector<Item>> by;
                for (auto& it : items) by[w(facetOf(*it.e, *rf))].push_back(it);
                std::vector<Item> unset;
                if (auto f = by.find(L""); f != by.end()) { unset = f->second; by.erase(f); }
                for (auto& [k, v] : by) subs.push_back({k, v});
                if (!unset.empty()) subs.push_back({L"Not set", unset});
                if (subs.size() == 1 && subs[0].first == L"Not set") subs[0].first.clear();
            } else if (wb_.kind == "character" && wb_.groupBy == "gender" && wb_.facet.rfind("gender=", 0) != 0) {
                subKind = "gender";
                const Facet* gf = findFacet("gender");
                std::map<std::wstring, std::vector<Item>> by;
                for (auto& it : items) by[w(facetOf(*it.e, *gf))].push_back(it);
                for (auto& o : gf->options) if (auto f = by.find(o); f != by.end()) { subs.push_back({o, f->second}); by.erase(f); }
                std::vector<Item> unset;
                if (auto f = by.find(L""); f != by.end()) { unset = f->second; by.erase(f); }
                for (auto& [k, v] : by) subs.push_back({k, v});
                if (!unset.empty()) subs.push_back({L"Not set", unset});
                if (subs.size() == 1 && subs[0].first == L"Not set") subs[0].first.clear();   // nothing set yet: no sub-headings
            } else subs.push_back({L"", items});
            for (auto& [subLabel, subItems] : subs) {
            if (!subLabel.empty()) {
                const std::wstring sign = subKind == "gender" ? genderSign(subLabel) : std::wstring();
                const cjui::Color sc = subKind == "gender" ? genderColor(subLabel, pal_)
                                     : subLabel == L"Not set" || subLabel == L"Other creatures" ? pal_.inkMuted
                                                                                               : kindColor(subKind, pal_.dark);
                float lx = m.x;
                if (!sign.empty()) {   // the sign, then the group's name
                    drawGenderSign(gfx_, m.x + 9, y + 10, 10, subLabel, sc);
                    lx += 26;
                }
                gfx_.text(subLabel + L"  \u00B7  " + std::to_wstring(subItems.size()), {lx, y, cw - (lx - m.x), 20}, Font::SmallStrong, sc,
                          {Align::Left, false, true});
                y += 26;
            }
            for (size_t i = 0; i < subItems.size(); ++i) {
                const Entry* x = subItems[i].e;
                const int depth = subItems[i].depth;
                const int col = (int)i % cols;
                const Rect cr{m.x + col * (cardW + gap), y, cardW, cardH};
                const std::string id = "wb:card:" + x->id;
                addHit(id, cr, [this, eid = x->id] { openEntry(eid); });
                const float hv = hoverAmount(id);
                softShadow(cr, 4, hv);
                const cjui::Color kc = kindColor(x->kind, pal_.dark);
                gfx_.fillRounded(cr, 4, pal_.surface);
                gfx_.strokeRounded(cr, 4, pal_.rule.mix(kc, 0.35f * hv), 1);
                // The kind's colour: a band down the left edge, and the initials' circle.
                gfx_.fill({cr.x + 1, cr.y + 4, 3, cr.h - 8}, kc);
                // The picture (a portrait's top), or the initials in a ring of the kind's colour.
                const Rect pic{cr.x + 14, cr.y + 14, 72, 90};
                if (x->imageBlob.empty() || !gfx_.drawImage(w(lib_->blobPath(x->imageBlob)), pic, true)) {
                    const Rect ring{pic.x + 4, pic.y + 13, 64, 64};
                    gfx_.fillRounded(ring, 32, kc.withAlpha(pal_.dark ? 0.20f : 0.13f));
                    gfx_.strokeRounded(ring.inset(0.75f), 31.25f, kc.withAlpha(0.45f), 1.2f);
                    gfx_.text(initials(x->name), ring, Font::Heading, kc.mix(pal_.ink, 0.35f), {Align::Center, false, false, true});
                } else {
                    gfx_.stroke({pic.x - 0.5f, pic.y - 0.5f, pic.w + 1, pic.h + 1}, pal_.rule, 1);
                }
                const float tx = pic.right() + 14, tw = cr.right() - 14 - tx;
                {
                    std::wstring label = upper(w(entryKindLabel(x->kind)));
                    for (const Facet* f : facetsFor(x->kind))
                        if (f->key != "gender")
                            if (const std::string v = facetOf(*x, *f); !v.empty()) label += L"  \u00B7  " + upper(w(v));
                    gfx_.text(label, {tx, cr.y + 13, tw - 20, 16}, Font::Label, kc, {Align::Left, false, true});
                }
                // A character's gender or sex: its sign in the card's corner, in the group's colour.
                if (x->kind == "character")
                    if (const std::wstring g = w(facetOf(*x, *findFacet("gender"))); !g.empty())
                        drawGenderSign(gfx_, cr.right() - 18, cr.y + 20, 9, g, genderColor(g, pal_));
                gfx_.text(w(x->name), {tx, cr.y + 29, tw, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
                float ty = cr.y + 52;
                // A place inside another: "in Akunda", and a mark per level.
                if (depth > 0)
                    if (auto pit = parentOf.find(x->id); pit != parentOf.end())
                        if (const Entry* par = lib_->entry(pit->second)) {
                            for (int d = 0; d < depth && d < 4; ++d) gfx_.circle(cr.right() - 14 - d * 9, cr.y + 40, 2.5f, kc.withAlpha(0.6f), true);
                            gfx_.text((x->kind == "creature" ? L"a kind of " : L"in ") + w(par->name), {tx, ty, tw, 18}, Font::Small,
                                      kc.mix(pal_.inkMuted, 0.4f), {Align::Left, false, true});
                            ty += 18;
                        }
                if (!x->summary.empty()) {
                    // At most two lines (one under a place's "in ..."), cut at a word with an ellipsis, so the
                    // summary can never run under the details whatever the screen's text measures (0.46).
                    const int lines = depth > 0 ? 1 : 2;
                    const std::wstring s = linesOf(gfx_, w(x->summary), tw, Font::Small, lines);
                    gfx_.pushClip({tx, ty, tw, std::max(18.0f, cr.bottom() - 32 - ty)});
                    gfx_.text(s, {tx, ty, tw, 40}, Font::Small, pal_.inkMuted, {Align::Left, lines > 1, false});
                    gfx_.popClip();
                }
                // A few details as chips along the foot: the first ones written that the label doesn't already show.
                {
                    float chx = tx;
                    const float chy = cr.bottom() - 30;
                    int shown = 0;
                    size_t at = 0;
                    while (at < x->details.size() && shown < 3) {
                        size_t nl = x->details.find('\n', at);
                        if (nl == std::string::npos) nl = x->details.size();
                        const std::string line = trimmedCopy(x->details.substr(at, nl - at));
                        at = nl + 1;
                        const size_t colon = line.find(':');
                        if (colon == std::string::npos) continue;
                        const std::string lab = trimmedCopy(line.substr(0, colon)), val = trimmedCopy(line.substr(colon + 1));
                        if (lab.empty() || val.empty() || val.size() > 40) continue;
                        bool facet = false;
                        for (const Facet* f : facetsFor(x->kind)) { size_t c2; if (isFacetLine(line, *f, c2)) facet = true; }
                        if (facet) continue;
                        const std::wstring lw = w(lab), vw = w(val);
                        const float lwid = gfx_.measureWidth(lw, Font::Small), vwid = gfx_.measureWidth(vw, Font::Small);
                        const float room = tw - (chx - tx);
                        if (room < 48 || (shown > 0 && lwid + vwid + 22 > room)) break;   // the first may be shortened, the rest fit or wait
                        const float chw = std::min(lwid + vwid + 22, room);
                        const Rect chip{chx, chy, chw, 20};
                        gfx_.fillRounded(chip, 10, pal_.sunken.mix(pal_.surface, 0.3f));
                        gfx_.text(lw, {chip.x + 8, chip.y, lwid, chip.h}, Font::Small, pal_.inkFaint, {Align::Left, false, false, true});
                        gfx_.text(vw, {chip.x + 12 + lwid, chip.y, chip.w - lwid - 18, chip.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
                        chx += chw + 6;
                        ++shown;
                    }
                }
                if (focused(id)) focusRing(cr);
                if (col == cols - 1 || i + 1 == subItems.size()) y += cardH + gap;
            }
            if (!subLabel.empty()) y += 6;
            }
            y += headings ? 10 : 0;
            }
        }
    } else {
        // ---- One entry.
        const std::wstring back = wb_.kind.empty() ? L"‹ Everything" : L"‹ " + w(entryKindPlural(wb_.kind));
        linkText("wb:back", m.x, y, back, [this] { openEntry({}); });
        // Where it is: the places it sits in, each a link (0.48).
        if (e->kind == "place" || e->kind == "realm" || e->kind == "creature") {
            const auto path = placePath(*lib_, *e);
            float px = m.x + gfx_.measureWidth(back, Font::Small) + 34;
            for (size_t i = 0; i < path.size(); ++i) {
                const std::wstring t = w(path[i]->name);
                const float tw2 = gfx_.measureWidth(t, Font::Small);
                if (px + tw2 > m.right()) break;
                linkText("wb:path:" + path[i]->id, px, y, t, [this, id = path[i]->id] { openEntry(id); });
                px += tw2 + 18;
                gfx_.text(L"\u203A", {px - 4, y + 2, 14, 20}, Font::Small, pal_.inkFaint, {Align::Left, false});
                px += 14;
            }
            if (!path.empty()) gfx_.text(w(e->name), {px, y + 3, m.right() - px, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        }
        y += 34;
        const bool hasPic = !e->imageBlob.empty();
        const float picW = hasPic ? std::min(220.0f, cw * 0.3f) : 0;
        float ty = y;
        const float tx = m.x + (hasPic ? picW + 28 : 0), tw = m.right() - tx;
        if (hasPic) {
            const auto [iw, ih] = gfx_.imageSize(w(lib_->blobPath(e->imageBlob)));
            const float ph = iw > 0 ? std::min(picW * ih / (float)iw, picW * 1.5f) : picW;
            gfx_.drawImage(w(lib_->blobPath(e->imageBlob)), {m.x, y, picW, ph}, true);
            gfx_.stroke({m.x, y, picW, ph}, pal_.rule);
            ty = y;
            y += ph;
        }
        {
            std::wstring label = upper(w(entryKindLabel(e->kind)));
            for (const Facet* f : facetsFor(e->kind))
                if (const std::string v = facetOf(*e, *f); !v.empty()) label += L"  \u00B7  " + upper(w(v));
            gfx_.text(label, {tx, ty, tw, 16}, Font::Label, kindColor(e->kind, pal_.dark), {Align::Left, false});
        }
        ty += 20;
        ty += gfx_.text(w(e->name), {tx, ty, tw, 90}, Font::SerifTitle, pal_.ink) + 4;
        if (!e->pronunciation.empty()) ty += gfx_.text(L"Said \u201C" + w(e->pronunciation) + L"\u201D", {tx, ty, tw, 22}, Font::Body, pal_.inkMuted) + 6;
        if (const studio::Book* rb = lib_->book(e->revealedIn("entry")))
            ty += gfx_.text(L"SPOILER \u00B7 FIRST MET IN " + upper(w(rb->title)), {tx, ty, tw, 18}, Font::Label, pal_.demo, {Align::Left, false}) + 8;
        if (!e->aliases.empty()) {
            std::wstring a = L"Also ";
            for (size_t i = 0; i < e->aliases.size(); ++i) a += (i ? L", " : L"") + std::wstring(L"“") + w(e->aliases[i]) + L"”";
            ty += gfx_.text(a, {tx, ty, tw, 60}, Font::Small, pal_.inkMuted) + 8;
        }
        if (!e->summary.empty()) ty += drawLinkedText(e->summary, {tx, ty, std::min(tw, 640.0f), 0}, Font::Body, pal_.ink, e->id, "wb:ls:") + 12;
        // Details: "Label: value" lines as a table.
        if (!e->details.empty()) {
            ty += 8;
            size_t at = 0;
            while (at <= e->details.size()) {
                size_t nl = e->details.find('\n', at);
                if (nl == std::string::npos) nl = e->details.size();
                std::string line = e->details.substr(at, nl - at);
                at = nl + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty()) continue;
                const size_t colon = line.find(':');
                const float lw = std::min(160.0f, tw * 0.35f);
                if (colon != std::string::npos && colon > 0 && colon < 40) {
                    std::string val = line.substr(colon + 1);
                    while (!val.empty() && val.front() == ' ') val.erase(0, 1);
                    gfx_.text(w(line.substr(0, colon)), {tx, ty, lw - 10, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
                    if (const studio::Book* rb = lib_->book(e->revealedIn("detail:" + line.substr(0, colon)))) {
                        const std::wstring t = L"spoiler \u00B7 " + w(rb->title);
                        const float bw = std::min(gfx_.measureWidth(t, Font::Small) + 14, 220.0f);
                        const Rect badge{tx + std::min(tw, 640.0f) - bw, ty, bw, 20};
                        gfx_.fillRounded(badge, 10, pal_.demo.withAlpha(0.12f));
                        gfx_.text(t, badge, Font::Small, pal_.demo, {Align::Center, false, true, true});
                    }
                    ty += std::max(20.0f, drawLinkedText(val, {tx + lw, ty, std::min(tw - lw, 480.0f), 0}, Font::Small, pal_.ink, e->id,
                                                         "wb:ld" + std::to_string(at) + ":")) + 4;
                } else {
                    ty += gfx_.text(w(line), {tx, ty, std::min(tw, 640.0f), 80}, Font::Small, pal_.ink) + 4;
                }
                gfx_.line(tx, ty - 1.5f, tx + std::min(tw, 640.0f), ty - 1.5f, pal_.rule.withAlpha(0.5f));
                ty += 2;
            }
        }
        y = std::max(y, ty) + 26;

        // The page's sections, from the kind's template: written ones in full, the rest as a row to start.
        {
            const auto secs = lib_->entrySections(*e);
            std::vector<std::string> empty;
            for (const auto& [title, text] : secs) {
                if (text.empty()) { empty.push_back(title); continue; }
                const std::string id = "wb:sec:" + title;
                gfx_.text(upper(w(title)), {m.x, y, cw - 60, 18}, Font::Label, wb_.section == title ? pal_.ribbon : pal_.inkFaint, {Align::Left, false});
                if (const studio::Book* rb = lib_->book(e->revealedIn("section:" + title)))
                    gfx_.text(L"SPOILER \u00B7 " + upper(w(rb->title)), {m.x + gfx_.measureWidth(upper(w(title)), Font::Label) + 14, y, 300, 18}, Font::Label,
                              pal_.demo, {Align::Left, false, true});
                linkText(id, m.right() - gfx_.measureWidth(L"Edit", Font::Small), y - 2, L"Edit", [this, title] {
                    commitFields();
                    wb_.section = title;
                    wb_.editTemplate = false;
                    loadedFocus_ = {Focus::None, "-", ""};
                    invalidate();
                    focusFieldSoon_ = FSection;
                });
                y += 26;
                y += drawLinkedText(text, {m.x, y, std::min(cw, 680.0f), 0}, Font::Body, pal_.ink, e->id, "wb:lsec" + title + ":") + 24;
            }
            if (!empty.empty()) {
                gfx_.text(L"STILL TO WRITE", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
                y += 26;
                float lx = m.x;
                for (const std::string& t : empty) {
                    const std::wstring nm = L"+ " + w(t);
                    const float lw2 = gfx_.measureWidth(nm, Font::Small) + 26;
                    if (lx + lw2 > m.right() && lx > m.x) { lx = m.x; y += 34; }
                    const Rect chip{lx, y, lw2, 28};
                    const std::string id = "wb:secnew:" + t;
                    addHit(id, chip, [this, t] {
                        commitFields();
                        wb_.section = t;
                        wb_.editTemplate = false;
                        loadedFocus_ = {Focus::None, "-", ""};
                        focusFieldSoon_ = FSection;
                        invalidate();
                    });
                    const float hv = hoverAmount(id);
                    const bool on = wb_.section == t;
                    gfx_.fillRounded(chip, 14, pal_.surface.mix(pal_.sunken, 0.4f * hv));
                    gfx_.strokeRounded(chip, 14, on ? pal_.ribbon : pal_.rule.mix(pal_.ribbon, hv));
                    gfx_.text(nm, chip, Font::Small, on ? pal_.ink : pal_.inkMuted, {Align::Center, false, true, true});
                    if (focused(id)) focusRing(chip);
                    lx += lw2 + 8;
                }
                y += 50;
            }
        }

        // Notes for each book, in reading order.
        {
            if (wb_.bookOrder.empty()) wb_.bookOrder = booksInReadingOrder(*lib_, wb_.world, wb_.book);
            for (const Id& bid : wb_.bookOrder) {
                const std::string& note = e->bookNote(bid);
                const studio::Book* b = lib_->book(bid);
                if (note.empty() || !b) continue;
                gfx_.text(L"IN " + upper(w(b->title)), {m.x, y, cw - 60, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
                linkText("wb:bnedit:" + bid, m.right() - gfx_.measureWidth(L"Edit", Font::Small), y - 2, L"Edit", [this, bid] {
                    commitFields();
                    wb_.noteBook = bid;
                    loadedFocus_ = {Focus::None, "-", ""};
                    focusFieldSoon_ = FBookNote;
                    invalidate();
                });
                y += 26;
                y += drawLinkedText(note, {m.x, y, std::min(cw, 680.0f), 0}, Font::Body, pal_.ink, e->id, "wb:lbn" + bid + ":") + 24;
            }
        }

        if (e->kind == "character") y = drawFamilySection(m, y, *e);
        if (e->kind == "place" || e->kind == "realm") y = drawPlaceSection(m, y, *e);
        if (e->kind == "place" || e->kind == "realm") y = drawMapSection(m, y, *e);

        // Gallery: pictures of it, kept with the entry.
        {
            gfx_.text(L"GALLERY" + (e->gallery.empty() ? std::wstring() : L"  ·  " + std::to_wstring(e->gallery.size())), {m.x, y, cw, 18},
                      Font::Label, pal_.inkFaint, {Align::Left, false});
            linkText("wb:galadd", m.right() - gfx_.measureWidth(L"+ Add pictures…", Font::Small), y - 2, L"+ Add pictures…",
                     [this] { addGalleryPictures(); });
            y += 30;
            if (e->gallery.empty()) {
                y += gfx_.text(L"Art, sketches, references and looks for it. The main picture is set in the panel; any of these can be it.",
                               {m.x, y, std::min(cw, 640.0f), 40}, Font::Small, pal_.inkFaint) + 18;
            } else {
                const float th = 150, gap = 12;
                float lx = m.x, rowH = th;
                for (size_t i = 0; i < e->gallery.size(); ++i) {
                    const auto& pic = e->gallery[i];
                    const std::wstring path = w(lib_->blobPath(pic.blob));
                    const auto [pw, ph] = gfx_.imageSize(path);
                    const float tw = pw > 0 ? std::clamp(th * pw / (float)ph, 80.0f, 300.0f) : th;
                    const float capH = pic.caption.empty() ? 0 : 22;
                    if (lx + tw > m.right() && lx > m.x) { lx = m.x; y += rowH + gap; rowH = th; }
                    rowH = std::max(rowH, th + capH);
                    const Rect pr{lx, y, tw, th};
                    const std::string id = "wb:gal:" + std::to_string(i);
                    addHit(id, pr, [this, i] {
                        commitFields();
                        wb_.picture = (int)i;
                        loadedFocus_ = {Focus::None, "-", ""};
                        focusFieldSoon_ = FCaption;
                        invalidate();
                    });
                    const float hv = hoverAmount(id);
                    const bool sel = wb_.picture == (int)i;
                    softShadow(pr, 4, std::max(0.3f + 0.7f * hv, sel ? 1.0f : 0.0f));
                    if (!gfx_.drawImage(path, pr, true)) {
                        gfx_.fillRounded(pr, 4, pal_.sunken);
                        gfx_.text(L"Missing", pr, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
                    }
                    if (pic.blob == e->imageBlob) {
                        const Rect tag{pr.x + 6, pr.y + 6, gfx_.measureWidth(L"Main", Font::Label) + 14, 20};
                        gfx_.fillRounded(tag, 10, pal_.paper.withAlpha(0.9f));
                        gfx_.text(L"MAIN", tag, Font::Label, pal_.ink, {Align::Center, false, false, true});
                    }
                    if (sel) gfx_.strokeRounded(pr, 4, pal_.ribbon, 2.5f);
                    if (!pic.caption.empty())
                        gfx_.text(w(pic.caption), {pr.x, pr.bottom() + 4, tw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
                    if (focused(id)) focusRing(pr);
                    lx += tw + gap;
                }
                y += rowH + 30;
            }
        }

        // Image boards kept on this page.
        {
            const json bj = boardsData();
            std::vector<const json*> mine;
            for (auto& bd : bj["boards"]) if (bd.value("entry", "") == e->id) mine.push_back(&bd);
            gfx_.text(L"IMAGE BOARDS" + (mine.empty() ? std::wstring() : L"  ·  " + std::to_wstring(mine.size())), {m.x, y, cw, 18},
                      Font::Label, pal_.inkFaint, {Align::Left, false});
            linkText("wb:eboardnew", m.right() - gfx_.measureWidth(L"+ New board", Font::Small), y - 2, L"+ New board", [this] { newEntryBoard(); });
            y += 30;
            if (mine.empty()) {
                y += gfx_.text(L"Boards to arrange pictures freely: outfits, moods, a place's look. A board from the sidebar can be moved here "
                               L"from its panel.",
                               {m.x, y, std::min(cw, 640.0f), 40}, Font::Small, pal_.inkFaint) + 18;
            } else {
                const float bw = 260, bh = 150, gap = 12;
                const int cols = std::max(1, (int)((cw + gap) / (bw + gap)));
                const float cwid = (cw - gap * (cols - 1)) / cols;
                for (size_t i = 0; i < mine.size(); ++i) {
                    const json& bd = *mine[i];
                    const int col = (int)i % cols;
                    const Rect br{m.x + col * (cwid + gap), y, cwid, bh};
                    const std::string bid = bd.value("id", "");
                    const std::string id = "wb:eboard:" + bid;
                    addHit(id, br, [this, bid, eid = e->id] {
                        commitFields();
                        wb_.board = bid;
                        wb_.item.clear();
                        wb_.boardEntry = eid;
                        wb_.entry.clear();
                        wb_.scroll = 0;
                        wb_.panelScroll = 0;
                        setFocus({Focus::Board, bid, {}});
                        invalidate();
                    });
                    const float hv = hoverAmount(id);
                    gfx_.fillRounded(br, 6, pal_.surface);
                    gfx_.strokeRounded(br, 6, pal_.rule.mix(pal_.ribbon, hv));
                    // Up to three of its pictures, side by side.
                    const auto& items = bd["items"];
                    const Rect strip{br.x + 8, br.y + 8, br.w - 16, bh - 52};
                    const size_t n = std::min<size_t>(3, items.size());
                    if (n == 0) {
                        gfx_.fillRounded(strip, 4, pal_.sunken);
                        gfx_.text(L"Empty", strip, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
                    }
                    for (size_t k = 0; k < n; ++k) {
                        const float sw = (strip.w - 6 * (float)(n - 1)) / (float)n;
                        const Rect sr{strip.x + k * (sw + 6), strip.y, sw, strip.h};
                        if (!gfx_.drawImage(w(lib_->blobPath(items[k].value("blob", ""))), sr, true)) gfx_.fillRounded(sr, 4, pal_.sunken);
                    }
                    gfx_.text(w(bd.value("name", "Board")), {br.x + 12, br.bottom() - 40, br.w - 24, 20}, Font::BodyStrong, pal_.ink,
                              {Align::Left, false, true});
                    gfx_.text(std::to_wstring(items.size()) + (items.size() == 1 ? L" picture" : L" pictures"), {br.x + 12, br.bottom() - 21, br.w - 24, 16},
                              Font::Small, pal_.inkMuted, {Align::Left, false, true});
                    if (focused(id)) focusRing(br);
                    if (col == cols - 1 || i + 1 == mine.size()) y += bh + gap;
                }
                y += 22;
            }
        }

        // Relationships.
        // Members: a group's or a race's people (and creatures), as small cards, with a way to add them.
        auto isMembership = [&](const EntryLink& l) {
            return (l.to == e->id && l.label == "Member of") || (l.from == e->id && l.label == "Has member");
        };
        if (e->kind == "group" || e->kind == "race") {
            std::vector<std::pair<const Entry*, EntryLink>> members;
            for (const EntryLink& l : lib_->linksOf(e->id))
                if (isMembership(l))
                    if (const Entry* o = lib_->entry(l.from == e->id ? l.to : l.from); o && !o->deleted) members.push_back({o, l});
            std::sort(members.begin(), members.end(), [](auto& a, auto& b) { return a.first->name < b.first->name; });
            gfx_.text(L"MEMBERS" + (members.empty() ? std::wstring() : L"  \u00B7  " + std::to_wstring(members.size())), {m.x, y, cw, 18},
                      Font::Label, pal_.inkFaint, {Align::Left, false});
            linkText("wb:addmembers", m.right() - gfx_.measureWidth(L"+ Add members\u2026", Font::Small), y - 2, L"+ Add members\u2026",
                     [this] { addMembers(); });
            y += 30;
            if (members.empty())
                y += gfx_.text(L"No members yet. Add the characters who belong to it.", {m.x, y, cw, 20}, Font::Small, pal_.inkFaint) + 14;
            const float gap = 10, mw = 210, mh = 48;
            const int cols = std::max(1, (int)((cw + gap) / (mw + gap)));
            const float cwid = (cw - gap * (cols - 1)) / cols;
            for (size_t i = 0; i < members.size(); ++i) {
                const Entry* o = members[i].first;
                const int col = (int)i % cols;
                const Rect mr{m.x + col * (cwid + gap), y, cwid, mh};
                const std::string id = "wb:member:" + o->id;
                addHit(id, mr, [this, oid = o->id] { openEntry(oid); });
                const float hv = hoverAmount(id);
                const cjui::Color kc = kindColor(o->kind, pal_.dark);
                gfx_.fillRounded(mr, 6, pal_.surface);
                gfx_.strokeRounded(mr, 6, pal_.rule.mix(kc, 0.4f * hv), 1);
                const Rect pic{mr.x + 8, mr.y + 8, 32, 32};
                if (o->imageBlob.empty() || !gfx_.drawImage(w(lib_->blobPath(o->imageBlob)), pic, true)) {
                    gfx_.fillRounded(pic, 16, kc.withAlpha(pal_.dark ? 0.22f : 0.16f));
                    gfx_.text(initials(o->name), pic, Font::SmallStrong, kc.mix(pal_.ink, 0.35f), {Align::Center, false, false, true});
                }
                gfx_.text(w(o->name), {pic.right() + 10, mr.y + 6, mr.w - 70, 20}, Font::SmallStrong, pal_.ink, {Align::Left, false, true});
                std::wstring sub = w(entryKindLabel(o->kind));
                if (const std::string g = facetOf(*o, *findFacet("gender")); !g.empty() && o->kind == "character") sub += L" \u00B7 " + w(g);
                gfx_.text(sub, {pic.right() + 10, mr.y + 25, mr.w - 70, 16}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
                if (hv > 0.01f) {
                    const Rect xb{mr.right() - 26, mr.y + 14, 20, 20};
                    addHit("wb:unmember:" + o->id, xb, [this, l = members[i].second] { lib_->removeLink(l.from, l.to, l.label); invalidate(); }, false);
                    const float xh = hoverAmount("wb:unmember:" + o->id);
                    if (xh > 0.01f) gfx_.fillRounded(xb, 5, pal_.ink.withAlpha(0.08f * xh));
                    const float cx = xb.x + 10, cyy = xb.y + 10;
                    gfx_.line(cx - 4, cyy - 4, cx + 4, cyy + 4, pal_.inkMuted, 1.3f);
                    gfx_.line(cx + 4, cyy - 4, cx - 4, cyy + 4, pal_.inkMuted, 1.3f);
                }
                if (focused(id)) focusRing(mr);
                if (col == cols - 1 || i + 1 == members.size()) y += mh + gap;
            }
            y += 22;
        }

        gfx_.text(L"RELATIONSHIPS", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        linkText("wb:addrel", m.right() - gfx_.measureWidth(L"+ Add a relationship", Font::Small), y - 2, L"+ Add a relationship",
                 [this] { addRelationship(); });
        y += 28;
        std::vector<EntryLink> links;
        for (const EntryLink& l : lib_->linksOf(e->id))
            if (!((e->kind == "group" || e->kind == "race") && isMembership(l))) links.push_back(l);   // listed under Members
        if (links.empty()) {
            y += gfx_.text(L"None yet: link this entry to the people, places and things around it.", {m.x, y, cw, 20}, Font::Small,
                           pal_.inkFaint) + 12;
        }
        for (const EntryLink& l : links) {
            const bool mine = l.from == e->id;
            const Entry* other = lib_->entry(mine ? l.to : l.from);
            if (!other) continue;
            const std::wstring label = w(mine ? l.label : lib_->relationReverse(wb_.world, wb_.world.empty() ? wb_.book : Id{}, l.label));
            gfx_.text(label, {m.x, y, 160, 24}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
            linkText("wb:rel:" + other->id + l.label, m.x + 170, y + 4, w(other->name), [this, oid = other->id] { openEntry(oid); }, Font::Body);
            gfx_.text(w(entryKindLabel(other->kind)), {m.x + 170, y, cw - 170 - 80, 24}, Font::Small, pal_.inkFaint,
                      {Align::Right, false, true, true});
            linkText("wb:unrel:" + other->id + l.label, m.right() - gfx_.measureWidth(L"Remove", Font::Small), y + 4, L"Remove",
                     [this, l] { lib_->removeLink(l.from, l.to, l.label); invalidate(); });
            y += 28;
            gfx_.line(m.x, y - 2.5f, m.right(), y - 2.5f, pal_.rule.withAlpha(0.6f));
        }
        y += 22;

        // Notes.
        if (!e->notes.empty()) {
            gfx_.text(L"NOTES", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
            y += 26;
            y += drawLinkedText(e->notes, {m.x, y, std::min(cw, 680.0f), 0}, Font::Body, pal_.ink, e->id, "wb:ln:") + 26;
        }

        // Linked from: other entries whose text names this one.
        {
            std::vector<const Entry*> from;
            const std::vector<const Entry*> self{e};
            for (const Entry* o : wb_.scopeEntries) {
                if (o->id == e->id) continue;
                const std::string all = o->summary + "\n" + o->details + "\n" + o->notes;
                if (!findNames(all, self).empty()) from.push_back(o);
            }
            if (!from.empty()) {
                gfx_.text(L"LINKED FROM", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
                y += 28;
                float lx = m.x;
                for (const Entry* o : from) {
                    const std::wstring nm = w(o->name);
                    const float lw2 = gfx_.measureWidth(nm, Font::Body) + 30;
                    if (lx + lw2 > m.right() && lx > m.x) { lx = m.x; y += 34; }
                    const Rect chip{lx, y, lw2, 28};
                    const std::string id = "wb:from:" + o->id;
                    addHit(id, chip, [this, oid = o->id] { openEntry(oid); });
                    const float hv = hoverAmount(id);
                    gfx_.fillRounded(chip, 14, pal_.surface.mix(pal_.sunken, 0.4f * hv));
                    gfx_.strokeRounded(chip, 14, pal_.rule.mix(pal_.ribbon, hv));
                    gfx_.text(nm, chip, Font::Body, pal_.ink, {Align::Center, false, true, true});
                    if (focused(id)) focusRing(chip);
                    lx += lw2 + 8;
                }
                y += 50;
            }
        }

        // Where it appears: looked up when the entry or its names change.
        std::string key = e->name;
        for (auto& a : e->aliases) key += "\n" + a;
        if (wb_.mentionsFor != e->id || wb_.mentionsKey != key) {
            wb_.mentions = lib_->mentionsOf(*e);
            wb_.mentionsFor = e->id;
            wb_.mentionsKey = key;
        }
        gfx_.text(L"APPEARS IN", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 28;
        if (wb_.mentions.empty()) {
            y += gfx_.text(L"Its name doesn't appear in the manuscripts yet. Other names it goes by are searched too.", {m.x, y, cw, 40},
                           Font::Small, pal_.inkFaint) + 12;
        }
        int total = 0;
        for (auto& mn : wb_.mentions) total += mn.count;
        if (!wb_.mentions.empty())
            y += gfx_.text(std::to_wstring(total) + (total == 1 ? L" mention in " : L" mentions in ") + std::to_wstring(wb_.mentions.size()) +
                               (wb_.mentions.size() == 1 ? L" chapter or scene." : L" chapters and scenes."),
                           {m.x, y, cw, 20}, Font::Small, pal_.inkMuted) + 10;
        for (const Mention& mn : wb_.mentions) {
            const ManuscriptNode* n = lib_->node(mn.nodeId);
            const studio::Book* b = lib_->book(mn.bookId);
            if (!n || !b) continue;
            const std::string id = "wb:mention:" + mn.nodeId;
            const float sh = gfx_.measureHeight(w(mn.snippet), cw - 24, Font::Small);
            const Rect rr{m.x - 8, y, cw + 16, 30 + sh + 10};
            addHit(id, rr, [this, bid = b->id, nid = n->id, words = searchNames(*e)] {
                showInWriting(bid, nid, words);
            });
            const float hv = hoverAmount(id);
            if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.8f * hv));
            const std::wstring where = (wb_.world.empty() ? std::wstring() : w(b->title) + L"  ›  ") +
                                       w(n->title.empty() ? std::string(label(n->kind)) : n->title);
            gfx_.text(where, {m.x, y + 6, cw - 90, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            gfx_.text(std::to_wstring(mn.count) + (mn.count == 1 ? L" time" : L" times"), {m.right() - 90, y + 6, 90, 20}, Font::Small,
                      pal_.inkFaint, {Align::Right, false});
            gfx_.text(w(mn.snippet), {m.x, y + 28, cw - 24, sh + 4}, Font::Small, pal_.inkMuted);
            if (focused(id)) focusRing(rr);
            y += rr.h + 4;
        }
    }
    wb_.content = y + wb_.scroll - r.y + 40;
    wb_.scroll = std::clamp(wb_.scroll, 0.0f, std::max(0.0f, wb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawWorldPanel(const Rect& r) {
    gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
    gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    if (!wb_.board.empty()) { drawBoardPanel(r); return; }
    if (wb_.languages) { drawLanguagePanel(r); return; }
    if (wb_.share) { drawSharePanel(r); return; }
    if (wb_.relationsPage) { drawRelationsPanel(r); return; }
    if (wb_.fontsPage) { drawFontsPanel(r); return; }
    if (!wb_.familyRoot.empty() && wb_.entry.empty()) { drawFamilyPanel(r); return; }
    if (const Entry* ce = wb_.entry.empty() ? nullptr : lib_->entry(wb_.entry)) {
        // The section written in the panel: the one picked, else the first of the template.
        const auto secs = lib_->entrySections(*ce);
        if (std::none_of(secs.begin(), secs.end(), [&](auto& s) { return s.first == wb_.section; })) {
            wb_.section = secs.empty() ? std::string() : secs[0].first;
            loadedFocus_ = {Focus::None, "-", ""};
        }
        if (wb_.picture >= (int)ce->gallery.size()) wb_.picture = -1;
    }
    if (!(focus_ == loadedFocus_)) loadFields();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - wb_.panelScroll;
    const Entry* e = wb_.entry.empty() ? nullptr : lib_->entry(wb_.entry);
    std::map<int, float> fieldTop;   // where some fields are, to scroll to them
    if (e && !e->deleted) {
        panelHeading(c, y, L"ENTRY", true);
        // A whole page from elsewhere (0.37), and taking the last one back.
        // Taking a paste back stays at the top, where it's wanted straight after one (the paste tools are at the foot, 0.43).
        if (wb_.pasteUndo && wb_.pasteUndo->entry == e->id) {
            linkText("wb:pasteundo", c.x, y, L"Undo the paste", [this] { undoEntryPaste(); });
            y += 32;
        }
        fieldTop[FName] = y;
        y = panelField(c, y, L"Name", FName);
        panelChoice("wb:kind", c, y, L"Kind", w(entryKindLabel(e->kind)), [this] {
            const Entry* x = lib_->entry(wb_.entry);
            if (!x) return;
            std::vector<std::wstring> items;
            int cur = 0;
            for (size_t i = 0; i < entryKinds().size(); ++i) {
                items.push_back(w(entryKindLabel(entryKinds()[i])));
                if (entryKinds()[i] == x->kind) cur = (int)i;
            }
            const int i = popupAt(hwnd(), items, cur);
            if (i < 0 || i == cur) return;
            commitFields();
            Entry u = *lib_->entry(wb_.entry);
            u.kind = entryKinds()[i];
            lib_->updateEntry(u);
            invalidate();
        });
        if (e->kind == "creature" || e->kind == "character") {
            // What it's a kind of: another creature in the bible (Dragon > Wyvern > Bluebolt). A character
            // that's a kind of creature is a named one (Bluebolt the wyvern), listed under its kind.
            Id pid;
            for (const EntryLink& l : lib_->linksOf(e->id))
                if (l.from == e->id && l.label == "Kind of") if (const Entry* o = lib_->entry(l.to); o && !o->deleted && o->kind == "creature") pid = o->id;
            const Entry* parent = lib_->entry(pid);
            const bool ch = e->kind == "character";
            panelChoice("wb:kindof", c, y, ch ? L"Kind of creature (for a named creature)" : L"A kind of",
                        parent ? w(parent->name) : ch ? L"Not a creature" : L"Nothing (a species of its own)", [this] {
                const Entry* x = lib_->entry(wb_.entry);
                if (!x) return;
                std::vector<const Entry*> options;
                for (const Entry* o : lib_->entriesOf(wb_.world, wb_.book))
                    if (o->kind == "creature" && o->id != x->id && !isUnder(*lib_, *o, x->id)) options.push_back(o);   // never under itself
                std::vector<std::wstring> items;
                Id now;
                for (const EntryLink& l : lib_->linksOf(x->id)) if (l.from == x->id && l.label == "Kind of") now = l.to;
                int checked = (int)options.size();
                for (size_t i = 0; i < options.size(); ++i) { items.push_back(w(options[i]->name)); if (options[i]->id == now) checked = (int)i; }
                items.push_back(x->kind == "character" ? L"No (not a creature)" : L"Nothing (a species of its own)");
                if (options.empty()) { flash(L"Add the kinds of creature to the bible first (a Dragon, say), then choose one here.", 6); return; }
                const int i = popupAt(hwnd(), items, checked);
                if (i < 0 || i == checked) return;
                setKindOf(x->id, i < (int)options.size() ? options[(size_t)i]->id : Id{});
            });
        }
        for (const Facet* f : facetsFor(e->kind)) {
            const std::string cur = facetOf(*e, *f);
            panelChoice("wb:facet:" + f->key, c, y, f->title, cur.empty() ? L"Not set" : w(cur), [this, f] {
                const Entry* x = lib_->entry(wb_.entry);
                if (!x) return;
                std::vector<std::wstring> items = facetChoices(*f, lib_->entriesOf(wb_.world, wb_.book), x->kind);
                const size_t n = items.size();
                items.push_back(L"Not set");
                if (f->key == "race") items.push_back(L"New race\u2026");
                else if (f->key == "realm") items.push_back(L"New realm or plane\u2026");
                else if (f->openList) items.push_back(L"Write a new one\u2026");
                const std::wstring now = w(facetOf(*x, *f));
                int checked = now.empty() ? (int)n : -1;
                for (size_t i = 0; i < n; ++i) if (items[i] == now) checked = (int)i;
                const int i = popupAt(hwnd(), items, checked);
                if (i < 0 || i == checked) return;
                commitFields();
                if ((f->key == "race" || f->key == "realm") && i == (int)n + 1) {
                    // A new race or realm: made in the bible, opened with its name ready to type. Choose it on
                    // the entry afterwards.
                    const bool race = f->key == "race";
                    const Id rid = lib_->createEntry(wb_.world, wb_.book, f->key, race ? "New race" : "New realm");
                    if (rid.empty()) return;
                    openEntry(rid);
                    focusFieldSoon_ = FName;
                    flash(race ? L"Name the race, then choose it on the character." : L"Name the realm, then choose it on the entry.", 6);
                    return;
                }
                if (f->openList && i == (int)n + 1) {
                    // Anything else (a type of place): a line in the details, ready to finish.
                    Entry u = *lib_->entry(wb_.entry);
                    u.details = withFacet(u, *f, std::string());
                    u.details += (u.details.empty() || u.details.back() == '\n' ? "" : "\n") + f->written + ": ";
                    lib_->updateEntry(u);
                    loadedFocus_ = {Focus::None, "-", ""};
                    focusFieldSoon_ = FSynopsis;
                    flash(L"Write it after \u201c" + w(f->written) + L": \u201d in the details.", 6);
                    invalidate();
                    return;
                }
                Entry u = *lib_->entry(wb_.entry);
                u.details = withFacet(u, *f, i < (int)n ? narrow(items[i]) : std::string());
                lib_->updateEntry(u);
                // A race or realm that isn't in the bible yet goes in as one of its Races or Realms.
                if ((f->key == "race" || f->key == "realm") && i < (int)n) {
                    bool known = false;
                    for (const Entry* r : lib_->entriesOf(wb_.world, wb_.book)) if (r->kind == f->key && w(r->name) == items[i]) known = true;
                    if (!known && items[i] != L"Other" && !lib_->createEntry(wb_.world, wb_.book, f->key, narrow(items[i])).empty())
                        flash(L"Added " + items[i] + (f->key == "race" ? L" to the bible's races." : L" to the bible's realms."), 4);
                }
                loadedFocus_ = {Focus::None, "-", ""};   // the details field shows the new line
                invalidate();
            });
        }
        y = panelField(c, y, L"Other names (separate with commas)", FTags);
        y = panelField(c, y, L"How it's said (for the glossary: ZAH-mez)", FAlt);
        y = panelField(c, y, L"Summary", FDescription, 70);
        y = panelImage(c, y, L"Picture", e->imageBlob, [this] { chooseImage(Focus::Entry, wb_.entry, 0); }, [this] {
            if (const Entry* x = lib_->entry(wb_.entry)) { Entry u = *x; u.imageBlob.clear(); lib_->updateEntry(u); }
        }, false);
        y = panelField(c, y, L"Details (one per line, like Age: 34)", FSynopsis, 120);

        // The selected gallery picture.
        if (wb_.picture >= 0 && wb_.picture < (int)e->gallery.size()) {
            panelHeading(c, y, L"GALLERY PICTURE", false);
            const auto& pic = e->gallery[(size_t)wb_.picture];
            const std::wstring path = w(lib_->blobPath(pic.blob));
            const auto [pw, ph] = gfx_.imageSize(path);
            const float bh = pw > 0 ? std::min(c.w * ph / (float)pw, 220.0f) : 120.0f;
            const float bw = pw > 0 ? std::min(c.w, bh * pw / (float)ph) : c.w;
            if (!gfx_.drawImage(path, {c.x, y, bw, bh}, true)) gfx_.fillRounded({c.x, y, c.w, bh}, 3, pal_.sunken);
            y += bh + 10;
            fieldTop[FCaption] = y;
            y = panelField(c, y, L"Caption", FCaption);
            if (pic.blob != e->imageBlob) {
                linkText("wb:galmain", c.x, y, L"Use as the main picture", [this] {
                    commitFields();
                    if (const Entry* x = lib_->entry(wb_.entry); x && wb_.picture >= 0 && wb_.picture < (int)x->gallery.size()) {
                        Entry u = *x;
                        u.imageBlob = x->gallery[(size_t)wb_.picture].blob;
                        lib_->updateEntry(u);
                        flash(L"It's now the entry's main picture.", 4);
                    }
                });
                y += 26;
            }
            linkText("wb:galremove", c.x, y, L"Remove from the gallery", [this] { galleryMenu((size_t)wb_.picture); });
            y += 26;
            linkText("wb:galdone", c.x, y, L"Done", [this] { commitFields(); wb_.picture = -1; loadedFocus_ = {Focus::None, "-", ""}; invalidate(); });
            y += 36;
        }

        // The page's sections, one at a time.
        panelHeading(c, y, L"PAGE", false);
        {
            const auto secs = lib_->entrySections(*e);
            if (!secs.empty()) {
                panelChoice("wb:section", c, y, L"Section", w(wb_.section), [this] {
                    const Entry* x = lib_->entry(wb_.entry);
                    if (!x) return;
                    const auto ss = lib_->entrySections(*x);
                    std::vector<std::wstring> items;
                    int cur = -1;
                    for (size_t i = 0; i < ss.size(); ++i) {
                        items.push_back(w(ss[i].first) + (ss[i].second.empty() ? L"" : L"  ✓"));
                        if (ss[i].first == wb_.section) cur = (int)i;
                    }
                    const int i = popupAt(hwnd(), items, cur);
                    if (i < 0 || i == cur) return;
                    commitFields();
                    wb_.section = ss[(size_t)i].first;
                    loadedFocus_ = {Focus::None, "-", ""};
                    focusFieldSoon_ = FSection;
                    invalidate();
                });
                // A choice (0.37): its options as buttons, and the box for anything else.
                const SectionSpec spec = lib_->entrySectionSpec(*e, wb_.section);
                if (!spec.options.empty()) {
                    const std::string cur = e->section(wb_.section);
                    gfx_.text(spec.many ? L"Pick any that fit" : L"Pick one", {c.x, y, c.w, 18}, Font::Small, pal_.inkFaint, {Align::Left, false});
                    y += 22;
                    float lx = c.x;
                    for (const std::string& o : spec.options) {
                        const std::wstring nm = w(o);
                        const float bw = std::min(c.w, gfx_.measureWidth(nm, Font::Small) + 28);
                        if (lx + bw > c.right() && lx > c.x) { lx = c.x; y += 36; }
                        const Rect chip{lx, y, bw, 30};
                        const std::string id = "wb:opt:" + o;
                        const bool on = sectionOptionPicked(spec, cur, o);
                        addHit(id, chip, [this, o] {
                            commitFields();
                            const Entry* x = lib_->entry(wb_.entry);
                            if (!x) return;
                            const SectionSpec sp = lib_->entrySectionSpec(*x, wb_.section);
                            Entry u = *x;
                            u.setSection(wb_.section, toggleSectionOption(sp, x->section(wb_.section), o));
                            lib_->updateEntry(u);
                            loadedFocus_ = {Focus::None, "-", ""};
                            invalidate();
                        });
                        const float hv = hoverAmount(id);
                        gfx_.fillRounded(chip, 15, on ? pal_.ribbon : pal_.surface.mix(pal_.sunken, 0.4f * hv));
                        gfx_.strokeRounded(chip, 15, on ? pal_.ribbon : pal_.rule.mix(pal_.ribbon, hv));
                        gfx_.text(nm, chip, Font::Small, on ? pal_.paper : pal_.ink, {Align::Center, false, true, true});
                        if (focused(id)) focusRing(chip);
                        lx += bw + 8;
                    }
                    y += 44;
                }
                fieldTop[FSection] = y;
                y = panelField(c, y, spec.options.empty() ? w(wb_.section) : L"Or in your own words", FSection, spec.options.empty() ? 170 : 70);
            }
        }
        {
            const std::wstring kindName = w(entryKindLabel(e->kind));
            if (!wb_.editTemplate) {
                linkText("wb:tpl", c.x, y, L"Change the sections every " + kindName + L" page has…", [this] {
                    commitFields();
                    wb_.editTemplate = true;
                    loadedFocus_ = {Focus::None, "-", ""};
                    focusFieldSoon_ = FTemplate;
                    invalidate();
                });
                y += 34;
            } else {
                const World* wo = lib_->world(e->worldId);
                fieldTop[FTemplate] = y;
                y = panelField(c, y, L"Sections of a " + kindName + L" page, one per line" + (wo ? L" (all of " + w(wo->name) + L")" : L""), FTemplate, 130);
                linkText("wb:tpldone", c.x, y, L"Done", [this] {
                    commitFields();
                    wb_.editTemplate = false;
                    loadedFocus_ = {Focus::None, "-", ""};
                    invalidate();
                });
                linkText("wb:tplreset", c.x + 60, y, L"Use the built-in sections", [this] {
                    const Entry* x = lib_->entry(wb_.entry);
                    if (!x) return;
                    wb_.editTemplate = false;
                    commitFields();
                    lib_->setEntryTemplate(x->worldId, x->kind, defaultEntryTemplate(x->kind));
                    loadedFocus_ = {Focus::None, "-", ""};
                    invalidate();
                });
                y += 26;
                y += gfx_.text(L"For buttons to pick from, write the choices after a colon: “Status: Alive | Dead | Missing”. "
                               L"Add (any) to the name to pick several: “Traits (any): Brave | Loyal | Proud”. "
                               L"Writing already in a section you take out stays on the page, after the others.",
                               {c.x, y, c.w, 120}, Font::Small, pal_.inkFaint) + 12;
            }
        }

        // Notes for one book at a time.
        if (const Id nb = noteBookFor(*e); !nb.empty()) {
            const studio::Book* b = lib_->book(nb);
            if (wb_.bookOrder.size() > 1) {
                panelChoice("wb:notebook", c, y, L"Notes for the book", b ? w(b->title) : L"", [this] {
                    std::vector<std::wstring> items;
                    int cur = -1;
                    const Entry* x = lib_->entry(wb_.entry);
                    if (!x) return;
                    const Id now = noteBookFor(*x);
                    for (size_t i = 0; i < wb_.bookOrder.size(); ++i) {
                        const studio::Book* bk = lib_->book(wb_.bookOrder[i]);
                        items.push_back(bk ? w(bk->title) + (x->bookNote(bk->id).empty() ? L"" : L"  ✓") : L"?");
                        if (wb_.bookOrder[i] == now) cur = (int)i;
                    }
                    const int i = popupAt(hwnd(), items, cur);
                    if (i < 0 || i == cur) return;
                    commitFields();
                    wb_.noteBook = wb_.bookOrder[(size_t)i];
                    loadedFocus_ = {Focus::None, "-", ""};
                    focusFieldSoon_ = FBookNote;
                    invalidate();
                });
            }
            fieldTop[FBookNote] = y;
            y = panelField(c, y, L"In " + (b ? w(b->title) : std::wstring()) + L": what happens to it, what it knows", FBookNote, 110);
        }
        y = panelField(c, y, L"Notes", FNotes, 180);

        // Spoilers: what readers only learn in a later book.
        if (wb_.bookOrder.empty()) wb_.bookOrder = booksInReadingOrder(*lib_, wb_.world, wb_.book);
        if (!wb_.bookOrder.empty()) {
            panelHeading(c, y, L"SPOILERS", false);
            y += gfx_.text(L"Mark what readers only learn in a later book. A reader's guide or shared wiki for an earlier book leaves it out.",
                           {c.x, y, c.w, 60}, Font::Small, pal_.inkMuted) + 8;
            std::vector<std::pair<std::string, std::wstring>> facts{{"entry", L"The whole entry"}};
            if (!e->summary.empty()) facts.push_back({"summary", L"Summary"});
            {
                size_t at = 0;
                while (at <= e->details.size()) {
                    size_t nl = e->details.find('\n', at);
                    if (nl == std::string::npos) nl = e->details.size();
                    const std::string line = e->details.substr(at, nl - at);
                    at = nl + 1;
                    const size_t colon = line.find(':');
                    if (colon != std::string::npos && colon > 0 && colon < 40) facts.push_back({"detail:" + line.substr(0, colon), w(line.substr(0, colon))});
                }
            }
            for (auto& [t, text] : lib_->entrySections(*e)) if (!text.empty()) facts.push_back({"section:" + t, w(t)});
            for (auto& [key, label] : facts) {
                const studio::Book* rb = lib_->book(e->revealedIn(key));
                gfx_.text(label, {c.x, y, c.w * 0.5f, 22}, Font::Small, pal_.ink, {Align::Left, false, true, true});
                const std::wstring v = rb ? w(rb->title) : L"Not a spoiler";
                const float vw = gfx_.measureWidth(v, Font::Small);
                linkText("wb:spoil:" + key, c.right() - vw - 4, y + 3, v, [this, key = key] {
                    const Entry* x = lib_->entry(wb_.entry);
                    if (!x) return;
                    std::vector<std::wstring> items{L"Not a spoiler"};
                    int cur = 0;
                    for (size_t i = 0; i < wb_.bookOrder.size(); ++i) {
                        const studio::Book* bk = lib_->book(wb_.bookOrder[i]);
                        items.push_back(L"Revealed in " + (bk ? w(bk->title) : std::wstring(L"?")));
                        if (x->revealedIn(key) == wb_.bookOrder[i]) cur = (int)i + 1;
                    }
                    const int i = popupAt(hwnd(), items, cur);
                    if (i < 0 || i == cur) return;
                    commitFields();
                    Entry u = *lib_->entry(wb_.entry);
                    u.setRevealedIn(key, i == 0 ? Id{} : wb_.bookOrder[(size_t)i - 1]);
                    lib_->updateEntry(u);
                    invalidate();
                }, Font::Small);
                y += 26;
            }
            y += 14;
        }
        panelHeading(c, y, L"PAGES FROM ELSEWHERE", false);
        linkText("wb:paste", c.x, y, L"Fill this page from what you've copied", [this] { pasteFromClipboard(); });
        y += 30;
        linkText("wb:pasteall", c.x, y, L"Replace all its text with what you've copied", [this] { pasteFromClipboard(true); });
        tooltip("wb:pasteall", L"Everything written on this page goes and the copied page takes its place; its picture, gallery and map stay. Ctrl+Z puts it back.");
        y += 30;
        linkText("wb:blank", c.x, y, L"Copy a blank page to fill in", [this] { copyBlankPage(); });
        y += 30;
        y += gfx_.text(L"Or press Ctrl+V on this page with a copied page: what it has replaces what's here.", {c.x, y + 2, c.w, 40}, Font::Small,
                       pal_.inkFaint) + 14;
        linkText("wb:trash", c.x, y, L"Move to Trash", [this] {
            const Entry* x = lib_->entry(wb_.entry);
            if (!x) return;
            const std::wstring name = w(x->name);
            commitFields();
            if (lib_->trashEntry(wb_.entry)) {
                focus_ = {};
                loadedFocus_ = {Focus::None, "-", ""};
                openEntry({});
                flash(L"Moved “" + name + L"” to the Trash.", 5);
            }
        });
        y += 34;
    } else {
        // The world at a glance (0.43): how many of each kind, what changed lately, and the things to do from here.
        const auto all = lib_->entriesOf(wb_.world, wb_.book);
        std::wstring scope;
        if (const World* wo = lib_->world(wb_.world)) scope = w(wo->name);
        else if (const studio::Book* b = lib_->book(wb_.book)) scope = w(b->title);
        panelHeading(c, y, L"THIS WORLD", true);
        y += gfx_.text(scope, {c.x, y, c.w, 30}, Font::Heading, pal_.ink, {Align::Left, false, true}) + 2;
        y += gfx_.text(std::to_wstring(all.size()) + (all.size() == 1 ? L" entry, shared by every book in it." : L" entries, shared by every book in it."),
                       {c.x, y, c.w, 40}, Font::Small, pal_.inkMuted) + 12;
        {
            // A bar per kind with something in it.
            int most = 1;
            std::map<std::string, int> n;
            for (const Entry* x : all) most = std::max(most, ++n[x->kind]);
            for (const auto& k : entryKinds()) {
                if (!n[k]) continue;
                const std::string id = "wb:pk:" + k;
                const Rect row{c.x - 6, y, c.w + 12, 24};
                addHit(id, row, [this, k] { wb_.kind = k; wb_.facet.clear(); wb_.scroll = 0; openEntry({}); });
                const float hv = hoverAmount(id);
                if (hv > 0.01f) gfx_.fillRounded(row, 4, pal_.sunken.withAlpha(0.6f * hv));
                const cjui::Color kc = kindColor(k, pal_.dark);
                gfx_.text(w(entryKindPlural(k)), {c.x, y, c.w * 0.42f, 24}, Font::Small, pal_.ink, {Align::Left, false, true, true});
                const float bx = c.x + c.w * 0.44f, bw = c.w * 0.44f;
                gfx_.fillRounded({bx, y + 9, bw, 6}, 3, pal_.sunken);
                gfx_.fillRounded({bx, y + 9, std::max(6.0f, bw * n[k] / most), 6}, 3, kc);
                gfx_.text(std::to_wstring(n[k]), {c.x, y, c.w, 24}, Font::Small, pal_.inkMuted, {Align::Right, false, false, true});
                y += 26;
            }
            y += 10;
        }
        {
            // What changed lately.
            std::vector<const Entry*> recent(all.begin(), all.end());
            std::sort(recent.begin(), recent.end(), [](const Entry* a, const Entry* b) { return a->modified > b->modified; });
            if (!recent.empty()) {
                panelHeading(c, y, L"CHANGED LATELY", false);
                for (size_t i = 0; i < recent.size() && i < 5; ++i) {
                    const Entry* x = recent[i];
                    const std::string id = "wb:recent:" + x->id;
                    const Rect row{c.x - 6, y, c.w + 12, 26};
                    addHit(id, row, [this, eid = x->id] { openEntry(eid); });
                    const float hv = hoverAmount(id);
                    if (hv > 0.01f) gfx_.fillRounded(row, 4, pal_.sunken.withAlpha(0.6f * hv));
                    gfx_.circle(c.x + 4, y + 13, 3.5f, kindColor(x->kind, pal_.dark), true);
                    gfx_.text(w(x->name), {c.x + 16, y, c.w - 16, 26}, Font::Small, pal_.ink, {Align::Left, false, true, true});
                    y += 27;
                }
                y += 10;
            }
        }
        panelHeading(c, y, L"FROM HERE", false);
        auto act = [&](const std::string& id, const std::wstring& label, std::function<void()> fn) {
            linkText(id, c.x, y, label, std::move(fn));
            y += 30;
        };
        act("wb:pa:new", wb_.kind.empty() ? L"New entry\u2026" : L"New " + [&] { std::wstring l = w(entryKindLabel(wb_.kind)); for (auto& ch : l) ch = (wchar_t)towlower(ch); return l; }(),
            [this] { newEntry(wb_.kind); });
        act("wb:pa:import", L"Import many pages\u2026", [this] { importManyPages(false); });
        act("wb:pa:blank", L"Copy a blank page to fill in", [this] { copyBlankPageFor(wb_.kind.empty() ? std::string("character") : wb_.kind); });
        act("wb:pa:share", L"Share the bible\u2026", [this] { shareBible(); });
        y += gfx_.text(L"Paste a copied page onto the cards with Ctrl+V to make an entry from it.", {c.x, y + 4, c.w, 40}, Font::Small, pal_.inkFaint) + 10;
    }
    wb_.panelContent = y + wb_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
    if (focusFieldSoon_ >= 0 && focusFieldSoon_ < FieldCount) {
        if (placed_[focusFieldSoon_]) {
            SetFocus(fields_[focusFieldSoon_]);
            // A name is replaced as you type; writing goes on at the end.
            if (focusFieldSoon_ == FName) SendMessageW(fields_[focusFieldSoon_], EM_SETSEL, 0, -1);
            else SendMessageW(fields_[focusFieldSoon_], EM_SETSEL, (WPARAM)-1, -1);
            focusFieldSoon_ = -1;
        } else if (auto it = fieldTop.find(focusFieldSoon_); it != fieldTop.end()) {
            // Out of sight: scroll the panel to it, and focus it once it's placed.
            const float want = wb_.panelScroll + (it->second - (r.y + 50));
            wb_.panelScroll = std::clamp(want, 0.0f, std::max(0.0f, wb_.panelContent - r.h));
            invalidate();
        } else {
            focusFieldSoon_ = -1;
        }
    }
}

// The book whose notes the panel shows: the one picked, else the book open in Writing, else the first.
Id StudioApp::noteBookFor(const Entry& e) const {
    if (!e.bookId.empty()) return e.bookId;
    std::vector<Id> books;
    for (const studio::Book* b : lib_->liveBooks()) if (b->worldId == e.worldId && !e.worldId.empty()) books.push_back(b->id);
    auto in = [&](const Id& id) { return !id.empty() && std::find(books.begin(), books.end(), id) != books.end(); };
    if (in(wb_.noteBook)) return wb_.noteBook;
    if (in(writingBook_)) return writingBook_;
    if (!wb_.bookOrder.empty() && in(wb_.bookOrder[0])) return wb_.bookOrder[0];
    return books.empty() ? Id{} : books[0];
}

// Pictures from files into the entry's gallery (stored once in the library, listed in Artwork).
void StudioApp::addGalleryPictures() {
    const Entry* e = lib_->entry(wb_.entry);
    if (!e) return;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Pictures", L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp"}};
    dlg->SetFileTypes(1, spec);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    dlg->SetTitle((L"Add pictures of " + w(e->name)).c_str());
    std::vector<std::wstring> paths;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(dlg->GetResults(&arr))) {
            DWORD n = 0;
            arr->GetCount(&n);
            for (DWORD i = 0; i < n && i < 200; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &item))) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { paths.push_back(p); CoTaskMemFree(p); }
                    item->Release();
                }
            }
            arr->Release();
        }
    }
    dlg->Release();
    if (paths.empty()) return;
    commitFields();
    Entry u = *lib_->entry(wb_.entry);
    int added = 0, failed = 0;
    for (const std::wstring& p : paths) {
        std::ifstream f(p.c_str(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.empty() || bytes.size() > 80u * 1024 * 1024) { ++failed; continue; }
        std::wstring name = p.substr(p.find_last_of(L"\\/") + 1);
        if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos) name.resize(dot);
        const Id aid = lib_->addArtwork(bytes, narrow(name), "Bible");
        const Artwork* a = aid.empty() ? nullptr : lib_->artwork(aid);
        if (!a) { ++failed; continue; }
        if (std::none_of(u.gallery.begin(), u.gallery.end(), [&](auto& g) { return g.blob == a->blob; })) u.gallery.push_back({a->blob, ""});
        if (u.imageBlob.empty()) u.imageBlob = a->blob;   // the first picture is the main one
        ++added;
    }
    lib_->updateEntry(u);
    flash(std::to_wstring(added) + (added == 1 ? L" picture added to the gallery." : L" pictures added to the gallery.") +
              (failed ? L" " + std::to_wstring(failed) + L" couldn't be read." : L""), 5);
    invalidate();
}

// Taking a picture out of the gallery (it stays in Artwork).
void StudioApp::galleryMenu(size_t index) {
    const Entry* e = lib_->entry(wb_.entry);
    if (!e || index >= e->gallery.size()) return;
    commitFields();
    Entry u = *lib_->entry(wb_.entry);
    const std::string blob = u.gallery[index].blob;
    u.gallery.erase(u.gallery.begin() + (long)index);
    if (u.imageBlob == blob) u.imageBlob = u.gallery.empty() ? std::string() : u.gallery[0].blob;
    lib_->updateEntry(u);
    wb_.picture = -1;
    loadedFocus_ = {Focus::None, "-", ""};
    flash(L"Removed from the gallery. The picture stays in Artwork.", 5);
    invalidate();
}

// ------------------------------------------------------------- tidying and the creature tree

// Every world's bible tidied when a library opens: copies merged, books' own entries moved in.
void StudioApp::tidyBibles() {
    if (!lib_) return;
    int merged = 0, moved = 0;
    std::vector<std::string> names;
    for (const World* wo : lib_->liveWorlds()) {
        const auto r = lib_->tidyBible(wo->id);
        merged += r.merged;
        moved += r.moved;
        for (auto& n : r.mergedNames) names.push_back(n);
    }
    if (!merged && !moved) return;
    std::wstring msg = L"Tidied the world bible: ";
    if (merged) {
        msg += std::to_wstring(merged) + (merged == 1 ? L" copy merged" : L" copies merged");
        std::wstring list;
        for (size_t i = 0; i < names.size() && i < 4; ++i) list += (i ? L", " : L"") + w(names[i]);
        if (names.size() > 4) list += L"…";
        msg += L" (" + list + L")";
    }
    if (moved) msg += std::wstring(merged ? L"; " : L"") + std::to_wstring(moved) + L" entries moved in from their books";
    msg += L". The copies are in the Trash if you want one back.";
    flash(msg, 12);
}

void StudioApp::setKindOf(const Id& entryId, const Id& parent) {
    const Entry* x = lib_->entry(entryId);
    if (!x) return;
    if (!parent.empty()) {
        const Entry* p = lib_->entry(parent);
        if (!p || p->kind != "creature") return;
        if (isUnder(*lib_, *p, entryId)) { flash(L"A kind can't go under one of its own kinds.", 5); return; }
    }
    commitFields();
    for (const EntryLink& l : lib_->linksOf(entryId)) {
        if (l.from == entryId && l.label == "Kind of") lib_->removeLink(l.from, l.to, l.label);
        if (l.to == entryId && l.label == "Has kind") lib_->removeLink(l.from, l.to, l.label);
    }
    if (!parent.empty()) lib_->addLink(entryId, parent, "Kind of");
    if (const Entry* p = lib_->entry(parent)) {
        wb_.collapsed.erase(p->id);   // show where it went
        flash(w(x->name) + L" is now a kind of " + w(p->name) + L".", 4);
    } else flash(w(x->name) + L" is now a species of its own.", 4);
    invalidate();
}

void StudioApp::addKindUnder(const Id& parent) {
    const Entry* p = lib_->entry(parent);
    if (!p) return;
    commitFields();
    const Id id = lib_->createEntry(wb_.world, wb_.book, "creature", "New kind of " + p->name);
    if (id.empty()) { flash(w(lib_->lastError()), 6); return; }
    lib_->addLink(id, parent, "Kind of");
    wb_.collapsed.erase(parent);
    openEntry(id);
    flash(L"Name the new kind on the right.", 4);
}

// Right-click an entry: change what kind it is.
void StudioApp::entryKindMenu(const Id& id) {
    const Entry* x = lib_->entry(id);
    if (!x) return;
    std::vector<std::wstring> items;
    int cur = 0;
    for (size_t i = 0; i < entryKinds().size(); ++i) {
        items.push_back(L"Change to " + w(entryKindLabel(entryKinds()[i])));
        if (entryKinds()[i] == x->kind) cur = (int)i;
    }
    items.push_back(L"Open");
    const int i = popupAt(hwnd(), items, cur);
    if (i < 0 || i == cur) return;
    if (i == (int)entryKinds().size()) { openEntry(id); return; }
    commitFields();
    Entry u = *x;
    u.kind = entryKinds()[(size_t)i];
    lib_->updateEntry(u);
    flash(w(x->name) + L" is now " + w(entryKindLabel(u.kind)) + L".", 4);
    invalidate();
}

// Kinds that look wrong, with a way to change them or let them be.
float StudioApp::drawKindSuggestions(const Rect& m, float y) {
    const std::string key = "kind-ok:" + (wb_.world.empty() ? wb_.book : wb_.world);
    json ok = json::parse(lib_->setting(key), nullptr, false);
    if (!ok.is_array()) ok = json::array();
    std::vector<Library::KindSuggestion> list;
    for (auto& s : lib_->kindSuggestions(wb_.world, wb_.book)) {
        bool dismissed = false;
        for (auto& o : ok) if (o.is_string() && o.get<std::string>() == s.entry + ":" + s.kind) dismissed = true;
        const Entry* e = lib_->entry(s.entry);
        if (!dismissed && e && (wb_.kind.empty() || e->kind == wb_.kind || s.kind == wb_.kind)) list.push_back(s);
    }
    if (list.empty()) return y;
    if (!wb_.kindsOpen) {
        // One line, out of the way: open it to see them (0.43).
        const Rect note{m.x, y, m.w, 38};
        const std::string id = "wb:sug:open";
        addHit(id, note, [this] { wb_.kindsOpen = true; invalidate(); });
        const float hv = hoverAmount(id);
        gfx_.fillRounded(note, 6, pal_.surface.mix(pal_.demo, 0.06f + 0.06f * hv));
        gfx_.strokeRounded(note, 6, pal_.demo.withAlpha(0.45f + 0.3f * hv), 1);
        gfx_.circle(note.x + 18, note.y + note.h / 2, 4, pal_.demo, true);
        const std::wstring t = list.size() == 1 ? L"1 entry may be filed under the wrong kind." : std::to_wstring(list.size()) + L" entries may be filed under the wrong kind.";
        gfx_.text(t, {note.x + 32, note.y, note.w - 140, note.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(L"Check \u203A", {note.x, note.y, note.w - 16, note.h}, Font::SmallStrong, pal_.demo.mix(pal_.ink, 0.3f), {Align::Right, false, false, true});
        if (focused(id)) focusRing(note);
        return note.bottom() + 20;
    }
    const float rowH = 44;
    const Rect card{m.x, y, m.w, 40 + rowH * std::min<size_t>(list.size(), 6) + 8};
    gfx_.fillRounded(card, 6, pal_.surface);
    gfx_.strokeRounded(card, 6, pal_.demo.withAlpha(0.6f), 1);
    gfx_.text(L"CHECK THESE KINDS", {card.x + 18, card.y + 12, card.w - 36, 18}, Font::Label, pal_.inkMuted, {Align::Left, false});
    linkText("wb:sug:hide", card.right() - 18 - gfx_.measureWidth(L"Hide", Font::Small), card.y + 8, L"Hide", [this] { wb_.kindsOpen = false; invalidate(); });
    float ry = card.y + 40;
    for (size_t i = 0; i < list.size() && i < 6; ++i) {
        const auto& s = list[i];
        const Entry* e = lib_->entry(s.entry);
        const std::wstring head = w(e->name) + L" is filed as " + w(entryKindLabel(e->kind)) + L".";
        gfx_.text(head, {card.x + 18, ry, card.w - 320, 20}, Font::SmallStrong, pal_.ink, {Align::Left, false, true});
        gfx_.text(w(s.why), {card.x + 18, ry + 19, card.w - 320, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        const std::wstring keep = L"Keep it", change = L"Make it a " + w(entryKindLabel(s.kind));
        const float kw = gfx_.measureWidth(keep, Font::Small), cw2 = gfx_.measureWidth(change, Font::Small);
        linkText("wb:sug:keep:" + s.entry, card.right() - 18 - kw, ry + 8, keep, [this, key, tag = s.entry + ":" + s.kind] {
            json j = json::parse(lib_->setting(key), nullptr, false);
            if (!j.is_array()) j = json::array();
            j.push_back(tag);
            lib_->setSetting(key, j.dump());
            invalidate();
        });
        linkText("wb:sug:make:" + s.entry, card.right() - 18 - kw - 30 - cw2, ry + 8, change, [this, id = s.entry, kind = s.kind] {
            const Entry* x = lib_->entry(id);
            if (!x) return;
            commitFields();
            Entry u = *x;
            u.kind = kind;
            lib_->updateEntry(u);
            flash(w(u.name) + L" is now " + w(entryKindLabel(kind)) + L".", 4);
            invalidate();
        });
        ry += rowH;
    }
    return card.bottom() + 22;
}

// Creatures, one level at a time: the species as cards; a species' card shows its kinds; a kind's card
// shows the named ones (characters) of that kind. Each level can be dragged onto a card to move it.
float StudioApp::drawCreatureTree(const Rect& m, float y) {
    const auto all = lib_->entriesOf(wb_.world, wb_.book);
    std::map<Id, std::vector<const Entry*>> kids, named;
    std::vector<const Entry*> roots;
    for (const Entry* e : all) {
        Id up;
        for (const EntryLink& l : lib_->linksOf(e->id))
            if ((l.from == e->id && l.label == "Kind of") || (l.to == e->id && l.label == "Has kind")) {
                const Entry* o = lib_->entry(l.from == e->id ? l.to : l.from);
                if (o && !o->deleted && o->kind == "creature") up = o->id;
            }
        if (e->kind == "creature") {
            if (up.empty()) roots.push_back(e);
            else kids[up].push_back(e);
        } else if (!up.empty()) named[up].push_back(e);
    }
    // Where we are: the species list, or under one species or kind.
    const Id at = wb_.facet.rfind("under=", 0) == 0 ? Id(wb_.facet.substr(6)) : Id{};
    const Entry* here = at.empty() ? nullptr : lib_->entry(at);
    if (!at.empty() && (!here || here->deleted)) { wb_.facet.clear(); return y; }
    auto parentOfE = [&](const Entry* e) -> const Entry* {
        for (const EntryLink& l : lib_->linksOf(e->id))
            if (l.from == e->id && l.label == "Kind of") if (const Entry* o = lib_->entry(l.to); o && !o->deleted && o->kind == "creature") return o;
        return nullptr;
    };
    auto go = [this](const Id& id) { wb_.kind = "creature"; wb_.facet = id.empty() ? std::string() : "under=" + id; wb_.scroll = 0; invalidate(); };

    // The path back up: Creatures › Dragon › Wyvern.
    if (here) {
        std::vector<const Entry*> path;
        for (const Entry* p = here; p && path.size() < 12; p = parentOfE(p)) path.insert(path.begin(), p);
        float bx = m.x;
        linkText("wb:crumb:root", bx + 7, y, L"Creatures", [go] { go({}); });
        bx += gfx_.measureWidth(L"Creatures", Font::Small) + 22;
        for (const Entry* p : path) {
            gfx_.text(L"›", {bx, y, 14, 22}, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
            bx += 18;
            if (p == here) {
                gfx_.text(w(p->name), {bx, y, 300, 22}, Font::SmallStrong, pal_.ink, {Align::Left, false, true, true});
            } else {
                linkText("wb:crumb:" + p->id, bx + 7, y, w(p->name), [go, id = p->id] { go(id); });
                bx += gfx_.measureWidth(w(p->name), Font::Small) + 22;
            }
        }
        y += 36;
    }
    // What can be done here.
    if (drag_.active && drag_.kind == DragState::Entry) {
        const Rect zone{m.x, y - 4, m.w, 38};
        gfx_.fillRounded(zone, 6, pal_.surface);
        gfx_.strokeRounded(zone, 6, pal_.inkFaint, 1);
        gfx_.text(L"Drop here to make it a species of its own", zone, Font::Small, pal_.inkMuted, {Align::Center, false, false, true});
        addDrop({Drop::KindOf, zone, Id{}, {}, false, false, {}});
    } else {
        float bx = m.x;
        const std::wstring add = here ? L"+ Add a kind of " + w(here->name) : L"+ New species";
        const float aw = gfx_.measureWidth(add, Font::Small) + 28;
        button("wb:tree:addhere", {bx, y, aw, 30}, add, [this, at] { if (at.empty()) newEntry("creature"); else addKindUnder(at); }, false);
        bx += aw + 8;
        if (here) {
            const std::wstring open = L"Open " + w(here->name) + L"'s page";
            const float ow = gfx_.measureWidth(open, Font::Small) + 28;
            button("wb:tree:openhere", {bx, y, ow, 30}, open, [this, at] { openEntry(at); }, false);
            bx += ow + 8;
        }
        gfx_.text(L"Click a card to go in. Drag a card onto another to move it under that one.", {bx + 8, y, m.right() - bx - 8, 30}, Font::Small,
                  pal_.inkFaint, {Align::Left, false, true, true});
    }
    y += 46;

    // The cards at this level: kinds first, then the named ones.
    std::vector<std::pair<const Entry*, bool>> cards;   // entry, named (a character)
    for (const Entry* e : here ? kids[at] : roots) cards.push_back({e, false});
    if (here) for (const Entry* e : named[at]) cards.push_back({e, true});
    if (cards.empty()) {
        const std::wstring hint = here ? L"Nothing under " + w(here->name) + L" yet. Add its kinds, or make a character one of them (on the "
                                         L"character's page, choose “A creature?”)."
                                       : L"No creatures yet. Add a species (a Dragon, say), then its kinds (a Wyvern), then the named ones.";
        return y + gfx_.text(hint, {m.x, y, std::min(m.w, 640.0f), 80}, Font::Body, pal_.inkMuted) + 16;
    }
    const float gap = 16;
    const int cols = std::max(1, (int)((m.w + gap) / (240 + gap)));
    const float cardW = (m.w - gap * (cols - 1)) / cols, cardH = 128;
    bool namedHeading = false;
    int col = 0;
    for (size_t i = 0; i < cards.size(); ++i) {
        const Entry* e = cards[i].first;
        const bool isNamed = cards[i].second;
        if (isNamed && !namedHeading) {
            if (col) { y += cardH + gap; col = 0; }
            if (i > 0) y += 8;
            gfx_.text(L"NAMED " + upper(w(here->name)), {m.x, y, m.w, 18}, Font::Label, pal_.inkMuted, {Align::Left, false});
            gfx_.line(m.x, y + 24.5f, m.right(), y + 24.5f, pal_.rule);
            y += 36;
            namedHeading = true;
        } else if (!isNamed && i == 0 && here) {
            gfx_.text(L"KINDS OF " + upper(w(here->name)), {m.x, y, m.w, 18}, Font::Label, pal_.inkMuted, {Align::Left, false});
            gfx_.line(m.x, y + 24.5f, m.right(), y + 24.5f, pal_.rule);
            y += 36;
        }
        const Rect cr{m.x + col * (cardW + gap), y, cardW, cardH};
        const std::string id = "wb:tree:" + e->id;
        const int nKinds = (int)kids[e->id].size(), nNamed = (int)named[e->id].size();
        const bool deeper = !isNamed;   // a species or kind goes in; a named one opens
        addHit(id, cr, [this, go, eid = e->id, deeper] { if (deeper) go(eid); else openEntry(eid); });
        if (!isNamed) addDrop({Drop::KindOf, cr, e->id, {}, false, false, {}});
        const float hv = hoverAmount(id);
        softShadow(cr, 4, hv);
        const cjui::Color kc = kindColor(e->kind, pal_.dark);
        gfx_.fillRounded(cr, 4, pal_.surface);
        gfx_.strokeRounded(cr, 4, pal_.rule.mix(kc, 0.35f * hv), 1);
        gfx_.fill({cr.x + 1, cr.y + 4, 3, cr.h - 8}, kc);
        const Rect pic{cr.x + 14, cr.y + 14, 100, 100};
        if (e->imageBlob.empty() || !gfx_.drawImage(w(lib_->blobPath(e->imageBlob)), pic, true)) {
            gfx_.fillRounded(pic, 50, kc.withAlpha(pal_.dark ? 0.22f : 0.16f));
            gfx_.strokeRounded(pic.inset(0.75f), 49.25f, kc.withAlpha(0.45f), 1.5f);
            gfx_.text(initials(e->name), pic, Font::Title, kc.mix(pal_.ink, 0.35f), {Align::Center, false, false, true});
        }
        const float tx = pic.right() + 14, tw = cr.right() - 12 - tx;
        std::wstring label = isNamed ? upper(w(entryKindLabel(e->kind))) : here ? L"KIND OF " + upper(w(here->name)) : std::wstring(L"SPECIES");
        if (isNamed)
            for (const Facet* f : facetsFor(e->kind))
                if (const std::string v = facetOf(*e, *f); !v.empty()) label += L"  ·  " + upper(w(v));
        gfx_.text(label, {tx, cr.y + 14, tw, 16}, Font::Label, kc, {Align::Left, false, true});
        if (e->kind == "character")
            if (const std::wstring g = w(facetOf(*e, *findFacet("gender"))); !g.empty())
                if (const std::wstring sg = genderSign(g); !sg.empty())
                    gfx_.text(sg, {cr.right() - 40, cr.y + 4, 36, 36}, Font::Title, genderColor(g, pal_), {Align::Center, false, false, true});
        gfx_.text(w(e->name), {tx, cr.y + 31, tw, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        if (!e->summary.empty()) gfx_.text(w(e->summary), {tx, cr.y + 53, tw, 36}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        if (!isNamed) {
            std::wstring counts;
            if (nKinds) counts += std::to_wstring(nKinds) + (nKinds == 1 ? L" kind" : L" kinds");
            if (nNamed) counts += (counts.empty() ? L"" : L"  ·  ") + std::to_wstring(nNamed) + L" named";
            if (counts.empty()) counts = L"Nothing under it yet";
            gfx_.text(counts + L"  ›", {tx, cr.y + 76, tw, 18}, Font::Small, kc.mix(pal_.inkMuted, 0.3f), {Align::Left, false, true});
        }
        if (focused(id)) focusRing(cr);
        if (++col == cols || i + 1 == cards.size() || (cards[i + 1].second && !isNamed)) {
            y += cardH + gap;
            col = 0;
        }
    }
    return y + 20;
}

// ------------------------------------------------------------- family trees

void StudioApp::openFamilyTree(const Id& root, bool bloodline) {
    commitFields();
    wb_.board.clear();
    wb_.item.clear();
    const bool same = wb_.familyRoot == root && wb_.treeBloodline == bloodline;
    wb_.familyRoot = root;
    wb_.treeBloodline = bloodline;
    wb_.treeSelected = root;
    if (!same) {
        wb_.scroll = 0;
        wb_.treeX = -1;   // centred on the person on the first frame
    }
    setFocus({Focus::Entry, root, {}});
    invalidate();
}

// The panel beside a family tree: the person picked, their summary, and their close family as links that pick them.
void StudioApp::drawFamilyPanel(const Rect& r) {
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - wb_.panelScroll;
    const Entry* e = lib_->entry(wb_.treeSelected);
    if (!e || e->deleted) e = lib_->entry(wb_.familyRoot);
    if (e) {
        panelHeading(c, y, L"IN THE TREE", true);
        y += gfx_.text(w(e->name), {c.x, y, c.w, 60}, Font::Heading, pal_.ink) + 8;
        if (!e->summary.empty()) y += gfx_.text(w(e->summary), {c.x, y, c.w, 200}, Font::Small, pal_.inkMuted) + 12;
        const Id id = e->id;
        linkText("fam:open", c.x, y, L"Open " + w(e->name) + L"'s page", [this, id] { openEntry(id); });
        y += 26;
        if (id != wb_.familyRoot || wb_.treeBloodline) {
            linkText("fam:root", c.x, y, L"See the tree from " + w(e->name), [this, id] { openFamilyTree(id, false); });
            y += 26;
        }
        y += 10;
        const auto fam = lib_->familyOf(id);
        auto group = [&](const char* key, const wchar_t* title, const std::vector<Id>& ids, int role) {
            if (ids.empty()) return;
            gfx_.text(title, {c.x, y, c.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
            y += 22;
            for (const Id& o : ids) {
                const Entry* x = lib_->entry(o);
                if (!x || x->deleted) continue;
                const std::wstring nm = w(x->name);
                linkText("fam:pick:" + std::string(key) + ":" + o, c.x + 6, y, nm,
                         [this, o] { wb_.treeSelected = o; setFocus({Focus::Entry, o, {}}); invalidate(); });
                // What kind (twin sister, stepfather, adopted son), beside the name (0.46).
                if (role != 1)
                    if (auto it = fam.how.find(o); it != fam.how.end())
                        if (const std::wstring kw = kinWord(o, role, it->second); !kw.empty()) {
                            const float nx = c.x + 6 + gfx_.measureWidth(nm, Font::Small) + 22;
                            gfx_.text(kw, {nx, y, c.right() - nx, 20}, Font::Small, pal_.inkFaint, {Align::Left, false, true, true});
                        }
                y += 24;
            }
            y += 8;
        };
        group("pa", L"PARENTS", fam.parents, 0);
        group("pt", L"PARTNERS", fam.partners, 1);
        group("sb", L"BROTHERS AND SISTERS", fam.siblings, 2);
        group("ch", L"CHILDREN", fam.children, 3);
        y += gfx_.text(L"Click someone in the tree to pick them; double-click to see the tree from them.", {c.x, y, c.w, 60}, Font::Small,
                       pal_.inkFaint) + 12;
    }
    wb_.panelContent = y + wb_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

// "Save as a picture…" (0.45): the tree on screen, drawn for paper at twice its size, as a PNG.
void StudioApp::saveFamilyTreePicture() {
    if (!lib_ || wb_.familyRoot.empty()) return;
    const std::string name = studio::familytree::title(*lib_, wb_.familyRoot, wb_.treeBloodline);
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"PNG picture", L"*.png"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(L"png");
    std::wstring fileName = w(name);
    for (auto& c : fileName) if (wcschr(L"\\/:*?\"<>|", c)) c = L'-';
    dlg->SetFileName(fileName.c_str());
    dlg->SetTitle(L"Save the family tree as a picture");
    std::wstring path;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    if (path.empty()) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    studio::familytree::Options o;
    o.bloodline = wb_.treeBloodline;
    o.scale = 2;
    const auto pic = studio::familytree::render(*lib_, wb_.familyRoot, narrow(exeDir() + L"\\fonts"), o);
    if (!pic.ok) { cjui::messageBox(hwnd(), (L"The picture couldn't be made.\n\n" + w(pic.error)).c_str(), L"Family tree", MB_OK | MB_ICONWARNING); return; }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, pic.png.data(), (DWORD)pic.png.size(), &wrote, nullptr) && wrote == pic.png.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) { cjui::messageBox(hwnd(), L"The picture couldn't be saved there.", L"Family tree", MB_OK | MB_ICONWARNING); return; }
    flash(L"Saved " + w(name) + L" as a picture (" + std::to_wstring(pic.width) + L" \u00D7 " + std::to_wstring(pic.height) + L" pixels).", 6);
}

void StudioApp::zoomFamilyTree(float factor) {
    const float avail = pageRect_.w - 64;
    const float before = wb_.treeZoom > 0 ? wb_.treeZoom : std::clamp(wb_.treeW > 0 ? std::min(1.0f, avail / std::max(1.0f, wb_.treeW)) : 1.0f, 0.66f, 1.0f);
    if (factor <= 0) { wb_.treeZoom = 0; wb_.treeX = -1; invalidate(); return; }
    const float after = std::clamp(before * factor, 0.4f, 1.8f);
    const float mid = avail / 2 + wb_.treeX;   // keep the middle of the view where it is
    wb_.treeX = std::max(0.0f, mid * after / before - avail / 2);
    wb_.treeZoom = after;
    invalidate();
}

// Adds a parent, partner, sibling or child: someone already in the bible, or a new character
// (opened with its name ready to type).
namespace {
// The kinds of parent, child and brother or sister, with their links (0.46). `role` 0 parent, 2 sibling, 3 child.
struct KinKind { const char* how; const wchar_t* menu; };
const std::vector<KinKind>& kinKinds(int role) {
    static const std::vector<KinKind> parents{{"", L"Birth parent"}, {"adoptive", L"Adoptive parent"}, {"step", L"Step-parent"},
                                              {"foster", L"Foster parent"}};
    static const std::vector<KinKind> siblings{{"", L"Brother or sister"}, {"twin", L"Twin"}, {"half", L"Half-brother or half-sister"},
                                               {"step", L"Stepbrother or stepsister"}, {"adoptive", L"Adoptive brother or sister"},
                                               {"foster", L"Foster brother or sister"}};
    static const std::vector<KinKind> children{{"", L"Child"}, {"adoptive", L"Adopted child"}, {"step", L"Stepchild"}, {"foster", L"Foster child"}};
    return role == 0 ? parents : role == 2 ? siblings : children;
}
// The link from `who` that says `other` is their kin of this role and kind.
struct KinLink { Id from, to; std::string label; };
KinLink kinLink(const Id& who, const Id& other, int role, const std::string& how) {
    if (role == 2) {
        const std::string l = how == "twin" ? "Twin of" : how == "half" ? "Half-sibling of" : how == "step" ? "Step-sibling of"
                            : how == "adoptive" ? "Adoptive sibling of" : how == "foster" ? "Foster sibling of" : "Sibling of";
        return {who, other, l};
    }
    const std::string l = how == "adoptive" ? "Adoptive parent of" : how == "step" ? "Step-parent of" : how == "foster" ? "Foster parent of" : "Parent of";
    return role == 0 ? KinLink{other, who, l} : KinLink{who, other, l};
}
}  // namespace

// What someone is to a person, in words: "twin sister", "stepfather", "adopted son", "half-brother" (0.46).
std::wstring StudioApp::kinWord(const Id& other, int role, const std::string& how) {
    const Entry* o = lib_->entry(other);
    std::wstring g = o ? w(facetOf(*o, *findFacet("gender"))) : L"";
    for (auto& c : g) c = (wchar_t)towlower(c);
    const int sex = g.find(L"female") != std::wstring::npos || g.find(L"woman") != std::wstring::npos || g == L"f" ? 1
                  : g.find(L"male") != std::wstring::npos || g.find(L"man") != std::wstring::npos || g == L"m" ? 0 : 2;
    const wchar_t* base = role == 0 ? (sex == 1 ? L"mother" : sex == 0 ? L"father" : L"parent")
                        : role == 2 ? (sex == 1 ? L"sister" : sex == 0 ? L"brother" : L"sibling")
                                    : (sex == 1 ? L"daughter" : sex == 0 ? L"son" : L"child");
    if (how.empty()) return L"";
    if (how == "twin") return std::wstring(L"twin ") + base;
    if (how == "half") return std::wstring(L"half-") + base;
    if (how == "step") return std::wstring(L"step") + base;
    if (how == "foster") return std::wstring(L"foster ") + base;
    if (how == "adoptive") return (role == 3 ? std::wstring(L"adopted ") : std::wstring(L"adoptive ")) + base;
    return L"";
}

void StudioApp::addFamilyMember(const Id& who, int relation) {
    const Entry* me = lib_->entry(who);
    if (!me) return;
    POINT pt;
    GetCursorPos(&pt);
    // Which kind of parent, brother or sister, or child first (0.46).
    std::string how;
    if (relation != 1) {
        std::vector<cjui::MenuItem> kinds;
        const auto& ks = kinKinds(relation);
        for (size_t i = 0; i < ks.size(); ++i) kinds.push_back(cjui::MenuItem::item(ks[i].menu, (UINT)(1 + i)));
        const UINT k = cjui::showMenu(hwnd(), kinds, pt, pal_);
        if (!k) return;
        how = ks[k - 1].how;
    }
    const auto fam = lib_->familyOf(who);
    std::set<Id> already;
    for (auto* v : {&fam.parents, &fam.partners, &fam.siblings, &fam.children}) already.insert(v->begin(), v->end());
    std::vector<const Entry*> people;
    for (const Entry* o : lib_->entriesOf(wb_.world, wb_.book))
        if (o->kind == "character" && o->id != who && !already.count(o->id)) people.push_back(o);
    static const wchar_t* what[] = {L"parent", L"partner", L"brother or sister", L"child"};
    std::wstring role = what[relation];
    if (relation != 1 && !how.empty())
        for (auto& kk : kinKinds(relation)) if (kk.how == how) { role = kk.menu; for (auto& c : role) c = (wchar_t)towlower(c); }
    std::vector<cjui::MenuItem> menu{cjui::MenuItem::item(L"New character as " + w(me->name) + L"'s " + role + L"…", 1)};
    if (!people.empty()) menu.push_back(cjui::MenuItem::sep());
    for (size_t i = 0; i < people.size() && i < 300; ++i) menu.push_back(cjui::MenuItem::item(w(people[i]->name), (UINT)(10 + i)));
    const UINT ch = cjui::showMenu(hwnd(), menu, pt, pal_);
    if (!ch) return;
    commitFields();
    Id other;
    if (ch == 1) {
        other = lib_->createEntry(wb_.world, wb_.book, "character", "New character");
        if (other.empty()) return;
    } else {
        other = people[ch - 10]->id;
    }
    bool ok = false;
    if (relation == 1) ok = lib_->addLink(who, other, "Partner of");
    else { const KinLink kl = kinLink(who, other, relation, how); ok = lib_->addLink(kl.from, kl.to, kl.label); }
    if (!ok) { flash(w("Couldn't add them: " + lib_->lastError()), 6); return; }
    if (ch == 1) {
        const bool inTree = !wb_.familyRoot.empty();
        openEntry(other);
        focusFieldSoon_ = FName;
        flash(inTree ? L"Name them; “See the family tree” on their page goes back to the tree." : L"Name them here.", 6);
    }
    invalidate();
}

// A family member's chip: open their page, change what kind of parent, brother or sister, or child they are, or take
// them out of the family (0.46). Changing the kind replaces the links between the two.
void StudioApp::familyChipMenu(const Id& who, const Id& other, int role) {
    const Entry* o = lib_->entry(other);
    if (!o) return;
    const auto fam = lib_->familyOf(who);
    std::string cur;
    if (auto it = fam.how.find(other); it != fam.how.end()) cur = it->second;
    std::vector<cjui::MenuItem> menu{cjui::MenuItem::item(L"Open " + w(o->name) + L"'s page", 1)};
    if (role != 1) {
        menu.push_back(cjui::MenuItem::sep());
        const auto& ks = kinKinds(role);
        for (size_t i = 0; i < ks.size(); ++i) menu.push_back(cjui::MenuItem::item(ks[i].menu, (UINT)(10 + i), {}, true, cur == ks[i].how));
    }
    menu.push_back(cjui::MenuItem::sep());
    menu.push_back(cjui::MenuItem::item(L"Not family after all (remove the link)", 2));
    POINT pt;
    GetCursorPos(&pt);
    const UINT ch = cjui::showMenu(hwnd(), menu, pt, pal_);
    if (!ch) return;
    if (ch == 1) { openEntry(other); return; }
    commitFields();
    // Every family link between the two goes (either way round); then the chosen one is made.
    static const std::set<std::string> kin{"Parent of", "Child of", "Sibling of", "Twin of", "Half-sibling of", "Step-sibling of",
                                           "Adoptive sibling of", "Foster sibling of", "Adoptive parent of", "Adopted child of",
                                           "Step-parent of", "Stepchild of", "Foster parent of", "Foster child of", "Partner of", "Married to"};
    const bool partnerRole = role == 1;
    for (const EntryLink& l : lib_->linksOf(who)) {
        const Id x = l.from == who ? l.to : l.from;
        if (x != other || !kin.count(l.label)) continue;
        const bool partnerLink = l.label == "Partner of" || l.label == "Married to";
        if (partnerLink != partnerRole) continue;
        lib_->removeLink(l.from, l.to, l.label);
    }
    if (ch >= 10) {
        const KinLink kl = kinLink(who, other, role, kinKinds(role)[ch - 10].how);
        if (!lib_->addLink(kl.from, kl.to, kl.label)) flash(w("Couldn't change it: " + lib_->lastError()), 6);
    }
    invalidate();
}

// A place's (or realm's) page: the places inside it, who lives there or was born there, the groups based there and
// what happened there, from the bible's links (0.48). Each is a chip that opens the entry; "+ Place inside" makes one.
float StudioApp::drawPlaceSection(const Rect& m, float y, const Entry& e) {
    struct Row { const wchar_t* label; std::vector<const Entry*> who; };
    Row rows[] = {{L"Places inside", {}}, {L"Who lives here", {}}, {L"Born here", {}}, {L"Based here", {}}, {L"What happened here", {}}};
    std::set<std::pair<int, Id>> seen;
    for (const EntryLink& l : lib_->linksOf(e.id)) {
        const bool out = l.from == e.id;
        const Entry* o = lib_->entry(out ? l.to : l.from);
        if (!o || o->deleted || o->id == e.id) continue;
        int row = -1;
        if ((!out && (l.label == "Located in" || l.label == "Part of")) || (out && (l.label == "Contains" || l.label == "Includes"))) row = 0;
        else if ((!out && l.label == "Lives in") || (out && l.label == "Home of")) row = 1;
        else if ((!out && l.label == "Born in") || (out && l.label == "Birthplace of")) row = 2;
        else if ((!out && l.label == "Based in") || (out && l.label == "Base of")) row = 3;
        else if ((!out && l.label == "Took place in") || (out && l.label == "Scene of")) row = 4;
        if (row < 0 || !seen.insert({row, o->id}).second) continue;
        rows[row].who.push_back(o);
    }
    for (auto& r : rows) std::sort(r.who.begin(), r.who.end(), [](const Entry* a, const Entry* b) { return a->name < b->name; });
    gfx_.text(e.kind == "realm" ? L"IN THIS REALM" : L"IN THIS PLACE", {m.x, y, m.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 28;
    int ri = 0;
    for (const Row& row : rows) {
        const int rr = ri++;
        if (row.who.empty() && rr != 0) continue;
        gfx_.text(row.label, {m.x, y, 160, 28}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        float lx = m.x + 170;
        for (const Entry* o : row.who) {
            const std::wstring nm = w(o->name);
            const float lw2 = gfx_.measureWidth(nm, Font::Small) + 26;
            if (lx + lw2 > m.right() && lx > m.x + 170) { lx = m.x + 170; y += 34; }
            const Rect chip{lx, y, lw2, 28};
            const std::string id = "wb:plc:" + std::to_string(rr) + o->id;
            addHit(id, chip, [this, oid = o->id] { openEntry(oid); });
            const float hv = hoverAmount(id);
            const cjui::Color kc = kindColor(o->kind, pal_.dark);
            gfx_.fillRounded(chip, 14, pal_.surface.mix(pal_.sunken, 0.4f * hv));
            gfx_.strokeRounded(chip, 14, kc.withAlpha(0.5f + 0.5f * hv));
            gfx_.text(nm, chip, Font::Small, pal_.ink, {Align::Center, false, true, true});
            if (focused(id)) focusRing(chip);
            lx += lw2 + 8;
        }
        if (rr == 0) {
            const std::wstring add = L"+ Place inside";
            if (lx + gfx_.measureWidth(add, Font::Small) > m.right() && lx > m.x + 170) { lx = m.x + 170; y += 34; }
            linkText("wb:plcadd", lx + 4, y + 6, add, [this, id = e.id] {
                commitFields();
                const Id n = lib_->createEntry(wb_.world, wb_.book, "place", "New place");
                if (n.empty()) return;
                lib_->addLink(n, id, "Located in");
                openEntry(n);
                focusFieldSoon_ = FName;
                flash(L"Name it; it's inside the place you came from.", 5);
            });
        }
        y += 34;
    }
    return y + 22;
}

// A character's close family on their page: who, and quick ways to add more.
float StudioApp::drawFamilySection(const Rect& m, float y, const Entry& e) {
    const float cw = m.w;
    const auto fam = lib_->familyOf(e.id);
    gfx_.text(L"FAMILY", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    const bool any = !fam.parents.empty() || !fam.partners.empty() || !fam.siblings.empty() || !fam.children.empty();
    if (any) {
        const std::wstring t = L"See the family tree \u203A";
        linkText("wb:famtree", m.right() - gfx_.measureWidth(t, Font::Small), y - 2, t, [this, id = e.id] { openFamilyTree(id); });
    }
    y += 28;
    struct Row { const wchar_t* label; const std::vector<Id>* ids; int rel; const wchar_t* add; };
    const Row rows[] = {{L"Parents", &fam.parents, 0, L"+ Parent"}, {L"Partners", &fam.partners, 1, L"+ Partner"},
                        {L"Brothers and sisters", &fam.siblings, 2, L"+ Brother or sister"}, {L"Children", &fam.children, 3, L"+ Child"}};
    for (const Row& row : rows) {
        gfx_.text(row.label, {m.x, y, 160, 28}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        float lx = m.x + 170;
        for (const Id& pid : *row.ids) {
            const Entry* o = lib_->entry(pid);
            if (!o) continue;
            // The name, and what they are when it isn't plain (twin sister, stepfather, adopted son).
            std::string how;
            if (auto it = fam.how.find(pid); it != fam.how.end() && row.rel != 1) how = it->second;
            const std::wstring nm = w(o->name), kw = row.rel == 1 ? L"" : kinWord(pid, row.rel, how);
            const float nw = gfx_.measureWidth(nm, Font::Small), kww = kw.empty() ? 0 : gfx_.measureWidth(kw, Font::Small) + 12;
            const float lw2 = nw + kww + 26;
            if (lx + lw2 > m.right() && lx > m.x + 170) { lx = m.x + 170; y += 34; }
            const Rect chip{lx, y, lw2, 28};
            const std::string id = "wb:famp:" + std::to_string(row.rel) + pid;
            addHit(id, chip, [this, who = e.id, pid, rel = row.rel] { familyChipMenu(who, pid, rel); });
            tooltip(id, L"Open their page, change what kind of family they are, or remove them");
            const float hv = hoverAmount(id);
            const std::wstring g = w(facetOf(*o, *findFacet("gender")));
            const cjui::Color gc = g.empty() ? pal_.rule : genderColor(g, pal_);
            gfx_.fillRounded(chip, 14, pal_.surface.mix(pal_.sunken, 0.4f * hv));
            gfx_.strokeRounded(chip, 14, gc.mix(pal_.ribbon, hv));
            gfx_.text(nm, {chip.x + 13, chip.y, nw + 2, chip.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
            if (!kw.empty())
                gfx_.text(kw, {chip.x + 13 + nw + 10, chip.y, kww, chip.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
            if (focused(id)) focusRing(chip);
            lx += lw2 + 8;
        }
        const float aw = gfx_.measureWidth(row.add, Font::Small);
        if (lx + aw > m.right() && lx > m.x + 170) { lx = m.x + 170; y += 34; }
        linkText("wb:famadd" + std::to_string(row.rel), lx + 4, y + 6, row.add, [this, id = e.id, rel = row.rel] { addFamilyMember(id, rel); });
        y += 34;
    }
    return y + 22;
}

// A family tree: generations top to bottom, couples side by side, lines to children. One person's tree (their lines
// up, brothers and sisters, partners and descendants) or a family from its founder; people who married in are marked,
// with the way to their own family's tree. Click someone to pick them (the panel shows them, their lines light up);
// double-click to see the tree from them. Zoom and moving about glide.
void StudioApp::drawFamilyTree(const Rect& r) {
    const Entry* root = lib_->entry(wb_.familyRoot);
    if (!root || root->deleted) { wb_.familyRoot.clear(); invalidate(); return; }
    if (wb_.treeSelected.empty() || !lib_->entry(wb_.treeSelected)) wb_.treeSelected = root->id;
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    float y = r.y + 26 - wb_.scroll;
    linkText("wb:famback", r.x + pad, y, L"‹ " + w(root->name), [this, id = root->id] { openEntry(id); });
    y += 34;
    const auto lay = lib_->familyLayout(root->id, 150, wb_.treeBloodline);
    const int gens = lay.maxGeneration - lay.minGeneration + 1;
    const std::wstring title = w(studio::familytree::title(*lib_, root->id, wb_.treeBloodline));
    std::wstring sub = std::to_wstring(lay.people.size()) + (lay.people.size() == 1 ? L" person" : L" people") + L" across " +
                       std::to_wstring(gens) + (gens == 1 ? L" generation." : L" generations.");
    if (lay.cut) sub += L" Only the closest 150 are shown.";
    sub += wb_.treeBloodline ? L" From its founder down, with those who married or partnered into it." : L" Their lines up, brothers and sisters, partners and descendants.";
    y += pageHeader({r.x + pad, y, std::min(r.w - pad * 2, 900.0f), 0}, L"FAMILY TREE", title, sub);

    // Tools (0.45): who's picked, with adding family to them, on a line that wraps; then the other kind of tree.
    const Entry* sel = lib_->entry(wb_.treeSelected);
    {
        const float left = r.x + pad, rightEdge = r.right() - pad;
        float bx = left, bw = 0;
        const std::wstring who = sel ? w(sel->name) : w(root->name);
        const Id sid = sel ? sel->id : root->id;
        gfx_.text(L"PICKED", {bx, y - 6, 60, 28}, Font::Label, pal_.inkFaint, {Align::Left, false, false, true});
        bx += 58;
        {
            // The picked person as a chip that opens their page.
            const float nw = std::min(gfx_.measureWidth(who, Font::BodyStrong) + 30, 260.0f);
            const Rect chip{bx, y - 6, nw, 28};
            addHit("wb:famopen", chip, [this, sid] { openEntry(sid); });
            const float hv = hoverAmount("wb:famopen");
            gfx_.fillRounded(chip, 14, pal_.ribbon.withAlpha(0.12f + 0.08f * hv));
            gfx_.text(who, {chip.x + 12, chip.y, chip.w - 24, chip.h}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
            tooltip("wb:famopen", L"Open " + who + L"'s page");
            bx += nw + 12;
        }
        auto tool = [&](const std::string& id, const std::wstring& label, std::function<void()> fn) {
            const float tw = gfx_.measureWidth(label, Font::Small) + 22;
            if (bx + tw > rightEdge) { bx = left + 58; y += 34; }
            smallButton(id, bx, y - 6, label, std::move(fn), &bw);
            bx += bw + 6;
        };
        tool("wb:famaddp", L"+ Parent", [this, sid] { addFamilyMember(sid, 0); });
        tool("wb:famaddpt", L"+ Partner", [this, sid] { addFamilyMember(sid, 1); });
        tool("wb:famaddc", L"+ Child", [this, sid] { addFamilyMember(sid, 3); });
        tool("wb:famadds", L"+ Brother or sister", [this, sid] { addFamilyMember(sid, 2); });
        y += 40;
        // The other view: the whole family this person comes from, or this person's own tree; and saving it.
        float lx = left;
        if (wb_.treeBloodline) {
            if (sel && sel->id != root->id) {
                const std::wstring t = L"See " + w(sel->name) + L"'s own tree \u203A";
                linkText("wb:famown", lx, y - 4, t, [this, sid] { openFamilyTree(sid, false); });
                lx += gfx_.measureWidth(t, Font::Small) + 30;
            }
        } else {
            Id founder;
            for (const auto& fg : lib_->families(wb_.world, wb_.book)) {
                const auto t = lib_->familyLayout(fg.root, 400, true);
                if (std::any_of(t.people.begin(), t.people.end(), [&](const auto& p) { return p.id == root->id && !p.inLaw; })) { founder = fg.root; break; }
            }
            if (!founder.empty()) {
                linkText("wb:famwhole", lx, y - 4, L"See the whole family \u203A", [this, founder] { openFamilyTree(founder, true); });
                lx += gfx_.measureWidth(L"See the whole family \u203A", Font::Small) + 30;
            }
        }
        linkText("wb:famsave", lx, y - 4, L"Save as a picture\u2026", [this] { saveFamilyTreePicture(); });
        y += 34;
    }
    // The board the tree sits on, to the foot of the page.
    const Rect board{r.x + pad - 12, y - 8, r.w - pad * 2 + 24, std::max(260.0f, r.bottom() - 46 - (y - 8))};
    gfx_.fillRounded(board, 10, pal_.sunken.mix(pal_.paper, 0.45f));
    for (float gy = board.y + 18; gy < board.bottom() - 6; gy += 22)
        for (float gx = board.x + 18; gx < board.right() - 6; gx += 22) gfx_.fill({gx, gy, 1.4f, 1.4f}, pal_.rule.withAlpha(0.8f));
    y += 48;   // clear of the zoom buttons in the board's corner
    gfx_.pushClip(board);

    // Sizes: cards shrink to fit (down to two thirds) unless zoomed; zoom and moving across glide.
    const float baseW = 196, baseH = 80, baseGapX = 26, baseGapY = 64;
    const float avail = r.w - pad * 2;
    const float natural = lay.width * (baseW + baseGapX) - baseGapX;
    const float fitS = std::clamp(natural > 0 ? avail / natural : 1.0f, 0.66f, 1.0f);
    const float targetS = wb_.treeZoom > 0 ? wb_.treeZoom : fitS;
    const float s = anim("famtree:zoom", targetS, 14.0f);
    const float cardW = baseW * s, cardH = baseH * s, stepX = (baseW + baseGapX) * s, gapY = baseGapY * s;
    const float treeW = lay.width * stepX - baseGapX * s;
    wb_.treeW = treeW;
    std::map<Id, Rect> at;
    float x0 = treeW <= avail ? r.x + pad + (avail - treeW) / 2 : r.x + pad;
    if (treeW > avail) {
        if (wb_.treeX < 0) {   // first look: the person in the middle
            for (auto& p : lay.people)
                if (p.id == root->id) wb_.treeX = std::clamp(p.x * (baseW + baseGapX) * targetS + baseW * targetS / 2 - avail / 2, 0.0f, std::max(0.0f, lay.width * (baseW + baseGapX) * targetS - avail + 60));
            setAnim("famtree:x", wb_.treeX);
        }
        wb_.treeX = std::clamp(wb_.treeX, 0.0f, std::max(0.0f, lay.width * (baseW + baseGapX) * targetS - avail + 60));
        x0 -= wb_.treePan ? wb_.treeX : anim("famtree:x", wb_.treeX, 16.0f);
        if (wb_.treePan) setAnim("famtree:x", wb_.treeX);
    } else {
        wb_.treeX = 0;
        setAnim("famtree:x", 0);
    }
    for (auto& p : lay.people)
        at[p.id] = {x0 + p.x * stepX, y + (p.generation - lay.minGeneration) * (cardH + gapY), cardW, cardH};
    const cjui::Color lineC = pal_.inkFaint.withAlpha(0.8f);
    const cjui::Color litC = pal_.ribbon;
    const Id& lit = wb_.treeSelected;
    std::map<Id, bool> inLaw;
    for (auto& p : lay.people) inLaw[p.id] = p.inLaw;

    // Partners: a line between them at mid-height (two for a marriage).
    std::set<std::pair<Id, Id>> married;
    for (auto& [a, b] : lay.couples)
        for (const EntryLink& l : lib_->linksOf(a))
            if (l.label == "Married to" && (l.from == b || l.to == b)) married.insert({a, b});
    for (auto& [a, b] : lay.couples) {
        Rect ra = at[a], rb = at[b];
        if (std::fabs(ra.y - rb.y) > 1) continue;
        if (ra.x > rb.x) std::swap(ra, rb);
        const float cy = ra.y + ra.h / 2;
        const bool on = a == lit || b == lit;
        const cjui::Color c = on ? litC : lineC;
        const float t = on ? 2.2f : 1.4f;
        if (married.count({a, b})) {
            gfx_.line(ra.right(), cy - 2, rb.x, cy - 2, c, t);
            gfx_.line(ra.right(), cy + 2, rb.x, cy + 2, c, t);
        } else {
            gfx_.line(ra.right(), cy, rb.x, cy, c, t);
        }
    }
    // Children, grouped by their parents: down from the parents (from the middle of a couple's line), along, and
    // down to each child. The picked person's lines are lit.
    // A child hangs from their main parents: birth parents, else adoptive, else foster, else step-parents. Any other
    // parents (a step-parent beside a birth parent, say) get a dotted line of their own (0.46).
    std::map<std::vector<Id>, std::vector<Id>> byParents;
    std::vector<std::pair<Id, Id>> otherParents;   // parent, child
    auto howOf = [&](const Id& par, const Id& ch) {
        const auto it = lay.parentHow.find({par, ch});
        return it == lay.parentHow.end() ? std::string() : it->second;
    };
    auto rankHow = [](const std::string& h) { return h.empty() ? 0 : h == "adoptive" ? 1 : h == "foster" ? 2 : 3; };
    {
        std::map<Id, std::vector<Id>> parentsOf;
        for (auto& [par, ch] : lay.parentChild) parentsOf[ch].push_back(par);
        for (auto& [ch, ps] : parentsOf) {
            int best = 9;
            for (const Id& p : ps) best = std::min(best, rankHow(howOf(p, ch)));
            std::vector<Id> mainPs;
            for (const Id& p : ps) (rankHow(howOf(p, ch)) == best ? mainPs.push_back(p) : otherParents.push_back({p, ch}));
            std::sort(mainPs.begin(), mainPs.end());
            byParents[mainPs].push_back(ch);
        }
    }
    std::set<std::pair<Id, Id>> twinPairs;
    for (auto& sb : lay.siblings) if (sb.how == "twin") { twinPairs.insert({sb.a, sb.b}); twinPairs.insert({sb.b, sb.a}); }
    // Dotted lines (adopted, step, foster) and a dashed one (parents who aren't partners) share these.
    auto dotted = [&](float x1, float y1, float x2, float y2, cjui::Color c, float t) {
        const float len = std::hypot(x2 - x1, y2 - y1);
        if (len < 0.5f) return;
        const float step = 6 * s, dot = 2.4f * s;
        for (float d = 0; d < len; d += step) {
            const float e = std::min(len, d + dot);
            gfx_.line(x1 + (x2 - x1) * d / len, y1 + (y2 - y1) * d / len, x1 + (x2 - x1) * e / len, y1 + (y2 - y1) * e / len, c, t);
        }
    };
    auto kinTag = [&](const std::string& how) -> std::wstring {
        return how == "adoptive" ? L"adopted" : how == "step" ? L"step" : how == "foster" ? L"foster" : L"";
    };
    // Several sets of parents over one row of children: each set's line at its own height (0.46).
    std::map<std::vector<Id>, float> busShift;
    {
        std::map<long, std::vector<std::pair<float, std::vector<Id>>>> rows;
        for (auto& [ps, kids] : byParents) {
            float top = 1e9f, px = 0;
            for (const Id& k : kids) top = std::min(top, at[k].y);
            for (const Id& p : ps) px += at[p].x;
            rows[std::lround(top)].push_back({ps.empty() ? 0 : px / ps.size(), ps});
        }
        for (auto& [row, gs] : rows) {
            std::sort(gs.begin(), gs.end());
            const float step = std::min(9 * s, gapY * 0.55f / std::max<size_t>(1, gs.size()));
            for (size_t i = 0; i < gs.size(); ++i) busShift[gs[i].second] = ((float)i - (gs.size() - 1) / 2.0f) * step;
        }
    }
    for (auto& [ps, kids] : byParents) {
        float ox = 0, oy = 0;
        bool couple = ps.size() == 2;
        if (couple) {
            // Two parents in a row: joined at mid-height, by a dashed line when they aren't partners (0.45).
            Rect a = at[ps[0]], b = at[ps[1]];
            couple = std::fabs(a.y - b.y) < 1;
            if (couple) {
                if (a.x > b.x) std::swap(a, b);
                ox = (a.right() + b.x) / 2;
                oy = a.y + a.h / 2;
                const bool partners = std::any_of(lay.couples.begin(), lay.couples.end(), [&](auto& c) {
                    return (c.first == ps[0] && c.second == ps[1]) || (c.first == ps[1] && c.second == ps[0]);
                });
                if (!partners) {
                    const bool on = std::find(ps.begin(), ps.end(), lit) != ps.end();
                    for (float dx = a.right(); dx < b.x; dx += 10 * s)
                        gfx_.line(dx, oy, std::min(dx + 5 * s, b.x), oy, on ? litC : lineC, on ? 2.2f : 1.4f);
                }
            }
        }
        if (!couple) {
            float sx = 0, by = 0;
            for (const Id& p : ps) { sx += at[p].x + at[p].w / 2; by = std::max(by, at[p].bottom()); }
            ox = sx / ps.size();
            oy = by;
        }
        float minX = ox, maxX = ox, topY = 1e9f;
        for (const Id& k : kids) {   // the line runs to each child, or to the point a pair of twins comes down from
            const Rect kr = at[k];
            float kx = kr.x + kr.w / 2;
            for (const Id& o : kids) if (o != k && twinPairs.count({k, o})) kx = (kx + at[o].x + at[o].w / 2) / 2;
            minX = std::min(minX, kx);
            maxX = std::max(maxX, kx);
            topY = std::min(topY, kr.y);
        }
        if (topY > 1e8f) continue;
        const bool parentLit = std::find(ps.begin(), ps.end(), lit) != ps.end();
        const float busY = topY - gapY / 2 + busShift[ps];
        gfx_.line(ox, oy, ox, busY, parentLit ? litC : lineC, parentLit ? 2.2f : 1.4f);
        gfx_.line(minX, busY, maxX, busY, parentLit ? litC : lineC, parentLit ? 2.2f : 1.4f);
        for (const Id& k : kids) {
            const Rect kr = at[k];
            const bool on = parentLit || k == lit;
            const float kx = kr.x + kr.w / 2;
            // Twins come down from one point on the line, between them.
            float fromX = kx;
            for (const Id& o : kids)
                if (o != k && twinPairs.count({k, o})) fromX = (kx + at[o].x + at[o].w / 2) / 2;
            if (k == lit && !parentLit) {   // a child picked: the way up to their parents lit
                gfx_.line(ox, oy, ox, busY, litC, 2.2f);
                gfx_.line(std::min(ox, fromX), busY, std::max(ox, fromX), busY, litC, 2.2f);
            }
            const std::string how = ps.empty() ? std::string() : howOf(ps[0], k);
            const cjui::Color c = on ? litC : lineC;
            const float t = on ? 2.2f : 1.4f;
            if (fromX != kx) {
                const float midY = busY + (kr.y - busY) * 0.45f;
                (how.empty() ? gfx_.line(fromX, busY, kx, midY, c, t) : dotted(fromX, busY, kx, midY, c, t));
                (how.empty() ? gfx_.line(kx, midY, kx, kr.y, c, t) : dotted(kx, midY, kx, kr.y, c, t));
            } else if (how.empty()) {
                gfx_.line(kx, busY, kx, kr.y, c, t);
            } else {
                dotted(kx, busY, kx, kr.y, c, t);
            }
            if (const std::wstring tag = kinTag(how); !tag.empty() && s > 0.7f)
                gfx_.text(tag, {kx + 5, busY + 2, 70, 16}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
        }
    }
    // Other parents (a step-parent beside a birth parent, say): a dotted line from them to the child, labelled.
    for (auto& [par, ch] : otherParents) {
        if (!at.count(par) || !at.count(ch)) continue;
        const Rect pr = at[par], kr = at[ch];
        const bool on = par == lit || ch == lit;
        const cjui::Color c = on ? litC : lineC;
        const float t = on ? 2.2f : 1.4f;
        const float px = pr.x + pr.w / 2 + 14 * s, kx = kr.x + kr.w / 2 + 14 * s;
        const float midY = kr.y - gapY * 0.28f;
        dotted(px, pr.bottom(), px, midY, c, t);
        dotted(px, midY, kx, midY, c, t);
        dotted(kx, midY, kx, kr.y, c, t);
        if (const std::wstring tag = kinTag(howOf(par, ch)); !tag.empty() && s > 0.7f)
            gfx_.text(tag, {kx + 5, midY + 2, 70, 16}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
    }
    // Brothers and sisters joined only by their own link (no parent drawn), or of a kind: a bracket over them.
    for (auto& sb : lay.siblings) {
        if (sb.sharedParents || !at.count(sb.a) || !at.count(sb.b)) continue;
        Rect ra = at[sb.a], rb = at[sb.b];
        if (ra.x > rb.x) std::swap(ra, rb);
        const bool on = sb.a == lit || sb.b == lit;
        const cjui::Color c = on ? litC : lineC;
        const float t = on ? 2.2f : 1.4f;
        const float ax = ra.x + ra.w / 2, bx = rb.x + rb.w / 2, topY = ra.y - gapY * 0.3f;
        const bool plain = sb.how.empty() || sb.how == "twin" || sb.how == "half";
        if (plain) {
            gfx_.line(ax, ra.y, ax, topY, c, t); gfx_.line(ax, topY, bx, topY, c, t); gfx_.line(bx, topY, bx, rb.y, c, t);
        } else {
            dotted(ax, ra.y, ax, topY, c, t); dotted(ax, topY, bx, topY, c, t); dotted(bx, topY, bx, rb.y, c, t);
        }
        const std::wstring tag = sb.how == "twin" ? L"twins" : sb.how == "half" ? L"half-siblings" : sb.how == "step" ? L"step-siblings"
                               : sb.how == "adoptive" ? L"adoptive siblings" : sb.how == "foster" ? L"foster siblings" : L"siblings";
        if (s > 0.7f) gfx_.text(tag, {(ax + bx) / 2 - 60, topY - 17, 120, 16}, Font::Small, pal_.inkFaint, {Align::Center, false, true});
    }
    // The people.
    float bottom = y;
    for (auto& p : lay.people) {
        const Entry* x = lib_->entry(p.id);
        if (!x) continue;
        const Rect cr = at[p.id];
        bottom = std::max(bottom, cr.bottom());
        if (cr.right() < r.x || cr.x > r.right()) continue;
        const std::string id = "wb:famcard:" + p.id;
        addHit(id, cr, [this, pid = p.id] {
            const DWORD now = GetTickCount();
            const bool twice = wb_.treeLastClick == pid && now - wb_.treeLastClickAt < 450;
            wb_.treeLastClick = pid;
            wb_.treeLastClickAt = now;
            if (twice && pid != wb_.familyRoot) { openFamilyTree(pid, false); return; }   // the tree from them
            commitFields();
            wb_.treeSelected = pid;
            setFocus({Focus::Entry, pid, {}});
            invalidate();
        });
        tooltip(id, L"Click to pick; double-click to see the tree from " + w(x->name) + L".");
        const float hv = hoverAmount(id);
        const bool picked = p.id == lit;
        const std::wstring g = w(facetOf(*x, *findFacet("gender")));
        const cjui::Color gc = g.empty() ? kindColor("character", pal_.dark) : genderColor(g, pal_);
        softShadow(cr, 6, 0.25f + 0.5f * hv + (picked ? 0.4f : 0));
        gfx_.fillRounded(cr, 6, p.inLaw ? pal_.surface.mix(pal_.paper, 0.5f) : pal_.surface);
        gfx_.strokeRounded(cr, 6, picked ? pal_.ribbon : pal_.rule.mix(gc, 0.5f + 0.5f * hv), picked ? 2.4f : 1.0f);
        gfx_.fill({cr.x + 1, cr.y + 8 * s, 3, cr.h - 16 * s}, p.inLaw ? gc.withAlpha(0.45f) : gc);
        const float ps = cr.h - 18 * s;
        const Rect pic{cr.x + 12 * s, cr.y + 9 * s, ps, ps};
        if (x->imageBlob.empty() || !gfx_.drawImage(w(lib_->blobPath(x->imageBlob)), pic, true)) {
            gfx_.fillRounded(pic, ps / 2, gc.withAlpha(pal_.dark ? 0.22f : 0.16f));
            gfx_.text(initials(x->name), pic, Font::SmallStrong, gc.mix(pal_.ink, 0.35f), {Align::Center, false, false, true});
        }
        const float tx = pic.right() + 10 * s, tw = cr.right() - 8 - tx;
        // The name on up to two lines.
        {
            const Font nf = s < 0.8f ? Font::SmallStrong : Font::BodyStrong;
            gfx_.pushClip({tx, cr.y + 4, tw, cr.h - 26 * s});
            gfx_.text(linesOf(gfx_, w(x->name), tw, nf, 2), {tx, cr.y + 9 * s, tw, cr.h}, nf, pal_.ink, {Align::Left, true, false});
            gfx_.popClip();
        }
        if (p.inLaw) {
            // Married in: their own family's tree is a click away (when they have one).
            const auto theirs = lib_->familyOf(p.id);
            const bool has = !theirs.parents.empty() || !theirs.siblings.empty();
            const std::wstring how = p.joined == "married" ? L"Married in" : p.joined == "partner" ? L"Partner" : L"Other parent";
            const std::wstring lbl = has ? how + L" · their family ›" : how;
            const Rect lr{tx, cr.y + cr.h - 26 * s, tw, 18};
            if (has) {
                const std::string lid = "wb:faminlaw:" + p.id;
                addHit(lid, lr, [this, pid = p.id] { openFamilyTree(pid, false); });
                gfx_.text(lbl, lr, Font::Small, pal_.ribbon.mix(pal_.ink, 0.25f * hoverAmount(lid)), {Align::Left, false, true});
            } else {
                gfx_.text(lbl, lr, Font::Small, pal_.inkFaint, {Align::Left, false, true});
            }
        } else {
            // A drawn gender sign, then a detail: age, when born, or a title.
            float lx = tx;
            if (!genderSign(g).empty()) { drawGenderSign(gfx_, lx + 6, cr.y + cr.h - 18 * s, 8, g, gc); lx += 18; }
            std::string d2;
            for (const char* lab : {"Age", "Born", "Birthday", "Title", "Occupation"}) {
                for (size_t at2 = 0; at2 < x->details.size() && d2.empty();) {
                    size_t nl = x->details.find('\n', at2);
                    if (nl == std::string::npos) nl = x->details.size();
                    const std::string line = trimmedCopy(x->details.substr(at2, nl - at2));
                    at2 = nl + 1;
                    const size_t colon = line.find(':');
                    if (colon != std::string::npos && trimmedCopy(line.substr(0, colon)) == lab) {
                        const std::string v = trimmedCopy(line.substr(colon + 1));
                        if (!v.empty()) d2 = (std::string(lab) == "Age" || std::string(lab) == "Born") ? std::string(lab) + " " + v : v;
                    }
                }
                if (!d2.empty()) break;
            }
            if (d2.empty() && !g.empty() && genderSign(g).empty()) d2 = narrow(g);
            if (!d2.empty()) gfx_.text(w(d2), {lx, cr.y + cr.h - 27 * s, cr.right() - 8 - lx, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        }
        if (focused(id)) focusRing(cr);
    }
    wb_.content = bottom + wb_.scroll - r.y + 60;
    wb_.scroll = std::clamp(wb_.scroll, 0.0f, std::max(0.0f, wb_.content - r.h));
    gfx_.popClip();
    {
        // The key to the lines, and zoom floating in the board's corner (0.45).
        const float ky = r.bottom() - 30;
        float kx = r.x + pad;
        auto key = [&](int kind, const std::wstring& label) {
            const float cy = ky + 10;
            if (kind == 0) gfx_.line(kx, cy, kx + 22, cy, lineC, 1.4f);
            if (kind == 1) { gfx_.line(kx, cy - 2, kx + 22, cy - 2, lineC, 1.4f); gfx_.line(kx, cy + 2, kx + 22, cy + 2, lineC, 1.4f); }
            if (kind == 2) for (float dx = 0; dx < 22; dx += 8) gfx_.line(kx + dx, cy, kx + dx + 4, cy, lineC, 1.4f);
            if (kind == 3) for (float dx = 0; dx < 22; dx += 5) gfx_.line(kx + dx, cy, kx + dx + 2, cy, lineC, 1.4f);
            kx += 28;
            gfx_.text(label, {kx, ky, 200, 20}, Font::Small, pal_.inkFaint, {Align::Left, false, false, true});
            kx += gfx_.measureWidth(label, Font::Small) + 18;
        };
        key(0, L"partners");
        key(1, L"married");
        key(2, L"parents, not partners");
        key(3, L"adopted, step or foster");
        gfx_.text(L"Click to pick \u00B7 double-click: their tree \u00B7 drag to move", {kx + 10, ky, r.right() - pad - kx - 10, 20}, Font::Small,
                  pal_.inkFaint, {Align::Right, false, true, true});
        const float zy = board.y + 10, zx = board.right() - 10 - 3 * 34;
        const Rect zr{zx - 4, zy - 4, 3 * 34 + 8, 36};
        gfx_.fillRounded(zr, 8, pal_.surface);
        gfx_.strokeRounded(zr, 8, pal_.rule);
        auto zbtn = [&](int i, const std::string& id, const std::wstring& lbl, std::function<void()> fn, const std::wstring& tip) {
            const Rect br{zx + i * 34.0f, zy, 30, 28};
            addHit(id, br, std::move(fn));
            const float hv = hoverAmount(id);
            if (hv > 0.01f) gfx_.fillRounded(br, 6, pal_.sunken.withAlpha(hv));
            gfx_.text(lbl, br, i == 1 ? Font::Small : Font::BodyStrong, pal_.ink, {Align::Center, false, false, true});
            tooltip(id, tip);
        };
        zbtn(0, "wb:famzout", L"\u2212", [this] { zoomFamilyTree(1 / 1.25f); }, L"Zoom out (\u2212)");
        zbtn(1, "wb:famzfit", L"Fit", [this] { zoomFamilyTree(0); }, L"Fit the tree (0)");
        zbtn(2, "wb:famzin", L"+", [this] { zoomFamilyTree(1.25f); }, L"Zoom in (+)");
    }
    clearHitClip();
    gfx_.popClip();
}

// ------------------------------------------------------------- maps

void StudioApp::chooseMapPicture(const Id& entryId) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Pictures", L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetTitle(L"Choose a map");
    std::wstring path;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    if (path.empty()) return;
    std::ifstream f(path.c_str(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > 120u * 1024 * 1024) { flash(L"That map couldn't be read (or is over 120 MB).", 6); return; }
    std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos) name.resize(dot);
    const Id aid = lib_->addArtwork(bytes, narrow(name), "Map");
    const Artwork* a = aid.empty() ? nullptr : lib_->artwork(aid);
    if (!a) { flash(w("Couldn't keep the map: " + lib_->lastError()), 6); return; }
    commitFields();
    if (const Entry* e = lib_->entry(entryId)) {
        Entry u = *e;
        u.mapBlob = a->blob;   // pins stay where they were (across the new picture)
        lib_->updateEntry(u);
    }
    invalidate();
}

// Choose an entry to pin on a map: places inside it first, then everything not pinned yet; the next click on the map
// says where (0.48: shared by the page's map and the map viewer).
void StudioApp::pinOnMenu(const Id& id) {
    const Entry* x = lib_->entry(id);
    if (!x) return;
    // Places inside this one first, then everything else not pinned yet.
    std::vector<const Entry*> inside, rest;
    std::set<Id> pinned;
    for (auto& p : x->pins) pinned.insert(p.entry);
    for (const Entry* o : lib_->entriesOf(wb_.world, wb_.book)) {
        if (o->id == id || pinned.count(o->id)) continue;
        bool in = false;
        for (const EntryLink& l : lib_->linksOf(o->id))
            if ((l.from == o->id && l.to == id && (l.label == "Located in" || l.label == "Part of")) ||
                (l.to == o->id && l.from == id && (l.label == "Contains" || l.label == "Includes"))) in = true;
        (in ? inside : rest).push_back(o);
    }
    std::stable_sort(rest.begin(), rest.end(), [](const Entry* a, const Entry* b) { return (a->kind == "place") > (b->kind == "place"); });
    std::vector<cjui::MenuItem> menu;
    std::vector<const Entry*> all;
    for (const Entry* o : inside) { menu.push_back(cjui::MenuItem::item(w(o->name) + L"  (in " + w(x->name) + L")", (UINT)(1 + all.size()))); all.push_back(o); }
    if (!inside.empty() && !rest.empty()) menu.push_back(cjui::MenuItem::sep());
    for (const Entry* o : rest) {
        if (all.size() >= 300) break;
        menu.push_back(cjui::MenuItem::item(w(o->name) + L"  (" + w(entryKindLabel(o->kind)) + L")", (UINT)(1 + all.size())));
        all.push_back(o);
    }
    if (all.empty()) { flash(L"Everything in the bible is pinned already.", 5); return; }
    POINT pt;
    GetCursorPos(&pt);
    const UINT ch = cjui::showMenu(hwnd(), menu, pt, pal_);
    if (!ch) return;
    wb_.pinning = all[ch - 1]->id;
    invalidate();
}

// A pin clicked: open its entry (or its own map: maps inside maps, 0.48), move it, or take it off the map.
void StudioApp::pinMenu(const Id& mapEntry, const Id& pinned) {
    const Entry* p = lib_->entry(pinned);
    if (!p) return;
    const bool hasMap = !p->mapBlob.empty();
    std::vector<std::wstring> items{L"Open " + w(p->name)};
    if (hasMap) items.push_back(L"Open the map of " + w(p->name));
    items.push_back(L"Move the pin");
    items.push_back(L"Take it off the map");
    int i = popupAt(hwnd(), items);
    if (i < 0) return;
    if (!hasMap && i >= 1) ++i;   // the same choices whether or not "Open the map" is there
    if (i == 0) openEntry(pinned);
    else if (i == 1) openMap(pinned);
    else if (i == 2) { wb_.pinning = pinned; invalidate(); }
    else if (i == 3) {
        if (const Entry* e = lib_->entry(mapEntry)) {
            Entry u = *e;
            u.pins.erase(std::remove_if(u.pins.begin(), u.pins.end(), [&](const Entry::Pin& q) { return q.entry == pinned; }), u.pins.end());
            lib_->updateEntry(u);
        }
        invalidate();
    }
}

// The map on a place's or realm's page: the picture, its pins (click one for its menu), and pinning:
// choose an entry, then click on the map where it is.
float StudioApp::drawMapSection(const Rect& m, float y, const Entry& e) {
    const float cw = m.w;
    gfx_.text(L"MAP" + (e.pins.empty() ? std::wstring() : L"  ·  " + std::to_wstring(e.pins.size()) + (e.pins.size() == 1 ? L" pin" : L" pins")),
              {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    if (e.mapBlob.empty()) {
        const std::wstring t = L"+ Choose a map…";
        linkText("wb:mapchoose", m.right() - gfx_.measureWidth(t, Font::Small), y - 2, t, [this, id = e.id] { chooseMapPicture(id); });
        y += 30;
        y += gfx_.text(L"A map of " + w(e.name) + L": a picture you've drawn or made. Pin the places, people and events on it.",
                       {m.x, y, std::min(cw, 640.0f), 40}, Font::Small, pal_.inkFaint) + 8;
        if (!e.imageBlob.empty()) {
            linkText("wb:mapmain", m.x, y, L"Use its main picture as the map", [this, id = e.id] {
                if (const Entry* x = lib_->entry(id)) { Entry u = *x; u.mapBlob = x->imageBlob; lib_->updateEntry(u); invalidate(); }
            });
            y += 28;
        }
        return y + 18;
    }
    {
        float lx = m.right();
        auto act = [&](const std::string& id, const std::wstring& t, std::function<void()> f) {
            lx -= gfx_.measureWidth(t, Font::Small);
            linkText(id, lx, y - 2, t, std::move(f));
            lx -= 22;
        };
        act("wb:mapremove", L"Remove the map", [this, id = e.id] {
            if (cjui::messageBox(hwnd(), L"Remove the map and its pins? The picture stays in Artwork.", L"Remove the map",
                                 MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
            if (const Entry* x = lib_->entry(id)) { Entry u = *x; u.mapBlob.clear(); u.pins.clear(); lib_->updateEntry(u); }
            wb_.pinning.clear();
            invalidate();
        });
        act("wb:mapreplace", L"Replace…", [this, id = e.id] { chooseMapPicture(id); });
        act("wb:mappin", L"+ Pin an entry…", [this, id = e.id] { pinOnMenu(id); });
        act("wb:mapopen", L"Open the map \u203A", [this, id = e.id] { openMap(id); });
    }
    y += 30;
    const std::wstring path = w(lib_->blobPath(e.mapBlob));
    const auto [iw, ih] = gfx_.imageSize(path);
    float mw = cw, mh = iw > 0 ? cw * ih / (float)iw : cw * 0.6f;
    if (mh > 720) { mh = 720; mw = iw > 0 ? mh * iw / (float)ih : cw; }
    const Rect mr{m.x + (cw - mw) / 2, y, mw, mh};
    // Pinning: a note above the map, and the map takes the click.
    if (!wb_.pinning.empty()) {
        const Entry* pe = lib_->entry(wb_.pinning);
        const Rect band{m.x, y, cw, 34};
        gfx_.fillRounded(band, 6, pal_.ribbon.withAlpha(0.14f));
        gfx_.text(L"Click on the map where " + (pe ? w(pe->name) : std::wstring(L"it")) + L" is.", {band.x + 12, band.y, band.w - 100, band.h}, Font::Body,
                  pal_.ink, {Align::Left, false, true, true});
        linkText("wb:pinstop", band.right() - 60, band.y + 9, L"Stop", [this] { wb_.pinning.clear(); invalidate(); });
        y += 44;
    }
    const Rect map{mr.x, y, mr.w, mr.h};
    if (!wb_.pinning.empty())
        addHit("wb:mapclick", map, [this, id = e.id, map] {
            const float fx = std::clamp((mouseX() - map.x) / map.w, 0.0f, 1.0f), fy = std::clamp((mouseY() - map.y) / map.h, 0.0f, 1.0f);
            if (const Entry* x = lib_->entry(id)) {
                Entry u = *x;
                bool moved = false;
                for (auto& p : u.pins) if (p.entry == wb_.pinning) { p.x = fx; p.y = fy; moved = true; }
                if (!moved) u.pins.push_back({wb_.pinning, fx, fy});
                lib_->updateEntry(u);
            }
            wb_.pinning.clear();
            invalidate();
        }, false);
    if (wb_.pinning.empty()) {
        addHit("wb:mapbig", map, [this, id = e.id] { openMap(id); }, false);   // the map at full size (0.48)
        tooltip("wb:mapbig", L"Open the map to zoom in, move about and drag pins");
    }
    softShadow(map, 4, 0.35f);
    if (!gfx_.drawImage(path, map, false)) {
        gfx_.fillRounded(map, 4, pal_.sunken);
        gfx_.text(L"The map's picture is missing.", map, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
    }
    if (!wb_.pinning.empty()) gfx_.strokeRounded(map, 4, pal_.ribbon, 2);
    // Pins: a dot in the entry's colour, and its name on a small label beside it.
    for (const auto& p : e.pins) {
        const Entry* o = lib_->entry(p.entry);
        if (!o || o->deleted) continue;
        const float px = map.x + (float)p.x * map.w, py = map.y + (float)p.y * map.h;
        const cjui::Color kc = kindColor(o->kind, pal_.dark);
        const std::wstring nm = w(o->name);
        const float lw = gfx_.measureWidth(nm, Font::Small) + 16;
        const bool left = px + 10 + lw > map.right();
        const Rect label{left ? px - 10 - lw : px + 10, py - 11, lw, 22};
        const std::string id = "wb:pin:" + p.entry;
        if (wb_.pinning.empty()) {
            addHit(id, {px - 9, py - 9, 18, 18}, [this, mid = e.id, pid = p.entry] { pinMenu(mid, pid); });
            addHit(id + ":l", label, [this, mid = e.id, pid = p.entry] { pinMenu(mid, pid); });
        }
        const float hv = std::max(hoverAmount(id), hoverAmount(id + ":l"));
        gfx_.fillRounded(label, 11, pal_.paper.withAlpha(0.88f + 0.12f * hv));
        gfx_.strokeRounded(label, 11, kc.withAlpha(0.6f + 0.4f * hv));
        gfx_.text(nm, label, Font::Small, pal_.ink, {Align::Center, false, true, true});
        gfx_.circle(px, py, 7 + 2 * hv, pal_.paper, true);
        gfx_.circle(px, py, 5 + 2 * hv, kc, true);
    }
    y += map.h + 8;
    y += gfx_.text(L"Click a pin to open it, move it or take it off; click the map to open it full size.", {m.x, y, cw, 20}, Font::Small, pal_.inkFaint) + 22;
    return y;
}

// ------------------------------------------------------------- the map viewer (0.48)

void StudioApp::openMap(const Id& entryId) {
    const Entry* e = lib_ ? lib_->entry(entryId) : nullptr;
    if (!e || e->mapBlob.empty()) return;
    commitFields();
    if (wb_.entry != entryId) openEntry(entryId);
    wb_.mapView = entryId;
    wb_.mapZoom = 1;
    wb_.mapCX = wb_.mapCY = 0.5f;
    wb_.pinning.clear();
    wb_.scroll = 0;
    setFocus({Focus::Entry, entryId, {}});
    invalidate();
}

// A map over the whole page: the wheel zooms about the pointer, dragging moves it, pins are dragged to move them and
// clicked for their menu (open, open their own map, take off). Labels give way to each other, and show on hover.
void StudioApp::drawMapView(const Rect& r) {
    const Entry* e = lib_->entry(wb_.mapView);
    if (!e || e->deleted || e->mapBlob.empty()) { wb_.mapView.clear(); invalidate(); return; }
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    float y = r.y + 22;
    linkText("wb:mapback", r.x + pad, y, L"‹ " + w(e->name), [this, id = e->id] { openEntry(id); });
    // Up to the map this place is pinned on (a place's map inside its country's, say).
    {
        float lx = r.x + pad + gfx_.measureWidth(L"‹ " + w(e->name), Font::Small) + 34;
        for (const Entry* o : lib_->entriesOf(wb_.world, wb_.book))
            if (o->id != e->id && !o->mapBlob.empty())
                for (auto& p : o->pins)
                    if (p.entry == e->id) {
                        const std::wstring t = L"Up to the map of " + w(o->name);
                        linkText("wb:mapup:" + o->id, lx, y, t, [this, oid = o->id] { openMap(oid); });
                        lx += gfx_.measureWidth(t, Font::Small) + 30;
                        break;
                    }
    }
    y += 36;
    gfx_.text(L"MAP", {r.x + pad, y, 200, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 18;
    gfx_.text(L"The map of " + w(e->name), {r.x + pad, y, r.w - pad * 2, 40}, Font::Title, pal_.ink, {Align::Left, false, true});
    y += 46;
    {
        float lx = r.x + pad;
        auto tool = [&](const std::string& id, const std::wstring& t, std::function<void()> f) {
            linkText(id, lx, y, t, std::move(f));
            lx += gfx_.measureWidth(t, Font::Small) + 30;
        };
        tool("wb:mvpin", L"+ Pin an entry…", [this, id = e->id] { pinOnMenu(id); });
        tool("wb:mvreplace", L"Replace the picture…", [this, id = e->id] { chooseMapPicture(id); });
        gfx_.text(std::to_wstring(e->pins.size()) + (e->pins.size() == 1 ? L" pin" : L" pins"), {lx, y + 2, 120, 20}, Font::Small, pal_.inkFaint,
                  {Align::Left, false});
    }
    y += 38;
    if (!wb_.pinning.empty()) {
        const Entry* pe = lib_->entry(wb_.pinning);
        const Rect band{r.x + pad, y, r.w - pad * 2, 34};
        gfx_.fillRounded(band, 6, pal_.ribbon.withAlpha(0.14f));
        gfx_.text(L"Click on the map where " + (pe ? w(pe->name) : std::wstring(L"it")) + L" is.", {band.x + 12, band.y, band.w - 100, band.h}, Font::Body,
                  pal_.ink, {Align::Left, false, true, true});
        linkText("wb:mvpinstop", band.right() - 60, band.y + 9, L"Stop", [this] { wb_.pinning.clear(); invalidate(); });
        y += 44;
    }
    // The board, to the foot of the page.
    const Rect board{r.x + pad - 12, y, r.w - pad * 2 + 24, std::max(220.0f, r.bottom() - 40 - y)};
    wb_.mapBoard = board;
    gfx_.fillRounded(board, 10, pal_.sunken.mix(pal_.paper, 0.45f));
    const std::wstring path = w(lib_->blobPath(e->mapBlob));
    const auto [iw, ih] = gfx_.imageSize(path);
    const float fit = iw > 0 && ih > 0 ? std::min((board.w - 24) / iw, (board.h - 24) / ih) : 1.0f;
    const float sc = fit * wb_.mapZoom;
    const float mw = iw > 0 ? iw * sc : board.w, mh = ih > 0 ? ih * sc : board.h;
    // Keep the centre where the map still covers the board when zoomed in (or centred when it's smaller).
    auto clampCentre = [&](float c, float size, float view) {
        if (size <= view) return 0.5f;
        const float half = view / 2 / size;
        return std::clamp(c, half, 1 - half);
    };
    wb_.mapCX = clampCentre(wb_.mapCX, mw, board.w - 24);
    wb_.mapCY = clampCentre(wb_.mapCY, mh, board.h - 24);
    const Rect img{board.x + board.w / 2 - wb_.mapCX * mw, board.y + board.h / 2 - wb_.mapCY * mh, mw, mh};
    wb_.mapImage = img;
    gfx_.pushClip(board);
    if (!gfx_.drawImage(path, img, false)) gfx_.text(L"The map's picture is missing.", board, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
    if (!wb_.pinning.empty()) gfx_.strokeRounded(board.inset(1), 10, pal_.ribbon, 2);
    // Pins: dots first, then labels where they don't cover one another (the hovered or dragged one always).
    struct P { Id id; float x, y; const Entry* o; };
    std::vector<P> ps;
    for (const auto& p : e->pins) {
        const Entry* o = lib_->entry(p.entry);
        if (!o || o->deleted) continue;
        float fx = (float)p.x, fy = (float)p.y;
        if (wb_.mapDrag == 2 && wb_.mapPin == p.entry) { fx = wb_.mapPinX; fy = wb_.mapPinY; }
        ps.push_back({p.entry, img.x + fx * img.w, img.y + fy * img.h, o});
    }
    std::vector<Rect> placed;
    Id hot;
    for (auto& p : ps) if (hovered("wb:mvp:" + p.id) || (wb_.mapDrag == 2 && wb_.mapPin == p.id)) hot = p.id;
    std::stable_partition(ps.begin(), ps.end(), [&](const P& p) { return p.id == hot; });   // its label first
    for (auto& p : ps) {
        const cjui::Color kc = kindColor(p.o->kind, pal_.dark);
        const std::string id = "wb:mvp:" + p.id;
        addHit(id, {p.x - 11, p.y - 11, 22, 22}, [] {}, false);
        tooltip(id, w(p.o->name) + (p.o->mapBlob.empty() ? L"" : L" (has its own map)") + L": drag to move, click for more");
        const float hv = hoverAmount(id);
        const std::wstring nm = w(p.o->name);
        const float lw = gfx_.measureWidth(nm, Font::Small) + 16;
        Rect label{p.x + 11, p.y - 11, lw, 22};
        if (label.right() > board.right() - 4) label.x = p.x - 11 - lw;
        bool clear = true;
        for (auto& q : placed)
            if (label.x < q.right() + 3 && q.x - 3 < label.right() && label.y < q.bottom() + 3 && q.y - 3 < label.bottom()) clear = false;
        if (clear || p.id == hot) {
            placed.push_back(label);
            gfx_.fillRounded(label, 11, pal_.paper.withAlpha(0.9f + 0.1f * hv));
            gfx_.strokeRounded(label, 11, kc.withAlpha(0.55f + 0.45f * hv));
            gfx_.text(nm, label, Font::Small, pal_.ink, {Align::Center, false, true, true});
        }
        gfx_.circle(p.x, p.y, 8 + 2 * hv, pal_.paper, true);
        gfx_.circle(p.x, p.y, 6 + 2 * hv, kc, true);
        if (!p.o->mapBlob.empty()) gfx_.circle(p.x, p.y, 11 + 2 * hv, kc.withAlpha(0.7f), false, 1.4f);   // a ring: it has a map of its own
    }
    gfx_.popClip();
    // Zoom, floating in the board's corner.
    {
        const float zy = board.y + 10, zx = board.right() - 10 - 3 * 34;
        const Rect zr{zx - 4, zy - 4, 3 * 34 + 8, 36};
        gfx_.fillRounded(zr, 8, pal_.surface);
        gfx_.strokeRounded(zr, 8, pal_.rule);
        auto zbtn = [&](int i, const std::string& id, const std::wstring& lbl, std::function<void()> fn, const std::wstring& tip) {
            const Rect br{zx + i * 34.0f, zy, 30, 28};
            addHit(id, br, std::move(fn));
            tooltip(id, tip);
            gfx_.fillRounded(br, 6, pal_.sunken.withAlpha(hoverAmount(id)));
            gfx_.text(lbl, br, Font::SmallStrong, pal_.ink, {Align::Center, false, false, true});
        };
        zbtn(0, "wb:mvzout", L"−", [this] { wb_.mapZoom = std::max(1.0f, wb_.mapZoom / 1.4f); invalidate(); }, L"Zoom out");
        zbtn(1, "wb:mvfit", L"Fit", [this] { wb_.mapZoom = 1; wb_.mapCX = wb_.mapCY = 0.5f; invalidate(); }, L"The whole map");
        zbtn(2, "wb:mvzin", L"+", [this] { wb_.mapZoom = std::min(8.0f, wb_.mapZoom * 1.4f); invalidate(); }, L"Zoom in");
    }
    gfx_.text(L"Wheel to zoom · drag the map to move about · drag a pin to move it · click a pin for more", {r.x + pad, board.bottom() + 10, r.w - pad * 2, 20},
              Font::Small, pal_.inkFaint, {Align::Left, false, true});
    wb_.content = r.h;
    gfx_.popClip();
}

bool StudioApp::mapMouseDown(float x, float y) {
    if (ws_ != Workspace::Worldbuilding || wb_.mapView.empty() || !wb_.mapBoard.contains(x, y)) return false;
    const std::string id = hitAt(x, y);
    if (id.rfind("wb:mvz", 0) == 0 || id == "wb:mvfit") return false;   // the zoom buttons click as usual
    wb_.mapDownX = x, wb_.mapDownY = y, wb_.mapMoved = false;
    if (id.rfind("wb:mvp:", 0) == 0 && wb_.pinning.empty()) {
        wb_.mapDrag = 2;
        wb_.mapPin = id.substr(7);
        if (const Entry* e = lib_->entry(wb_.mapView))
            for (auto& p : e->pins) if (p.entry == wb_.mapPin) { wb_.mapPinX = (float)p.x; wb_.mapPinY = (float)p.y; }
    } else {
        wb_.mapDrag = 1;
        wb_.mapStartCX = wb_.mapCX, wb_.mapStartCY = wb_.mapCY;
    }
    SetCapture(hwnd());
    return true;
}

bool StudioApp::mapMouseMove(float x, float y, bool leftDown) {
    if (!wb_.mapDrag) return false;
    if (!leftDown) { mapMouseUp(x, y); return true; }
    if (std::hypot(x - wb_.mapDownX, y - wb_.mapDownY) > 4) wb_.mapMoved = true;
    const Rect& img = wb_.mapImage;
    if (wb_.mapDrag == 1 && img.w > 0) {
        wb_.mapCX = wb_.mapStartCX - (x - wb_.mapDownX) / img.w;
        wb_.mapCY = wb_.mapStartCY - (y - wb_.mapDownY) / img.h;
    } else if (wb_.mapDrag == 2 && img.w > 0 && wb_.mapMoved) {
        wb_.mapPinX = std::clamp((x - img.x) / img.w, 0.0f, 1.0f);
        wb_.mapPinY = std::clamp((y - img.y) / img.h, 0.0f, 1.0f);
    }
    invalidate();
    return true;
}

bool StudioApp::mapMouseUp(float x, float y) {
    if (!wb_.mapDrag) return false;
    const int kind = wb_.mapDrag;
    wb_.mapDrag = 0;
    ReleaseCapture();
    const Rect& img = wb_.mapImage;
    if (kind == 2) {
        if (wb_.mapMoved) {
            if (const Entry* e = lib_->entry(wb_.mapView)) {
                Entry u = *e;
                for (auto& p : u.pins) if (p.entry == wb_.mapPin) { p.x = wb_.mapPinX; p.y = wb_.mapPinY; }
                lib_->updateEntry(u);
            }
        } else {
            pinMenu(wb_.mapView, wb_.mapPin);
        }
    } else if (!wb_.mapMoved && !wb_.pinning.empty() && img.contains(x, y)) {
        // Pinning: the click says where.
        const float fx = std::clamp((x - img.x) / img.w, 0.0f, 1.0f), fy = std::clamp((y - img.y) / img.h, 0.0f, 1.0f);
        if (const Entry* e = lib_->entry(wb_.mapView)) {
            Entry u = *e;
            bool moved = false;
            for (auto& p : u.pins) if (p.entry == wb_.pinning) { p.x = fx; p.y = fy; moved = true; }
            if (!moved) u.pins.push_back({wb_.pinning, fx, fy});
            lib_->updateEntry(u);
        }
        wb_.pinning.clear();
    }
    invalidate();
    return true;
}

bool StudioApp::mapWheel(float x, float y, float lines) {
    if (ws_ != Workspace::Worldbuilding || wb_.mapView.empty() || !wb_.mapBoard.contains(x, y)) return false;
    const Rect img = wb_.mapImage;
    const float before = wb_.mapZoom, after = std::clamp(before * (lines > 0 ? 1 / 1.15f : 1.15f), 1.0f, 8.0f);
    if (after == before || img.w <= 0) return true;
    // About the pointer: the map's point under it stays under it.
    const float px = (x - img.x) / img.w, py = (y - img.y) / img.h;
    const float k = before / after;
    const Rect& b = wb_.mapBoard;
    const float cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    wb_.mapCX = px - (x - cx) / (img.w / k);
    wb_.mapCY = py - (y - cy) / (img.h / k);
    wb_.mapZoom = after;
    invalidate();
    return true;
}

// ------------------------------------------------------------- checks

void StudioApp::checkIsFine(const std::string& key) {
    const std::string sk = "checks-ok:" + (wb_.world.empty() ? wb_.book : wb_.world);
    json j = json::parse(lib_->setting(sk), nullptr, false);
    if (!j.is_array()) j = json::array();
    j.push_back(key);
    lib_->setSetting(sk, j.dump());
    invalidate();
}

// The bible checked: names spelt like one of its names, facts that disagree (in the bible, or with the
// text), and entries the manuscripts never name. Each can be marked as fine.
void StudioApp::drawChecks(const Rect& r) {
    const std::string scopeKey = wb_.world.empty() ? wb_.book : wb_.world;
    const std::string key = scopeKey + "|" + std::to_string(lib_->changesMade());
    if (key != wb_.checksKey) {
        wb_.checksKey = key;
        wb_.continuity = review::continuityChecks(*lib_, wb_.world, wb_.world.empty() ? wb_.book : Id{});
        wb_.spellings.clear();
        std::set<std::string> names;
        for (const Id& b : booksInReadingOrder(*lib_, wb_.world, wb_.book))
            for (auto& is : review::nameChecks(*lib_, b))
                if (is.kind == review::NameIssue::Variant && names.insert(is.name).second) wb_.spellings.push_back({b, is});
    }
    std::set<std::string> ok;
    {
        const json j = json::parse(lib_->setting("checks-ok:" + scopeKey), nullptr, false);
        if (j.is_array()) for (auto& x : j) if (x.is_string()) ok.insert(x.get<std::string>());
    }
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 900.0f);
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;
    y += pageHeader(m, L"WORLDBUILDING", L"Checks",
                    L"The bible against itself and the manuscripts. Anything here can be fine as it is: mark it, and it isn't shown again.");
    auto heading = [&](const std::wstring& t, int n) {
        gfx_.text(t + L"  ·  " + std::to_wstring(n), {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 28;
    };
    // ---- Spellings.
    {
        std::vector<const std::pair<Id, review::NameIssue>*> list;
        for (auto& sp : wb_.spellings) if (!ok.count("spell|" + sp.second.name)) list.push_back(&sp);
        heading(L"NAMES SPELT LIKE A BIBLE NAME", (int)list.size());
        if (list.empty()) y += gfx_.text(L"None: no name in the text looks like a misspelling of one in the bible.", {m.x, y, cw, 20}, Font::Small, pal_.inkFaint) + 18;
        for (auto* sp : list) {
            const auto& is = sp->second;
            gfx_.text(w(is.name), {m.x, y, cw * 0.5f, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            const studio::Book* bk = lib_->book(sp->first);
            gfx_.text(L"looks like " + w(is.like) + L" · " + std::to_wstring(is.count) + (is.count == 1 ? L" time" : L" times") +
                          (bk ? L" in " + w(bk->title) : std::wstring()),
                      {m.x, y + 22, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
            float ax = m.x;
            auto act = [&](const std::string& id, const std::wstring& t, std::function<void()> f) {
                linkText(id, ax, y + 46, t, std::move(f));
                ax += gfx_.measureWidth(t, Font::Small) + 26;
            };
            if (!is.nodes.empty()) act("ck:sp:show:" + is.name, L"Show it", [this, b = sp->first, n = is.nodes.front(), nm = is.name] { showInWriting(b, n, {nm}); });
            act("ck:sp:fix:" + is.name, L"Fix it in Review › Names", [this, b = sp->first] { openReview(b, 2); });
            act("ck:sp:ok:" + is.name, L"It's fine", [this, nm = is.name] { checkIsFine("spell|" + nm); });
            y += 74;
            gfx_.line(m.x, y - 6.5f, m.right(), y - 6.5f, pal_.rule.withAlpha(0.5f));
        }
        y += 18;
    }
    // ---- Facts.
    {
        std::vector<const review::FactIssue*> list;
        for (auto& f : wb_.continuity.facts) if (!ok.count("fact|" + f.key())) list.push_back(&f);
        heading(L"FACTS THAT DISAGREE", (int)list.size());
        if (list.empty())
            y += gfx_.text(L"None found. Studio compares each entry's details with each other, and a character's eyes, hair and age with what "
                           L"the text says about them.", {m.x, y, cw, 40}, Font::Small, pal_.inkFaint) + 18;
        for (auto* f : list) {
            const Entry* e = lib_->entry(f->entry);
            if (!e) continue;
            gfx_.text(w(e->name) + L"  ·  " + w(f->label), {m.x, y, cw, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            y += 24;
            const std::wstring said = f->inBible ? L"The bible says both “" + w(f->bible) + L"” and “" + w(f->other) + L"”."
                                                 : L"The bible says “" + w(f->bible) + L"”; the text says “" + w(f->other) + L"”.";
            y += gfx_.text(said, {m.x, y, cw, 40}, Font::Small, pal_.inkMuted) + 4;
            if (!f->snippet.empty()) {
                const studio::Book* bk = lib_->book(f->bookId);
                const ManuscriptNode* nd = lib_->node(f->nodeId);
                std::wstring where = bk ? w(bk->title) : L"";
                if (nd) where += L" › " + w(nd->title.empty() ? std::string(label(nd->kind)) : nd->title);
                y += gfx_.text(where + L":  “" + w(f->snippet) + L"”", {m.x, y, cw, 60}, Font::Small, pal_.ink) + 6;
            }
            float ax = m.x;
            auto act = [&](const std::string& id, const std::wstring& t, std::function<void()> fn) {
                linkText(id, ax, y, t, std::move(fn));
                ax += gfx_.measureWidth(t, Font::Small) + 26;
            };
            const std::string k = f->key();
            if (!f->nodeId.empty())
                act("ck:f:show:" + k, L"Show it", [this, b = f->bookId, n = f->nodeId, words = searchNames(*e)] { showInWriting(b, n, words); });
            act("ck:f:open:" + k, L"Open " + w(e->name), [this, id = e->id] { openEntry(id); });
            act("ck:f:ok:" + k, L"It's fine", [this, k] { checkIsFine("fact|" + k); });
            y += 36;
            gfx_.line(m.x, y - 6.5f, m.right(), y - 6.5f, pal_.rule.withAlpha(0.5f));
        }
        y += 18;
    }
    // ---- Unused.
    {
        std::vector<const Entry*> list;
        for (const Id& id : wb_.continuity.unused)
            if (const Entry* e = lib_->entry(id); e && !e->deleted && !ok.count("unused|" + id)) list.push_back(e);
        heading(L"NEVER NAMED IN THE TEXT", (int)list.size());
        if (list.empty()) y += gfx_.text(L"Every entry's name appears somewhere in the manuscripts.", {m.x, y, cw, 20}, Font::Small, pal_.inkFaint) + 18;
        else y += gfx_.text(L"Their names (and other names) don't appear in any chapter yet. Click one to open it; × marks it as fine.",
                            {m.x, y, cw, 40}, Font::Small, pal_.inkMuted) + 10;
        float lx = m.x;
        for (const Entry* e : list) {
            const std::wstring nm = w(e->name);
            const float lw2 = gfx_.measureWidth(nm, Font::Small) + 52;
            if (lx + lw2 > m.right() && lx > m.x) { lx = m.x; y += 36; }
            const Rect chip{lx, y, lw2, 28};
            const std::string id = "ck:un:" + e->id;
            addHit(id, {chip.x, chip.y, chip.w - 26, chip.h}, [this, eid = e->id] { openEntry(eid); });
            addHit(id + ":x", {chip.right() - 26, chip.y, 26, chip.h}, [this, eid = e->id] { checkIsFine("unused|" + eid); }, false);
            const float hv = hoverAmount(id), xv = hoverAmount(id + ":x");
            const cjui::Color kc = kindColor(e->kind, pal_.dark);
            gfx_.fillRounded(chip, 14, pal_.surface.mix(pal_.sunken, 0.4f * hv));
            gfx_.strokeRounded(chip, 14, pal_.rule.mix(kc, 0.4f + 0.6f * hv));
            gfx_.circle(chip.x + 13, chip.y + 14, 4, kc, true);
            gfx_.text(nm, {chip.x + 22, chip.y, chip.w - 48, chip.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
            const float cx = chip.right() - 14, cy = chip.y + 14;
            gfx_.line(cx - 4, cy - 4, cx + 4, cy + 4, xv > 0.01f ? pal_.ink : pal_.inkFaint, 1.3f);
            gfx_.line(cx + 4, cy - 4, cx - 4, cy + 4, xv > 0.01f ? pal_.ink : pal_.inkFaint, 1.3f);
            if (focused(id)) focusRing(chip);
            lx += lw2 + 8;
        }
        y += 50;
    }
    if (!ok.empty()) {
        linkText("ck:reset", m.x, y, L"Check what was marked as fine again (" + std::to_wstring(ok.size()) + L")", [this, scopeKey] {
            lib_->setSetting("checks-ok:" + scopeKey, "[]");
            invalidate();
        });
        y += 30;
    }
    wb_.content = y + wb_.scroll - r.y + 40;
    wb_.scroll = std::clamp(wb_.scroll, 0.0f, std::max(0.0f, wb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

// ------------------------------------------------------------- glossary

// Every name in the bible from A to Z: how it's said, what it is, and its line or two. Other names
// point to the main one ("Captain Wren, see Ada Wren").
void StudioApp::drawGlossary(const Rect& r) {
    struct Row { std::string sortKey; const Entry* e; std::string alias; };
    std::vector<Row> rows;
    auto sortKey = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        if (s.rfind("the ", 0) == 0) s = s.substr(4);
        return s;
    };
    for (const Entry* e : lib_->entriesOf(wb_.world, wb_.book)) {
        rows.push_back({sortKey(e->name), e, {}});
        for (auto& a : e->aliases) if (!a.empty()) rows.push_back({sortKey(a), e, a});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.sortKey < b.sortKey; });
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 820.0f);
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;
    int said = 0;
    for (auto& row : rows) if (row.alias.empty() && !row.e->pronunciation.empty()) ++said;
    y += pageHeader(m, L"WORLDBUILDING", L"Glossary",
                    std::to_wstring(rows.size()) + L" names, " + std::to_wstring(said) +
                        L" with how they're said. Add how a name is said on its page (“How it's said”).");
    std::wstring letter;
    for (const Row& row : rows) {
        const std::wstring name = w(row.alias.empty() ? row.e->name : row.alias);
        const std::wstring sk = w(row.sortKey);   // "the Admiral" files under A
        std::wstring first = sk.empty() ? L"#" : std::wstring(1, (wchar_t)towupper(sk[0]));
        if (!iswalpha(first[0])) first = L"#";
        if (first != letter) {
            letter = first;
            y += 10;
            gfx_.text(letter, {m.x, y, 60, 34}, Font::Title, pal_.ribbon, {Align::Left, false});
            y += 38;
            gfx_.line(m.x, y - 4.5f, m.right(), y - 4.5f, pal_.rule);
        }
        const std::string id = "wb:gl:" + row.e->id + ":" + row.alias;
        float tx = m.x;
        const float nw = gfx_.measureWidth(name, Font::BodyStrong);
        addHit(id, {m.x, y, nw, 24}, [this, eid = row.e->id] { openEntry(eid); });
        const float hv = hoverAmount(id);
        gfx_.text(name, {tx, y, nw + 2, 24}, Font::BodyStrong, hv > 0.01f ? pal_.ribbon : pal_.ink, {Align::Left, false});
        tx += nw + 12;
        if (!row.alias.empty()) {
            gfx_.text(L"see " + w(row.e->name), {tx, y + 2, m.right() - tx, 22}, Font::Body, pal_.inkMuted, {Align::Left, false, true});
            y += 32;
            continue;
        }
        if (!row.e->pronunciation.empty()) {
            const std::wstring p = L"(" + w(row.e->pronunciation) + L")";
            gfx_.text(p, {tx, y + 2, m.right() - tx, 22}, Font::Body, pal_.inkMuted, {Align::Left, false, true});
            tx += gfx_.measureWidth(p, Font::Body) + 12;
        }
        gfx_.text(upper(w(entryKindLabel(row.e->kind))), {tx, y + 5, m.right() - tx, 18}, Font::Label, kindColor(row.e->kind, pal_.dark),
                  {Align::Left, false, true});
        y += 26;
        if (!row.e->summary.empty()) y += drawLinkedText(row.e->summary, {m.x, y, cw, 0}, Font::Small, pal_.inkMuted, row.e->id, "wb:gls" + row.e->id + ":") + 4;
        if (focused(id)) focusRing({m.x, y - 30, nw, 24});
        y += 12;
    }
    wb_.content = y + wb_.scroll - r.y + 40;
    wb_.scroll = std::clamp(wb_.scroll, 0.0f, std::max(0.0f, wb_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

// ------------------------------------------------------------- sharing

}  // namespace nrs

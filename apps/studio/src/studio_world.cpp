// The Worldbuilding workspace: a world's (or an independent book's) bible of
// characters, places, groups, things, lore and events. The sidebar filters by
// kind; the middle shows the entries as cards, or one entry with its details,
// relationships and where it appears in the manuscripts; the right-hand panel
// edits it.
#include "studio_app.hpp"
#include "studio/entry_paste.hpp"
#include "studio/family_tree.hpp"
#include "cjui/dialog.hpp"
#include "studio/bible_export.hpp"
#include "cjui/menu.hpp"
#include "cjui/northray.hpp"
#include "studio/sync.hpp"
#include "studio/fonts.hpp"

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
static std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
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


// ------------------------------------------------------------- gender and place types
// Kept as a line of the entry's details ("Gender: Female", "Type: City"), so what writers have
// already written counts, and it's still plain text they can edit.

// Each kind's colour (the editor's links and name cards use the same ones).
static cjui::Color kindColor(const std::string& kind, bool dark) { return entryKindColor(kind, dark); }

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
        // Sexuality (0.60): the usual words, and any the world has written.
        {"sexuality", L"Sexuality", "Sexuality", {"sexuality", "sexual orientation", "orientation"},
         {L"Gay", L"Lesbian", L"Bisexual", L"Heterosexual", L"Pansexual", L"Asexual", L"Aromantic", L"Queer", L"Questioning",
          L"Unknown", L"Not said"}, true},
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
    if (kind == "character") keys = {"gender", "sexuality", "race", "realm"};
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

// Sexuality (0.62): a small mark of three bands in the colours its community uses, beside the gender sign on cards and
// group headings; a plain grey mark for any other word (Questioning, Unknown, Not said, your own).
static std::vector<cjui::Color> sexualityBands(const std::wstring& v) {
    auto c = [](const char* h) { return cjui::hexColor(h); };
    std::wstring l;
    for (wchar_t ch : v) l += (wchar_t)towlower(ch);
    if (l == L"gay") return {c("#078D70"), c("#98E8C1"), c("#7BADE2")};
    if (l == L"lesbian") return {c("#D52D00"), c("#FF9A56"), c("#D362A4")};
    if (l == L"bisexual" || l == L"bi") return {c("#D60270"), c("#9B4F96"), c("#0038A8")};
    if (l == L"heterosexual" || l == L"straight") return {c("#3A3A3A"), c("#8A8A8A"), c("#C8C8C8")};
    if (l == L"pansexual" || l == L"pan") return {c("#FF218C"), c("#FFD800"), c("#21B1FF")};
    if (l == L"asexual" || l == L"ace") return {c("#222222"), c("#A3A3A3"), c("#800080")};
    if (l == L"aromantic" || l == L"aro") return {c("#3DA542"), c("#A7D379"), c("#A9A9A9")};
    if (l == L"queer") return {c("#B57EDC"), c("#F2F2F2"), c("#4A8123")};
    return {};
}
static void drawSexualityMark(cjui::Gfx& gfx, const cjui::Rect& r, const std::wstring& v, const cjui::Palette& pal) {
    const auto bands = sexualityBands(v);
    gfx.pushClip(r);
    if (bands.empty()) gfx.fillRounded(r, 2.5f, pal.inkFaint.withAlpha(0.45f));
    else {
        const float h = r.h / (float)bands.size();
        for (size_t i = 0; i < bands.size(); ++i) gfx.fill({r.x, r.y + h * (float)i, r.w, h + 0.5f}, bands[i]);
    }
    gfx.popClip();
    gfx.strokeRounded(r, 2.5f, pal.rule, 1);
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
    return fromBook(fallbackBook());   // the book last written in, or the latest changed (0.66)
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
    wb_.calendarPage = false;
    wb_.share = false;
    wb_.syncPage = false;
    wb_.relationsPage = false;
    wb_.fontsPage = false;
    if (wb_.entry != id) {
        wb_.panelScroll = 0;
        wb_.section.clear();
        wb_.editTemplate = false;
        wb_.picture = -1;
        wb_.spoilersOpen = false;
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
    // Made while looking inside a place: it goes in that place (0.48.5).
    if (kind == "place" && wb_.kind == "place" && wb_.facet.rfind("under=", 0) == 0)
        if (const Entry* in = lib_->entry(wb_.facet.substr(6)); in && in->kind == "place") lib_->addLink(id, in->id, "Located in");
    openEntry(id);
    // Straight into naming it, on its page (0.68).
    focusFieldSoon_ = FName;
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

// The bible's tools (0.68): what the sidebar listed, in one menu at the top of every bible page.
std::vector<StudioApp::ToolItem> StudioApp::bibleTools() {
    std::vector<ToolItem> t;
    auto open = [this](std::function<void()> f) { return [this, f] { f(); invalidate(); }; };
    t.push_back(ToolItem::heading(L"The world"));
    t.push_back(ToolItem::item(L"Glossary", open([this] { openEntry({}); wb_.glossary = true; wb_.scroll = 0; }), wb_.glossary));
    {
        const size_t n = conlangs(wb_.world, wb_.world.empty() ? wb_.book : Id{}).size();
        t.push_back(ToolItem::item(n ? L"Languages  (" + std::to_wstring(n) + L")" : L"Languages", [this] { openLanguages(); }, wb_.languages));
    }
    {
        const studio::cal::Calendar cc = scopeCalendar();
        t.push_back(ToolItem::item(cc.empty() ? L"Calendar" : L"Calendar  (" + w(cc.name.empty() ? std::string("made") : cc.name) + L")",
                                   [this] { openCalendar(); }, wb_.calendarPage));
    }
    {
        const size_t n = lib_->customRelations(wb_.world, wb_.world.empty() ? wb_.book : Id{}).size();
        t.push_back(ToolItem::item(n ? L"Relationships  (" + std::to_wstring(n) + L")" : L"Relationships", [this] { openRelations({}); }, wb_.relationsPage));
    }
    {
        const size_t n = studio::fonts::familiesIn(ownFontsFolder(wb_.world, wb_.book)).size();
        t.push_back(ToolItem::item(n ? L"Fonts  (" + std::to_wstring(n) + L")" : L"Fonts", [this] { openFonts({}); }, wb_.fontsPage));
    }
    {
        // How many it found when last worked out, while the bible hasn't changed since (0.87).
        std::wstring lbl = L"Checks";
        const std::string scopeKey = wb_.world.empty() ? wb_.book : wb_.world;
        if (wb_.checksKey == scopeKey + "|" + std::to_string(lib_->changesMade())) {
            const size_t n = wb_.continuity.facts.size() + wb_.continuity.gaps.size() + wb_.continuity.capitals.size() + wb_.spellings.size();
            lbl += n ? L"  (" + std::to_wstring(n) + L" to look at)" : L"  (all clear)";
        }
        t.push_back(ToolItem::item(lbl, open([this] { openEntry({}); wb_.checks = true; wb_.scroll = 0; }), wb_.checks));
    }
    // The world's map (0.87): the map of a realm or place the writer picks, with every place pinned on it.
    {
        const std::string key = "worldmap:" + (wb_.world.empty() ? wb_.book : wb_.world);
        const Entry* wm = lib_->entry(lib_->setting(key));
        if (wm && (wm->deleted || wm->mapBlob.empty())) wm = nullptr;
        std::vector<ToolItem> sub;
        if (wm) sub.push_back(ToolItem::item(L"Open the world map (" + w(wm->name) + L")", [this, id = wm->id] { openMap(id); }));
        std::vector<ToolItem> pick;
        for (const Entry* x : lib_->entriesOf(wb_.world, wb_.world.empty() ? wb_.book : Id{}))
            if (!x->mapBlob.empty())
                pick.push_back(ToolItem::item(L"The map of " + w(x->name), [this, key, id = x->id] {
                    lib_->setSetting(key, id);
                    openMap(id);
                }, wm && wm->id == x->id));
        if (!pick.empty()) sub.push_back(ToolItem::menu(L"Use as the world map", pick));
        sub.push_back(ToolItem::item(L"Make a world map…", [this, key] {
            // A realm for the whole world, whose map is the world map: the picture is added on its page.
            std::string name = "The world";
            if (const World* wd = lib_->world(wb_.world)) name = wd->name;
            const Id id = lib_->createEntry(wb_.world, wb_.world.empty() ? wb_.book : Id{}, "realm", name);
            if (id.empty()) return;
            lib_->setSetting(key, id);
            openEntry(id);
            flash(L"Made the realm " + w(name) + L" for the world map: add the map's picture under Map on its page, then pin places on it.", 10);
        }));
        t.push_back(ToolItem::menu(wm ? L"World map" : L"World map (none yet)", sub));
    }
    // Image boards: the world's own (a character's are on their page).
    {
        std::vector<ToolItem> boards;
        const json bj = boardsData();
        for (auto& bd : bj["boards"]) {
            if (!bd.value("entry", "").empty()) continue;
            const std::string bid = bd.value("id", "");
            boards.push_back(ToolItem::item(w(bd.value("name", "Board")) + L"  (" + std::to_wstring(bd["items"].size()) + L")", [this, bid] {
                commitFields();
                wb_.board = bid;
                wb_.item.clear();
                wb_.entry.clear();
                wb_.scroll = 0;
                wb_.panelScroll = 0;
                setFocus({Focus::Board, bid, {}});
                invalidate();
            }, wb_.board == bid));
        }
        if (!boards.empty()) boards.push_back(ToolItem::line());
        boards.push_back(ToolItem::item(L"New image board", [this] { newBoard(); }));
        t.push_back(ToolItem::menu(L"Image boards", boards));
    }
    t.push_back(ToolItem::line());
    t.push_back(ToolItem::heading(L"Sharing"));
    t.push_back(ToolItem::item(L"Share the bible", [this] { shareBible(); }, wb_.share));
    if (!wb_.world.empty()) {   // a world (not an independent book) can be kept in GitHub (0.61)
        std::wstring l = L"Sync with GitHub";
        const studio::sync::State ss = studio::sync::loadState(*lib_, wb_.world);
        if (!ss.repo.empty()) {
            const bool bad = syncFailed_.count(wb_.world) && syncFailed_[wb_.world];
            l += syncBusy_ && syncRunning_ == wb_.world ? L"  (syncing…)" : bad ? L"  (didn't finish)" : L"  (on)";
        }
        t.push_back(ToolItem::item(l, [this] { openSyncPage(); }, wb_.syncPage));
    }
    // Its books, to write in.
    std::vector<const studio::Book*> books;
    if (wb_.world.empty()) { if (const studio::Book* b = lib_->book(wb_.book)) books.push_back(b); }
    else for (const studio::Book* b : lib_->liveBooks()) if (b->worldId == wb_.world) books.push_back(b);
    if (!books.empty()) {
        std::vector<ToolItem> bs;
        for (const studio::Book* b : books) bs.push_back(ToolItem::item(w(b->title), [this, id = b->id] { openWriting(id); }));
        t.push_back(ToolItem::line());
        t.push_back(ToolItem::menu(L"Write in a book", bs));
    }
    t.push_back(ToolItem::line());
    t.push_back(ToolItem::heading(L"Bringing pages in"));
    t.push_back(ToolItem::item(L"Import many pages…", [this] { importManyPages(false); }));
    t.push_back(ToolItem::item(L"Copy a blank page to fill in", [this] { copyBlankPageFor(wb_.kind.empty() ? std::string("character") : wb_.kind); }));
    t.push_back(ToolItem::line());
    t.push_back(ToolItem::item(L"Reset the bible…", [this] { resetBible(); }));
    return t;
}

// Along the top of a bible tool's page: back to the cards, and the tools.
void StudioApp::drawWorldStrip(const Rect& r) {
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    linkText("wb:strip:back", r.x + pad, r.y + 16, L"‹ The bible", [this] { openEntry({}); });
    menuButton("wb:strip:tools", r.right() - pad - (gfx_.measureWidth(L"Bible tools", Font::Body) + 44), r.y + 10, L"Bible tools",
               [this] { return bibleTools(); });
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawWorldPage(const Rect& r) {
    // Notes kept for books that weren't here yet move onto them once they are (0.56): looked at when anything changed.
    if (const auto changes = lib_->changesMade(); changes != wb_.claimedAt) {
        wb_.claimedAt = changes;
        if (studio::paste::claimLaterBookNotes(*lib_, wb_.world, wb_.book, booksInReadingOrder(*lib_, wb_.world, wb_.book)) > 0) {
            wb_.claimedAt = lib_->changesMade();
            loadedFocus_ = {Focus::None, "-", ""};
        }
    }
    wb_.scopeEntries = lib_->entriesOf(wb_.world, wb_.book);
    if (!wb_.board.empty()) { drawBoard(r); return; }
    if (!wb_.familyRoot.empty()) { drawFamilyTree(r); return; }
    if (!wb_.mapView.empty()) { drawMapView(r); return; }
    if (wb_.share) { drawShare(r); return; }
    if (wb_.syncPage) { drawSync(r); return; }
    if (wb_.relationsPage) { drawRelations(r); return; }
    if (wb_.fontsPage) { drawFonts(r); return; }
    if (wb_.checks) { drawChecks(r); return; }
    if (wb_.glossary) { drawGlossary(r); return; }
    if (wb_.languages) { drawLanguages(r); return; }
    if (wb_.calendarPage) { drawCalendar(r); return; }
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
        StudioTimer tm_cards("world:cards");
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
        // Places nest (0.48.5): only the places directly in the one looked into ("under=<id>"), or the outermost ones
        // (those in no place, or only in a realm), are cards; the rest are a click away inside them. Finding shows all.
        const bool nestPlaces = findWords.empty() && (wb_.kind.empty() || wb_.kind == "place") &&
                                (wb_.facet.empty() || (wb_.kind == "place" && wb_.facet.rfind("under=", 0) == 0));
        const Id placeUnder = wb_.kind == "place" && wb_.facet.rfind("under=", 0) == 0 ? Id(wb_.facet.substr(6)) : Id{};
        auto directPlaceParent = [&](const Entry& x) -> Id {
            const auto path = placePath(*lib_, x);
            if (path.empty() || path.back()->kind != "place") return {};
            return path.back()->id;
        };
        wb_.placeKids.clear();
        if (nestPlaces)
            for (const Entry* x : lib_->entriesOf(wb_.world, wb_.book))
                if (x->kind == "place") if (const Id p = directPlaceParent(*x); !p.empty()) ++wb_.placeKids[p];
        for (const Entry* x : lib_->entriesOf(wb_.world, wb_.book)) {
            if (!found(*x)) continue;
            if (!wb_.kind.empty() && x->kind != wb_.kind) continue;
            if (nestPlaces && x->kind == "place") {
                if (directPlaceParent(*x) != placeUnder) continue;
                list.push_back(x);
                continue;
            }
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
        // How many: all of the kind (places inside others included, as on the tabs), or how many were found.
        size_t shown = list.size();
        if (findWords.empty() && wb_.facet.empty()) {
            shown = 0;
            for (const Entry* x : wb_.scopeEntries) if (wb_.kind.empty() || x->kind == wb_.kind) ++shown;
        }
        std::wstring sub = std::to_wstring(shown) + (shown == 1 ? L" entry" : L" entries");
        if (!findWords.empty()) sub += L" found";
        if (!wb_.kind.empty()) sub += L" in " + scopeName;
        (void)title;
        // The header (0.68): what this bible is, its name, and the two things to do from here.
        {
            const std::wstring kick = wb_.world.empty() ? L"AN INDEPENDENT BOOK’S BIBLE" : L"THE WORLD BIBLE";
            gfx_.text(kick, {m.x, y, cw, 16}, Font::Label, gold_, {Align::Left, false, true});
            std::wstring nl = L"+ New entry";
            if (!wb_.kind.empty()) {
                std::wstring l = w(entryKindLabel(wb_.kind));
                for (auto& c : l) c = (wchar_t)towlower(c);
                nl = L"+ New " + l;
            }
            const float tbw = gfx_.measureWidth(L"Bible tools", Font::Body) + 44;
            const float nw = buttonWidth(nl);
            menuButton("wb:tools", m.right() - tbw, y + 12, L"Bible tools", [this] { return bibleTools(); });
            button("wb:new", {m.right() - tbw - 10 - nw, y + 12, nw, 32}, nl, [this] { newEntry(wb_.kind); }, true);
            y += 20;
            y += gfx_.text(scopeName, {m.x, y, std::max(200.0f, cw - tbw - nw - 30), 90}, Font::SerifTitle, pal_.ink) + 2;
            gfx_.text(sub, {m.x, y, cw, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
            y += 26;
        }
        // The kinds as tabs (the empty ones under More), and the find box at the end of the row.
        StudioTimer tm_tabs("world:tabs");
        const float findW = std::min(280.0f, cw * 0.32f);
        {
            std::vector<PageTab> tabs, more;
            const auto& everything = wb_.scopeEntries;
            PageTab all;
            all.label = L"Everything";
            all.count = (int)everything.size();
            all.on = wb_.kind.empty();
            all.act = [this] { wb_.kind.clear(); wb_.facet.clear(); wb_.scroll = 0; openEntry({}); };
            tabs.push_back(all);
            for (const auto& k : entryKinds()) {
                int n = 0;
                for (const Entry* x : everything) if (x->kind == k) ++n;
                PageTab t;
                t.label = w(entryKindPlural(k));
                t.count = n;
                t.on = wb_.kind == k;
                t.dot = kindColor(k, pal_.dark);
                t.hasDot = true;
                t.icon = entryKindIcon(k);   // each kind's symbol (0.87)
                t.act = [this, k] { wb_.kind = k; wb_.facet.clear(); wb_.scroll = 0; openEntry({}); };
                (n || t.on ? tabs : more).push_back(t);
            }
            pageTabs("wb:tabs", m.x, y, m.right() - findW - 16, tabs, more);
            const Rect fr{m.right() - findW, y + 3, findW, 32};
            gfx_.fillRounded(fr, 16, pal_.surface);
            gfx_.strokeRounded(fr, 16, GetFocus() == fields_[FFind] ? pal_.ribbon : pal_.rule);
            cjui::drawIcon(gfx_, cjui::Icon::Search, {fr.x + 11, fr.y + (fr.h - 15) / 2, 15, 15}, pal_.inkFaint, 1.4f);
            placeField(FFind, {fr.x + 30, fr.y, fr.w - 30 - (find_.empty() ? 8 : 28), fr.h});
            if (!find_.empty()) {
                // Clear: a drawn cross (not every font has one).
                const Rect xr{fr.right() - 30, fr.y + 4, 24, fr.h - 8};
                addHit("wb:findclear", xr, [this] { setFieldText(FFind, ""); find_.clear(); invalidate(); });
                const float hv = hoverAmount("wb:findclear");
                if (hv > 0.01f) gfx_.fillRounded(xr, 4, pal_.sunken.withAlpha(hv));
                const float cx = xr.x + xr.w / 2, cy = xr.y + xr.h / 2;
                gfx_.line(cx - 4, cy - 4, cx + 4, cy + 4, pal_.inkMuted, 1.4f);
                gfx_.line(cx - 4, cy + 4, cx + 4, cy - 4, pal_.inkMuted, 1.4f);
            }
            y += 40 + 14;
        }
        // Looking inside a place: the way back up (0.48.5).
        if (!placeUnder.empty())
            if (const Entry* in = lib_->entry(placeUnder)) {
                float px = m.x;
                auto crumb = [&](const std::string& id, const std::wstring& t, std::function<void()> f) {
                    linkText(id, px, y - 8, t, std::move(f));
                    px += gfx_.measureWidth(t, Font::Small) + 18;
                    gfx_.text(L"\u203A", {px - 4, y - 6, 14, 20}, Font::Small, pal_.inkFaint, {Align::Left, false});
                    px += 14;
                };
                crumb("wb:pin:all", L"All places", [this] { wb_.facet.clear(); wb_.scroll = 0; invalidate(); });
                for (const Entry* up : placePath(*lib_, *in))
                    if (up->kind == "place") crumb("wb:pin:" + up->id, w(up->name), [this, id = up->id] { wb_.facet = "under=" + id; wb_.scroll = 0; invalidate(); });
                gfx_.text(w(in->name), {px, y - 5, m.right() - px, 20}, Font::SmallStrong, pal_.ink, {Align::Left, false, true});
                linkText("wb:pinopen", m.right() - gfx_.measureWidth(L"Open " + w(in->name) + L"'s page", Font::Small), y - 8,
                         L"Open " + w(in->name) + L"'s page", [this, id = in->id] { openEntry(id); });
                y += 34;
            }
        {
            // Under the tabs (0.68): a kind's entries by gender, race, realm or type as chips; and how they're shown.
        StudioTimer tm_chips("world:chips");
            const bool listable = !list.empty() && !(wb_.kind == "place" && wb_.placesOutline && findWords.empty() && wb_.facet.empty()) &&
                                  !(wb_.kind == "creature" && findWords.empty() && (wb_.facet.empty() || wb_.facet.rfind("under=", 0) == 0));
            const bool grouping = wb_.kind == "character" && !list.empty();
            const std::wstring groupLabel = wb_.groupBy == "sexuality" ? L"Grouped by sexuality" : wb_.groupBy == "race" ? L"Grouped by race"
                                          : wb_.groupBy == "none" ? L"Not grouped" : L"Grouped by gender";
            const float gw = grouping ? gfx_.measureWidth(groupLabel, Font::Body) + 44 : 0;
            const float vw = listable ? 140 : 0;
            const float ctrlW = gw + vw + (gw > 0 && vw > 0 ? 8 : 0);
            const float chipRight = m.right() - ctrlW - (ctrlW > 0 ? 16 : 0);
            const float rowY = y;
            float cx = m.x, cy = y;
            bool anyChip = false;
            auto chip = [&](const std::string& id, const std::wstring& label, int count, bool on, std::function<void()> act) {
                const std::wstring cnt = count >= 0 ? std::to_wstring(count) : L"";
                const float cwid = gfx_.measureWidth(label, Font::Small) + (cnt.empty() ? 0 : gfx_.measureWidth(cnt, Font::Small) + 6) + 24;
                if (cx + cwid > chipRight && cx > m.x) { cx = m.x; cy += 36; }
                const Rect r2{cx, cy, cwid, 28};
                addHit(id, r2, std::move(act));
                const float hv = hoverAmount(id);
                if (!on) softShadow(r2, 14, 0.7f + 0.6f * hv);
                gfx_.fillRounded(r2, 14, on ? pal_.ribbon : pal_.surface);
                const Color ink = on ? (pal_.dark ? pal_.paper : cjui::hexColor("#FFFFFF")) : pal_.ink;
                const float lw = gfx_.measureWidth(label, Font::Small);
                gfx_.text(label, {r2.x + 12, r2.y, lw + 2, r2.h}, Font::Small, ink, {Align::Left, false, false, true});
                if (!cnt.empty())
                    gfx_.text(cnt, {r2.x + 18 + lw, r2.y, 40, r2.h}, Font::Small, on ? ink.withAlpha(0.8f) : pal_.inkFaint, {Align::Left, false, false, true});
                if (focused(id)) focusRing(r2);
                cx += cwid + 8;
                anyChip = true;
            };
            auto chipLabel = [&](const std::wstring& t) {
                const float lw = gfx_.measureWidth(t, Font::Label) + 10;
                if (cx + lw + 130 > chipRight && cx > m.x) { cx = m.x; cy += 36; }   // with room for a chip after it
                gfx_.text(t, {cx, cy, lw, 28}, Font::Label, pal_.inkFaint, {Align::Left, false, false, true});
                cx += lw;
            };
            if (!wb_.kind.empty() && findWords.empty()) {
                const auto& everything = wb_.scopeEntries;
                if (wb_.kind == "creature") {
                    // The top-level kinds of creature (Dragon, ...), each with everything under it.
                    bool first = true;
                    for (const Entry* x : everything)
                        if (x->kind == "creature" && parentPlace(*lib_, *x).empty()) {
                            int under = 0;
                            for (const Entry* o : everything) if (o->kind == "creature" && o != x && isUnder(*lib_, *o, x->id)) ++under;
                            if (!under) continue;
                            if (first) { chipLabel(L"KINDS"); first = false; }
                            const std::string filter = "under=" + x->id;
                            chip("wb:chip:under:" + x->id, w(x->name), under + 1, wb_.facet == filter,
                                 [this, filter] { wb_.facet = wb_.facet == filter ? std::string() : filter; wb_.scroll = 0; invalidate(); });
                        }
                    if (!first) cx += 10;
                }
                for (const Facet* f : facetsFor(wb_.kind)) {
                    std::map<std::string, int> counts;
                    int unset = 0;
                    for (const Entry* x : everything)
                        if (x->kind == wb_.kind) { const std::string v = facetOf(*x, *f); if (v.empty()) ++unset; else ++counts[v]; }
                    if (counts.empty()) continue;   // nothing set yet: no chips
                    // The listed options in order, then the rest; past eight, the seven with most and "N more".
                    std::vector<std::pair<std::string, std::pair<std::wstring, int>>> vals;
                    for (auto& o : f->options)
                        if (auto it = counts.find(narrow(o)); it != counts.end()) { vals.push_back({it->first, {o, it->second}}); counts.erase(it); }
                    for (auto& [v, cnt] : counts) vals.push_back({v, {w(v), cnt}});
                    const std::string openKey = wb_.kind + ":" + f->key;
                    const bool open = wb_.facetsOpen.count(openKey) > 0;
                    const size_t total = vals.size();
                    if (!open && vals.size() > 8) {
                        std::vector<size_t> idx(vals.size());
                        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
                        std::stable_sort(idx.begin(), idx.end(), [&](size_t a2, size_t b2) { return vals[a2].second.second > vals[b2].second.second; });
                        std::set<size_t> keep(idx.begin(), idx.begin() + 7);
                        for (size_t i = 0; i < vals.size(); ++i) if (wb_.facet == f->key + "=" + vals[i].first) keep.insert(i);
                        decltype(vals) kept;
                        for (size_t i = 0; i < vals.size(); ++i) if (keep.count(i)) kept.push_back(vals[i]);
                        vals = kept;
                    }
                    chipLabel(upper(f->title));
                    for (auto& [v, lc] : vals) {
                        const std::string filter = f->key + "=" + v;
                        chip("wb:chip:" + filter, lc.first, lc.second, wb_.facet == filter,
                             [this, filter] { wb_.facet = wb_.facet == filter ? std::string() : filter; wb_.scroll = 0; invalidate(); });
                    }
                    if (unset) {
                        const std::string filter = f->key + "=-";
                        chip("wb:chip:" + filter, L"Not set", unset, wb_.facet == filter,
                             [this, filter] { wb_.facet = wb_.facet == filter ? std::string() : filter; wb_.scroll = 0; invalidate(); });
                    }
                    if (total > 8)
                        chip("wb:chip:more:" + openKey, open ? L"Fewer" : std::to_wstring(total - vals.size()) + L" more", -1, false, [this, openKey] {
                            if (!wb_.facetsOpen.erase(openKey)) wb_.facetsOpen.insert(openKey);
                            invalidate();
                        });
                    cx += 10;
                }
            }
            float rx = m.right() - ctrlW;
            if (grouping) {
                menuButton("wb:groupby", rx, rowY - 2, groupLabel, [this] {
                    std::vector<ToolItem> t;
                    const std::pair<const char*, const wchar_t*> opts[] = {{"gender", L"By gender or sex"}, {"sexuality", L"By sexuality"},
                                                                           {"race", L"By race"}, {"none", L"Not grouped"}};
                    for (auto& [k, l] : opts) {
                        const std::string key = k;
                        const bool on = wb_.groupBy == key || (key == "gender" && wb_.groupBy.empty());
                        t.push_back(ToolItem::item(l, [this, key] { wb_.groupBy = key; invalidate(); }, on));
                    }
                    return t;
                });
                rx += gw + 8;
            }
            if (listable)
                segmented("wb:view", {rx, rowY, vw, 28}, {L"Cards", L"List"}, settings_.entryList ? 1 : 0, [this](int i) {
                    settings_.entryList = i == 1;
                    saveSettings();
                    invalidate();
                });
            if (anyChip || ctrlW > 0) y = cy + 28 + 22;
        }
        { StudioTimer ts_("world:suggest"); if (findWords.empty()) y = drawKindSuggestions(m, y); }
        if ((wb_.kind == "character" || wb_.kind == "creature") && wb_.facet.empty() && findWords.empty()) {
            // The families in the bible (of this kind), each opening its tree; worked out again only when the library
            // changes (0.59). With many, the biggest eight and a link to the rest.
            const std::string fk = wb_.world + "|" + wb_.book + "|" + std::to_string(lib_->changesMade());
            if (fk != wb_.famKey) { wb_.famKey = fk; wb_.fams = lib_->families(wb_.world, wb_.book); }
            std::vector<studio::Library::FamilyGroup> fams;
            for (const auto& fg : wb_.fams)
                if (const Entry* re = lib_->entry(fg.root); re && re->kind == wb_.kind) fams.push_back(fg);
            const size_t total = fams.size();
            constexpr size_t kFew = 8;
            if (!wb_.allFamilies && fams.size() > kFew + 2) {
                std::stable_sort(fams.begin(), fams.end(), [](const auto& a, const auto& b) { return a.people > b.people; });
                fams.resize(kFew);
            }
            if (!fams.empty()) {
                gfx_.text(L"FAMILY TREES", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
                if (total > kFew + 2)
                    linkText("wb:fam:all", m.right() - gfx_.measureWidth(wb_.allFamilies ? L"Only the biggest" : L"All " + std::to_wstring(total), Font::Small), y - 3,
                             wb_.allFamilies ? L"Only the biggest" : L"All " + std::to_wstring(total), [this] { wb_.allFamilies = !wb_.allFamilies; invalidate(); });
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
        if (wb_.kind == "place" && findWords.empty() && wb_.facet.empty()) {
            // Cards or the outline (0.48.3).
            float lx = m.x;
            for (int i = 0; i < 2; ++i) {
                const std::wstring t = i == 0 ? L"Cards" : L"Outline";
                const bool on = wb_.placesOutline == (i == 1);
                const float tw2 = gfx_.measureWidth(t, Font::Small) + 26;
                const Rect b{lx, y, tw2, 28};
                const std::string id = i == 0 ? "wb:plview0" : "wb:plview1";
                addHit(id, b, [this, i] { wb_.placesOutline = i == 1; invalidate(); });
                gfx_.fillRounded(b, 6, on ? pal_.surface : pal_.sunken.withAlpha(0.5f * hoverAmount(id)));
                if (on) gfx_.strokeRounded(b, 6, pal_.rule);
                gfx_.text(t, b, Font::Small, on ? pal_.ink : pal_.inkMuted, {Align::Center, false, false, true});
                lx += tw2 + 4;
            }
            y += 44;
        }
        if (wb_.kind == "place" && wb_.placesOutline && findWords.empty() && wb_.facet.empty()) {
            y = drawPlacesOutline(m, y);
        } else if (wb_.kind == "creature" && findWords.empty() && (wb_.facet.empty() || wb_.facet.rfind("under=", 0) == 0)) {
            y = drawCreatureTree(m, y);
        } else if (list.empty() && !findWords.empty()) {
            y += gfx_.text(L"Nothing in the bible matches \u201C" + w(find_) + L"\u201D" + (wb_.kind.empty() ? L"." : L" among the " + w(entryKindPlural(wb_.kind)) + L"."),
                           {m.x, y, std::min(cw, 640.0f), 60}, Font::Body, pal_.inkMuted) + 16;
        } else if (list.empty()) {
            // One line and one button (0.65).
            const std::wstring line = wb_.kind.empty() ? L"The bible is empty. Start with a character, a place or anything else."
                                                       : L"No " + lower(w(entryKindPlural(wb_.kind))) + L" yet.";
            y += emptyState({m.x, y, std::min(cw, 640.0f), 300}, cjui::Icon::World, line, wb_.kind.empty() ? L"+ New entry" : L"+ New " + lower(w(entryKindLabel(wb_.kind))), [this] { newEntry(wb_.kind); });
        } else {
            // Each entry's first book, worked out again when the library has changed.
            const std::string key = wb_.world + "|" + wb_.book + "|" + std::to_string(lib_->changesMade());
            if (key != wb_.firstBookKey) {
                StudioTimer tfb_("world:firstBook");
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

            // Cards, or (0.59) a list of rows, one per entry, for big bibles.
            const bool rows = settings_.entryList;
            const float gap = rows ? 4 : 14;
            const int cols = rows ? 1 : std::max(1, (int)((cw + gap) / (290 + gap)));   // 0.46: a little wider, more room for the summary
            const float cardW = (cw - gap * (cols - 1)) / cols, cardH = rows ? 44 : 118;   // 0.43: picture, two lines, details
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
            } else if (wb_.kind == "character" && wb_.groupBy == "sexuality" && wb_.facet.rfind("sexuality=", 0) != 0) {
                subKind = "sexuality";
                const Facet* sf = findFacet("sexuality");
                std::map<std::wstring, std::vector<Item>> by;
                for (auto& it : items) by[w(facetOf(*it.e, *sf))].push_back(it);
                for (auto& o : sf->options) if (auto f = by.find(o); f != by.end()) { subs.push_back({o, f->second}); by.erase(f); }
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
                } else if (subKind == "sexuality" && subLabel != L"Not set") {
                    drawSexualityMark(gfx_, {m.x + 1, y + 4, 16, 12}, subLabel, pal_);
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
                // Only the cards on screen are drawn (0.59): a bible of hundreds of entries took seconds a frame.
                if (cr.bottom() < r.y - 20 || cr.y > r.bottom() + 20) {
                    if (col == cols - 1 || i + 1 == subItems.size()) y += cardH + gap;
                    continue;
                }
                if (rows) {
                    // One row: the kind's colour, a small picture or initials, the name, what it is, the summary's start.
                    const std::string id = "wb:card:" + x->id;
                    addHit(id, cr, [this, eid = x->id] { openEntry(eid); });
                    const float hv = hoverAmount(id);
                    const cjui::Color kc = kindColor(x->kind, pal_.dark);
                    gfx_.fillRounded(cr, 4, pal_.surface.mix(pal_.sunken, 0.35f * hv));
                    gfx_.fill({cr.x + 1, cr.y + 6, 3, cr.h - 12}, kc);
                    const float indent = std::min(depth, 4) * 18.0f;
                    const Rect pic{cr.x + 14 + indent, cr.y + 6, 32, 32};
                    drawPortrait(*x, pic, 16);   // its picture in a circle, else its symbol or initials (0.87)
                    drawKindMark(x->kind, {pic.right() - 11, pic.bottom() - 11, 14, 14});
                    const float nx = pic.right() + 12;
                    const float nameW = std::min(260.0f, (cr.right() - nx) * 0.34f);
                    gfx_.text(w(x->name), {nx, cr.y, nameW, cr.h}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
                    std::wstring label = upper(w(entryKindLabel(x->kind)));
                    for (const Facet* f : facetsFor(x->kind))
                        if (const std::string v = facetOf(*x, *f); !v.empty()) label += L"  ·  " + upper(w(v));
                    const float lx2 = nx + nameW + 14, lw2 = std::min(300.0f, (cr.right() - lx2) * 0.45f);
                    gfx_.text(label, {lx2, cr.y, lw2, cr.h}, Font::Label, kc, {Align::Left, false, true, true});
                    const float sx = lx2 + lw2 + 14;
                    if (!x->summary.empty() && cr.right() - 14 - sx > 60)
                        gfx_.text(w(x->summary), {sx, cr.y, cr.right() - 14 - sx, cr.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
                    if (focused(id)) focusRing(cr);
                    y += cardH + gap;
                    continue;
                }
                const std::string id = "wb:card:" + x->id;
                addHit(id, cr, [this, eid = x->id] { openEntry(eid); });
                const float hv = hoverAmount(id);
                const cjui::Color kc = kindColor(x->kind, pal_.dark);
                // A card on the paper (0.68): lifted a little, more when pointed at.
                softShadow(cr, 10, 1.3f + 1.2f * hv);
                gfx_.fillRounded(cr, 10, pal_.surface);
                if (pal_.dark || hv > 0.01f) gfx_.strokeRounded(cr, 10, (pal_.dark ? pal_.rule : pal_.surface).mix(kc, 0.4f * hv), 1);
                // The kind's colour: a band down the left edge, and the initials' circle.
                gfx_.fillRounded({cr.x + 5, cr.y + 12, 3, cr.h - 24}, 1.5f, kc);
                // The picture (a portrait's top), or the initials in a ring of the kind's colour.
                const Rect pic{cr.x + 14, cr.y + 14, 72, 90};
                if (x->imageBlob.empty() || !gfx_.drawImageRounded(w(lib_->blobPath(x->imageBlob)), pic, 6)) {
                    drawPortrait(*x, {pic.x + 4, pic.y + 13, 64, 64}, 32);   // its symbol or initials (0.87)
                } else {
                    gfx_.strokeRounded(pic, 6, pal_.rule, 1);
                }
                const float tx = pic.right() + 14, tw = cr.right() - 14 - tx;
                {
                    // The kind's symbol before its name (0.87), so the kind reads at a glance.
                    drawKindMark(x->kind, {tx, cr.y + 12, 16, 16}, "wb:kind:" + x->id);
                    std::wstring label = upper(w(entryKindLabel(x->kind)));
                    for (const Facet* f : facetsFor(x->kind))
                        if (f->key != "gender" && f->key != "sexuality")
                            if (const std::string v = facetOf(*x, *f); !v.empty()) label += L"  \u00B7  " + upper(w(v));
                    gfx_.text(label, {tx + 21, cr.y + 13, tw - 41, 16}, Font::Label, kc, {Align::Left, false, true});
                }
                // A character's gender or sex: its sign in the card's corner, in the group's colour; its sexuality beside it
                // as a small banded mark (0.62).
                if (x->kind == "character") {
                    const std::wstring g = w(facetOf(*x, *findFacet("gender")));
                    if (!g.empty()) drawGenderSign(gfx_, cr.right() - 18, cr.y + 20, 9, g, genderColor(g, pal_));
                    if (const std::wstring sx = w(facetOf(*x, *findFacet("sexuality"))); !sx.empty()) {
                        const Rect mk{cr.right() - (g.empty() ? 30.0f : 46.0f), cr.y + 14, 16, 12};
                        drawSexualityMark(gfx_, mk, sx, pal_);
                        addHit("wb:sx:" + x->id, mk, [this, eid = x->id] { openEntry(eid); }, false);
                        tooltip("wb:sx:" + x->id, sx);
                    }
                }
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
                        float reserve = 0;   // room for "N inside ›" on a place with places inside
                        if (x->kind == "place") if (auto pk = wb_.placeKids.find(x->id); pk != wb_.placeKids.end() && pk->second > 0) reserve = 90;
                        const float room = tw - (chx - tx) - reserve;
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
                // A place with places inside: a button into them (0.48.5).
                if (x->kind == "place")
                    if (auto pk = wb_.placeKids.find(x->id); pk != wb_.placeKids.end() && pk->second > 0) {
                        const std::wstring t = std::to_wstring(pk->second) + L" inside \u203A";
                        const float bw = gfx_.measureWidth(t, Font::Small) + 18;
                        const Rect b{cr.right() - bw - 10, cr.bottom() - 30, bw, 22};
                        const std::string bid = "wb:inside:" + x->id;
                        addHit(bid, b, [this, eid = x->id] { wb_.kind = "place"; wb_.facet = "under=" + eid; wb_.scroll = 0; invalidate(); });
                        tooltip(bid, L"The places inside " + w(x->name));
                        const float bh = hoverAmount(bid);
                        gfx_.fillRounded(b, 11, kc.withAlpha(0.14f + 0.16f * bh));
                        gfx_.text(t, b, Font::Small, kc.mix(pal_.ink, 0.35f), {Align::Center, false, false, true});
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
        // ---- One entry, edited where it's shown (0.68): click a name, the summary, the details or a section and type;
        // Enter or a click elsewhere keeps it, Esc puts it back. Its rarer things are under the "..." button.
        if (!(focus_ == loadedFocus_)) loadFields();
        if (wb_.picture >= (int)e->gallery.size()) wb_.picture = -1;
        if (wb_.editTemplate && !editingInline("template")) wb_.editTemplate = false;
        // A field asked for elsewhere (a new entry's name, a section to write, a picture to caption) is edited here.
        if (focusFieldSoon_ >= 0) {
            const int f = focusFieldSoon_;
            focusFieldSoon_ = -1;
            if (f == FName) startEntryEdit("name", FName);
            else if (f == FSection && !wb_.section.empty()) startEntryEdit("sec:" + wb_.section, FSection);
            else if (f == FBookNote) { if (const Id nb = noteBookFor(*e); !nb.empty()) startEntryEdit("bn:" + nb, FBookNote); }
            else if (f == FSynopsis) startEntryEdit("details", FSynopsis);
            else if (f == FCaption && wb_.picture >= 0) startEntryEdit("caption", FCaption);
            else if (f == FTemplate) startEntryEdit("template", FTemplate);
            else if (f == FNotes) startEntryEdit("notes", FNotes);
        }
        const float measure = std::min(cw, 700.0f);
        // A piece of text to click and edit: the hover frame (behind it, sized as it was last drawn), then the text.
        auto editable = [&](const std::string& key, int field, float x, float yy, float wid, std::function<float()> drawIt, const std::wstring& tip) {
            float& h = areaH_["wb:" + key];
            editableArea("wb:ed:" + key, {x, yy, wid, std::max(h, 18.0f)}, tip, [this, key, field] { startEntryEdit(key, field); });
            h = drawIt();
            return h;
        };
        const std::wstring back = wb_.kind.empty() ? L"‹ Everything" : L"‹ " + w(entryKindPlural(wb_.kind));
        linkText("wb:back", m.x, y, back, [this] { openEntry({}); });
        // Where it is: the places it sits in, each a link (0.48).
        if (e->kind == "place" || e->kind == "realm" || e->kind == "creature") {
            const auto path = placePath(*lib_, *e);
            float px = m.x + gfx_.measureWidth(back, Font::Small) + 34;
            for (size_t i = 0; i < path.size(); ++i) {
                const std::wstring t = w(path[i]->name);
                const float tw2 = gfx_.measureWidth(t, Font::Small);
                if (px + tw2 > m.right() - 300) break;
                linkText("wb:path:" + path[i]->id, px, y, t, [this, id = path[i]->id] { openEntry(id); });
                px += tw2 + 18;
                gfx_.text(L"›", {px - 4, y + 2, 14, 20}, Font::Small, pal_.inkFaint, {Align::Left, false});
                px += 14;
            }
            if (!path.empty()) gfx_.text(w(e->name), {px, y + 3, std::max(0.0f, m.right() - 300 - px), 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        }
        // At the right: a section to add, and the rest of what can be done with it.
        {
            const Rect mb{m.right() - 40, y - 7, 40, 32};
            addHit("wb:entrymore", mb, [this, mb] {
                if (const Entry* x = lib_->entry(wb_.entry)) showTools(entryTools(*x), mb.right(), mb.bottom() + 4, true);
            });
            tooltip("wb:entrymore", L"More for this entry");
            const float hv = hoverAmount("wb:entrymore");
            if (hv > 0.01f) softShadow(mb, 8, 0.6f * hv);
            gfx_.fillRounded(mb, 8, pal_.surface.mix(pal_.sunken, 0.35f * hv));
            gfx_.strokeRounded(mb, 8, pal_.rule.mix(pal_.inkFaint, 0.55f * hv));
            for (int i = -1; i <= 1; ++i) gfx_.circle(mb.x + mb.w / 2 + i * 6.5f, mb.y + mb.h / 2, 1.8f, pal_.ink, true);
            if (focused("wb:entrymore")) focusRing(mb);
            const float aw = gfx_.measureWidth(L"Add a section", Font::Body) + 44;
            menuButton("wb:addsec", mb.x - 8 - aw, mb.y, L"Add a section", [this] {
                std::vector<ToolItem> t;
                const Entry* x = lib_->entry(wb_.entry);
                if (!x) return t;
                bool any = false;
                for (const auto& [title, text] : lib_->entrySections(*x))
                    if (text.empty()) { t.push_back(ToolItem::item(w(title), [this, title = title] { startEntryEdit("sec:" + title, FSection); })); any = true; }
                if (wb_.bookOrder.empty()) wb_.bookOrder = booksInReadingOrder(*lib_, wb_.world, wb_.book);
                std::vector<ToolItem> notes;
                for (const Id& bid : x->bookId.empty() ? wb_.bookOrder : std::vector<Id>{x->bookId})
                    if (const studio::Book* b = lib_->book(bid); b && x->bookNote(bid).empty())
                        notes.push_back(ToolItem::item(L"In " + w(b->title), [this, bid] { startEntryEdit("bn:" + bid, FBookNote); }));
                if (!notes.empty()) {
                    if (any) t.push_back(ToolItem::line());
                    t.push_back(ToolItem::heading(L"What happens to it in a book"));
                    for (auto& n : notes) t.push_back(n);
                    any = true;
                }
                if (x->notes.empty()) {
                    if (any) t.push_back(ToolItem::line());
                    t.push_back(ToolItem::item(L"Notes", [this] { startEntryEdit("notes", FNotes); }));
                }
                t.push_back(ToolItem::line());
                t.push_back(ToolItem::item(L"Change the sections every " + w(entryKindLabel(x->kind)) + L" page has…", [this] {
                    wb_.editTemplate = true;
                    startEntryEdit("template", FTemplate);
                }));
                return t;
            });
        }
        y += 40;
        // A paste taken back stays offered at the top, where it's wanted straight after one.
        if (wb_.pasteUndo && wb_.pasteUndo->entry == e->id) {
            gfx_.text(L"This page was filled from what you copied.", {m.x, y, measure, 22}, Font::Small, pal_.inkMuted, {Align::Left, false, false, true});
            linkText("wb:pasteundo", m.x + gfx_.measureWidth(L"This page was filled from what you copied.", Font::Small) + 16, y + 2, L"Undo the paste",
                     [this] { undoEntryPaste(); });
            y += 34;
        }
        // The spoilers, in a card while they're being set.
        if (wb_.spoilersOpen) {
            if (wb_.bookOrder.empty()) wb_.bookOrder = booksInReadingOrder(*lib_, wb_.world, wb_.book);
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
            const float ch = 92 + facts.size() * 28.0f;
            const Rect cr{m.x, y, measure, ch};
            card(cr, 12);
            gfx_.text(L"Spoilers", {cr.x + 18, cr.y + 14, cr.w - 120, 26}, Font::Heading, pal_.ink, {Align::Left, false});
            linkText("wb:spoildone", cr.right() - 18 - gfx_.measureWidth(L"Done", Font::Small), cr.y + 18, L"Done", [this] { wb_.spoilersOpen = false; invalidate(); });
            gfx_.text(L"Mark what readers only learn in a later book. A reader's guide or shared wiki for an earlier book leaves it out.",
                      {cr.x + 18, cr.y + 42, cr.w - 36, 36}, Font::Small, pal_.inkMuted);
            float sy = cr.y + 82;
            for (auto& [key, label] : facts) {
                const studio::Book* rb = lib_->book(e->revealedIn(key));
                gfx_.text(label, {cr.x + 18, sy, cr.w * 0.5f, 22}, Font::Small, pal_.ink, {Align::Left, false, true, true});
                const std::wstring v = rb ? L"Revealed in " + w(rb->title) : L"Not a spoiler";
                linkText("wb:spoil:" + key, cr.right() - 18 - gfx_.measureWidth(v, Font::Small), sy + 3, v, [this, key = key] { entrySpoilerChoice(key); });
                sy += 28;
            }
            y += ch + 24;
        }
        const bool hasPic = !e->imageBlob.empty();
        const float picW = std::min(200.0f, cw * 0.28f);
        float ty = y;
        const float tx = m.x + picW + 28, tw = m.right() - tx, tmeasure = std::min(tw, 640.0f);
        {
            // The picture, or a place for one: click to choose it, or drop one from the gallery on it.
            float ph = picW * 1.2f;
            const std::wstring path = hasPic ? w(lib_->blobPath(e->imageBlob)) : std::wstring();
            if (hasPic) {
                const auto [iw, ih] = gfx_.imageSize(path);
                ph = iw > 0 ? std::min(picW * ih / (float)iw, picW * 1.5f) : picW;
            }
            const Rect pr{m.x, y, picW, ph};
            addHit("wb:pic", pr, [this] { chooseImage(Focus::Entry, wb_.entry, 0); });
            tooltip("wb:pic", hasPic ? L"Change the picture" : L"Choose a picture");
            addDrop({Drop::MainPicture, pr, e->id});   // drag a picture here to make it the main one
            const float hv = hoverAmount("wb:pic");
            if (hasPic) {
                softShadow(pr, 12, 1.6f + hv);
                gfx_.drawImage(path, pr, true);
                cjui::northray::roundCorners(gfx_, pr, 12, pal_.paper);
                if (hv > 0.01f) {
                    const Rect lb{pr.x + 10, pr.bottom() - 36, pr.w - 20, 26};
                    gfx_.fillRounded(lb, 7, cjui::Color{0.06f, 0.08f, 0.12f, 0.6f * hv});
                    gfx_.text(L"Change the picture", lb, Font::Small, cjui::hexColor("#FFFFFF").withAlpha(hv), {Align::Center, false, true, true});
                }
            } else {
                gfx_.fillRounded(pr, 12, pal_.surface.mix(pal_.sunken, 0.3f + 0.4f * hv));
                gfx_.strokeRounded(pr, 12, pal_.rule.mix(pal_.inkFaint, 0.5f * hv), 1.2f);
                if (!e->symbol.empty()) {
                    // Its chosen symbol until it has a picture (0.87).
                    const float ds = std::min(pr.w, pr.h) * 0.5f;
                    drawPortrait(*e, {pr.x + (pr.w - ds) / 2, pr.y + pr.h / 2 - ds / 2 - 12, ds, ds}, ds / 2);
                    gfx_.text(L"Add a picture", {pr.x, pr.y + pr.h / 2 + ds / 2 - 2, pr.w, 20}, Font::Small, pal_.inkMuted, {Align::Center, false, true});
                } else {
                    cjui::drawIcon(gfx_, cjui::Icon::Picture, {pr.x + pr.w / 2 - 14, pr.y + pr.h / 2 - 26, 28, 28}, pal_.inkFaint, 1.4f);
                    gfx_.text(L"Add a picture", {pr.x, pr.y + pr.h / 2 + 8, pr.w, 20}, Font::Small, pal_.inkMuted, {Align::Center, false, true});
                }
            }
            if (focused("wb:pic")) focusRing(pr);
            y += ph;
        }
        {
            // What it is: the kind and how it's filed, each a click away from being changed.
            float kx = tx;
            auto part = [&](const std::string& id, const std::wstring& t, cjui::Color c, std::function<void()> act, const std::wstring& tip) {
                const float pw = gfx_.measureWidth(t, Font::Label);
                const Rect pr{kx - 3, ty - 3, pw + 6, 22};
                addHit(id, pr, std::move(act));
                tooltip(id, tip);
                const float hv = hoverAmount(id);
                if (hv > 0.01f) gfx_.fillRounded(pr, 5, c.withAlpha(0.12f * hv));
                gfx_.text(t, {kx, ty, pw + 2, 16}, Font::Label, c, {Align::Left, false});
                if (focused(id)) focusRing(pr);
                kx += pw;
            };
            const cjui::Color kc = kindColor(e->kind, pal_.dark);
            part("wb:k:kind", upper(w(entryKindLabel(e->kind))), kc, [this] { entryKindChoice(); }, L"Change what kind of entry it is");
            for (const Facet* f : facetsFor(e->kind))
                if (const std::string v = facetOf(*e, *f); !v.empty()) {
                    gfx_.text(L"  ·  ", {kx, ty, 30, 16}, Font::Label, kc, {Align::Left, false});
                    kx += gfx_.measureWidth(L"  ·  ", Font::Label);
                    part("wb:k:" + f->key, upper(w(v)), kc, [this, key = f->key] { entryFacetChoice(key); }, L"Change its " + f->title);
                }
        }
        ty += 22;
        // Its name, in the title's size.
        if (editingInline("name")) {
            const Rect nr{tx - 10, ty - 4, std::min(tw + 10, 660.0f), 50};
            inlineBox(nr);
            ty += 50 + 22;
        } else {
            ty += editable("name", FName, tx, ty, tmeasure, [&] { return gfx_.text(w(e->name), {tx, ty, tmeasure, 90}, Font::SerifTitle, pal_.ink); },
                           L"Click to rename it") + 6;
        }
        // How it's said.
        if (editingInline("said")) {
            gfx_.text(L"How it's said (for the glossary: ZAH-mez)", {tx, ty, tmeasure, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
            inlineBox({tx - 8, ty + 20, std::min(tmeasure, 360.0f), 34});
            ty += 20 + 34 + 26;
        } else if (!e->pronunciation.empty()) {
            ty += editable("said", FAlt, tx, ty, tmeasure,
                           [&] { return gfx_.text(L"Said “" + w(e->pronunciation) + L"”", {tx, ty, tmeasure, 22}, Font::Body, pal_.inkMuted); },
                           L"Click to change how it's said") + 6;
        }
        if (const studio::Book* rb = lib_->book(e->revealedIn("entry")))
            ty += gfx_.text(L"SPOILER · FIRST MET IN " + upper(w(rb->title)), {tx, ty, tw, 18}, Font::Label, pal_.demo, {Align::Left, false}) + 8;
        // Its other names.
        if (editingInline("aliases")) {
            gfx_.text(L"Other names, with commas between", {tx, ty, tmeasure, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
            inlineBox({tx - 8, ty + 20, tmeasure + 8, 34});
            ty += 20 + 34 + 26;
        } else if (!e->aliases.empty()) {
            std::wstring a = L"Also ";
            for (size_t i = 0; i < e->aliases.size(); ++i) a += (i ? L", " : L"") + std::wstring(L"“") + w(e->aliases[i]) + L"”";
            ty += editable("aliases", FTags, tx, ty, tmeasure, [&] { return gfx_.text(a, {tx, ty, tmeasure, 60}, Font::Small, pal_.inkMuted); },
                           L"Click to change its other names") + 8;
        }
        // The summary.
        ty += 4;
        if (editingInline("summary")) {
            const float h = inlineHeight(70);
            inlineBox({tx - 8, ty - 4, tmeasure + 8, h});
            ty += h + 26;
        } else if (!e->summary.empty()) {
            ty += editable("summary", FDescription, tx, ty, tmeasure,
                           [&] { return drawLinkedText(e->summary, {tx, ty, tmeasure, 0}, Font::Body, pal_.ink, e->id, "wb:ls:"); },
                           L"Click to change the summary") + 12;
        } else {
            ty += editable("summary", FDescription, tx, ty, tmeasure,
                           [&] { return gfx_.text(L"Write a summary…", {tx, ty, tmeasure, 22}, Font::Body, pal_.inkFaint); },
                           L"Click to write a summary") + 12;
        }
        // Details: "Label: value" lines as a table; click it to change them.
        ty += 6;
        if (editingInline("details")) {
            gfx_.text(L"One per line, like Age: 34", {tx, ty, tmeasure, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
            // A date from the world's calendar, picked from a little calendar (0.75).
            if (!scopeCalendar().empty() && !wb_.datePick)
                linkText("wb:pickdate", tx + tmeasure - gfx_.measureWidth(L"Insert a date…", Font::Small) - 14, ty - 4, L"Insert a date…",
                         [this] { openDatePicker(FSynopsis); });
            const float h = inlineHeight(90);
            inlineBox({tx - 8, ty + 20, tmeasure + 8, h});
            ty += 20 + h + 26;
            if (wb_.datePick) ty += drawDatePicker({tx - 8, ty + 2, tmeasure + 8, 0}, FSynopsis) + 22;
        } else if (!e->details.empty()) {
            ty += editable("details", FSynopsis, tx, ty, tmeasure, [&] {
                float dy = ty;
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
                        gfx_.text(w(line.substr(0, colon)), {tx, dy, lw - 10, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
                        if (const studio::Book* rb = lib_->book(e->revealedIn("detail:" + line.substr(0, colon)))) {
                            const std::wstring t = L"spoiler · " + w(rb->title);
                            const float bw = std::min(gfx_.measureWidth(t, Font::Small) + 14, 220.0f);
                            const Rect badge{tx + tmeasure - bw, dy, bw, 20};
                            gfx_.fillRounded(badge, 10, pal_.demo.withAlpha(0.12f));
                            gfx_.text(t, badge, Font::Small, pal_.demo, {Align::Center, false, true, true});
                        }
                        dy += std::max(20.0f, drawLinkedText(val, {tx + lw, dy, std::min(tw - lw, 480.0f), 0}, Font::Small, pal_.ink, e->id,
                                                             "wb:ld" + std::to_string(at) + ":")) + 4;
                    } else {
                        dy += gfx_.text(w(line), {tx, dy, tmeasure, 80}, Font::Small, pal_.ink) + 4;
                    }
                    gfx_.line(tx, dy - 1.5f, tx + tmeasure, dy - 1.5f, pal_.rule.withAlpha(0.5f));
                    dy += 2;
                }
                return dy - ty;
            }, L"Click to change the details");
        } else {
            linkText("wb:adddetail", tx, ty, L"+ Detail", [this] { startEntryEdit("details", FSynopsis); });
            ty += 30;
        }
        if (!editingInline("details")) {
            if (wb_.datePick) { wb_.datePick = false; ie_.hold = false; }
            if (const float dh = drawEntryDates(*e, tx, ty + 6, tmeasure); dh > 0) ty += dh + 10;
        }
        y = std::max(y, ty) + 30;
        // A row to jump to the page's parts further down (where they started last time it was drawn) (0.48.4).
        {
            const auto anchors = wb_.anchors;
            wb_.anchors.clear();
            if (anchors.size() >= 3) {
                float lx = m.x;
                gfx_.text(L"ON THIS PAGE", {lx, y + 5, 110, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
                lx += 112;
                for (auto& [name, at] : anchors) {
                    const float tw2 = gfx_.measureWidth(name, Font::Small);
                    if (lx + tw2 + 20 > m.right()) break;
                    const float target = at;
                    linkText("wb:jump:" + narrow(name), lx, y + 2, name, [this, target] {
                        wb_.scroll = std::clamp(target - 16, 0.0f, std::max(0.0f, wb_.content - pageRect_.h));
                        invalidate();
                    });
                    lx += tw2 + 26;
                }
                y += 40;
            }
        }
        auto anchor = [&](const wchar_t* name, float at) { wb_.anchors.push_back({name, at - m.y}); };

        // Changing the sections every page of its kind has: here, in a card, while it's being done.
        if (editingInline("template")) {
            const World* wo = lib_->world(e->worldId);
            const std::wstring kindName = w(entryKindLabel(e->kind));
            const float h = inlineHeight(130);
            const Rect cr{m.x, y, measure, 150 + h};
            card(cr, 12);
            gfx_.text(L"The sections of a " + kindName + L" page" + (wo ? L" (all of " + w(wo->name) + L")" : L""), {cr.x + 18, cr.y + 14, cr.w - 36, 26},
                      Font::Heading, pal_.ink, {Align::Left, false, true});
            gfx_.text(L"One per line.", {cr.x + 18, cr.y + 42, cr.w - 36, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
            inlineBox({cr.x + 14, cr.y + 64, cr.w - 28, h});
            linkText("wb:tplreset", cr.x + 18, cr.y + 64 + h + 28, L"Use the built-in sections", [this] {
                const Entry* x = lib_->entry(wb_.entry);
                if (!x) return;
                endInline(false);
                wb_.editTemplate = false;
                lib_->setEntryTemplate(x->worldId, x->kind, defaultEntryTemplate(x->kind));
                loadedFocus_ = {Focus::None, "-", ""};
                invalidate();
            });
            gfx_.text(L"For buttons to pick from, write the choices after a colon: “Status: Alive | Dead | Missing”. Add (any) to the name to "
                      L"pick several. Writing in a section you take out stays on the page.",
                      {cr.x + 230, cr.y + 64 + h + 26, cr.w - 248, 40}, Font::Small, pal_.inkFaint);
            y += cr.h + 26;
        }
        // The page's sections, from the kind's template: those written, and the one being started. The rest are under
        // "Add a section".
        for (const auto& [title, text] : lib_->entrySections(*e)) {
            const std::string key = "sec:" + title;
            const bool editing = editingInline(key);
            if (text.empty() && !editing) continue;
            gfx_.text(w(title), {m.x, y, measure, 28}, Font::Heading, pal_.ink, {Align::Left, false, true});
            if (const studio::Book* rb = lib_->book(e->revealedIn("section:" + title)))
                gfx_.text(L"SPOILER · " + upper(w(rb->title)), {m.x + gfx_.measureWidth(w(title), Font::Heading) + 14, y + 6, 300, 18}, Font::Label,
                          pal_.demo, {Align::Left, false, true});
            y += 32;
            if (editing) {
                // A choice (0.37): its options as buttons, and the box for anything else.
                const SectionSpec spec = lib_->entrySectionSpec(*e, title);
                if (!spec.options.empty()) {
                    const std::string cur = e->section(title);
                    gfx_.text(spec.many ? L"Pick any that fit, or write your own" : L"Pick one, or write your own", {m.x, y, measure, 18}, Font::Small,
                              pal_.inkFaint, {Align::Left, false});
                    y += 24;
                    float lx = m.x;
                    for (const std::string& o : spec.options) {
                        const std::wstring nm = w(o);
                        const float bw = std::min(measure, gfx_.measureWidth(nm, Font::Small) + 28);
                        if (lx + bw > m.x + measure && lx > m.x) { lx = m.x; y += 36; }
                        const Rect chip{lx, y, bw, 30};
                        const std::string id = "wb:opt:" + o;
                        const bool on = sectionOptionPicked(spec, cur, o);
                        addHit(id, chip, [this, o, title = title] {
                            commitFields();   // what was typed first
                            const Entry* x = lib_->entry(wb_.entry);
                            if (!x) return;
                            const SectionSpec sp = lib_->entrySectionSpec(*x, title);
                            Entry u = *x;
                            u.setSection(title, toggleSectionOption(sp, u.section(title), o));
                            lib_->updateEntry(u);
                            startEntryEdit("sec:" + title, FSection);   // still being written
                        }, false);
                        const float hv = hoverAmount(id);
                        gfx_.fillRounded(chip, 15, on ? pal_.ribbon : pal_.surface.mix(pal_.sunken, 0.4f * hv));
                        gfx_.strokeRounded(chip, 15, on ? pal_.ribbon : pal_.rule.mix(pal_.ribbon, hv));
                        gfx_.text(nm, chip, Font::Small, on ? pal_.paper : pal_.ink, {Align::Center, false, true, true});
                        lx += bw + 8;
                    }
                    y += 44;
                }
                const float h = inlineHeight(spec.options.empty() ? 120 : 60);
                inlineBox({m.x - 8, y - 4, measure + 8, h});
                y += h + 34;
            } else {
                y += editable(key, FSection, m.x, y, measure,
                              [&] { return drawLinkedText(text, {m.x, y, measure, 0}, Font::Body, pal_.ink, e->id, "wb:lsec" + title + ":"); },
                              L"Click to change it") + 28;
            }
        }

        // Notes for each book, in reading order: what happens to it there, what it knows.
        {
            if (wb_.bookOrder.empty()) wb_.bookOrder = booksInReadingOrder(*lib_, wb_.world, wb_.book);
            for (const Id& bid : wb_.bookOrder) {
                const std::string& note = e->bookNote(bid);
                const studio::Book* b = lib_->book(bid);
                const std::string key = "bn:" + bid;
                const bool editing = editingInline(key);
                if ((note.empty() && !editing) || !b) continue;
                gfx_.text(L"In " + w(b->title), {m.x, y, measure, 28}, Font::Heading, pal_.ink, {Align::Left, false, true});
                y += 32;
                if (editing) {
                    gfx_.text(L"What happens to it in this book, what it knows", {m.x, y, measure, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
                    const float h = inlineHeight(100);
                    inlineBox({m.x - 8, y + 20, measure + 8, h});
                    y += 20 + h + 34;
                } else {
                    y += editable(key, FBookNote, m.x, y, measure,
                                  [&] { return drawLinkedText(note, {m.x, y, measure, 0}, Font::Body, pal_.ink, e->id, "wb:lbn" + bid + ":"); },
                                  L"Click to change it") + 28;
                }
            }
            // Notes for books that aren't in Writing Studio yet (0.56): kept until the book is added, then moved onto it.
            for (auto& [label, note] : studio::paste::laterBookNotes(*e)) {
                gfx_.text(L"IN " + upper(w(label)), {m.x, y, cw - 140, 18}, Font::Label, pal_.inkFaint, {Align::Left, false, true, true});
                const std::string k = std::string(studio::paste::kLaterBook) + label;
                const Id eid = e->id;
                linkText("wb:bnlater:" + label, m.right() - gfx_.measureWidth(L"Remove", Font::Small), y - 2, L"Remove", [this, eid, k, label] {
                    const Entry* x = lib_->entry(eid);
                    if (!x) return;
                    const Entry before = *x;
                    Entry u = *x;
                    u.setBookNote(k, "");
                    lib_->updateEntry(u);
                    invalidate();
                    flashUndo(L"Removed the notes for " + w(label) + L".", [this, before] { lib_->updateEntry(before); invalidate(); });
                });
                y += 22;
                y += gfx_.text(L"Not in Writing Studio yet: these notes move onto the book when a book of that title is added.",
                               {m.x, y, std::min(cw, 680.0f), 40}, Font::Small, pal_.inkMuted) + 6;
                y += drawLinkedText(note, {m.x, y, std::min(cw, 680.0f), 0}, Font::Body, pal_.ink, e->id, "wb:lbl" + label + ":") + 24;
            }
        }

        if (e->kind == "character" || e->kind == "creature") y = drawFamilySection(m, y, *e);
        if (e->kind == "place" || e->kind == "realm") y = drawPlaceSection(m, y, *e);
        if (e->kind == "place" || e->kind == "realm") y = drawMapSection(m, y, *e);

        // Pictures (0.60): the gallery and the entry's boards in one place. Drag a picture onto the main picture to make it
        // the main one, or onto a board to put it there.
        {
            anchor(L"Pictures", y);
            const json bj = boardsData();
            std::vector<const json*> mine;
            for (auto& bd : bj["boards"]) if (bd.value("entry", "") == e->id) mine.push_back(&bd);
            gfx_.text(L"PICTURES" + (e->gallery.empty() ? std::wstring() : L"  \u00B7  " + std::to_wstring(e->gallery.size())), {m.x, y, cw, 18},
                      Font::Label, pal_.inkFaint, {Align::Left, false});
            {
                const float bwid = gfx_.measureWidth(L"+ New board", Font::Small);
                const float awid = gfx_.measureWidth(L"+ Add pictures\u2026", Font::Small);
                linkText("wb:eboardnew", m.right() - bwid, y - 2, L"+ New board", [this] { newEntryBoard(); });
                linkText("wb:galadd", m.right() - bwid - 24 - awid, y - 2, L"+ Add pictures\u2026", [this] { addGalleryPictures(); });
            }
            y += 30;
            if (e->gallery.empty()) {
                y += gfx_.text(L"Art, sketches, references and looks for it. The first one you add is the main picture.",
                               {m.x, y, std::min(cw, 640.0f), 40}, Font::Small, pal_.inkFaint) + 18;
            } else {
                y += gfx_.text(L"Drag a picture onto the main picture at the top to swap it in, or onto a board below to add it there. "
                               L"Click one to caption it.",
                               {m.x, y, std::min(cw, 640.0f), 40}, Font::Small, pal_.inkFaint) + 12;
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
                    const bool isMain = pic.blob == e->imageBlob;
                    if (isMain) addDrop({Drop::MainPicture, pr, e->id});
                    const float hv = hoverAmount(id);
                    const bool sel = wb_.picture == (int)i;
                    const bool lifted = drag_.active && drag_.kind == DragState::Picture && drag_.id == std::to_string(i);
                    softShadow(pr, 4, std::max(0.3f + 0.7f * hv, sel ? 1.0f : 0.0f));
                    if (!gfx_.drawImage(path, pr, true)) {
                        gfx_.fillRounded(pr, 4, pal_.sunken);
                        gfx_.text(L"Missing", pr, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
                    }
                    if (lifted) gfx_.fillRounded(pr, 4, pal_.paper.withAlpha(0.6f));
                    if (isMain) {
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
                y += rowH + 24;
                // The picture picked: its caption, edited here, and what else can be done with it (0.68).
                if (wb_.picture >= 0 && wb_.picture < (int)e->gallery.size()) {
                    const auto& pic = e->gallery[(size_t)wb_.picture];
                    const bool editing = editingInline("caption");
                    const Rect cr{m.x, y, measure, editing ? 132.0f : 104.0f};
                    card(cr, 12);
                    gfx_.text(L"Caption", {cr.x + 18, cr.y + 14, 200, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
                    if (editing) inlineBox({cr.x + 14, cr.y + 36, cr.w - 28, 36});
                    else {
                        const float cx0 = cr.x + 18, cyy = cr.y + 38;
                        editable("caption", FCaption, cx0, cyy, cr.w - 36, [&] {
                            return gfx_.text(pic.caption.empty() ? L"Write a caption…" : w(pic.caption), {cx0, cyy, cr.w - 36, 22}, Font::Body,
                                             pic.caption.empty() ? pal_.inkFaint : pal_.ink, {Align::Left, false, true});
                        }, L"Click to change the caption");
                    }
                    float lx2 = cr.x + 18;
                    const float ly = cr.bottom() - 30;
                    if (pic.blob != e->imageBlob) {
                        linkText("wb:galmain", lx2, ly, L"Use as the main picture", [this] {
                            commitFields();
                            if (const Entry* x = lib_->entry(wb_.entry); x && wb_.picture >= 0 && wb_.picture < (int)x->gallery.size()) {
                                Entry u = *x;
                                u.imageBlob = x->gallery[(size_t)wb_.picture].blob;
                                lib_->updateEntry(u);
                                flash(L"It's now the entry's main picture.", 4);
                            }
                        });
                        lx2 += gfx_.measureWidth(L"Use as the main picture", Font::Small) + 28;
                    }
                    linkText("wb:galremove", lx2, ly, L"Remove from the gallery", [this] { galleryMenu((size_t)wb_.picture); });
                    linkText("wb:galdone", cr.right() - 18 - gfx_.measureWidth(L"Done", Font::Small), ly, L"Done", [this] {
                        endInline(true);
                        wb_.picture = -1;
                        loadedFocus_ = {Focus::None, "-", ""};
                        invalidate();
                    });
                    y += cr.h + 24;
                }
            }

            // Its boards, under the pictures.
            gfx_.text(L"BOARDS" + (mine.empty() ? std::wstring() : L"  \u00B7  " + std::to_wstring(mine.size())), {m.x, y, cw, 18},
                      Font::Label, pal_.inkFaint, {Align::Left, false});
            y += 26;
            if (mine.empty()) {
                y += gfx_.text(L"Boards arrange pictures freely: outfits, moods, a place's look. A world board (Bible tools) can be moved "
                               L"here from its details.",
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
                    addDrop({Drop::PictureToBoard, br, bid});
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
            anchor(L"Members", y);
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
                drawPortrait(*o, pic, 16);
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

        anchor(L"Relationships", y);
        gfx_.text(L"RELATIONSHIPS", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        linkText("wb:addrel", m.right() - gfx_.measureWidth(L"+ Add a relationship", Font::Small), y - 2, L"+ Add a relationship",
                 [this] { addRelationship(); });
        // As a list, or as a web of portraits (0.87).
        segmented("wb:relview", {m.x + 150, y - 6, 150, 26}, {L"List", L"Web"}, wb_.relView, [this](int i) { wb_.relView = i; invalidate(); });
        y += 28;
        std::vector<EntryLink> links;
        // A character's parents, partners, brothers and sisters and children are under Family, not again here (0.48.4).
        for (const EntryLink& l : lib_->linksOf(e->id)) {
            if ((e->kind == "group" || e->kind == "race") && isMembership(l)) continue;   // listed under Members
            if ((e->kind == "character" || e->kind == "creature") && isFamilyRelation(l.label)) {
                const Entry* o = lib_->entry(l.from == e->id ? l.to : l.from);
                if (o && o->kind == e->kind) continue;
            }
            links.push_back(l);
        }
        // Grouped by how they read from here (all the "Friend of" together), A to Z within each (0.48.4).
        const Id scope = wb_.world.empty() ? wb_.book : Id{};
        auto readAs = [&](const EntryLink& l) { return l.from == e->id ? l.label : lib_->relationReverse(wb_.world, scope, l.label); };
        std::stable_sort(links.begin(), links.end(), [&](const EntryLink& a, const EntryLink& b) {
            const std::string la = readAs(a), lb = readAs(b);
            if (la != lb) return la < lb;
            const Entry* oa = lib_->entry(a.from == e->id ? a.to : a.from);
            const Entry* ob = lib_->entry(b.from == e->id ? b.to : b.from);
            return (oa ? oa->name : std::string()) < (ob ? ob->name : std::string());
        });
        std::string lastLabel;
        if (links.empty()) {
            y += gfx_.text(L"None yet: link this entry to the people, places and things around it.", {m.x, y, cw, 20}, Font::Small,
                           pal_.inkFaint) + 12;
        }
        // Each kind of link has a colour (the same everywhere: by its words), and is written on its line too.
        auto linkColour = [&](const std::string& label) {
            static const char* light[] = {"#C0392B", "#2471A3", "#1E8449", "#B9770E", "#7D3C98", "#148F77", "#A04000", "#2E4053", "#B03A2E", "#5B2C6F"};
            static const char* dark[] = {"#EC7063", "#5DADE2", "#58D68D", "#F5B041", "#BB8FCE", "#48C9B0", "#E59866", "#AEB6BF", "#F1948A", "#C39BD3"};
            unsigned h = 2166136261u;
            for (char c : label) h = (h ^ (unsigned char)std::tolower((unsigned char)c)) * 16777619u;
            return cjui::hexColor((pal_.dark ? dark : light)[h % 10]);
        };
        if (wb_.relView == 1 && !links.empty()) {
            // The web: this page in the middle, each linked page round it, joined by a line in its link's colour with the
            // link's words on it. Chips above leave a kind of link out, or bring it back.
            std::map<std::string, int> counts;
            for (const EntryLink& l : links) ++counts[readAs(l)];
            float fx = m.x;
            for (auto& [label, n] : counts) {
                const bool off = wb_.relHidden.count(label) > 0;
                const std::wstring t = w(label) + L"  " + std::to_wstring(n);
                const float cwid = gfx_.measureWidth(t, Font::Small) + 34;
                if (fx + cwid > m.right() && fx > m.x) { fx = m.x; y += 34; }
                const Rect chip{fx, y, cwid, 26};
                const std::string id = "wb:relf:" + label;
                addHit(id, chip, [this, label] {
                    if (!wb_.relHidden.erase(label)) wb_.relHidden.insert(label);
                    invalidate();
                });
                tooltip(id, off ? L"Show these links in the web" : L"Leave these links out of the web");
                const cjui::Color lc = linkColour(label);
                gfx_.fillRounded(chip, 13, off ? pal_.paper : lc.withAlpha(pal_.dark ? 0.18f : 0.10f));
                gfx_.strokeRounded(chip, 13, off ? pal_.rule : lc.withAlpha(0.7f), 1.2f);
                if (off) gfx_.circle(chip.x + 13, chip.y + 13, 4.5f, pal_.inkFaint, false, 1.4f);
                else gfx_.circle(chip.x + 13, chip.y + 13, 4.5f, lc, true);
                gfx_.text(t, {chip.x + 24, chip.y, chip.w - 28, chip.h}, Font::Small, off ? pal_.inkFaint : pal_.ink, {Align::Left, false, true, true, off});
                if (focused(id)) focusRing(chip);
                fx += cwid + 8;
            }
            y += 40;
            std::vector<const EntryLink*> shown;
            for (const EntryLink& l : links) if (!wb_.relHidden.count(readAs(l))) shown.push_back(&l);
            const float webH = std::clamp(220.0f + 18.0f * shown.size(), 300.0f, 560.0f);
            const Rect board{m.x, y, cw, webH};
            gfx_.fillRounded(board, 12, pal_.sunken.withAlpha(0.45f));
            const float cx = board.x + board.w / 2, cyy = board.y + board.h / 2;
            const size_t n = shown.size();
            const float rx = std::min(board.w / 2 - 70, 120 + 9.0f * n), ry = board.h / 2 - 52;
            std::vector<std::pair<float, float>> at(n);
            for (size_t i = 0; i < n; ++i) {
                const float a = -3.14159265f / 2 + 2 * 3.14159265f * (float)i / (float)std::max<size_t>(1, n);
                // Two rings when crowded, so neighbours don't sit on each other.
                const float k = n > 12 && i % 2 ? 0.66f : 1.0f;
                at[i] = {cx + rx * k * std::cos(a), cyy + ry * k * std::sin(a)};
            }
            // Lines first, labels on them, then the portraits over everything.
            for (size_t i = 0; i < n; ++i) {
                const cjui::Color lc = linkColour(readAs(*shown[i]));
                gfx_.line(cx, cyy, at[i].first, at[i].second, lc.withAlpha(0.8f), 2);
            }
            for (size_t i = 0; i < n; ++i) {
                const std::wstring t = w(readAs(*shown[i]));
                const float tw2 = gfx_.measureWidth(t, Font::Label) + 12;
                const float lxm = cx + (at[i].first - cx) * 0.55f, lym = cyy + (at[i].second - cyy) * 0.55f;
                const Rect pill{lxm - tw2 / 2, lym - 9, tw2, 18};
                const cjui::Color lc = linkColour(readAs(*shown[i]));
                gfx_.fillRounded(pill, 9, pal_.surface);
                gfx_.strokeRounded(pill, 9, lc.withAlpha(0.8f), 1);
                gfx_.text(t, pill, Font::Label, pal_.ink, {Align::Center, false, true, true});
            }
            const float ps = n > 16 ? 38 : 46;
            for (size_t i = 0; i < n; ++i) {
                const EntryLink& l = *shown[i];
                const Entry* other = lib_->entry(l.from == e->id ? l.to : l.from);
                if (!other) continue;
                const Rect pr{at[i].first - ps / 2, at[i].second - ps / 2, ps, ps};
                const std::string id = "wb:web:" + other->id + l.label;
                addHit(id, {pr.x - 20, pr.y, ps + 40, ps + 20}, [this, oid = other->id] { openEntry(oid); });
                tooltip(id, w(other->name) + L" (" + w(entryKindLabel(other->kind)) + L"): " + w(readAs(l)));
                const float hv = hoverAmount(id);
                if (hv > 0.01f) softShadow(pr, ps / 2, hv);
                drawPortrait(*other, pr, ps / 2);
                gfx_.strokeRounded(pr.inset(-2), ps / 2 + 2, linkColour(readAs(l)).withAlpha(0.6f + 0.4f * hv), 2);
                drawKindMark(other->kind, {pr.right() - 12, pr.bottom() - 12, 15, 15});
                gfx_.text(w(other->name), {pr.x - 50, pr.bottom() + 3, ps + 100, 16}, Font::Label, pal_.ink, {Align::Center, false, true, true});
                if (focused(id)) focusRing(pr);
            }
            const float cs = 64;
            drawPortrait(*e, {cx - cs / 2, cyy - cs / 2, cs, cs}, cs / 2);
            gfx_.strokeRounded({cx - cs / 2 - 3, cyy - cs / 2 - 3, cs + 6, cs + 6}, cs / 2 + 3, pal_.ribbon, 2.5f);
            if (n == 0)
                gfx_.text(L"Every kind of link is left out. Click a chip above to bring one back.", {board.x, board.bottom() - 34, board.w, 20},
                          Font::Small, pal_.inkFaint, {Align::Center, false, true});
            y += webH + 22;
            links.clear();   // drawn as the web, not again as the list
        }
        for (const EntryLink& l : links) {
            const bool mine = l.from == e->id;
            const Entry* other = lib_->entry(mine ? l.to : l.from);
            if (!other) continue;
            const std::string readLabel = mine ? l.label : lib_->relationReverse(wb_.world, scope, l.label);
            if (readLabel != lastLabel) {   // the label once, over the names that share it; a rule between groups
                if (!lastLabel.empty()) { y += 6; gfx_.line(m.x, y - 4.5f, m.right(), y - 4.5f, pal_.rule.withAlpha(0.6f)); }
                gfx_.text(w(readLabel), {m.x, y, 160, 24}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
            }
            lastLabel = readLabel;
            // Its portrait and kind's symbol before the name (0.87); the label's colour beside it, as on the web.
            gfx_.circle(m.x + 162, y + 12, 3.5f, linkColour(readLabel), true);
            drawPortrait(*other, {m.x + 170, y + 1, 22, 22}, 11);
            linkText("wb:rel:" + other->id + l.label, m.x + 200, y + 4, w(other->name), [this, oid = other->id] { openEntry(oid); }, Font::Body);
            drawKindMark(other->kind, {m.right() - gfx_.measureWidth(L"Remove", Font::Small) - 40, y + 4, 16, 16}, "wb:relk:" + other->id + l.label);
            gfx_.text(w(entryKindLabel(other->kind)), {m.x + 200, y, cw - 200 - 84, 24}, Font::Small, pal_.inkFaint,
                      {Align::Right, false, true, true});
            linkText("wb:unrel:" + other->id + l.label, m.right() - gfx_.measureWidth(L"Remove", Font::Small), y + 4, L"Remove",
                     [this, l] { lib_->removeLink(l.from, l.to, l.label); invalidate(); });
            y += 28;
        }
        y += 22;

        // Notes: click them to change them.
        if (!e->notes.empty() || editingInline("notes")) {
            anchor(L"Notes", y);
            gfx_.text(L"Notes", {m.x, y, measure, 28}, Font::Heading, pal_.ink, {Align::Left, false});
            y += 32;
            if (editingInline("notes")) {
                const float h = inlineHeight(140);
                inlineBox({m.x - 8, y - 4, measure + 8, h});
                y += h + 34;
            } else {
                y += editable("notes", FNotes, m.x, y, measure,
                              [&] { return drawLinkedText(e->notes, {m.x, y, measure, 0}, Font::Body, pal_.ink, e->id, "wb:ln:"); },
                              L"Click to change the notes") + 28;
            }
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
        anchor(L"Appears in", y);
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
    // Only the bible's tools that keep a details column call this (as a card on the page, 0.68); an entry's own
    // things are on its page.
    if (!panelAsCard_) {
        gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
        gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    }
    if (!wb_.board.empty()) { drawBoardPanel(r); return; }
    if (wb_.languages) { drawLanguagePanel(r); return; }
    if (wb_.calendarPage) { drawCalendarPanel(r); return; }
    if (wb_.share) { drawSharePanel(r); return; }
    if (wb_.syncPage) { drawSyncPanel(r); return; }
    if (wb_.relationsPage) { drawRelationsPanel(r); return; }
    if (wb_.fontsPage) { drawFontsPanel(r); return; }
    if (!wb_.familyRoot.empty() && wb_.entry.empty()) { drawFamilyPanel(r); return; }
}

// ------------------------------------------------------------- an entry's choices (from its page, 0.68)

void StudioApp::startEntryEdit(const std::string& key, int field) {
    const Entry* e = lib_->entry(wb_.entry);
    if (!e) return;
    if (ie_.field >= 0) endInline(true);
    commitFields();
    if (key.rfind("sec:", 0) == 0) wb_.section = key.substr(4);
    else if (key.rfind("bn:", 0) == 0) wb_.noteBook = key.substr(3);
    if (key == "template") wb_.editTemplate = true;
    loadedFocus_ = {Focus::None, "-", ""};
    loadFields();
    // Esc puts back what was there when the edit began (what's typed is saved as it goes).
    const Entry before = *lib_->entry(wb_.entry);
    const auto tpl = lib_->entryTemplate(before.worldId, before.kind);
    startInline(key, field, [this, before, tpl, key] {
        lib_->updateEntry(before);
        if (key == "template") { lib_->setEntryTemplate(before.worldId, before.kind, tpl); wb_.editTemplate = false; }
    }, field == FName);
}

// What can be done with an entry beyond its text: under the "..." button on its page.
std::vector<StudioApp::ToolItem> StudioApp::entryTools(const Entry& e) {
    std::vector<ToolItem> t;
    t.push_back(ToolItem::heading(L"What it is"));
    t.push_back(ToolItem::item(L"Kind: " + w(entryKindLabel(e.kind)) + L"…", [this] { entryKindChoice(); }));
    t.push_back(ToolItem::item(L"Symbol (until it has a picture)…", [this, id = e.id] { chooseEntrySymbol(id); }));
    if (e.kind == "creature" || e.kind == "character") {
        const Entry* parent = nullptr;
        for (const EntryLink& l : lib_->linksOf(e.id))
            if (l.from == e.id && l.label == "Kind of") if (const Entry* o = lib_->entry(l.to); o && !o->deleted && o->kind == "creature") parent = o;
        const std::wstring now = parent ? w(parent->name) : e.kind == "character" ? L"not a creature" : L"a species of its own";
        t.push_back(ToolItem::item((e.kind == "character" ? L"Kind of creature: " : L"A kind of: ") + now + L"…", [this] { entryKindOfChoice(); }));
    }
    for (const Facet* f : facetsFor(e.kind)) {
        const std::string v = facetOf(e, *f);
        t.push_back(ToolItem::item(f->title + L": " + (v.empty() ? std::wstring(L"not set") : w(v)) + L"…", [this, key = f->key] { entryFacetChoice(key); }));
    }
    t.push_back(ToolItem::line());
    t.push_back(ToolItem::heading(L"On the page"));
    t.push_back(ToolItem::item(e.aliases.empty() ? L"Add other names" : L"Change its other names", [this] { startEntryEdit("aliases", FTags); }));
    t.push_back(ToolItem::item(e.pronunciation.empty() ? L"Add how it's said" : L"Change how it's said", [this] { startEntryEdit("said", FAlt); }));
    t.push_back(ToolItem::item(e.imageBlob.empty() ? L"Choose a picture…" : L"Change the picture…", [this] { chooseImage(Focus::Entry, wb_.entry, 0); }));
    if (!e.imageBlob.empty())
        t.push_back(ToolItem::item(L"Remove the picture", [this] {
            if (const Entry* x = lib_->entry(wb_.entry)) {
                const Entry before = *x;
                Entry u = *x;
                u.imageBlob.clear();
                lib_->updateEntry(u);
                flashUndo(L"Removed the picture.", [this, before] { lib_->updateEntry(before); invalidate(); });
            }
        }));
    if (wb_.bookOrder.empty()) wb_.bookOrder = booksInReadingOrder(*lib_, wb_.world, wb_.book);
    if (!wb_.bookOrder.empty()) t.push_back(ToolItem::item(L"Spoilers…", [this] { wb_.spoilersOpen = true; wb_.scroll = 0; invalidate(); }, wb_.spoilersOpen));
    t.push_back(ToolItem::line());
    t.push_back(ToolItem::heading(L"Pages from elsewhere"));
    t.push_back(ToolItem::item(L"Fill this page from what you've copied", [this] { pasteFromClipboard(); }));
    t.push_back(ToolItem::item(L"Replace all its text with what you've copied", [this] { pasteFromClipboard(true); }));
    t.push_back(ToolItem::item(L"Copy a blank page to fill in", [this] { copyBlankPage(); }));
    t.push_back(ToolItem::line());
    t.push_back(ToolItem::item(L"Move to Trash", [this] { trashOpenEntry(); }));
    return t;
}

void StudioApp::entryKindChoice() {
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
}

void StudioApp::entryKindOfChoice() {
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
}

void StudioApp::entryFacetChoice(const std::string& key) {
    const Facet* f = findFacet(key);
    if (!f) return;
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
}

void StudioApp::entrySpoilerChoice(const std::string& key) {
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
}

void StudioApp::entryNoteBookChoice() {
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
}

void StudioApp::trashOpenEntry() {
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

// 0.60: a picture dragged onto the main picture becomes the main one. The old main stays among the pictures.
void StudioApp::makeMainPicture(size_t index) {
    const Entry* e = lib_->entry(wb_.entry);
    if (!e || index >= e->gallery.size() || e->gallery[index].blob == e->imageBlob) return;
    Entry u = *e;
    u.imageBlob = e->gallery[index].blob;
    // The two swap places too, so the main picture stays where the old one was.
    for (size_t k = 0; k < u.gallery.size(); ++k)
        if (u.gallery[k].blob == e->imageBlob) { std::swap(u.gallery[k], u.gallery[index]); break; }
    lib_->updateEntry(u);
    if (wb_.picture >= 0) { wb_.picture = -1; loadedFocus_ = {Focus::None, "-", ""}; }
    flash(L"It's now the entry's main picture.", 4);
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
    if (x->kind == "place" || x->kind == "realm") { setPlaceIn(entryId, parent); return; }   // the places outline (0.48.3)
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

// A place moved inside another (or a realm), or to the top ("" ): its old "Located in" / "Part of" (and the other
// side's "Contains" / "Includes") go, and "Located in" the new one is made. Never inside itself or its own places.
void StudioApp::setPlaceIn(const Id& placeId, const Id& parent) {
    const Entry* x = lib_->entry(placeId);
    if (!x) return;
    if (!parent.empty()) {
        const Entry* p = lib_->entry(parent);
        if (!p || (p->kind != "place" && p->kind != "realm")) return;
        if (p->id == placeId) return;
        for (const Entry* up : placePath(*lib_, *p))
            if (up->id == placeId) { flash(L"A place can't go inside one of its own places.", 5); return; }
    }
    commitFields();
    for (const EntryLink& l : lib_->linksOf(placeId)) {
        const bool up = (l.from == placeId && (l.label == "Located in" || l.label == "Part of")) ||
                        (l.to == placeId && (l.label == "Contains" || l.label == "Includes"));
        if (!up) continue;
        const Entry* o = lib_->entry(l.from == placeId ? l.to : l.from);
        if (o && (o->kind == "place" || o->kind == "realm")) lib_->removeLink(l.from, l.to, l.label);
    }
    if (!parent.empty()) lib_->addLink(placeId, parent, "Located in");
    if (const Entry* p = lib_->entry(parent)) {
        wb_.collapsed.erase(p->id);
        flash(w(x->name) + L" is now in " + w(p->name) + L".", 4);
    } else flash(w(x->name) + L" is now a place of its own (in nowhere else).", 4);
    invalidate();
}

// Places as an outline: each with the places inside it beneath, indented; realms come first, holding the places in them.
// Drag a place onto another to put it inside, or onto the strip at the foot to take it out of everything.
float StudioApp::drawPlacesOutline(const Rect& m, float y) {
    const auto all = lib_->entriesOf(wb_.world, wb_.book);
    std::map<Id, std::vector<const Entry*>> kids;
    std::vector<const Entry*> roots;
    for (const Entry* e : all) {
        if (e->kind != "place" && e->kind != "realm") continue;
        const auto path = placePath(*lib_, *e);
        if (path.empty()) roots.push_back(e); else kids[path.back()->id].push_back(e);
    }
    auto byName = [](const Entry* a, const Entry* b) { return a->name < b->name; };
    std::stable_sort(roots.begin(), roots.end(), [&](const Entry* a, const Entry* b) {
        if ((a->kind == "realm") != (b->kind == "realm")) return a->kind == "realm";   // realms first
        return byName(a, b);
    });
    for (auto& [k, v] : kids) std::sort(v.begin(), v.end(), byName);
    std::function<int(const Entry*)> count = [&](const Entry* e) {
        int n = 0;
        if (auto it = kids.find(e->id); it != kids.end()) for (const Entry* c : it->second) n += 1 + count(c);
        return n;
    };
    gfx_.text(L"Drag a place onto another to put it inside; onto the strip at the foot to take it out.", {m.x, y, m.w, 20}, Font::Small, pal_.inkFaint,
              {Align::Left, false, true});
    y += 30;
    const float rowH = 34;
    std::function<void(const Entry*, int)> row = [&](const Entry* e, int depth) {
        if (depth > 10) return;
        const int n = count(e);
        const Rect r{m.x + depth * 26.0f, y, m.w - depth * 26.0f, rowH - 4};
        const std::string id = "wb:tree:" + e->id;
        addHit(id, r, [this, eid = e->id] { openEntry(eid); });
        addDrop({Drop::KindOf, r, e->id, {}, false, false, {}});
        const float hv = hoverAmount(id);
        const cjui::Color kc = kindColor(e->kind, pal_.dark);
        gfx_.fillRounded(r, 6, pal_.surface.mix(pal_.sunken, 0.35f * hv));
        gfx_.strokeRounded(r, 6, pal_.rule.mix(kc, 0.4f * hv));
        gfx_.fill({r.x + 1, r.y + 6, 3, r.h - 12}, kc);
        float tx = r.x + 12;
        const bool open = !wb_.collapsed.count(e->id);
        if (n > 0) {
            const std::string tid = "wb:pltog:" + e->id;
            const Rect tb{tx - 2, r.y + 3, 22, r.h - 6};
            addHit(tid, tb, [this, eid = e->id] { if (!wb_.collapsed.erase(eid)) wb_.collapsed.insert(eid); invalidate(); }, false);
            const float cx = tb.x + 11, cy = tb.y + tb.h / 2;
            const cjui::Color cc = pal_.inkMuted.mix(pal_.ink, hoverAmount(tid));
            if (open) { gfx_.line(cx - 4, cy - 2, cx, cy + 2, cc, 1.6f); gfx_.line(cx, cy + 2, cx + 4, cy - 2, cc, 1.6f); }
            else { gfx_.line(cx - 2, cy - 4, cx + 2, cy, cc, 1.6f); gfx_.line(cx + 2, cy, cx - 2, cy + 4, cc, 1.6f); }
        }
        tx += 24;
        const std::wstring nm = w(e->name);
        gfx_.text(nm, {tx, r.y, r.w - (tx - r.x) - 180, r.h}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
        // Its type (a nation, a city) or "realm", and how many places are inside it.
        std::wstring right = e->kind == "realm" ? L"realm" : w(facetOf(*e, *findFacet("type")));
        for (auto& c : right) c = (wchar_t)towlower(c);
        if (n > 0) right += (right.empty() ? L"" : L"  \u00B7  ") + std::to_wstring(n) + (n == 1 ? L" inside" : L" inside");
        if (!e->mapBlob.empty()) right += (right.empty() ? L"" : L"  \u00B7  ") + std::wstring(L"map");
        gfx_.text(right, {r.right() - 180, r.y, 168, r.h}, Font::Small, pal_.inkFaint, {Align::Right, false, true, true});
        if (focused(id)) focusRing(r);
        y += rowH;
        if (open)
            if (auto it = kids.find(e->id); it != kids.end())
                for (const Entry* c : it->second) row(c, depth + 1);
    };
    for (const Entry* e : roots) row(e, 0);
    // Out of everything: a strip to drop on while dragging.
    y += 6;
    if (drag_.active && drag_.kind == DragState::Entry) {
        const Rect zone{m.x, y, m.w, 40};
        gfx_.strokeRounded(zone, 8, pal_.ribbon.withAlpha(0.6f), 1.4f);
        gfx_.text(L"Drop here to take it out of everything", zone, Font::Small, pal_.inkMuted, {Align::Center, false, false, true});
        addDrop({Drop::KindOf, zone, Id{}, {}, false, false, {}});
        y += 50;
    }
    return y + 20;
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
    // Worked out again only when the library has changed (0.59: it reads every entry's text).
    const std::string at = wb_.world + "|" + wb_.book + "|" + std::to_string(lib_->changesMade());
    if (at != wb_.kindSugKey) { wb_.kindSugKey = at; wb_.kindSug = lib_->kindSuggestions(wb_.world, wb_.book); }
    for (auto& s : wb_.kindSug) {
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
        // Quiet (0.65): a plain line with a small dot, no coloured box.
        const float hv = hoverAmount(id);
        if (hv > 0.01f) gfx_.fillRounded(note, 7, pal_.ink.withAlpha(0.035f * hv));
        gfx_.circle(note.x + 12, note.y + note.h / 2, 3, pal_.demo, true);
        const std::wstring t = list.size() == 1 ? L"1 entry may be filed under the wrong kind." : std::to_wstring(list.size()) + L" entries may be filed under the wrong kind.";
        const float tw = gfx_.measureWidth(t, Font::Small);
        gfx_.text(t, {note.x + 24, note.y, note.w - 140, note.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        gfx_.text(L"Check \u203A", {note.x + 24 + tw + 10, note.y, 120, note.h}, Font::SmallStrong, pal_.ink.mix(pal_.inkMuted, 0.4f - 0.4f * hv),
                  {Align::Left, false, false, true});
        if (focused(id)) focusRing(note);
        return note.bottom() + 12;
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
        drawPortrait(*e, pic, 50);
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
                const auto& hows = role == 1 ? fam.partnerHow : fam.how;
                if (role != 4)
                    if (auto it = hows.find(o); it != hows.end())
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
        group("fp", L"FORMER PARTNERS", fam.formerPartners, 4);
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
    static const std::vector<KinKind> siblings{{"", L"Brother or sister"}, {"twin", L"Twin"}, {"triplet", L"Triplet"},
                                               {"half", L"Half-brother or half-sister"},
                                               {"step", L"Stepbrother or stepsister"}, {"adoptive", L"Adoptive brother or sister"},
                                               {"foster", L"Foster brother or sister"}};
    static const std::vector<KinKind> children{{"", L"Child"}, {"adoptive", L"Adopted child"}, {"step", L"Stepchild"}, {"foster", L"Foster child"}};
    // Partners (0.57): together, married, engaged, or no longer together. Role 4, former partners, has the same kinds.
    static const std::vector<KinKind> partners{{"", L"Partner"}, {"married", L"Married"}, {"engaged", L"Engaged"},
                                               {"former", L"Former partner (no longer together)"}};
    return role == 0 ? parents : role == 2 ? siblings : role == 1 || role == 4 ? partners : children;
}
// The link from `who` that says `other` is their kin of this role and kind.
struct KinLink { Id from, to; std::string label; };
KinLink kinLink(const Id& who, const Id& other, int role, const std::string& how) {
    if (role == 1 || role == 4) {
        const std::string l = how == "married" ? "Married to" : how == "engaged" ? "Engaged to" : how == "former" ? "Former partner of" : "Partner of";
        return {who, other, l};
    }
    if (role == 2) {
        const std::string l = how == "twin" ? "Twin of" : how == "triplet" ? "Triplet of" : how == "half" ? "Half-sibling of" : how == "step" ? "Step-sibling of"
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
    if (role == 1) {   // partners: what a married or engaged one is (0.57)
        if (how == "married") return sex == 1 ? L"wife" : sex == 0 ? L"husband" : L"married";
        if (how == "engaged") return sex == 1 ? L"fianc\u00E9e" : sex == 0 ? L"fianc\u00E9" : L"engaged";
        return L"";
    }
    if (how == "twin") return std::wstring(L"twin ") + base;
    if (how == "triplet") return std::wstring(L"triplet ") + base;
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
    // Which kind of parent, partner, brother or sister, or child first (0.46; partners 0.57). A former partner is one.
    std::string how = relation == 4 ? "former" : "";
    if (relation != 4) {
        std::vector<cjui::MenuItem> kinds;
        const auto& ks = kinKinds(relation);
        for (size_t i = 0; i < ks.size(); ++i) kinds.push_back(cjui::MenuItem::item(ks[i].menu, (UINT)(1 + i)));
        const UINT k = cjui::showMenu(hwnd(), kinds, pt, pal_);
        if (!k) return;
        how = ks[k - 1].how;
    }
    const auto fam = lib_->familyOf(who);
    std::set<Id> already;
    for (auto* v : {&fam.parents, &fam.partners, &fam.siblings, &fam.children, &fam.formerPartners}) already.insert(v->begin(), v->end());
    // A creature's family are creatures; a character's, characters (0.57).
    const std::string kind = me->kind == "creature" ? "creature" : "character";
    std::vector<const Entry*> people;
    for (const Entry* o : lib_->entriesOf(wb_.world, wb_.book))
        if (o->kind == kind && o->id != who && !already.count(o->id)) people.push_back(o);
    static const wchar_t* what[] = {L"parent", L"partner", L"brother or sister", L"child", L"former partner"};
    std::wstring role = what[relation];
    if (how == "former") role = L"former partner";
    else if (relation == 1 && how == "married") role = L"husband or wife";
    else if (relation == 1 && how == "engaged") role = L"fianc\u00E9 or fianc\u00E9e";
    else if (relation != 1 && relation != 4 && !how.empty())
        for (auto& kk : kinKinds(relation)) if (kk.how == how) { role = kk.menu; for (auto& c : role) c = (wchar_t)towlower(c); }
    std::vector<cjui::MenuItem> menu{cjui::MenuItem::item(std::wstring(kind == "creature" ? L"New creature as " : L"New character as ") + w(me->name) + L"'s " + role + L"…", 1)};
    if (!people.empty()) menu.push_back(cjui::MenuItem::sep());
    for (size_t i = 0; i < people.size() && i < 300; ++i) menu.push_back(cjui::MenuItem::item(w(people[i]->name), (UINT)(10 + i)));
    const UINT ch = cjui::showMenu(hwnd(), menu, pt, pal_);
    if (!ch) return;
    commitFields();
    Id other;
    if (ch == 1) {
        other = lib_->createEntry(wb_.world, wb_.book, kind, kind == "creature" ? "New creature" : "New character");
        if (other.empty()) return;
    } else {
        other = people[ch - 10]->id;
    }
    bool ok = false;
    { const KinLink kl = kinLink(who, other, relation, how); ok = lib_->addLink(kl.from, kl.to, kl.label); }
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
    std::string cur = role == 4 ? "former" : "";
    if (role == 1) { if (auto it = fam.partnerHow.find(other); it != fam.partnerHow.end()) cur = it->second; }
    else if (role != 4) { if (auto it = fam.how.find(other); it != fam.how.end()) cur = it->second; }
    std::vector<cjui::MenuItem> menu{cjui::MenuItem::item(L"Open " + w(o->name) + L"'s page", 1)};
    {
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
    const bool partnerRole = role == 1 || role == 4;
    for (const EntryLink& l : lib_->linksOf(who)) {
        const Id x = l.from == who ? l.to : l.from;
        if (x != other || !isFamilyRelation(l.label)) continue;
        const bool partnerLink = l.label == "Partner of" || l.label == "Married to" || l.label == "Engaged to" || l.label == "Former partner of";
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
    wb_.anchors.push_back({e.kind == "realm" ? L"In this realm" : L"In this place", y - m.y});
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
    wb_.anchors.push_back({L"Family", y - m.y});
    gfx_.text(L"FAMILY", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    const bool any = !fam.parents.empty() || !fam.partners.empty() || !fam.siblings.empty() || !fam.children.empty() || !fam.formerPartners.empty();
    if (any) {
        const std::wstring t = L"See the family tree \u203A";
        linkText("wb:famtree", m.right() - gfx_.measureWidth(t, Font::Small), y - 2, t, [this, id = e.id] { openFamilyTree(id); });
    }
    y += 28;
    struct Row { const wchar_t* label; const std::vector<Id>* ids; int rel; const wchar_t* add; };
    const Row rows[] = {{L"Parents", &fam.parents, 0, L"+ Parent"}, {L"Partners", &fam.partners, 1, L"+ Partner"},
                        {L"Brothers and sisters", &fam.siblings, 2, L"+ Brother or sister"}, {L"Children", &fam.children, 3, L"+ Child"},
                        {L"Former partners", &fam.formerPartners, 4, L"+ Former partner"}};
    for (const Row& row : rows) {
        gfx_.text(row.label, {m.x, y, 160, 28}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        float lx = m.x + 170;
        for (const Id& pid : *row.ids) {
            const Entry* o = lib_->entry(pid);
            if (!o) continue;
            // The name, and what they are when it isn't plain (twin sister, stepfather, adopted son).
            std::string how;
            const auto& hows = row.rel == 1 ? fam.partnerHow : fam.how;
            if (auto it = hows.find(pid); it != hows.end() && row.rel != 4) how = it->second;
            const std::wstring nm = w(o->name), kw = row.rel == 4 ? L"" : kinWord(pid, row.rel, how);
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
    for (auto& sb : lay.siblings) if (sb.how == "twin" || sb.how == "triplet") { twinPairs.insert({sb.a, sb.b}); twinPairs.insert({sb.b, sb.a}); }
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
            { float sum = kx; int n = 1; for (const Id& o : kids) if (o != k && twinPairs.count({k, o})) { sum += at[o].x + at[o].w / 2; ++n; } kx = sum / n; }
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
            {   // triplets from one point too, under the middle of the three (0.57)
                float sum = kx; int n = 1;
                for (const Id& o : kids) if (o != k && twinPairs.count({k, o})) { sum += at[o].x + at[o].w / 2; ++n; }
                fromX = sum / n;
            }
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
        const bool plain = sb.how.empty() || sb.how == "twin" || sb.how == "triplet" || sb.how == "half";
        if (plain) {
            gfx_.line(ax, ra.y, ax, topY, c, t); gfx_.line(ax, topY, bx, topY, c, t); gfx_.line(bx, topY, bx, rb.y, c, t);
        } else {
            dotted(ax, ra.y, ax, topY, c, t); dotted(ax, topY, bx, topY, c, t); dotted(bx, topY, bx, rb.y, c, t);
        }
        const std::wstring tag = sb.how == "twin" ? L"twins" : sb.how == "triplet" ? L"triplets" : sb.how == "half" ? L"half-siblings" : sb.how == "step" ? L"step-siblings"
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
        drawPortrait(*x, pic, ps / 2);   // round, like the rest of the bible's portraits (0.87)
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
            const std::wstring how = p.joined == "married" ? L"Married in" : p.joined == "engaged" ? L"Engaged" : p.joined == "partner" ? L"Partner"
                                   : p.joined == "former" ? L"Ex-partner" : L"Other parent";
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
    wb_.anchors.push_back({L"Map", y - m.y});
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
            if (const Entry* x = lib_->entry(id)) {
                const Entry before = *x;
                Entry u = *x; u.mapBlob.clear(); u.pins.clear(); lib_->updateEntry(u);
                flashUndo(L"Removed the map. The picture stays in Artwork.", [this, before] { lib_->updateEntry(before); invalidate(); });
            }
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
    // Places inside other pinned places (0.87): joined to the place they're in by a dotted line and drawn smaller, so the
    // Located in chain reads on the map (Akunda inside Highcliff inside the realm).
    std::map<Id, const P*> pinAt;
    for (auto& p : ps) pinAt[p.id] = &p;
    std::map<Id, Id> inside;   // pin -> the nearest pinned place it's in
    std::map<Id, std::wstring> chain;
    for (auto& p : ps) {
        std::wstring ch;
        const auto path = placePath(*lib_, *p.o);
        for (const Entry* up : path) ch += (ch.empty() ? L"" : L" \u203A ") + w(up->name);
        for (auto it = path.rbegin(); it != path.rend(); ++it)   // the nearest pinned place it's in
            if ((*it)->id != p.id && pinAt.count((*it)->id)) { inside[p.id] = (*it)->id; break; }
        chain[p.id] = ch;
    }
    for (auto& [kid, par] : inside) {
        const P* a = pinAt[kid];
        const P* b = pinAt[par];
        const float dx = b->x - a->x, dy = b->y - a->y, len = std::sqrt(dx * dx + dy * dy);
        for (float t = 0; t < len; t += 7) {
            const float t2 = std::min(len, t + 3.5f);
            gfx_.line(a->x + dx * t / len, a->y + dy * t / len, a->x + dx * t2 / len, a->y + dy * t2 / len, pal_.ink.withAlpha(0.55f), 1.4f);
        }
    }
    std::stable_partition(ps.begin(), ps.end(), [&](const P& p) { return p.id == hot; });   // its label first
    for (auto& p : ps) {
        const cjui::Color kc = kindColor(p.o->kind, pal_.dark);
        const std::string id = "wb:mvp:" + p.id;
        addHit(id, {p.x - 12, p.y - 30, 24, 32}, [] {}, false);
        tooltip(id, (chain[p.id].empty() ? w(p.o->name) : w(p.o->name) + L" (in " + chain[p.id] + L")") +
                        (p.o->mapBlob.empty() ? L"" : L", with a map of its own") + L": drag to move, click for more");
        const float hv = hoverAmount(id);
        const std::wstring nm = w(p.o->name);
        const float lw = gfx_.measureWidth(nm, Font::Small) + 16;
        Rect label{p.x + 15, p.y - 32, lw, 22};
        if (label.right() > board.right() - 4) label.x = p.x - 15 - lw;
        bool clear = true;
        for (auto& q : placed)
            if (label.x < q.right() + 3 && q.x - 3 < label.right() && label.y < q.bottom() + 3 && q.y - 3 < label.bottom()) clear = false;
        if (clear || p.id == hot) {
            placed.push_back(label);
            gfx_.fillRounded(label, 11, pal_.paper.withAlpha(0.9f + 0.1f * hv));
            gfx_.strokeRounded(label, 11, kc.withAlpha(0.55f + 0.45f * hv));
            gfx_.text(nm, label, Font::Small, pal_.ink, {Align::Center, false, true, true});
        }
        // A pin with the place's symbol in its head (0.87): its picture or chosen symbol, else its kind's; smaller when it's
        // inside another pinned place.
        const bool nested = inside.count(p.id) > 0;
        const float hr = (nested ? 7.5f : 10.5f) + 1.5f * hv, hy = p.y - hr - 10;
        gfx_.fillTriangle(p.x - hr * 0.62f, hy + hr * 0.6f, p.x + hr * 0.62f, hy + hr * 0.6f, p.x, p.y, kc);
        gfx_.circle(p.x, hy, hr + 1.5f, pal_.paper, true);
        if (!p.o->imageBlob.empty() || !p.o->symbol.empty()) drawPortrait(*p.o, {p.x - hr, hy - hr, hr * 2, hr * 2}, hr);
        else {
            gfx_.circle(p.x, hy, hr, kc, true);
            cjui::drawIcon(gfx_, entryKindIcon(p.o->kind), {p.x - hr * 0.62f, hy - hr * 0.62f, hr * 1.24f, hr * 1.24f}, pal_.paper, 1.3f);
        }
        gfx_.circle(p.x, p.y, 2, pal_.ink, true);
        if (!p.o->mapBlob.empty()) gfx_.circle(p.x, hy, hr + 4, kc.withAlpha(0.7f), false, 1.4f);   // a ring: it has a map of its own
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
                    L"The bible against itself and the books. Mark anything that's fine and it isn't shown again.");
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
            const std::wstring said = !f->note.empty() ? w(f->note)
                                      : f->inBible ? L"The bible says both “" + w(f->bible) + L"” and “" + w(f->other) + L"”."
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
    // ---- Time between chapters (0.86).
    {
        std::vector<const review::GapIssue*> list;
        for (auto& g : wb_.continuity.gaps) if (!ok.count(g.key())) list.push_back(&g);
        heading(L"TIME BETWEEN CHAPTERS", (int)list.size());
        if (list.empty())
            y += gfx_.text(L"None found. Studio reads each chapter's opening for how long after the one before it it is (“three days later”, "
                           L"“over a year later”, “the next morning”) and checks it against Planning's timeline.",
                           {m.x, y, cw, 40}, Font::Small, pal_.inkFaint) + 18;
        for (auto* g : list) {
            const studio::Book* bk = lib_->book(g->bookId);
            const ManuscriptNode* nd = lib_->node(g->nodeId);
            const ManuscriptNode* pv = lib_->node(g->prevId);
            if (!bk || !nd) continue;
            gfx_.text(w(bk->title) + L" › " + w(nd->title), {m.x, y, cw, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            y += 24;
            y += gfx_.text(L"It opens “" + w(g->said) + L"”, but the timeline puts it " + w(g->actual) + L" after " +
                               (pv ? L"“" + w(pv->title) + L"”" : std::wstring(L"the chapter before")) + L".",
                           {m.x, y, cw, 40}, Font::Small, pal_.inkMuted) + 4;
            if (!g->snippet.empty()) y += gfx_.text(L"“" + w(g->snippet) + L"”", {m.x, y, cw, 60}, Font::Small, pal_.ink) + 6;
            float ax = m.x;
            auto act = [&](const std::string& id, const std::wstring& t, std::function<void()> fn) {
                linkText(id, ax, y, t, std::move(fn));
                ax += gfx_.measureWidth(t, Font::Small) + 26;
            };
            const std::string k = g->key();
            act("ck:g:show:" + k, L"Show it", [this, b = g->bookId, n = g->nodeId, s = g->said] { showInWriting(b, n, {s}); });
            act("ck:g:plan:" + k, L"Open the timeline", [this, b = g->bookId, n = g->nodeId] {
                openPlanning(b);
                pl_.view = 1;
                selectCard(n);
            });
            act("ck:g:ok:" + k, L"It's fine", [this, k] { checkIsFine(k); });
            y += 36;
            gfx_.line(m.x, y - 6.5f, m.right(), y - 6.5f, pal_.rule.withAlpha(0.5f));
        }
        y += 18;
    }
    // ---- Names written with the wrong capitals (0.86).
    {
        std::vector<const review::CapsIssue*> list;
        for (auto& ci : wb_.continuity.capitals) if (!ok.count("caps|" + ci.entry + "|" + ci.name)) list.push_back(&ci);
        heading(L"BIBLE NAMES WITH THE WRONG CAPITALS", (int)list.size());
        if (list.empty())
            y += gfx_.text(L"None found: every bible name in the text is written with its capitals.", {m.x, y, cw, 20}, Font::Small, pal_.inkFaint) + 18;
        for (auto* ci : list) {
            gfx_.text(L"“" + w(ci->written) + L"” for " + w(ci->name), {m.x, y, cw, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            y += 24;
            const studio::Book* bk = lib_->book(ci->bookId);
            const ManuscriptNode* nd = lib_->node(ci->nodeId);
            std::wstring where = std::to_wstring(ci->count) + (ci->count == 1 ? L" time" : L" times") + L"; first in " +
                                 (bk ? w(bk->title) : std::wstring()) + (nd ? L" › " + w(nd->title) : std::wstring());
            y += gfx_.text(where, {m.x, y, cw, 20}, Font::Small, pal_.inkMuted) + 4;
            if (!ci->snippet.empty()) y += gfx_.text(L"“" + w(ci->snippet) + L"”", {m.x, y, cw, 60}, Font::Small, pal_.ink) + 6;
            float ax = m.x;
            auto act = [&](const std::string& id, const std::wstring& t, std::function<void()> fn) {
                linkText(id, ax, y, t, std::move(fn));
                ax += gfx_.measureWidth(t, Font::Small) + 26;
            };
            const std::string k = "caps|" + ci->entry + "|" + ci->name;
            act("ck:c:show:" + k, L"Show it", [this, b = ci->bookId, n = ci->nodeId, s = ci->written] { showInWriting(b, n, {s}); });
            act("ck:c:fix:" + k, L"Change to " + w(ci->name) + L" everywhere", [this, from = ci->written, to = ci->name] {
                int n = 0;
                for (const Id& b : booksInReadingOrder(*lib_, wb_.world, wb_.book)) n += review::replaceName(*lib_, b, from, to);
                flash(L"Changed " + std::to_wstring(n) + (n == 1 ? L" use" : L" uses") + L" of “" + w(from) + L"” to “" + w(to) +
                      L"”. Each chapter's text before the change is kept in its saved versions.");
                wb_.checksKey.clear();
                invalidate();
            });
            act("ck:c:open:" + k, L"Open " + w(ci->name), [this, id = ci->entry] { openEntry(id); });
            act("ck:c:ok:" + k, L"It's fine", [this, k] { checkIsFine(k); });
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
            const float lw2 = gfx_.measureWidth(nm, Font::Small) + 56;
            if (lx + lw2 > m.right() && lx > m.x) { lx = m.x; y += 36; }
            const Rect chip{lx, y, lw2, 28};
            const std::string id = "ck:un:" + e->id;
            addHit(id, {chip.x, chip.y, chip.w - 26, chip.h}, [this, eid = e->id] { openEntry(eid); });
            addHit(id + ":x", {chip.right() - 26, chip.y, 26, chip.h}, [this, eid = e->id] { checkIsFine("unused|" + eid); }, false);
            const float hv = hoverAmount(id), xv = hoverAmount(id + ":x");
            const cjui::Color kc = kindColor(e->kind, pal_.dark);
            gfx_.fillRounded(chip, 14, pal_.surface.mix(pal_.sunken, 0.4f * hv));
            gfx_.strokeRounded(chip, 14, pal_.rule.mix(kc, 0.4f + 0.6f * hv));
            drawKindMark(e->kind, {chip.x + 6, chip.y + 6, 16, 16});   // its kind's symbol (0.87)
            gfx_.text(nm, {chip.x + 26, chip.y, chip.w - 52, chip.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
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
                        L" with how they're said.");
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

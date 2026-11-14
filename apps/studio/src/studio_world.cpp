// The Worldbuilding workspace: a world's (or an independent book's) bible of
// characters, places, groups, things, lore and events. The sidebar filters by
// kind; the middle shows the entries as cards, or one entry with its details,
// relationships and where it appears in the manuscripts; the right-hand panel
// edits it.
#include "studio_app.hpp"
#include "cjui/menu.hpp"

#include <algorithm>
#include <cmath>
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
                           {"event", "#C9476A", "#F07C98"}};
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
        {"race", L"Race", "Race", {"race", "species", "people", "kin"},
         {L"Human", L"Elf", L"Dwarf", L"Halfling", L"Orc", L"Goblin", L"Giant", L"Dragon", L"Fae", L"Merfolk", L"Vampire",
          L"Werewolf", L"Undead", L"Spirit", L"Demon", L"Angel", L"Android or robot", L"Alien", L"Beast", L"Other"}, true},
        {"realm", L"Realm or plane", "Realm", {"realm", "plane", "world of origin", "realm/plane"},
         {L"Mortal world", L"Heaven", L"Hell", L"Underworld", L"Purgatory", L"Fae realm", L"Spirit world", L"Dream world",
          L"Astral plane", L"Elemental plane", L"The Void", L"Other"}, true},
        {"type", L"Type of place", "Type", {"type", "kind of place", "place type"},
         {L"City", L"Town", L"Village", L"Kingdom or country", L"Region", L"Castle or fortress", L"Building", L"Room", L"Forest",
          L"Mountain", L"River or lake", L"Sea or ocean", L"Island", L"Desert", L"Cave or dungeon", L"Ruin", L"Road or path",
          L"Planet", L"Other"}, false},
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
static std::vector<Id> booksInReadingOrder(const Library& lib, const Id& world, const Id& book) {
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
    const auto& choices = relationsFor(e->kind);
    std::vector<std::wstring> labels;
    for (auto& r : choices) labels.push_back(w(r.label));
    const int li = popupAt(hwnd(), labels);
    if (li < 0) return;
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
        const bool sel = wb_.kind == kind && wb_.facet.empty() && wb_.entry.empty() && wb_.board.empty();
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        if (!kind.empty()) gfx_.circle(tx0 + 4, rr.y + rr.h / 2, 3.5f, kindColor(kind, pal_.dark), true);
        const float lx = kind.empty() ? tx0 : tx0 + 16;
        gfx_.text(label, {lx, rr.y, cw - 30 - (lx - tx0), rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(std::to_wstring(count), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
        if (focused(id)) focusRing(rr);
        y += 32;
    };
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
    y += 20;
    // Image boards.
    gfx_.text(L"IMAGE BOARDS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    linkText("side:wb:newboard", tx0 + cw - gfx_.measureWidth(L"+ New", Font::Small), y - 2, L"+ New", [this] { newBoard(); });
    y += 24;
    {
        const json bj = boardsData();
        for (auto& bd : bj["boards"]) {
            const std::string bid = bd.value("id", "");
            const std::string id = "side:wb:board:" + bid;
            const Rect rr{x, y, wd, 30};
            addHit(id, rr, [this, bid] {
                commitFields();
                wb_.board = bid;
                wb_.item.clear();
                wb_.entry.clear();
                wb_.scroll = 0;
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
        if (bj["boards"].empty()) {
            y += gfx_.text(L"Pictures for this world: characters, maps, places, moods.", {tx0, y, cw, 40}, Font::Small, pal_.inkFaint) + 6;
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
    wb_.sideContent = y + wb_.sideScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawWorldPage(const Rect& r) {
    wb_.scopeEntries = lib_->entriesOf(wb_.world, wb_.book);
    if (!wb_.board.empty()) { drawBoard(r); return; }
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 40 : 28;
    const float cw = std::min(r.w - pad * 2, 900.0f);
    const Rect m{r.x + pad, r.y + 26 - wb_.scroll, cw, 0};
    float y = m.y;
    const Entry* e = wb_.entry.empty() ? nullptr : lib_->entry(wb_.entry);
    if (e && e->deleted) { e = nullptr; wb_.entry.clear(); }

    if (!e) {
        // ---- All entries (or one kind), as cards.
        std::wstring scopeName;
        if (const World* wo = lib_->world(wb_.world)) scopeName = w(wo->name);
        else if (const studio::Book* b = lib_->book(wb_.book)) scopeName = w(b->title);
        std::vector<const Entry*> list;
        for (const Entry* x : lib_->entriesOf(wb_.world, wb_.book)) {
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
        if (!wb_.kind.empty()) sub += L" in " + scopeName;
        y += pageHeader(m, L"WORLDBUILDING", title, sub);
        if (wb_.kind == "character" && !list.empty()) {
            // How Characters are split within each book.
            gfx_.text(L"Group by", {m.x, y - 4, 70, 28}, Font::Small, pal_.inkMuted, {Align::Left, false, false, true});
            const int at = wb_.groupBy == "race" ? 1 : wb_.groupBy == "none" ? 2 : 0;
            segmented("wb:groupby", {m.x + 72, y - 4, 300, 28}, {L"Gender or sex", L"Race", L"Nothing"}, at, [this](int i) {
                wb_.groupBy = i == 1 ? "race" : i == 2 ? "none" : "gender";
                invalidate();
            });
            y += 40;
        }
        y = drawKindSuggestions(m, y);
        if (wb_.kind == "creature" && wb_.facet.empty()) {
            y = drawCreatureTree(m, y);
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

            const float gap = 16;
            const int cols = std::max(1, (int)((cw + gap) / (240 + gap)));
            const float cardW = (cw - gap * (cols - 1)) / cols, cardH = 96;
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
                gfx_.text((sign.empty() ? L"" : sign + L"  ") + subLabel + L"  \u00B7  " + std::to_wstring(subItems.size()), {m.x, y, cw, 20},
                          Font::SmallStrong, sc, {Align::Left, false, true});
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
                const Rect pic{cr.x + 14, cr.y + 12, 72, 72};
                if (x->imageBlob.empty() || !gfx_.drawImage(w(lib_->blobPath(x->imageBlob)), pic, true)) {
                    gfx_.fillRounded(pic, 36, kc.withAlpha(pal_.dark ? 0.22f : 0.16f));
                    gfx_.strokeRounded(pic.inset(0.75f), 35.25f, kc.withAlpha(0.45f), 1.5f);
                    gfx_.text(initials(x->name), pic, Font::Heading, kc.mix(pal_.ink, 0.35f), {Align::Center, false, false, true});
                }
                const float tx = pic.right() + 14, tw = cr.right() - 12 - tx;
                {
                    std::wstring label = upper(w(entryKindLabel(x->kind)));
                    for (const Facet* f : facetsFor(x->kind))
                        if (const std::string v = facetOf(*x, *f); !v.empty()) label += L"  \u00B7  " + upper(w(v));
                    gfx_.text(label, {tx, cr.y + 13, tw, 16}, Font::Label, kc, {Align::Left, false, true});
                }
                // A character's gender or sex: its sign in the card's corner, in the group's colour.
                if (x->kind == "character")
                    if (const std::wstring g = w(facetOf(*x, *findFacet("gender"))); !g.empty())
                        if (const std::wstring sg = genderSign(g); !sg.empty())
                            gfx_.text(sg, {cr.right() - 30, cr.y + 6, 22, 22}, Font::BodyStrong, genderColor(g, pal_), {Align::Center, false, false, true});
                // A place inside another: "in Akunda", and a mark per level.
                if (depth > 0)
                    if (auto pit = parentOf.find(x->id); pit != parentOf.end())
                        if (const Entry* par = lib_->entry(pit->second)) {
                            for (int d = 0; d < depth && d < 4; ++d) gfx_.circle(cr.right() - 14 - d * 9, cr.y + 14, 2.5f, kc.withAlpha(0.6f), true);
                            gfx_.text((x->kind == "creature" ? L"a kind of " : L"in ") + w(par->name), {tx, cr.y + 52, tw, 18}, Font::Small,
                                      kc.mix(pal_.inkMuted, 0.4f), {Align::Left, false, true});
                        }
                gfx_.text(w(x->name), {tx, cr.y + 30, tw, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
                if (!x->summary.empty()) gfx_.text(w(x->summary), {tx, cr.y + (depth > 0 ? 70 : 52), tw, depth > 0 ? 18 : 34}, Font::Small, pal_.inkMuted,
                                                   {Align::Left, false, true});
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
            const std::wstring label = w(mine ? l.label : reverseRelation(l.label));
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
    if (!(focus_ == loadedFocus_)) loadFields();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - wb_.panelScroll;
    const Entry* e = wb_.entry.empty() ? nullptr : lib_->entry(wb_.entry);
    if (e && !e->deleted) {
        panelHeading(c, y, L"ENTRY", true);
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
            panelChoice("wb:kindof", c, y, ch ? L"A creature? (a named one)" : L"A kind of",
                        parent ? w(parent->name) : ch ? L"No" : L"Nothing (a species of its own)", [this] {
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
                const std::wstring now = w(facetOf(*x, *f));
                int checked = now.empty() ? (int)n : -1;
                for (size_t i = 0; i < n; ++i) if (items[i] == now) checked = (int)i;
                const int i = popupAt(hwnd(), items, checked);
                if (i < 0 || i == checked) return;
                commitFields();
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
        y = panelField(c, y, L"Summary", FDescription, 70);
        y = panelImage(c, y, L"Picture", e->imageBlob, [this] { chooseImage(Focus::Entry, wb_.entry, 0); }, [this] {
            if (const Entry* x = lib_->entry(wb_.entry)) { Entry u = *x; u.imageBlob.clear(); lib_->updateEntry(u); }
        }, false);
        y = panelField(c, y, L"Details (one per line, like Age: 34)", FSynopsis, 120);
        y = panelField(c, y, L"Notes", FNotes, 180);
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
        panelHeading(c, y, L"WORLDBUILDING", true);
        y += gfx_.text(L"Entries belong to a world, so every book in it shares them. An independent book has its own.\n\n"
                       L"“Appears in” searches the manuscripts for an entry's name and its other names, as whole words.",
                       {c.x, y, c.w, 240}, Font::Small, pal_.inkMuted) + 14;
        panelHeading(c, y, L"KINDS", false);
        for (auto& k : entryKinds()) {
            static const std::map<std::string, const wchar_t*> what = {
                {"character", L"The people in the story."}, {"race", L"Peoples and species: elves, dwarves, the Fluxians."}, {"realm", L"Worlds and planes: Heaven, Hell, the spirit world."}, {"creature", L"Beasts, monsters, animals, species."}, {"place", L"Countries, towns, buildings, rooms."},
                {"group", L"Families, guilds, crews, armies, nations."}, {"thing", L"Objects, ships, weapons, heirlooms."},
                {"lore", L"History, magic, beliefs, languages, rules of the world."}, {"event", L"Battles, festivals, disasters, turning points."}};
            gfx_.text(w(entryKindPlural(k)), {c.x, y, c.w, 20}, Font::SmallStrong, pal_.ink, {Align::Left, false});
            y += 20;
            y += gfx_.text(what.at(k), {c.x, y, c.w, 40}, Font::Small, pal_.inkMuted) + 8;
        }
    }
    wb_.panelContent = y + wb_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
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
    const float rowH = 44;
    const Rect card{m.x, y, m.w, 40 + rowH * std::min<size_t>(list.size(), 6) + 8};
    gfx_.fillRounded(card, 6, pal_.surface);
    gfx_.strokeRounded(card, 6, pal_.demo.withAlpha(0.6f), 1);
    gfx_.text(L"CHECK THESE KINDS", {card.x + 18, card.y + 12, card.w - 36, 18}, Font::Label, pal_.inkMuted, {Align::Left, false});
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

// Creatures as an indented list: species, their breeds, and the named ones (characters) under them.
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
        } else if (!up.empty()) named[up].push_back(e);   // a named creature
    }
    std::function<int(const Id&)> under = [&](const Id& id) {
        int n = 0;
        for (const Entry* k : kids[id]) n += 1 + under(k->id);
        return n + (int)named[id].size();
    };
    if (drag_.active && drag_.kind == DragState::Entry) {
        // Dropping here (in place of the buttons, so nothing moves): a species of its own.
        const Rect zone{m.x, y - 4, m.w, 38};
        gfx_.fillRounded(zone, 6, pal_.surface);
        gfx_.strokeRounded(zone, 6, pal_.inkFaint, 1);
        gfx_.text(L"Drop here to make it a species of its own", zone, Font::Small, pal_.inkMuted, {Align::Center, false, false, true});
        addDrop({Drop::KindOf, zone, Id{}, {}, false, false, {}});
    } else {
        const std::wstring add = L"+ New species";
        button("wb:newspecies", {m.x, y, gfx_.measureWidth(add, Font::Small) + 28, 30}, add, [this] { newEntry("creature"); }, false);
        gfx_.text(L"Drag a kind onto another to put it under it. Right-click to change what kind of entry it is.",
                  {m.x + gfx_.measureWidth(add, Font::Small) + 44, y, m.w - 200, 30}, Font::Small, pal_.inkFaint, {Align::Left, false, true, true});
    }
    y += 46;
    if (roots.empty())
        return y + gfx_.text(L"No creatures yet. Add a species (a Dragon, say), then its kinds under it (a Wyvern), and the named ones "
                             L"(Bluebolt) as characters that are a kind of it.", {m.x, y, std::min(m.w, 640.0f), 80}, Font::Body, pal_.inkMuted) + 16;
    std::function<void(const Entry*, int, bool, const Entry*)> row = [&](const Entry* e, int depth, bool individual, const Entry* parent) {
        // A card per entry, indented under its species or kind; the arrow at its left opens and closes the family.
        const float indent = depth * 34.0f, ch = 76;
        const Rect cr{m.x + 26 + indent, y, std::min(m.w - 26 - indent, 640.0f), ch};
        const std::string id = "wb:tree:" + e->id;
        const bool hasKids = !kids[e->id].empty() || !named[e->id].empty();
        const bool open = !wb_.collapsed.count(e->id);
        if (depth > 0) {   // the line joining it to its parent
            const float lx = m.x + 26 + indent - 20;
            gfx_.line(lx, y - 8, lx, y + ch / 2, pal_.rule, 1.2f);
            gfx_.line(lx, y + ch / 2, cr.x, y + ch / 2, pal_.rule, 1.2f);
        }
        if (hasKids) {
            const Rect chev{cr.x - 24, cr.y + ch / 2 - 10, 20, 20};
            addHit("wb:tree:fold:" + e->id, chev, [this, eid = e->id] {
                if (wb_.collapsed.count(eid)) wb_.collapsed.erase(eid); else wb_.collapsed.insert(eid);
                invalidate();
            }, false);
            gfx_.fillRounded(chev, 10, pal_.paper);
            const float cx = chev.x + 10, cy = chev.y + 10;
            if (open) { gfx_.line(cx - 4, cy - 2, cx, cy + 2.5f, pal_.inkMuted, 1.6f); gfx_.line(cx + 4, cy - 2, cx, cy + 2.5f, pal_.inkMuted, 1.6f); }
            else { gfx_.line(cx - 2, cy - 4, cx + 2.5f, cy, pal_.inkMuted, 1.6f); gfx_.line(cx - 2, cy + 4, cx + 2.5f, cy, pal_.inkMuted, 1.6f); }
        }
        addHit(id, cr, [this, eid = e->id] { openEntry(eid); });
        if (!individual) addDrop({Drop::KindOf, cr, e->id, {}, false, false, {}});
        const float hv = hoverAmount(id);
        softShadow(cr, 4, hv);
        const cjui::Color kc = kindColor(e->kind, pal_.dark);
        gfx_.fillRounded(cr, 4, pal_.surface);
        gfx_.strokeRounded(cr, 4, pal_.rule.mix(kc, 0.35f * hv), 1);
        gfx_.fill({cr.x + 1, cr.y + 4, 3, cr.h - 8}, kc);
        const Rect pic{cr.x + 14, cr.y + 10, 56, 56};
        if (e->imageBlob.empty() || !gfx_.drawImage(w(lib_->blobPath(e->imageBlob)), pic, true)) {
            gfx_.fillRounded(pic, 28, kc.withAlpha(pal_.dark ? 0.22f : 0.16f));
            gfx_.strokeRounded(pic.inset(0.75f), 27.25f, kc.withAlpha(0.45f), individual ? 2.0f : 1.5f);
            gfx_.text(initials(e->name), pic, Font::BodyStrong, kc.mix(pal_.ink, 0.35f), {Align::Center, false, false, true});
        }
        const float tx = pic.right() + 14, tw = cr.right() - 12 - tx;
        std::wstring label = individual ? L"NAMED " + upper(w(parent ? parent->name : std::string())) + L"  ·  " + upper(w(entryKindLabel(e->kind)))
                           : depth == 0 ? std::wstring(L"SPECIES") : L"KIND OF " + upper(w(parent ? parent->name : std::string()));
        gfx_.text(label, {tx, cr.y + 11, tw - 90, 16}, Font::Label, kc, {Align::Left, false, true});
        gfx_.text(w(e->name), {tx, cr.y + 28, tw - 90, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        if (!e->summary.empty()) gfx_.text(w(e->summary), {tx, cr.y + 50, tw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        if (!individual) {
            if (hv > 0.01f) {
                const std::wstring add = L"+ Add a kind";
                linkText("wb:tree:add:" + e->id, cr.right() - 14 - gfx_.measureWidth(add, Font::Small), cr.y + 10, add,
                         [this, eid = e->id] { addKindUnder(eid); });
            } else if (const int n = under(e->id)) {
                gfx_.text(std::to_wstring(n) + (n == 1 ? L" under it" : L" under it"), {cr.right() - 110, cr.y + 10, 96, 18}, Font::Small, pal_.inkFaint,
                          {Align::Right, false, false, true});
            }
        }
        if (focused(id)) focusRing(cr);
        y += ch + 10;
        if (!open) return;
        for (const Entry* k : kids[e->id]) row(k, depth + 1, false, e);
        for (const Entry* n : named[e->id]) row(n, depth + 1, true, e);
    };
    for (const Entry* r : roots) {
        row(r, 0, false, nullptr);
        y += 10;
    }
    return y + 20;
}

}  // namespace nrs

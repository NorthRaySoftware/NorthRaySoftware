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
    else if (kind == "creature") keys = {"ctype", "realm"};
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

// The place a place is in ("Located in" / "Part of" from it, or "Contains" / "Includes" to it), if any.
static Id parentPlace(const Library& lib, const Entry& e) {
    if (e.kind != "place") return {};
    for (const EntryLink& l : lib.linksOf(e.id)) {
        const bool up = (l.from == e.id && (l.label == "Located in" || l.label == "Part of")) ||
                        (l.to == e.id && (l.label == "Contains" || l.label == "Includes"));
        if (!up) continue;
        const Id other = l.from == e.id ? l.to : l.from;
        const Entry* o = lib.entry(other);
        if (o && !o->deleted && o->kind == "place") return other;
    }
    return {};
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
            if (!facetMatches(*x, wb_.facet)) continue;
            list.push_back(x);
        }
        std::wstring title = wb_.kind.empty() ? scopeName : w(entryKindPlural(wb_.kind));
        if (const size_t eq = wb_.facet.find('='); eq != std::string::npos) {
            const Facet* f = findFacet(wb_.facet.substr(0, eq));
            const std::string v = wb_.facet.substr(eq + 1);
            if (f) title += L": " + (v == "-" ? f->title + L" not set" : w(v));
        }
        std::wstring sub = std::to_wstring(list.size()) + (list.size() == 1 ? L" entry" : L" entries");
        if (!wb_.kind.empty()) sub += L" in " + scopeName;
        y += pageHeader(m, L"WORLDBUILDING", title, sub);
        if (list.empty()) {
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
            if (wb_.kind == "character" && wb_.facet.rfind("gender=", 0) != 0) {
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
                const std::wstring sign = genderSign(subLabel);
                gfx_.text((sign.empty() ? L"" : sign + L"  ") + subLabel + L"  \u00B7  " + std::to_wstring(subItems.size()), {m.x, y, cw, 20},
                          Font::SmallStrong, genderColor(subLabel, pal_), {Align::Left, false, true});
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
                            gfx_.text(L"in " + w(par->name), {tx, cr.y + 52, tw, 18}, Font::Small, kc.mix(pal_.inkMuted, 0.4f), {Align::Left, false, true});
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
        gfx_.text(L"RELATIONSHIPS", {m.x, y, cw, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        linkText("wb:addrel", m.right() - gfx_.measureWidth(L"+ Add a relationship", Font::Small), y - 2, L"+ Add a relationship",
                 [this] { addRelationship(); });
        y += 28;
        const auto links = lib_->linksOf(e->id);
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

}  // namespace nrs

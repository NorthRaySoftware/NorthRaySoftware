// The Planning workspace: a book's chapters and scenes as cards on a board, in
// manuscript order and grouped by part, with each one's status, synopsis,
// length against its target, viewpoint character and the world's entries that
// appear in it. The panel edits the selected card, or the book's word target.
#include "studio_app.hpp"
#include "studio/bible_export.hpp"
#include "studio/calendar.hpp"
#include "cjui/menu.hpp"
#include "studio/planning.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <nlohmann/json.hpp>

namespace nrs {

using nlohmann::json;
using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }

static int popupAtCursor(HWND owner, const std::vector<std::wstring>& items, int checked = -1) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i)
        AppendMenuW(m, MF_STRING | ((int)i == checked ? MF_CHECKED : 0), 1 + (UINT)i, items[i].c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = (int)cjui::trackMenu(m, pt, owner);
    DestroyMenu(m);
    return cmd - 1;
}

static std::wstring thousandsW(std::int64_t n) {
    std::wstring s = std::to_wstring(n < 0 ? -n : n), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) out += L',';
        out += s[i];
    }
    return (n < 0 ? L"-" : L"") + out;
}

// About as much synopsis as a card shows: cut at a word, with an ellipsis.
static std::wstring shortened(const std::wstring& s, size_t max) {
    std::wstring t;
    for (wchar_t c : s) t += (c == L'\r' || c == L'\n') ? L' ' : c;
    if (t.size() <= max) return t;
    size_t cut = t.rfind(L' ', max);
    if (cut == std::wstring::npos || cut < max / 2) cut = max;
    return t.substr(0, cut) + L"…";
}

static const NodeStatus kStatuses[] = {NodeStatus::Outline, NodeStatus::Draft, NodeStatus::Revised, NodeStatus::Final};

Color StudioApp::statusColour(NodeStatus s) const {
    switch (s) {
        case NodeStatus::Outline: return pal_.inkFaint;
        case NodeStatus::Draft: return pal_.demo;
        case NodeStatus::Revised: return pal_.ribbon;
        case NodeStatus::Final: return pal_.ink;
    }
    return pal_.inkFaint;
}

std::int64_t StudioApp::bookTarget(const Id& bookId) const {
    const json j = json::parse(lib_->setting("plan:" + bookId), nullptr, false);
    return j.is_object() ? std::max<std::int64_t>(0, j.value("targetWords", (std::int64_t)0)) : 0;
}
// A viewpoint character's colour (0.35), from their id so it never changes: one of ten that stay apart from each
// other and from the status colours, a little lighter in the dark theme.
Color StudioApp::povColour(const Id& id) const {
    // A colour chosen on its bible page (0.101) everywhere: lanes, cards coloured by viewpoint, icons (0.102).
    if (lib_)
        if (const Entry* e = lib_->entry(id); e && e->colour.size() == 7 && e->colour[0] == '#') return cjui::hexColor(e->colour.c_str());
    static const char* light[] = {"#3B7DD8", "#D2513A", "#2E9E6B", "#9B59B6", "#D48A1F", "#1F9CA8", "#C2417E", "#6C7A2E", "#5B5FD6", "#A0522D"};
    static const char* dark[] = {"#6FA6F0", "#F07C64", "#5CC995", "#C08EDA", "#F0B252", "#52C6D2", "#E77CB0", "#A6B85A", "#8C90F2", "#D08A5E"};
    std::uint32_t h = 2166136261u;
    for (unsigned char c : id) h = (h ^ c) * 16777619u;
    return cjui::hexColor((pal_.dark ? dark : light)[h % 10]);
}

void StudioApp::setBookTarget(const Id& bookId, std::int64_t words) {
    json j = json::parse(lib_->setting("plan:" + bookId), nullptr, false);
    if (!j.is_object()) j = json::object();
    j["targetWords"] = std::max<std::int64_t>(0, words);
    lib_->setSetting("plan:" + bookId, j.dump());
}

std::string StudioApp::planPref(const Id& book, const char* key, const std::string& fallback) const {
    const json j = json::parse(lib_->setting("plan:" + book), nullptr, false);
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}

void StudioApp::setPlanPref(const Id& book, const char* key, const std::string& value) {
    json j = json::parse(lib_->setting("plan:" + book), nullptr, false);
    if (!j.is_object()) j = json::object();
    j[key] = value;
    lib_->setSetting("plan:" + book, j.dump());
    invalidate();
}

// Whose eyes a chapter or scene is seen through: one of the world's characters.
void StudioApp::chooseViewpoint(const Id& nodeId) {
    const ManuscriptNode* x = lib_->node(nodeId);
    const studio::Book* bk = lib_->book(pl_.book);
    if (!x || !bk) return;
    std::vector<const Entry*> people;
    for (const Entry* e : bk->worldId.empty() ? lib_->entriesOf({}, bk->id) : lib_->entriesOf(bk->worldId))
        if (e->kind == "character") people.push_back(e);
    if (people.empty()) { flash(L"Add characters in Worldbuilding first; then choose whose eyes this is seen through.", 6); return; }
    std::vector<std::wstring> items{L"None"};
    int cur = 0;
    for (size_t i = 0; i < people.size(); ++i) {
        items.push_back(w(people[i]->name));
        if (people[i]->id == x->povId) cur = (int)i + 1;
    }
    const int i = popupAtCursor(hwnd(), items, cur);
    if (i < 0 || i == cur) return;
    commitFields();
    ManuscriptNode u = *lib_->node(nodeId);
    u.povId = i == 0 ? Id{} : people[i - 1]->id;
    lib_->updateNode(u);
    invalidate();
}

// Where a chapter or scene happens: one of the bible's places or realms (0.52).
void StudioApp::choosePlace(const Id& nodeId) {
    const studio::Book* bk = lib_->book(pl_.book);
    if (!lib_->node(nodeId) || !bk) return;
    std::vector<const Entry*> list;
    for (const Entry* e : bk->worldId.empty() ? lib_->entriesOf({}, bk->id) : lib_->entriesOf(bk->worldId))
        if (e->kind == "place" || e->kind == "realm") list.push_back(e);
    if (list.empty()) { flash(L"Add places in Worldbuilding first; then choose where this happens.", 6); return; }
    const auto places = plan::nodePlaces(*lib_, pl_.book);
    const auto had = places.find(nodeId);
    std::vector<std::wstring> items{L"None"};
    int cur = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        items.push_back(w(list[i]->name));
        if (had != places.end() && list[i]->id == had->second) cur = (int)i + 1;
    }
    const int i = popupAtCursor(hwnd(), items, cur);
    if (i < 0 || i == cur) return;
    commitFields();
    plan::setNodePlace(*lib_, pl_.book, nodeId, i == 0 ? Id{} : list[i - 1]->id);
    invalidate();
}

// Who's present in a chapter or scene (0.59): chosen here, not worked out from names in the text, so a character
// only mentioned isn't counted and one surname doesn't bring every Smith. The menu puts the characters (and
// creatures) the text names first, as suggestions.
void StudioApp::addToCast(const Id& nodeId) {
    const studio::Book* bk = lib_->book(pl_.book);
    if (!lib_->node(nodeId) || !bk) return;
    const auto casts = plan::nodeCast(*lib_, pl_.book);
    std::vector<Id> cur;
    if (auto it = casts.find(nodeId); it != casts.end()) cur = it->second;
    std::vector<const Entry*> named, others;
    std::set<Id> inText;
    if (auto it = pl_.appearances.find(nodeId); it != pl_.appearances.end()) inText.insert(it->second.begin(), it->second.end());
    for (const Entry* e : bk->worldId.empty() ? lib_->entriesOf({}, bk->id) : lib_->entriesOf(bk->worldId)) {
        if ((e->kind != "character" && e->kind != "creature") || std::find(cur.begin(), cur.end(), e->id) != cur.end()) continue;
        (inText.count(e->id) ? named : others).push_back(e);
    }
    // The text's names in the order they're most mentioned.
    if (auto it = pl_.appearances.find(nodeId); it != pl_.appearances.end())
        std::stable_sort(named.begin(), named.end(), [&](const Entry* a, const Entry* b) {
            const auto& v = it->second;
            return std::find(v.begin(), v.end(), a->id) < std::find(v.begin(), v.end(), b->id);
        });
    if (named.empty() && others.empty()) {
        flash(cur.empty() ? L"Add characters in Worldbuilding first; then choose who's here." : L"Everyone in the bible is already here.", 6);
        return;
    }
    std::vector<cjui::MenuItem> items;
    if (!named.empty()) {
        items.push_back(cjui::MenuItem::heading(L"Named in its text"));
        for (size_t i = 0; i < named.size(); ++i) items.push_back(cjui::MenuItem::item(w(named[i]->name), (UINT)(1 + i)));
        if (named.size() > 1) items.push_back(cjui::MenuItem::item(L"All of these", 5000));
    }
    if (!others.empty()) {
        items.push_back(cjui::MenuItem::heading(named.empty() ? L"Characters and creatures" : L"Everyone else"));
        for (size_t i = 0; i < others.size(); ++i)
            items.push_back(cjui::MenuItem::item(w(others[i]->name) + (others[i]->kind == "creature" ? L"  (creature)" : L""), (UINT)(1000 + i)));
    }
    POINT pt;
    GetCursorPos(&pt);
    const UINT id = cjui::showMenu(hwnd(), items, pt, pal_);
    if (id == 0) return;
    commitFields();
    if (id == 5000) for (const Entry* e : named) cur.push_back(e->id);
    else if (id >= 1000 && id - 1000 < others.size()) cur.push_back(others[id - 1000]->id);
    else if (id >= 1 && id - 1 < named.size()) cur.push_back(named[id - 1]->id);
    else return;
    plan::setNodeCast(*lib_, pl_.book, nodeId, cur);
    invalidate();
}

void StudioApp::cardCast(const Rect& c, float& y) {
    const ManuscriptNode* n = lib_->node(pl_.node);
    if (!n || n->kind == NodeKind::Part) return;
    panelHeading(c, y, L"WHO'S THERE", false);
    const auto casts = plan::nodeCast(*lib_, pl_.book);
    std::vector<Id> cur;
    if (auto it = casts.find(n->id); it != casts.end()) cur = it->second;
    const Id node = n->id;
    int shown = 0;
    for (const Id& eid : cur) {
        const Entry* e = lib_->entry(eid);
        if (!e || e->deleted) continue;
        ++shown;
        linkText("pl:cast:" + eid, c.x, y, w(e->name), [this, eid] {
            const Entry* x = lib_->entry(eid);
            if (!x) return;
            openWorldbuilding(x->worldId, x->bookId);
            openEntry(eid);
        });
        linkText("pl:castx:" + eid, c.right() - gfx_.measureWidth(L"Remove", Font::Small), y, L"Remove", [this, node, eid] {
            auto all = plan::nodeCast(*lib_, pl_.book);
            auto v = all[node];
            v.erase(std::remove(v.begin(), v.end(), eid), v.end());
            commitFields();
            plan::setNodeCast(*lib_, pl_.book, node, v);
            invalidate();
        });
        y += 24;
    }
    // A chapter also shows who its scenes have.
    if (n->kind == NodeKind::Chapter) {
        std::wstring fromScenes;
        int more = 0;
        for (const Id& eid : plan::castOf(*lib_, pl_.book, node)) {
            if (std::find(cur.begin(), cur.end(), eid) != cur.end()) continue;
            if (const Entry* e = lib_->entry(eid)) {
                if (++more <= 6) fromScenes += (fromScenes.empty() ? L"" : L", ") + w(e->name);
            }
        }
        if (more > 6) fromScenes += L" and " + std::to_wstring(more - 6) + L" more";
        if (!fromScenes.empty()) y += gfx_.text(L"In its scenes: " + fromScenes, {c.x, y, c.w, 60}, Font::Small, pal_.inkMuted) + 8;
    }
    if (shown == 0 && n->kind == NodeKind::Chapter && plan::castOf(*lib_, pl_.book, node).empty())
        y += gfx_.text(L"No one chosen yet. Until you choose, Capture's cast list goes by the names in the text.", {c.x, y, c.w, 60},
                       Font::Small, pal_.inkFaint) + 8;
    linkText("pl:castadd", c.x, y, L"+ Who's there", [this, node] { addToCast(node); });
    y += 32;
}

// ------------------------------------------------------------- opening

void StudioApp::openPlanning(const Id& bookId) {
    if (!lib_ || !lib_->book(bookId)) return;
    commitFields();
    if (ws_ == Workspace::Writing && editor_.ready()) editor_.send(R"({"type":"flush"})");
    if (pl_.book != bookId) {
        pl_ = {};
        pl_.book = bookId;
    }
    ws_ = Workspace::Planning;
    // Who appears where (one pass over the book's text).
    pl_.appearances = lib_->appearancesIn(bookId);
    const ManuscriptNode* n = lib_->node(pl_.node);
    if (n && !n->deleted && n->bookId == bookId) setFocus({Focus::Node, n->id, {}});
    else {
        pl_.node.clear();
        setFocus({Focus::Plan, bookId, {}});
    }
    invalidate();
}

void StudioApp::selectCard(const Id& nodeId) {
    commitFields();
    pl_.panelOpen = true;
    // A second click on the same card soon after: open it in Writing (0.49.2).
    static Id lastCard;
    static DWORD lastAt = 0;
    const DWORD now = GetTickCount();
    if (!nodeId.empty() && nodeId == lastCard && now - lastAt < 450 && pl_.node == nodeId) {
        lastCard.clear();
        if (const ManuscriptNode* n = lib_->node(nodeId); n && !n->deleted) { writingBook_ = pl_.book; currentNode_ = n->id; openWriting(pl_.book); return; }
    }
    lastCard = nodeId;
    lastAt = now;
    if (pl_.node != nodeId) pl_.cardEdit = -1;
    pl_.node = nodeId;
    setFocus(nodeId.empty() ? Focus{Focus::Plan, pl_.book, {}} : Focus{Focus::Node, nodeId, {}});
    invalidate();
}

// ---------------------------------------------------------------- drawing

// Planning's tools (0.69): what the sidebar's ALSO list and its + New thread had.
std::vector<StudioApp::ToolItem> StudioApp::planTools() {
    namespace tb = studio::toolbar;
    std::vector<ToolItem> t;
    t.push_back(ToolItem::item(L"Set the book's target…", [this] { selectCard({}); pl_.targetOpen = true; }).in("target", tb::Settings, cjui::Icon::Target));
    t.push_back(ToolItem::item(L"New plot thread", [this] {
        const ManuscriptNode* sel = lib_->node(pl_.node);
        newThread(sel && !sel->deleted && sel->kind != NodeKind::Part ? sel->id : Id{});
    }).in("thread", tb::Create, cjui::Icon::Thread));
    t.push_back(ToolItem::line());
    t.push_back(ToolItem::heading(L"Planning as text"));
    t.push_back(ToolItem::item(L"Import planning…", [this] { importPlanText(); }).in("import", tb::ImportExport, cjui::Icon::Import));
    t.push_back(ToolItem::item(L"Export the plan…", [this] { exportPlanText(); }).in("export", tb::ImportExport, cjui::Icon::Export));
    t.push_back(ToolItem::line());
    // The day the bible's ages are worked out at for this book (0.86): its first dated chapter, or one the writer picks.
    {
        const cal::Calendar cc = cal::calendarOfBook(*lib_, pl_.book);
        const cal::StorySpan sp = cal::storySpan(*lib_, pl_.book);
        if (!cc.empty() && sp.on) {
            std::vector<ToolItem> sub;
            const std::string chosen = cal::agesAsOfText(*lib_, pl_.book);
            sub.push_back(ToolItem::item(L"The first chapter with a date", [this] {
                cal::setAgesAsOf(*lib_, pl_.book, "");
                flash(L"Ages in the bible are worked out as of this book's first chapter with a date.");
            }, chosen.empty()));
            const nlohmann::json tl = nlohmann::json::parse(lib_->setting("timeline:" + pl_.book), nullptr, false);
            if (tl.is_object() && tl.contains("nodes") && tl["nodes"].is_object())
                for (const ManuscriptNode* n : lib_->manuscriptOf(pl_.book)) {
                    if (n->kind != NodeKind::Chapter || !tl["nodes"].contains(n->id)) continue;
                    const auto& v = tl["nodes"][n->id];
                    if (!v.is_object() || !v.contains("day") || !v["day"].is_number()) continue;
                    const std::string date = cal::formatDate(cc, cal::fromDay(cc, sp.start + (std::int64_t)std::floor(v["day"].get<double>())));
                    sub.push_back(ToolItem::item(L"As of “" + w(n->title) + L"” (" + w(date) + L")", [this, date] {
                        cal::setAgesAsOf(*lib_, pl_.book, date);
                        flash(L"Ages in the bible are worked out as of " + w(date) + L" for this book.");
                    }, chosen == date));
                }
            t.push_back(ToolItem::menu(L"Ages in the bible", std::move(sub)).in("ages", tb::Settings, cjui::Icon::Calendar));
            t.push_back(ToolItem::line());
        }
    }
    // Where the shared bible's quotes come from in this book (0.85).
    t.push_back(ToolItem::heading(L"Quotes in the shared bible"));
    {
        const bool on = bible::excerptsFromText(*lib_, pl_.book);
        t.push_back(ToolItem::item(L"Only from chapters Who's there or the viewpoint puts them in", [this] {
            bible::setExcerptsFromText(*lib_, pl_.book, false);
            flash(L"Quotes come only from chapters where Who's there or the viewpoint puts the character.");
        }, !on));
        t.push_back(ToolItem::item(L"Also from chapters naming them, where nobody is chosen", [this] {
            bible::setExcerptsFromText(*lib_, pl_.book, true);
            flash(L"Chapters with nobody in Who's there and no viewpoint now give quotes for the characters their text names.");
        }, on));
    }
    t.push_back(ToolItem::line());
    t.push_back(ToolItem::item(L"Write this book", [this] { openWriting(pl_.book); }).in("write", tb::Organise, cjui::Icon::Writing));
    t.push_back(ToolItem::item(L"The world's bible", [this] {
        if (const studio::Book* bk = lib_->book(pl_.book)) openWorldbuilding(bk->worldId, bk->worldId.empty() ? bk->id : Id{});
    }).in("bible", tb::Organise, cjui::Icon::World));
    return t;
}

// The top of every Planning view (0.69): the book, how far it's got, + Chapter and Plan tools, the views as tabs and
// the filters, then a line about the view. Its height.
float StudioApp::planHeader(const Rect& m, const std::wstring& sub) {
    const studio::Book* b = lib_->book(pl_.book);
    if (!b) return 0;
    float y = m.y;
    gfx_.text(L"PLANNING", {m.x, y, m.w, 16}, Font::Label, gold_, {Align::Left, false, true});
    // Tools (0.100): the toolbar under the tabs, or the Plan tools menu here when it's hidden.
    const bool bar = toolBarOn("plan");
    const float tbw = bar ? -10.0f : gfx_.measureWidth(L"Plan tools", Font::Body) + 44;
    const float nw = buttonWidth(L"+ Chapter");
    if (!bar) menuButton("pl:tools", m.right() - tbw, y + 12, L"Plan tools", [this] { return withBarItem("plan", planTools()); });
    button("pl:newch", {m.right() - tbw - 10 - nw, y + 12, nw, 32}, L"+ Chapter", [this] {
        const Id id = lib_->createNode(pl_.book, {}, NodeKind::Chapter);
        if (!id.empty()) selectCard(id);
    });
    y += 20;
    const float titleW = std::max(200.0f, m.w - tbw - nw - 320);
    const float th = gfx_.text(w(b->title), {m.x, y, titleW, 90}, Font::SerifTitle, pal_.ink);
    // How far it's got, beside the title when there's room.
    {
        const auto all = lib_->manuscriptOf(pl_.book);
        std::int64_t words = 0;
        for (auto* n : all) words += n->wordCount;
        const std::int64_t target = bookTarget(pl_.book);
        const float px = m.x + std::min(gfx_.measureWidth(w(b->title), Font::SerifTitle), titleW) + 28, pw = 240;
        const bool beside = px + pw < m.right() - tbw - nw - 30;
        const float bx = beside ? px : m.x, by = beside ? y + 8 : y + th + 4;
        const std::wstring line = target > 0 ? thousandsW(words) + L" of " + thousandsW(target) + L" words" : thousandsW(words) + L" words · no target yet";
        if (target > 0) {
            // A ring with the percentage, and the words beside it (0.87).
            drawProgressRing(bx + 15, by + 14, 15, (float)((double)words / target));
            gfx_.text(line, {bx + 38, by + 5, pw + 30, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        } else
            gfx_.text(line, {bx, by, pw + 60, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        addHit("pl:target", {bx - 4, by - 2, pw + 8, 32}, [this] { selectCard({}); pl_.targetOpen = true; invalidate(); });
        tooltip("pl:target", L"Set the book's target");
        y += beside ? th + 6 : th + 40;
    }
    // The views as tabs, the filters at the end of the row.
    {
        const auto all = lib_->manuscriptOf(pl_.book);
        std::vector<PageTab> tabs;
        static const wchar_t* names[] = {L"Corkboard", L"Timeline", L"Structure", L"Threads"};
        for (int v = 0; v < 4; ++v) {
            PageTab t;
            t.label = names[v];
            t.on = pl_.view == v;
            t.act = [this, v] { commitFields(); pl_.view = v; pl_.scroll = 0; invalidate(); };
            if (v == 3) t.count = (int)plan::threadsOf(*lib_, pl_.book).threads.size();
            tabs.push_back(t);
        }
        // Filters: status, viewpoint, thread.
        std::wstring stLabel = L"All statuses";
        if (pl_.status >= 0) stLabel = w(label((NodeStatus)pl_.status));
        std::wstring povLabel = L"Any viewpoint";
        if (pl_.pov == "-") povLabel = L"No viewpoint";
        else if (const Entry* e = pl_.pov.empty() ? nullptr : lib_->entry(pl_.pov)) povLabel = w(e->name);
        const plan::Threads th = plan::threadsOf(*lib_, pl_.book);
        std::wstring thLabel = L"Any thread";
        for (const plan::Thread& t : th.threads) if (t.id == pl_.thread) thLabel = w(t.name);
        auto chipW = [&](const std::wstring& l) { return gfx_.measureWidth(l, Font::Body) + 44; };
        const bool showThreads = !th.threads.empty();
        const float fw = chipW(stLabel) + 8 + chipW(povLabel) + (showThreads ? 8 + chipW(thLabel) : 0);
        const bool filtersBeside = m.w - fw > 420;
        pageTabs("pl:tabs", m.x, y, filtersBeside ? m.right() - fw - 16 : m.right(), tabs);
        float fx = filtersBeside ? m.right() - fw : m.x;
        const float fy = filtersBeside ? y + 4 : y + 48;
        fx += menuButton("pl:fstatus", fx, fy, stLabel, [this] {
            std::vector<ToolItem> t;
            int counts[8] = {};
            int cards = 0;
            for (auto* n : lib_->manuscriptOf(pl_.book))
                if (n->kind != NodeKind::Part) { ++cards; if ((int)n->status >= 0 && (int)n->status < 8) ++counts[(int)n->status]; }
            t.push_back(ToolItem::item(L"All statuses  (" + std::to_wstring(cards) + L")", [this] { pl_.status = -1; pl_.scroll = 0; }, pl_.status < 0));
            for (NodeStatus st : kStatuses)
                t.push_back(ToolItem::item(w(label(st)) + L"  (" + std::to_wstring(counts[(int)st]) + L")", [this, st] { pl_.status = (int)st; pl_.scroll = 0; },
                                           pl_.status == (int)st));
            return t;
        }) + 8;
        fx += menuButton("pl:fpov", fx, fy, povLabel, [this] {
            std::vector<ToolItem> t;
            std::map<Id, int> povs;
            int none = 0;
            for (auto* n : lib_->manuscriptOf(pl_.book))
                if (n->kind != NodeKind::Part) { if (n->povId.empty() || !lib_->entry(n->povId)) ++none; else ++povs[n->povId]; }
            t.push_back(ToolItem::item(L"Any viewpoint", [this] { pl_.pov.clear(); pl_.scroll = 0; }, pl_.pov.empty()));
            std::vector<std::pair<std::wstring, Id>> named;
            for (auto& [id, c] : povs) if (const Entry* e = lib_->entry(id)) named.push_back({w(e->name), id});
            std::sort(named.begin(), named.end());
            for (auto& [nm, id] : named)
                t.push_back(ToolItem::item(nm + L"  (" + std::to_wstring(povs[id]) + L")", [this, id = id] { pl_.pov = id; pl_.scroll = 0; }, pl_.pov == id));
            if (none) t.push_back(ToolItem::item(L"No viewpoint  (" + std::to_wstring(none) + L")", [this] { pl_.pov = "-"; pl_.scroll = 0; }, pl_.pov == "-"));
            return t;
        }) + 8;
        if (showThreads)
            menuButton("pl:fthread", fx, fy, thLabel, [this] {
                std::vector<ToolItem> t;
                t.push_back(ToolItem::item(L"Any thread", [this] { pl_.thread.clear(); pl_.scroll = 0; }, pl_.thread.empty()));
                for (const plan::Thread& x : plan::threadsOf(*lib_, pl_.book).threads)
                    t.push_back(ToolItem::item(w(x.name), [this, id = x.id] { pl_.thread = id; pl_.scroll = 0; }, pl_.thread == x.id));
                return t;
            });
        y += filtersBeside ? 40 : 40 + 46;
        (void)all;
    }
    if (bar) y += 8 + toolBar("plan", {m.x, y + 8, m.w, 38}, planTools());
    y += 12;
    if (!sub.empty()) y += gfx_.text(sub, {m.x, y, std::min(m.w, 760.0f), 60}, Font::Small, pal_.inkMuted) + 14;
    return y - m.y + 8;
}

// The plot threads as cards (0.69): what the sidebar listed. Click one to edit it beside them.
void StudioApp::drawPlanThreads(const Rect& r) {
    const studio::Book* b = lib_->book(pl_.book);
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    const float pad = r.w > 900 ? 36 : 24;
    const float cw = r.w - pad * 2;
    const Rect m{r.x + pad, r.y + 26 - pl_.scroll, cw, 0};
    float y = m.y + planHeader(m, L"The story's strands: a subplot, a mystery, a romance. Tag the chapters each one runs through, and colour the "
                                  L"corkboard by them.");
    const plan::Threads th = plan::threadsOf(*lib_, pl_.book);
    const auto all = lib_->manuscriptOf(pl_.book);
    if (th.threads.empty()) {
        y += emptyState({m.x, y, cw, 220}, cjui::Icon::Spark, L"No plot threads yet.", L"+ New thread", [this] {
            const ManuscriptNode* sel = lib_->node(pl_.node);
            newThread(sel && !sel->deleted && sel->kind != NodeKind::Part ? sel->id : Id{});
        });
    } else {
        const float gap = 16, cardW = 320;
        const int cols = std::max(1, (int)((cw + gap) / (cardW + gap)));
        const float wcol = (cw - gap * (cols - 1)) / cols;
        float rowY = y, rowH = 0;
        int col = 0;
        for (const plan::Thread& t : th.threads) {
            std::vector<const ManuscriptNode*> in;
            for (auto* n : all) if (n->kind != NodeKind::Part && !n->deleted && th.has(n->id, t.id)) in.push_back(n);
            const float ch = 132;
            const Rect cr{m.x + col * (wcol + gap), rowY, wcol, ch};
            const std::string id = "pl:thr:" + t.id;
            addHit(id, cr, [this, tid = t.id] { selectThread(tid); });
            const float hv = hoverAmount(id);
            const bool sel = focus_.kind == Focus::Thread && focus_.id == t.id;
            card(cr, 12, 1.0f + hv);
            const Color tc = cjui::hexColor(t.colour.c_str());
            gfx_.fillRounded({cr.x + 6, cr.y + 14, 4, cr.h - 28}, 2, tc);
            if (sel) gfx_.strokeRounded(cr, 12, pal_.ribbon, 2);
            gfx_.text(w(t.name), {cr.x + 22, cr.y + 14, cr.w - 40, 26}, Font::Heading, pal_.ink, {Align::Left, false, true});
            gfx_.text(in.size() == 1 ? L"1 card" : std::to_wstring(in.size()) + L" cards", {cr.x + 22, cr.y + 42, cr.w - 40, 18}, Font::Small,
                      pal_.inkFaint, {Align::Left, false});
            gfx_.pushClip({cr.x, cr.y, cr.w, cr.h - 8});
            if (!t.about.empty()) gfx_.text(w(t.about), {cr.x + 22, cr.y + 64, cr.w - 40, 40}, Font::Small, pal_.inkMuted);
            else {
                std::wstring names;
                for (size_t i = 0; i < in.size() && i < 4; ++i) names += (i ? L", " : L"") + w(in[i]->title);
                if (in.size() > 4) names += L"…";
                gfx_.text(names.empty() ? L"No chapters tagged yet." : names, {cr.x + 22, cr.y + 64, cr.w - 40, 40}, Font::Small, pal_.inkMuted);
            }
            gfx_.popClip();
            if (focused(id)) focusRing(cr);
            rowH = std::max(rowH, ch);
            if (++col == cols) { col = 0; rowY += rowH + gap; rowH = 0; }
        }
        y = rowY + (col ? rowH + gap : 0);
        smallButton("pl:thrnew", m.x, y + 4, L"+ New thread", [this] {
            const ManuscriptNode* sel = lib_->node(pl_.node);
            newThread(sel && !sel->deleted && sel->kind != NodeKind::Part ? sel->id : Id{});
        });
        y += 50;
    }
    pl_.content = y + pl_.scroll - r.y + 40;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawPlanBoard(const Rect& r) {
    const studio::Book* b = lib_->book(pl_.book);
    if (!b) return;
    pl_.selCard = {};
    gfx_.pushClip(r);
    setHitClip(r);
    // Cork: a warm board with a faint grain. The grain is a tile made once and laid across the board (0.72: 900 dots
    // drawn each frame were most of the corkboard's time).
    const Color cork = pal_.dark ? cjui::hexColor("#2E2822") : pal_.paper.mix(cjui::hexColor("#C8A97E"), 0.32f);
    {
        constexpr int T = 256;
        static std::vector<std::uint8_t> tiles[2];
        static Color madeFor[2];
        const int k = pal_.dark ? 1 : 0;
        std::vector<std::uint8_t>& tile = tiles[k];
        const std::wstring key = std::wstring(L"#cork:") + (pal_.dark ? L"dark" : L"light");
        if (tile.empty() || madeFor[k].r != cork.r || madeFor[k].g != cork.g || madeFor[k].b != cork.b) {
            if (!tile.empty()) gfx_.forgetImage(key);
            madeFor[k] = cork;
            tile.assign((size_t)T * T * 4, 255);
            std::vector<float> px((size_t)T * T * 3);
            for (int i = 0; i < T * T; ++i) { px[i * 3] = cork.r; px[i * 3 + 1] = cork.g; px[i * 3 + 2] = cork.b; }
            const Color grain = pal_.dark ? cjui::hexColor("#000000") : cjui::hexColor("#8A6A42");
            const float ga = pal_.dark ? 0.18f : 0.10f;
            unsigned seed = 2166136261u;
            auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
            for (int d = 0; d < 70; ++d) {   // about the old density
                const float cx = rnd() * T, cy = rnd() * T, rad = 0.6f + rnd() * 1.1f;
                for (int yy = (int)(cy - rad - 1); yy <= (int)(cy + rad + 1); ++yy)
                    for (int xx = (int)(cx - rad - 1); xx <= (int)(cx + rad + 1); ++xx) {
                        const float dist = std::hypot(xx + 0.5f - cx, yy + 0.5f - cy);
                        const float cover = std::clamp(rad + 0.5f - dist, 0.0f, 1.0f) * ga;
                        if (cover <= 0) continue;
                        const int wx = (xx % T + T) % T, wy = (yy % T + T) % T;   // wraps, so the tiles meet without seams
                        float* p = &px[(wy * T + wx) * 3];
                        p[0] += (grain.r - p[0]) * cover;
                        p[1] += (grain.g - p[1]) * cover;
                        p[2] += (grain.b - p[2]) * cover;
                    }
            }
            for (int i = 0; i < T * T; ++i)
                for (int c = 0; c < 3; ++c) tile[(size_t)i * 4 + c] = (std::uint8_t)std::lround(std::clamp(px[i * 3 + c], 0.0f, 1.0f) * 255);
        }
        gfx_.fill(r, cork);
        for (float ty = r.y; ty < r.bottom(); ty += T)
            for (float tx = r.x; tx < r.right(); tx += T) gfx_.drawPixels(key, T, T, tile.data(), {tx, ty, (float)T, (float)T});
    }
    const float pad = r.w > 900 ? 36 : 24;
    const float cw = r.w - pad * 2;
    const Rect m{r.x + pad, r.y + 26 - pl_.scroll, cw, 0};
    const auto all = lib_->manuscriptOf(pl_.book);
    int chapters = 0, scenes = 0;
    for (auto* n : all) {
        if (n->kind == NodeKind::Chapter) ++chapters;
        else if (n->kind == NodeKind::Scene) ++scenes;
    }
    std::wstring sub = std::to_wstring(chapters) + (chapters == 1 ? L" chapter" : L" chapters");
    if (scenes) sub += L", " + std::to_wstring(scenes) + (scenes == 1 ? L" scene" : L" scenes");
    sub += clickOpensPage() ? L". Click a card for its page, right-click for its details here." : L". Click a card to plan it; double-click to write it; right-click for its page.";
    float y = m.y + planHeader(m, sub);
    if (all.empty()) {
        y += gfx_.text(L"No chapters yet.", {m.x, y, cw, 24}, Font::Body, pal_.inkMuted) + 12;
        button("pl:first", {m.x, y, 160, 34}, L"Add a chapter", [this] {
            const Id id = lib_->createNode(pl_.book, {}, NodeKind::Chapter);
            if (!id.empty()) selectCard(id);
        }, true);
        y += 50;
    }

    // How the board is laid out (0.51): rows or a column per part, the cards' size, and what colours them.
    const std::string layout = planPref(pl_.book, "layout", "rows"), colourBy = planPref(pl_.book, "colourBy", "viewpoint"),
                      size = planPref(pl_.book, "cardSize", "medium");
    const bool columns = layout == "columns";
    if (!all.empty()) {
        float tx = m.x;
        const float th = 28;
        auto label = [&](const wchar_t* t) {
            const float lw = gfx_.measureWidth(t, Font::Small) + 8;
            gfx_.text(t, {tx, y, lw, th}, Font::Small, pal_.inkMuted, {Align::Left, false, false, true});
            tx += lw;
        };
        auto wrap = [&](float wanted) { if (tx + wanted > m.x + cw && tx > m.x) { tx = m.x; y += th + 8; } };
        wrap(150);
        segmented("pl:layout", {tx, y, 150, th}, {L"Rows", L"Columns"}, columns ? 1 : 0, [this](int i) {
            setPlanPref(pl_.book, "layout", i ? "columns" : "rows");
            pl_.boardX = 0;
        });
        tx += 150 + 20;
        wrap(40 + 150);
        label(L"Size");
        segmented("pl:size", {tx, y, 150, th}, {L"Small", L"Medium", L"Large"}, size == "small" ? 0 : size == "large" ? 2 : 1, [this](int i) {
            setPlanPref(pl_.book, "cardSize", i == 0 ? "small" : i == 2 ? "large" : "medium");
        });
        tx += 150 + 20;
        wrap(70 + 250);
        label(L"Colour by");
        segmented("pl:colourby", {tx, y, 250, th}, {L"Viewpoint", L"Thread", L"Status"}, colourBy == "thread" ? 1 : colourBy == "status" ? 2 : 0,
                  [this](int i) { setPlanPref(pl_.book, "colourBy", i == 1 ? "thread" : i == 2 ? "status" : "viewpoint"); });
        tx += 250 + 20;
        // What a click on a card opens (0.90): its page, or its details beside the board; right-click opens the other.
        wrap(80 + 150);
        label(L"Click opens");
        segmented("pl:click", {tx, y, 150, th}, {L"Page", L"Details"}, clickOpensPage() ? 0 : 1, [this](int i) {
            setPlanPref(pl_.book, "click", i == 0 ? "page" : "panel");
        });
        tooltip("pl:click", L"A card's own page, or its details beside the board. Right-click a card for the other.");
        y += th + 18;
    }

    // Who's who, for viewpoint and appearances.
    auto entryName = [&](const Id& id) -> const Entry* {
        const Entry* e = lib_->entry(id);
        return e && !e->deleted ? e : nullptr;
    };
    // Plot threads (0.50): a row of coloured tags on each card, when the book has any.
    const plan::Threads threads = plan::threadsOf(*lib_, pl_.book);
    const float tagRow = threads.threads.empty() ? 0.0f : 22.0f;
    const float gap = 14;
    const float cardW = size == "small" ? 190.0f : size == "large" ? 300.0f : 232.0f;
    const float cardH = (size == "small" ? 138.0f : size == "large" ? 216.0f : 166.0f) + tagRow;
    // The card being edited in place (0.51), and its field.
    if (pl_.cardEdit >= 0 && (pl_.node.empty() || !lib_->node(pl_.node))) pl_.cardEdit = -1;
    bool editPlaced = false;

    // What the cards show as pictures (0.87): each one's place, and its date in the calendar with the first moon's phase.
    const std::map<Id, Id> places = plan::nodePlaces(*lib_, pl_.book);
    const cal::Calendar cardCal = cal::calendarOfBook(*lib_, pl_.book);
    const cal::StorySpan cardSpan = cal::storySpan(*lib_, pl_.book);
    const nlohmann::json cardTl = nlohmann::json::parse(lib_->setting("timeline:" + pl_.book), nullptr, false);
    auto cardDay = [&](const Id& nid) -> std::optional<std::int64_t> {
        if (!cardSpan.on || cardCal.empty() || !cardTl.is_object() || !cardTl.contains("nodes") || !cardTl["nodes"].contains(nid)) return std::nullopt;
        const auto& v = cardTl["nodes"][nid];
        if (!v.is_object() || !v.contains("day") || !v["day"].is_number()) return std::nullopt;
        return cardSpan.start + (std::int64_t)std::floor(v["day"].get<double>());
    };
    // One card, drawn and made live at `cr`.
    auto drawCard = [&](const ManuscriptNode* n, const Rect& cr, int chapterNo, int sceneNo) {
        if (pl_.node == n->id) pl_.selCard = cr;
        // Only the cards on screen are drawn (0.72): a long book's board was drawn whole every frame.
        if (cr.bottom() < r.y - 40 || cr.y > r.bottom() + 40 || cr.right() < r.x - 40 || cr.x > r.right() + 40) return;
        const std::string id = "pl:card:" + n->id;
        addHit(id, cr, [this, nid = n->id] { cardClick(nid); });
        tooltip(id, clickOpensPage() ? L"Click for its page; right-click for its details here" : L"Click for its details; right-click for its page");
        {
            Drop drop{Drop::NodeRow, cr, n->id, n->parentId};
            drop.horizontal = !columns;   // rows of cards: before or after by left and right halves; columns: top and bottom
            addDrop(drop);
        }
        const bool sel = pl_.node == n->id;
        if (sel) pl_.selCard = cr;   // its pop-over goes beside it (0.69)
        const bool lifted = drag_.active && drag_.id == n->id;
        const float hv = hoverAmount(id);
        // An index card: a little tilt of light, a red rule under the heading, a pin in its status colour.
        softShadow(cr, 3, std::max(0.35f + 0.65f * hv, sel ? 1.0f : 0.0f));
        const Color paperCard = pal_.dark ? (n->kind == NodeKind::Scene ? pal_.surface.mix(pal_.paper, 0.3f) : pal_.surface)
                                          : (n->kind == NodeKind::Scene ? cjui::hexColor("#FBF6E9") : cjui::hexColor("#FFFDF7"));
        gfx_.fillRounded(cr, 3, lifted ? paperCard.withAlpha(0.5f) : paperCard);
        // Its colour (0.35; chosen 0.51): its viewpoint character's, its first plot thread's, or its status's.
        const bool hasPov = entryName(n->povId) != nullptr;
        const Color pc = hasPov ? povColour(n->povId) : Color{};
        bool tinted = false;
        Color band{};
        if (colourBy == "status") { band = statusColour(n->status); tinted = true; }
        else if (colourBy == "thread") {
            auto it = threads.nodes.find(n->id);
            if (it != threads.nodes.end())
                for (auto& tid : it->second)
                    if (const plan::Thread* t = threads.find(tid)) { band = cjui::hexColor(t->colour.c_str()); tinted = true; break; }
        } else if (hasPov) { band = pc; tinted = true; }
        if (tinted) {
            gfx_.fillRounded(cr, 3, band.withAlpha(pal_.dark ? 0.10f : 0.07f));
            gfx_.fillRounded({cr.x, cr.y, cr.w, 5}, 2, band);
        }
        if (sel) gfx_.strokeRounded(cr, 3, pal_.ribbon, 2.0f);
        else if (hv > 0.01f) gfx_.strokeRounded(cr, 3, pal_.rule.mix(pal_.inkFaint, hv), 1.0f);
        gfx_.line(cr.x + 10, cr.y + 50.5f, cr.right() - 10, cr.y + 50.5f, cjui::hexColor("#D9534F").withAlpha(pal_.dark ? 0.45f : 0.55f), 1.0f);
        const float whoY = cr.bottom() - 44 - tagRow;
        for (float ly = cr.y + 72; ly < whoY - 4; ly += 18)
            gfx_.line(cr.x + 10, ly + 0.5f, cr.right() - 10, ly + 0.5f, cjui::hexColor("#6A9BC3").withAlpha(pal_.dark ? 0.10f : 0.16f), 1.0f);
        const float ix = cr.x + 16, iw = cr.w - 30;
        // The pin and the status word choose the status (0.51).
        {
            const float px = cr.x + cr.w / 2, py = cr.y + 2;
            gfx_.circle(px + 1, py + 2, 6, cjui::hexColor("#000000").withAlpha(0.18f), true);
            gfx_.circle(px, py, 6, statusColour(n->status), true);
            gfx_.circle(px - 2, py - 2, 2, cjui::hexColor("#FFFFFF").withAlpha(0.55f), true);
        }
        std::wstring kicker = n->kind == NodeKind::Chapter ? L"CHAPTER " + std::to_wstring(chapterNo)
                                                           : std::to_wstring(chapterNo) + L" · SCENE " + std::to_wstring(sceneNo);
        gfx_.text(kicker, {ix, cr.y + 12, iw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false, true});
        std::wstring st = w(label(n->status));
        for (auto& ch : st) ch = (wchar_t)towupper(ch);
        const float stW = gfx_.measureWidth(st, Font::Label);
        const std::string sid = "pl:card:" + n->id + "|status";
        const Rect stR{ix + iw - stW - 6, cr.y + 8, stW + 8, 22};
        addHit(sid, stR, [this, nid = n->id] {
            if (pl_.node != nid) selectQuiet(nid);
            const ManuscriptNode* x = lib_->node(nid);
            if (!x) return;
            std::vector<std::wstring> items;
            for (NodeStatus s2 : kStatuses) items.push_back(w(label(s2)));
            const int i = popupAtCursor(hwnd(), items, (int)x->status);
            if (i < 0) return;
            commitFields();
            ManuscriptNode u = *lib_->node(nid);
            u.status = kStatuses[i];
            lib_->updateNode(u);
            invalidate();
        });
        tooltip(sid, L"Change its status");
        // A badge in the status's colour, with its word (0.87).
        gfx_.fillRounded(stR, 11, statusColour(n->status).withAlpha((pal_.dark ? 0.22f : 0.13f) + 0.12f * hoverAmount(sid)));
        gfx_.strokeRounded(stR, 11, statusColour(n->status).withAlpha(0.55f), 1);
        gfx_.text(st, stR, Font::Label, statusColour(n->status).mix(pal_.ink, 0.25f), {Align::Center, false, false, true});
        // Its date as a little calendar page, with the moon (0.87).
        if (const auto day = cardDay(n->id)) {
            const cal::Date d = cal::fromDay(cardCal, *day);
            const float kw = gfx_.measureWidth(kicker, Font::Label);
            const Rect chip{ix + kw + 10, cr.y + 7, 24, 24};
            if (chip.right() + (cardCal.moons.empty() ? 0 : 16) < stR.x - 4) {
                drawDateChip(chip, d.day, w(cal::monthLabel(cardCal, d.month)));
                const std::string did = "pl:card:" + n->id + "|date";
                addHit(did, chip, [this, nid = n->id] { cardClick(nid); }, false);
                std::wstring tip = w(cal::formatDate(cardCal, d));
                if (!cardCal.moons.empty()) {
                    const double ph = cal::moonPhase(cardCal, cardCal.moons[0], *day);
                    drawMoonDisc(chip.right() + 9, chip.y + 12, 5.5f, ph);
                    tip += L"  \u00B7  " + w(cardCal.moons[0].name + ": " + cal::phaseName(ph));
                }
                tooltip(did, tip);
            }
        }
        // The title: click to rename it here.
        const Rect titleR{ix - 4, cr.y + 27, iw + 6, 24};
        const bool editingTitle = sel && pl_.cardEdit == FName;
        const bool editingSyn = sel && pl_.cardEdit == FSynopsis;
        if (!editingTitle) {
            const std::string tid = "pl:card:" + n->id + "|title";
            addHit(tid, titleR, [this, nid = n->id] {
                if (pl_.node != nid) selectQuiet(nid);
                pl_.cardEdit = FName;
                pl_.cardEditFocused = false;
                invalidate();
            });
            tooltip(tid, L"Click to rename it");
            gfx_.text(w(n->title.empty() ? std::string(label(n->kind)) : n->title), {ix, cr.y + 30, iw, 22}, Font::BodyStrong,
                      pal_.ink, {Align::Left, false, true});
            if (hoverAmount(tid) > 0.01f) gfx_.line(ix, cr.y + 48, ix + std::min(iw, gfx_.measureWidth(w(n->title), Font::BodyStrong)), cr.y + 48,
                                                     pal_.inkFaint.withAlpha(hoverAmount(tid)), 1.0f);
        } else {
            gfx_.fillRounded(titleR, 3, pal_.surface);
            gfx_.strokeRounded(titleR, 3, pal_.ribbon, 1.5f);
            placeField(FName, titleR.inset(-2, 0));
            editPlaced = true;
        }
        // The synopsis, or a prompt for one: click to write it here.
        const Rect syn{ix, cr.y + 54, iw, std::max(18.0f, whoY - 4 - (cr.y + 54))};
        if (!editingSyn) {
            const std::string yid = "pl:card:" + n->id + "|syn";
            addHit(yid, syn, [this, nid = n->id] {
                if (pl_.node != nid) selectQuiet(nid);
                pl_.cardEdit = FSynopsis;
                pl_.cardEditFocused = false;
                invalidate();
            });
            tooltip(yid, L"Click to write its synopsis");
            gfx_.pushClip(syn);
            const size_t room = (size_t)std::max(40.0f, 150.0f * (syn.w * syn.h) / (202.0f * 64.0f));
            if (n->synopsis.empty()) gfx_.text(sel || hoverAmount(yid) > 0.01f ? L"Write a synopsis…" : L"No synopsis yet.", syn, Font::Small, pal_.inkFaint);
            else gfx_.text(shortened(w(n->synopsis), room), syn, Font::Small, pal_.inkMuted);
            gfx_.popClip();
        } else {
            const Rect er{syn.x - 4, syn.y - 2, syn.w + 6, std::max(syn.h + 4, 60.0f)};
            gfx_.fillRounded(er, 3, pal_.surface);
            gfx_.strokeRounded(er, 3, pal_.ribbon, 1.5f);
            placeField(FSynopsis, er);
            editPlaced = true;
        }
        // Viewpoint and who appears; click to choose the viewpoint (0.51).
        std::wstring who;
        if (const Entry* pov = entryName(n->povId)) who = L"Viewpoint: " + w(pov->name);
        // Who's there as chosen (0.59), else who the text names.
        const std::vector<Id> chosen = plan::castOf(*lib_, pl_.book, n->id);
        if (auto it = pl_.appearances.find(n->id); !chosen.empty() || it != pl_.appearances.end()) {
            std::wstring others;
            int shown = 0, more = 0;
            for (const Id& eid : chosen.empty() ? it->second : chosen) {
                const Entry* e = entryName(eid);
                if (!e || (e->kind != "character" && !(chosen.size() && e->kind == "creature")) || eid == n->povId) continue;
                if (shown < 3) { others += (shown ? L", " : L"") + w(e->name); ++shown; }
                else ++more;
            }
            if (more) others += L" +" + std::to_wstring(more);
            if (!others.empty()) who += (who.empty() ? L"With " : L" · with ") + others;
        }
        {
            const std::string vid = "pl:card:" + n->id + "|pov";
            const Rect vr{ix - 4, whoY - 2, iw + 6, 22};
            addHit(vid, vr, [this, nid = n->id] {
                if (pl_.node != nid) selectQuiet(nid);
                chooseViewpoint(nid);
            });
            tooltip(vid, L"Choose whose eyes it's seen through");
            const float vh = hoverAmount(vid);
            if (vh > 0.01f) gfx_.fillRounded(vr, 4, pal_.ink.withAlpha(0.05f * vh));
            if (who.empty() && (sel || vh > 0.01f)) who = L"Choose a viewpoint…";
            if (!who.empty()) {
                float wx = ix;
                // The viewpoint's portrait, ringed in its colour; who's there as small portraits at the right (0.87).
                std::vector<const Entry*> faces;
                for (const Id& eid : chosen.empty() ? (pl_.appearances.count(n->id) ? pl_.appearances[n->id] : std::vector<Id>{}) : chosen)
                    if (const Entry* e = entryName(eid); e && eid != n->povId && (e->kind == "character" || e->kind == "creature")) faces.push_back(e);
                const size_t most = faces.size() > 4 ? 3 : faces.size();
                const float facesW = most ? 16 + 11.0f * (float)(most - 1) + (faces.size() > most ? 22 : 0) : 0;
                if (const Entry* pov = entryName(n->povId)) {
                    drawPortrait(*pov, {ix, whoY - 3, 24, 24}, 12);   // larger, the bible's icon (4b, 0.102)
                    gfx_.strokeRounded({ix - 1.5f, whoY - 4.5f, 27, 27}, 13.5f, pc, 1.6f);
                    wx += 30;
                    gfx_.text(w(pov->name), {wx, whoY, iw - (wx - ix) - facesW - 6, 18}, Font::Small, pal_.ink, {Align::Left, false, true, true});
                } else if (faces.empty())
                    gfx_.text(who, {wx, whoY, iw, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
                float fx = ix + iw - facesW;
                for (size_t k = 0; k < most; ++k) {
                    drawPortrait(*faces[k], {fx, whoY + 1, 16, 16}, 8);
                    gfx_.strokeRounded({fx - 1, whoY, 18, 18}, 9, paperCard, 1.5f);
                    fx += 11;
                }
                if (faces.size() > most)
                    gfx_.text(L"+" + std::to_wstring(faces.size() - most), {fx + 7, whoY, 22, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, false, true});
                tooltip(vid, who.rfind(L"Choose", 0) == 0 ? L"Choose whose eyes it's seen through" : who + L"\nClick to choose the viewpoint");
            }
        }
        // Its plot threads, as tags.
        if (auto it = threads.nodes.find(n->id); it != threads.nodes.end()) {
            float tx = ix;
            const float ty = cr.bottom() - 46;
            size_t i = 0;
            for (; i < it->second.size(); ++i) {
                const plan::Thread* t = threads.find(it->second[i]);
                if (!t) continue;
                const Color tc = cjui::hexColor(t->colour.c_str());
                const std::wstring nm = w(t->name);
                const float room = ix + iw - tx - (i + 1 < it->second.size() ? 30.0f : 0.0f);
                if (room < 40) break;
                const float tw = std::min(gfx_.measureWidth(nm, Font::Small) + 22, room);
                const Rect tag{tx, ty, tw, 17};
                gfx_.fillRounded(tag, 8.5f, tc.withAlpha(pal_.dark ? 0.28f : 0.16f));
                gfx_.circle(tag.x + 8, tag.y + 8.5f, 3.5f, tc, true);
                gfx_.text(nm, {tag.x + 15, tag.y, tag.w - 19, tag.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
                tx += tw + 5;
            }
            if (i < it->second.size())
                gfx_.text(L"+" + std::to_wstring(it->second.size() - i), {tx, ty, 30, 17}, Font::Small, pal_.inkFaint, {Align::Left, false, false, true});
        }
        // Its threads as coloured stripes along the foot of the card (0.87).
        if (auto it = threads.nodes.find(n->id); it != threads.nodes.end() && !it->second.empty()) {
            std::vector<Color> cs;
            for (auto& tid : it->second) if (const plan::Thread* t = threads.find(tid)) cs.push_back(cjui::hexColor(t->colour.c_str()));
            if (!cs.empty()) {
                const float sw = (cr.w - 6) / cs.size();
                for (size_t k = 0; k < cs.size(); ++k) gfx_.fill({cr.x + 3 + sw * k, cr.bottom() - 5, sw - (k + 1 < cs.size() ? 2 : 0), 4}, cs[k]);
            }
        }
        // Where it happens: the place's symbol and name at the foot, right (0.87).
        float placeW = 0;
        if (auto pit = places.find(n->id); pit != places.end())
            if (const Entry* pe = entryName(pit->second)) {
                const std::wstring pn = w(pe->name);
                placeW = std::min(iw * 0.5f, gfx_.measureWidth(pn, Font::Small) + 20);
                const float py = cr.bottom() - 22;
                drawKindMark(pe->kind, {ix + iw - placeW, py + 1, 14, 14});
                gfx_.text(pn, {ix + iw - placeW + 18, py, placeW - 18, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
                const std::string plid = "pl:card:" + n->id + "|place";
                addHit(plid, {ix + iw - placeW, py, placeW, 18}, [this, nid = n->id] { cardClick(nid); }, false);
                tooltip(plid, L"Where it happens: " + pn);
            }
        // Length against the target.
        const float fy = cr.bottom() - 22;
        std::int64_t words = n->wordCount;
        if (n->kind == NodeKind::Chapter)
            for (auto* c : all) if (c->parentId == n->id) words += c->wordCount;
        std::wstring len = thousandsW(words) + L" words";
        if (n->targetWords > 0) {
            len += L" of " + thousandsW(n->targetWords);
            const float f = (float)std::min(1.0, (double)words / n->targetWords);
            gfx_.fillRounded({ix, fy - 6, iw, 3}, 1.5f, pal_.rule);
            if (f > 0) gfx_.fillRounded({ix, fy - 6, std::max(3.0f, iw * f), 3}, 1.5f, words >= n->targetWords ? pal_.ink : pal_.ribbon);
        }
        gfx_.text(len, {ix, fy, iw - placeW - 6, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true, true});
        if (focused(id)) focusRing(cr);
    };

    if (!columns) {
        const int cols = std::max(1, (int)((cw + gap) / (cardW + gap)));
        const float cardWid = (cw - gap * (cols - 1)) / cols;
        int col = 0;
        auto newRow = [&] { if (col) { y += cardH + gap; col = 0; } };
        int chapterNo = 0, sceneNo = 0;
        for (const ManuscriptNode* n : all) {
            if (n->kind == NodeKind::Part) {
                newRow();
                y += 8;
                gfx_.text(w(n->title.empty() ? std::string("Part") : n->title), {m.x, y, cw, 28}, Font::Heading, pal_.ink, {Align::Left, false, true});
                const std::string pid = "pl:part:" + n->id;
                if (!n->synopsis.empty())
                    gfx_.text(shortened(w(n->synopsis), 160), {m.x, y + 26, cw, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
                addHit(pid, {m.x, y, std::min(cw, gfx_.measureWidth(w(n->title), Font::Heading) + 10), 26}, [this, id = n->id] { selectCard(id); });
                y += n->synopsis.empty() ? 36 : 54;
                continue;
            }
            if (n->kind == NodeKind::Chapter) { ++chapterNo; sceneNo = 0; }
            else ++sceneNo;
            if (!nodeShown(n)) continue;
            drawCard(n, {m.x + col * (cardWid + gap), y, cardWid, cardH}, chapterNo, sceneNo);
            if (++col == cols) newRow();
        }
        newRow();
    } else {
        // A column for each part (and one for what comes before the first part), its cards stacked down it.
        struct Column { const ManuscriptNode* part = nullptr; std::vector<std::tuple<const ManuscriptNode*, int, int>> cards; };
        std::vector<Column> cols;
        int chapterNo = 0, sceneNo = 0;
        for (const ManuscriptNode* n : all) {
            if (n->kind == NodeKind::Part) { cols.push_back({n, {}}); continue; }
            if (n->kind == NodeKind::Chapter) { ++chapterNo; sceneNo = 0; }
            else ++sceneNo;
            if (cols.empty()) cols.push_back({nullptr, {}});
            if (nodeShown(n)) cols.back().cards.push_back({n, chapterNo, sceneNo});
        }
        const float colGap = 22;
        const float x0 = m.x - pl_.boardX;
        const float top = y;
        float bottom = y;
        for (size_t c = 0; c < cols.size(); ++c) {
            const float cx = x0 + c * (cardW + colGap);
            float cy = top;
            const Column& colm = cols[c];
            // The column's head: the part's name and count; drop a card here to move it into the part.
            const Rect head{cx, cy, cardW, 50};
            gfx_.fillRounded(head, 4, pal_.surface.withAlpha(pal_.dark ? 0.35f : 0.55f));
            const std::wstring pname = colm.part ? w(colm.part->title.empty() ? std::string("Part") : colm.part->title) : L"Before the first part";
            gfx_.text(pname, {cx + 12, cy + 6, cardW - 24, 24}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            gfx_.text(std::to_wstring(colm.cards.size()) + (colm.cards.size() == 1 ? L" card" : L" cards"), {cx + 12, cy + 28, cardW - 24, 16},
                      Font::Small, pal_.inkFaint, {Align::Left, false, true});
            if (colm.part) addHit("pl:part:" + colm.part->id, head, [this, id = colm.part->id] { selectCard(id); });
            cy += 60;
            for (auto& [n, cno, sno] : colm.cards) {
                drawCard(n, {cx, cy, cardW, cardH}, cno, sno);
                cy += cardH + gap;
            }
            if (colm.part) {
                // The rest of the column takes a card into the part, at its end.
                Drop into{Drop::NodeRow, {cx, cy, cardW, std::max(80.0f, r.bottom() - cy)}, colm.part->id, colm.part->parentId};
                into.into = true;
                addDrop(into);
                Drop headDrop{Drop::NodeRow, head, colm.part->id, colm.part->parentId};
                headDrop.into = true;
                addDrop(headDrop);
                if (colm.cards.empty()) {
                    gfx_.strokeRounded({cx, cy, cardW, 70}, 4, pal_.inkFaint.withAlpha(0.5f), 1.0f);
                    gfx_.text(L"Drag cards here", {cx, cy, cardW, 70}, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
                    cy += 80;
                }
            }
            bottom = std::max(bottom, cy);
        }
        pl_.boardW = cols.size() * (cardW + colGap) - colGap;
        pl_.boardX = std::clamp(pl_.boardX, 0.0f, std::max(0.0f, pl_.boardW - cw));
        y = bottom + 10;
        if (pl_.boardW > cw) {
            y += gfx_.text(L"Shift and the mouse wheel move along the parts.", {m.x, y, cw, 18}, Font::Small, pal_.inkMuted,
                           {Align::Left, false}) + 10;
        }
    }
    // The field being edited on a card: focused once it's placed; when focus leaves it, the edit ends (and is kept).
    if (pl_.cardEdit >= 0) {
        if (!editPlaced) pl_.cardEdit = -1;
        else if (!pl_.cardEditFocused) {
            if (!(focus_ == loadedFocus_)) loadFields();   // the card's own text, before it's selected
            HWND f = fields_[pl_.cardEdit];
            SetFocus(f);
            if (pl_.cardEdit == FName) SendMessageW(f, EM_SETSEL, 0, -1);
            else SendMessageW(f, EM_SETSEL, (WPARAM)-1, -1);
            pl_.cardEditFocused = true;
        } else if (GetFocus() != fields_[pl_.cardEdit]) {
            pl_.cardEdit = -1;
            commitFields();
            invalidate();
        }
    }
    if (!all.empty()) {
        y += 6;
        float bw = 0;
        smallButton("pl:addch", m.x, y, L"+ Chapter", [this] {
            const Id id = lib_->createNode(pl_.book, {}, NodeKind::Chapter);
            if (!id.empty()) selectCard(id);
        }, &bw);
        const ManuscriptNode* sel = lib_->node(pl_.node);
        const Id chapter = sel ? (sel->kind == NodeKind::Chapter ? sel->id : sel->kind == NodeKind::Scene ? sel->parentId : Id{}) : Id{};
        if (!chapter.empty())
            smallButton("pl:addsc", m.x + bw + 8, y, L"+ Scene in this chapter", [this, chapter] {
                const Id id = lib_->createNode(pl_.book, chapter, NodeKind::Scene);
                if (!id.empty()) selectCard(id);
            });
        y += 44;
    }
    pl_.content = y + pl_.scroll - r.y + 30;
    pl_.scroll = std::clamp(pl_.scroll, 0.0f, std::max(0.0f, pl_.content - r.h));
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawPlanPanel(const Rect& r) {
    if (!panelAsCard_) {   // as a card or a pop-over (0.69), that's its background
        gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
        gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    }
    if (!(focus_ == loadedFocus_)) loadFields();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - pl_.panelScroll;
    const ManuscriptNode* n = pl_.node.empty() ? nullptr : lib_->node(pl_.node);
    if (n && n->deleted) n = nullptr;
    if (focus_.kind == Focus::Thread) drawThreadPanel(c, y);
    else if (focus_.kind == Focus::Beat) drawBeatPanel(c, y);
    else if (focus_.kind == Focus::Structure) drawOwnStructurePanel(c, y);
    else if (n) {
        std::wstring kind = w(label(n->kind));
        for (auto& ch : kind) ch = (wchar_t)towupper(ch);
        panelHeading(c, y, kind, true);
        y = panelField(c, y, L"Title", FName);
        if (n->kind != NodeKind::Part) {
            panelChoice("pl:status", c, y, L"Status", w(label(n->status)), [this] {
                const ManuscriptNode* x = lib_->node(pl_.node);
                if (!x) return;
                std::vector<std::wstring> items;
                for (NodeStatus st : kStatuses) items.push_back(w(label(st)));
                const int i = popupAtCursor(hwnd(), items, (int)x->status);
                if (i < 0) return;
                commitFields();
                ManuscriptNode u = *lib_->node(pl_.node);
                u.status = kStatuses[i];
                lib_->updateNode(u);
                invalidate();
            });
            // The viewpoint: one of the world's characters.
            const studio::Book* bk = lib_->book(pl_.book);
            std::vector<const Entry*> people;
            if (bk)
                for (const Entry* e : bk->worldId.empty() ? lib_->entriesOf({}, bk->id) : lib_->entriesOf(bk->worldId))
                    if (e->kind == "character") people.push_back(e);
            const Entry* pov = lib_->entry(n->povId);
            const std::wstring povLabel = pov && !pov->deleted ? w(pov->name) : people.empty() ? L"No characters yet" : L"None";
            panelChoice("pl:pov", c, y, L"Viewpoint character", povLabel, [this] { chooseViewpoint(pl_.node); });
            {
                const auto places = plan::nodePlaces(*lib_, pl_.book);
                const auto it = places.find(n->id);
                const Entry* pe = it != places.end() ? lib_->entry(it->second) : nullptr;
                panelChoice("pl:place", c, y, L"Where it happens", pe && !pe->deleted ? w(pe->name) : L"Not chosen", [this] { choosePlace(pl_.node); });
            }
            cardCast(c, y);
            y = panelField(c, y, L"Target words (blank: none)", FTarget);
        }
        y = panelField(c, y, L"Synopsis", FSynopsis, 150);
        planPanelExtras(c, y);
        if (n->kind != NodeKind::Part) cardThreads(c, y);
        // The picture or ornament it opens with (0.89): its own, else what it would take.
        if (n->kind != NodeKind::Scene) {
            panelHeading(c, y, L"HEADER PICTURE", false);
            const headers::BookHeaders hb = headers::read(*lib_, pl_.book);
            const auto ownIt = hb.nodes.find(n->id);
            const headers::Header own = ownIt == hb.nodes.end() ? headers::Header{} : ownIt->second;
            headers::BookHeaders without = hb;
            without.nodes.erase(n->id);
            const auto fallback = headers::resolve(*lib_, *n, without);
            headerChooser("pl:head", c, y, own, true, fallback, w(n->title), [this, id = n->id](const headers::Header& h) {
                commitFields();
                headers::BookHeaders b = headers::read(*lib_, pl_.book);
                if (h.none || !h.empty()) b.nodes[id] = h;
                else b.nodes.erase(id);
                headers::write(*lib_, pl_.book, b);
                invalidate();
            });
            if (n->matter != 0)
                y += gfx_.text(L"Front and back matter open with a header only when it's chosen here.", {c.x, y, c.w, 40}, Font::Small, pal_.inkFaint) + 10;
        }
        // The world's entries that appear in it.
        if (auto it = pl_.appearances.find(n->id); it != pl_.appearances.end() && !it->second.empty()) {
            panelHeading(c, y, L"APPEARING", false);
            int shown = 0;
            for (const Id& eid : it->second) {
                const Entry* e = lib_->entry(eid);
                if (!e || e->deleted || ++shown > 12) continue;
                linkText("pl:appear:" + eid, c.x, y, w(e->name), [this, eid] {
                    const Entry* x = lib_->entry(eid);
                    if (!x) return;
                    openWorldbuilding(x->worldId, x->bookId);
                    openEntry(eid);
                });
                gfx_.text(w(entryKindLabel(e->kind)), {c.x, y, c.w, 18}, Font::Small, pal_.inkFaint, {Align::Right, false});
                y += 24;
            }
            y += 6;
        }
        panelHeading(c, y, L"MORE", false);
        button("pl:write", {c.x, y, c.w, 32}, L"Open in Writing", [this] {
            writingBook_ = pl_.book;
            currentNode_ = pl_.node;
            openWriting(pl_.book);
        }, true);
        y += 44;
        if (n->kind != NodeKind::Part) {
            button("pl:page", {c.x, y, c.w, 32}, L"Open as page", [this, id = n->id] { openPlanPage(id); });
            y += 44;
        }
        float lx = c.x, lw = 0;
        auto link = [&](const std::string& id, const std::wstring& t, std::function<void()> act) {
            linkText(id, lx, y, t, std::move(act));
            lw = gfx_.measureWidth(t, Font::Small);
            lx += lw + 26;
        };
        link("pl:up", L"Move earlier", [this] { moveNodeBy(pl_.node, -1); pl_.appearances = lib_->appearancesIn(pl_.book); });
        link("pl:down", L"Move later", [this] { moveNodeBy(pl_.node, 1); pl_.appearances = lib_->appearancesIn(pl_.book); });
        y += 34;
    } else {
        panelHeading(c, y, L"THE BOOK", true);
        y = panelField(c, y, L"Target words for the book (blank: none)", FTarget);
        y += gfx_.text(L"Novels are often 70,000 to 100,000 words.",
                       {c.x, y, c.w, 80}, Font::Small, pal_.inkMuted) + 14;
        planPanelExtras(c, y);
        panelHeading(c, y, pl_.view == 0 ? L"THE CORKBOARD" : pl_.view == 1 ? L"ABOUT THE TIMELINE" : L"ABOUT STRUCTURE", false);
        y += gfx_.text(pl_.view == 0 ? L"Drag a card to move it, or onto a chapter to make it a scene. Ctrl and the wheel change their size."
                       : pl_.view == 1 ? L"Drag a card along to set when it happens, or onto the timeline from below."
                       : L"Match each beat to the chapters that carry it; the bar shows where it falls by length.",
                       {c.x, y, c.w, 200}, Font::Small, pal_.inkMuted) + 14;
    }
    pl_.panelContent = y + pl_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

}  // namespace nrs

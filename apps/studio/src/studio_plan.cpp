// The Planning workspace: a book's chapters and scenes as cards on a board, in
// manuscript order and grouped by part, with each one's status, synopsis,
// length against its target, viewpoint character and the world's entries that
// appear in it. The panel edits the selected card, or the book's word target.
#include "studio_app.hpp"
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

void StudioApp::drawPlanSidebar(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), pal_.rule);
    const studio::Book* b = lib_->book(pl_.book);
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    const float x = r.x + 10, wd = r.w - 20, tx0 = x + 12, cw = wd - 24;
    float y = r.y + 14 - pl_.sideScroll;
    linkText("side:pl:library", tx0, y, L"‹ Library", [this] { ws_ = Workspace::Library; goTo(View::Book, pl_.book); });
    y += 30;
    gfx_.text(L"PLANNING", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 20;
    y += gfx_.text(w(b->title), {tx0, y, cw, 90}, Font::SerifTitle, pal_.ink) + 6;

    // Progress towards the book's target.
    const auto all = lib_->manuscriptOf(pl_.book);
    std::int64_t words = 0;
    for (auto* n : all) words += n->wordCount;
    const std::int64_t target = bookTarget(pl_.book);
    if (target > 0) {
        const float f = (float)std::min(1.0, (double)words / target);
        gfx_.fillRounded({tx0, y, cw, 6}, 3, pal_.rule);
        if (f > 0) gfx_.fillRounded({tx0, y, std::max(6.0f, cw * f), 6}, 3, pal_.ribbon);
        y += 14;
        gfx_.text(thousandsW(words) + L" of " + thousandsW(target) + L" words (" + std::to_wstring((int)std::floor(100.0 * words / target)) + L"%)",
                  {tx0, y, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    } else {
        gfx_.text(thousandsW(words) + L" words · no target set", {tx0, y, cw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    }
    y += 34;

    // Views.
    gfx_.text(L"VIEW", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    {
        static const wchar_t* names[] = {L"Corkboard", L"Timeline", L"Structure"};
        for (int v = 0; v < 3; ++v) {
            const std::string id = "side:pl:view:" + std::to_string(v);
            const Rect rr{x, y, wd, 30};
            addHit(id, rr, [this, v] { commitFields(); pl_.view = v; pl_.scroll = 0; invalidate(); });
            const bool sel = pl_.view == v;
            const float hv = hoverAmount(id);
            if (sel) sideRowFrame({}, rr, true, 0, 0);
            else if (hv > 0.01f) gfx_.fillRounded(rr, 8, pal_.surface.withAlpha(0.6f * hv));
            gfx_.text(names[v], {tx0, rr.y, cw, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
            if (focused(id)) focusRing(rr);
            y += 32;
        }
    }
    y += 16;

    // Filter by status.
    gfx_.text(L"STATUS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    auto row = [&](int status, const std::wstring& label, int count, Color dot) {
        const std::string id = "side:pl:status:" + std::to_string(status);
        const Rect rr{x, y, wd, 30};
        addHit(id, rr, [this, status] { pl_.status = status; pl_.scroll = 0; invalidate(); });
        const bool sel = pl_.status == status;
        const float hv = hoverAmount(id);
        if (sel) sideRowFrame({}, rr, true, 0, 0);
        else if (hv > 0.01f) gfx_.fillRounded(rr, 8, pal_.surface.withAlpha(0.6f * hv));
        float lx = tx0;
        if (status >= 0) { gfx_.circle(tx0 + 4, rr.y + rr.h / 2, 4, dot, true); lx += 16; }
        gfx_.text(label, {lx, rr.y, cw - 30, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(std::to_wstring(count), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
        if (focused(id)) focusRing(rr);
        y += 32;
    };
    int cards = 0;
    for (auto* n : all) if (n->kind != NodeKind::Part) ++cards;
    row(-1, L"Everything", cards, pal_.inkFaint);
    for (NodeStatus st : kStatuses) {
        int c = 0;
        for (auto* n : all) if (n->kind != NodeKind::Part && n->status == st) ++c;
        row((int)st, w(label(st)), c, statusColour(st));
    }
    // Filter by viewpoint (0.49.3): each viewpoint character with their colour, and chapters with none.
    {
        std::map<Id, int> povs;
        int none = 0;
        for (auto* n : all) if (n->kind != NodeKind::Part) { if (n->povId.empty() || !lib_->entry(n->povId)) ++none; else ++povs[n->povId]; }
        if (!povs.empty()) {
            y += 16;
            gfx_.text(L"VIEWPOINT", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
            y += 24;
            auto prow = [&](const std::string& key, const std::wstring& label, int count, bool dot, Color c) {
                const std::string id = "side:pl:pov:" + key;
                const Rect rr{x, y, wd, 30};
                addHit(id, rr, [this, key] { pl_.pov = pl_.pov == key ? std::string() : key; pl_.scroll = 0; invalidate(); });
                const bool sel = pl_.pov == key;
                const float hv = hoverAmount(id);
                if (sel) sideRowFrame({}, rr, true, 0, 0);
                else if (hv > 0.01f) gfx_.fillRounded(rr, 8, pal_.surface.withAlpha(0.6f * hv));
                float lx = tx0;
                if (dot) { gfx_.circle(tx0 + 4, rr.y + rr.h / 2, 4, c, true); lx += 16; }
                gfx_.text(label, {lx, rr.y, cw - 30, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
                gfx_.text(std::to_wstring(count), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
                if (focused(id)) focusRing(rr);
                y += 32;
            };
            std::vector<std::pair<std::wstring, Id>> named;
            for (auto& [id, c] : povs) if (const Entry* e = lib_->entry(id)) named.push_back({w(e->name), id});
            std::sort(named.begin(), named.end());
            for (auto& [nm, id] : named) prow(id, nm, povs[id], true, povColour(id));
            if (none) prow("-", L"No viewpoint", none, false, pal_.inkFaint);
        }
    }
    // Plot threads (0.50): each with its colour; choose one to see its cards and edit it.
    {
        const plan::Threads th = plan::threadsOf(*lib_, pl_.book);
        y += 16;
        gfx_.text(L"PLOT THREADS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 24;
        for (const plan::Thread& t : th.threads) {
            int count = 0;
            for (auto* n : all) if (n->kind != NodeKind::Part && th.has(n->id, t.id)) ++count;
            const std::string id = "side:pl:thread:" + t.id;
            const Rect rr{x, y, wd, 30};
            addHit(id, rr, [this, tid = t.id] {
                const bool again = pl_.thread == tid;
                pl_.thread = again ? std::string() : tid;
                pl_.scroll = 0;
                if (again) selectThread({});
                else selectThread(tid);
            });
            const bool sel = pl_.thread == t.id;
            const float hv = hoverAmount(id);
            if (sel) sideRowFrame({}, rr, true, 0, 0);
            else if (hv > 0.01f) gfx_.fillRounded(rr, 8, pal_.surface.withAlpha(0.6f * hv));
            gfx_.fillRounded({tx0, rr.y + rr.h / 2 - 5, 10, 10}, 2, cjui::hexColor(t.colour.c_str()));
            gfx_.text(w(t.name), {tx0 + 16, rr.y, cw - 46, rr.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
            gfx_.text(std::to_wstring(count), {tx0, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
            if (focused(id)) focusRing(rr);
            y += 32;
        }
        linkText("side:pl:newthread", tx0, y + 4, L"+ New thread", [this] {
            const ManuscriptNode* sel = lib_->node(pl_.node);
            newThread(sel && !sel->deleted && sel->kind != NodeKind::Part ? sel->id : Id{});
        });
        y += 30;
    }
    y += 20;
    gfx_.text(L"ALSO", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    linkText("side:pl:import", tx0, y, L"Import planning…", [this] { importPlanText(); });
    y += 26;
    linkText("side:pl:export", tx0, y, L"Export the plan…", [this] { exportPlanText(); });
    y += 26;
    linkText("side:pl:write", tx0, y, L"Write", [this] { openWriting(pl_.book); });
    y += 26;
    linkText("side:pl:world", tx0, y, L"The world's bible", [this] {
        const studio::Book* bk = lib_->book(pl_.book);
        if (bk) openWorldbuilding(bk->worldId, bk->worldId.empty() ? bk->id : Id{});
    });
    y += 30;
    pl_.sideContent = y + pl_.sideScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

void StudioApp::drawPlanBoard(const Rect& r) {
    const studio::Book* b = lib_->book(pl_.book);
    if (!b) return;
    gfx_.pushClip(r);
    setHitClip(r);
    // Cork: a warm board with a faint grain, quiet enough to read the cards on.
    const Color cork = pal_.dark ? cjui::hexColor("#2E2822") : pal_.paper.mix(cjui::hexColor("#C8A97E"), 0.32f);
    gfx_.fill(r, cork);
    {
        unsigned seed = 2166136261u;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
        const Color grain = pal_.dark ? cjui::hexColor("#000000").withAlpha(0.18f) : cjui::hexColor("#8A6A42").withAlpha(0.10f);
        for (int i = 0; i < 900; ++i) {
            const float gx = r.x + rnd() * r.w, gy = r.y + rnd() * std::max(r.h, 1.0f);
            gfx_.circle(gx, gy, 0.6f + rnd() * 1.1f, grain, true);
        }
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
    sub += L". Select a card to plan it in the panel; double-click to write it.";
    float y = m.y + pageHeader(m, L"PLANNING", w(b->title), sub);
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

    // One card, drawn and made live at `cr`.
    auto drawCard = [&](const ManuscriptNode* n, const Rect& cr, int chapterNo, int sceneNo) {
        const std::string id = "pl:card:" + n->id;
        addHit(id, cr, [this, nid = n->id] { selectCard(nid); });
        {
            Drop drop{Drop::NodeRow, cr, n->id, n->parentId};
            drop.horizontal = !columns;   // rows of cards: before or after by left and right halves; columns: top and bottom
            addDrop(drop);
        }
        const bool sel = pl_.node == n->id;
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
            if (pl_.node != nid) selectCard(nid);
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
        if (hoverAmount(sid) > 0.01f) gfx_.fillRounded(stR, 4, statusColour(n->status).withAlpha(0.12f * hoverAmount(sid)));
        gfx_.text(st, {ix, cr.y + 12, iw, 16}, Font::Label, statusColour(n->status), {Align::Right, false});
        // The title: click to rename it here.
        const Rect titleR{ix - 4, cr.y + 27, iw + 6, 24};
        const bool editingTitle = sel && pl_.cardEdit == FName;
        const bool editingSyn = sel && pl_.cardEdit == FSynopsis;
        if (!editingTitle) {
            const std::string tid = "pl:card:" + n->id + "|title";
            addHit(tid, titleR, [this, nid = n->id] {
                if (pl_.node != nid) selectCard(nid);
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
                if (pl_.node != nid) selectCard(nid);
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
                if (pl_.node != nid) selectCard(nid);
                chooseViewpoint(nid);
            });
            tooltip(vid, L"Choose whose eyes it's seen through");
            const float vh = hoverAmount(vid);
            if (vh > 0.01f) gfx_.fillRounded(vr, 4, pal_.ink.withAlpha(0.05f * vh));
            if (who.empty() && (sel || vh > 0.01f)) who = L"Choose a viewpoint…";
            if (!who.empty()) {
                float wx = ix;
                if (hasPov) { gfx_.circle(ix + 4, whoY + 9, 4, pc, true); wx += 13; }
                gfx_.text(who, {wx, whoY, iw - (wx - ix), 18}, Font::Small, hasPov ? pal_.ink : pal_.inkFaint, {Align::Left, false, true});
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
        gfx_.text(len, {ix, fy, iw, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
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
    gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
    gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
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
        y += gfx_.text(L"Novels are often 70,000 to 100,000 words; novellas 20,000 to 40,000. A chapter's or scene's own target "
                       L"is set on its card.",
                       {c.x, y, c.w, 80}, Font::Small, pal_.inkMuted) + 14;
        planPanelExtras(c, y);
        panelHeading(c, y, pl_.view == 0 ? L"THE CORKBOARD" : pl_.view == 1 ? L"ABOUT THE TIMELINE" : L"ABOUT STRUCTURE", false);
        y += gfx_.text(pl_.view == 0 ? L"Every chapter and scene as an index card, in the order of the book. The pin is its status: "
                       L"outline, draft, revised or final. Click a card's title or synopsis to write it there, its status word to change "
                       L"it, and its viewpoint line to choose whose eyes it's seen through.\n\nDrag a card to move it: beside another card "
                       L"to go before or after it, onto the middle of a chapter to make a scene part of it, or (in columns) to the foot "
                       L"of a part's column. Ctrl and the mouse wheel make the cards bigger or smaller.\n\nA card shows who's there: those you "
                       L"choose under Who's there in its panel, or until you choose, the bible's characters its text names."
                       : pl_.view == 1 ? L"Choose a card to set when it happens, or drag it along. Cards without a time wait below; drag one "
                                         L"onto the timeline. Lanes can follow the viewpoint, the plot threads or where it happens (set in a "
                                         L"card's panel). The bible's events can be dragged on too, and stay for this book."
                       : L"Match each beat to the chapters that carry it, and select a beat to keep notes on it for this book. The "
                         L"bar shows the book by length, so you can see whether a turning point comes early or late.",
                       {c.x, y, c.w, 200}, Font::Small, pal_.inkMuted) + 14;
    }
    pl_.panelContent = y + pl_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

}  // namespace nrs

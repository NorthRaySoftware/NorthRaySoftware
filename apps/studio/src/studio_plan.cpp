// The Planning workspace: a book's chapters and scenes as cards on a board, in
// manuscript order and grouped by part, with each one's status, synopsis,
// length against its target, viewpoint character and the world's entries that
// appear in it. The panel edits the selected card, or the book's word target.
#include "studio_app.hpp"

#include <algorithm>
#include <cmath>
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
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, owner, nullptr);
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
void StudioApp::setBookTarget(const Id& bookId, std::int64_t words) {
    json j = json::parse(lib_->setting("plan:" + bookId), nullptr, false);
    if (!j.is_object()) j = json::object();
    j["targetWords"] = std::max<std::int64_t>(0, words);
    lib_->setSetting("plan:" + bookId, j.dump());
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

    // Filter by status.
    gfx_.text(L"STATUS", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    auto row = [&](int status, const std::wstring& label, int count, Color dot) {
        const std::string id = "side:pl:status:" + std::to_string(status);
        const Rect rr{x, y, wd, 30};
        addHit(id, rr, [this, status] { pl_.status = status; pl_.scroll = 0; invalidate(); });
        const bool sel = pl_.status == status;
        const float hv = hoverAmount(id);
        if (sel) { gfx_.fillRounded(rr, 3, pal_.surface); gfx_.fill({rr.x, rr.y + 6, 3, rr.h - 12}, pal_.ribbon); }
        else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
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
    y += 20;
    gfx_.text(L"ALSO", {tx0, y, cw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
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
    sub += L". Select a card to plan it; the panel on the right edits it.";
    float y = m.y + pageHeader(m, L"PLANNING", w(b->title), sub);
    if (all.empty()) {
        y += gfx_.text(L"No chapters yet.", {m.x, y, cw, 24}, Font::Body, pal_.inkMuted) + 12;
        button("pl:first", {m.x, y, 160, 34}, L"Add a chapter", [this] {
            const Id id = lib_->createNode(pl_.book, {}, NodeKind::Chapter);
            if (!id.empty()) selectCard(id);
        }, true);
        y += 50;
    }

    // Who's who, for viewpoint and appearances.
    auto entryName = [&](const Id& id) -> const Entry* {
        const Entry* e = lib_->entry(id);
        return e && !e->deleted ? e : nullptr;
    };
    const float gap = 14, cardW = 232, cardH = 166;
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
        if (pl_.status >= 0 && (int)n->status != pl_.status) continue;
        const Rect cr{m.x + col * (cardWid + gap), y, cardWid, cardH};
        const std::string id = "pl:card:" + n->id;
        addHit(id, cr, [this, nid = n->id] { selectCard(nid); });
        const bool sel = pl_.node == n->id;
        const float hv = hoverAmount(id);
        softShadow(cr, 4, std::max(hv, sel ? 1.0f : 0.0f));
        gfx_.fillRounded(cr, 4, n->kind == NodeKind::Scene ? pal_.surface.mix(pal_.paper, 0.35f) : pal_.surface);
        gfx_.strokeRounded(cr, 4, sel ? pal_.ribbon : pal_.rule.mix(pal_.inkFaint, hv), sel ? 2.0f : 1.0f);
        gfx_.fill({cr.x + 1, cr.y + 10, 3, cr.h - 20}, statusColour(n->status));
        const float ix = cr.x + 16, iw = cr.w - 30;
        std::wstring kicker = n->kind == NodeKind::Chapter ? L"CHAPTER " + std::to_wstring(chapterNo)
                                                           : std::to_wstring(chapterNo) + L" · SCENE " + std::to_wstring(sceneNo);
        gfx_.text(kicker, {ix, cr.y + 12, iw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false, true});
        std::wstring st = w(label(n->status));
        for (auto& ch : st) ch = (wchar_t)towupper(ch);
        gfx_.text(st, {ix, cr.y + 12, iw, 16}, Font::Label, statusColour(n->status), {Align::Right, false});
        gfx_.text(w(n->title.empty() ? std::string(label(n->kind)) : n->title), {ix, cr.y + 30, iw, 22}, Font::BodyStrong, pal_.ink,
                  {Align::Left, false, true});
        // The synopsis, or a prompt for one.
        const Rect syn{ix, cr.y + 54, iw, 64};
        gfx_.pushClip(syn);
        if (n->synopsis.empty()) gfx_.text(L"No synopsis yet.", syn, Font::Small, pal_.inkFaint);
        else gfx_.text(shortened(w(n->synopsis), 150), syn, Font::Small, pal_.inkMuted);
        gfx_.popClip();
        // Viewpoint and who appears.
        float fy = cr.y + 122;
        std::wstring who;
        if (const Entry* pov = entryName(n->povId)) who = L"Viewpoint: " + w(pov->name);
        if (auto it = pl_.appearances.find(n->id); it != pl_.appearances.end()) {
            std::wstring others;
            int shown = 0, more = 0;
            for (const Id& eid : it->second) {
                const Entry* e = entryName(eid);
                if (!e || e->kind != "character" || eid == n->povId) continue;
                if (shown < 3) { others += (shown ? L", " : L"") + w(e->name); ++shown; }
                else ++more;
            }
            if (more) others += L" +" + std::to_wstring(more);
            if (!others.empty()) who += (who.empty() ? L"With " : L" · with ") + others;
        }
        if (!who.empty()) gfx_.text(who, {ix, fy, iw, 18}, Font::Small, pal_.ink, {Align::Left, false, true});
        // Length against the target.
        fy = cr.bottom() - 22;
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
        if (++col == cols) newRow();
    }
    newRow();
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
    if (n) {
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
            panelChoice("pl:pov", c, y, L"Viewpoint character", povLabel, [this, people] {
                const ManuscriptNode* x = lib_->node(pl_.node);
                if (!x) return;
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
                ManuscriptNode u = *lib_->node(pl_.node);
                u.povId = i == 0 ? Id{} : people[i - 1]->id;
                lib_->updateNode(u);
                invalidate();
            });
            y = panelField(c, y, L"Target words (blank: none)", FTarget);
        }
        y = panelField(c, y, L"Synopsis", FSynopsis, 150);
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
            lx += lw + 18;
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
        panelHeading(c, y, L"THE BOARD", false);
        y += gfx_.text(L"Every chapter and scene, in the order of the book. The coloured edge is its status: outline, draft, "
                       L"revised or final.\n\nCharacters from the world's bible are found in the text, so a card shows who appears "
                       L"in it without being told.",
                       {c.x, y, c.w, 200}, Font::Small, pal_.inkMuted) + 14;
    }
    pl_.panelContent = y + pl_.panelScroll - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

}  // namespace nrs

// A chapter's or scene's own page in Planning (0.90): its opening as the book sets it (the header picture and title),
// a strip of facts (date and moon, viewpoint, place, status, words against the target), then its synopsis, notes,
// scenes and text on the left, and when it happens, who's there, threads, beats and its header picture on the right.
// Everything is edited through the same fields as the quick panel (Focus::Node), so the card, the other views and
// Writing stay in step. ‹ › step through every card in the corkboard's order; Back (or Esc) returns to the board.
#include "studio_app.hpp"
#include "cjui/menu.hpp"
#include "studio/document.hpp"
#include "studio/planning.hpp"

#include <algorithm>
#include <cmath>

namespace nrs {

using namespace studio;
namespace plan = studio::plan;
namespace cal = studio::cal;

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

static const NodeStatus kPageStatuses[] = {NodeStatus::Outline, NodeStatus::Draft, NodeStatus::Revised, NodeStatus::Final};

bool StudioApp::clickOpensPage() const { return planPref(pl_.book, "click", "page") != "panel"; }

void StudioApp::cardClick(const Id& nodeId) {
    if (clickOpensPage()) openPlanPage(nodeId);
    else selectCard(nodeId);
}

void StudioApp::cardOtherClick(const Id& nodeId) {
    if (clickOpensPage()) selectCard(nodeId);
    else openPlanPage(nodeId);
}

// Selected for a change made on the card itself (its title, synopsis, status or viewpoint), without the details.
void StudioApp::selectQuiet(const Id& nodeId) {
    commitFields();
    if (pl_.node != nodeId) pl_.cardEdit = -1;
    pl_.node = nodeId;
    pl_.panelOpen = !clickOpensPage();
    setFocus(nodeId.empty() ? Focus{Focus::Plan, pl_.book, {}} : Focus{Focus::Node, nodeId, {}});
    invalidate();
}

void StudioApp::openPlanPage(const Id& nodeId) {
    const ManuscriptNode* n = lib_->node(nodeId);
    if (!n || n->deleted || n->kind == NodeKind::Part) return;
    if (ie_.field >= 0) endInline(true);
    commitFields();
    pl_.page = nodeId;
    pl_.node = nodeId;
    pl_.panelOpen = false;
    pl_.cardEdit = -1;
    pl_.pageScroll = 0;
    pl_.pageTextAll = false;
    setFocus({Focus::Node, nodeId, {}});
    invalidate();
}

void StudioApp::closePlanPage() {
    if (ie_.field >= 0) endInline(true);
    commitFields();
    pl_.page.clear();
    pl_.node.clear();
    pl_.panelOpen = true;
    setFocus({Focus::Plan, pl_.book, {}});
    invalidate();
}

// Every card in the corkboard's order: chapters and scenes.
std::vector<Id> StudioApp::planPageOrder() const {
    std::vector<Id> out;
    for (const ManuscriptNode* n : lib_->manuscriptOf(pl_.book))
        if (n->kind != NodeKind::Part) out.push_back(n->id);
    return out;
}

void StudioApp::stepPlanPage(int dir) {
    const auto order = planPageOrder();
    auto it = std::find(order.begin(), order.end(), pl_.page);
    if (it == order.end()) return;
    const long i = (long)(it - order.begin()) + dir;
    if (i < 0 || i >= (long)order.size()) return;
    openPlanPage(order[(size_t)i]);
}

void StudioApp::drawPlanPage(const Rect& r) {
    const ManuscriptNode* n = lib_->node(pl_.page);
    if (!n) return;
    const Id node = n->id;
    if (!(focus_ == loadedFocus_)) loadFields();
    panelRect_ = r;   // the fields are placed while on screen here
    gfx_.pushClip(r);
    setHitClip(r);
    const float maxW = std::min(r.w - 64, 1120.0f);
    const Rect m{r.x + (r.w - maxW) / 2, r.y, maxW, r.h};
    float y = r.y + 18 - pl_.pageScroll;
    auto entryOf = [&](const Id& id) -> const Entry* {
        const Entry* e = id.empty() ? nullptr : lib_->entry(id);
        return e && !e->deleted ? e : nullptr;
    };
    auto openBible = [this](const Id& id) {
        const Entry* x = lib_->entry(id);
        if (!x) return;
        commitFields();
        openWorldbuilding(x->worldId, x->bookId);
        openEntry(id);
    };

    // ---- The top row: back, where it is, ‹ ›, Open in Writing.
    const auto order = planPageOrder();
    const long at = (long)(std::find(order.begin(), order.end(), node) - order.begin());
    {
        float bw = buttonWidth(L"‹ Corkboard");
        button("plp:back", {m.x, y, bw, 32}, L"‹ Corkboard", [this] { closePlanPage(); });
        tooltip("plp:back", L"Back to the corkboard   Esc");
        // Which chapter (and scene) it is.
        int chapterNo = 0, sceneNo = 0, chNoOf = 0, scNoOf = 0, chapters = 0;
        for (const ManuscriptNode* x : lib_->manuscriptOf(pl_.book)) {
            if (x->kind == NodeKind::Chapter && x->matter == 0) { ++chapterNo; sceneNo = 0; ++chapters; }
            else if (x->kind == NodeKind::Scene) ++sceneNo;
            if (x->id == node) { chNoOf = chapterNo; scNoOf = sceneNo; }
        }
        std::wstring where;
        if (n->kind == NodeKind::Scene) where = L"SCENE " + std::to_wstring(scNoOf) + L" OF CHAPTER " + std::to_wstring(chNoOf);
        else if (n->matter != 0) where = n->matter == 1 ? L"FRONT MATTER" : L"BACK MATTER";
        else where = L"CHAPTER " + std::to_wstring(chNoOf) + L" OF " + std::to_wstring(chapters);
        const float right = m.right();
        const float ww = buttonWidth(L"Open in Writing");
        button("plp:write", {right - ww, y, ww, 32}, L"Open in Writing", [this, node] {
            commitFields();
            writingBook_ = pl_.book;
            currentNode_ = node;
            openWriting(pl_.book);
        }, true);
        const float nx = right - ww - 12 - 2 * 36 - 6;
        if (at > 0) button("plp:prev", {nx, y, 36, 32}, L"‹", [this] { stepPlanPage(-1); });
        else disabledButton({nx, y, 36, 32}, L"‹");
        if (at + 1 < (long)order.size()) button("plp:next", {nx + 42, y, 36, 32}, L"›", [this] { stepPlanPage(1); });
        else disabledButton({nx + 42, y, 36, 32}, L"›");
        tooltip("plp:prev", L"The card before");
        tooltip("plp:next", L"The card after");
        const Rect wr{m.x + bw + 16, y, std::min(gfx_.measureWidth(where, Font::Label) + 4, nx - (m.x + bw + 16) - 12), 32};
        if (n->kind == NodeKind::Scene && !n->parentId.empty()) {
            // A scene's chapter, one click away.
            addHit("plp:chapter", wr, [this, ch = n->parentId] { openPlanPage(ch); });
            tooltip("plp:chapter", L"Open its chapter's page");
            if (hoverAmount("plp:chapter") > 0.01f) gfx_.line(wr.x, wr.y + 24, wr.right() - 4, wr.y + 24, gold_.withAlpha(hoverAmount("plp:chapter")), 1);
        }
        gfx_.text(where, {wr.x, wr.y, wr.w + 40, 32}, Font::Label, gold_, {Align::Left, false, true, true});
        y += 52;
    }

    // ---- The opening, as the book sets it: the header picture with the title under, above or over it.
    const headers::BookHeaders hb = headers::read(*lib_, pl_.book);
    const auto head = headers::resolve(*lib_, *n, hb);
    {
        const std::wstring title = n->title.empty() ? std::wstring(w(label(n->kind))) : w(n->title);
        const float titleH = 48;
        auto titleAt = [&](float ty, cjui::Color ink) {
            const Rect tr{m.x + 40, ty, m.w - 80, titleH};
            if (editingInline("plp:title")) { inlineBox(tr); return; }
            editableArea("plp:title", tr, L"Click to rename it", [this, before = *n] {
                startInline("plp:title", FName, [this, before] { lib_->updateNode(before); }, true);
            });
            gfx_.text(title, tr, Font::SerifTitle, ink, {Align::Center, false, true, true});
        };
        if (head && !head->header.blob.empty()) {
            const headers::Header& h = head->header;
            const std::wstring path = w(lib_->blobPath(h.blob));
            const auto [iw, ih] = gfx_.imageSize(path);
            const float aspect = iw > 0 ? (float)ih / iw : 0.4f;
            float pw = m.w, ph = std::min(m.w / 3.2f, 260.0f);
            if (h.size == "full") ph = std::min(m.w * aspect, 380.0f);
            else if (h.size == "ornament") { pw = 150; ph = std::min(150 * aspect, 150.0f); }
            if (h.title == "over") ph = std::max(ph, 150.0f);
            Rect pr{m.x + (m.w - pw) / 2, y, pw, ph};
            if (h.title == "above") { titleAt(y, pal_.ink); y += titleH + 14; pr.y = y; }
            if (!gfx_.drawImageRounded(path, pr, 12)) gfx_.fillRounded(pr, 12, pal_.sunken);
            if (h.title == "over") {
                const bool dark = h.tone != "fade";
                gfx_.fillRounded(pr, 12, dark ? cjui::Color{0, 0, 0, 0.55f} : pal_.paper.withAlpha(0.7f));
                titleAt(pr.y + (pr.h - titleH) / 2, dark ? cjui::Color{1, 1, 1, 1} : pal_.ink);
                y = pr.bottom() + 14;
            } else {
                y = pr.bottom() + 14;
                if (h.title != "above") { titleAt(y, pal_.ink); y += titleH; }
            }
        } else if (head && headers::printableSymbol(head->header.symbol)) {
            const bool above = head->header.title == "above";
            if (above) { titleAt(y, pal_.ink); y += titleH + 6; }
            drawSymbol(head->header.symbol, {m.x + m.w / 2 - 22, y, 44, 44}, pal_.ink, cjui::Icon::Star);
            y += 52;
            if (!above) { titleAt(y, pal_.ink); y += titleH; }
        } else {
            titleAt(y, pal_.ink);
            y += titleH;
        }
        if (head) {
            std::wstring from = head->from == "own" ? L"Its own header" : head->from == "viewpoint" ? L"Its viewpoint's header"
                              : head->from == "parts" ? L"The book's header for parts" : L"The book's header for chapters";
            gfx_.text(from + L" · change it under Header picture", {m.x, y + 2, m.w, 16}, Font::Small, pal_.inkFaint, {Align::Center, false});
            y += 22;
        }
    }

    // ---- The facts: date and moon, viewpoint, place, status, words.
    {
        y += 8;
        struct Chip { std::string id; float w; std::function<void(const Rect&)> draw; std::function<void()> act; std::wstring tip; };
        std::vector<Chip> chips;
        // The date.
        const auto times = storyTimes(pl_.book);
        const auto ti = times.find(node);
        if (ti != times.end() && ti->second.placed) {
            std::wstring dt = storyDayText(pl_.book, ti->second.day);
            const cal::Calendar cc = cal::calendarOfBook(*lib_, pl_.book);
            if (ti->second.minutes >= 0 || !ti->second.part.empty())
                dt += L", " + w(cal::timeOfDayText(cc.empty() ? cal::gregorian() : cc, {ti->second.minutes, ti->second.part}));
            const bool inCal = timelineInCalendar(pl_.book) && !cc.empty();
            std::int64_t day = 0;
            if (inCal) day = cal::storySpan(*lib_, pl_.book).start + (std::int64_t)std::floor(ti->second.day);
            const float tw = gfx_.measureWidth(dt, Font::Body);
            chips.push_back({"plp:date", tw + (inCal ? 64.0f : 24.0f), [this, dt, inCal, day, cc](const Rect& cr) {
                float x = cr.x + 10;
                if (inCal) {
                    const cal::Date d = cal::fromDay(cc, day);
                    drawDateChip({x, cr.y + 4, 24, 24}, d.day, w(cal::monthLabel(cc, d.month)));
                    x += 30;
                    if (!cc.moons.empty()) { drawMoonDisc(x + 5, cr.y + 16, 5.5f, cal::moonPhase(cc, cc.moons[0], day)); x += 16; }
                }
                gfx_.text(dt, {x, cr.y, cr.right() - x, cr.h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
            }, nullptr, L"When it happens (change it under When)"});
        } else {
            chips.push_back({"plp:date", gfx_.measureWidth(L"Not on the timeline", Font::Body) + 24, [this](const Rect& cr) {
                gfx_.text(L"Not on the timeline", cr.inset(12, 0), Font::Body, pal_.inkFaint, {Align::Left, false, true, true});
            }, nullptr, L"Give it a date or day under When"});
        }
        // The viewpoint.
        if (n->kind != NodeKind::Part) {
            if (const Entry* pov = entryOf(n->povId)) {
                const std::wstring nm = w(pov->name);
                chips.push_back({"plp:pov", gfx_.measureWidth(nm, Font::BodyStrong) + 70, [this, pov, nm](const Rect& cr) {
                    drawPortrait(*pov, {cr.x + 4, cr.y + 2, 28, 28}, 14);
                    gfx_.circle(cr.x + 18, cr.y + 16, 15, povColour(pov->id), false, 2);
                    gfx_.text(nm, {cr.x + 40, cr.y, cr.w - 62, cr.h}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
                    gfx_.text(L"▾", {cr.right() - 20, cr.y, 14, cr.h}, Font::Small, pal_.inkMuted, {Align::Center, false, false, true});
                }, [this, node, id = pov->id, openBible] {
                    // The name opens their page; the end of the chip changes who it is.
                    std::vector<cjui::MenuItem> items{cjui::MenuItem::item(L"Open their page in the bible", 1), cjui::MenuItem::item(L"Change the viewpoint…", 2)};
                    POINT pt;
                    GetCursorPos(&pt);
                    const UINT c = cjui::showMenu(hwnd(), items, pt, pal_);
                    if (c == 1) openBible(id);
                    else if (c == 2) chooseViewpoint(node);
                }, L"Whose eyes it's seen through"});
            } else {
                chips.push_back({"plp:pov", gfx_.measureWidth(L"Choose a viewpoint…", Font::Body) + 24, [this](const Rect& cr) {
                    gfx_.text(L"Choose a viewpoint…", cr.inset(12, 0), Font::Body, pal_.inkMuted, {Align::Left, false, true, true});
                }, [this, node] { chooseViewpoint(node); }, L"Whose eyes it's seen through"});
            }
        }
        // The place.
        {
            const auto places = plan::nodePlaces(*lib_, pl_.book);
            const auto pit = places.find(node);
            if (const Entry* pe = pit != places.end() ? entryOf(pit->second) : nullptr) {
                const std::wstring nm = w(pe->name);
                chips.push_back({"plp:place", gfx_.measureWidth(nm, Font::Body) + 64, [this, pe, nm](const Rect& cr) {
                    drawKindMark(pe->kind, {cr.x + 8, cr.y + 6, 20, 20});
                    gfx_.text(nm, {cr.x + 34, cr.y, cr.w - 56, cr.h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
                    gfx_.text(L"▾", {cr.right() - 20, cr.y, 14, cr.h}, Font::Small, pal_.inkMuted, {Align::Center, false, false, true});
                }, [this, node, id = pe->id, openBible] {
                    std::vector<cjui::MenuItem> items{cjui::MenuItem::item(L"Open its page in the bible", 1), cjui::MenuItem::item(L"Change where it happens…", 2)};
                    POINT pt;
                    GetCursorPos(&pt);
                    const UINT c = cjui::showMenu(hwnd(), items, pt, pal_);
                    if (c == 1) openBible(id);
                    else if (c == 2) choosePlace(node);
                }, L"Where it happens"});
            } else {
                chips.push_back({"plp:place", gfx_.measureWidth(L"Choose a place…", Font::Body) + 24, [this](const Rect& cr) {
                    gfx_.text(L"Choose a place…", cr.inset(12, 0), Font::Body, pal_.inkMuted, {Align::Left, false, true, true});
                }, [this, node] { choosePlace(node); }, L"Where it happens"});
            }
        }
        // The status.
        {
            const std::wstring st = w(label(n->status));
            chips.push_back({"plp:status", gfx_.measureWidth(st, Font::BodyStrong) + 40, [this, st, s = n->status](const Rect& cr) {
                gfx_.circle(cr.x + 14, cr.y + 16, 5, statusColour(s), true);
                gfx_.text(st, {cr.x + 26, cr.y, cr.w - 30, cr.h}, Font::BodyStrong, statusColour(s).mix(pal_.ink, 0.3f), {Align::Left, false, true, true});
            }, [this, node] {
                const ManuscriptNode* x = lib_->node(node);
                if (!x) return;
                std::vector<std::wstring> items;
                for (NodeStatus s2 : kPageStatuses) items.push_back(w(label(s2)));
                const int i = popupAtCursor(hwnd(), items, (int)x->status);
                if (i < 0) return;
                commitFields();
                ManuscriptNode u = *lib_->node(node);
                u.status = kPageStatuses[i];
                lib_->updateNode(u);
                invalidate();
            }, L"How far the writing has got: click to change"});
        }
        // Words against the target.
        {
            std::int64_t words = n->wordCount;
            if (n->kind == NodeKind::Chapter)
                for (const ManuscriptNode* s : lib_->manuscriptOf(pl_.book)) if (s->parentId == node) words += s->wordCount;
            std::wstring t = w(std::to_string(words)) + L" words";
            if (n->targetWords > 0) t += L" of " + w(std::to_string(n->targetWords));
            const double frac = n->targetWords > 0 ? std::min(1.0, (double)words / n->targetWords) : -1;
            chips.push_back({"plp:words", gfx_.measureWidth(t, Font::Body) + (frac >= 0 ? 48.0f : 24.0f), [this, t, frac](const Rect& cr) {
                float x = cr.x + 12;
                if (frac >= 0) { drawProgressRing(x + 9, cr.y + 16, 9, (float)frac); x += 26; }
                gfx_.text(t, {x, cr.y, cr.right() - x, cr.h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
            }, nullptr, n->targetWords > 0 ? L"Words written against its target (set the target at the right)" : L"Words written (give it a target at the right)"});
        }
        // Laid out in centred rows.
        size_t i = 0;
        while (i < chips.size()) {
            float rowW = 0;
            size_t j = i;
            while (j < chips.size() && (j == i || rowW + 10 + chips[j].w <= m.w)) { rowW += (j > i ? 10 : 0) + chips[j].w; ++j; }
            float x = m.x + (m.w - rowW) / 2;
            for (size_t k = i; k < j; ++k) {
                const Rect cr{x, y, chips[k].w, 32};
                if (chips[k].act) addHit(chips[k].id, cr, chips[k].act);
                else addHit(chips[k].id, cr, nullptr, false);
                tooltip(chips[k].id, chips[k].tip);
                const float hv = chips[k].act ? hoverAmount(chips[k].id) : 0;
                gfx_.fillRounded(cr, 16, pal_.surface);
                gfx_.strokeRounded(cr, 16, pal_.rule.mix(pal_.inkFaint, 0.3f * hv), 1);
                chips[k].draw(cr);
                if (focused(chips[k].id)) focusRing(cr);
                x += chips[k].w + 10;
            }
            y += 42;
            i = j;
        }
        y += 14;
    }

    // ---- Two columns when there's room: the writing on the left, the facts on the right.
    const bool two = m.w >= 860;
    const float gap = 36;
    const float sideW = two ? std::min(360.0f, m.w * 0.36f) : m.w;
    const Rect left{m.x, 0, two ? m.w - sideW - gap : m.w, 0};
    const Rect side{two ? m.right() - sideW : m.x, 0, sideW, 0};
    const float top = y;
    float ly = y;
    // Synopsis and notes.
    {
        Rect c = left;
        panelHeading(c, ly, L"SYNOPSIS", true);
        ly = panelField(c, ly, L"What happens, in a few lines", FSynopsis, 120);
        panelHeading(c, ly, L"NOTES", false);
        ly = panelField(c, ly, L"Anything else: ideas, research, what to fix, what it must set up", FNotes, 260);
    }
    // Its scenes, as small cards.
    if (n->kind == NodeKind::Chapter) {
        std::vector<const ManuscriptNode*> scenes;
        for (const ManuscriptNode* s : lib_->manuscriptOf(pl_.book)) if (s->parentId == node && s->kind == NodeKind::Scene) scenes.push_back(s);
        if (!scenes.empty()) {
            Rect c = left;
            panelHeading(c, ly, L"SCENES", false);
            const float cw = 210, ch = 104;
            float x = left.x;
            int k = 0;
            for (const ManuscriptNode* s : scenes) {
                if (x + cw > left.right() + 1 && x > left.x) { x = left.x; ly += ch + 12; }
                const Rect cr{x, ly, cw, ch};
                const std::string id = "plp:scene:" + s->id;
                addHit(id, cr, [this, sid = s->id] { openPlanPage(sid); });
                tooltip(id, L"Open this scene's page");
                const float hv = hoverAmount(id);
                softShadow(cr, 6, 0.6f + 0.6f * hv);
                gfx_.fillRounded(cr, 6, pal_.dark ? pal_.surface : cjui::hexColor("#FFFDF7"));
                if (const Entry* pov = entryOf(s->povId)) gfx_.fillRounded({cr.x, cr.y, cr.w, 4}, 2, povColour(pov->id));
                gfx_.text(L"SCENE " + std::to_wstring(++k), {cr.x + 12, cr.y + 10, cr.w - 24, 14}, Font::Label, gold_, {Align::Left, false});
                gfx_.text(w(s->title), {cr.x + 12, cr.y + 26, cr.w - 24, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
                std::wstring sub;
                if (const Entry* pov = entryOf(s->povId)) sub = w(pov->name) + L" · ";
                sub += w(label(s->status));
                gfx_.text(sub, {cr.x + 12, cr.y + 46, cr.w - 24, 16}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
                if (!s->synopsis.empty())
                    gfx_.text(w(s->synopsis), {cr.x + 12, cr.y + 64, cr.w - 24, 34}, Font::Small, pal_.inkFaint, {Align::Left, true, true});
                if (focused(id)) focusRing(cr);
                x += cw + 12;
            }
            ly += ch + 22;
        }
    }
    // Its text, read-only and folded.
    {
        Rect c = left;
        panelHeading(c, ly, L"THE TEXT", false);
        const std::string plain = doc::plainText(lib_->nodeContent(node));
        std::vector<std::wstring> paras;
        for (size_t a = 0; a < plain.size();) {
            size_t b = plain.find("\n\n", a);
            if (b == std::string::npos) b = plain.size();
            std::string p = plain.substr(a, b - a);
            while (!p.empty() && (p.back() == '\n' || p.back() == ' ')) p.pop_back();
            while (!p.empty() && (p.front() == '\n' || p.front() == ' ')) p.erase(p.begin());
            if (!p.empty()) paras.push_back(w(p));
            a = b + 2;
        }
        if (paras.empty()) {
            bool hasScenes = false;
            for (const ManuscriptNode* s : lib_->manuscriptOf(pl_.book)) if (s->parentId == node && s->kind == NodeKind::Scene) hasScenes = true;
            ly += gfx_.text(hasScenes ? L"Nothing written in the chapter itself yet (its scenes have their own pages)." : L"Nothing written yet.",
                            {left.x, ly, left.w, 40}, Font::Body, pal_.inkFaint) + 12;
        } else {
            const size_t shown = pl_.pageTextAll ? paras.size() : std::min<size_t>(4, paras.size());
            const Rect sheet{left.x, ly, left.w, 0};
            float ty = ly + 22;
            const float tx = sheet.x + 28, tw = sheet.w - 56;
            // The sheet's height first, then the text on it.
            float th = 0;
            for (size_t k = 0; k < shown; ++k) th += gfx_.measureHeight(paras[k], tw, Font::Body) + 10;
            const Rect paper{sheet.x, ly, sheet.w, th + 44};
            gfx_.fillRounded(paper, 8, pal_.dark ? pal_.surface : cjui::hexColor("#FFFDF7"));
            gfx_.strokeRounded(paper, 8, pal_.rule);
            for (size_t k = 0; k < shown; ++k) ty += gfx_.text(paras[k], {tx, ty, tw, 4000}, Font::Body, pal_.ink, {Align::Left, true}) + 10;
            ly = paper.bottom() + 12;
            float bx = left.x;
            if (paras.size() > 4) {
                const std::wstring t = pl_.pageTextAll ? L"Show less" : L"Show all " + std::to_wstring(paras.size()) + L" paragraphs";
                bx += linkButton("plp:textall", bx, ly, t, [this] { pl_.pageTextAll = !pl_.pageTextAll; invalidate(); });
            }
            linkButton("plp:write2", bx, ly, L"Open in Writing", [this, node] {
                commitFields();
                writingBook_ = pl_.book;
                currentNode_ = node;
                openWriting(pl_.book);
            });
            ly += 40;
        }
    }

    // ---- The right: when, target, who's there, threads, beats, header picture.
    float ry = two ? top : ly + 10;
    {
        Rect c = side;
        panelHeading(c, ry, L"WHEN", true);
        const bool inCal = timelineInCalendar(pl_.book);
        ry = panelField(c, ry, inCal ? L"Date (blank: not on the timeline)" : L"Story day (blank: not on the timeline)", FStoryDay, 34);
        if (inCal) ry = panelField(c, ry, L"Time of day (14:30, dawn, evening)", FTimeOfDay, 34);
        ry = panelField(c, ry, L"When, in words", FWhen, 34);
        ry = panelField(c, ry, L"Target words (blank: none)", FTarget, 34);
        // Who's there, as portraits.
        panelHeading(c, ry, L"WHO'S THERE", false);
        const auto casts = plan::nodeCast(*lib_, pl_.book);
        std::vector<Id> cur;
        if (auto it = casts.find(node); it != casts.end()) cur = it->second;
        float px = c.x;
        int shown = 0;
        for (const Id& eid : cur) {
            const Entry* e = entryOf(eid);
            if (!e) continue;
            if (px + 64 > c.right() + 1 && px > c.x) { px = c.x; ry += 84; }
            const Rect pr{px, ry, 64, 78};
            const std::string id = "plp:cast:" + eid;
            addHit(id, pr, [openBible, eid] { openBible(eid); });
            tooltip(id, w(e->name) + L": open their page. Right-click to take them out.");
            const float hv = hoverAmount(id);
            if (hv > 0.01f) gfx_.fillRounded(pr.inset(-2), 8, pal_.surface.withAlpha(0.8f * hv));
            drawPortrait(*e, {pr.x + 10, pr.y + 2, 44, 44}, 22);
            gfx_.text(w(e->name), {pr.x - 2, pr.y + 50, pr.w + 4, 28}, Font::Small, pal_.ink, {Align::Center, true, true});
            px += 72;
            ++shown;
        }
        if (shown) ry += 86;
        else {
            ry += gfx_.text(L"No one chosen yet.", {c.x, ry, c.w, 20}, Font::Small, pal_.inkFaint) + 8;
        }
        linkButton("plp:castadd", c.x, ry, L"Add who's there…", [this, node] { addToCast(node); });
        ry += 42;
        // Plot threads: ticks, and each opens its thread.
        cardThreads(c, ry);
        {
            const plan::Threads th = plan::threadsOf(*lib_, pl_.book);
            if (auto it = th.nodes.find(node); it != th.nodes.end() && !it->second.empty()) {
                float tx = c.x;
                for (const auto& tid : it->second) {
                    const plan::Thread* t = th.find(tid);
                    if (!t) continue;
                    const std::wstring lb = L"Open “" + w(t->name) + L"”";
                    const float lw = gfx_.measureWidth(lb, Font::Small) + 30;
                    if (tx + lw > c.right() && tx > c.x) { tx = c.x; ry += 34; }
                    tx += linkButton("plp:thread:" + tid, tx, ry, lb, [this, tid] {
                        commitFields();
                        pl_.page.clear();
                        pl_.view = 3;
                        selectThread(tid);
                    });
                }
                ry += 40;
            }
        }
        // The beats it carries.
        {
            const plan::BookStructure bs = plan::bookStructure(*lib_, pl_.book);
            const auto all = plan::allStructures(*lib_, plan::structureScope(*lib_, pl_.book));
            if (const plan::Structure* st = plan::findStructure(all, bs.templateId)) {
                panelHeading(c, ry, L"BEAT", false);
                int any = 0;
                for (const auto& beat : st->beats) {
                    auto it = bs.beats.find(beat.id);
                    if (it == bs.beats.end() || std::find(it->second.begin(), it->second.end(), node) == it->second.end()) continue;
                    ++any;
                    ry += 2;
                    linkButton("plp:beat:" + beat.id, c.x, ry, w(beat.name), [this, bid = beat.id] {
                        commitFields();
                        pl_.page.clear();
                        pl_.view = 2;
                        selectBeat(bid);
                    });
                    ry += 36;
                }
                if (!any) ry += gfx_.text(L"It carries none of " + w(st->name) + L"'s beats. Choose them in Structure.", {c.x, ry, c.w, 40}, Font::Small, pal_.inkFaint) + 8;
            }
        }
        // Its header picture (chapters: a scene opens with none, 0.89).
        if (n->kind == NodeKind::Chapter) {
        panelHeading(c, ry, L"HEADER PICTURE", false);
        const auto ownIt = hb.nodes.find(node);
        const headers::Header own = ownIt == hb.nodes.end() ? headers::Header{} : ownIt->second;
        headers::BookHeaders without = hb;
        without.nodes.erase(node);
        headerChooser("plp:head", c, ry, own, true, headers::resolve(*lib_, *n, without), w(n->title), [this, node](const headers::Header& h) {
            commitFields();
            headers::BookHeaders b = headers::read(*lib_, pl_.book);
            if (h.none || !h.empty()) b.nodes[node] = h;
            else b.nodes.erase(node);
            headers::write(*lib_, pl_.book, b);
            invalidate();
        });
        ry += 10;
        }
    }
    const float bottom = std::max(ly, ry) + 40;
    pl_.pageContent = bottom + pl_.pageScroll - r.y;
    clearHitClip();
    gfx_.popClip();
}

}  // namespace nrs

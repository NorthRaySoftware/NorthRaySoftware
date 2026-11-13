// Drawing: workspace bar, library sidebar, pages, properties panel.
#include "studio_app.hpp"

#include "cjui/format.hpp"
#include "cjui/http.hpp"
#include "version.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace nrs {

using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }

static std::wstring plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

// ------------------------------------------------------------------- frame

void StudioApp::drawFrame(float W, float H) {
    drops_.clear();
    const bool panel = settings_.showPanel && lib_ && !(ws_ == Workspace::Library && view_ == View::Settings);
    const float centreW = W - kSideW - (panel ? kPanelW : 0);
    sideRect_ = {0, kTopH, kSideW, H - kTopH - kStatusH};
    panelRect_ = panel ? Rect{W - kPanelW, kTopH, kPanelW, H - kTopH - kStatusH} : Rect{};
    pageRect_ = {kSideW, kTopH, centreW, H - kTopH - kStatusH};

    gfx_.fill({0, 0, W, H}, pal_.paper);
    const float pad = centreW > 900 ? 40 : 28;
    if (ws_ != Workspace::Preview) preview_.hide();
    if (ws_ == Workspace::Preview && lib_ && lib_->book(pv_.book)) {
        editor_.hide();
        gfx_.fill(pageRect_, pal_.dark ? hexColor("#121214") : hexColor("#DCD7CC"));
        if (!previewNote_.empty()) {
            const Rect m{kSideW + pad, kTopH + 26, std::min(centreW - pad * 2, 620.0f), 0};
            float y = m.y + pageHeader(m, L"PREVIEW", L"The ebook preview isn't available", previewNote_);
            linkText("pv:webview", m.x, y - 6, L"Get the Microsoft Edge WebView2 Runtime",
                     [] { cjui::openInBrowser("https://developer.microsoft.com/microsoft-edge/webview2/"); });
            preview_.hide();
        } else {
            if (!preview_.ready()) gfx_.text(L"Opening the preview...", pageRect_, Font::Body, pal_.inkFaint, {Align::Center, false, false, true});
            const float s = gfx_.dpiScale();
            RECT rc{(LONG)std::lround(pageRect_.x * s), (LONG)std::lround(pageRect_.y * s), (LONG)std::lround(pageRect_.right() * s),
                    (LONG)std::lround(pageRect_.bottom() * s)};
            preview_.place(rc);
        }
        drawPreviewSidebar(sideRect_);
        if (panel) drawPreviewPanel(panelRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H - kStatusH, W, kStatusH});
        hideUnplacedFields();
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Preview) ws_ = Workspace::Library;
    if (ws_ == Workspace::Typesetting && lib_ && lib_->book(ts_.book)) {
        editor_.hide();
        drawTypesetPages(pageRect_);
        drawTypesetSidebar(sideRect_);
        if (panel) drawTypesetPanel(panelRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H - kStatusH, W, kStatusH});
        hideUnplacedFields();
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Typesetting) ws_ = Workspace::Library;   // its book went to the Trash
    if (ws_ == Workspace::Publishing && lib_ && lib_->book(pb_.book)) {
        editor_.hide();
        gfx_.fill(pageRect_, pal_.paper);
        drawPublishPage(pageRect_);
        drawPublishSidebar(sideRect_);
        if (panel) drawPublishPanel(panelRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H - kStatusH, W, kStatusH});
        hideUnplacedFields();
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Publishing) ws_ = Workspace::Library;
    if (ws_ == Workspace::Worldbuilding && lib_ && (wb_.world.empty() ? lib_->book(wb_.book) && !lib_->book(wb_.book)->deleted
                                                                       : lib_->world(wb_.world) && !lib_->world(wb_.world)->deleted)) {
        editor_.hide();
        gfx_.fill(pageRect_, pal_.paper);
        drawWorldPage(pageRect_);
        drawWorldSidebar(sideRect_);
        if (panel) drawWorldPanel(panelRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H - kStatusH, W, kStatusH});
        hideUnplacedFields();
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Worldbuilding) ws_ = Workspace::Library;
    if (ws_ == Workspace::Planning && lib_ && lib_->book(pl_.book) && !lib_->book(pl_.book)->deleted) {
        editor_.hide();
        gfx_.fill(pageRect_, pal_.paper);
        drawPlanBoard(pageRect_);
        drawPlanSidebar(sideRect_);
        if (panel) drawPlanPanel(panelRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H - kStatusH, W, kStatusH});
        hideUnplacedFields();
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Planning) ws_ = Workspace::Library;
    if (ws_ == Workspace::Review && lib_ && lib_->book(rv_.book) && !lib_->book(rv_.book)->deleted) {
        editor_.hide();
        gfx_.fill(pageRect_, pal_.paper);
        drawReviewPage(pageRect_);
        drawReviewSidebar(sideRect_);
        if (panel) drawReviewPanel(panelRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H - kStatusH, W, kStatusH});
        hideUnplacedFields();
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Review) ws_ = Workspace::Library;
    if (ws_ == Workspace::Writing && lib_ && lib_->book(writingBook_)) {
        // The editor fills the centre; it's a separate window placed over this area.
        const Rect m{kSideW + pad, kTopH + 26, std::min(centreW - pad * 2, 620.0f), 0};
        if (!editorNote_.empty()) {
            float y = m.y + pageHeader(m, L"WRITING", L"The editor isn't available", editorNote_);
            linkText("ed:webview", m.x, y - 6, L"Get the Microsoft Edge WebView2 Runtime",
                     [] { cjui::openInBrowser("https://developer.microsoft.com/microsoft-edge/webview2/"); });
        } else if (currentNode_.empty()) {
            pageHeader(m, L"WRITING", L"Choose a chapter", L"Pick a chapter or scene in the outline on the left, or press + Chapter.");
        } else if (!editor_.ready()) {
            gfx_.text(L"Opening the editor...", {m.x, m.y + 20, 400, 30}, Font::Body, pal_.inkFaint);
        }
        placeEditor();
        drawBinder(sideRect_);
        if (panel) drawPanel(panelRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H - kStatusH, W, kStatusH});
        hideUnplacedFields();
        if (drag_.active) {
            if (const Drop* d = dropAt(mouseX(), mouseY())) {
                const float third = d->rect.h / 3;
                if (mouseY() > d->rect.y + third && mouseY() < d->rect.bottom() - third) gfx_.strokeRounded(d->rect.inset(1), 3, pal_.ribbon, 2);
                else gfx_.fill({d->rect.x, mouseY() < d->rect.y + d->rect.h / 2 ? d->rect.y - 2 : d->rect.bottom(), d->rect.w, 3}, pal_.ribbon);
            }
            const float lw = std::min(gfx_.measureWidth(drag_.label, Font::BodyStrong) + 24, 320.0f);
            const Rect g{mouseX() + 14, mouseY() + 10, lw, 30};
            gfx_.fillRounded(g, 3, pal_.ink.withAlpha(0.88f));
            gfx_.text(drag_.label, g.inset(12, 0), Font::BodyStrong, pal_.paper, {Align::Left, false, true, true});
        }
        lastDrops_ = drops_;
        return;
    }
    editor_.hide();
    const float top = beginScroll(pageRect_);
    Rect page{kSideW + pad, top + 26, std::min(centreW - pad * 2, 1100.0f), 0};
    float h = 0;
    if (!lib_) h = pageEmpty(page);
    else if (ws_ == Workspace::Import) h = pageImport(page);
    else switch (view_) {
        case View::AllBooks: h = pageAllBooks(page); break;
        case View::World: h = lib_->world(viewId_) ? pageWorld(page) : pageAllBooks(page); break;
        case View::Series: h = lib_->seriesById(viewId_) ? pageSeries(page) : pageAllBooks(page); break;
        case View::Independent: h = pageIndependent(page); break;
        case View::Trash: h = pageTrash(page); break;
        case View::Settings: h = pageSettings(page); break;
        case View::Artwork: h = pageArtwork(page); break;
        case View::Book: h = lib_->book(viewId_) ? pageBook(page) : pageAllBooks(page); break;
    }
    endScroll(h + 60);

    drawSidebar(sideRect_);
    if (panel) drawPanel(panelRect_);
    drawTopBar({0, 0, W, kTopH});
    drawStatusBar({0, H - kStatusH, W, kStatusH});
    hideUnplacedFields();

    // The thing being dragged, and where it would land.
    if (drag_.active) {
        if (const Drop* d = dropAt(mouseX(), mouseY())) {
            if (d->kind == Drop::ToWorld || d->kind == Drop::ToIndependent || d->kind == Drop::ToSeries) {
                gfx_.strokeRounded(d->rect.inset(1), 3, pal_.ribbon, 2);
            } else if (d->horizontal) {
                const float x = mouseX() > d->rect.x + d->rect.w / 2 ? d->rect.right() + 6 : d->rect.x - 8;
                gfx_.fill({x, d->rect.y, 3, d->rect.h}, pal_.ribbon);
            } else {
                const float y = mouseY() > d->rect.y + d->rect.h / 2 ? d->rect.bottom() : d->rect.y - 2;
                gfx_.fill({d->rect.x, y, d->rect.w, 3}, pal_.ribbon);
            }
        }
        const float lw = std::min(gfx_.measureWidth(drag_.label, Font::BodyStrong) + 24, 320.0f);
        const Rect g{mouseX() + 14, mouseY() + 10, lw, 30};
        gfx_.fillRounded(g, 3, pal_.ink.withAlpha(0.88f));
        gfx_.text(drag_.label, g.inset(12, 0), Font::BodyStrong, pal_.paper, {Align::Left, false, true, true});
    }
    lastDrops_ = drops_;
}

// ---------------------------------------------------------------- top bar

void StudioApp::drawTopBar(const Rect& r) {
    gfx_.fill(r, pal_.surface);
    gfx_.line(r.x, r.bottom() - 0.5f, r.right(), r.bottom() - 0.5f, pal_.rule);
    // Product wordmark with the suite's ribbon, in the Studio's ink blue.
    gfx_.bookmark({18, 0, 11, 30}, pal_.ribbon);
    gfx_.text(L"Writing Studio", {38, 0, kSideW - 40, r.h}, Font::Heading, pal_.ink, {Align::Left, false, true, true});

    struct Tab { const wchar_t* name; int milestone; };
    static const Tab tabs[] = {{L"Library", 1}, {L"Writing", 2}, {L"Planning", 8}, {L"Worldbuilding", 8}, {L"Review", 9},
                               {L"Typesetting", 5}, {L"Preview", 6}, {L"Publishing", 7}};
    float x = kSideW + 16;
    for (const Tab& t : tabs) {
        const float tw = gfx_.measureWidth(t.name, Font::BodyStrong) + 24;
        const Rect tr{x, 6, tw, r.h - 12};
        const bool planning = wcscmp(t.name, L"Planning") == 0;   // Milestone 8 has two tabs
        const bool built = t.milestone <= 2 || (t.milestone >= 5 && t.milestone <= 9);
        const bool active = (t.milestone == 1 && ws_ != Workspace::Writing && ws_ != Workspace::Typesetting && ws_ != Workspace::Preview &&
                             ws_ != Workspace::Publishing && ws_ != Workspace::Worldbuilding && ws_ != Workspace::Planning &&
                             ws_ != Workspace::Review &&
                             view_ != View::Settings) ||
                            (t.milestone == 2 && ws_ == Workspace::Writing) || (t.milestone == 5 && ws_ == Workspace::Typesetting) ||
                            (t.milestone == 6 && ws_ == Workspace::Preview) || (t.milestone == 7 && ws_ == Workspace::Publishing) ||
                            (t.milestone == 8 && !planning && ws_ == Workspace::Worldbuilding) ||
                            (t.milestone == 8 && planning && ws_ == Workspace::Planning) || (t.milestone == 9 && ws_ == Workspace::Review);
        const std::string id = std::string("tab:") + narrow(t.name);
        addHit(id, tr, [this, t, planning] {
            if (t.milestone == 1) {
                if (ws_ == Workspace::Writing) writingCommand(WLibrary);  // back to the book's page
                else if (ws_ == Workspace::Typesetting) { ws_ = Workspace::Library; goTo(View::Book, ts_.book); }
                else if (ws_ == Workspace::Preview) { ws_ = Workspace::Library; goTo(View::Book, pv_.book); }
                else if (ws_ == Workspace::Publishing) { ws_ = Workspace::Library; goTo(View::Book, pb_.book); }
                else if (ws_ == Workspace::Planning) { ws_ = Workspace::Library; goTo(View::Book, pl_.book); }
                else if (ws_ == Workspace::Review) { ws_ = Workspace::Library; goTo(View::Book, rv_.book); }
                else if (ws_ == Workspace::Worldbuilding) {
                    ws_ = Workspace::Library;
                    if (!wb_.world.empty()) goTo(View::World, wb_.world); else goTo(View::Book, wb_.book);
                }
                else if (view_ == View::Settings) goTo(View::AllBooks);
            }
            else if ((t.milestone == 8 && planning) || t.milestone == 9) {
                Id book = ws_ == Workspace::Writing && lib_->book(writingBook_) ? writingBook_
                        : ws_ == Workspace::Review ? rv_.book
                        : ws_ == Workspace::Typesetting ? ts_.book : ws_ == Workspace::Preview ? pv_.book : ws_ == Workspace::Publishing ? pb_.book
                        : ws_ == Workspace::Planning ? pl_.book : ws_ == Workspace::Worldbuilding ? wb_.book
                        : focus_.kind == Focus::Book ? focus_.id : view_ == View::Book ? viewId_ : !pl_.book.empty() ? pl_.book : writingBook_;
                if (ws_ == Workspace::Worldbuilding && book.empty()) {
                    // From a world's bible: the book being written in it, else its first.
                    if (lib_->book(writingBook_) && lib_->book(writingBook_)->worldId == wb_.world) book = writingBook_;
                    else for (const studio::Book* x : lib_->liveBooks()) if (x->worldId == wb_.world) { book = x->id; break; }
                }
                if (book.empty() || !lib_->book(book))
                    flash(std::wstring(L"Choose a book in the Library first, then open ") + (planning ? L"Planning." : L"Review."), 5);
                else if (planning) openPlanning(book);
                else openReview(book);
            }
            else if (t.milestone == 8) {
                Id world, book;
                if (!worldbuildingScope(world, book)) flash(L"Choose a world or a book in the Library first, then open Worldbuilding.", 5);
                else openWorldbuilding(world, book);
            }
            else if (t.milestone >= 5 && t.milestone <= 7) {
                Id book = ws_ == Workspace::Writing && lib_->book(writingBook_) ? writingBook_
                        : ws_ == Workspace::Typesetting ? ts_.book : ws_ == Workspace::Preview ? pv_.book : ws_ == Workspace::Publishing ? pb_.book
                        : focus_.kind == Focus::Book ? focus_.id : view_ == View::Book ? viewId_ : !ts_.book.empty() ? ts_.book : writingBook_;
                if (book.empty() || !lib_->book(book))
                    flash(std::wstring(L"Choose a book in the Library first, then open ") +
                              (t.milestone == 5 ? L"Typesetting." : t.milestone == 6 ? L"Preview." : L"Publishing."), 5);
                else if (t.milestone == 5) openTypesetting(book);
                else if (t.milestone == 6) openPreview(book);
                else openPublishing(book);
            }
            else if (t.milestone == 2) {
                if (ws_ == Workspace::Typesetting) { openWriting(ts_.book); return; }
                if (ws_ == Workspace::Preview) { openWriting(pv_.book); return; }
                if (ws_ == Workspace::Publishing) { openWriting(pb_.book); return; }
                if (ws_ == Workspace::Review) {
                    if (const ManuscriptNode* nd = lib_->node(rv_.node); nd && nd->bookId == rv_.book) { writingBook_ = rv_.book; currentNode_ = nd->id; }
                    openWriting(rv_.book);
                    return;
                }
                if (ws_ == Workspace::Planning) {
                    if (!pl_.node.empty() && lib_->node(pl_.node)) { writingBook_ = pl_.book; currentNode_ = pl_.node; }
                    openWriting(pl_.book);
                    return;
                }
                if (ws_ == Workspace::Worldbuilding) {
                    Id b = wb_.book;
                    if (b.empty()) for (const studio::Book* x : lib_->liveBooks()) if (x->worldId == wb_.world) { b = x->id; break; }
                    if (!writingBook_.empty() && lib_->book(writingBook_) &&
                        (lib_->book(writingBook_)->worldId == wb_.world && !wb_.world.empty()))
                        b = writingBook_;
                    if (!b.empty()) { openWriting(b); return; }
                }
                Id book = !writingBook_.empty() && lib_ && lib_->book(writingBook_) ? writingBook_
                        : focus_.kind == Focus::Book ? focus_.id : view_ == View::Book ? viewId_ : Id{};
                if (book.empty()) flash(L"Choose a book in the Library first, then open Writing.", 5);
                else openWriting(book);
            } else
                flash(std::wstring(t.name) + L" isn't built yet. It arrives in Milestone " + std::to_wstring(t.milestone) +
                      L".", 6);
        }, false);
        if (active) {
            gfx_.text(t.name, tr, Font::BodyStrong, pal_.ink, {Align::Center, false, false, true});
            // The underline slides between workspaces.
            const float ux = slideTo("tab#x", tr.x + 10), uw = slideTo("tab#w", tw - 20);
            gfx_.fill({ux, r.bottom() - 3, uw, 3}, pal_.ribbon);
        } else {
            const Color base = built ? pal_.inkMuted : pal_.inkFaint;
            gfx_.text(t.name, tr, Font::Body, base.mix(built ? pal_.ink : pal_.inkMuted, hoverAmount(id)), {Align::Center, false, false, true});
        }
        x += tw + 4;
    }

    // Settings sits at the right-hand end of the same row of tabs.
    {
        const std::wstring lbl = L"Settings";
        const float tw = gfx_.measureWidth(lbl, Font::BodyStrong) + 24;
        const Rect tr{r.right() - tw - 16, 6, tw, r.h - 12};
        const std::string id = "tab:settings";
        const bool active = ws_ == Workspace::Library && view_ == View::Settings;
        addHit(id, tr, [this] { openSettings(); }, false);
        if (active) {
            gfx_.text(lbl, tr, Font::BodyStrong, pal_.ink, {Align::Center, false, false, true});
            const float ux = slideTo("tab#x", tr.x + 10), uw = slideTo("tab#w", tw - 20);
            gfx_.fill({ux, r.bottom() - 3, uw, 3}, pal_.ribbon);
        } else {
            gfx_.text(lbl, tr, Font::Body, pal_.inkMuted.mix(pal_.ink, hoverAmount(id)), {Align::Center, false, false, true});
        }
    }
}

// ----------------------------------------------------------------- sidebar

void StudioApp::drawSidebar(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), pal_.rule);
    if (!lib_) return;
    gfx_.pushClip(r);
    setHitClip(r);
    float y = r.y + 14 - sideScroll_;
    const float x = r.x + 10, wdt = r.w - 20;

    auto row = [&](const std::string& id, float indent, const std::wstring& label, const std::wstring& count, bool selected,
                   std::function<void()> act, Color dot = {}, bool hasDot = false) {
        const Rect rr{x, y, wdt, 30};
        addHit(id, rr, std::move(act));
        const float hv = hoverAmount(id);
        if (selected) {
            // The highlight slides to the new row (positions measured without the scroll, so scrolling doesn't drag it).
            const float sy = slideTo("side#sel", rr.y + sideScroll_) - sideScroll_;
            const Rect hr{rr.x, sy, rr.w, rr.h};
            gfx_.fillRounded(hr, 3, pal_.surface);
            gfx_.fill({hr.x, hr.y + 7, 3, 16}, pal_.ribbon);
        } else if (hv > 0.01f) gfx_.fillRounded(rr, 3, pal_.surface.withAlpha(0.6f * hv));
        float tx = rr.x + 12 + indent;
        if (hasDot) { gfx_.circle(tx + 4, rr.y + 15, 4, dot, true); tx += 14; }
        const float cw = count.empty() ? 0 : gfx_.measureWidth(count, Font::Small) + 8;
        gfx_.text(label, {tx, rr.y, rr.right() - tx - cw - 4, rr.h}, selected ? Font::BodyStrong : Font::Body,
                  selected ? pal_.ink : pal_.inkMuted, {Align::Left, false, true, true});
        if (!count.empty())
            gfx_.text(count, {rr.right() - cw - 4, rr.y, cw, rr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
        if (focused(id)) focusRing(rr);
        y += 32;
        return rr;
    };
    auto section = [&](const std::wstring& label, std::function<void()> add, const std::string& addId) {
        y += 10;
        gfx_.text(label, {x + 12, y, wdt - 50, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        if (add) {
            const Rect plus{x + wdt - 26, y - 4, 24, 24};
            addHit(addId, plus, std::move(add));
            gfx_.text(L"+", plus, Font::Heading, pal_.inkFaint.mix(pal_.ink, hoverAmount(addId)), {Align::Center, false, false, true});
            if (focused(addId)) focusRing(plus);
        }
        y += 22;
    };

    row("side:all", 0, L"All books", std::to_wstring(lib_->liveBooks().size()), view_ == View::AllBooks,
        [this] { goTo(View::AllBooks); });

    section(L"WORLDS", [this] { newWorld(); }, "side:addworld");
    std::vector<Id> worldOrder;
    for (auto* wd : lib_->liveWorlds()) worldOrder.push_back(wd->id);
    for (auto* wd : lib_->liveWorlds()) {
        const bool collapsed = std::find(settings_.collapsedWorlds.begin(), settings_.collapsedWorlds.end(), wd->id) !=
                               settings_.collapsedWorlds.end();
        size_t count = lib_->standaloneBooksIn(wd->id).size();
        for (auto* s : lib_->seriesIn(wd->id)) count += lib_->booksInSeries(s->id).size();
        const Color accent = wd->accent.empty() ? pal_.ribbon : hexColor(wd->accent, pal_.ribbon);
        const std::string id = "side:world:" + wd->id;
        const Rect rr = row(id, 14, w(wd->name), std::to_wstring(count), view_ == View::World && viewId_ == wd->id,
                            [this, i = wd->id] { goTo(View::World, i); }, accent, true);
        // Disclosure triangle for its series.
        const Rect tri{rr.x + 2, rr.y + 7, 14, 16};
        const std::string tid = "side:toggle:" + wd->id;
        addHit(tid, tri, [this, i = wd->id] {
            auto& c = settings_.collapsedWorlds;
            auto it = std::find(c.begin(), c.end(), i);
            if (it == c.end()) c.push_back(i); else c.erase(it);
            saveSettings();
        }, false);
        if (lib_->seriesIn(wd->id).empty()) {}
        else if (collapsed) gfx_.fillTriangle(tri.x + 4, tri.y + 4, tri.x + 4, tri.y + 12, tri.x + 10, tri.y + 8, pal_.inkFaint);
        else gfx_.fillTriangle(tri.x + 3, tri.y + 5, tri.x + 11, tri.y + 5, tri.x + 7, tri.y + 11, pal_.inkFaint);
        addDrop({Drop::ToWorld, rr, wd->id, {}, false, false, {}});
        addDrop({Drop::ReorderWorlds, rr, wd->id, {}, false, false, worldOrder});
        if (collapsed) continue;
        for (auto* s : lib_->seriesIn(wd->id)) {
            const Rect sr = row("side:series:" + s->id, 34, w(s->name), std::to_wstring(lib_->booksInSeries(s->id).size()),
                                view_ == View::Series && viewId_ == s->id, [this, i = s->id] { goTo(View::Series, i); });
            addDrop({Drop::ToSeries, sr, s->id, {}, false, false, {}});
        }
    }
    if (lib_->liveWorlds().empty())
        y += gfx_.text(L"A world groups series, books and their shared lore. Press + to make one.", {x + 12, y, wdt - 20, 60},
                       Font::Small, pal_.inkFaint) + 8;

    section(L"OUTSIDE ANY WORLD", nullptr, "");
    size_t indie = lib_->standaloneBooksIn({}).size();
    for (auto* s : lib_->seriesIn({})) indie += lib_->booksInSeries(s->id).size();
    const Rect ir = row("side:independent", 0, L"Independent books", std::to_wstring(indie), view_ == View::Independent,
                        [this] { goTo(View::Independent); });
    addDrop({Drop::ToIndependent, ir, {}, {}, false, false, {}});
    for (auto* s : lib_->seriesIn({})) {
        const Rect sr = row("side:series:" + s->id, 20, w(s->name), std::to_wstring(lib_->booksInSeries(s->id).size()),
                            view_ == View::Series && viewId_ == s->id, [this, i = s->id] { goTo(View::Series, i); });
        addDrop({Drop::ToSeries, sr, s->id, {}, false, false, {}});
    }

    y += 10;
    row("side:artwork", 0, L"Artwork", std::to_wstring(lib_->liveArtworks().size()), view_ == View::Artwork, [this] { goTo(View::Artwork); });
    const auto tc = lib_->trashCounts();
    int inTrash = tc.worlds + tc.series + tc.books;
    for (auto& a : lib_->artworks()) if (a.deleted) ++inTrash;
    row("side:trash", 0, L"Trash", inTrash ? std::to_wstring(inTrash) : L"", view_ == View::Trash, [this] { goTo(View::Trash); });

    sideContent_ = y + sideScroll_ - r.y + 16;
    clearHitClip();
    gfx_.popClip();
}

// -------------------------------------------------------------- status bar

void StudioApp::drawStatusBar(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.x, r.y + 0.5f, r.right(), r.y + 0.5f, pal_.rule);
    std::wstring left = lib_ ? L"Library: " + w(lib_->folder()) : L"No library open";
    if (lib_ && ws_ == Workspace::Writing) {
        // While writing: today's words, and the goal if there is one.
        const std::string today = Library::localDay(now());
        const auto days = lib_->wordsWritten({}, today);
        const std::int64_t n = days.count(today) ? days.at(today) : 0;
        const std::int64_t goal = dailyGoal();
        left = L"Today: " + std::to_wstring(n) + (goal ? L" of " + std::to_wstring(goal) : std::wstring()) + L" words";
        if (goal && n >= goal) left += L"  ·  goal reached";
    }
    gfx_.text(left, {r.x + 14, r.y, r.w * 0.55f, r.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
    // Right: messages, otherwise whether everything is saved. Typing shows Saving..., then Saved.
    const bool flashing = !flashText().empty();
    const float right = r.right() - 14;
    if (lib_) {
        const bool saving = dirty_ || editorDirty_;
        const std::wstring st = saveFailed_ ? L"Not saved" : saving ? L"Saving..." : L"Saved";
        const float sw = gfx_.measureWidth(L"Saving...", Font::Small) + 18;
        const Color dot = saveFailed_ ? hexColor("#B4442C") : saving ? hexColor("#C9922E") : pal_.inkFaint;
        gfx_.circle(right - sw + 4, r.y + r.h / 2, 3, dot.withAlpha(0.4f + 0.6f * anim("save#dot", saving ? 1.0f : 0.0f, 10.0f)), true);
        gfx_.text(st, {right - sw + 12, r.y, sw - 12, r.h}, Font::Small, saveFailed_ ? hexColor("#B4442C") : pal_.inkMuted,
                  {Align::Left, false, false, true});
        if (flashing)
            gfx_.text(flashText(), {r.x + r.w * 0.4f, r.y, r.w * 0.6f - sw - 30, r.h}, Font::Small, flashColor(pal_.ink),
                      {Align::Right, false, true, true});
    }
}

// ----------------------------------------------------------- shared bits

Color StudioApp::accentOfWorld(const Id& worldId) const {
    const World* x = worldId.empty() ? nullptr : lib_->world(worldId);
    return x && !x->accent.empty() ? hexColor(x->accent, hexColor("#2D4A6E")) : hexColor("#2D4A6E");
}

Color StudioApp::accentFor(const Book& b) const { return accentOfWorld(b.worldId); }

std::wstring StudioApp::seriesLine(const Book& b) const {
    std::wstring out;
    for (auto* s : lib_->seriesOfBook(b.id)) {
        const Membership* m = lib_->membership(s->id, b.id);
        if (!out.empty()) out += L"; ";
        out += w(s->name);
        if (m && !m->number.empty()) out += L" " + w(m->number);
    }
    return out;
}

float StudioApp::pageHeader(const Rect& r, const std::wstring& kicker, const std::wstring& title, const std::wstring& sub) {
    float y = r.y;
    if (!kicker.empty()) {
        gfx_.text(kicker, {r.x, y, r.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false, true});
        y += 20;
    }
    y += gfx_.text(title, {r.x, y, r.w, 80}, Font::SerifTitle, pal_.ink) + 4;
    if (!sub.empty()) y += gfx_.text(sub, {r.x, y, std::min(r.w, 700.0f), 200}, Font::Body, pal_.inkMuted) + 4;
    return y - r.y + 16;
}

bool StudioApp::smallButton(const std::string& id, float x, float y, const std::wstring& label, std::function<void()> act, float* outW) {
    const float bw = gfx_.measureWidth(label, Font::Small) + 22;
    if (outW) *outW = bw;
    return button(id, {x, y, bw, 28}, label, std::move(act));
}

// Search box, sort, filter and view toggle above a collection.
float StudioApp::toolbar(const Rect& r, bool withSearch) {
    float x = r.x;
    if (withSearch) {
        const Rect sr{x, r.y, std::min(300.0f, r.w * 0.4f), 30};
        gfx_.fillRounded(sr, 3, pal_.surface);
        gfx_.strokeRounded(sr, 3, GetFocus() == fields_[FSearch] ? pal_.ribbon : pal_.rule);
        placeField(FSearch, sr);
        x = sr.right() + 12;
    }
    static const wchar_t* sortNames[] = {L"Title", L"Series order", L"Author", L"Status", L"Recently changed"};
    float bw = 0;
    smallButton("tb:sort", x, r.y + 1, std::wstring(L"Sort: ") + sortNames[settings_.sort], [this] { popupSortMenu(0, 0); }, &bw);
    x += bw + 8;
    std::wstring filt = L"Show: everything";
    if (statusFilter_ >= 0) filt = L"Show: " + w(label((BookStatus)statusFilter_));
    if (!tagFilter_.empty()) filt = L"Show: tag “" + w(tagFilter_) + L"”";
    smallButton("tb:filter", x, r.y + 1, filt, [this] { popupFilterMenu(0, 0); }, &bw);
    x += bw + 8;
    segmented("tb:view", {std::max(x + 4, r.right() - 180), r.y + 1, 180, 28}, {L"Bookshelf", L"List"}, settings_.gallery ? 0 : 1, [this](int i) {
        settings_.gallery = i == 0;
        saveSettings();
    });
    return 46;
}

std::vector<const Book*> StudioApp::filteredBooks(std::vector<const Book*> in) const {
    auto lower = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    const std::string q = lower(search_);
    std::vector<const Book*> out;
    for (auto* b : in) {
        if (statusFilter_ >= 0 && (int)b->status != statusFilter_) continue;
        if (!tagFilter_.empty() && std::find(b->tags.begin(), b->tags.end(), tagFilter_) == b->tags.end()) continue;
        if (!q.empty()) {
            std::string hay = lower(b->title + " " + b->subtitle + " " + b->author + " " + b->penName + " " + b->genre);
            for (auto& t : b->tags) hay += " " + lower(t);
            if (hay.find(q) == std::string::npos) continue;
        }
        out.push_back(b);
    }
    return out;
}

// ------------------------------------------------------------ book covers

void StudioApp::drawCover(const Rect& r, const Book& b) {
    if (!b.coverBlob.empty() && gfx_.drawImage(w(lib_->blobPath(b.coverBlob)), r, true)) {
        gfx_.stroke(r, pal_.rule);
        return;
    }
    // No cover yet: a plain cloth binding in the world's colour, title set in serif.
    const Color cloth = accentFor(b);
    gfx_.fill(r, cloth);
    gfx_.fill({r.x, r.y, 6, r.h}, cloth.mix(hexColor("#000000"), 0.25f));  // spine shadow
    const Color ink = hexColor("#F4F1EA");
    if (r.w < 60) return;  // thumbnails: the cloth alone reads better than squeezed type
    const float m = std::max(10.0f, r.w * 0.1f);
    gfx_.line(r.x + m, r.y + m, r.right() - m, r.y + m, ink.withAlpha(0.5f));
    gfx_.line(r.x + m, r.bottom() - m, r.right() - m, r.bottom() - m, ink.withAlpha(0.5f));
    const Rect t{r.x + m + 2, r.y + m + 10, r.w - 2 * m - 4, r.h * 0.55f};
    gfx_.text(w(b.title), t, r.w > 150 ? Font::SerifTitle : Font::Serif, ink, {Align::Center, true, true, false});
    const std::string by = !b.penName.empty() ? b.penName : b.author;
    if (!by.empty())
        gfx_.text(w(by), {r.x + m, r.bottom() - m - 26, r.w - 2 * m, 20}, Font::SerifSmall, ink.withAlpha(0.85f),
                  {Align::Center, false, true});
}

void StudioApp::bookCard(const Rect& r, const Book& b, const Id& seriesId, const std::string& hitId) {
    const bool sel = focus_.kind == Focus::Book && focus_.id == b.id;
    addHit(hitId, r, [this, id = b.id, seriesId] { setFocus({Focus::Book, id, seriesId}); });
    // Covers rise a little on hover, with a soft shadow.
    const float lift = hoverAmount(hitId);
    const Rect cover{r.x, r.y - 3 * lift, r.w, r.w * 1.5f};
    if (drag_.active && drag_.id == b.id) gfx_.fill(cover, pal_.sunken);
    else { softShadow(cover, 2, 0.4f + lift); drawCover(cover, b); }
    if (sel) gfx_.stroke(cover.inset(-3), pal_.ribbon, 2);
    float y = cover.bottom() + 8;
    if (!seriesId.empty())
        if (const Membership* m = lib_->membership(seriesId, b.id)) {
            const std::wstring k = (m->number.empty() ? std::wstring(L"") : L"Book " + w(m->number)) +
                                   (m->kind != Instalment::Novel ? (m->number.empty() ? L"" : L" · ") + w(label(m->kind)) : L"");
            if (!k.empty()) { gfx_.text(k, {r.x, y, r.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false, true}); y += 17; }
        }
    gfx_.text(w(b.title), {r.x, y, r.w, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
    gfx_.text(w(label(b.status)), {r.x, y + 20, r.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
    if (focused(hitId)) focusRing(cover);
}

void StudioApp::bookRow(const Rect& r, const Book& b, const Id& seriesId, const std::string& hitId, bool header) {
    const float c1 = r.w * 0.36f, c2 = r.w * 0.24f, c3 = r.w * 0.14f, c4 = r.w * 0.12f;
    if (header) {
        float x = r.x + 44;
        for (auto [t, wd] : std::initializer_list<std::pair<const wchar_t*, float>>{{L"TITLE", c1 - 44}, {L"SERIES", c2}, {L"STATUS", c3}, {L"GENRE", c4}, {L"CHANGED", 200}}) {
            gfx_.text(t, {x, r.y, wd, r.h}, Font::Label, pal_.inkFaint, {Align::Left, false, true, true});
            x += wd;
        }
        return;
    }
    const bool sel = focus_.kind == Focus::Book && focus_.id == b.id;
    addHit(hitId, r, [this, id = b.id, seriesId] { setFocus({Focus::Book, id, seriesId}); });
    if (sel) gfx_.fill(r, pal_.surface);
    else if (float hv = hoverAmount(hitId); hv > 0.01f) gfx_.fill(r, pal_.surface.withAlpha(0.5f * hv));
    if (sel) gfx_.fill({r.x, r.y + 6, 3, r.h - 12}, pal_.ribbon);
    drawCover({r.x + 10, r.y + 5, 26, 38}, b);
    float x = r.x + 44;
    const std::wstring sub = !b.subtitle.empty() ? w(b.subtitle) : !b.penName.empty() ? w(b.penName) : w(b.author);
    gfx_.text(w(b.title), {x, r.y + 6, c1 - 50, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
    gfx_.text(sub, {x, r.y + 25, c1 - 50, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
    x += c1 - 44;
    gfx_.text(seriesLine(b), {x, r.y, c2 - 10, r.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true}); x += c2;
    gfx_.text(w(label(b.status)), {x, r.y, c3 - 10, r.h}, Font::Small, pal_.ink, {Align::Left, false, true, true}); x += c3;
    gfx_.text(w(b.genre), {x, r.y, c4 - 10, r.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true}); x += c4;
    gfx_.text(cjui::formatRelative((cj::UtcSeconds)b.modified, cjui::nowUtc()), {x, r.y, r.right() - x - 8, r.h}, Font::Small,
              pal_.inkFaint, {Align::Left, false, true, true});
    gfx_.line(r.x, r.bottom() - 0.5f, r.right(), r.bottom() - 0.5f, pal_.rule);
    if (focused(hitId)) focusRing(r);
}

// Books as a shelf or a list. Reorderable lists register drop positions.
float StudioApp::bookCollection(const Rect& r, const std::vector<const Book*>& books, const std::string& listKey,
                                const Id& seriesId, bool chronological, bool reorderable) {
    std::vector<Id> ids;
    for (auto* b : books) ids.push_back(b->id);
    const Drop::Kind kind = seriesId.empty() ? Drop::ReorderStandalone : Drop::ReorderSeriesBooks;
    const Id owner = seriesId.empty() ? (books.empty() ? Id{} : books[0]->worldId) : seriesId;
    if (books.empty()) {
        const Rect empty{r.x, r.y, r.w, 64};
        gfx_.strokeRounded(empty, 3, pal_.rule);
        gfx_.text(seriesId.empty() ? L"No books here yet." : L"No books in this series yet. Drag a book here, or use New book.",
                  empty.inset(16, 0), Font::Small, pal_.inkFaint, {Align::Left, false, true, true});
        if (reorderable) addDrop({kind, empty, {}, seriesId, false, chronological, ids});
        return empty.h;
    }
    const std::string suffix = seriesId.empty() ? "" : "|" + seriesId;
    if (settings_.gallery) {
        const float cw = 132, gap = 26;
        const int perRow = std::max(1, (int)((r.w + gap) / (cw + gap)));
        const float rowH = cw * 1.5f + 62;
        for (size_t i = 0; i < books.size(); ++i) {
            const Rect cr{r.x + (i % perRow) * (cw + gap), r.y + (i / perRow) * (rowH + 18), cw, rowH};
            bookCard(cr, *books[i], seriesId, "book:" + books[i]->id + suffix + "@" + listKey);
            if (reorderable) addDrop({kind, {cr.x - gap / 2, cr.y, cw + gap, rowH}, books[i]->id, owner, true, chronological, ids});
        }
        const size_t rows = (books.size() + perRow - 1) / perRow;
        return rows * (rowH + 18);
    }
    float y = r.y;
    bookRow({r.x, y, r.w, 24}, *books[0], seriesId, "", true);
    y += 26;
    for (auto* b : books) {
        const Rect rr{r.x, y, r.w, 48};
        bookRow(rr, *b, seriesId, "book:" + b->id + suffix + "@" + listKey, false);
        if (reorderable) addDrop({kind, rr, b->id, owner, false, chronological, ids});
        y += 48;
    }
    return y - r.y;
}

// ----------------------------------------------------------------- pages

float StudioApp::pageEmpty(const Rect& r) {
    float y = r.y + pageHeader(r, L"", L"No library is open", libraryNote_.empty() ? L"" : libraryNote_);
    button("empty:open", {r.x, y, 220, 34}, L"Open or create a library...", [this] { chooseLibraryFolder(); }, true);
    return y + 40 - r.y;
}

float StudioApp::pageAllBooks(const Rect& r) {
    float y = r.y;
    const auto all = lib_->liveBooks();
    y += pageHeader(r, L"AUTHOR LIBRARY", L"All books",
                    all.empty() ? L"Your library is empty. Start with a world for a fictional universe, or a book on its own."
                                : plural(all.size(), L"book", L"books") + L" across " +
                                      plural(lib_->liveWorlds().size(), L"world", L"worlds") + L".");
    if (!libraryNote_.empty()) {
        const float h = gfx_.measureHeight(libraryNote_, std::min(r.w, 700.0f) - 28, Font::Small);
        gfx_.fillRounded({r.x, y, std::min(r.w, 700.0f), h + 20}, 3, pal_.sunken);
        gfx_.text(libraryNote_, {r.x + 14, y + 10, std::min(r.w, 700.0f) - 28, h + 4}, Font::Small, pal_.ink);
        y += h + 36;
    }
    if (all.empty()) {
        // First run: the two ways in.
        const std::wstring create = L"Create a book", import = L"Import a book...";
        const float cw = gfx_.measureWidth(create, Font::BodyStrong) + 40, iw = gfx_.measureWidth(import, Font::BodyStrong) + 36;
        button("all:newbook", {r.x, y, cw, 34}, create, [this] { newBook({}, {}); }, true);
        button("all:import", {r.x + cw + 10, y, iw, 34}, import, [this] { startImport({}); });
        y += 46;
        y += gfx_.text(L"Import reads EPUB, Word (.docx), OpenDocument (.odt), web pages, Markdown and plain text. PDF importing is experimental.",
                       {r.x, y, std::min(r.w, 640.0f), 60}, Font::Small, pal_.inkMuted) + 6;
        float lw = 0;
        linkText("all:newworld", r.x, y, L"Or start with a world, for a series set in one universe", [this] { newWorld(); });
        (void)lw;
        y += 44;
        gfx_.text(L"HOW THE LIBRARY WORKS", {r.x, y, r.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 26;
        static const wchar_t* tips[] = {
            L"A world (say, Alvora) holds its series, standalone books and, later, its characters, places and timeline.",
            L"A series keeps a reading order and a separate chronological order.",
            L"Books can also sit outside any world: nonfiction, collections, anything else.",
            L"Nothing is ever copied: moving a book between series or worlds just changes where it's listed."};
        for (auto* t : tips) {
            gfx_.text(L"–", {r.x, y, 14, 20}, Font::Body, pal_.inkFaint);
            y += gfx_.text(t, {r.x + 18, y, std::min(r.w, 640.0f) - 18, 60}, Font::Body, pal_.inkMuted) + 8;
        }
        return y - r.y;
    }
    {
        float bx = r.x, bw = 0;
        smallButton("all:new", bx, y, L"New book", [this] { newBook({}, {}); }, &bw);
        bx += bw + 8;
        smallButton("all:import2", bx, y, L"Import a book...", [this] { startImport({}); }, &bw);
        bx += bw + 8;
        smallButton("all:world", bx, y, L"New world", [this] { newWorld(); }, &bw);
        y += 44;
    }
    y += toolbar({r.x, y, r.w, 32}, true);
    auto books = filteredBooks(all);
    auto key = [&](const Book* b) -> std::string {
        switch (settings_.sort) {
            case 1: { std::string s = narrow(seriesLine(*b)); return s.empty() ? "~" + b->title : s; }
            case 2: return (b->penName.empty() ? b->author : b->penName) + "\x01" + b->title;
            case 3: return std::to_string((int)b->status) + b->title;
            default: return b->title;
        }
    };
    if (settings_.sort == 4)
        std::stable_sort(books.begin(), books.end(), [](auto* a, auto* b) { return a->modified > b->modified; });
    else
        std::stable_sort(books.begin(), books.end(), [&](auto* a, auto* b) {
            std::string ka = key(a), kb = key(b);
            for (auto& c : ka) c = (char)std::tolower((unsigned char)c);
            for (auto& c : kb) c = (char)std::tolower((unsigned char)c);
            return ka < kb;
        });
    if (books.empty()) {
        y += gfx_.text(L"Nothing matches. Clear the search or choose Show: everything.", {r.x, y, r.w, 40}, Font::Body, pal_.inkMuted);
        return y - r.y;
    }
    y += bookCollection({r.x, y, r.w, 0}, books, "all", {}, false, false);
    return y - r.y;
}

float StudioApp::pageWorld(const Rect& r) {
    const World& wd = *lib_->world(viewId_);
    float y = r.y;
    // Banner: the world's artwork, or its colour.
    const Color accent = wd.accent.empty() ? hexColor("#2D4A6E") : hexColor(wd.accent, hexColor("#2D4A6E"));
    const Rect banner{r.x, y, r.w, 150};
    if (wd.bannerBlob.empty() || !gfx_.drawImage(w(lib_->blobPath(wd.bannerBlob)), banner, true)) {
        gfx_.fill(banner, accent);
        for (int i = 0; i < 6; ++i)
            gfx_.line(banner.x, banner.y + 30 + i * 22, banner.right(), banner.y + 10 + i * 22,
                      hexColor("#FFFFFF").withAlpha(0.05f), 14);
    }
    gfx_.fill({banner.x, banner.bottom() - 54, banner.w, 54}, hexColor("#000000").withAlpha(0.35f));
    float tx = banner.x + 22;
    if (!wd.logoBlob.empty() && gfx_.drawImage(w(lib_->blobPath(wd.logoBlob)), {tx, banner.bottom() - 48, 42, 42}, false)) tx += 54;
    gfx_.text(L"WORLD", {tx, banner.bottom() - 50, 200, 16}, Font::Label, hexColor("#F4F1EA").withAlpha(0.75f), {Align::Left, false});
    gfx_.text(w(wd.name), {tx, banner.bottom() - 36, banner.w - 60, 32}, Font::SerifTitle, hexColor("#F4F1EA"), {Align::Left, false, true});
    y = banner.bottom() + 16;
    if (!wd.description.empty())
        y += gfx_.text(w(wd.description), {r.x, y, std::min(r.w, 720.0f), 400}, Font::Body, pal_.ink) + 14;

    float bx = r.x, bw = 0;
    smallButton("world:newseries", bx, y, L"New series", [this] { newSeries(viewId_); }, &bw); bx += bw + 8;
    smallButton("world:newbook", bx, y, L"New standalone book", [this] { newBook(viewId_, {}); }, &bw); bx += bw + 8;
    segmented("world:view", {r.right() - 180, y, 180, 28}, {L"Bookshelf", L"List"}, settings_.gallery ? 0 : 1, [this](int i) {
        settings_.gallery = i == 0;
        saveSettings();
    });
    y += 48;

    const auto series = lib_->seriesIn(wd.id);
    std::vector<Id> seriesIds;
    for (auto* s : series) seriesIds.push_back(s->id);
    for (auto* s : series) {
        const auto books = lib_->booksInSeries(s->id);
        const std::string hid = "series:" + s->id;
        const Rect head{r.x, y, r.w, 34};
        addHit(hid, head, [this, i = s->id] { goTo(View::Series, i); });
        addDrop({Drop::ReorderSeriesList, head, s->id, wd.id, false, false, seriesIds});
        addDrop({Drop::ToSeries, head, s->id, {}, false, false, {}});
        if (float hv = hoverAmount(hid); hv > 0.01f) gfx_.fill(head, pal_.surface.withAlpha(0.6f * hv));
        gfx_.text(w(s->name), {head.x, head.y, head.w * 0.6f, head.h}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
        const std::wstring meta = plural(books.size(), L"book", L"books") + L"  ·  " + w(label(s->status)) + L"  ›";
        gfx_.text(meta, {head.x + head.w * 0.6f, head.y, head.w * 0.4f, head.h}, Font::Small, pal_.inkMuted,
                  {Align::Right, false, true, true});
        if (focused(hid)) focusRing(head);
        y += 38;
        gfx_.line(r.x, y - 2.5f, r.right(), y - 2.5f, pal_.rule);
        y += bookCollection({r.x, y + 8, r.w, 0}, books, "w" + s->id, s->id, false, true) + 30;
    }
    gfx_.text(L"STANDALONE BOOKS", {r.x, y, r.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    y += bookCollection({r.x, y, r.w, 0}, filteredBooks(lib_->standaloneBooksIn(wd.id)), "ws", {}, false, true) + 30;

    // The world's bible: a summary, and the way in.
    gfx_.text(L"WORLDBUILDING", {r.x, y, r.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    {
        const auto entries = lib_->entriesOf(wd.id);
        std::wstring line;
        if (entries.empty())
            line = L"Characters, places, groups, things, lore and events, shared by every book in " + w(wd.name) + L".";
        else {
            for (auto& k : entryKinds()) {
                int n = 0;
                for (auto* e : entries) if (e->kind == k) ++n;
                if (!n) continue;
                std::wstring kl = w(n == 1 ? entryKindLabel(k) : entryKindPlural(k));
                for (auto& ch : kl) ch = (wchar_t)towlower(ch);
                line += (line.empty() ? L"" : L", ") + std::to_wstring(n) + L" " + kl;
            }
            line += L".";
        }
        y += gfx_.text(line, {r.x, y, std::min(r.w, 700.0f), 60}, Font::Small, pal_.inkMuted) + 10;
        smallButton("world:wb", r.x, y, entries.empty() ? L"Start the world's bible" : L"Open Worldbuilding",
                    [this, id = wd.id] { openWorldbuilding(id, {}); });
        y += 40;
    }
    return y - r.y;
}

float StudioApp::pageSeries(const Rect& r) {
    const Series& s = *lib_->seriesById(viewId_);
    const World* wd = s.worldId.empty() ? nullptr : lib_->world(s.worldId);
    float y = r.y;
    const std::wstring kicker = wd ? L"SERIES IN " + w(wd->name) : std::wstring(L"SERIES");
    if (wd) {
        const float kw = gfx_.measureWidth(kicker, Font::Label);
        addHit("series:up", {r.x, y, kw, 18}, [this, i = wd->id] { goTo(View::World, i); });
    }
    const float coverW = s.coverBlob.empty() ? 0 : 110;
    if (coverW) gfx_.drawImage(w(lib_->blobPath(s.coverBlob)), {r.right() - coverW, y, coverW, coverW * 1.5f}, true);
    y += pageHeader({r.x, y, r.w - coverW - 20, 0}, kicker, w(s.name),
                    s.description.empty() ? w(label(s.status)) : w(s.description));
    const auto books = lib_->booksInSeries(s.id, chrono_);
    float bx = r.x, bw = 0;
    smallButton("series:newbook", bx, y, L"New book in series", [this] { newBook({}, viewId_); }, &bw); bx += bw + 12;
    segmented("series:order", {bx, y, 260, 28}, {L"Reading order", L"Chronological"}, chrono_ ? 1 : 0, [this](int i) { chrono_ = i == 1; });
    segmented("series:view", {r.right() - 180, y, 180, 28}, {L"Bookshelf", L"List"}, settings_.gallery ? 0 : 1, [this](int i) {
        settings_.gallery = i == 0;
        saveSettings();
    });
    y += 40;
    y += gfx_.text(chrono_ ? L"Chronological order follows the story's own timeline. Drag books to rearrange it; reading order isn't affected."
                           : L"Reading order is the order readers should take. Drag books to rearrange, or select one and press Alt+Up or Alt+Down.",
                   {r.x, y, std::min(r.w, 700.0f), 60}, Font::Small, pal_.inkMuted) + 16;
    y += bookCollection({r.x, y, r.w, 0}, books, chrono_ ? "sc" : "sr", s.id, chrono_, true) + 24;
    if (!s.notes.empty()) {
        gfx_.text(L"SERIES NOTES", {r.x, y, r.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 24;
        y += gfx_.text(w(s.notes), {r.x, y, std::min(r.w, 700.0f), 1000}, Font::Body, pal_.ink);
    }
    return y - r.y;
}

float StudioApp::pageIndependent(const Rect& r) {
    float y = r.y + pageHeader(r, L"OUTSIDE ANY WORLD", L"Independent books",
                               L"Books and series that don't belong to a fictional world: nonfiction, collections, one-offs.");
    float bw = 0;
    smallButton("ind:newbook", r.x, y, L"New independent book", [this] { newBook({}, {}); }, &bw);
    smallButton("ind:newseries", r.x + bw + 8, y, L"New series", [this] { newSeries({}); });
    y += 48;
    for (auto* s : lib_->seriesIn({})) {
        const std::string hid = "series:" + s->id;
        addHit(hid, {r.x, y, r.w, 30}, [this, i = s->id] { goTo(View::Series, i); });
        gfx_.text(w(s->name) + L"  ›", {r.x, y, r.w, 30}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
        y += 38;
        y += bookCollection({r.x, y, r.w, 0}, lib_->booksInSeries(s->id), "i" + s->id, s->id, false, true) + 28;
    }
    gfx_.text(L"STANDALONE", {r.x, y, r.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 24;
    y += bookCollection({r.x, y, r.w, 0}, filteredBooks(lib_->standaloneBooksIn({})), "is", {}, false, true);
    return y - r.y;
}

float StudioApp::pageTrash(const Rect& r) {
    float y = r.y + pageHeader(r, L"", L"Trash",
                               L"Nothing here is lost until you empty the Trash. A world in the Trash keeps its series and books with it.");
    const auto c = lib_->trashCounts();
    int pictures = 0;
    for (auto& a : lib_->artworks()) if (a.deleted) ++pictures;
    if (!c.worlds && !c.series && !c.books && !pictures && !c.entries) {
        y += gfx_.text(L"The Trash is empty.", {r.x, y, r.w, 24}, Font::Body, pal_.inkMuted);
        return y - r.y;
    }
    button("trash:empty", {r.x, y, 150, 34}, L"Empty Trash...", [this] { emptyTrash(); });
    y += 52;
    auto item = [&](const std::string& id, const std::wstring& kind, const std::wstring& name, Time when) {
        const Rect rr{r.x, y, r.w, 46};
        gfx_.text(kind, {rr.x, rr.y + 4, 90, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        gfx_.text(name, {rr.x + 90, rr.y, rr.w - 240, 24}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        gfx_.text(L"Moved to the Trash " + cjui::formatRelative((cj::UtcSeconds)when, cjui::nowUtc()), {rr.x + 90, rr.y + 22, 300, 18},
                  Font::Small, pal_.inkMuted, {Align::Left, false});
        button("trash:restore:" + id, {rr.right() - 110, rr.y + 6, 110, 30}, L"Restore", [this, id, name] {
            if (lib_->restore(id)) flash(L"Restored “" + name + L"”.", 4);
        });
        gfx_.line(rr.x, rr.bottom() - 0.5f, rr.right(), rr.bottom() - 0.5f, pal_.rule);
        y += 50;
    };
    for (auto& x : lib_->worlds()) if (x.deleted) item(x.id, L"WORLD", w(x.name), x.deleted);
    for (auto& x : lib_->series()) if (x.deleted) item(x.id, L"SERIES", w(x.name), x.deleted);
    for (auto& x : lib_->books()) if (x.deleted) item(x.id, L"BOOK", w(x.title), x.deleted);
    for (auto& x : lib_->artworks()) if (x.deleted) item(x.id, L"PICTURE", w(x.name), x.deleted);
    for (const Entry* x : lib_->trashedEntries()) {
        std::wstring k = w(entryKindLabel(x->kind));
        for (auto& ch : k) ch = (wchar_t)towupper(ch);
        item(x->id, k, w(x->name), x->deleted);
    }
    return y - r.y;
}

float StudioApp::pageBook(const Rect& r) {
    const Book& b = *lib_->book(viewId_);
    float y = r.y;
    const World* wd = b.worldId.empty() ? nullptr : lib_->world(b.worldId);
    const std::wstring crumb = (wd ? w(wd->name) : std::wstring(L"Independent books")) + L"  ›  " + w(b.title);
    addHit("book:crumb", {r.x, y, gfx_.measureWidth(crumb, Font::Small), 18}, [this, wd] {
        if (wd) goTo(View::World, wd->id); else goTo(View::Independent);
    });
    gfx_.text(crumb, {r.x, y, r.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
    y += 30;
    const float cw = std::min(220.0f, r.w * 0.3f);
    drawCover({r.x, y, cw, cw * 1.5f}, b);
    const float x = r.x + cw + 32, tw = r.w - cw - 32;
    float ty = y;
    ty += gfx_.text(w(b.title), {x, ty, tw, 90}, Font::SerifTitle, pal_.ink) + 2;
    if (!b.subtitle.empty()) ty += gfx_.text(w(b.subtitle), {x, ty, tw, 60}, Font::Heading, pal_.inkMuted) + 4;
    const std::string by = !b.penName.empty() ? b.penName + (b.author.empty() || b.author == b.penName ? "" : " (" + b.author + ")") : b.author;
    if (!by.empty()) ty += gfx_.text(L"by " + w(by), {x, ty, tw, 24}, Font::Body, pal_.ink) + 10;
    float tagW = 0;
    tag(x, ty, w(label(b.status)), pal_.ribbon, &tagW);
    ty += 34;
    auto kv = [&](const std::wstring& k, const std::wstring& v) {
        if (v.empty()) return;
        gfx_.text(k, {x, ty, 120, 20}, Font::Small, pal_.inkMuted, {Align::Left, false});
        ty += std::max(20.0f, gfx_.text(v, {x + 120, ty, tw - 120, 60}, Font::Body, pal_.ink)) + 6;
    };
    kv(L"Series", seriesLine(b));
    kv(L"Genre", w(b.genre));
    kv(L"Language", w(b.language));
    kv(L"ISBN", w(b.isbn));
    kv(L"Edition", w(b.edition));
    if (!b.tags.empty()) {
        std::wstring t;
        for (auto& x2 : b.tags) t += (t.empty() ? L"" : L", ") + w(x2);
        kv(L"Tags", t);
    }
    kv(L"Words", b.wordCount ? std::to_wstring(b.wordCount) : L"");
    kv(L"Last changed", cjui::formatRelative((cj::UtcSeconds)b.modified, cjui::nowUtc()));
    y = std::max(y + cw * 1.5f, ty) + 28;
    if (!b.synopsis.empty()) {
        gfx_.text(L"SYNOPSIS", {r.x, y, r.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 24;
        y += gfx_.text(w(b.synopsis), {r.x, y, std::min(r.w, 720.0f), 2000}, Font::Body, pal_.ink) + 24;
    }
    gfx_.text(L"MANUSCRIPT", {r.x, y, r.w, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 26;
    {
        float bx = r.x, bw = 0;
        const auto top = lib_->childrenOf(b.id, {});
        const std::wstring write = top.empty() ? L"Start writing" : L"Open in Writing";
        bw = gfx_.measureWidth(write, Font::BodyStrong) + 36;
        button("book:write", {bx, y, bw, 34}, write, [this, id = b.id] { openWriting(id); }, true);
        bx += bw + 10;
        float iw = 0;
        smallButton("book:import", bx, y + 3, L"Import into this book...", [this, id = b.id] { startImport(id); }, &iw);
        if (!top.empty()) {
            float bx2 = bx + iw + 8, bw2 = 0;
            smallButton("book:plan", bx2, y + 3, L"Plan", [this, id = b.id] { openPlanning(id); }, &bw2);
            bx2 += bw2 + 8;
            smallButton("book:typeset", bx2, y + 3, L"Typeset", [this, id = b.id] { openTypesetting(id); }, &bw2);
            bx2 += bw2 + 8;
            smallButton("book:publish", bx2, y + 3, L"Publish", [this, id = b.id] { openPublishing(id); });
        }
        y += 50;
        if (top.empty()) {
            y += gfx_.text(L"No chapters yet. Start writing, or import an EPUB, Word, OpenDocument, PDF, web page, Markdown or text file.",
                           {r.x, y, std::min(r.w, 720.0f), 40}, Font::Small, pal_.inkMuted) + 12;
        }
        int shown = 0;
        for (auto* n : lib_->manuscriptOf(b.id)) {
            if (n->kind == NodeKind::Scene) continue;
            if (++shown > 40) {
                gfx_.text(L"...", {r.x, y, 40, 20}, Font::Small, pal_.inkFaint, {Align::Left, false});
                y += 22;
                break;
            }
            const bool part = n->kind == NodeKind::Part;
            const float ix = r.x + (part || n->parentId.empty() ? 0 : 20);
            const Rect row{ix, y, std::min(r.w, 720.0f) - (ix - r.x), 28};
            const std::string id = "bookms:" + n->id;
            addHit(id, row, [this, nid = n->id, bid = b.id] { openWriting(bid); selectNode(nid); });
            if (hoverAmount(id) > 0.01f) gfx_.fillRounded(row, 3, pal_.surface.withAlpha(0.7f * hoverAmount(id)));
            std::int64_t words = n->wordCount;
            std::function<void(const Id&)> add = [&](const Id& pid) { for (auto* c : lib_->childrenOf(b.id, pid)) { words += c->wordCount; add(c->id); } };
            add(n->id);
            gfx_.text(n->title.empty() ? w(label(n->kind)) : w(n->title), {row.x + 8, row.y, row.w - 130, row.h},
                      part ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
            gfx_.text(std::to_wstring(words) + L" words", {row.right() - 120, row.y, 112, row.h}, Font::Small, pal_.inkMuted,
                      {Align::Right, false, false, true});
            y += 30;
        }
    }
    y += 20;
    return y - r.y;
}

// --------------------------------------------------------- properties panel

float StudioApp::panelField(const Rect& r, float y, const std::wstring& lbl, Field f, float h) {
    gfx_.text(lbl, {r.x, y, r.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 20;
    const Rect box{r.x, y, r.w, h};
    const bool inView = box.y >= panelRect_.y + 40 && box.bottom() <= panelRect_.bottom() - 4;
    gfx_.fillRounded(box, 3, pal_.surface);
    gfx_.strokeRounded(box, 3, GetFocus() == fields_[f] ? pal_.ribbon : pal_.rule);
    if (inView) placeField(f, box);
    return y + h + 12;
}

float StudioApp::panelImage(const Rect& r, float y, const std::wstring& lbl, const std::string& blob, std::function<void()> choose,
                            std::function<void()> clear, bool wide) {
    gfx_.text(lbl, {r.x, y, r.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 20;
    const Rect box = wide ? Rect{r.x, y, r.w, r.w * 0.38f} : Rect{r.x, y, 96, 144};
    if (blob.empty() || !gfx_.drawImage(w(lib_->blobPath(blob)), box, true)) {
        gfx_.fillRounded(box, 3, pal_.sunken);
        gfx_.text(L"None", box, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
    } else {
        gfx_.stroke(box, pal_.rule);
    }
    const float bx = wide ? r.x : box.right() + 12;
    const float by = wide ? box.bottom() + 8 : box.y;
    const std::string base = "panel:img:" + narrow(lbl);
    float bw = 0;
    smallButton(base + ":choose", bx, by, blob.empty() ? L"Choose..." : L"Replace...", choose, &bw);
    if (!blob.empty()) linkText(base + ":clear", wide ? bx + bw + 12 : bx, wide ? by + 6 : by + 38, L"Remove", clear);
    if (!blob.empty()) {
        const auto [pw, ph] = gfx_.imageSize(w(lib_->blobPath(blob)));
        if (pw) gfx_.text(std::to_wstring(pw) + L" × " + std::to_wstring(ph) + L" px", {bx, wide ? by + 34 : by + 62, 150, 18},
                          Font::Small, pal_.inkFaint, {Align::Left, false});
    }
    return (wide ? by + 58 : box.bottom() + 14);
}

// A section of the details panel: a fine rule, then a small capitals label.
void StudioApp::panelHeading(const Rect& c, float& y, const std::wstring& kicker, bool first) {
    if (!first) {
        y += 8;
        gfx_.line(c.x, y + 0.5f, c.right(), y + 0.5f, pal_.rule);
        y += 18;
    }
    gfx_.text(kicker, {c.x, y, c.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    y += 26;
}

// A labelled button that opens a menu (status, kind).
void StudioApp::panelChoice(const std::string& id, const Rect& c, float& y, const std::wstring& lbl, const std::wstring& value,
                            std::function<void()> act) {
    gfx_.text(lbl, {c.x, y, c.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 20;
    const Rect b{c.x, y, c.w, 30};
    button(id, b, value, std::move(act));
    gfx_.fillTriangle(b.right() - 20, b.y + 13, b.right() - 10, b.y + 13, b.right() - 15, b.y + 18, pal_.inkMuted);
    y += 42;
}

void StudioApp::drawPanel(const Rect& r) {
    gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
    gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    if (!(focus_ == loadedFocus_)) loadFields();
    gfx_.pushClip(r);
    setHitClip(r);
    const Rect c{r.x + 20, r.y, r.w - 40, 0};
    float y = r.y + 18 - panelScroll_;

    auto heading = [&](const std::wstring& kicker, bool first = false) { panelHeading(c, y, kicker, first); };
    auto statusButton = [&](const std::wstring& current) { panelChoice("panel:status", c, y, L"Status", current, [this] { popupStatusMenu(); }); };

    if (focus_.kind == Focus::Picture) {
        drawPicturePanel(c, y);
    } else if (focus_.kind == Focus::Comment) {
        drawCommentPanel(c, y);
    } else if (focus_.kind == Focus::Artwork) {
        drawArtworkPanel(c, y);
    } else if (focus_.kind == Focus::Node) {
        drawNodePanel(r, c, y);
    } else if (focus_.kind == Focus::None) {
        heading(L"DETAILS", true);
        y += gfx_.text(L"Select a world, series or book to see and edit its details here. Changes save as you type.",
                       {c.x, y, c.w, 80}, Font::Small, pal_.inkMuted) + 12;
    } else if (focus_.kind == Focus::World) {
        const World* wd = lib_->world(focus_.id);
        if (wd) {
            heading(L"WORLD", true);
            y = panelField(c, y, L"Name", FName);
            y = panelField(c, y, L"Description", FDescription, 110);
            heading(L"LOOK");
            gfx_.text(L"Colour", {c.x, y, c.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
            y += 22;
            static const char* swatches[] = {"#2D4A6E", "#6A2E3F", "#B4442C", "#2F6B5A", "#5B4B8A", "#8A6A12", "#3E5C76", "#4A4A48"};
            float sx = c.x;
            for (const char* sw : swatches) {
                const Rect sr{sx, y, 26, 26};
                const std::string sid = std::string("panel:swatch:") + sw;
                addHit(sid, sr, [this, sw] {
                    if (const World* x = lib_->world(focus_.id)) {
                        commitFields();
                        World n = *lib_->world(focus_.id);
                        n.accent = sw;
                        lib_->updateWorld(n);
                        setFieldText(FAccent, sw);
                        (void)x;
                    }
                });
                gfx_.fillRounded(sr, 3, hexColor(sw));
                if (wd->accent == sw) gfx_.strokeRounded(sr.inset(-3), 4, pal_.ink, 2);
                if (focused(sid)) focusRing(sr);
                sx += 32;
            }
            y += 36;
            y = panelField(c, y, L"Custom colour (#RRGGBB)", FAccent);
            y = panelImage(c, y, L"Banner", wd->bannerBlob, [this] { chooseImage(Focus::World, focus_.id, 0); }, [this] {
                World n = *lib_->world(focus_.id); n.bannerBlob.clear(); lib_->updateWorld(n);
            }, true);
            y = panelImage(c, y, L"Logo", wd->logoBlob, [this] { chooseImage(Focus::World, focus_.id, 1); }, [this] {
                World n = *lib_->world(focus_.id); n.logoBlob.clear(); lib_->updateWorld(n);
            }, false);
            heading(L"NOTES");
            y = panelField(c, y, L"Notes", FNotes, 150);
        }
    } else if (focus_.kind == Focus::Series) {
        const Series* s = lib_->seriesById(focus_.id);
        if (s) {
            heading(L"SERIES", true);
            y = panelField(c, y, L"Name", FName);
            statusButton(w(label(s->status)));
            y = panelField(c, y, L"Description", FDescription, 110);
            heading(L"LOOK");
            y = panelImage(c, y, L"Cover", s->coverBlob, [this] { chooseImage(Focus::Series, focus_.id, 0); }, [this] {
                Series n = *lib_->seriesById(focus_.id); n.coverBlob.clear(); lib_->updateSeries(n);
            }, false);
            y = panelImage(c, y, L"Logo", s->logoBlob, [this] { chooseImage(Focus::Series, focus_.id, 1); }, [this] {
                Series n = *lib_->seriesById(focus_.id); n.logoBlob.clear(); lib_->updateSeries(n);
            }, false);
            heading(L"NOTES");
            y = panelField(c, y, L"Series notes and planning", FNotes, 150);
        }
    } else if (focus_.kind == Focus::Book) {
        const Book* b = lib_->book(focus_.id);
        if (b) {
            heading(L"BOOK", true);
            y = panelField(c, y, L"Title", FName);
            y = panelField(c, y, L"Subtitle", FSubtitle);
            statusButton(w(label(b->status)));
            button("panel:write", {c.x, y, c.w, 34}, L"Open in Writing", [this] { openWriting(focus_.id); }, true);
            y += 46;
            if (const Membership* m = lib_->membership(focus_.contextSeries, b->id)) {
                const Series* s = lib_->seriesById(m->seriesId);
                gfx_.fillRounded({c.x - 8, y - 6, c.w + 16, 126}, 3, pal_.sunken);
                gfx_.text(L"In “" + (s ? w(s->name) : L"") + L"”", {c.x, y, c.w, 18}, Font::SmallStrong, pal_.ink,
                          {Align::Left, false, true});
                y += 24;
                const float half = (c.w - 10) / 2;
                gfx_.text(L"Number", {c.x, y, half, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
                gfx_.text(L"Kind", {c.x + half + 10, y, half, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
                y += 20;
                const Rect nb{c.x, y, half, 30};
                gfx_.fillRounded(nb, 3, pal_.surface);
                gfx_.strokeRounded(nb, 3, GetFocus() == fields_[FNumber] ? pal_.ribbon : pal_.rule);
                if (nb.y >= panelRect_.y + 40 && nb.bottom() <= panelRect_.bottom()) placeField(FNumber, nb);
                const Rect kb{c.x + half + 10, y, half, 30};
                button("panel:kind", kb, w(label(m->kind)), [this] { popupKindMenu(); });
                gfx_.fillTriangle(kb.right() - 20, kb.y + 13, kb.right() - 10, kb.y + 13, kb.right() - 15, kb.y + 18, pal_.inkMuted);
                y += 38;
                linkText("panel:leave", c.x, y, L"Remove from this series", [this] {
                    commitFields();
                    lib_->removeBookFromSeries(focus_.id, focus_.contextSeries);
                    setFocus({Focus::Book, focus_.id, {}});
                });
                y += 34;
            }
            heading(L"COVER");
            y = panelImage(c, y, L"Cover", b->coverBlob, [this] { chooseImage(Focus::Book, focus_.id, 0); }, [this] {
                Book n = *lib_->book(focus_.id); n.coverBlob.clear(); lib_->updateBook(n);
            }, false);
            heading(L"PUBLISHING DETAILS");
            panelChoice("panel:trim", c, y, L"Trim size (for print)", w(trimSize(b->trim).label), [this] { popupTrimMenu(); });
            y = panelField(c, y, L"Author", FAuthor);
            y = panelField(c, y, L"Pen name", FPenName);
            y = panelField(c, y, L"Genre", FGenre);
            const float half = (c.w - 10) / 2;
            {
                const float y0 = y;
                const float ya = panelField({c.x, 0, half, 0}, y0, L"Language", FLanguage);
                panelField({c.x + half + 10, 0, half, 0}, y0, L"Edition", FEdition);
                y = ya;
            }
            if (const auto eds = lib_->editionsOf(b->id); !eds.empty()) {
                // Once a book has editions, each keeps its own ISBN (set in Publishing).
                gfx_.text(L"ISBNs", {c.x, y, c.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
                y += 20;
                for (const Edition* e : eds) {
                    const std::wstring nm = w(e->name.empty() ? std::string(editionKindLabel(e->kind)) : e->name);
                    gfx_.text(nm, {c.x, y, c.w * 0.45f, 20}, Font::Small, pal_.ink, {Align::Left, false, true});
                    gfx_.text(e->isbn.empty() ? L"None yet" : w(e->isbn), {c.x + c.w * 0.45f, y, c.w * 0.55f, 20}, Font::Small,
                              e->isbn.empty() ? pal_.inkFaint : pal_.ink, {Align::Right, false, true});
                    y += 22;
                }
                linkText("panel:editions", c.x, y + 2, L"Editions and ISBNs: Publishing", [this, id = b->id] { openPublishing(id); });
                y += 34;
            } else {
                y = panelField(c, y, L"ISBN", FIsbn);
            }
            y = panelField(c, y, L"Tags (separate with commas)", FTags);
            heading(L"SYNOPSIS");
            y = panelField(c, y, L"Synopsis", FSynopsis, 150);
            if (view_ != View::Book) {
                linkText("panel:open", c.x, y, L"Open the book's page", [this] { goTo(View::Book, focus_.id); });
                y += 30;
            }
        }
    }
    // The same closing row for everything in the panel.
    if (focus_.kind != Focus::None && focus_.kind != Focus::Picture && focus_.kind != Focus::Comment) {
        heading(L"MORE");
        float lx = c.x;
        auto link = [&](const std::string& id, const std::wstring& t, std::function<void()> act) {
            linkText(id, lx, y, t, std::move(act));
            lx += gfx_.measureWidth(t, Font::Small) + 18;
        };
        if (focus_.kind == Focus::Node) {
            link("panel:up", L"Move up", [this] { moveNodeBy(focus_.id, -1); });
            link("panel:down", L"Move down", [this] { moveNodeBy(focus_.id, 1); });
        } else if (focus_.kind == Focus::Book) {
            link("panel:importinto", L"Import into this book...", [this] { startImport(focus_.id); });
        }
        link("panel:trash", L"Move to Trash", [this] { trashFocused(); });
        y += 34;
    }
    panelContent_ = y + panelScroll_ - r.y + 20;
    clearHitClip();
    gfx_.popClip();
}

// ---------------------------------------------------------------- settings

float StudioApp::pageSettings(const Rect& r0) {
    const Rect r{r0.x, r0.y, std::min(r0.w, 760.0f), 0};
    float y = r.y + pageHeader(r, L"WRITING STUDIO", L"Settings", L"Your library, how the Studio looks, and the keyboard shortcuts.");
    auto section = [&](const std::wstring& t) {
        y += 8;
        gfx_.line(r.x, y + 0.5f, r.right(), y + 0.5f, pal_.rule);
        y += 22;
        gfx_.text(t, {r.x, y, r.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        y += 28;
    };
    auto rowLabel = [&](const std::wstring& t, float h = 28) {
        gfx_.text(t, {r.x, y, 200, h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
    };

    section(L"LIBRARY");
    y += gfx_.text(w(lib_->folder()), {r.x, y, r.w, 40}, Font::BodyStrong, pal_.ink) + 4;
    y += gfx_.text(L"Your library is a folder. library.db holds your worlds, series, books and text (every change is saved at once); "
                   L"objects holds covers and pictures; backups has a full copy made each time the library opens (the newest 10). "
                   L"Keep it out of OneDrive or Dropbox while the Studio is open.",
                   {r.x, y, r.w, 100}, Font::Small, pal_.inkMuted) + 14;
    {
        float bx = r.x, bw = 0;
        smallButton("set:open", bx, y, L"Open or create a library...", [this] { chooseLibraryFolder(); }, &bw); bx += bw + 8;
        smallButton("set:folder", bx, y, L"Show the folder", [this] {
            ShellExecuteW(hwnd(), L"open", w(lib_->folder()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }, &bw); bx += bw + 8;
        smallButton("set:export", bx, y, L"Export...", [this] { exportLibrary(); }, &bw); bx += bw + 8;
        smallButton("set:backup", bx, y, L"Back up now", [this] { backupNow(); }, &bw);
        y += 48;
    }

    section(L"APPEARANCE");
    const float cx = r.x + 200, cw = std::min(360.0f, r.w - 200);
    rowLabel(L"Theme");
    segmented("set:theme", {cx, y, cw, 28}, {L"As Windows", L"Light", L"Dark"}, settings_.theme, [this](int i) {
        settings_.theme = i; saveSettings(); applyTheme();
    });
    y += 40;
    rowLabel(L"Show books as");
    segmented("set:gallery", {cx, y, cw, 28}, {L"Bookshelf", L"List"}, settings_.gallery ? 0 : 1, [this](int i) {
        settings_.gallery = i == 0; saveSettings();
    });
    y += 40;
    rowLabel(L"Measurements in");
    {
        static const char* units[] = {"in", "cm", "mm", "px"};
        int ui = 0;
        for (int i = 0; i < 4; ++i) if (settings_.units == units[i]) ui = i;
        segmented("set:units", {cx, y, cw, 28}, {L"Inches", L"cm", L"mm", L"Pixels"}, ui, [this](int i) {
            static const char* u[] = {"in", "cm", "mm", "px"};
            settings_.units = u[i]; saveSettings();
        });
    }
    y += 40;
    rowLabel(L"Details panel");
    segmented("set:panel", {cx, y, cw, 28}, {L"Shown", L"Hidden"}, settings_.showPanel ? 0 : 1, [this](int i) {
        settings_.showPanel = i == 0; saveSettings();
    });
    y += 48;

    section(L"KEYBOARD SHORTCUTS");
    struct K { const wchar_t* keys; const wchar_t* what; };
    auto group = [&](const std::wstring& name, std::initializer_list<K> keys) {
        gfx_.text(name, {r.x, y, r.w, 20}, Font::SmallStrong, pal_.ink, {Align::Left, false});
        y += 24;
        for (const K& k : keys) {
            gfx_.text(k.what, {r.x, y, r.w - 220, 22}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
            gfx_.text(k.keys, {r.right() - 210, y, 210, 22}, Font::Small, pal_.ink, {Align::Right, false, true});
            gfx_.line(r.x, y + 22.5f, r.right(), y + 22.5f, pal_.rule.withAlpha(0.5f));
            y += 26;
        }
        y += 14;
    };
    group(L"Library", {{L"Ctrl+N", L"New book"}, {L"Ctrl+Shift+N", L"New series"}, {L"Ctrl+Shift+W", L"New world"},
                       {L"Ctrl+Shift+I", L"Import a book"}, {L"F2", L"Rename"}, {L"Del", L"Move to Trash"},
                       {L"Ctrl+Z", L"Undo Move to Trash"}, {L"Ctrl+1 / Ctrl+2", L"Bookshelf / list"}, {L"F4", L"Show or hide the details panel"},
                       {L"Ctrl+O", L"Open or create a library"}, {L"Ctrl+E", L"Export the library"}, {L"Ctrl+,", L"Settings"}});
    group(L"Writing", {{L"Ctrl+W", L"Open the current book in Writing (or double-click it)"},
                       {L"Ctrl+Up / Ctrl+Down", L"Previous / next chapter"}, {L"Up / Down", L"Move through the outline"},
                       {L"Alt+Up / Alt+Down", L"Move a chapter or scene up or down"}, {L"Enter", L"From the outline or a title, go to the text"}});
    group(L"Typesetting", {{L"Left / Right, Page Up / Page Down", L"Previous / next pages"}, {L"Home / End", L"First / last pages"}});
    group(L"Text", {{L"Ctrl+B / Ctrl+I / Ctrl+U", L"Bold / italic / underline"}, {L"Ctrl+Shift+K", L"Small capitals"},
                    {L"Ctrl+K", L"Link"}, {L"Ctrl+Alt+M", L"Comment on the selected words"}, {L"Ctrl+Enter", L"Scene break (a *** divider; + Scene adds one at the end)"}, {L"Shift+Enter", L"Line break"},
                    {L"Ctrl+Alt+0", L"Body text"}, {L"Ctrl+Alt+1 / 2 / 3", L"Headings"}, {L"Ctrl+Z / Ctrl+Y", L"Undo / redo"}});

    section(L"ABOUT");
    y += gfx_.text(L"NorthRay Writing Studio " CJ_VERSION_WSTR L", from " CJ_DEVELOPER_WSTR L".", {r.x, y, r.w, 22},
                   Font::BodyStrong, pal_.ink) + 6;
    y += gfx_.text(L"The Author Library, the manuscript editor with saved versions and comments, planning, worldbuilding, review, an "
                   L"artwork library, typesetting, print and ebook previews, and print-ready PDF and EPUB files. It imports EPUB, Word, "
                   L"OpenDocument, web pages, Markdown and text (PDF importing is experimental). No account or subscription is needed; "
                   L"your work stays on this PC.",
                   {r.x, y, r.w, 100}, Font::Small, pal_.inkMuted) + 8;
    y += gfx_.text(L"Uses SQLite (public domain), ProseMirror (MIT), miniz (MIT), PDFium (BSD/Apache), HarfBuzz (MIT), FreeType (FreeType "
                   L"Licence), stb_image and stb_image_write (public domain), hyph-utf8 hyphenation patterns, the Crimson Text and Gentium Book fonts (SIL Open "
                   L"Font License) and Microsoft Edge WebView2. "
                   L"Licences are in the licenses folder next to the program.",
                   {r.x, y, r.w, 60}, Font::Small, pal_.inkFaint) + 10;
    return y - r.y + 20;
}

}  // namespace nrs


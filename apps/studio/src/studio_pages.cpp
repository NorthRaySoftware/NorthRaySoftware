// Drawing: workspace bar, library sidebar, pages, properties panel.
#include "studio_app.hpp"
#include "cjui/icons.hpp"
#include "cjui/northray.hpp"
#include "cjui/menu.hpp"

#include "cjui/format.hpp"
#include "cjui/http.hpp"
#include "version.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace nrs {

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

using namespace studio;

static std::wstring w(const std::string& s) { return widen(s); }

static std::wstring upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

static std::wstring plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

// ------------------------------------------------------------------- frame

void StudioApp::drawFrame(float W, float H) {
    drops_.clear();
    // Focus mode (Writing): the text alone, without the outline and the details panel.
    const bool focusing = focusMode() && lib_ && lib_->book(writingBook_);
    const bool settingsPage = ws_ == Workspace::Library && view_ == View::Settings;
    // The second redesign (0.68): Worldbuilding has no sidebar and no details panel. Its pages have tabs and a tools
    // menu, entries are edited where they're shown, and the few pages that keep a details column have it as a card.
    const bool worldLayout = ws_ == Workspace::Worldbuilding && !focusing;
    const bool worldCard = worldLayout && (!wb_.board.empty() || wb_.languages || (wb_.calendarPage && wb_.calMode == 1) || wb_.share || wb_.syncPage || wb_.relationsPage ||
                                           wb_.fontsPage || (!wb_.familyRoot.empty() && wb_.entry.empty()));
    // Planning too (0.69): its views are tabs, and a card's details are a pop-over beside it on the corkboard, or a card at
    // the right of the other views.
    const bool planLayout = ws_ == Workspace::Planning && !focusing;
    // And the Library (0.69): its places are tabs along the top; a book has its own page with tabs; a world's, series'
    // or picture's details are a card at the right.
    const bool libLayout = ws_ == Workspace::Library && !settingsPage && !focusing;
    const bool libCard = libLayout && lib_ && view_ != View::Book && view_ != View::FromGitHub &&
                         (focus_.kind == Focus::World || focus_.kind == Focus::Series || focus_.kind == Focus::Artwork ||
                          focus_.kind == Focus::Picture);
    // Writing (0.70): a narrower outline (F3 hides it), a bar over the page for the chapter, and the details only when
    // there's something to show: a picture or comment chosen in the text, or the chapter's details asked for (F4).
    const bool writeLayout = ws_ == Workspace::Writing && !focusing && lib_ && lib_->book(writingBook_);
    const bool writeCard = writeLayout && (focus_.kind == Focus::Picture || focus_.kind == Focus::Comment || settings_.writingDetails);
    // Review, Typesetting, Preview and Publishing (0.71): a header with the book, its parts as tabs where it has them and
    // a tools menu; their choices or details as a card at the right (Typesetting's design and Preview's reader's
    // settings can be put away).
    const bool bookWs = !focusing && lib_ && (ws_ == Workspace::Review || ws_ == Workspace::Typesetting || ws_ == Workspace::Preview ||
                                              ws_ == Workspace::Publishing);
    const bool bookCard = bookWs && ((ws_ == Workspace::Review && ((rv_.section == 1 && !rv_.snapshot.empty()) || rv_.section == 3)) ||
                                     (ws_ == Workspace::Typesetting && settings_.typesetDesign) ||
                                     (ws_ == Workspace::Preview && settings_.previewChoices) || ws_ == Workspace::Publishing);
    const float sideW = focusing || worldLayout || planLayout || libLayout || bookWs || (!settings_.showSide && !settingsPage) ? 0
                      : writeLayout ? kWriteSideW : kSideW;
    const bool panel = !focusing && !worldLayout && !planLayout && !libLayout && !writeLayout && !bookWs && settings_.showPanel && lib_ && !settingsPage;
    panelAsCard_ = worldCard || planLayout || libCard || writeCard || bookCard;
    // The workspaces' rail down the left edge (UI pass, 0.64); gone in focus mode, like everything but the text.
    const float railW = focusing ? 0 : kRailW;
    railRect_ = {0, kTopH, railW, H - kTopH};
    const float centreW = W - railW - sideW - (panel ? kPanelW : 0) - (libCard || writeCard || bookCard ? kPanelW + 34 : 0);
    // Draws a details or choices card at the right edge.
    auto rightCard = [&](const std::function<void(const Rect&)>& drawIt) {
        panelRect_ = {W - kPanelW - 16, kTopH + 16, kPanelW, H - kTopH - 32};
        card(panelRect_, 14);
        gfx_.pushClip(panelRect_.inset(0, 6));
        drawIt(panelRect_);
        gfx_.popClip();
    };
    sideRect_ = {railW, kTopH, sideW, H - kTopH};
    panelRect_ = panel ? Rect{W - kPanelW, kTopH, kPanelW, H - kTopH} : Rect{};
    pageRect_ = {railW + sideW, kTopH, centreW, H - kTopH};

    gfx_.fill({0, 0, W, H}, pal_.paper);
    const float pad = centreW > 900 ? 40 : 28;
    if (ws_ != Workspace::Preview) preview_.hide();
    if (ws_ == Workspace::Preview && lib_ && lib_->book(pv_.book)) {
        editor_.hide();
        Rect headRect{};
        if (bookWs) {   // the head over the device (0.71)
            headRect = {pageRect_.x, pageRect_.y, pageRect_.w, kViewerHeadH};
            pageRect_.y += kViewerHeadH;
            pageRect_.h -= kViewerHeadH;
        }
        gfx_.fill(pageRect_, pal_.dark ? hexColor("#121214") : hexColor("#DCD7CC"));
        if (!previewNote_.empty()) {
            const Rect m{pageRect_.x + pad, pageRect_.y + 26, std::min(centreW - pad * 2, 620.0f), 0};
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
        if (sideRect_.w > 0) drawPreviewSidebar(sideRect_);
        if (panel) drawPreviewPanel(panelRect_);
        if (headRect.w > 0) drawPreviewHead(headRect);
        if (bookCard) rightCard([this](const Rect& rr) { drawPreviewSidebar(rr); });
        { StudioTimer t_("drawRail"); drawRail(railRect_); }
        { StudioTimer t_("drawTopBar"); drawTopBar({0, 0, W, kTopH}); }
        drawStatusBar({0, H, W, 0});
        hideUnplacedFields();
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Preview) ws_ = Workspace::Library;
    if (ws_ == Workspace::Typesetting && lib_ && lib_->book(ts_.book)) {
        editor_.hide();
        Rect headRect{};
        if (bookWs) {
            headRect = {pageRect_.x, pageRect_.y, pageRect_.w, kViewerHeadH};
            pageRect_.y += kViewerHeadH;
            pageRect_.h -= kViewerHeadH;
        }
        { StudioTimer t_("drawTypesetPages"); drawTypesetPages(pageRect_); }
        if (sideRect_.w > 0) drawTypesetSidebar(sideRect_);
        if (panel) drawTypesetPanel(panelRect_);
        if (headRect.w > 0) drawTypesetHead(headRect);
        if (bookCard) rightCard([this](const Rect& rr) { drawTypesetSidebar(rr); });
        drawRail(railRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H, W, 0});
        hideUnplacedFields();
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Typesetting) ws_ = Workspace::Library;   // its book went to the Trash
    if (ws_ == Workspace::Publishing && lib_ && lib_->book(pb_.book)) {
        editor_.hide();
        gfx_.fill(pageRect_, pal_.paper);
        if (cv_.open && (cv_.book != pb_.book || focus_.kind != Focus::Cover)) {   // left by another way: put away
            saveCoverDesign();
            if (!cv_.preview.empty()) { gfx_.forgetImage(cv_.preview); DeleteFileW(cv_.preview.c_str()); }
            cv_ = {};
        }
        const bool designing = cv_.open;
        if (designing) drawCoverDesigner(pageRect_);
        else { StudioTimer t_("drawPublishPage"); drawPublishPage(pageRect_); }
        if (sideRect_.w > 0) drawPublishSidebar(sideRect_);
        if (panel) { if (designing) drawCoverPanel(panelRect_); else drawPublishPanel(panelRect_); }
        if (bookCard) rightCard([this, designing](const Rect& rr) { if (designing) drawCoverPanel(rr); else drawPublishPanel(rr); });
        drawRail(railRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H, W, 0});
        hideUnplacedFields();
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Publishing) ws_ = Workspace::Library;
    if (ws_ == Workspace::Worldbuilding && lib_ && (wb_.world.empty() ? lib_->book(wb_.book) && !lib_->book(wb_.book)->deleted
                                                                       : lib_->world(wb_.world) && !lib_->world(wb_.world)->deleted)) {
        editor_.hide();
        gfx_.fill(pageRect_, pal_.paper);
        const bool worldSub = worldCard || !wb_.familyRoot.empty() || !wb_.mapView.empty() || wb_.checks || wb_.glossary || wb_.calendarPage;
        if (worldSub) {
            // One of the bible's tools: a strip along its top back to the bible, and any details as a card at the right.
            const float cardW = worldCard ? std::min(kPanelW + 10, pageRect_.w * 0.4f) : 0;
            const Rect page{pageRect_.x, pageRect_.y + 44, pageRect_.w - (worldCard ? cardW + 24 : 0), pageRect_.h - 44};
            { StudioTimer t_("drawWorldPage"); drawWorldPage(page); }
            drawWorldStrip({pageRect_.x, pageRect_.y, page.w, 44});
            if (worldCard) {
                panelRect_ = {pageRect_.right() - cardW - 16, pageRect_.y + 16, cardW, pageRect_.h - 32};
                card(panelRect_, 14);
                gfx_.pushClip(panelRect_.inset(0, 6));
                drawWorldPanel(panelRect_);
                gfx_.popClip();
            }
        } else {
            { StudioTimer t_("drawWorldPage"); drawWorldPage(pageRect_); }
        }
        drawRail(railRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H, W, 0});
        hideUnplacedFields();
        if (drag_.active) {   // a creature being moved in the tree
            if (const Drop* d = dropAt(mouseX(), mouseY())) gfx_.strokeRounded(d->rect.inset(1), 4, pal_.ribbon, 2);
            const float lw = std::min(gfx_.measureWidth(drag_.label, Font::BodyStrong) + 24, 320.0f);
            const Rect g{mouseX() + 14, mouseY() + 10, lw, 30};
            gfx_.fillRounded(g, 3, pal_.ink.withAlpha(0.88f));
            gfx_.text(drag_.label, g.inset(12, 0), Font::BodyStrong, pal_.paper, {Align::Left, false, true, true});
        }
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Worldbuilding) ws_ = Workspace::Library;
    if (ws_ == Workspace::Planning && lib_ && lib_->book(pl_.book) && !lib_->book(pl_.book)->deleted) {
        editor_.hide();
        gfx_.fill(pageRect_, pal_.paper);
        const ManuscriptNode* seln = pl_.node.empty() ? nullptr : lib_->node(pl_.node);
        if (seln && seln->deleted) seln = nullptr;
        if (focus_.kind != Focus::Plan) pl_.targetOpen = false;
        const bool popover = seln && pl_.view == 0 && !focusing;
        const bool details = !focusing && !popover && (seln || focus_.kind == Focus::Thread || focus_.kind == Focus::Beat ||
                                                       focus_.kind == Focus::Structure || pl_.targetOpen);
        const float cardW = std::min(kPanelW + 10, pageRect_.w * 0.4f);
        const Rect page{pageRect_.x, pageRect_.y, pageRect_.w - (details ? cardW + 24 : 0), pageRect_.h};
        if (pl_.view == 1) { StudioTimer t_("drawPlanTimeline"); drawPlanTimeline(page); }
        else if (pl_.view == 2) { StudioTimer t_("drawPlanStructure"); drawPlanStructure(page); }
        else if (pl_.view == 3) { StudioTimer t_("drawPlanThreads"); drawPlanThreads(page); }
        else { StudioTimer t_("drawPlanBoard"); drawPlanBoard(page); }
        if (drag_.active && pl_.view == 0) {
            // Where a dragged card would go: a bar beside the card it's over, or a frame round a chapter it would join.
            if (const Drop* d = dropAt(mouseX(), mouseY())) {
                const ManuscriptNode* t = lib_->node(d->id);
                const ManuscriptNode* mv = lib_->node(drag_.id);
                const bool middle = d->into || (d->horizontal ? mouseX() > d->rect.x + d->rect.w / 3 && mouseX() < d->rect.right() - d->rect.w / 3
                                                              : mouseY() > d->rect.y + d->rect.h / 3 && mouseY() < d->rect.bottom() - d->rect.h / 3);
                const bool into = t && mv && middle && Library::canContain(t->kind, mv->kind);
                if (into) gfx_.strokeRounded(d->rect.inset(-3), 6, pal_.ribbon, 2.5f);
                else if (d->horizontal)
                    gfx_.fillRounded({mouseX() < d->rect.x + d->rect.w / 2 ? d->rect.x - 9 : d->rect.right() + 5, d->rect.y, 4, d->rect.h}, 2, pal_.ribbon);
                else
                    gfx_.fillRounded({d->rect.x, mouseY() < d->rect.y + d->rect.h / 2 ? d->rect.y - 9 : d->rect.bottom() + 5, d->rect.w, 4}, 2, pal_.ribbon);
            }
            const float lw = std::min(gfx_.measureWidth(drag_.label, Font::BodyStrong) + 24, 320.0f);
            const Rect g{mouseX() + 14, mouseY() + 10, lw, 30};
            gfx_.fillRounded(g, 3, pal_.ink.withAlpha(0.88f));
            gfx_.text(drag_.label, g.inset(12, 0), Font::BodyStrong, pal_.paper, {Align::Left, false, true, true});
        }
        // The details: beside the selected card, or as a card at the right; a cross puts them away.
        Rect dr{};
        if (popover && pl_.selCard.w > 0 && !drag_.active) {
            const float pw = 340, ph = std::min(600.0f, page.h - 24);
            const Rect sc = pl_.selCard;
            float px = sc.right() + 14;
            if (px + pw > page.right() - 12) px = sc.x - 14 - pw;
            if (px < page.x + 12) px = std::clamp(sc.x + sc.w / 2 - pw / 2, page.x + 12, page.right() - pw - 12);
            const float py = std::clamp(sc.y - 10, page.y + 12, page.bottom() - ph - 12);
            dr = {px, py, pw, ph};
            softShadow(dr, 14, 3.5f);
        } else if (details) {
            dr = {pageRect_.right() - cardW - 16, pageRect_.y + 16, cardW, pageRect_.h - 32};
        }
        if (dr.w > 0) {
            panelRect_ = dr;
            addHit("pl:details", dr, nullptr, false);   // clicks on it stay on it
            card(dr, 14);
            gfx_.pushClip(dr.inset(0, 6));
            drawPlanPanel(dr);
            gfx_.popClip();
            const Rect xr{dr.right() - 38, dr.y + 10, 28, 28};
            addHit("pl:details:close", xr, [this] { pl_.targetOpen = false; selectCard({}); });
            tooltip("pl:details:close", L"Close   Esc");
            const float hv = hoverAmount("pl:details:close");
            if (hv > 0.01f) gfx_.fillRounded(xr, 7, pal_.sunken.withAlpha(hv));
            const float cx = xr.x + xr.w / 2, cy = xr.y + xr.h / 2;
            gfx_.line(cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f, pal_.inkMuted, 1.5f);
            gfx_.line(cx - 4.5f, cy + 4.5f, cx + 4.5f, cy - 4.5f, pal_.inkMuted, 1.5f);
        } else {
            panelRect_ = {};
        }
        drawRail(railRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H, W, 0});
        hideUnplacedFields();
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Planning) ws_ = Workspace::Library;
    if (ws_ == Workspace::Review && lib_ && lib_->book(rv_.book) && !lib_->book(rv_.book)->deleted) {
        editor_.hide();
        gfx_.fill(pageRect_, pal_.paper);
        { StudioTimer t_("drawReviewPage"); drawReviewPage(pageRect_); }
        if (sideRect_.w > 0) drawReviewSidebar(sideRect_);
        if (panel) drawReviewPanel(panelRect_);
        if (bookCard) rightCard([this](const Rect& rr) { drawReviewPanel(rr); });
        else if (bookWs) panelRect_ = {};
        drawRail(railRect_);
        drawTopBar({0, 0, W, kTopH});
        drawStatusBar({0, H, W, 0});
        hideUnplacedFields();
        lastDrops_ = drops_;
        return;
    }
    if (ws_ == Workspace::Review) ws_ = Workspace::Library;
    if (ws_ == Workspace::Writing && lib_ && lib_->book(writingBook_)) {
        // The chapter's bar over the page (0.70); the editor fills the rest of the centre (a separate window placed over it).
        Rect stripRect{};
        if (writeLayout && !currentNode_.empty()) {
            stripRect = {pageRect_.x, pageRect_.y, pageRect_.w, kWriteBarH};
            pageRect_.y += kWriteBarH;
            pageRect_.h -= kWriteBarH;
        }
        // The editor fills the centre; it's a separate window placed over this area.
        const Rect m{pageRect_.x + pad, pageRect_.y + 26, std::min(centreW - pad * 2, 620.0f), 0};
        if (!editorNote_.empty()) {
            float y = m.y + pageHeader(m, L"WRITING", L"The editor isn't available", editorNote_);
            linkText("ed:webview", m.x, y - 6, L"Get the Microsoft Edge WebView2 Runtime",
                     [] { cjui::openInBrowser("https://developer.microsoft.com/microsoft-edge/webview2/"); });
        } else if (currentNode_.empty()) {
            emptyState({pageRect_.x + pad, pageRect_.y + 26, centreW - pad * 2, pageRect_.h - 60}, cjui::Icon::Writing,
                       L"Pick a chapter on the left, or start a new one.", L"+ Chapter", [this] { addNode(NodeKind::Chapter); });
        } else if (!editor_.ready()) {
            gfx_.text(L"Opening the editor...", {m.x, m.y + 20, 400, 30}, Font::Body, pal_.inkFaint);
        }
        placeEditor();
        if (!focusing) {
            // The outline, with today's words at its foot (where the status bar used to say it).
            drawBinder({sideRect_.x, sideRect_.y, sideRect_.w, sideRect_.h - 36});
            const Rect foot{sideRect_.x, sideRect_.bottom() - 36, sideRect_.w, 36};
            gfx_.fill(foot, pal_.sunken);
            gfx_.line(foot.x + 16, foot.y + 0.5f, foot.right() - 16, foot.y + 0.5f, pal_.rule);
            gfx_.line(foot.right() - 0.5f, foot.y, foot.right() - 0.5f, foot.bottom(), pal_.rule);
            gfx_.text(todayLine(), {foot.x + 22, foot.y, foot.w - 36, foot.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        }
        if (panel) drawPanel(panelRect_);
        if (stripRect.w > 0) drawWritingBar(stripRect);
        if (writeCard) {
            panelRect_ = {W - kPanelW - 16, kTopH + 16, kPanelW, H - kTopH - 32};
            card(panelRect_, 14);
            gfx_.pushClip(panelRect_.inset(0, 6));
            drawPanel(panelRect_);
            gfx_.popClip();
            if (focus_.kind != Focus::Picture && focus_.kind != Focus::Comment) {
                const Rect xr{panelRect_.right() - 38, panelRect_.y + 10, 28, 28};
                addHit("wr:details:close", xr, [this] { settings_.writingDetails = false; saveSettings(); invalidate(); });
                tooltip("wr:details:close", L"Close   F4");
                const float hv = hoverAmount("wr:details:close");
                if (hv > 0.01f) gfx_.fillRounded(xr, 7, pal_.sunken.withAlpha(hv));
                const float cx = xr.x + xr.w / 2, cy = xr.y + xr.h / 2;
                gfx_.line(cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f, pal_.inkMuted, 1.5f);
                gfx_.line(cx - 4.5f, cy + 4.5f, cx + 4.5f, cy - 4.5f, pal_.inkMuted, 1.5f);
            }
        } else if (writeLayout) {
            panelRect_ = {};
        }
        drawRail(railRect_);
        drawTopBar({0, 0, W, kTopH});
        {   // Today's words against the daily goal: a thin bar along the foot of the top bar (0.35).
            const std::int64_t goal = dailyGoal();
            if (goal > 0) {
                namespace nr = cjui::northray;
                const float f = std::clamp((float)wordsToday() / (float)goal, 0.0f, 1.0f);
                const float bw = slideTo("goalbar", W * f);
                gfx_.fill({0, kTopH - 3, W, 3}, nr::kNightInk.withAlpha(0.10f));
                gfx_.fill({0, kTopH - 3, bw, 3}, f >= 1 ? hexColor("#E8C547") : nr::kAuroraGreen);
                addHit("goalbar", {0, kTopH - 5, W, 6}, nullptr, false);
                tooltip("goalbar", todayLine());
            }
        }
        drawStatusBar({0, H, W, 0});
        hideUnplacedFields();
        if (drag_.active) {
            if (const Drop* d = dropAt(mouseX(), mouseY())) {
                const float third = d->rect.h / 3;
                if (mouseY() > d->rect.y + third && mouseY() < d->rect.bottom() - third) drawDropInto(d->rect);
                else drawDropLine(d->rect.x, mouseY() < d->rect.y + d->rect.h / 2 ? d->rect.y - 1 : d->rect.bottom() + 1, d->rect.w, false);
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
    Rect page{pageRect_.x + pad, top + 26, std::min(centreW - pad * 2, 1100.0f), 0};
    float h = 0;
    if (!lib_) h = pageEmpty(page);
    else if (ws_ == Workspace::Import) h = pageImport(page);
    else {
        // The Library's places as tabs (0.69), over every page of it but a book's own, Settings and opening from GitHub.
        if (ws_ == Workspace::Library && view_ != View::Book && view_ != View::Settings && view_ != View::FromGitHub) {
            const float th = libraryTabs(page);
            page.y += th;
            h += th;
        }
        switch (view_) {
        case View::Worlds: h += pageWorlds(page); break;
        case View::AllBooks: h += pageAllBooks(page); break;
        case View::World: h += lib_->world(viewId_) ? pageWorld(page) : pageWorlds(page); break;
        case View::Series: h += lib_->seriesById(viewId_) ? pageSeries(page) : pageAllBooks(page); break;
        case View::Independent: h += pageIndependent(page); break;
        case View::Trash: h += pageTrash(page); break;
        case View::Settings: h += pageSettings(page); break;
        case View::Artwork: h += pageArtwork(page); break;
        case View::FromGitHub: h += pageFromGitHub(page); break;
        case View::Book: h += lib_->book(viewId_) ? pageBook(page) : pageAllBooks(page); break;
        }
    }
    endScroll(h + 60);

    if (sideRect_.w > 0) drawSidebar(sideRect_);
    if (panel) drawPanel(panelRect_);
    if (libCard) {
        panelRect_ = {W - kPanelW - 16, kTopH + 16, kPanelW, H - kTopH - 32};
        card(panelRect_, 14);
        gfx_.pushClip(panelRect_.inset(0, 6));
        drawPanel(panelRect_);
        gfx_.popClip();
    } else if (libLayout) {
        panelRect_ = {};
    }
    drawRail(railRect_);
        drawTopBar({0, 0, W, kTopH});
    drawStatusBar({0, H, W, 0});
    hideUnplacedFields();

    // The thing being dragged, and where it would land.
    if (drag_.active) {
        if (const Drop* d = dropAt(mouseX(), mouseY())) {
            if (d->kind == Drop::ToWorld || d->kind == Drop::ToIndependent || d->kind == Drop::ToSeries) {
                gfx_.strokeRounded(d->rect.inset(1), 3, pal_.ribbon, 2);
            } else if (d->horizontal) {
                const float x = mouseX() > d->rect.x + d->rect.w / 2 ? d->rect.right() + 7 : d->rect.x - 7;
                drawDropLine(x, d->rect.y, d->rect.h, true);
            } else {
                const float y = mouseY() > d->rect.y + d->rect.h / 2 ? d->rect.bottom() + 1 : d->rect.y - 1;
                drawDropLine(d->rect.x, y, d->rect.w, false);
            }
        }
        const float lw = std::min(gfx_.measureWidth(drag_.label, Font::BodyStrong) + 24, 320.0f);
        const Rect g{mouseX() + 14, mouseY() + 10, lw, 30};
        gfx_.fillRounded(g, 3, pal_.ink.withAlpha(0.88f));
        gfx_.text(drag_.label, g.inset(12, 0), Font::BodyStrong, pal_.paper, {Align::Left, false, true, true});
    }
    lastDrops_ = drops_;
}

// Where a dragged chapter, scene or book would land (0.35: clearer): a line with a ring at its start and a soft
// glow, centred on the gap; or, for "inside", the row tinted and outlined.
void StudioApp::drawDropLine(float x, float y, float len, bool vertical) {
    const Color c = pal_.ribbon;
    if (vertical) {
        gfx_.fillRounded({x - 4, y - 2, 8, len + 4}, 4, c.withAlpha(0.16f));
        gfx_.fillRounded({x - 1.5f, y, 3, len}, 1.5f, c);
        gfx_.circle(x, y - 1, 4, pal_.paper, true);
        gfx_.circle(x, y - 1, 4, c, false, 2);
    } else {
        gfx_.fillRounded({x - 2, y - 4, len + 4, 8}, 4, c.withAlpha(0.16f));
        gfx_.fillRounded({x + 4, y - 1.5f, len - 4, 3}, 1.5f, c);
        gfx_.circle(x + 4, y, 4.5f, pal_.paper, true);
        gfx_.circle(x + 4, y, 4.5f, c, false, 2);
    }
}

void StudioApp::drawDropInto(const Rect& r) {
    gfx_.fillRounded(r, 4, pal_.ribbon.withAlpha(0.14f));
    gfx_.strokeRounded(r.inset(1), 4, pal_.ribbon, 2);
}

// ---------------------------------------------------------------- top bar

namespace {
struct Tab {
    const wchar_t* name;
    const wchar_t* shortName;   // under its icon on the rail
    int milestone;
    cjui::Icon icon;
};
const Tab kTabs[] = {{L"Library", L"Library", 1, cjui::Icon::Library},         {L"Writing", L"Write", 2, cjui::Icon::Writing},
                     {L"Planning", L"Plan", 8, cjui::Icon::Planning},          {L"Worldbuilding", L"World", 8, cjui::Icon::World},
                     {L"Review", L"Review", 9, cjui::Icon::Review},            {L"Typesetting", L"Typeset", 5, cjui::Icon::Typeset},
                     {L"Preview", L"Preview", 6, cjui::Icon::Preview},         {L"Publishing", L"Publish", 7, cjui::Icon::Publish}};
}  // namespace

int StudioApp::tabCount() const { return (int)(sizeof kTabs / sizeof kTabs[0]); }

// Which workspace is showing, as its place on the rail (-1: Settings).
bool StudioApp::tabActive(int i) const {
    const Tab& t = kTabs[i];
    const bool planning = wcscmp(t.name, L"Planning") == 0;   // Milestone 8 has two tabs
    return (t.milestone == 1 && ws_ != Workspace::Writing && ws_ != Workspace::Typesetting && ws_ != Workspace::Preview &&
                                 ws_ != Workspace::Publishing && ws_ != Workspace::Worldbuilding && ws_ != Workspace::Planning &&
                                 ws_ != Workspace::Review &&
                                 view_ != View::Settings) ||
                                (t.milestone == 2 && ws_ == Workspace::Writing) || (t.milestone == 5 && ws_ == Workspace::Typesetting) ||
                                (t.milestone == 6 && ws_ == Workspace::Preview) || (t.milestone == 7 && ws_ == Workspace::Publishing) ||
                                (t.milestone == 8 && !planning && ws_ == Workspace::Worldbuilding) ||
                                (t.milestone == 8 && planning && ws_ == Workspace::Planning) || (t.milestone == 9 && ws_ == Workspace::Review);
}

// The book a workspace opens on when nothing names one (0.66): a series' first book, else the book last written in, else
// the one changed most recently. So every place on the rail opens something rather than asking you to choose first.
Id StudioApp::fallbackBook() const {
    if (!lib_) return {};
    if (view_ == View::Series)
        for (const studio::Book* b : lib_->booksInSeries(viewId_)) if (b && !b->deleted) return b->id;
    if (view_ == View::World)
        for (const studio::Book* b : lib_->liveBooks()) if (b->worldId == viewId_) return b->id;
    if (const studio::Book* lb = lib_->book(settings_.lastBook); lb && !lb->deleted) return lb->id;
    Id best;
    Time newest = 0;
    for (const studio::Book* x : lib_->liveBooks()) if (x->modified >= newest) { newest = x->modified; best = x->id; }
    return best;
}

// Opens a workspace from the rail (or Ctrl+1 to Ctrl+8): the book or world in hand, as the tabs did.
void StudioApp::openTab(int i) {
    if (!lib_ || i < 0 || i >= tabCount()) return;
    const Tab& t = kTabs[i];
    const bool planning = wcscmp(t.name, L"Planning") == 0;

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
else if (view_ == View::Settings) goTo(View::Worlds);
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
if (book.empty() || !lib_->book(book) || lib_->book(book)->deleted) book = fallbackBook();
if (book.empty())
    flash(std::wstring(L"Make a book in the Library first, then open ") + (planning ? L"Planning." : L"Review."), 5);
else if (planning) openPlanning(book);
else openReview(book);
    }
    else if (t.milestone == 8) {
Id world, book;
if (!worldbuildingScope(world, book)) flash(L"Make a world or a book in the Library first, then open Worldbuilding.", 5);
else openWorldbuilding(world, book);
    }
    else if (t.milestone >= 5 && t.milestone <= 7) {
Id book = ws_ == Workspace::Writing && lib_->book(writingBook_) ? writingBook_
        : ws_ == Workspace::Typesetting ? ts_.book : ws_ == Workspace::Preview ? pv_.book : ws_ == Workspace::Publishing ? pb_.book
        : focus_.kind == Focus::Book ? focus_.id : view_ == View::Book ? viewId_ : !ts_.book.empty() ? ts_.book : writingBook_;
if (book.empty() || !lib_->book(book) || lib_->book(book)->deleted) book = fallbackBook();
if (book.empty())
    flash(std::wstring(L"Make a book in the Library first, then open ") +
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
if (book.empty()) openLastWriting();   // the book last written in, or the latest changed (0.48.5)
else openWriting(book);
    } else
flash(std::wstring(t.name) + L" isn't built yet. It arrives in Milestone " + std::to_wstring(t.milestone) +
      L".", 6);

}

void StudioApp::drawRail(const Rect& r) {
    if (r.w <= 0) return;
    namespace nr = cjui::northray;
    gfx_.fill(r, pal_.dark ? pal_.sunken : pal_.surface);
    gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), pal_.rule);
    const float itemH = std::clamp((r.h - 80) / 9.0f, 50.0f, 62.0f);
    float y = r.y + 10;
    int activeIndex = -1;
    for (int i = 0; i < tabCount(); ++i)
        if (tabActive(i)) activeIndex = i;
    const bool settingsOn = ws_ == Workspace::Library && view_ == View::Settings;
    // The selection: a soft pill and a bar at the edge, sliding between workspaces.
    const float selY = settingsOn ? r.bottom() - itemH - 10 : y + (activeIndex < 0 ? 0 : activeIndex) * itemH;
    const float sy = slideTo("rail#y", selY);
    if (activeIndex >= 0 || settingsOn) {
        gfx_.fillRounded({r.x + 8, sy + 3, r.w - 16, itemH - 6}, 10, pal_.ribbon.withAlpha(pal_.dark ? 0.16f : 0.12f));
        gfx_.fillRounded({r.x + 2, sy + itemH / 2 - 12, 3, 24}, 1.5f, pal_.ribbon);
    }
    auto item = [&](const std::string& id, float iy, cjui::Icon icon, const wchar_t* label, const std::wstring& tip, bool on,
                    std::function<void()> act) {
        const Rect b{r.x + 6, iy, r.w - 12, itemH};
        addHit(id, b, std::move(act));
        tooltip(id, tip);
        const float hv = hoverAmount(id);
        if (!on && hv > 0.01f) gfx_.fillRounded({b.x + 2, b.y + 3, b.w - 4, b.h - 6}, 10, pal_.ink.withAlpha(0.05f * hv));
        const Color c = on ? pal_.ink : pal_.inkMuted.mix(pal_.ink, hv);
        const float lift = 2.0f * hv;
        cjui::drawIcon(gfx_, icon, {b.x + (b.w - 22) / 2, b.y + 8 - lift, 22, 22}, on ? pal_.ribbon.mix(pal_.ink, pal_.dark ? 0.1f : 0.35f) : c, 1.6f);
        gfx_.text(label, {b.x, b.y + 33 - lift, b.w, 16}, on ? Font::SmallStrong : Font::Small, c, {Align::Center, false, true});
        if (focused(id)) focusRing(b.inset(2));
    };
    for (int i = 0; i < tabCount(); ++i) {
        const Tab& t = kTabs[i];
        item("nav:" + std::to_string(i), y, t.icon, t.shortName, std::wstring(t.name) + L"   Ctrl+" + std::to_wstring(i + 1), i == activeIndex,
             [this, i] { openTab(i); });
        y += itemH;
    }
    item("nav:settings", r.bottom() - itemH - 10, cjui::Icon::Settings, L"Settings", L"Settings   Ctrl+,", settingsOn, [this] { openSettings(); });
}

void StudioApp::drawTopBar(const Rect& r) {
    // The night sky with its aurora band, like the other NorthRay apps' title bars; light text on it.
    namespace nr = cjui::northray;
    nr::nightSky(gfx_, r, 0, 0.3f, 13);   // a quiet aurora: the words on it come first
    nr::auroraBand(gfx_, {r.x, r.y, r.w, 3}, pal_.dark);
    // Minimise, maximise and close end the row (the window has no separate title bar).
    const float capW = drawCaptionButtons({r.x, r.y, r.w, r.h}, true);
    // The app's mark and name: "NorthRay" over "WRITING STUDIO", as the Launcher has it.
    float lx = 14;
    if (gfx_.drawImage(exeDir() + L"\\web\\studio-mark.png", {lx, (r.h - 30) / 2 + 1, 30, 30}, false)) lx += 38;
    gfx_.text(L"NorthRay", {lx, 5, 180, 20}, Font::BodyStrong, nr::kNightInk, {Align::Left, false, true});
    gfx_.text(L"WRITING STUDIO", {lx + 1, 25, 180, 14}, Font::Label, nr::kAuroraGreen.withAlpha(0.9f), {Align::Left, false, true});
    // Where you are, quietly, after the name.
    {
        std::wstring where;
        for (int i = 0; i < tabCount(); ++i)
            if (tabActive(i)) where = kTabs[i].name;
        if (ws_ == Workspace::Library && view_ == View::Settings) where = L"Settings";
        const Book* b = lib_ ? lib_->book(ws_ == Workspace::Writing ? writingBook_ : ws_ == Workspace::Planning ? pl_.book : ws_ == Workspace::Review ? rv_.book
                                          : ws_ == Workspace::Typesetting ? ts_.book : ws_ == Workspace::Preview ? pv_.book
                                          : ws_ == Workspace::Publishing ? pb_.book : Id{}) : nullptr;
        if (b && !b->deleted) where += L"  \u00B7  " + w(b->title);
        const float wx = lx + 130;
        const float sx = std::max(wx + 150, r.w * 0.5f - 230);
        gfx_.text(where, {wx, 0, sx - wx - 16, r.h}, Font::Small, nr::kNightMuted, {Align::Left, false, true, true});
    }
    // The search box in the middle (Ctrl+K): chapters, the bible, books and commands.
    {
        const float sw = std::clamp(r.w * 0.32f, 300.0f, 520.0f);
        const float sx = std::max(lx + 280, r.w * 0.5f - sw / 2);
        topSearchRect_ = {sx, 8, std::min(sw, r.right() - capW - 120 - sx), r.h - 16};
        const Rect sr = topSearchRect_;
        const bool on = GetFocus() == fields_[FTopSearch];
        // On the field's own colour (the box the text is typed in), so the typing and the box are one.
        gfx_.fillRounded(sr, 9, pal_.surface);
        gfx_.strokeRounded(sr, 9, on ? pal_.ribbon : pal_.rule, on ? 1.5f : 1.0f);
        cjui::drawIcon(gfx_, cjui::Icon::Search, {sr.x + 10, sr.y + (sr.h - 15) / 2, 15, 15}, pal_.inkMuted, 1.5f);
        addHit("top:search", sr, [this] { focusTopSearch(); }, false);
        placeField(FTopSearch, {sr.x + 32, sr.y + (sr.h - 22) / 2, sr.w - 92, 22});
        gfx_.text(L"Ctrl+K", {sr.right() - 58, sr.y, 48, sr.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
    }
    // Show or hide the side panels (F3 the outline / sidebar, F4 the details), for more room to read.
    // (Not in Worldbuilding, which has neither since 0.68.)
    if (lib_ && !(ws_ == Workspace::Library && view_ == View::Settings) && ws_ != Workspace::Worldbuilding &&
        ws_ != Workspace::Planning && ws_ != Workspace::Library && ws_ == Workspace::Writing) {   // only Writing has them now (0.71)
        auto layoutButton = [&](const std::string& bid, float bx, cjui::Icon icon, bool on, const wchar_t* tip, std::function<void()> act) {
            const Rect b{bx, (r.h - 30) / 2, 30, 30};
            addHit(bid, b, std::move(act), false);
            tooltip(bid, tip);
            const float hv = hoverAmount(bid);
            if (hv > 0.01f) gfx_.fillRounded(b, 7, nr::kNightInk.withAlpha(0.08f * hv));
            cjui::drawIcon(gfx_, icon, {b.x + 7, b.y + 7, 16, 16}, nr::kNightMuted.mix(nr::kNightInk, std::max(hv, on ? 0.6f : 0.0f)), 1.4f);
        };
        const float bx = r.right() - capW - 80;
        layoutButton("tab:side", bx, cjui::Icon::Sidebar, settings_.showSide, L"Show or hide the left side   F3",
                     [this] { settings_.showSide = !settings_.showSide; saveSettings(); invalidate(); });
        const bool writing = ws_ == Workspace::Writing;   // there, the button shows the chapter's details (0.70)
        layoutButton("tab:panel", bx + 34, cjui::Icon::Panel, writing ? settings_.writingDetails : settings_.showPanel, L"Show or hide the details   F4",
                     [this, writing] {
                         if (writing) settings_.writingDetails = !settings_.writingDetails;
                         else settings_.showPanel = !settings_.showPanel;
                         saveSettings();
                         invalidate();
                     });
    }    drawSearchResults();   // over everything below the bar
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
        sideRowFrame("side#sel", rr, selected, hv, sideScroll_);
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
        y += sideSection(x, y, wdt, label, std::move(add), addId);
    };

    row("side:worlds", 0, L"Worlds", std::to_wstring(lib_->liveWorlds().size()), view_ == View::Worlds, [this] { goTo(View::Worlds); });
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

// One sidebar look for every workspace (UI pass, 0.64): rows 30 tall with a rounded highlight; the chosen one on the
// surface with the accent bar at its edge, sliding between rows (`key` keeps each sidebar's own slide).
void StudioApp::sideRowFrame(const std::string& key, const Rect& rr, bool selected, float hover, float scroll) {
    if (selected) {
        const float sy = key.empty() ? rr.y : slideTo(key, rr.y + scroll) - scroll;
        const Rect hr{rr.x, sy, rr.w, rr.h};
        gfx_.fillRounded(hr, 8, pal_.surface);
        gfx_.strokeRounded(hr, 8, pal_.rule);
        gfx_.fillRounded({hr.x + 1, hr.y + 7, 3, hr.h - 14}, 1.5f, pal_.ribbon);
    } else if (hover > 0.01f) {
        gfx_.fillRounded(rr, 8, pal_.surface.withAlpha(0.7f * hover));
    }
}

// A sidebar's section heading in small capitals, with a + for adding to it when there's one. The height used.
float StudioApp::sideSection(float x, float y, float w, const std::wstring& label, std::function<void()> add, const std::string& addId) {
    y += 10;
    gfx_.text(label, {x + 12, y, w - 50, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
    if (add) {
        const Rect plus{x + w - 26, y - 4, 24, 24};
        addHit(addId, plus, std::move(add));
        tooltip(addId, L"Add");
        const float hv = hoverAmount(addId);
        if (hv > 0.01f) gfx_.fillRounded(plus, 6, pal_.surface.withAlpha(hv));
        cjui::drawIcon(gfx_, cjui::Icon::Plus, plus.inset(5), pal_.inkFaint.mix(pal_.ink, hv), 1.5f);
        if (focused(addId)) focusRing(plus);
    }
    return 32;
}

// -------------------------------------------------------------- status bar

// There's no status bar. Messages rise as a toast over the page; "Saving..." and
// "Not saved" show as a small note at the page's corner (the editor shows its own);
// today's words sit at the foot of the outline while writing (drawBinder).
void StudioApp::drawStatusBar(const Rect&) {
    if (lib_ && !(ws_ == Workspace::Writing && editor_.ready() && !currentNode_.empty())) {
        const bool saving = dirty_ || editorDirty_;
        const float a = anim("save#note", saving || saveFailed_ ? 1.0f : 0.0f, 10.0f);
        if (a > 0.01f) {
            const std::wstring st = saveFailed_ ? L"Not saved" : L"Saving...";
            const float tw = gfx_.measureWidth(st, Font::Small) + 34;
            const Rect r{pageRect_.right() - tw - 14, pageRect_.bottom() - 38, tw, 26};
            gfx_.fillRounded(r, 13, pal_.surface.withAlpha(0.95f * a));
            gfx_.strokeRounded(r, 13, pal_.rule.withAlpha(a));
            gfx_.circle(r.x + 14, r.y + r.h / 2, 3, (saveFailed_ ? hexColor("#B4442C") : hexColor("#C9922E")).withAlpha(a), true);
            gfx_.text(st, {r.x + 22, r.y, r.w - 26, r.h}, Font::Small, (saveFailed_ ? hexColor("#B4442C") : pal_.inkMuted).withAlpha(a),
                      {Align::Left, false, false, true});
        }
    }
    drawToast(pageRect_);
}

// Today's words (and the daily goal), for the foot of the outline while writing.
std::int64_t StudioApp::wordsToday() const {
    if (!lib_) return 0;
    const std::string today = Library::localDay(now());
    const auto days = lib_->wordsWritten({}, today);
    return days.count(today) ? days.at(today) : 0;
}

// The keyboard shortcuts, for Settings and the Ctrl+/ sheet (one list, so they never disagree).
std::vector<ShortcutGroup> StudioApp::shortcutGroups() {
    return {
        {L"Anywhere", {{L"Ctrl+K", L"Search: chapters, scenes, the bible, books, worlds and commands"}, {L"Ctrl+1 to Ctrl+8", L"The workspaces on the rail, top to bottom"}, {L"Ctrl+/", L"These shortcuts"}}},
        {L"Library", {{L"Ctrl+N", L"New book"}, {L"Ctrl+Shift+N", L"New series"}, {L"Ctrl+Shift+W", L"New world"},
                      {L"Ctrl+Shift+I", L"Import a book"}, {L"F2", L"Rename"}, {L"Del", L"Move to Trash"},
                      {L"Ctrl+Z", L"Undo Move to Trash"}, {L"Ctrl+Shift+1 / Ctrl+Shift+2", L"Bookshelf / list"}, {L"F3", L"Show or hide the outline (Writing)"},
                      {L"F4", L"Show or hide the chapter's details (Writing)"}, {L"Ctrl+O", L"Open or create a library"}, {L"Ctrl+E", L"Export the library"},
                      {L"Ctrl+,", L"Settings"}}},
        {L"Writing", {{L"Ctrl+W", L"Open the current book in Writing (or double-click it)"},
                      {L"Ctrl+Up / Ctrl+Down", L"Previous / next chapter"}, {L"Up / Down", L"Move through the outline"},
                      {L"Alt+Up / Alt+Down", L"Move a chapter or scene up or down"}, {L"Enter", L"From the outline or a title, go to the text"}}},
        {L"Typesetting", {{L"Left / Right, Page Up / Page Down", L"Previous / next pages"}, {L"Home / End", L"First / last pages"}}},
        {L"Text", {{L"Ctrl+B / Ctrl+I / Ctrl+U", L"Bold / italic / underline"}, {L"Ctrl+Shift+K", L"Small capitals"},
                   {L"Ctrl+Alt+K", L"Link"}, {L"Ctrl+Alt+M", L"Comment on the selected words"}, {L"Ctrl+Shift+F", L"Focus mode (the text alone)"},
                   {L"Ctrl+Shift+O", L"Other words for the selection"}, {L"Ctrl+Shift+T", L"Typewriter scrolling"},
                   {L"Ctrl+Enter", L"Scene break (a *** divider; + Scene adds one at the end)"}, {L"Shift+Enter", L"Line break"},
                   {L"Ctrl+Alt+0", L"Body text"}, {L"Ctrl+Alt+1 / 2 / 3", L"Headings"}, {L"Ctrl+Z / Ctrl+Y", L"Undo / redo"}}},
    };
}

std::wstring StudioApp::todayLine() const {
    if (!lib_) return {};
    const std::int64_t n = wordsToday();
    const std::int64_t goal = dailyGoal();
    std::wstring t = L"Today: " + std::to_wstring(n) + (goal ? L" of " + std::to_wstring(goal) : std::wstring()) + L" words";
    if (goal && n >= goal) t += L"  ·  goal reached";
    return t;
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
        if (m && !m->number.empty()) out += L" \u00B7 Book " + w(m->number);
    }
    return out;
}

// Totals under a world's or series' title (0.44): books, words, and when one was last worked on. Returns the height.
float StudioApp::booksTotals(float x, float y, const std::vector<const Book*>& books, float maxW) {
    if (books.empty()) return 0;
    std::int64_t words = 0;
    Time last = 0;
    for (const Book* b : books) { words += b->wordCount; last = std::max(last, b->modified); }
    std::wstring t = std::to_wstring(books.size()) + (books.size() == 1 ? L" book" : L" books") + L"  \u00B7  " + wordsText(words);
    if (last) t += L"  \u00B7  changed " + cjui::formatRelative((cj::UtcSeconds)last, cjui::nowUtc());
    gfx_.text(t, {x, y, maxW, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
    return 26;
}

float StudioApp::pageHeader(const Rect& r, const std::wstring& kicker, const std::wstring& title, const std::wstring& sub) {
    float y = r.y;
    if (!kicker.empty()) {
        gfx_.text(kicker, {r.x, y, r.w, 16}, Font::Label, gold_, {Align::Left, false, true});
        y += 20;
    }
    y += gfx_.text(title, {r.x, y, r.w, 80}, Font::SerifTitle, pal_.ink) + 4;
    if (!sub.empty()) y += gfx_.text(sub, {r.x, y, std::min(r.w, 700.0f), 200}, Font::Body, pal_.inkMuted) + 4;
    return y - r.y + 16;
}

// ------------------------------------------------------- the second redesign (0.68)

// The head of a book's workspace (0.71): a gold kicker, the book's title, a tools menu at the right, its parts as tabs,
// and a line about the part. Its height.
float StudioApp::workspaceHeader(const Rect& m, const std::wstring& kicker, const std::wstring& title, const std::string& tabsId,
                                 const std::vector<PageTab>& tabs, const std::wstring& toolsLabel,
                                 std::function<std::vector<ToolItem>()> tools, const std::wstring& sub) {
    float y = m.y;
    gfx_.text(kicker, {m.x, y, m.w, 16}, Font::Label, gold_, {Align::Left, false, true});
    float tbw = 0;
    if (tools) {
        tbw = gfx_.measureWidth(toolsLabel, Font::Body) + 44;
        menuButton(tabsId + ":tools", m.right() - tbw, y + 10, toolsLabel, std::move(tools));
    }
    y += 20;
    y += gfx_.text(title, {m.x, y, std::max(160.0f, m.w - tbw - 20), 90}, Font::SerifTitle, pal_.ink) + 8;
    if (!tabs.empty()) y += pageTabs(tabsId, m.x, y, m.right(), tabs) + 14;
    if (!sub.empty()) y += gfx_.text(sub, {m.x, y, std::min(m.w, 760.0f), 60}, Font::Small, pal_.inkMuted) + 14;
    return y - m.y + 8;
}

// The head over a viewer (Typesetting's pages, Preview's device), 0.71: kicker, title and a note on the left. The
// caller adds its buttons from the right.
void StudioApp::viewerHead(const Rect& r, const std::wstring& kicker, const std::wstring& title, const std::wstring& note) {
    gfx_.fill(r, pal_.paper);
    gfx_.line(r.x, r.bottom() - 0.5f, r.right(), r.bottom() - 0.5f, pal_.rule);
    const float x = r.x + (r.w > 900 ? 28 : 18);
    gfx_.text(kicker, {x, r.y + 10, 400, 14}, Font::Label, gold_, {Align::Left, false, true});
    const float tw = std::min(gfx_.measureWidth(title, Font::Heading) + 4, r.w * 0.4f);
    gfx_.text(title, {x, r.y + 26, tw, 26}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
    if (!note.empty()) gfx_.text(note, {x + tw + 16, r.y + 26, r.w * 0.3f, 26}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
}

// A button that's on or off (Design, Reader's settings), 0.71. Its width.
static float toggleWidth(cjui::Gfx& g, const std::wstring& label) { return g.measureWidth(label, cjui::Font::Body) + 28; }

// Review's head (0.71): Changes, Snapshots, Names, Comments and Goals as tabs, and Review tools.
float StudioApp::toggleButton(const std::string& id, float right, float y, const std::wstring& label, bool on, std::function<void()> flip,
                              const std::wstring& tip) {
    const float bw = toggleWidth(gfx_, label);
    const Rect b{right - bw, y, bw, 32};
    addHit(id, b, std::move(flip));
    if (!tip.empty()) tooltip(id, tip);
    const float hv = hoverAmount(id);
    gfx_.fillRounded(b, 8, on ? pal_.ribbon.withAlpha(pal_.dark ? 0.22f : 0.14f) : pal_.surface.mix(pal_.sunken, 0.35f * hv));
    gfx_.strokeRounded(b, 8, on ? pal_.ribbon.withAlpha(0.6f) : pal_.rule.mix(pal_.inkFaint, 0.5f * hv));
    gfx_.text(label, b, Font::Body, on ? pal_.ink : pal_.inkMuted.mix(pal_.ink, hv), {Align::Center, false, false, true});
    if (focused(id)) focusRing(b);
    return bw;
}

float StudioApp::reviewHeader(const Rect& m, const std::wstring& sub) {
    const studio::Book* b = lib_->book(rv_.book);
    std::vector<PageTab> tabs;
    static const wchar_t* names[] = {L"Changes", L"Snapshots", L"Names", L"Goals", L"Comments"};
    for (int i : {0, 1, 2, 4, 3}) {
        PageTab t;
        t.label = names[i];
        t.on = rv_.section == i;
        t.act = [this, i] { showReviewSection(i); };
        tabs.push_back(t);
    }
    return workspaceHeader(m, L"REVIEW", b ? w(b->title) : std::wstring(), "rv:tabs", tabs, L"Review tools", [this] {
        std::vector<ToolItem> t;
        t.push_back(ToolItem::item(L"Make a review copy…", [this] { makeReviewCopy(); }));
        t.push_back(ToolItem::item(L"Import a reader's comments…", [this] { importReaderComments(); }));
        t.push_back(ToolItem::line());
        t.push_back(ToolItem::item(L"Write this book", [this] { openWriting(rv_.book); }));
        t.push_back(ToolItem::item(L"The book's page", [this] { ws_ = Workspace::Library; goTo(View::Book, rv_.book); }));
        return t;
    }, sub);
}

// The Library's places (0.69): what its sidebar listed, as tabs, with Library tools at the end of the row. Its height.
float StudioApp::libraryTabs(const Rect& page) {
    const float y = page.y - 8;
    std::vector<PageTab> tabs;
    auto add = [&](const wchar_t* l, int n, bool on, View v) {
        PageTab t;
        t.label = l;
        t.count = n;
        t.on = on;
        t.act = [this, v] { goTo(v); };
        tabs.push_back(t);
    };
    bool indieSeries = false;
    if (view_ == View::Series) if (const Series* s = lib_->seriesById(viewId_)) indieSeries = s->worldId.empty();
    size_t indie = lib_->standaloneBooksIn({}).size();
    for (auto* s : lib_->seriesIn({})) indie += lib_->booksInSeries(s->id).size();
    const auto tc = lib_->trashCounts();
    int inTrash = tc.worlds + tc.series + tc.books;
    for (auto& a : lib_->artworks()) if (a.deleted) ++inTrash;
    add(L"Your worlds", (int)lib_->liveWorlds().size(), view_ == View::Worlds || view_ == View::World || (view_ == View::Series && !indieSeries), View::Worlds);
    add(L"All books", (int)lib_->liveBooks().size(), view_ == View::AllBooks, View::AllBooks);
    add(L"Independent books", (int)indie, view_ == View::Independent || indieSeries, View::Independent);
    add(L"Artwork", (int)lib_->liveArtworks().size(), view_ == View::Artwork, View::Artwork);
    add(L"Trash", inTrash, view_ == View::Trash, View::Trash);
    const float tbw = gfx_.measureWidth(L"Library tools", Font::Body) + 44;
    pageTabs("lib:tabs", page.x, y, page.right() - tbw - 16, tabs);
    menuButton("lib:tools", page.right() - tbw, y + 4, L"Library tools", [this] {
        std::vector<ToolItem> t;
        const Id world = view_ == View::World ? viewId_ : Id{};
        t.push_back(ToolItem::item(L"New world", [this] { newWorld(); }));
        t.push_back(ToolItem::item(world.empty() ? L"New book" : L"New book in this world", [this, world] { newBook(world, {}); }));
        if (!world.empty()) t.push_back(ToolItem::item(L"New series in this world", [this, world] { newSeries(world); }));
        t.push_back(ToolItem::item(L"Import a book…", [this] { startImport({}); }));
        t.push_back(ToolItem::item(L"Open a world from GitHub…", [this] { goTo(View::FromGitHub); }));
        t.push_back(ToolItem::line());
        t.push_back(ToolItem::heading(L"The library"));
        t.push_back(ToolItem::item(L"Back up now", [this] { backupNow(); }));
        t.push_back(ToolItem::item(L"Export the library…", [this] { exportLibrary(); }));
        t.push_back(ToolItem::item(L"Open or create another library…", [this] { chooseLibraryFolder(); }));
        return t;
    });
    return 40 + 26;
}

// A card on the paper: the surface, lifted by a soft shadow.
void StudioApp::card(const Rect& r, float radius, float lift) {
    softShadow(r, radius, 1.6f * lift);
    gfx_.fillRounded(r, radius, pal_.surface);
    if (pal_.dark) gfx_.strokeRounded(r, radius, pal_.rule);
}

float StudioApp::pageTabs(const std::string& id, float x, float y, float right, const std::vector<PageTab>& tabs, const std::vector<PageTab>& more) {
    const float h = 40;
    gfx_.line(x, y + h - 0.5f, right, y + h - 0.5f, pal_.rule);
    auto widthOf = [&](const PageTab& t) {
        float tw = gfx_.measureWidth(t.label, t.on ? Font::BodyStrong : Font::Body) + 24;
        if (t.count >= 0) tw += gfx_.measureWidth(std::to_wstring(t.count), Font::Small) + 7;
        if (t.hasDot) tw += 14;
        return tw;
    };
    // What fits; the rest (and those given as "more") under More.
    std::vector<const PageTab*> shown, rest;
    const float moreW = gfx_.measureWidth(L"More", Font::Body) + 40;
    float used = 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
        const float tw = widthOf(tabs[i]);
        const bool last = i + 1 == tabs.size() && more.empty();
        if (x + used + tw > right - (last ? 0 : moreW) && !shown.empty() && rest.empty()) rest.push_back(&tabs[i]);
        else if (!rest.empty()) rest.push_back(&tabs[i]);
        else { shown.push_back(&tabs[i]); used += tw; }
    }
    // The chosen tab always shows: if it fell under More, it takes the last place.
    for (size_t i = 0; i < rest.size(); ++i)
        if (rest[i]->on && !shown.empty()) { rest.insert(rest.begin(), shown.back()); shown.back() = rest[i + 1]; rest.erase(rest.begin() + (long)i + 1); break; }
    for (const PageTab& t : more) rest.push_back(&t);
    float tx = x, selX = -1, selW = 0;
    for (size_t i = 0; i < shown.size(); ++i) {
        const PageTab& t = *shown[i];
        const float tw = widthOf(t);
        const Rect b{tx, y, tw, h};
        const std::string tid = id + ":" + std::to_string(i);
        addHit(tid, b, t.act);
        const float hv = hoverAmount(tid);
        if (!t.on && hv > 0.01f) gfx_.fillRounded({b.x + 2, b.y + 5, b.w - 4, b.h - 10}, 7, pal_.ink.withAlpha(0.05f * hv));
        float lx = b.x + 12;
        if (t.hasDot) { gfx_.circle(lx + 3.5f, b.y + h / 2, 3.5f, t.dot, true); lx += 14; }
        const Font f = t.on ? Font::BodyStrong : Font::Body;
        const float lw = gfx_.measureWidth(t.label, f);
        gfx_.text(t.label, {lx, b.y, lw + 2, h}, f, t.on ? pal_.ink : pal_.inkMuted.mix(pal_.ink, hv), {Align::Left, false, false, true});
        if (t.count >= 0)
            gfx_.text(std::to_wstring(t.count), {lx + lw + 7, b.y + 1, 60, h}, Font::Small, pal_.inkFaint, {Align::Left, false, false, true});
        if (t.on) { selX = b.x; selW = b.w; }
        if (focused(tid)) focusRing(b.inset(2, 4));
        tx += tw;
    }
    if (!rest.empty()) {
        bool restOn = false;
        for (const PageTab* t : rest) restOn = restOn || t->on;
        const Rect b{tx, y, moreW, h};
        const std::string mid = id + ":more";
        std::vector<PageTab> copy;
        for (const PageTab* t : rest) copy.push_back(*t);
        addHit(mid, b, [this, copy, b] {
            std::vector<ToolItem> items;
            for (const PageTab& t : copy)
                items.push_back(ToolItem::item(t.count > 0 ? t.label + L"  (" + std::to_wstring(t.count) + L")" : t.label, t.act, t.on));
            showTools(items, b.x, b.bottom());
        });
        tooltip(mid, L"The rest");
        const float hv = hoverAmount(mid);
        if (hv > 0.01f) gfx_.fillRounded({b.x + 2, b.y + 5, b.w - 4, b.h - 10}, 7, pal_.ink.withAlpha(0.05f * hv));
        const Font f = restOn ? Font::BodyStrong : Font::Body;
        gfx_.text(L"More", {b.x + 12, b.y, b.w, h}, f, restOn ? pal_.ink : pal_.inkMuted, {Align::Left, false, false, true});
        const float cx = b.x + 12 + gfx_.measureWidth(L"More", f) + 10, cy = b.y + h / 2;
        gfx_.line(cx - 3.5f, cy - 1.5f, cx, cy + 2, pal_.inkMuted, 1.4f);
        gfx_.line(cx, cy + 2, cx + 3.5f, cy - 1.5f, pal_.inkMuted, 1.4f);
        if (restOn) { selX = b.x; selW = b.w; }
        if (focused(mid)) focusRing(b.inset(2, 4));
    }
    if (selX >= 0) {
        const float ax = slideTo(id + "#x", selX), aw = slideTo(id + "#w", selW);
        gfx_.fillRounded({ax + 8, y + h - 2.5f, aw - 16, 2.5f}, 1.25f, pal_.ribbon);
    }
    return h;
}

void StudioApp::showTools(const std::vector<ToolItem>& items, float x, float y, bool right) {
    std::vector<std::function<void()>> acts;
    std::function<std::vector<cjui::MenuItem>(const std::vector<ToolItem>&)> build = [&](const std::vector<ToolItem>& in) {
        std::vector<cjui::MenuItem> out;
        for (const ToolItem& t : in) {
            if (t.kind == 1) { out.push_back(cjui::MenuItem::sep()); continue; }
            if (t.kind == 2) {
                if (!out.empty() && out.back().separator && out.back().label.empty()) out.pop_back();   // a heading has its own line
                out.push_back(cjui::MenuItem::heading(t.label));
                continue;
            }
            if (!t.sub.empty()) { out.push_back(cjui::MenuItem::sub(t.label, build(t.sub))); continue; }
            acts.push_back(t.act);
            out.push_back(cjui::MenuItem::item(t.label, (UINT)acts.size(), {}, t.enabled, t.checked));
        }
        return out;
    };
    const auto menu = build(items);
    const float s = gfx_.dpiScale();
    POINT pt{(LONG)std::lround(x * s), (LONG)std::lround(y * s)};
    ClientToScreen(hwnd(), &pt);
    const UINT c = cjui::showMenu(hwnd(), menu, pt, pal_, right ? cjui::MenuAnchor::TopRight : cjui::MenuAnchor::TopLeft);
    if (c >= 1 && c <= acts.size() && acts[c - 1]) {
        commitFields();
        acts[c - 1]();
    }
    invalidate();
}

float StudioApp::menuButton(const std::string& id, float x, float y, const std::wstring& label, std::function<std::vector<ToolItem>()> items,
                            bool primary) {
    const float bw = gfx_.measureWidth(label, Font::Body) + 26 + 18;
    const Rect b{x, y, bw, 32};
    // A button on the right half of the page opens its menu leftwards, so it stays in the window.
    const bool rightSide = b.x > pageRect_.x + pageRect_.w / 2;
    addHit(id, b, [this, items, b, rightSide] { showTools(items(), rightSide ? b.right() : b.x, b.bottom() + 4, rightSide); });
    const float hv = hoverAmount(id), dn = pressed(id) ? 1.0f : 0.0f;
    const Rect f{b.x, b.y - 1.5f * hv + dn, b.w, b.h};
    if (hv > 0.01f) softShadow(f, 8, 0.6f * hv);
    const Color bg = primary ? pal_.ribbon : pal_.surface.mix(pal_.sunken, 0.35f * hv);
    const Color ink = primary ? (pal_.dark ? pal_.paper : cjui::hexColor("#FFFFFF")) : pal_.ink;
    gfx_.fillRounded(f, 8, bg);
    if (!primary) gfx_.strokeRounded(f, 8, pal_.rule.mix(pal_.inkFaint, 0.55f * hv));
    gfx_.text(label, {f.x + 13, f.y, bw, f.h}, primary ? Font::BodyStrong : Font::Body, ink, {Align::Left, false, false, true});
    const float cx = f.right() - 16, cy = f.y + f.h / 2;
    gfx_.line(cx - 3.5f, cy - 1.5f, cx, cy + 2, ink, 1.5f);
    gfx_.line(cx, cy + 2, cx + 3.5f, cy - 1.5f, ink, 1.5f);
    if (focused(id)) focusRing(f);
    return bw;
}

void StudioApp::startInline(const std::string& key, int field, std::function<void()> revert, bool big) {
    if (ie_.field >= 0) endInline(true);
    commitFields();
    ie_ = {};
    ie_.field = field;
    ie_.key = key;
    ie_.big = big;
    ie_.revert = std::move(revert);
    invalidate();
}

float StudioApp::inlineHeight(float minH) const {
    if (ie_.field < 0) return minH;
    if (!isMultiField(ie_.field)) return ie_.big ? 50.0f : 36.0f;
    const int lines = std::max(1, (int)SendMessageW(fields_[ie_.field], EM_GETLINECOUNT, 0, 0));
    return std::max(minH, lines * 20.0f + 22);
}

void StudioApp::inlineBox(const Rect& r) {
    if (ie_.field < 0) return;
    gfx_.fillRounded(r.inset(-4), 10, pal_.ribbon.withAlpha(0.12f));
    gfx_.fillRounded(r, 8, pal_.surface);
    gfx_.strokeRounded(r, 8, pal_.ribbon, 1.5f);
    const Field f = (Field)ie_.field;
    if (ie_.big && !isMultiField(f)) {
        // The name in the title's size: its own font, and the box sized for it.
        const float s = gfx_.dpiScale();
        const RECT rc{(LONG)std::lround((r.x + 10) * s), (LONG)std::lround((r.y + (r.h - 36) / 2) * s), (LONG)std::lround((r.right() - 8) * s),
                      (LONG)std::lround((r.y + (r.h + 36) / 2) * s)};
        placed_[f] = true;
        if (titleFont_ && (HFONT)SendMessageW(fields_[f], WM_GETFONT, 0, 0) != titleFont_)
            SendMessageW(fields_[f], WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
        if (memcmp(&rc, &fieldRects_[f], sizeof rc) != 0 || !IsWindowVisible(fields_[f])) {
            fieldRects_[f] = rc;
            SetWindowPos(fields_[f], nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    } else {
        placeField(f, r);
    }
    ie_.placed = true;
    if (ie_.key.rfind("wr:", 0) == 0) return;   // the chapter's bar: no room for the hint under it
    gfx_.text(isMultiField(ie_.field) ? L"Ctrl+Enter or a click elsewhere keeps it; Esc puts it back." : L"Enter or a click elsewhere keeps it; Esc puts it back.",
              {r.x + 2, r.bottom() + 4, r.w, 16}, Font::Small, pal_.inkFaint,
              {Align::Left, false, true});
}

void StudioApp::editableArea(const std::string& id, const Rect& r, const std::wstring& tip, std::function<void()> start) {
    addHit(id, r, std::move(start));
    tooltip(id, tip);
    const float hv = hoverAmount(id);
    if (hv > 0.01f) {
        gfx_.fillRounded(r.inset(-6, -3), 8, pal_.surface.withAlpha(0.55f * hv));
        gfx_.strokeRounded(r.inset(-6, -3), 8, pal_.rule.mix(pal_.inkFaint, 0.4f).withAlpha(hv), 1.0f);
    }
    if (focused(id)) focusRing(r.inset(-6, -3));
}

bool StudioApp::smallButton(const std::string& id, float x, float y, const std::wstring& label, std::function<void()> act, float* outW) {
    // As wide as the words, with room for the icon its words give it (the calm controls, 0.64).
    std::wstring text = label;
    if (text.rfind(L"+ ", 0) == 0) text.erase(0, 2);
    const float bw = gfx_.measureWidth(text, Font::Body) + 26 + (cjui::iconFor(label) == cjui::Icon::None ? 0 : 21);
    if (outW) *outW = bw;
    return button(id, {x, y, bw, 30}, label, std::move(act));
}

// A page's actions as one row of the same buttons, wrapping onto another row when the page is narrow (0.64). The height.
float StudioApp::actionRow(float x, float y, float maxW, const std::vector<Action>& actions) {
    float bx = x, by = y;
    for (const Action& a : actions) {
        std::wstring text = a.label;
        if (text.rfind(L"+ ", 0) == 0) text.erase(0, 2);
        const float bw = gfx_.measureWidth(text, Font::Body) + 26 + (cjui::iconFor(a.label) == cjui::Icon::None ? 0 : 21);
        if (bx > x && bx + bw > x + maxW) bx = x, by += 38;
        smallButton(a.id, bx, by, a.label, a.act, nullptr);
        bx += bw + 8;
    }
    return by - y + 30;
}

// Bookshelf / List at the right of a row of controls, or on its own row below when the row is full (0.66). The extra height.
float StudioApp::viewSwitch(const std::string& id, float left, float x, float y, float right) {
    const float sw = 180;
    float extra = 0, sx = right - sw;
    if (sx < x + 8) { sx = left; extra = 38; }
    segmented(id, {sx, y + extra, sw, 28}, {L"Bookshelf", L"List"}, settings_.gallery ? 0 : 1, [this](int i) {
        settings_.gallery = i == 0;
        saveSettings();
    });
    return extra;
}

// Search box, sort, filter and view toggle above a collection.
float StudioApp::toolbar(const Rect& r, bool withSearch) {
    float x = r.x;
    if (withSearch) {
        // The same find box as the bible's (0.66): a drawn magnifier, rounded like the buttons.
        const Rect sr{x, r.y, std::min(300.0f, r.w * 0.4f), 30};
        gfx_.fillRounded(sr, 7, pal_.surface);
        gfx_.strokeRounded(sr, 7, GetFocus() == fields_[FSearch] ? pal_.ribbon : pal_.rule);
        cjui::drawIcon(gfx_, cjui::Icon::Search, {sr.x + 9, sr.y + (sr.h - 15) / 2, 15, 15}, pal_.inkFaint, 1.4f);
        placeField(FSearch, {sr.x + 28, sr.y, sr.w - 28, sr.h});
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
    return 46 + viewSwitch("tb:view", r.x, x, r.y + 1, r.right());
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

// A book's status as a small coloured tag (0.44): a dot and the name, tinted by how far along it is.
Color StudioApp::bookStatusColour(BookStatus s) const {
    static const char* light[] = {"#8A8F9C", "#6C63C9", "#2F7FD0", "#C27A1A", "#B4507A", "#1F9C6E", "#1C7A55", "#8A8F9C"};
    static const char* dark[] = {"#A6ABB8", "#A49CF0", "#6FA9F0", "#F0B252", "#E68AB0", "#52D1A0", "#5CC995", "#A6ABB8"};
    const int i = std::clamp((int)s, 0, 7);
    return hexColor((pal_.dark ? dark : light)[i]);
}

float StudioApp::statusTag(float x, float y, BookStatus s, float maxW) {
    const std::wstring t = w(label(s));
    const Color c = bookStatusColour(s);
    const float tw = std::min(gfx_.measureWidth(t, Font::Small) + 24, maxW);
    const Rect tag{x, y, tw, 20};
    gfx_.fillRounded(tag, 10, c.withAlpha(pal_.dark ? 0.18f : 0.12f));
    gfx_.circle(tag.x + 9, tag.y + 10, 3, c, true);
    gfx_.text(t, {tag.x + 16, tag.y, tag.w - 20, tag.h}, Font::Small, c.mix(pal_.ink, 0.35f), {Align::Left, false, true, true});
    return tw;
}

std::wstring StudioApp::wordsText(std::int64_t n) const {
    if (n <= 0) return L"No words yet";
    std::wstring d = std::to_wstring(n), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += L',';
        out += d[i];
    }
    return out + (n == 1 ? L" word" : L" words");
}

void StudioApp::bookCard(const Rect& r, const Book& b, const Id& seriesId, const std::string& hitId) {
    const bool sel = focus_.kind == Focus::Book && focus_.id == b.id;
    addHit(hitId, r, [this, id = b.id, seriesId] { goTo(View::Book, id); setFocus({Focus::Book, id, seriesId}); });   // its own page (0.69)
    // Covers rise a little on hover, with a soft shadow.
    const float lift = hoverAmount(hitId);
    const Rect cover{r.x, r.y - 3 * lift, r.w, r.w * 1.5f};
    if (drag_.active && drag_.id == b.id) gfx_.fill(cover, pal_.sunken);
    else { softShadow(cover, 2, 0.4f + lift); drawCover(cover, b); }
    if (sel) gfx_.stroke(cover.inset(-3), pal_.ribbon, 2);
    float y = cover.bottom() + 8;
    if (!seriesId.empty())
        if (const Membership* m = lib_->membership(seriesId, b.id)) {
            // The number, then the book's type unless it's a novel (0.73).
            const std::wstring type = b.type == "novel" ? std::wstring() : bookTypeText(b);
            const std::wstring k = (m->number.empty() ? std::wstring(L"") : L"Book " + w(m->number)) +
                                   (!type.empty() ? (m->number.empty() ? L"" : L" · ") + type : L"");
            if (!k.empty()) { gfx_.text(k, {r.x, y, r.w, 16}, Font::Label, pal_.inkFaint, {Align::Left, false, true}); y += 17; }
        }
    // Progress towards the book's target, as a thin bar under the cover.
    if (const std::int64_t target = bookTarget(b.id); target > 0) {
        const float f = std::clamp((float)b.wordCount / (float)target, 0.0f, 1.0f);
        gfx_.fillRounded({cover.x, cover.bottom() + 3, cover.w, 3}, 1.5f, pal_.sunken);
        if (f > 0) gfx_.fillRounded({cover.x, cover.bottom() + 3, std::max(3.0f, cover.w * f), 3}, 1.5f, f >= 1 ? pal_.ribbon : bookStatusColour(b.status));
    }
    gfx_.text(w(b.title), {r.x, y, r.w, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
    statusTag(r.x, y + 23, b.status, r.w);
    gfx_.text(wordsText(b.wordCount), {r.x, y + 46, r.w, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
    if (focused(hitId)) focusRing(cover);
}

void StudioApp::bookRow(const Rect& r, const Book& b, const Id& seriesId, const std::string& hitId, bool header) {
    const float c1 = r.w * 0.34f, c2 = r.w * 0.26f, c3 = r.w * 0.16f, c4 = r.w * 0.12f;
    if (header) {
        float x = r.x + 44;
        for (auto [t, wd] : std::initializer_list<std::pair<const wchar_t*, float>>{{L"TITLE", c1 - 44}, {L"SERIES", c2}, {L"STATUS", c3}, {L"WORDS", c4}, {L"CHANGED", 200}}) {
            gfx_.text(t, {x, r.y, wd, r.h}, Font::Label, pal_.inkFaint, {Align::Left, false, true, true});
            x += wd;
        }
        return;
    }
    const bool sel = focus_.kind == Focus::Book && focus_.id == b.id;
    addHit(hitId, r, [this, id = b.id, seriesId] { goTo(View::Book, id); setFocus({Focus::Book, id, seriesId}); });   // its own page (0.69)
    if (sel) gfx_.fill(r, pal_.surface);
    else if (float hv = hoverAmount(hitId); hv > 0.01f) gfx_.fill(r, pal_.surface.withAlpha(0.5f * hv));
    if (sel) gfx_.fill({r.x, r.y + 6, 3, r.h - 12}, pal_.ribbon);
    // In a series' list, sub-books sit indented under the book before them (0.73).
    const Membership* own = seriesId.empty() ? nullptr : lib_->membership(seriesId, b.id);
    const float in = own && own->role != SeriesRole::Main ? 24.0f : 0.0f;
    if (in > 0) {
        gfx_.line(r.x + 22, r.y, r.x + 22, r.y + r.h / 2, pal_.rule, 1.5f);
        gfx_.line(r.x + 22, r.y + r.h / 2, r.x + 30, r.y + r.h / 2, pal_.rule, 1.5f);
    }
    drawCover({r.x + 10 + in, r.y + 5, 26, 38}, b);
    float x = r.x + 44 + in;
    const std::wstring sub = !b.subtitle.empty() ? w(b.subtitle) : !b.penName.empty() ? w(b.penName) : w(b.author);
    // The title centred in the row, or over its subtitle when it has one.
    if (sub.empty()) {
        gfx_.text(w(b.title), {x, r.y, c1 - 50 - in, r.h}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
    } else {
        gfx_.text(w(b.title), {x, r.y + 5, c1 - 50 - in, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        gfx_.text(sub, {x, r.y + 24, c1 - 50 - in, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
    }
    x += c1 - 44 - in;
    // The series, with the book's number under it (so a long series name never hides it). In a series' own list,
    // that series, and where a sub-book is set.
    {
        auto ss = lib_->seriesOfBook(b.id);
        if (own)
            if (const Series* here = lib_->seriesById(seriesId)) {
                ss.erase(std::remove(ss.begin(), ss.end(), here), ss.end());
                ss.insert(ss.begin(), here);
            }
        const Membership* mb = ss.empty() ? nullptr : lib_->membership(ss.front()->id, b.id);
        if (ss.empty()) {
            gfx_.text(L"\u2014", {x, r.y, c2 - 10, r.h}, Font::Small, pal_.inkFaint, {Align::Left, false, true, true});
        } else if (mb && (!mb->number.empty() || mb->role != SeriesRole::Main)) {
            gfx_.text(w(ss.front()->name), {x, r.y + 6, c2 - 10, 18}, Font::Small, pal_.ink, {Align::Left, false, true});
            std::wstring line = mb->number.empty() ? std::wstring() : L"Book " + w(mb->number);
            if (mb->role != SeriesRole::Main) line += (line.empty() ? L"" : L" \u00B7 ") + seriesRoleText(*mb);
            else if (ss.size() > 1) line += L" \u00B7 and " + std::to_wstring(ss.size() - 1) + L" more";
            gfx_.text(line, {x, r.y + 24, c2 - 10, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
        } else {
            gfx_.text(w(ss.front()->name), {x, r.y, c2 - 10, r.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
        }
        x += c2;
    }
    statusTag(x, r.y + (r.h - 20) / 2, b.status, c3 - 10); x += c3;
    gfx_.text(b.wordCount > 0 ? wordsText(b.wordCount) : L"\u2014", {x, r.y, c4 - 10, r.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true}); x += c4;
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
        const float rowH = cw * 1.5f + 84;   // number, title, status, words
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
    if (!libraryNote_.empty()) {   // something went wrong opening it: say what, then the way on
        float y = r.y + pageHeader(r, L"", L"No library is open", libraryNote_);
        smallButton("empty:open", r.x, y, L"Open or create a library...", [this] { chooseLibraryFolder(); });
        return y + 40 - r.y;
    }
    return emptyState(r, cjui::Icon::Library, L"No library is open yet.", L"Open or create a library...", [this] { chooseLibraryFolder(); });
}

// A small Edit (or Remove) beside a section, shown while the pointer is over the section or the keyboard is on it (0.65):
// the page reads as writing, not as a row of buttons. It's always there to click and to Tab to.
void StudioApp::quietEdit(const std::string& id, const Rect& section, float x, float y, const std::wstring& label, std::function<void()> act) {
    const bool over = section.contains(mouseX(), mouseY()) || hovered(id) || focused(id);
    if (over) {
        linkText(id, x, y, label, std::move(act), Font::Small);
    } else {
        addHit(id, {x - 7, y - 4, gfx_.measureWidth(label, Font::Small) + 14, 24}, std::move(act));
    }
}

// An empty page or pane (UI pass, 0.64): a quiet icon, one line and one button, centred a little above the middle.
float StudioApp::emptyState(const Rect& area, cjui::Icon icon, const std::wstring& line, const std::wstring& label,
                            std::function<void()> act) {
    const float a = anim("empty#in:" + std::string(line.begin(), line.begin() + std::min<size_t>(line.size(), 12)), 1.0f, 9.0f);
    const float lineW = std::min(gfx_.measureWidth(line, Font::Body) + 4, std::max(200.0f, area.w - 40));
    const float lineH = gfx_.measureHeight(line, lineW, Font::Body);
    const float h = 40 + 18 + lineH + (label.empty() ? 0 : 20 + 30);
    const float avail = area.h > 0 ? area.h : 420;
    float y = area.y + std::max(24.0f, (avail - h) * 0.4f) + 10 * (1 - a);
    const float cx = area.x + area.w / 2;
    cjui::drawIcon(gfx_, icon, {cx - 20, y, 40, 40}, pal_.inkFaint.withAlpha(0.7f * a + 0.3f), 1.3f);
    y += 40 + 18;
    gfx_.text(line, {cx - lineW / 2, y, lineW, lineH + 4}, Font::Body, pal_.inkMuted, {Align::Center, true});
    y += lineH + 20;
    if (!label.empty()) {
        std::wstring text = label;
        if (text.rfind(L"+ ", 0) == 0) text.erase(0, 2);
        const float bw = gfx_.measureWidth(text, Font::Body) + 26 + (cjui::iconFor(label) == cjui::Icon::None ? 0 : 21);
        button("empty:act", {cx - bw / 2, y, bw, 30}, label, std::move(act));
        y += 30;
    }
    return y - area.y + 20;
}

float StudioApp::pageAllBooks(const Rect& r) {
    float y = r.y;
    const auto all = lib_->liveBooks();
    y += pageHeader(r, L"AUTHOR LIBRARY", L"All books",
                    all.empty() ? L"Your library is empty."
                                : plural(all.size(), L"book", L"books") + L" across " +
                                      plural(lib_->liveWorlds().size(), L"world", L"worlds") + L".");
    if (!libraryNote_.empty()) {
        const float h = gfx_.measureHeight(libraryNote_, std::min(r.w, 700.0f) - 28, Font::Small);
        gfx_.fillRounded({r.x, y, std::min(r.w, 700.0f), h + 20}, 3, pal_.sunken);
        gfx_.text(libraryNote_, {r.x + 14, y + 10, std::min(r.w, 700.0f) - 28, h + 4}, Font::Small, pal_.ink);
        y += h + 36;
    }
    if (all.empty()) {
        // First run (0.66): the other ways in as the page's usual row, then one line and one button.
        y += actionRow(r.x, y, r.w, {{"all:import", L"Import a book...", [this] { startImport({}); }},
                                     {"all:newworld", L"New world", [this] { newWorld(); }},
                                     {"all:github", L"Open a world from GitHub...", [this] { goTo(View::FromGitHub); }}}) + 14;
        y += emptyState({r.x, y, std::min(r.w, 700.0f), 360}, cjui::Icon::Book, L"Start with a book, or a world for a series set in one place.",
                        L"+ New book", [this] { newBook({}, {}); });
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

// The start page (0.38): the author's worlds as cards (banner or colour, name, description, what's in it, a few
// covers), then books outside any world as one more card. An empty library shows the first-run page.
float StudioApp::pageWorlds(const Rect& r) {
    const auto worlds = lib_->liveWorlds();
    if (worlds.empty() && lib_->liveBooks().empty()) return pageAllBooks(r);
    float y = r.y;
    size_t indie = lib_->standaloneBooksIn({}).size();
    for (auto* s : lib_->seriesIn({})) indie += lib_->booksInSeries(s->id).size();
    y += pageHeader(r, L"AUTHOR LIBRARY", L"Your worlds",
                    worlds.empty() ? L"No worlds yet. A world holds a series' books and their shared bible."
                                   : plural(worlds.size(), L"world", L"worlds") + L", " + plural(lib_->liveBooks().size(), L"book", L"books") + L" in all.");
    if (!libraryNote_.empty()) {
        const float h = gfx_.measureHeight(libraryNote_, std::min(r.w, 700.0f) - 28, Font::Small);
        gfx_.fillRounded({r.x, y, std::min(r.w, 700.0f), h + 20}, 3, pal_.sunken);
        gfx_.text(libraryNote_, {r.x + 14, y + 10, std::min(r.w, 700.0f) - 28, h + 4}, Font::Small, pal_.ink);
        y += h + 36;
    }
    // The page's actions in one row (All books is in the sidebar already).
    y += actionRow(r.x, y, r.w, {{"worlds:new", L"New world", [this] { newWorld(); }},
                                 {"worlds:book", L"New book", [this] { newBook({}, {}); }},
                                 {"worlds:import", L"Import a book\u2026", [this] { startImport({}); }},
                                 {"worlds:github", L"Open a world from GitHub\u2026", [this] { goTo(View::FromGitHub); }}}) + 18;
    const float gap = 20;
    const int cols = std::max(1, (int)((r.w + gap) / (420 + gap)));
    const float cw = (r.w - gap * (cols - 1)) / cols, ch = 326;
    int i = 0;
    auto cardAt = [&](int n) { return Rect{r.x + (n % cols) * (cw + gap), y + (n / cols) * (ch + gap), cw, ch}; };
    std::vector<Id> order;
    for (auto* wd : worlds) order.push_back(wd->id);
    for (auto* wd : worlds) {
        const Rect c = cardAt(i++);
        const std::string id = "worlds:card:" + wd->id;
        addHit(id, c, [this, wid = wd->id] { goTo(View::World, wid); });
        addDrop({Drop::ToWorld, c, wd->id, {}, false, false, {}});
        const float hv = hoverAmount(id);
        const Color accent = wd->accent.empty() ? hexColor("#2D4A6E") : hexColor(wd->accent, hexColor("#2D4A6E"));
        gfx_.fillRounded({c.x, c.y + 3 * hv, c.w, c.h}, 10, hexColor("#000000").withAlpha(0.06f + 0.06f * hv));
        gfx_.fillRounded(c, 10, pal_.surface);
        // The banner: its picture, or its colour with soft bands.
        const Rect ban{c.x, c.y, c.w, 128};
        gfx_.pushClip(c);
        if (wd->bannerBlob.empty() || !gfx_.drawImage(w(lib_->blobPath(wd->bannerBlob)), ban, true)) {
            gfx_.fillRounded({ban.x, ban.y, ban.w, ban.h + 12}, 10, accent);
            for (int k = 0; k < 5; ++k) gfx_.line(ban.x, ban.y + 24 + k * 22, ban.right(), ban.y + 4 + k * 22, hexColor("#FFFFFF").withAlpha(0.06f), 12);
        }
        gfx_.fill({ban.x, ban.bottom() - 46, ban.w, 46}, hexColor("#000000").withAlpha(0.32f));
        gfx_.popClip();
        float tx = c.x + 18;
        if (!wd->logoBlob.empty() && gfx_.drawImage(w(lib_->blobPath(wd->logoBlob)), {tx, ban.bottom() - 42, 36, 36}, false)) tx += 46;
        gfx_.text(w(wd->name), {tx, ban.bottom() - 42, c.right() - tx - 14, 36}, Font::SerifTitle, hexColor("#F4F1EA"), {Align::Left, false, true, true});
        // What's in it.
        const auto series = lib_->seriesIn(wd->id);
        std::vector<const Book*> books;
        for (auto* s : series) for (auto* b : lib_->booksInSeries(s->id)) if (std::find(books.begin(), books.end(), b) == books.end()) books.push_back(b);
        for (auto* b : lib_->standaloneBooksIn(wd->id)) if (std::find(books.begin(), books.end(), b) == books.end()) books.push_back(b);
        const size_t entries = lib_->entriesOf(wd->id).size();
        std::wstring meta = plural(books.size(), L"book", L"books");
        if (!series.empty()) meta = plural(series.size(), L"series", L"series") + L"  \u00B7  " + meta;
        if (entries) meta += L"  \u00B7  " + plural(entries, L"bible entry", L"bible entries");
        float iy = ban.bottom() + 12;
        gfx_.text(meta, {c.x + 18, iy, c.w - 36, 18}, Font::Small, accent.mix(pal_.ink, pal_.dark ? 0.55f : 0.25f), {Align::Left, false, true, true});
        iy += 24;
        // A few covers, then the description beside them.
        float cx = c.x + 18;
        const float covH = 108, covW = covH * 2 / 3;
        size_t shown = 0;
        for (auto* b : books) {
            if (shown == 3) break;
            drawCover({cx, iy, covW, covH}, *b);
            cx += covW + 8;
            ++shown;
        }
        if (books.size() > 3) {
            gfx_.text(L"+" + std::to_wstring(books.size() - 3), {cx - 2, iy + covH - 20, 40, 20}, Font::Small, pal_.inkFaint, {Align::Left, false});
            cx += 30;
        }
        const float dx = shown ? cx + 8 : c.x + 18, dw = c.right() - 18 - dx;
        if (!wd->description.empty() && dw > 80) {
            gfx_.pushClip({dx, iy, dw, covH});
            gfx_.text(w(wd->description), {dx, iy, dw, covH}, Font::Small, pal_.inkMuted);
            gfx_.popClip();
        } else if (books.empty() && dw > 80)
            gfx_.text(L"No books yet.", {dx, iy, dw, 20}, Font::Small, pal_.inkFaint);
        // The way into its bible.
        const float fy = c.bottom() - 40;
        gfx_.line(c.x + 14, fy, c.right() - 14, fy, pal_.rule);
        {
            const std::string bid = "worlds:bible:" + wd->id;
            const Rect br{c.x + 14, fy + 6, 150, 28};
            addHit(bid, br, [this, wid = wd->id] { openWorldbuilding(wid, {}); });
            gfx_.text(L"Worldbuilding \u203A", br, Font::SmallStrong, pal_.ribbon.mix(pal_.ink, 0.3f * hoverAmount(bid)), {Align::Left, false, true, true});
            if (focused(bid)) focusRing(br);
        }
        gfx_.text(L"Open the world \u203A", {c.right() - 180, fy + 6, 166, 28}, Font::Small, pal_.inkFaint, {Align::Right, false, true, true});
        gfx_.strokeRounded(c, 10, hv > 0.01f ? pal_.rule.mix(accent, hv) : pal_.rule, 1 + hv);
        if (focused(id)) focusRing(c);
    }
    (void)order;
    // Books outside any world.
    if (indie) {
        const Rect c = cardAt(i++);
        const std::string id = "worlds:indie";
        addHit(id, c, [this] { goTo(View::Independent); });
        addDrop({Drop::ToIndependent, c, {}, {}, false, false, {}});
        const float hv = hoverAmount(id);
        gfx_.fillRounded(c, 10, pal_.surface.mix(pal_.sunken, 0.5f));
        gfx_.strokeRounded(c, 10, pal_.rule.mix(pal_.ribbon, hv), 1 + hv);
        gfx_.text(L"OUTSIDE ANY WORLD", {c.x + 18, c.y + 18, c.w - 36, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        gfx_.text(L"Independent books", {c.x + 18, c.y + 38, c.w - 36, 34}, Font::SerifTitle, pal_.ink, {Align::Left, false, true, true});
        gfx_.text(plural(indie, L"book", L"books") + L": nonfiction, collections, anything set nowhere in particular.", {c.x + 18, c.y + 80, c.w - 36, 40},
                  Font::Small, pal_.inkMuted);
        float cx = c.x + 18;
        size_t shown = 0;
        std::vector<const Book*> books = lib_->standaloneBooksIn({});
        for (auto* s : lib_->seriesIn({})) for (auto* b : lib_->booksInSeries(s->id)) books.push_back(b);
        for (auto* b : books) {
            if (shown == 4 || cx + 72 > c.right() - 18) break;
            drawCover({cx, c.y + 130, 72, 108}, *b);
            cx += 80;
            ++shown;
        }
        if (focused(id)) focusRing(c);
    }
    const int rows = (i + cols - 1) / cols;
    y += rows * (ch + gap);
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
        y += gfx_.text(w(wd.description), {r.x, y, std::min(r.w, 720.0f), 400}, Font::Body, pal_.ink) + 10;
    {
        std::vector<const Book*> inWorld;
        for (const Book* b : lib_->liveBooks()) if (b->worldId == wd.id) inWorld.push_back(b);
        y += booksTotals(r.x, y, inWorld, r.w) + 6;
    }

    float bx = r.x, bw = 0;
    smallButton("world:newseries", bx, y, L"New series", [this] { newSeries(viewId_); }, &bw); bx += bw + 8;
    smallButton("world:newbook", bx, y, L"New standalone book", [this] { newBook(viewId_, {}); }, &bw); bx += bw + 8;
    y += viewSwitch("world:view", r.x, bx, y, r.right());
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
    y += booksTotals(r.x, y - 6, lib_->booksInSeries(s.id), r.w - coverW - 20);
    const auto books = lib_->booksInSeries(s.id, chrono_);
    float bx = r.x, bw = 0;
    smallButton("series:newbook", bx, y, L"New book in series", [this] { newBook({}, viewId_); }, &bw); bx += bw + 12;
    const float ow = std::max(260.0f, gfx_.measureWidth(L"Reading order", Font::SmallStrong) + gfx_.measureWidth(L"Chronological", Font::SmallStrong) + 64);
    if (bx + ow > r.right()) { bx = r.x; y += 38; }
    segmented("series:order", {bx, y, ow, 28}, {L"Reading order", L"Chronological"}, chrono_ ? 1 : 0, [this](int i) { chrono_ = i == 1; });
    bx += ow;
    y += viewSwitch("series:view", r.x, bx, y, r.right());
    y += 40;
    y += gfx_.text(chrono_ ? L"The story's own timeline. Drag books to change it; reading order stays."
                           : L"The order readers take. Drag books to change it, or use Alt+Up and Alt+Down.",
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
    float y = r.y + pageHeader(r, L"AUTHOR LIBRARY", L"Trash", L"Nothing here is lost until you empty the Trash.");
    const auto c = lib_->trashCounts();
    int pictures = 0;
    for (auto& a : lib_->artworks()) if (a.deleted) ++pictures;
    if (!c.worlds && !c.series && !c.books && !pictures && !c.entries) {
        y += emptyState({r.x, y, std::min(r.w, 700.0f), 320}, cjui::Icon::Trash, L"The Trash is empty.", L"", nullptr);
        return y - r.y;
    }
    smallButton("trash:empty", r.x, y, L"Empty Trash...", [this] { emptyTrash(); });
    y += 46;
    auto item = [&](const std::string& id, const std::wstring& kind, const std::wstring& name, Time when) {
        const Rect rr{r.x, y, r.w, 46};
        gfx_.text(kind, {rr.x, rr.y + 4, 90, 18}, Font::Label, pal_.inkFaint, {Align::Left, false});
        gfx_.text(name, {rr.x + 90, rr.y, rr.w - 240, 24}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        gfx_.text(L"Moved to the Trash " + cjui::formatRelative((cj::UtcSeconds)when, cjui::nowUtc()), {rr.x + 90, rr.y + 22, 300, 18},
                  Font::Small, pal_.inkMuted, {Align::Left, false});
        // Restore shows on the row you point at (0.66), so the page is a list rather than a column of buttons.
        const std::string bid = "trash:restore:" + id;
        const Rect br{rr.right() - 110, rr.y + 6, 110, 30};
        auto restore = [this, id] { lib_->restore(id); invalidate(); };
        if (rr.contains(mouseX(), mouseY()) || hovered(bid) || focused(bid)) button(bid, br, L"Restore", restore);
        else addHit(bid, br, restore);
        (void)name;
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

// A book's own page (0.69): its cover, title and progress with the ways in, then tabs for its overview, its settings
// (what the details panel held), its editions, bringing things in and sending them out, and its history.
float StudioApp::pageBook(const Rect& r) {
    const Book& b = *lib_->book(viewId_);
    if (!(focus_.kind == Focus::Book && focus_.id == b.id)) setFocus({Focus::Book, b.id, {}});
    if (!(focus_ == loadedFocus_)) loadFields();
    float y = r.y;
    const World* wd = b.worldId.empty() ? nullptr : lib_->world(b.worldId);
    // A warm band behind the top of the page, fading into it.
    gfx_.fillGradient({pageRect_.x, y - 30, pageRect_.w, std::min(150.0f, r.w * 0.22f) * 1.5f + 110}, 0,
                      pal_.paper.mix(gold_, pal_.dark ? 0.10f : 0.13f), pal_.paper, true);
    linkText("book:crumb", r.x + 7, y, L"‹ " + (wd ? w(wd->name) : std::wstring(L"Independent books")), [this, wd] {
        if (wd) goTo(View::World, wd->id); else goTo(View::Independent);
    });
    y += 36;
    // The band: cover, what it is, how far it's got, and the ways in.
    const float cw = std::min(150.0f, r.w * 0.22f);
    {
        const Rect cr{r.x, y, cw, cw * 1.5f};
        softShadow(cr, 6, 2.5f);
        drawCover(cr, b);
        const float x = r.x + cw + 30, tw = r.w - cw - 30;
        float ty = y + 2;
        std::wstring kick = upper(seriesLine(b));
        if (kick.empty()) kick = wd ? L"A BOOK OF " + upper(w(wd->name)) : L"AN INDEPENDENT BOOK";
        if (const std::wstring t = bookTypeText(b); !t.empty()) kick = upper(t) + L"  \u00B7  " + kick;   // 0.73
        gfx_.text(kick, {x, ty, tw, 16}, Font::Label, gold_, {Align::Left, false, true});
        ty += 20;
        ty += gfx_.text(w(b.title), {x, ty, tw, 90}, Font::SerifTitle, pal_.ink) + 2;
        if (!b.subtitle.empty()) ty += gfx_.text(w(b.subtitle), {x, ty, tw, 40}, Font::Heading, pal_.inkMuted, {Align::Left, false, true}) + 4;
        const std::string by = !b.penName.empty() ? b.penName + (b.author.empty() || b.author == b.penName ? "" : " (" + b.author + ")") : b.author;
        if (!by.empty()) ty += gfx_.text(L"by " + w(by), {x, ty, tw, 22}, Font::SerifSmall, pal_.inkMuted, {Align::Left, false, true}) + 8;
        // Words (against the target), chapters, status.
        int chapters = 0;
        for (auto* n : lib_->manuscriptOf(b.id)) if (n->kind == NodeKind::Chapter) ++chapters;
        const std::int64_t target = bookTarget(b.id);
        float sx = x;
        std::wstring wl = wordsText(b.wordCount);
        if (target > 0) wl += L" of " + wordsText(target).substr(0, wordsText(target).find(L' '));
        gfx_.text(wl, {sx, ty, 260, 22}, Font::Body, pal_.ink, {Align::Left, false, true, true});
        sx += std::min(260.0f, gfx_.measureWidth(wl, Font::Body)) + 22;
        const std::wstring cl = std::to_wstring(chapters) + (chapters == 1 ? L" chapter" : L" chapters");
        gfx_.text(cl, {sx, ty, 140, 22}, Font::Body, pal_.inkMuted, {Align::Left, false, true, true});
        sx += gfx_.measureWidth(cl, Font::Body) + 22;
        statusTag(sx, ty + 1, b.status, 160);
        ty += 30;
        if (target > 0) {
            const float f = std::clamp((float)b.wordCount / (float)target, 0.0f, 1.0f);
            gfx_.fillRounded({x, ty, 260, 5}, 2.5f, pal_.rule);
            if (f > 0) gfx_.fillRounded({x, ty, std::max(5.0f, 260 * f), 5}, 2.5f, pal_.ribbon);
            ty += 16;
        }
        ty += 6;
        // The ways in: carry on writing (where you were), plan it, its bible, and the rest.
        float bx = x;
        std::wstring cont = L"Start writing";
        if (!lib_->childrenOf(b.id, {}).empty()) {
            cont = L"Open in Writing";
            if (settings_.lastBook == b.id)
                if (const ManuscriptNode* n = lib_->node(settings_.lastNode); n && !n->deleted)
                    cont = L"Continue writing · " + (n->title.empty() ? w(label(n->kind)) : w(n->title));
        }
        const float cwid = std::min(gfx_.measureWidth(cont, Font::BodyStrong) + 36, std::max(160.0f, tw - 330));
        button("book:write", {bx, ty, cwid, 34}, cont, [this, id = b.id] { openWriting(id); }, true);
        bx += cwid + 10;
        float bw2 = 0;
        smallButton("book:plan", bx, ty + 2, L"Plan", [this, id = b.id] { openPlanning(id); }, &bw2);
        bx += bw2 + 8;
        smallButton("book:bible", bx, ty + 2, L"The bible", [this, wid = b.worldId, id = b.id] { openWorldbuilding(wid, wid.empty() ? id : Id{}); }, &bw2);
        bx += bw2 + 8;
        menuButton("book:tools", bx, ty + 1, L"Book tools", [this, id = b.id] {
            std::vector<ToolItem> t;
            t.push_back(ToolItem::item(L"Review", [this, id] { openReview(id); }));
            t.push_back(ToolItem::item(L"Typesetting", [this, id] { openTypesetting(id); }));
            t.push_back(ToolItem::item(L"Publishing", [this, id] { openPublishing(id); }));
            t.push_back(ToolItem::line());
            t.push_back(ToolItem::item(L"Import into this book…", [this, id] { startImport(id); }));
            t.push_back(ToolItem::item(L"Design a cover…", [this, id] { openPublishing(id); openCoverDesigner(); }));
            t.push_back(ToolItem::line());
            t.push_back(ToolItem::item(L"Move to Trash", [this] { trashFocused(); }));
            return t;
        });
        ty += 34;
        y = std::max(y + cw * 1.5f, ty) + 26;
    }
    // The tabs.
    {
        static const wchar_t* names[] = {L"Overview", L"Book settings", L"Editions", L"Import & export", L"History"};
        std::vector<PageTab> tabs;
        for (int i = 0; i < 5; ++i) {
            PageTab t;
            t.label = names[i];
            t.on = bookTab_ == i;
            t.act = [this, i] { commitFields(); bookTab_ = i; invalidate(); };
            tabs.push_back(t);
        }
        pageTabs("book:tabs", r.x, y, r.right(), tabs);
        y += 40 + 22;
    }
    const float half = (r.w - 20) / 2;
    // A card whose height is what its contents took last time (it's drawn behind them).
    auto cardBehind = [&](const std::string& key, const Rect& at) {
        float& h = areaH_[key];
        card({at.x, at.y, at.w, std::max(h, 60.0f)}, 12);
        return &h;
    };
    if (bookTab_ == 0) {
        // Overview: the synopsis (click to change it), and the manuscript.
        gfx_.text(L"Synopsis", {r.x, y, r.w, 28}, Font::Heading, pal_.ink, {Align::Left, false});
        y += 32;
        const float measure = std::min(r.w, 720.0f);
        if (editingInline("book:syn")) {
            const float h = inlineHeight(110);
            inlineBox({r.x - 8, y - 4, measure + 8, h});
            y += h + 34;
        } else {
            float& h = areaH_["book:syn"];
            editableArea("book:ed:syn", {r.x, y, measure, std::max(h, 18.0f)}, L"Click to change the synopsis", [this] {
                const Book before = *lib_->book(viewId_);
                startInline("book:syn", FSynopsis, [this, before] { lib_->updateBook(before); });
            });
            h = gfx_.text(b.synopsis.empty() ? L"Write a synopsis…" : w(b.synopsis), {r.x, y, measure, 2000}, Font::Body,
                          b.synopsis.empty() ? pal_.inkFaint : pal_.ink);
            y += h + 28;
        }
        gfx_.text(L"Manuscript", {r.x, y, r.w, 28}, Font::Heading, pal_.ink, {Align::Left, false});
        y += 34;
        const auto top = lib_->childrenOf(b.id, {});
        if (top.empty())
            y += gfx_.text(L"No chapters yet. Start writing, or import an EPUB, Word, OpenDocument, PDF, web page, Markdown or text file "
                           L"(Import & export).", {r.x, y, measure, 40}, Font::Small, pal_.inkMuted) + 12;
        int shown = 0;
        for (auto* n : lib_->manuscriptOf(b.id)) {
            if (n->kind == NodeKind::Scene) continue;
            if (++shown > 60) { gfx_.text(L"…", {r.x, y, 40, 20}, Font::Small, pal_.inkFaint, {Align::Left, false}); y += 22; break; }
            const bool part = n->kind == NodeKind::Part;
            const float ix = r.x + (part || n->parentId.empty() ? 0 : 20);
            const Rect row{ix, y, measure - (ix - r.x), 30};
            const std::string id = "bookms:" + n->id;
            addHit(id, row, [this, nid = n->id, bid = b.id] { openWriting(bid); selectNode(nid); });
            if (hoverAmount(id) > 0.01f) gfx_.fillRounded(row, 7, pal_.surface.withAlpha(0.9f * hoverAmount(id)));
            std::int64_t words = n->wordCount;
            std::function<void(const Id&)> add = [&](const Id& pid) { for (auto* c : lib_->childrenOf(b.id, pid)) { words += c->wordCount; add(c->id); } };
            add(n->id);
            gfx_.circle(row.x + 10, row.y + 15, 3.5f, statusColour(n->status), true);
            gfx_.text(n->title.empty() ? w(label(n->kind)) : w(n->title), {row.x + 22, row.y, row.w - 150, row.h},
                      part ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
            gfx_.text(wordsText(words), {row.right() - 130, row.y, 122, row.h}, Font::Small, pal_.inkMuted, {Align::Right, false, false, true});
            if (focused(id)) focusRing(row);
            y += 32;
        }
    } else if (bookTab_ == 1) {
        // Book settings: what the details panel had, in cards.
        const Rect c1{r.x, y, half, 0}, c2{r.x + half + 20, y, half, 0};
        {
            float* h = cardBehind("book:c1", c1);
            const Rect c{c1.x + 18, 0, c1.w - 36, 0};
            float cy = c1.y + 16;
            gfx_.text(L"The book", {c.x, cy, c.w, 26}, Font::Heading, pal_.ink, {Align::Left, false});
            cy += 34;
            cy = pageField(c, cy, L"Title", FName);
            cy = pageField(c, cy, L"Subtitle", FSubtitle);
            panelChoice("book:status", c, cy, L"Status", w(label(b.status)), [this] { popupStatusMenu(); });
            panelChoice("book:type", c, cy, L"Type of book", bookTypeValue(b), [this, bid = b.id] { chooseBookType(bid); });
            panelChoice("book:format", c, cy, L"Format", isComic(b.id) ? L"Comic script" : L"Novel or prose", [this] {
                const Id bid = viewId_;
                const int i = popupAt(hwnd(), {L"Novel or prose", L"Comic script"}, isComic(bid) ? 1 : 0);
                if (i < 0) return;
                lib_->setSetting("format:" + bid, i == 1 ? "comic" : "");
                if (i == 1 && lib_->setting("typeset:" + bid).empty()) lib_->setSetting("typeset:" + bid, R"({"preset":"comic-script"})");
                if (ts_.book == bid) ts_.book.clear();
                sendView();
                invalidate();
            });
            cy = pageField(c, cy, L"Tags, with commas between", FTags);
            const float nh = cy - c1.y + 6;
            if (std::abs(nh - *h) > 0.5f) { *h = nh; invalidate(); }
        }
        {
            float* h = cardBehind("book:c2", c2);
            const Rect c{c2.x + 18, 0, c2.w - 36, 0};
            float cy = c2.y + 16;
            gfx_.text(L"Author and publishing", {c.x, cy, c.w, 26}, Font::Heading, pal_.ink, {Align::Left, false});
            cy += 34;
            cy = pageField(c, cy, L"Author", FAuthor);
            cy = pageField(c, cy, L"Pen name", FPenName);
            cy = pageField(c, cy, L"Genre", FGenre);
            {
                const float hw = (c.w - 12) / 2;
                const float y0 = cy;
                cy = pageField({c.x, 0, hw, 0}, y0, L"Language", FLanguage);
                pageField({c.x + hw + 12, 0, hw, 0}, y0, L"Edition", FEdition);
            }
            if (lib_->editionsOf(b.id).empty()) cy = pageField(c, cy, L"ISBN", FIsbn);
            else {
                gfx_.text(L"ISBNs are kept with each edition (the Editions tab).", {c.x, cy, c.w, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
                cy += 30;
            }
            panelChoice("book:trim", c, cy, L"Trim size (for print)", w(trimSize(b.trim).label), [this] { popupTrimMenu(); });
            const float nh = cy - c2.y + 6;
            if (std::abs(nh - *h) > 0.5f) { *h = nh; invalidate(); }
        }
        y += std::max(areaH_["book:c1"], areaH_["book:c2"]) + 20;
        const Rect c3{r.x, y, half, 0}, c4{r.x + half + 20, y, half, 0};
        {
            float* h = cardBehind("book:c3", c3);
            const Rect c{c3.x + 18, 0, c3.w - 36, 0};
            float cy = c3.y + 16;
            gfx_.text(L"Cover", {c.x, cy, c.w, 26}, Font::Heading, pal_.ink, {Align::Left, false});
            cy += 34;
            cy = panelImage(c, cy, L"The book's cover", b.coverBlob, [this] { chooseImage(Focus::Book, viewId_, 0); }, [this] {
                Book n = *lib_->book(viewId_);
                const Book before = n;
                n.coverBlob.clear();
                lib_->updateBook(n);
                flashUndo(L"Removed the cover.", [this, before] { lib_->updateBook(before); invalidate(); });
            }, false);
            linkText("book:designcover", c.x + 7, cy - 4, L"Design a cover…", [this, bid = b.id] { openPublishing(bid); openCoverDesigner(); });
            cy += 34;
            const float nh = cy - c3.y + 6;
            if (std::abs(nh - *h) > 0.5f) { *h = nh; invalidate(); }
        }
        {
            // Its series: number and kind in each, or a note that it's in none.
            float* h = cardBehind("book:c4", c4);
            const Rect c{c4.x + 18, 0, c4.w - 36, 0};
            float cy = c4.y + 16;
            gfx_.text(L"Series", {c.x, cy, c.w, 26}, Font::Heading, pal_.ink, {Align::Left, false});
            cy += 34;
            bool any = false;
            for (const Series* s : lib_->seriesIn(b.worldId))
                if (const Membership* m = lib_->membership(s->id, b.id)) {
                    any = true;
                    gfx_.text(w(s->name), {c.x, cy, c.w - 120, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
                    linkText("book:ser:" + s->id, c.right() - gfx_.measureWidth(L"The series", Font::Small) - 4, cy, L"The series",
                             [this, sid = s->id] { goTo(View::Series, sid); });
                    cy += 28;
                    // Its number (typed) and its place: a main book, between books, or during one (0.73).
                    const Rect nb{c.x, cy, 96, 32};
                    const bool here = focus_.kind == Focus::Book && focus_.id == b.id && focus_.contextSeries == s->id;
                    gfx_.fillRounded(nb, 7, pal_.surface);
                    gfx_.strokeRounded(nb, 7, here && GetFocus() == fields_[FNumber] ? pal_.ribbon : pal_.rule);
                    if (here) {
                        if (nb.y >= pageRect_.y && nb.bottom() <= pageRect_.bottom()) placeField(FNumber, nb.inset(8, 6));
                    } else {
                        gfx_.text(m->number.empty() ? L"Unnumbered" : L"Book " + w(m->number), nb.inset(10, 0), Font::Body,
                                  m->number.empty() ? pal_.inkFaint : pal_.ink, {Align::Left, false, true, true});
                        addHit("book:num:" + s->id, nb, [this, sid = s->id, bid = b.id] {
                            setFocus({Focus::Book, bid, sid});
                            focusFieldSoon_ = FNumber;
                            invalidate();
                        });
                    }
                    chooserBox("book:role:" + s->id, {nb.right() + 10, cy, c.right() - nb.right() - 10, 32}, seriesRoleText(*m),
                               [this, sid = s->id, bid = b.id] { chooseSeriesRole(sid, bid); });
                    cy += 46;
                }
            if (!any) {
                cy += gfx_.text(L"In no series. Drag the book onto a series on its world's page to add it.", {c.x, cy, c.w, 40}, Font::Small,
                                pal_.inkMuted) + 8;
            }
            const float nh = cy - c4.y + 6;
            if (std::abs(nh - *h) > 0.5f) { *h = nh; invalidate(); }
        }
        y += std::max(areaH_["book:c3"], areaH_["book:c4"]) + 20;
    } else if (bookTab_ == 2) {
        // Editions: each with its ISBN; they're made and changed in Publishing.
        const auto eds = lib_->editionsOf(b.id);
        if (eds.empty()) {
            y += gfx_.text(L"No editions yet. Publishing makes a paperback and an ebook the first time it opens, each with its own ISBN, trim "
                           L"and design.", {r.x, y, std::min(r.w, 720.0f), 40}, Font::Small, pal_.inkMuted) + 12;
        }
        const float gap = 16, ew = 260;
        const int cols = std::max(1, (int)((r.w + gap) / (ew + gap)));
        const float ewid = (r.w - gap * (cols - 1)) / cols;
        int col = 0;
        for (const Edition* e : eds) {
            const Rect er{r.x + col * (ewid + gap), y, ewid, 110};
            card(er, 12);
            gfx_.text(upper(w(editionKindLabel(e->kind))), {er.x + 18, er.y + 16, er.w - 36, 16}, Font::Label, gold_, {Align::Left, false});
            gfx_.text(w(e->name.empty() ? std::string(editionKindLabel(e->kind)) : e->name), {er.x + 18, er.y + 34, er.w - 36, 26}, Font::Heading,
                      pal_.ink, {Align::Left, false, true});
            gfx_.text(e->isbn.empty() ? L"No ISBN yet" : L"ISBN " + w(e->isbn), {er.x + 18, er.y + 64, er.w - 36, 20}, Font::Small,
                      e->isbn.empty() ? pal_.inkFaint : pal_.inkMuted, {Align::Left, false, true});
            if (e->kind != "ebook")
                gfx_.text(w(trimSize(e->trim.empty() ? b.trim : e->trim).label), {er.x + 18, er.y + 84, er.w - 36, 18}, Font::Small, pal_.inkFaint,
                          {Align::Left, false, true});
            if (++col == cols) { col = 0; y += 110 + gap; }
        }
        if (col) y += 110 + gap;
        y += 6;
        smallButton("book:editions", r.x, y, L"Change them in Publishing", [this, id = b.id] { openPublishing(id); });
        y += 50;
    } else if (bookTab_ == 3) {
        // Bringing things in, and sending the book out.
        auto row = [&](const std::string& id, const std::wstring& title, const std::wstring& about, const std::wstring& label,
                       std::function<void()> act) {
            const Rect rr{r.x, y, std::min(r.w, 820.0f), 76};
            card(rr, 12);
            gfx_.text(title, {rr.x + 18, rr.y + 14, rr.w - 260, 24}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            gfx_.text(about, {rr.x + 18, rr.y + 38, rr.w - 260, 34}, Font::Small, pal_.inkMuted);
            const float lw = gfx_.measureWidth(label, Font::Body) + 26 + (cjui::iconFor(label) == cjui::Icon::None ? 0 : 21);
            smallButton(id, rr.right() - 18 - lw, rr.y + 23, label, std::move(act));
            y += 76 + 12;
        };
        gfx_.text(L"Bringing things in", {r.x, y, r.w, 28}, Font::Heading, pal_.ink, {Align::Left, false});
        y += 36;
        row("book:ie:import", L"A manuscript", L"EPUB, Word, OpenDocument, PDF, a web page, Markdown or text, as chapters of this book.", L"Import…",
            [this, id = b.id] { startImport(id); });
        row("book:ie:plan", L"A plan", L"An outline, threads and a structure written as text, matched to the chapters it already has.", L"Import planning…",
            [this, id = b.id] { openPlanning(id); importPlanText(); });
        y += 14;
        gfx_.text(L"Sending it out", {r.x, y, r.w, 28}, Font::Heading, pal_.ink, {Align::Left, false});
        y += 36;
        row("book:ie:publish", L"Print-ready PDF and EPUB", L"Checked first, for each edition, in Publishing.", L"Publishing",
            [this, id = b.id] { openPublishing(id); });
        row("book:ie:proof", L"A proof PDF", L"The typeset pages as they are, to read through or print at home.", L"Typesetting",
            [this, id = b.id] { openTypesetting(id); });
        row("book:ie:review", L"A review copy", L"One web page a reader can open and comment on; their comments come back into Review.", L"Make a review copy…",
            [this, id = b.id] { rv_.book = id; makeReviewCopy(); });
        row("book:ie:planout", L"The plan as text", L"Its outline, threads and structure, to keep or to change and import again.", L"Export the plan…",
            [this, id = b.id] { openPlanning(id); exportPlanText(); });
    } else {
        // History: when it changed, and its snapshots.
        const float measure = std::min(r.w, 720.0f);
        gfx_.text(L"Last changed " + (b.modified ? cjui::formatRelative((cj::UtcSeconds)b.modified, cjui::nowUtc()) : std::wstring(L"never")) +
                      L". Every chapter keeps its earlier versions as you write; a snapshot keeps the whole book as it is.",
                  {r.x, y, measure, 40}, Font::Small, pal_.inkMuted);
        y += 44;
        float bw = 0;
        smallButton("book:snap", r.x, y, L"+ Take a snapshot", [this, id = b.id] {
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t buf[64];
            swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
            if (!lib_->takeSnapshot(id, narrow(std::wstring(L"Snapshot ") + buf)).empty()) flash(L"Took a snapshot of the whole book.", 4);
            invalidate();
        }, &bw);
        smallButton("book:compare", r.x + bw + 8, y, L"Compare in Review", [this, id = b.id] { openReview(id); });
        y += 50;
        const auto snaps = lib_->snapshotsOf(b.id);
        if (snaps.empty()) y += gfx_.text(L"No snapshots yet.", {r.x, y, measure, 20}, Font::Small, pal_.inkFaint, {Align::Left, false}) + 10;
        for (const Snapshot& sn : snaps) {
            const Rect rr{r.x, y, measure, 52};
            card(rr, 10, 0.7f);
            gfx_.text(w(sn.name), {rr.x + 16, rr.y + 8, rr.w - 220, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
            gfx_.text(cjui::formatRelative((cj::UtcSeconds)sn.taken, cjui::nowUtc()), {rr.x + 16, rr.y + 28, rr.w - 220, 18}, Font::Small,
                      pal_.inkMuted, {Align::Left, false, true});
            gfx_.text(wordsText(sn.words), {rr.right() - 200, rr.y, 184, rr.h}, Font::Small, pal_.inkMuted, {Align::Right, false, false, true});
            y += 60;
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
    gfx_.fillRounded(box, 7, pal_.surface);
    gfx_.strokeRounded(box, 7, GetFocus() == fields_[f] ? pal_.ribbon : pal_.rule);
    // A field being edited on a corkboard card (0.51) is there, not here.
    if (ws_ == Workspace::Planning && pl_.cardEdit == (int)f)
        gfx_.text(L"Being edited on the card", box.inset(10, 0), Font::Small, pal_.inkFaint, {Align::Left, false, true, h < 40});
    else if (ie_.field == (int)f)   // being edited where it's shown (0.70)
        gfx_.text(L"Being edited above", box.inset(10, 0), Font::Small, pal_.inkFaint, {Align::Left, false, true, h < 40});
    else if (inView) placeField(f, box);
    return y + h + 12;
}

float StudioApp::panelImage(const Rect& r, float y, const std::wstring& lbl, const std::string& blob, std::function<void()> choose,
                            std::function<void()> clear, bool wide) {
    gfx_.text(lbl, {r.x, y, r.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 20;
    // An empty picture is a short box, so the panel isn't mostly blank space (0.44).
    const bool none = blob.empty();
    const Rect box = wide ? Rect{r.x, y, r.w, none ? 48.0f : r.w * 0.38f} : Rect{r.x, y, 96, none ? 60.0f : 144.0f};
    if (blob.empty() || !gfx_.drawImage(w(lib_->blobPath(blob)), box, true)) {
        gfx_.fillRounded(box, 3, pal_.sunken);
        gfx_.text(L"No picture yet", box, Font::Small, pal_.inkFaint, {Align::Center, false, false, true});
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
    // A box you drop down, framed like the text fields, its value on the left (0.43).
    chooserBox(id, {c.x, y, c.w, 30}, value, std::move(act));
    y += 42;
}

// Every drop-down in the Studio (0.67): a field-like box, the value on the left and a drawn chevron on the right.
bool StudioApp::chooserBox(const std::string& id, const Rect& b, const std::wstring& value, std::function<void()> act) {
    addHit(id, b, std::move(act));
    const float hv = hoverAmount(id);
    gfx_.fillRounded(b, 7, pal_.surface.mix(pal_.sunken, 0.35f * hv));
    gfx_.strokeRounded(b, 7, pal_.rule.mix(pal_.inkFaint, 0.5f * hv));
    gfx_.text(value, {b.x + 10, b.y, b.w - 38, b.h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
    const float cx = b.right() - 15, cy = b.y + b.h / 2;
    gfx_.line(cx - 4, cy - 2, cx, cy + 2, pal_.inkMuted, 1.4f);
    gfx_.line(cx, cy + 2, cx + 4, cy - 2, pal_.inkMuted, 1.4f);
    if (focused(id)) focusRing(b);
    return hovered(id);
}

void StudioApp::drawPanel(const Rect& r) {
    if (!panelAsCard_) {   // as a card (0.69), that's its background
        gfx_.fill(r, pal_.paper.mix(pal_.surface, 0.5f));
        gfx_.line(r.x + 0.5f, r.y, r.x + 0.5f, r.bottom(), pal_.rule);
    }
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
            {
                // At a glance (0.44): words, chapters, the target, and when it was last changed.
                int chapters = 0;
                for (auto* n : lib_->manuscriptOf(b->id)) if (n->kind == NodeKind::Chapter) ++chapters;
                const std::int64_t target = bookTarget(b->id);
                const Rect box{c.x, y, c.w, target > 0 ? 82.0f : 64.0f};
                gfx_.fillRounded(box, 6, pal_.sunken.mix(pal_.surface, 0.3f));
                const float col = (box.w - 24) / 3;
                auto stat = [&](int i, const std::wstring& big, const std::wstring& small) {
                    const float sx = box.x + 12 + col * i;
                    gfx_.text(big, {sx, box.y + 10, col - 6, 22}, Font::Heading, pal_.ink, {Align::Left, false, true});
                    gfx_.text(small, {sx, box.y + 34, col - 6, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
                };
                std::wstring wn = wordsText(b->wordCount);
                stat(0, b->wordCount > 0 ? wn.substr(0, wn.find(L' ')) : L"0", L"words");
                stat(1, std::to_wstring(chapters), chapters == 1 ? L"chapter" : L"chapters");
                stat(2, b->modified ? cjui::formatRelative((cj::UtcSeconds)b->modified, cjui::nowUtc()) : L"\u2014", L"last changed");
                if (target > 0) {
                    const float f = std::clamp((float)b->wordCount / (float)target, 0.0f, 1.0f);
                    const Rect bar{box.x + 12, box.y + 62, box.w - 24, 5};
                    gfx_.fillRounded(bar, 2.5f, pal_.rule);
                    if (f > 0) gfx_.fillRounded({bar.x, bar.y, std::max(5.0f, bar.w * f), bar.h}, 2.5f, pal_.ribbon);
                    gfx_.text(std::to_wstring((int)std::lround(f * 100)) + L"% of " + wordsText(target), {bar.x, box.y + 66, bar.w, 16},
                              Font::Small, pal_.inkFaint, {Align::Right, false, true});
                    // the percentage sits under the bar, right-aligned
                }
                y += box.h + 14;
            }
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
                gfx_.text(L"Place in the series", {c.x + half + 10, y, half, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
                y += 20;
                const Rect nb{c.x, y, half, 30};
                gfx_.fillRounded(nb, 7, pal_.surface);
                gfx_.strokeRounded(nb, 7, GetFocus() == fields_[FNumber] ? pal_.ribbon : pal_.rule);
                if (nb.y >= panelRect_.y + 40 && nb.bottom() <= panelRect_.bottom()) placeField(FNumber, nb);
                const Rect kb{c.x + half + 10, y, half, 30};
                chooserBox("panel:kind", kb, seriesRoleText(*m), [this] { chooseSeriesRole(focus_.contextSeries, focus_.id); });
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
            linkText("panel:designcover", c.x + 7, y - 6, L"Design a cover\u2026", [this, bid = b->id] {
                openPublishing(bid);
                openCoverDesigner();
            });
            y += 30;
            // A novel or a comic script: comics get Page, Panel, Dialogue, Caption and SFX in Writing,
            // and the comic script layout in Typesetting.
            panelChoice("panel:btype", c, y, L"Type of book", bookTypeValue(*b), [this] { chooseBookType(focus_.id); });
            panelChoice("panel:format", c, y, L"Format", isComic(b->id) ? L"Comic script" : L"Novel or prose", [this] {
                const Id bid = focus_.id;
                const int i = popupAt(hwnd(), {L"Novel or prose", L"Comic script"}, isComic(bid) ? 1 : 0);
                if (i < 0) return;
                lib_->setSetting("format:" + bid, i == 1 ? "comic" : "");
                // A comic with no typesetting design of its own starts with the comic script layout.
                if (i == 1 && lib_->setting("typeset:" + bid).empty()) lib_->setSetting("typeset:" + bid, R"({"preset":"comic-script"})");
                if (ts_.book == bid) ts_.book.clear();   // its design is read again
                sendView();
                flash(i == 1 ? L"A comic script: Page, Panel, Dialogue, Caption and SFX are in Writing's style menu (Alt+1 to Alt+5)." : L"A novel.", 6);
                invalidate();
            });
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
            lx += gfx_.measureWidth(t, Font::Small) + 26;
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
                   L"objects holds covers and pictures; backups has full copies (see Backups below). "
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

    section(L"BACKUPS");
    y += gfx_.text(L"A full copy of the library is made when it opens, every 30 minutes while you're working (if anything changed) "
                   L"and when you close the Studio. The newest are kept in the library's backups folder.",
                   {r.x, y, r.w, 80}, Font::Small, pal_.inkMuted) + 10;
    if (settings_.backupFolder.empty()) {
        y += gfx_.text(L"For safety, keep copies somewhere else too: another drive, a USB stick or a synced folder such as OneDrive. "
                       L"Only finished backups are copied there, so syncing them is safe.",
                       {r.x, y, r.w, 60}, Font::Small, pal_.inkMuted) + 10;
        float bw = 0;
        smallButton("set:bkfolder", r.x, y, L"Also copy backups to a folder...", [this] { chooseBackupFolder(); }, &bw);
        y += 48;
    } else {
        gfx_.text(L"Also copied to", {r.x, y, r.w, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        y += 20;
        y += gfx_.text(w(settings_.backupFolder), {r.x, y, r.w, 40}, Font::BodyStrong, pal_.ink) + 8;
        float bx = r.x, bw = 0;
        smallButton("set:bkshow", bx, y, L"Show the folder", [this] {
            ShellExecuteW(hwnd(), L"open", w(settings_.backupFolder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }, &bw); bx += bw + 8;
        smallButton("set:bkfolder", bx, y, L"Change...", [this] { chooseBackupFolder(); }, &bw); bx += bw + 8;
        smallButton("set:bkstop", bx, y, L"Stop copying", [this] { settings_.backupFolder.clear(); saveSettings(); }, &bw);
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
    rowLabel(L"Chapter details in Writing");   // (the other workspaces show details where they're needed, 0.71)
    segmented("set:panel", {cx, y, cw, 28}, {L"Shown", L"Hidden"}, settings_.writingDetails ? 0 : 1, [this](int i) {
        settings_.writingDetails = i == 0; saveSettings();
    });
    y += 48;

    section(L"BOOK TYPES");
    y = drawBookTypeSettings(r, y);

    section(L"KEYBOARD SHORTCUTS");
    struct K { std::wstring keys, what; };
    auto group = [&](const std::wstring& name, const std::vector<K>& keys) {
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
    for (const auto& [name, keys] : shortcutGroups()) {
        std::vector<K> list;
        for (const auto& [k, what] : keys) list.push_back({k, what});
        group(name, list);
    }

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
    smallButton("set:website", r.x, y + 4, L"Visit the website", [] { cjui::northray::openWebsite(L"writing-studio/"); });
    y += 44;
    return y - r.y + 20;
}

}  // namespace nrs


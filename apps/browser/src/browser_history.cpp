// NorthRay Browser: the history (northray://history, Ctrl+H). Pages are recorded
// when they finish opening (PageEvents::loaded) and get their title when it
// arrives; the page lists them by day, newest first, with search, delete and
// "Clear history".
#include "browser_window.hpp"
#include "cjui/dialog.hpp"

#include "nrb/url.hpp"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <ctime>

namespace nrb {

using cjui::Align;
using cjui::Font;
using cjui::MenuItem;
using cjui::Rect;

void BrowserWindow::openHistoryStore() {
    const std::wstring state = settingsPath_.substr(0, settingsPath_.find_last_of(L"\\/"));
    const std::wstring folder = state + L"\\User";
    CreateDirectoryW(folder.c_str(), nullptr);
    std::string error;
    history_ = HistoryStore::open(narrow(folder + L"\\history.db"), error);
    if (!history_) flash(L"History couldn't be opened: " + widen(error), 12);
}

void BrowserWindow::recordVisit(int id, const std::wstring& uri) {
    TabView* v = view(id);
    if (!v || !history_ || !v->page) return;
    const std::string url = narrow(uri);
    if (!historyUrl(url)) return;
    v->visitId = history_->add(url, narrow(v->page->title()));
    v->visitUrl = url;
}

void BrowserWindow::setPageSearchCue(const wchar_t* cue) {
    if (!pageSearch_ || pageSearchCue_ == cue) return;
    pageSearchCue_ = cue;
    SendMessageW(pageSearch_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
}

void BrowserWindow::showHistoryPage() {
    if (!history_) { flash(L"History isn't available.", 6); return; }
    int target = 0;
    for (TabView& v : views_)
        if (v.showStart && v.historyPage) target = v.id;
    if (!target)
        if (TabView* a = activeView(); a && a->showStart && !a->bookmarksPage && !a->downloadsPage) target = a->id;
    if (!target) target = newTab(L"", true, true);
    TabView* v = view(target);
    if (!v) return;
    if (v->bookmarksPage && v->id == model_.activeId()) finishRename(true);
    v->showStart = true;
    v->bookmarksPage = false;
    v->downloadsPage = false;
    v->referencesPage = false;
    v->historyPage = true;
    v->error.reset();
    if (Tab* t = model_.find(target)) { t->title = "History"; t->url = "northray://history"; }
    activateTab(target);
    pageQuery_.clear();
    if (pageSearch_) SetWindowTextW(pageSearch_, L"");
    resetScroll();
    updateAddressText();
    updateTitle();
    invalidate();
}

namespace {

std::tm localTm(std::int64_t t) {
    std::tm tm{};
    const std::time_t tt = (std::time_t)t;
    localtime_s(&tm, &tt);
    return tm;
}

std::int64_t startOfDay(std::int64_t t) {
    std::tm tm = localTm(t);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (std::int64_t)std::mktime(&tm);
}

std::wstring dayHeading(std::int64_t day, std::int64_t today) {
    if (day == today) return L"Today";
    if (day == startOfDay(today - 12 * 3600)) return L"Yesterday";
    const std::tm tm = localTm(day);
    static const wchar_t* days[] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};
    static const wchar_t* months[] = {L"January", L"February", L"March", L"April", L"May", L"June", L"July",
                                      L"August", L"September", L"October", L"November", L"December"};
    return std::wstring(days[tm.tm_wday]) + L" " + std::to_wstring(tm.tm_mday) + L" " + months[tm.tm_mon];
}

std::wstring clock(std::int64_t t) {
    const std::tm tm = localTm(t);
    wchar_t b[8];
    swprintf(b, 8, L"%02d:%02d", tm.tm_hour, tm.tm_min);
    return b;
}

}  // namespace

void BrowserWindow::clearHistoryMenu(POINT screen) {
    if (!history_) return;
    enum : UINT { Hour = 1, Today, Week, All };
    const std::vector<MenuItem> items = {
        MenuItem::item(L"The last hour", Hour), MenuItem::item(L"Today", Today),
        MenuItem::item(L"The last 7 days", Week), MenuItem::sep(), MenuItem::item(L"Everything", All)};
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_, cjui::MenuAnchor::TopRight);
    if (!cmd) return;
    const std::int64_t now = (std::int64_t)std::time(nullptr);
    const std::int64_t since = cmd == Hour ? now - 3600 : cmd == Today ? startOfDay(now) : cmd == Week ? now - 7 * 24 * 3600 : 0;
    const wchar_t* what = cmd == Hour ? L"the last hour" : cmd == Today ? L"today" : cmd == Week ? L"the last 7 days" : L"all time";
    const std::wstring ask = std::wstring(L"Clear the history of ") + what +
                             L"?\n\nThe pages stay open, and bookmarks, passwords and sites' sign-ins aren't affected. This can't be undone.";
    if (cjui::messageBox(hwnd(), ask.c_str(), L"Clear history", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
    const std::size_t n = history_->clearSince(since);
    for (TabView& v : views_) v.visitId = 0;
    flash(n == 1 ? L"1 page was removed from the history." : std::to_wstring(n) + L" pages were removed from the history.", 6);
    invalidate();
}

void BrowserWindow::drawHistoryPage(const Rect& r) {
    gfx_.fill(r, pal_.paper);
    tileRects_.clear();
    rowRects_.clear();
    if (!history_) return;
    const float w = std::min(r.w - 48, 960.0f);
    const float x = r.x + (r.w - w) / 2;
    float y = r.y + 30;

    gfx_.text(L"History", {x, y, w, 36}, Font::Title, pal_.ink, {Align::Left, false});
    const size_t n = history_->count();
    gfx_.text(n == 0 ? L"Nothing yet" : n == 1 ? L"1 page, kept for 90 days" : std::to_wstring(n) + L" pages, kept for 90 days",
              {x, y + 38, w, 20}, Font::Small, pal_.inkMuted);
    const std::wstring clearLabel = L"Clear history...";
    const float bw = gfx_.measureWidth(clearLabel, Font::Body) + 28;
    const Rect cb{x + w - bw, y + 4, bw, 34};
    button("hip:clear", cb, clearLabel, [this, cb] {
        const float s = gfx_.dpiScale();
        POINT pt{(LONG)std::lround(cb.right() * s), (LONG)std::lround((cb.bottom() + 4) * s)};
        ClientToScreen(hwnd(), &pt);
        clearHistoryMenu(pt);
    });
    y += 74;

    const Rect sb{x, y, std::min(420.0f, w), 38};
    gfx_.fillRounded(sb, 8, pal_.sunken);
    gfx_.strokeRounded(sb, 8, GetFocus() == pageSearch_ ? pal_.focus : pal_.rule, GetFocus() == pageSearch_ ? 1.5f : 1);
    gfx_.circle(sb.x + 18, sb.y + 17, 5.5f, pal_.inkMuted, false, 1.5f);
    gfx_.line(sb.x + 22, sb.y + 21, sb.x + 26, sb.y + 25, pal_.inkMuted, 1.5f);
    pageSearchRect_ = {sb.x + 36, sb.y + 9, sb.w - 46, 20};
    pageSearchVisible_ = true;
    setPageSearchCue(L"Search history");
    y += 56;

    const std::vector<Visit> visits = history_->list(narrow(pageQuery_), 0, 1500);
    const Rect clip{x - 4, y, w + 8, r.bottom() - y - 16};
    float ry = beginScroll(clip);
    if (visits.empty())
        ry += gfx_.text(pageQuery_.empty() ? L"Pages you open appear here, so you can find them again."
                                           : L"No pages in the history match \"" + pageQuery_ + L"\".",
                        {x, ry + 8, w, 60}, Font::Body, pal_.inkMuted) + 16;
    const std::int64_t today = startOfDay((std::int64_t)std::time(nullptr));
    std::int64_t day = -1;
    for (const Visit& vi : visits) {
        const std::int64_t d = startOfDay(vi.at);
        if (d != day) {
            day = d;
            ry += day == startOfDay(visits.front().at) ? 4 : 18;
            gfx_.text(dayHeading(d, today), {x, ry, w, 24}, Font::BodyStrong, pal_.ink, {Align::Left, false});
            ry += 32;
        }
        const Rect row{x, ry, w, 44};
        const std::string id = "hir:" + std::to_string(vi.id);
        // Only rows on screen take part (a long history draws quickly).
        if (row.bottom() >= clip.y && row.y <= clip.bottom()) {
            addHit(id, row, [this, url = vi.url] {
                if (GetKeyState(VK_CONTROL) & 0x8000) newTab(widen(url), false, true);
                else if (TabView* a = activeView()) { a->historyPage = false; navigateTab(*a, widen(url)); focusPage(); }
            });
            const float hv = hoverAmount(id);
            if (hv > 0.01f) gfx_.fillRounded(row, 6, pal_.ink.withAlpha(0.05f * hv));
            gfx_.text(clock(vi.at), {row.x + 12, row.y, 52, row.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
            const float tx = row.x + 72, tw = row.w - 72 - 90;
            const std::wstring title = vi.title.empty() ? widen(displayUrl(vi.url)) : widen(vi.title);
            gfx_.text(title, {tx, row.y + 4, tw, 20}, Font::Body, pal_.ink, {Align::Left, false, true});
            gfx_.text(widen(hostOf(vi.url)), {tx, row.y + 23, tw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
            if (hv > 0.01f || focused(id)) {
                const std::wstring del = L"Remove";
                const float ax = row.right() - 12 - gfx_.measureWidth(del, Font::Small);
                linkText("hir:del:" + std::to_string(vi.id), ax, row.y + 13, del, [this, vid = vi.id] {
                    if (history_ && history_->remove(vid)) invalidate();
                });
            }
            if (focused(id)) focusRing(row);
        }
        ry += 46;
    }
    endScroll(ry - clip.y + 8);
}

}  // namespace nrb

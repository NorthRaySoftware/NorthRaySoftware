// NorthRay Browser's start page (a new tab, and Home): a greeting on the night
// sky with a search field, your bookmarks bar as tiles, recently closed tabs,
// and a few keyboard shortcuts. Everything on it is drawn locally; it loads
// nothing from the internet.
#include "browser_window.hpp"

#include "nrb/url.hpp"

#include "cjui/northray.hpp"

#include <algorithm>
#include <cmath>

namespace nrb {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::Rect;
namespace nr = cjui::northray;

namespace {

std::wstring greeting() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (t.wHour >= 5 && t.wHour < 12) return L"Good morning";
    if (t.wHour >= 12 && t.wHour < 18) return L"Good afternoon";
    return L"Good evening";
}

std::wstring today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[96] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &t, nullptr, buf, (int)std::size(buf), nullptr);
    return buf;
}

}  // namespace

void BrowserWindow::drawStartPage(const Rect& r) {
    gfx_.fill(r, pal_.paper);
    tileRects_.clear();
    rowRects_.clear();
    const float pad = r.w > 900 ? 44 : 24;
    const float w = std::min(r.w - pad * 2, 900.0f);
    const float x = r.x + (r.w - w) / 2;
    float y = r.y + std::clamp(r.h * 0.06f, 20.0f, 48.0f);

    // The night: greeting, date, and the search field.
    const float heroH = 214;
    const Rect hero{x, y, w, heroH};
    nr::nightSky(gfx_, hero, 14, 1.0f, 7, pal_.paper);
    gfx_.text(greeting(), {hero.x + 36, hero.y + 34, hero.w - 72, 40}, Font::Title, nr::kNightInk, {Align::Left, false});
    gfx_.text(today(), {hero.x + 36, hero.y + 74, hero.w - 72, 22}, Font::Body, nr::kNightMuted, {Align::Left, false});
    const Rect field{hero.x + 36, hero.y + 120, hero.w - 72, 50};
    const bool focusedField = GetFocus() == startSearch_;
    gfx_.fillRounded(field, 12, pal_.sunken);
    if (focusedField) gfx_.strokeRounded(field, 12, nr::kAuroraGreen, 2);
    gfx_.circle(field.x + 24, field.y + 23, 7, pal_.inkMuted, false, 1.8f);
    gfx_.line(field.x + 29, field.y + 28, field.x + 34, field.y + 33, pal_.inkMuted, 1.8f);
    startSearchRect_ = {field.x + 48, field.y + (field.h - 22) / 2, field.w - 64, 22};
    startSearchVisible_ = true;
    y = hero.bottom() + 34;

    drawStartTiles({x, y, w, r.bottom() - y}, y);

    // Recently closed tabs, if any.
    const auto& closed = model_.closed();
    if (!closed.empty() && y + 90 < r.bottom()) {
        y += sectionLabel(x, y, w, L"RECENTLY CLOSED") + 4;
        int shown = 0;
        for (const ClosedTab& c : closed) {
            if (shown >= 5 || y + 34 > r.bottom() - 40) break;
            const Rect row{x, y, w, 34};
            const std::string id = "start:closed:" + std::to_string(shown);
            const std::wstring url = widen(c.url);
            addHit(id, row, [this, url] {
                if (TabView* v = activeView()) { navigateTab(*v, url); focusPage(); }
            });
            const float hv = hoverAmount(id);
            if (hv > 0.01f) gfx_.fillRounded(row, 6, pal_.ink.withAlpha(0.05f * hv));
            const std::wstring title = c.title.empty() ? widen(hostOf(c.url)) : widen(c.title);
            gfx_.text(title, {row.x + 10, row.y, row.w * 0.55f, row.h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
            gfx_.text(widen(displayUrl(c.url)), {row.x + row.w * 0.55f + 16, row.y, row.w * 0.45f - 26, row.h}, Font::Small,
                      pal_.inkMuted, {Align::Right, false, true, true});
            if (focused(id)) focusRing(row);
            y += 36;
            ++shown;
        }
        y += 18;
    }

    // A few keys worth knowing, quietly at the foot.
    if (y + 30 < r.bottom()) {
        const std::wstring tips = L"Ctrl+L  address bar     Ctrl+T  new tab     Ctrl+D  bookmark this page     "
                                  L"Ctrl+Shift+B  bookmarks bar     Ctrl+Shift+T  reopen a closed tab";
        gfx_.text(tips, {x, std::max(y, r.bottom() - 44), w, 20}, Font::Small, pal_.inkFaint, {Align::Center, false, true});
    }
}

// The bookmarks bar as tiles: the quickest way back to the sites you use.
void BrowserWindow::drawStartTiles(const Rect& area, float& y) {
    const float x = area.x, w = area.w;
    const float labelH = sectionLabel(x, y, w, L"BOOKMARKS");
    if (bookmarks_) {
        const std::wstring all = L"All bookmarks";
        linkText("start:allbm", x + w - gfx_.measureWidth(all, Font::Small), y - 2, all, [this] { showBookmarksPage(); });
    }
    y += labelH + 8;
    std::vector<Bookmark> items;
    if (bookmarks_)
        for (const Bookmark& b : bookmarks_->children(BookmarkStore::kBar))
            if (!b.folder) items.push_back(b);
    if (items.empty()) {
        const Rect card{x, y, w, 84};
        gfx_.fillRounded(card, 10, pal_.surface);
        gfx_.strokeRounded(card, 10, pal_.rule, 1);
        drawStar(card.x + 38, card.y + 42, 13, pal_.ribbon, false);
        gfx_.text(L"Your bookmarks appear here", {card.x + 70, card.y + 18, card.w - 90, 22}, Font::BodyStrong, pal_.ink,
                  {Align::Left, false});
        gfx_.text(L"Open a page and click the star in the address bar, or press Ctrl+D.", {card.x + 70, card.y + 42, card.w - 300, 22},
                  Font::Small, pal_.inkMuted, {Align::Left, false, true});
        if (bookmarks_) {
            const std::wstring imp = L"Import from another browser";
            linkText("start:import", card.right() - 20 - gfx_.measureWidth(imp, Font::Small), card.y + 44, imp,
                     [this] { importBookmarks(); });
        }
        y = card.bottom() + 30;
        return;
    }
    const float tileW = 112, tileH = 100, gap = 12;
    const int cols = std::max(1, (int)((w + gap) / (tileW + gap)));
    const int rows = (int)items.size() > cols ? 2 : 1;
    const size_t maxTiles = (size_t)cols * (size_t)rows;
    const float rowW = std::min((float)std::min(items.size(), (size_t)cols), (float)cols) * (tileW + gap) - gap;
    const float x0 = x + (w - rowW) / 2;
    for (size_t i = 0; i < items.size() && i < maxTiles; ++i) {
        const Bookmark& b = items[i];
        const Rect t{x0 + (float)(i % (size_t)cols) * (tileW + gap), y + (float)(i / (size_t)cols) * (tileH + gap), tileW, tileH};
        const std::string id = "start:bm:" + std::to_string(b.id);
        addHit(id, t, [this, b] { openBookmark(b, (GetKeyState(VK_CONTROL) & 0x8000) != 0); });
        tileRects_.push_back({b.id, t});
        const float hv = hoverAmount(id);
        softShadow(t, 10, hv);
        gfx_.fillRounded({t.x, t.y - 2 * hv, t.w, t.h}, 10, pal_.surface);
        gfx_.strokeRounded({t.x, t.y - 2 * hv, t.w, t.h}, 10, pal_.rule.mix(pal_.inkFaint, 0.3f * hv), 1);
        const float iy = t.y + 18 - 2 * hv;
        drawBookmarkIcon(b, {t.x + (t.w - 32) / 2, iy, 32, 32});
        gfx_.text(widen(b.title), {t.x + 8, iy + 42, t.w - 16, 36}, Font::Small, pal_.ink, {Align::Center, true, true});
        if (focused(id)) focusRing(t);
    }
    y += (float)rows * (tileH + gap) + 22;
}

}  // namespace nrb

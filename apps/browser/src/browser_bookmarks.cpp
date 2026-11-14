// Bookmarks in NorthRay Browser: the star in the address bar, the bookmarks
// bar, the bookmarks menu, the bookmarks page (northray://bookmarks), import
// and export; and the password and form settings. The bookmarks themselves
// are nrb::BookmarkStore (core/), one SQLite file in the user folder.
#include "browser_window.hpp"

#include "nrb/url.hpp"

#include "cjui/northray.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

namespace nrb {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::Rect;
namespace nr = cjui::northray;

namespace {

constexpr UINT kBookmarkCmd = 10000;   // + index into a menu's list of bookmark ids
constexpr UINT CmdBmOpen = 1, CmdBmNewTab = 2, CmdBmRename = 3, CmdBmDelete = 4, CmdBmMoveBar = 5, CmdBmMoveOther = 6,
               CmdBmUp = 7, CmdBmDown = 8, CmdBmManage = 9, CmdBmOpenAll = 10;

std::wstring editText(HWND e) {
    const int n = GetWindowTextLengthW(e);
    std::wstring t((size_t)n, L'\0');
    if (n) GetWindowTextW(e, t.data(), n + 1);
    return t;
}

LRESULT CALLBACK quietEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == 1 /* Ctrl+A */)) {
        if (w == 1) SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return DefSubclassProc(h, m, w, l);
}

// A monogram's colour: one of the aurora's, chosen by the site's name.
Color monogramColor(const std::string& host) {
    static const Color colors[] = {nr::kAuroraTeal, nr::kAuroraBlue, nr::kAuroraViolet, cjui::hexColor("#E0884F"),
                                   cjui::hexColor("#4FA3A8"), cjui::hexColor("#C86F9C")};
    unsigned h = 2166136261u;
    for (char c : host) h = (h ^ (unsigned char)c) * 16777619u;
    return colors[h % (sizeof colors / sizeof colors[0])];
}

std::wstring firstLetter(const Bookmark& b) {
    // The name's first letter (what the person sees), else the site's.
    std::string host = hostOf(b.url);
    if (host.rfind("www.", 0) == 0) host = host.substr(4);
    for (const std::wstring& t : {widen(b.title), widen(host)})
        for (wchar_t c : t)
            if (iswalnum(c)) return std::wstring(1, (wchar_t)towupper(c));
    return L"•";
}

bool readFileUtf8(const std::wstring& path, std::string& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}

}  // namespace

// ---------------------------------------------------------------- the store

void BrowserWindow::openBookmarkStore() {
    const std::wstring state = settingsPath_.substr(0, settingsPath_.find_last_of(L"\\/"));
    const std::wstring folder = state + L"\\User";   // the person's own things, apart from the engine's data
    CreateDirectoryW(folder.c_str(), nullptr);
    std::string error, notice;
    bookmarks_ = BookmarkStore::open(narrow(folder + L"\\bookmarks.db"), error, &notice);
    if (!bookmarks_) {
        flash(L"Bookmarks couldn't be opened: " + widen(error), 12);
        return;
    }
    if (!notice.empty()) flash(widen(notice), 15);
    bookmarks_->backupIfDue((std::int64_t)std::time(nullptr));
}

bool BrowserWindow::currentBookmarkable(std::string* url) {
    TabView* v = activeView();
    if (!v || v->showStart || v->error || !v->page) return false;
    const std::string u = narrow(v->page->url());
    if (!bookmarkableUrl(u)) return false;
    if (url) *url = u;
    return true;
}

// ---------------------------------------------------------------- star

void BrowserWindow::drawStar(float cx, float cy, float r, Color c, bool filled) {
    float px[10], py[10];
    for (int i = 0; i < 10; ++i) {
        const float a = -1.5707963f + (float)i * 0.6283185f;
        const float rr = i % 2 ? r * 0.45f : r;
        px[i] = cx + rr * std::cos(a);
        py[i] = cy + rr * std::sin(a);
    }
    if (filled)
        for (int i = 0; i < 10; ++i) gfx_.fillTriangle(cx, cy, px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], c);
    for (int i = 0; i < 10; ++i) gfx_.line(px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], c, 1.4f);
}

void BrowserWindow::starClicked() {
    std::string url;
    if (!bookmarks_ || !currentBookmarkable(&url)) return;
    TabView* v = activeView();
    if (auto existing = bookmarks_->findUrl(url)) {
        // Already bookmarked: what to do with it.
        const auto b = *existing;
        const bool onBar = b.parent == BookmarkStore::kBar;
        const std::vector<MenuItem> items = {
            MenuItem::item(onBar ? L"In the bookmarks bar" : L"In Other bookmarks", 0, {}, false),
            MenuItem::sep(),
            MenuItem::item(L"Rename...", CmdBmRename),
            MenuItem::item(onBar ? L"Move to Other bookmarks" : L"Move to the bookmarks bar", onBar ? CmdBmMoveOther : CmdBmMoveBar),
            MenuItem::item(L"Remove bookmark", CmdBmDelete),
            MenuItem::sep(),
            MenuItem::item(L"Bookmarks page", CmdBmManage, L"Ctrl+Shift+O"),
        };
        const float s = gfx_.dpiScale();
        RECT rc;
        GetClientRect(hwnd(), &rc);
        POINT pt{rc.right - (LONG)(52 * s), (LONG)(92 * s)};
        ClientToScreen(hwnd(), &pt);
        const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_, cjui::MenuAnchor::TopRight);
        if (cmd == CmdBmRename) showBookmarksPage(b.parent, b.id);
        else if (cmd == CmdBmMoveOther) { bookmarks_->move(b.id, BookmarkStore::kOther, -1); flash(L"Moved to Other bookmarks."); }
        else if (cmd == CmdBmMoveBar) { bookmarks_->move(b.id, BookmarkStore::kBar, -1); flash(L"Moved to the bookmarks bar."); }
        else if (cmd == CmdBmDelete) { bookmarks_->remove(b.id); flash(L"Bookmark removed."); }
        else if (cmd == CmdBmManage) showBookmarksPage();
        invalidate();
        return;
    }
    const std::string title = v && v->page ? narrow(v->page->title()) : std::string();
    const std::int64_t id = bookmarks_->add(BookmarkStore::kBar, title.empty() ? hostOf(url) : title, url);
    if (!id) { flash(widen(bookmarks_->lastError()), 8); return; }
    if (v && !v->favicon.empty()) bookmarks_->setIcon(id, v->favW, v->favH, v->favicon);
    flash(L"Bookmarked, in the bookmarks bar. Click the star again to rename, move or remove it.", 6);
    invalidate();
}

void BrowserWindow::toggleBookmarksBar() {
    settings_.showBookmarksBar = !settings_.showBookmarksBar;
    settings_.save(settingsPath_);
    invalidate();
}

// ---------------------------------------------------------------- drawing pieces

void BrowserWindow::drawBookmarkIcon(const Bookmark& b, const Rect& r, bool onNight) {
    if (b.folder) {
        const Color c = onNight ? nr::kNightMuted : pal_.inkMuted;
        const float x = r.x + r.w * 0.06f, w = r.w * 0.88f, y = r.y + r.h * 0.18f, h = r.h * 0.66f;
        gfx_.fillRounded({x, y, w * 0.45f, h * 0.3f}, 1.5f, c.withAlpha(0.85f));
        gfx_.fillRounded({x, y + h * 0.15f, w, h * 0.85f}, 2, c);
        return;
    }
    if (!b.icon.empty() && gfx_.drawPixels(L"bm:" + std::to_wstring(b.id) + L":" + std::to_wstring(b.icon.size()), b.iconW, b.iconH,
                                           b.icon.data(), r, true))
        return;
    // No icon saved: a monogram in one of the aurora's colours.
    const Color c = monogramColor(hostOf(b.url));
    gfx_.fillRounded(r, r.w * 0.28f, c);
    gfx_.text(firstLetter(b), r, r.w >= 28 ? Font::Heading : Font::Label, nr::kNightTop, {Align::Center, false, false, true});
}

void BrowserWindow::drawBookmarksBar(const Rect& r) {
    gfx_.fill(r, pal_.surface);
    gfx_.line(r.x, r.bottom() - 0.5f, r.right(), r.bottom() - 0.5f, pal_.rule);
    barRects_.clear();
    if (!bookmarks_) return;
    const auto items = bookmarks_->children(BookmarkStore::kBar);
    if (items.empty()) {
        gfx_.text(L"Bookmarks you add with the star (or Ctrl+D) appear here.", {r.x + 16, r.y, r.w - 32, r.h}, Font::Small,
                  pal_.inkFaint, {Align::Left, false, true, true});
        return;
    }
    const float moreW = 34;
    float x = r.x + 8;
    size_t shown = 0;
    for (const Bookmark& b : items) {
        const std::wstring t = widen(b.title);
        const float tw = std::min(gfx_.measureWidth(t, Font::Small), 150.0f);
        const float w = 8 + 16 + 7 + tw + 10;
        if (x + w > r.right() - moreW - 8 && shown < items.size()) break;
        const Rect ir{x, r.y + 4, w, r.h - 8};
        const std::string id = "bm:" + std::to_string(b.id);
        if (b.folder) {
            addHit(id, ir, [this, fid = b.id, ir] {
                const float s = gfx_.dpiScale();
                POINT pt{(LONG)(ir.x * s), (LONG)(ir.bottom() * s)};
                ClientToScreen(hwnd(), &pt);
                showFolderMenu(fid, pt);
            });
        } else {
            addHit(id, ir, [this, b] { openBookmark(b, (GetKeyState(VK_CONTROL) & 0x8000) != 0); });
        }
        barRects_.push_back({b.id, ir});
        const float hv = hoverAmount(id);
        if (hv > 0.01f) gfx_.fillRounded(ir, 6, pal_.ink.withAlpha((pressed(id) ? 0.11f : 0.065f) * hv));
        drawBookmarkIcon(b, {ir.x + 8, ir.y + (ir.h - 16) / 2, 16, 16});
        gfx_.text(t, {ir.x + 31, ir.y, tw + 2, ir.h}, Font::Small, pal_.ink, {Align::Left, false, true, true});
        if (focused(id)) focusRing(ir);
        x += w + 2;
        ++shown;
    }
    if (shown < items.size()) {
        // The rest, in a menu.
        const Rect mr{r.right() - moreW - 6, r.y + 4, moreW, r.h - 8};
        addHit("bm:more", mr, [this, mr, shown] {
            std::vector<std::int64_t> ids;
            std::vector<MenuItem> menu;
            const auto all = bookmarks_->children(BookmarkStore::kBar);
            for (size_t i = shown; i < all.size(); ++i) {
                const Bookmark& b = all[i];
                if (b.folder) menu.push_back(MenuItem::sub(widen(b.title), folderItems(b.id, ids, 1)));
                else {
                    ids.push_back(b.id);
                    menu.push_back(MenuItem::item(widen(b.title), kBookmarkCmd + (UINT)ids.size() - 1));
                }
            }
            const float s = gfx_.dpiScale();
            POINT pt{(LONG)(mr.right() * s), (LONG)(mr.bottom() * s)};
            ClientToScreen(hwnd(), &pt);
            const UINT cmd = cjui::showMenu(hwnd(), menu, pt, pal_, cjui::MenuAnchor::TopRight);
            openFromMenuId(cmd, ids, false);
        });
        const float hv = hoverAmount("bm:more");
        if (hv > 0.01f) gfx_.fillRounded(mr, 6, pal_.ink.withAlpha(0.065f * hv));
        const float cx = mr.x + mr.w / 2, cy = mr.y + mr.h / 2;
        for (int k = 0; k < 2; ++k) {
            const float ox = (float)k * 5 - 2.5f;
            gfx_.line(cx + ox - 2.5f, cy - 4, cx + ox + 1.5f, cy, pal_.inkMuted, 1.4f);
            gfx_.line(cx + ox + 1.5f, cy, cx + ox - 2.5f, cy + 4, pal_.inkMuted, 1.4f);
        }
    }
}

// ---------------------------------------------------------------- opening

void BrowserWindow::openBookmark(const Bookmark& b, bool inNewTab) {
    if (b.folder || b.url.empty()) return;
    if (inNewTab) { newTab(widen(b.url), false, true); return; }
    TabView* v = activeView();
    if (!v) { newTab(widen(b.url), true, false); return; }
    navigateTab(*v, widen(b.url));
    focusPage();
}

std::vector<MenuItem> BrowserWindow::folderItems(std::int64_t folder, std::vector<std::int64_t>& ids, int depth) {
    std::vector<MenuItem> out;
    if (!bookmarks_) return out;
    const auto items = bookmarks_->children(folder);
    for (const Bookmark& b : items) {
        if (out.size() >= 60) { out.push_back(MenuItem::item(L"More on the bookmarks page...", CmdBmManage)); break; }
        if (b.folder) {
            if (depth < 6) {
                auto sub = folderItems(b.id, ids, depth + 1);
                if (sub.empty()) sub.push_back(MenuItem::item(L"(Empty)", 0, {}, false));
                out.push_back(MenuItem::sub(widen(b.title), std::move(sub)));
            }
        } else {
            ids.push_back(b.id);
            out.push_back(MenuItem::item(widen(b.title), kBookmarkCmd + (UINT)ids.size() - 1));
        }
    }
    return out;
}

void BrowserWindow::openFromMenuId(UINT cmd, const std::vector<std::int64_t>& ids, bool inNewTab) {
    if (cmd == CmdBmManage) { showBookmarksPage(); return; }
    if (cmd < kBookmarkCmd || cmd - kBookmarkCmd >= ids.size() || !bookmarks_) return;
    if (auto b = bookmarks_->get(ids[cmd - kBookmarkCmd])) openBookmark(*b, inNewTab);
}

void BrowserWindow::showFolderMenu(std::int64_t folder, POINT screen) {
    std::vector<std::int64_t> ids;
    auto items = folderItems(folder, ids, 1);
    if (items.empty()) items.push_back(MenuItem::item(L"(Empty)", 0, {}, false));
    if (!ids.empty()) {
        items.push_back(MenuItem::sep());
        items.push_back(MenuItem::item(L"Open all in new tabs", CmdBmOpenAll));
    }
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (cmd == CmdBmOpenAll) {
        for (std::int64_t id : ids)
            if (auto b = bookmarks_->get(id)) newTab(widen(b->url), false, true);
        return;
    }
    openFromMenuId(cmd, ids, false);
}

std::vector<MenuItem> BrowserWindow::bookmarksSubmenu(std::vector<std::int64_t>& ids) {
    std::vector<MenuItem> m = {
        MenuItem::item(L"Bookmark this page", 1, L"Ctrl+D", currentBookmarkable() && bookmarks_),
        MenuItem::item(L"Show bookmarks bar", 2, L"Ctrl+Shift+B", true, settings_.showBookmarksBar),
        MenuItem::item(L"Bookmarks page", CmdBmManage, L"Ctrl+Shift+O", bookmarks_ != nullptr),
        MenuItem::sep(),
        MenuItem::item(L"Import from another browser's file...", 3, {}, bookmarks_ != nullptr),
        MenuItem::item(L"Export to a file...", 4, {}, bookmarks_ && bookmarks_->count() > 0),
    };
    if (bookmarks_) {
        auto bar = folderItems(BookmarkStore::kBar, ids, 1);
        auto other = folderItems(BookmarkStore::kOther, ids, 1);
        if (!bar.empty() || !other.empty()) m.push_back(MenuItem::sep());
        if (bar.size() > 20) {
            bar.resize(20);
            bar.push_back(MenuItem::item(L"More on the bookmarks page...", CmdBmManage));
        }
        for (auto& b : bar) m.push_back(std::move(b));
        if (!other.empty()) m.push_back(MenuItem::sub(L"Other bookmarks", std::move(other)));
    }
    return m;
}

// ---------------------------------------------------------------- one bookmark's menu

void BrowserWindow::showBookmarkMenu(std::int64_t id, POINT screen) {
    if (!bookmarks_) return;
    const auto b = bookmarks_->get(id);
    if (!b) return;
    std::vector<MenuItem> items;
    if (!b->folder) {
        items.push_back(MenuItem::item(L"Open", CmdBmOpen));
        items.push_back(MenuItem::item(L"Open in a new tab", CmdBmNewTab));
    } else {
        items.push_back(MenuItem::item(L"Open all in new tabs", CmdBmOpenAll, {}, !bookmarks_->children(id).empty()));
    }
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"Rename...", CmdBmRename));
    items.push_back(MenuItem::item(L"Move up", CmdBmUp, {}, b->position > 0));
    items.push_back(MenuItem::item(L"Move down", CmdBmDown, {}, b->position + 1 < (int)bookmarks_->children(b->parent).size()));
    if (b->parent != BookmarkStore::kBar) items.push_back(MenuItem::item(L"Move to the bookmarks bar", CmdBmMoveBar));
    if (b->parent != BookmarkStore::kOther) items.push_back(MenuItem::item(L"Move to Other bookmarks", CmdBmMoveOther));
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"Delete", CmdBmDelete));
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    switch (cmd) {
        case CmdBmOpen: openBookmark(*b, false); break;
        case CmdBmNewTab: openBookmark(*b, true); break;
        case CmdBmOpenAll:
            for (const Bookmark& c : bookmarks_->children(id))
                if (!c.folder) newTab(widen(c.url), false, true);
            break;
        case CmdBmRename: {
            TabView* v = activeView();
            if (v && v->showStart && v->bookmarksPage) startRename(id);
            else showBookmarksPage(b->parent, id);
            break;
        }
        case CmdBmUp: moveBookmarkBy(id, -1); break;
        case CmdBmDown: moveBookmarkBy(id, +1); break;
        case CmdBmMoveBar: bookmarks_->move(id, BookmarkStore::kBar, -1); break;
        case CmdBmMoveOther: bookmarks_->move(id, BookmarkStore::kOther, -1); break;
        case CmdBmDelete: deleteBookmark(id); break;
        default: break;
    }
    invalidate();
}

void BrowserWindow::moveBookmarkBy(std::int64_t id, int delta) {
    if (auto b = bookmarks_->get(id)) bookmarks_->move(id, b->parent, std::max(0, b->position + delta));
    invalidate();
}

void BrowserWindow::deleteBookmark(std::int64_t id) {
    const auto b = bookmarks_->get(id);
    if (!b) return;
    if (b->folder && !bookmarks_->children(id).empty()) {
        const std::wstring q = L"Delete the folder \"" + widen(b->title) + L"\" and everything in it?";
        if (MessageBoxW(hwnd(), q.c_str(), L"Delete folder", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    }
    if (!bookmarks_->remove(id)) flash(widen(bookmarks_->lastError()), 8);
    else flash(b->folder ? L"Folder deleted." : L"Bookmark deleted.");
    invalidate();
}

bool BrowserWindow::bookmarkRightClick(float x, float y) {
    for (const auto* list : {&barRects_, &tileRects_, &rowRects_})
        for (auto& [id, r] : *list)
            if (r.contains(x, y)) {
                const float s = gfx_.dpiScale();
                POINT pt{(LONG)(x * s), (LONG)(y * s)};
                ClientToScreen(hwnd(), &pt);
                showBookmarkMenu(id, pt);
                return true;
            }
    return false;
}

bool BrowserWindow::bookmarkMiddleClick(float x, float y) {
    if (!bookmarks_) return false;
    for (const auto* list : {&barRects_, &tileRects_, &rowRects_})
        for (auto& [id, r] : *list)
            if (r.contains(x, y)) {
                if (auto b = bookmarks_->get(id)) openBookmark(*b, true);
                return true;
            }
    return false;
}

// ---------------------------------------------------------------- the bookmarks page

void BrowserWindow::showBookmarksPage(std::int64_t folder, std::int64_t rename) {
    if (!bookmarks_) { flash(L"Bookmarks aren't available.", 6); return; }
    // Reuse an open bookmarks page, else this tab if it's the start page, else a new tab.
    int target = 0;
    for (TabView& v : views_)
        if (v.showStart && v.bookmarksPage) target = v.id;
    if (!target)
        if (TabView* a = activeView(); a && a->showStart) target = a->id;
    if (!target) target = newTab(L"", true, true);
    TabView* v = view(target);
    if (!v) return;
    v->showStart = true;
    v->bookmarksPage = true;
    v->error.reset();
    if (Tab* t = model_.find(target)) { t->title = "Bookmarks"; t->url = "northray://bookmarks"; }
    activateTab(target);
    if (folder && bookmarks_->isFolder(folder)) pageFolder_ = folder;
    pageQuery_.clear();
    if (pageSearch_) SetWindowTextW(pageSearch_, L"");
    renaming_ = 0;
    if (rename) startRename(rename);
    updateAddressText();
    updateTitle();
    invalidate();
}

void BrowserWindow::folderTree(std::int64_t folder, int depth, float x, float& y, float w) {
    const auto b = bookmarks_->get(folder);
    if (!b) return;
    const std::string id = "bmf:" + std::to_string(folder);
    const Rect r{x, y, w, 32};
    const bool sel = pageQuery_.empty() && pageFolder_ == folder;
    addHit(id, r, [this, folder] {
        pageFolder_ = folder;
        pageQuery_.clear();
        if (pageSearch_) SetWindowTextW(pageSearch_, L"");
        finishRename(true);
        invalidate();
    });
    const float hv = hoverAmount(id);
    if (sel) gfx_.fillRounded(r, 6, pal_.ribbon.withAlpha(pal_.dark ? 0.18f : 0.12f));
    else if (hv > 0.01f) gfx_.fillRounded(r, 6, pal_.ink.withAlpha(0.06f * hv));
    const float ix = r.x + 10 + depth * 14.0f;
    drawBookmarkIcon(*b, {ix, r.y + 8, 16, 16});
    gfx_.text(widen(b->title), {ix + 24, r.y, r.right() - ix - 30, r.h}, sel ? Font::BodyStrong : Font::Body, pal_.ink,
              {Align::Left, false, true, true});
    if (focused(id)) focusRing(r);
    y += 34;
    for (const Bookmark& c : bookmarks_->children(folder))
        if (c.folder && depth < 8) folderTree(c.id, depth + 1, x, y, w);
}

void BrowserWindow::drawBookmarksPage(const Rect& r) {
    gfx_.fill(r, pal_.paper);
    tileRects_.clear();
    rowRects_.clear();
    if (!bookmarks_) return;
    if (!bookmarks_->isFolder(pageFolder_)) pageFolder_ = BookmarkStore::kBar;
    const float w = std::min(r.w - 48, 1080.0f);
    const float x = r.x + (r.w - w) / 2;
    float y = r.y + 30;

    // Header: title, count, actions.
    gfx_.text(L"Bookmarks", {x, y, w, 36}, Font::Title, pal_.ink, {Align::Left, false});
    const size_t n = bookmarks_->count();
    gfx_.text(n == 1 ? L"1 bookmark" : std::to_wstring(n) + L" bookmarks", {x, y + 38, w, 20}, Font::Small, pal_.inkMuted);
    float bx = x + w;
    auto action = [&](const std::string& id, const std::wstring& label, std::function<void()> act) {
        const float bw = gfx_.measureWidth(label, Font::Body) + 28;
        bx -= bw;
        button(id, {bx, y + 4, bw, 34}, label, std::move(act));
        bx -= 8;
    };
    action("bmp:export", L"Export...", [this] { exportBookmarks(); });
    action("bmp:import", L"Import...", [this] { importBookmarks(); });
    action("bmp:folder", L"New folder", [this] {
        const std::int64_t parent = pageQuery_.empty() ? pageFolder_ : BookmarkStore::kOther;
        const std::int64_t id = bookmarks_->addFolder(parent, "New folder");
        if (id) { pageFolder_ = parent; pageQuery_.clear(); startRename(id); }
        invalidate();
    });
    y += 74;

    // Search.
    const Rect sb{x, y, std::min(420.0f, w), 38};
    gfx_.fillRounded(sb, 8, pal_.sunken);
    gfx_.strokeRounded(sb, 8, GetFocus() == pageSearch_ ? pal_.focus : pal_.rule, GetFocus() == pageSearch_ ? 1.5f : 1);
    gfx_.circle(sb.x + 18, sb.y + 17, 5.5f, pal_.inkMuted, false, 1.5f);
    gfx_.line(sb.x + 22, sb.y + 21, sb.x + 26, sb.y + 25, pal_.inkMuted, 1.5f);
    pageSearchRect_ = {sb.x + 36, sb.y + 9, sb.w - 46, 20};
    pageSearchVisible_ = true;
    y += 56;

    // Folders on the left, the folder's contents (or search results) on the right.
    const float treeW = w >= 760 ? 240 : 0;
    const Rect list{x + treeW + (treeW ? 24 : 0), y, w - treeW - (treeW ? 24 : 0), r.bottom() - y - 16};
    if (treeW) {
        float ty = y;
        folderTree(BookmarkStore::kBar, 0, x, ty, treeW);
        folderTree(BookmarkStore::kOther, 0, x, ty, treeW);
    }
    std::vector<Bookmark> items;
    std::wstring heading;
    if (!pageQuery_.empty()) {
        items = bookmarks_->search(narrow(pageQuery_), 500);
        heading = items.empty() ? L"No bookmarks match \"" + pageQuery_ + L"\"." : L"Matching \"" + pageQuery_ + L"\"";
    } else {
        items = bookmarks_->children(pageFolder_);
        if (auto f = bookmarks_->get(pageFolder_)) heading = widen(f->title);
    }
    gfx_.text(heading, {list.x, list.y, list.w, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
    const Rect clip{list.x - 4, list.y + 30, list.w + 8, list.h - 30};
    float ry = beginScroll(clip);
    bool renameShown = false;
    for (const Bookmark& b : items) {
        const Rect row{list.x, ry, list.w, 46};
        const std::string id = "bmr:" + std::to_string(b.id);
        rowRects_.push_back({b.id, row});
        if (b.folder) addHit(id, row, [this, fid = b.id] { pageFolder_ = fid; pageQuery_.clear(); finishRename(true); invalidate(); });
        else addHit(id, row, [this, b] { openBookmark(b, (GetKeyState(VK_CONTROL) & 0x8000) != 0); });
        const float hv = hoverAmount(id);
        if (hv > 0.01f) gfx_.fillRounded(row, 6, pal_.ink.withAlpha(0.05f * hv));
        drawBookmarkIcon(b, {row.x + 12, row.y + 13, 20, 20});
        const float tx = row.x + 46, tw = row.w - 46 - 150;
        if (renaming_ == b.id) {
            const Rect er{tx - 6, row.y + 6, std::max(200.0f, tw), 34};
            gfx_.fillRounded(er, 6, pal_.sunken);
            gfx_.strokeRounded(er, 6, pal_.focus, 1.5f);
            renameRect_ = {er.x + 8, er.y + 7, er.w - 16, 20};
            renameShown = renameRect_.y > clip.y && renameRect_.bottom() < clip.bottom();
        } else if (b.folder) {
            gfx_.text(widen(b.title), {tx, row.y, tw, row.h}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
        } else {
            gfx_.text(widen(b.title), {tx, row.y + 5, tw, 20}, Font::Body, pal_.ink, {Align::Left, false, true});
            gfx_.text(widen(displayUrl(b.url)), {tx, row.y + 24, tw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        }
        // Actions, on hover (and from the right-click menu, and the keyboard via the menu key).
        if ((hv > 0.01f || focused(id)) && renaming_ != b.id) {
            float ax = row.right() - 12;
            const std::wstring del = L"Delete", ren = L"Rename";
            ax -= gfx_.measureWidth(del, Font::Small);
            linkText("bmr:del:" + std::to_string(b.id), ax, row.y + 15, del, [this, bid = b.id] { deleteBookmark(bid); });
            ax -= 16 + gfx_.measureWidth(ren, Font::Small);
            linkText("bmr:ren:" + std::to_string(b.id), ax, row.y + 15, ren, [this, bid = b.id] { startRename(bid); });
        }
        if (focused(id)) focusRing(row);
        ry += 48;
    }
    if (items.empty() && pageQuery_.empty())
        ry += gfx_.text(pageFolder_ == BookmarkStore::kBar
                            ? L"Nothing here yet. Open a page and click the star in the address bar (or press Ctrl+D) to bookmark it."
                            : L"This folder is empty. Right-click a bookmark and move it here.",
                        {list.x, ry + 8, list.w, 60}, Font::Body, pal_.inkMuted) + 16;
    endScroll(ry - clip.y + 8);
    renameVisible_ = renaming_ && renameShown;
}

void BrowserWindow::startRename(std::int64_t id) {
    if (!bookmarks_) return;
    const auto b = bookmarks_->get(id);
    if (!b || id == BookmarkStore::kBar || id == BookmarkStore::kOther) return;
    finishRename(true);
    renaming_ = id;
    if (pageQuery_.empty() && b->parent != pageFolder_) pageFolder_ = b->parent;
    SetWindowTextW(renameEdit_, widen(b->title).c_str());
    invalidate();
    UpdateWindow(hwnd());   // lays the field out before it takes the keyboard
    ShowWindow(renameEdit_, SW_SHOW);
    SetFocus(renameEdit_);
    SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
}

void BrowserWindow::finishRename(bool commit) {
    if (!renaming_) return;
    const std::int64_t id = renaming_;
    renaming_ = 0;
    if (commit && bookmarks_) bookmarks_->rename(id, narrow(editText(renameEdit_)));
    ShowWindow(renameEdit_, SW_HIDE);
    if (GetFocus() == renameEdit_) SetFocus(hwnd());
    invalidate();
}

// ---------------------------------------------------------------- import and export

void BrowserWindow::importBookmarks() {
    if (!bookmarks_) return;
    wchar_t file[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd();
    ofn.lpstrFilter = L"Bookmarks file (*.html, *.htm)\0*.html;*.htm\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)std::size(file);
    ofn.lpstrTitle = L"Import bookmarks";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    std::string html;
    if (!readFileUtf8(file, html) || html.size() > 64 * 1024 * 1024) { flash(L"That file couldn't be read.", 8); return; }
    if (html.find("NETSCAPE-Bookmark-file") == std::string::npos && html.find("<DT>") == std::string::npos &&
        html.find("<dt>") == std::string::npos) {
        flash(L"That isn't a bookmarks file. In Chrome, Edge or Firefox, export bookmarks as HTML, then import that.", 12);
        return;
    }
    const int n = bookmarks_->importHtml(html, BookmarkStore::kOther);
    if (n == 0 && !bookmarks_->lastError().empty()) flash(L"The import didn't work: " + widen(bookmarks_->lastError()), 10);
    else flash(n == 1 ? L"Imported 1 bookmark." : L"Imported " + std::to_wstring(n) + L" bookmarks.", 8);
    invalidate();
}

void BrowserWindow::exportBookmarks() {
    if (!bookmarks_) return;
    wchar_t file[MAX_PATH * 2] = L"bookmarks.html";
    wchar_t docs[MAX_PATH] = L"";
    SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs);
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd();
    ofn.lpstrFilter = L"Bookmarks file (*.html)\0*.html\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)std::size(file);
    ofn.lpstrInitialDir = docs;
    ofn.lpstrDefExt = L"html";
    ofn.lpstrTitle = L"Export bookmarks";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;
    const std::string html = bookmarks_->exportHtml();
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f << html;
    if (!f.good()) { flash(L"The file couldn't be saved there.", 8); return; }
    flash(L"Exported. Chrome, Edge and Firefox can import that file.", 8);
}

// ---------------------------------------------------------------- the extra text fields

HWND BrowserWindow::makeEdit(const wchar_t* cue) {
    HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd(), nullptr, GetModuleHandleW(nullptr),
                             nullptr);
    SetWindowSubclass(e, quietEditProc, 2, 0);
    SendMessageW(e, EM_SETLIMITTEXT, 2048, 0);
    if (cue) SendMessageW(e, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
    if (editFont_) SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), FALSE);
    return e;
}

void BrowserWindow::placeEdit(HWND e, const Rect& r, bool show) {
    if (!e) return;
    if (!show) {
        if (IsWindowVisible(e)) {
            if (GetFocus() == e) SetFocus(hwnd());
            ShowWindow(e, SW_HIDE);
        }
        return;
    }
    const float s = gfx_.dpiScale();
    SetWindowPos(e, nullptr, (int)std::lround(r.x * s), (int)std::lround(r.y * s), (int)std::lround(r.w * s),
                 (int)std::lround(r.h * s), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), FALSE);
}

void BrowserWindow::positionExtraEdits() {
    placeEdit(startSearch_, startSearchRect_, startSearchVisible_);
    placeEdit(pageSearch_, pageSearchRect_, pageSearchVisible_);
    placeEdit(renameEdit_, renameRect_, renameVisible_);
    startSearchVisible_ = pageSearchVisible_ = renameVisible_ = false;   // each frame says again
}

// ---------------------------------------------------------------- passwords and forms

void BrowserWindow::setFormSaving(bool passwords, bool forms) {
    settings_.savePasswords = passwords;
    settings_.fillForms = forms;
    settings_.save(settingsPath_);
    for (TabView& v : views_)
        if (v.page && v.page->ready()) v.page->setFormSaving(passwords, forms);
    flash(passwords ? L"NorthRay Browser will offer to save passwords when you sign in."
                    : L"NorthRay Browser won't offer to save passwords. Saved ones stay until you delete them.", 8);
}

void BrowserWindow::clearSavedPasswords() {
    if (MessageBoxW(hwnd(),
                    L"Delete every password NorthRay Browser has saved on this PC, and the form entries it fills in?\n\n"
                    L"This can't be undone. Sites' own accounts aren't affected; you'll just need to type your passwords again.",
                    L"Delete saved passwords", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
        return;
    // Any page reaches the profile's password store; one is made for the purpose if none is open.
    for (TabView& v : views_)
        if (v.page && v.page->ready()) {
            v.page->clearSavedPasswords(true, [this](bool ok) {
                flash(ok ? L"Saved passwords and form entries deleted." : L"The saved passwords couldn't be deleted.", 8);
            });
            return;
        }
    TabView* a = activeView();
    if (!a || !ensurePage(*a)) { flash(L"The browser engine isn't running, so nothing could be deleted.", 8); return; }
    clearPasswordsWhenReady_ = true;
}

void BrowserWindow::explainPasswords() {
    MessageBoxW(hwnd(),
                L"When you sign in to a site, NorthRay Browser offers to save the password, and fills it in next time. "
                L"It can also fill in addresses and other form entries you've typed before.\n\n"
                L"They're kept by the browser engine (Microsoft Edge WebView2, which has the same password manager as Microsoft "
                L"Edge) in NorthRay Browser's own folder on this PC, encrypted with a key protected by your Windows account. "
                L"They aren't sent to NorthRay Software or anyone else.\n\n"
                L"Not yet in this version: a list to view or edit saved passwords one by one. You can turn saving off, or "
                L"delete them all, from the menu.",
                L"Saved passwords", MB_OK | MB_ICONINFORMATION);
}

}  // namespace nrb

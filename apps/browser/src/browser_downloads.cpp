// NorthRay Browser: downloads (northray://downloads, Ctrl+J) and blocking ads and trackers.
//
// Downloads: the engine's own download pop-up is replaced by the Downloads page. Each download is
// listed in DownloadStore (kept between runs) and followed through its DownloadHandle while it runs;
// a tab closed mid-download keeps its page, out of sight, until the download ends.
//
// Blocking: each page asks blockedOn() for its site, and nrb::shouldBlock() for each request to a
// listed service; the shield in the address bar shows the count and lets a site off.
#include "browser_window.hpp"
#include "cjui/dialog.hpp"

#include "nrb/url.hpp"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <ctime>

namespace nrb {

extern const UINT_PTR kDownloadsTimer;

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::Rect;

void BrowserWindow::openDownloadStore() {
    const std::wstring state = settingsPath_.substr(0, settingsPath_.find_last_of(L"\\/"));
    const std::wstring folder = state + L"\\User";
    CreateDirectoryW(folder.c_str(), nullptr);
    std::string error;
    downloads_ = DownloadStore::open(narrow(folder + L"\\downloads.db"), error);
    if (!downloads_) { flash(L"The downloads list couldn't be opened: " + widen(error), 12); return; }
    // The toolbar button shows when something was downloaded in the last day.
    const std::int64_t now = (std::int64_t)std::time(nullptr);
    for (const Download& d : downloads_->all()) if (d.started > now - 24 * 3600) downloadsSeen_ = true;
}

static DownloadState stateOf(DownloadHandle::State s) {
    switch (s) {
        case DownloadHandle::Paused: return DownloadState::Paused;
        case DownloadHandle::Done: return DownloadState::Done;
        case DownloadHandle::Cancelled: return DownloadState::Cancelled;
        case DownloadHandle::Failed: return DownloadState::Failed;
        default: return DownloadState::Running;
    }
}

void BrowserWindow::downloadStarted(int tabId, std::shared_ptr<DownloadHandle> h) {
    if (!downloads_ || !h) return;
    const DownloadHandle::Info i = h->info();
    Download d;
    d.url = narrow(i.url);
    d.path = narrow(i.path);
    d.mime = narrow(i.mime);
    d.total = i.total;
    d.received = i.received;
    const std::int64_t id = downloads_->add(d);
    if (!id) return;
    live_[id] = {h, tabId};
    h->onChanged([this, id] { downloadChanged(id); });
    downloadsSeen_ = true;
    invalidate();
}

void BrowserWindow::downloadChanged(std::int64_t id) {
    auto it = live_.find(id);
    if (it == live_.end() || !downloads_) return;
    const Download* cur = downloads_->find(id);
    if (!cur) return;
    const DownloadHandle::Info i = it->second.handle->info();
    Download d = *cur;
    const DownloadState before = d.state;
    d.path = narrow(i.path);
    d.received = i.received;
    if (i.total > 0) d.total = i.total;
    d.estimatedEnd = i.estimatedEnd;
    d.state = stateOf(i.state);
    d.reason = narrow(i.reason);
    downloads_->update(d);
    if (d.state != before) {
        if (d.state == DownloadState::Done) flash(L"Downloaded " + widen(d.name()) + L".", 5);
        else if (d.state == DownloadState::Failed) flash(L"The download of " + widen(d.name()) + L" failed: " + widen(d.reason) + L".", 8);
        if (d.state == DownloadState::Done || d.state == DownloadState::Cancelled || d.state == DownloadState::Failed) {
            // Let go of it shortly (not from inside its own callback).
            finishedDownloads_.push_back(id);
            SetTimer(hwnd(), kDownloadsTimer, 200, nullptr);
        }
    }
    invalidate();
}

bool BrowserWindow::tabHasDownloads(int tabId) const {
    for (const auto& [id, l] : live_)
        if (l.tab == tabId)
            if (const Download* d = downloads_ ? downloads_->find(id) : nullptr;
                d && (d->state == DownloadState::Running || d->state == DownloadState::Paused))
                return true;
    return false;
}

void BrowserWindow::releaseParkedPages() {
    for (std::int64_t id : finishedDownloads_) live_.erase(id);
    finishedDownloads_.clear();
    for (auto it = parked_.begin(); it != parked_.end();) {
        if (!tabHasDownloads(it->first)) {
            it->second->close();
            it = parked_.erase(it);
        } else ++it;
    }
}

void BrowserWindow::showDownloadsPage() {
    int target = 0;
    for (TabView& v : views_)
        if (v.showStart && v.downloadsPage) target = v.id;
    if (!target)
        if (TabView* a = activeView(); a && a->showStart && !a->bookmarksPage && !a->historyPage) target = a->id;
    if (!target) target = newTab(L"", true, true);
    TabView* v = view(target);
    if (!v) return;
    if (v->bookmarksPage && v->id == model_.activeId()) finishRename(true);
    v->showStart = true;
    v->bookmarksPage = false;
    v->historyPage = false;
    v->referencesPage = false;
    v->downloadsPage = true;
    v->error.reset();
    if (Tab* t = model_.find(target)) { t->title = "Downloads"; t->url = "northray://downloads"; }
    activateTab(target);
    resetScroll();
    updateAddressText();
    updateTitle();
    invalidate();
}

static std::wstring downloadsFolder() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

void BrowserWindow::openDownloaded(std::int64_t id, bool folder) {
    const Download* d = downloads_ ? downloads_->find(id) : nullptr;
    if (!d) return;
    const std::wstring path = widen(d->path);
    const bool there = !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!there && !folder) { flash(L"The file isn't there any more. It may have been moved or deleted.", 7); return; }
    if (folder) {
        if (there) {
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
                return;
            }
        }
        std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
        if (dir.empty() || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) dir = downloadsFolder();
        ShellExecuteW(hwnd(), L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    // Windows decides how to open it (and checks downloaded programs before they run).
    const HINSTANCE r = ShellExecuteW(hwnd(), L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) flash(L"Windows has no app set to open " + widen(d->name()) + L".", 7);
}

// The toolbar button: an arrow into a tray, with a ring for progress while something downloads.
void BrowserWindow::drawDownloadsButton(const Rect& r) {
    const std::string id = "tb:downloads";
    addHit(id, r, [this] { showDownloadsPage(); });
    const float hv = hoverAmount(id);
    if (hv > 0.01f) gfx_.fillRounded(r, 6, pal_.ink.withAlpha(0.07f * hv));
    const int active = downloads_ ? downloads_->active() : 0;
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    const Color ink = active ? pal_.ribbon : pal_.inkMuted.mix(pal_.ink, hv);
    gfx_.line(cx, cy - 7, cx, cy + 3, ink, 1.7f);
    gfx_.line(cx - 4.5f, cy - 1.5f, cx, cy + 3, ink, 1.7f);
    gfx_.line(cx + 4.5f, cy - 1.5f, cx, cy + 3, ink, 1.7f);
    gfx_.line(cx - 7, cy + 7, cx + 7, cy + 7, ink, 1.7f);
    if (active) {
        const double p = downloads_->progress();
        gfx_.circle(cx, cy, 13.5f, pal_.ribbon.withAlpha(0.18f), false, 2);
        if (p >= 0) {
            // A ring around the button, filling as it goes.
            const int steps = std::max(2, (int)(p * 48));
            for (int i = 0; i < steps; ++i) {
                const float a0 = -1.5708f + 6.2832f * (float)p * i / steps, a1 = -1.5708f + 6.2832f * (float)p * (i + 1) / steps;
                gfx_.line(cx + 13.5f * std::cos(a0), cy + 13.5f * std::sin(a0), cx + 13.5f * std::cos(a1), cy + 13.5f * std::sin(a1), pal_.ribbon, 2);
            }
        }
    }
    if (focused(id)) focusRing(r);
}

void BrowserWindow::drawDownloadsPage(const Rect& r) {
    gfx_.fill(r, pal_.paper);
    tileRects_.clear();
    rowRects_.clear();
    if (!downloads_) return;
    const float w = std::min(r.w - 48, 960.0f);
    const float x = r.x + (r.w - w) / 2;
    float y = r.y + 30;
    gfx_.text(L"Downloads", {x, y, w, 36}, Font::Title, pal_.ink, {Align::Left, false});
    const auto& all = downloads_->all();
    const int active = downloads_->active();
    std::wstring sub = all.empty() ? L"Nothing yet" : all.size() == 1 ? L"1 file" : std::to_wstring(all.size()) + L" files";
    if (active) sub += L", " + std::to_wstring(active) + L" downloading";
    gfx_.text(sub, {x, y + 38, w, 20}, Font::Small, pal_.inkMuted);
    float bx = x + w;
    if ((int)all.size() > active) {
        const std::wstring clear = L"Clear the list";
        const float cw = gfx_.measureWidth(clear, Font::Body) + 28;
        bx -= cw;
        button("dl:clear", {bx, y + 4, cw, 34}, clear, [this] {
            const std::size_t n = downloads_->clearFinished();
            flash(n == 1 ? L"1 download was taken off the list. The file is still where it was saved."
                         : std::to_wstring(n) + L" downloads were taken off the list. The files are still where they were saved.", 6);
            invalidate();
        });
        bx -= 8;
    }
    const std::wstring folder = L"Open the Downloads folder";
    const float fw = gfx_.measureWidth(folder, Font::Body) + 28;
    button("dl:folder", {bx - fw, y + 4, fw, 34}, folder, [this] {
        const std::wstring f = downloadsFolder();
        if (!f.empty()) ShellExecuteW(hwnd(), L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    y += 82;

    const Rect clip{x - 4, y, w + 8, r.bottom() - y - 16};
    float ry = beginScroll(clip);
    if (all.empty())
        ry += gfx_.text(L"Files you download appear here. They're saved in your Downloads folder.", {x, ry + 8, w, 60}, Font::Body,
                        pal_.inkMuted) + 16;
    const std::int64_t now = (std::int64_t)std::time(nullptr);
    for (const Download& d : all) {
        const Rect row{x, ry, w, 72};
        if (row.bottom() >= clip.y && row.y <= clip.bottom()) {
            const std::string rid = "dlr:" + std::to_string(d.id);
            const bool done = d.state == DownloadState::Done;
            const bool missing = done && GetFileAttributesW(widen(d.path).c_str()) == INVALID_FILE_ATTRIBUTES;
            if (done && !missing) addHit(rid, {row.x, row.y, row.w - 300, row.h}, [this, id = d.id] { openDownloaded(id, false); });
            const float hv = hoverAmount(rid);
            gfx_.fillRounded(row.inset(0, 4), 8, pal_.surface.mix(pal_.ink, 0.02f * hv));
            gfx_.strokeRounded(row.inset(0, 4), 8, pal_.rule, 1);
            // The file's kind, from its extension.
            std::wstring name = widen(d.name());
            std::wstring ext = name.find_last_of(L'.') == std::wstring::npos ? L"" : name.substr(name.find_last_of(L'.') + 1);
            if (ext.size() > 4) ext = ext.substr(0, 4);
            for (auto& c : ext) c = (wchar_t)towupper(c);
            const Rect ic{row.x + 16, row.y + 18, 28, 36};
            const Color tint = done && !missing ? pal_.ribbon : pal_.inkFaint;
            gfx_.fillRounded(ic, 4, tint.withAlpha(0.14f));
            gfx_.strokeRounded(ic, 4, tint.withAlpha(0.6f), 1);
            gfx_.text(ext.empty() ? L"FILE" : ext, {ic.x - 6, ic.y + 20, ic.w + 12, 14}, Font::Label, tint.mix(pal_.ink, 0.3f),
                      {Align::Center, false, true});
            const float tx = ic.right() + 16, tw = row.w - (tx - row.x) - 290;
            gfx_.text(name, {tx, row.y + 14, tw, 22}, Font::BodyStrong, missing ? pal_.inkMuted : pal_.ink, {Align::Left, false, true});
            std::wstring status = widen(downloadStatus(d, now));
            if (missing) status = L"Deleted or moved";
            const std::wstring from = widen(hostOf(d.url));
            if (!from.empty()) status += L"  ·  " + from;
            gfx_.text(status, {tx, row.y + 38, tw, 18}, Font::Small,
                      d.state == DownloadState::Failed ? pal_.live : pal_.inkMuted, {Align::Left, false, true});
            if (d.state == DownloadState::Running || d.state == DownloadState::Paused) {
                const Rect bar{tx, row.y + 59, tw, 4};
                gfx_.fillRounded(bar, 2, pal_.ink.withAlpha(0.08f));
                if (d.total > 0)
                    gfx_.fillRounded({bar.x, bar.y, bar.w * (float)std::clamp((double)d.received / d.total, 0.0, 1.0), bar.h}, 2,
                                     d.state == DownloadState::Paused ? pal_.inkFaint : pal_.ribbon);
            }
            // What can be done with it, at the right.
            float ax = row.right() - 16;
            auto action = [&](const std::string& aid, const std::wstring& label, std::function<void()> fn) {
                const float aw = gfx_.measureWidth(label, Font::Small);
                ax -= aw;
                linkText(aid, ax, row.y + 26, label, std::move(fn));
                ax -= 26;
            };
            const auto live = live_.find(d.id);
            const std::shared_ptr<DownloadHandle> h = live == live_.end() ? nullptr : live->second.handle;
            const std::string k = std::to_string(d.id);
            if (d.state == DownloadState::Running && h) {
                action("dl:cancel:" + k, L"Cancel", [h] { h->cancel(); });
                action("dl:pause:" + k, L"Pause", [h] { h->pause(); });
            } else if (d.state == DownloadState::Paused && h) {
                action("dl:cancel:" + k, L"Cancel", [h] { h->cancel(); });
                action("dl:resume:" + k, L"Resume", [h] { h->resume(); });
            } else if (done && !missing) {
                action("dl:remove:" + k, L"Remove", [this, id = d.id] { if (downloads_->remove(id)) invalidate(); });
                action("dl:show:" + k, L"Show in folder", [this, id = d.id] { openDownloaded(id, true); });
                action("dl:open:" + k, L"Open", [this, id = d.id] { openDownloaded(id, false); });
            } else {
                action("dl:remove:" + k, L"Remove", [this, id = d.id] { if (downloads_->remove(id)) invalidate(); });
                if (!d.url.empty() && (d.url.rfind("http", 0) == 0))
                    action("dl:again:" + k, L"Download again", [this, url = d.url] {
                        // Any web page can start it again; a new tab if none is open.
                        for (TabView& v : views_)
                            if (!v.showStart && v.page && v.page->ready()) { v.page->navigate(widen(url)); return; }
                        newTab(widen(url), false, true);
                    });
            }
            if (focused(rid)) focusRing(row.inset(0, 4));
        }
        ry += 76;
    }
    endScroll(ry - clip.y + 8);
}

// ------------------------------------------------------------------ blocking

bool BrowserWindow::blockedOn(const std::wstring& pageUrl) const {
    if (!settings_.blockTrackers) return false;
    const std::string site = siteOf(hostOf(narrow(pageUrl)));
    return std::find(settings_.blockerAllowed.begin(), settings_.blockerAllowed.end(), site) == settings_.blockerAllowed.end();
}

void BrowserWindow::blockerMenu(POINT screen) {
    TabView* v = activeView();
    if (!v || !v->page) return;
    const std::string site = siteOf(hostOf(narrow(v->page->url())));
    const bool siteOn = std::find(settings_.blockerAllowed.begin(), settings_.blockerAllowed.end(), site) == settings_.blockerAllowed.end();
    enum : UINT { Site = 1, Everywhere, About };
    std::wstring head = !settings_.blockTrackers ? L"Blocking is off everywhere"
                        : !siteOn                ? L"Blocking is off on " + widen(site)
                        : v->blocked == 1        ? L"1 ad or tracker blocked on this page"
                                                 : std::to_wstring(v->blocked) + L" ads and trackers blocked on this page";
    const std::vector<MenuItem> items = {
        MenuItem::item(head, 0, {}, false),
        MenuItem::sep(),
        MenuItem::item(L"Block them on " + widen(site), Site, {}, settings_.blockTrackers && !site.empty(), siteOn),
        MenuItem::item(L"Block ads and trackers everywhere", Everywhere, {}, true, settings_.blockTrackers),
        MenuItem::sep(),
        MenuItem::item(L"What's blocked", About),
    };
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_, cjui::MenuAnchor::TopRight);
    if (cmd == Site) {
        if (siteOn) settings_.blockerAllowed.push_back(site);
        else settings_.blockerAllowed.erase(std::remove(settings_.blockerAllowed.begin(), settings_.blockerAllowed.end(), site),
                                            settings_.blockerAllowed.end());
        settings_.save(settingsPath_);
        v->page->reload();
        flash(siteOn ? L"Ads and trackers aren't blocked on " + widen(site) + L" now." : L"Ads and trackers are blocked on " + widen(site) + L" again.", 5);
    } else if (cmd == Everywhere) {
        settings_.blockTrackers = !settings_.blockTrackers;
        settings_.save(settingsPath_);
        v->page->reload();
        flash(settings_.blockTrackers ? L"Ads and trackers are blocked again." : L"Ads and trackers aren't blocked anywhere now.", 5);
    } else if (cmd == About) {
        cjui::messageBox(hwnd(),
                         L"NorthRay Browser refuses what pages ask from " + std::to_wstring(trackerDomains().size()) +
                             L" well-known advertising, tracking and data-collection services, when they're on a different site from "
                             L"the page. A site's own content is never blocked.\n\nPages load faster and fewer companies follow you from "
                             L"site to site. If a page doesn't work properly, turn blocking off for that site with the shield in the "
                             L"address bar.",
                         L"Blocking ads and trackers", MB_OK | MB_ICONINFORMATION);
    }
    invalidate();
}

}  // namespace nrb

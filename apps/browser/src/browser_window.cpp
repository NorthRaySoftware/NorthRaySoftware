#include "browser_window.hpp"
#include "cjui/dialog.hpp"

#include "nrb/url.hpp"
#include "resource.h"
#include "version.hpp"

#include "cjui/menu.hpp"
#include "cjui/northray.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace nrb {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::Rect;
using cjui::TextOpts;
namespace nr = cjui::northray;

namespace {

constexpr float kStripH = 44;    // tab strip, including the aurora band
constexpr float kToolbarH = 48;
constexpr float kTabTop = 8;
constexpr float kNewTabW = 36;
constexpr float kCaptionW = 46;  // each of minimise, maximise and close
constexpr float kDragMin = 44;   // empty strip always left for dragging the window
constexpr UINT_PTR kSpinTimer = 1, kShutdownTimer = 2, kPreviewTimer = 7;
}  // namespace
extern const UINT_PTR kDownloadsTimer = 3;
namespace {
constexpr UINT WM_APP_ENGINE_EXITED = WM_APP + 10;
constexpr ULONG_PTR kCopyDataOpen = 0x4E524231;  // "NRB1": addresses from a second launch

enum Icon { IconBack, IconForward, IconReload, IconStop, IconHome, IconMenu };

// Menu command ids.
enum : UINT {
    CmdNewTab = 100, CmdReopen, CmdPrint, CmdDevTools, CmdAbout, CmdFullscreen,
    CmdThemeSystem = 200, CmdThemeLight, CmdThemeDark, CmdZoomBase = 900,
    CmdEngineFirst = 300,   // + index in builtInSearchEngines()
    CmdTabReload = 400, CmdTabDuplicate, CmdTabClose, CmdTabCloseOthers, CmdTabCloseRight, CmdTabMute,
    CmdPwSave = 500, CmdPwForms, CmdPwClear, CmdPwAbout,
    CmdWebsite = 590,
    CmdHistory = 600, CmdBackup, CmdRestore, CmdSideTabs, CmdDownloads, CmdBlock, CmdReferences, CmdPickColour, CmdDefault, CmdPopout,
};

COLORREF toColorRef(Color c) {
    auto b = [](float v) { return (BYTE)std::clamp((int)std::lround(v * 255), 0, 255); };
    return RGB(b(c.r), b(c.g), b(c.b));
}

// An arc from a0 to a1 (radians, clockwise from 3 o'clock) drawn as short
// lines, which renders the same on every Direct2D (some don't draw arc segments).
void arcLines(cjui::Gfx& g, float cx, float cy, float r, float a0, float a1, Color c, float w) {
    const int n = std::max(6, (int)std::ceil(std::abs(a1 - a0) / 0.18f));
    float px = cx + r * std::cos(a0), py = cy + r * std::sin(a0);
    for (int i = 1; i <= n; ++i) {
        const float a = a0 + (a1 - a0) * (float)i / (float)n;
        const float x = cx + r * std::cos(a), y = cy + r * std::sin(a);
        g.line(px, py, x, y, c, w);
        px = x;
        py = y;
    }
}

std::wstring hostLabel(const std::wstring& url) { return widen(hostOf(narrow(url))); }

constexpr UINT WM_APP_ADDRESS_MENU = WM_APP + 30;   // lParam: screen point (the address bar's right-click menu)

void putTextOnClipboard(HWND owner, const std::wstring& t) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t))) {
        std::copy_n(t.c_str(), t.size() + 1, static_cast<wchar_t*>(GlobalLock(g)));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

std::wstring clipboardText(HWND owner) {
    std::wstring out;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) return out;
    if (HANDLE g = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(g))) {
            out.assign(p, wcsnlen(p, 32 * 1024));
            GlobalUnlock(g);
        }
    }
    CloseClipboard();
    // One line: line breaks become spaces, and the ends are trimmed.
    for (auto& c : out) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    const auto a = out.find_first_not_of(L' '), b = out.find_last_not_of(L' ');
    return a == std::wstring::npos ? std::wstring() : out.substr(a, b - a + 1);
}

// Edit-control subclass: no beep on Enter or Escape (the window handles them).
LRESULT CALLBACK addressProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_CONTEXTMENU) {   // NorthRay's menu, with Paste and go (0.9)
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (pt.x == -1 && pt.y == -1) {
            RECT rc;
            GetWindowRect(h, &rc);
            pt = {rc.left + 10, rc.bottom};
        }
        SendMessageW(GetParent(h), WM_APP_ADDRESS_MENU, 0, MAKELPARAM((short)pt.x, (short)pt.y));
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == 1 /* Ctrl+A */)) {
        if (w == 1) SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return DefSubclassProc(h, m, w, l);
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

BrowserWindow::BrowserWindow(Engine& engine, Settings& settings, std::wstring settingsPath)
    : engine_(engine), settings_(settings), settingsPath_(std::move(settingsPath)) {
    all().push_back(this);
}

std::vector<BrowserWindow*>& BrowserWindow::all() {
    static std::vector<BrowserWindow*> windows;
    return windows;
}

namespace {
std::vector<std::unique_ptr<BrowserWindow>>& spawned() {
    static std::vector<std::unique_ptr<BrowserWindow>> list;
    return list;
}
}  // namespace

void BrowserWindow::reapClosed() {
    auto& list = spawned();
    list.erase(std::remove_if(list.begin(), list.end(), [](const std::unique_ptr<BrowserWindow>& w) { return w->destroyed_; }), list.end());
}

BrowserWindow::~BrowserWindow() {
    auto& a = all();
    a.erase(std::remove(a.begin(), a.end(), this), a.end());
    if (editFont_) DeleteObject(editFont_);
    if (editBrush_) DeleteObject(editBrush_);
}

bool BrowserWindow::create(HINSTANCE inst, int show, bool adopting) {
    cjui::WindowSpec spec;
    spec.instance = inst;
    spec.className = kClassName;
    spec.title = L"NorthRay Browser";
    spec.iconId = IDI_APP;
    spec.width = 1280;
    spec.height = 820;
    spec.minWidth = 640;
    spec.minHeight = 420;
    spec.show = show;
    spec.style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;   // never paint over the page
    spec.quitOnDestroy = false;   // the app ends with its last window (onDestroy)
    if (!createWindow(spec)) return false;
    // Recalculate the frame now that we handle WM_NCCALCSIZE: the caption goes, the tabs take its place.
    SetWindowPos(hwnd(), nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd(), nullptr, inst, nullptr);
    SetWindowSubclass(edit_, addressProc, 1, 0);
    SendMessageW(edit_, EM_SETLIMITTEXT, 32 * 1024, 0);
    const SearchEngine* se = findSearchEngine(settings_.searchEngine);
    const std::wstring cue = L"Search with " + widen((se ? *se : defaultSearchEngine()).name) + L" or type an address";
    SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));

    openBookmarkStore();
    openHistoryStore();
    openDownloadStore();
    openReferenceStore();
    {
        const SearchEngine* e = findSearchEngine(settings_.searchEngine);
        const std::wstring c = L"Search with " + widen((e ? *e : defaultSearchEngine()).name) + L" or type an address";
        startSearch_ = makeEdit(c.c_str());
    }
    pageSearch_ = makeEdit(L"Search bookmarks");
    renameEdit_ = makeEdit(nullptr);
    notesEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10, 10, hwnd(), nullptr, inst, nullptr);
    SendMessageW(notesEdit_, EM_SETLIMITTEXT, 64 * 1024, 0);
    SendMessageW(notesEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Notes to yourself. They're kept on this computer."));
    loadNotes();
    SetTimer(hwnd(), 4, 15000, nullptr);   // the start page's clock (and weather, every half hour)

    applyTheme();
    if (adopting) {
        engineOk_ = engine_.ready();
        return true;   // shown once its tab has arrived
    }
    if (!restoreSession()) newTab(L"", true, false);
    ShowWindow(hwnd(), show);
    UpdateWindow(hwnd());
    focusAddressBar();
    return true;
}

void BrowserWindow::engineStarted(bool ok, const std::wstring& problem) {
    engineOk_ = ok;
    engineFailed_ = !ok;
    engineProblem_ = problem;
    runtimeMissing_ = !ok && problem.find(L"isn't installed") != std::wstring::npos;
    if (ok) {
        for (TabView& v : views_)
            if (!v.pendingUrl.empty()) {
                const std::wstring u = std::move(v.pendingUrl);
                v.pendingUrl.clear();
                navigateTab(v, u);
            }
        for (const std::wstring& u : queued_) newTab(u, true, false);
        queued_.clear();
    }
    invalidate();
}

void BrowserWindow::openFromCommandLine(const std::vector<std::wstring>& args) {
    // Only web addresses and files: a link from another app must never run
    // script or open an internal page.
    for (const std::wstring& a : args) {
        const InputResult r = interpretInput(narrow(a), defaultSearchEngine());
        if (r.kind != InputKind::Url) continue;
        UrlParts p;
        if (!splitUrl(r.url, p) || (p.scheme != "http" && p.scheme != "https" && p.scheme != "file")) continue;
        TabView* v = activeView();
        if (v && v->showStart && !v->page && v->pendingUrl.empty()) navigateTab(*v, widen(r.url));
        else newTab(widen(r.url), true, false);
    }
    if (IsIconic(hwnd())) ShowWindow(hwnd(), SW_RESTORE);
    SetForegroundWindow(hwnd());
}

void BrowserWindow::onDestroy() {
    destroyed_ = true;
    {   // the last window: the app ends
        bool others = false;
        for (BrowserWindow* b : all())
            if (b != this && b->hwnd() && !b->destroyed_) others = true;
        if (!others) PostQuitMessage(0);
    }
    if (preview_) DestroyWindow(preview_);
    preview_ = nullptr;
    KillTimer(hwnd(), kSpinTimer);
    KillTimer(hwnd(), kShutdownTimer);
    popBackIn();
    views_.clear();
}

void BrowserWindow::applyTheme() {
    const bool dark = settings_.theme == Theme::Dark || (settings_.theme == Theme::System && cjui::systemPrefersDark());
    setCalmControls(true);   // the calm controls (UI pass, 2026-11)
    applyPalette(nr::palette(dark));
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush(toColorRef(pal_.sunken));
    if (notesBrush_) { DeleteObject(notesBrush_); notesBrush_ = nullptr; }
    for (TabView& v : views_)
        if (v.page) v.page->setBackground(toColorRef(pal_.paper));
    if (edit_) InvalidateRect(edit_, nullptr, TRUE);
}

void BrowserWindow::onSystemThemeChanged() {
    if (settings_.theme == Theme::System) applyTheme();
}

// ---------------------------------------------------------------- tabs

BrowserWindow::TabView* BrowserWindow::view(int id) {
    for (TabView& v : views_)
        if (v.id == id) return &v;
    return nullptr;
}

BrowserWindow::TabView* BrowserWindow::activeView() { return view(model_.activeId()); }

int BrowserWindow::newTab(const std::wstring& url, bool activate, bool afterActive, int openerId) {
    if (TabView* cur = activeView()) {   // keep what was being typed in the tab we're leaving
        cur->editing = editFocused_;
    }
    const int id = model_.add(narrow(url), activate, afterActive, openerId);
    TabView v;
    v.id = id;
    views_.push_back(std::move(v));
    if (!url.empty()) navigateTab(*view(id), url);
    if (activate) {
        updateAddressText();
        if (url.empty()) focusAddressBar();
    }
    updateTitle();
    invalidate();
    return id;
}

void BrowserWindow::closeTab(int id) {
    if (id == popTab_) popBackIn();
    TabView* v = view(id);
    if (!v) return;
    if (Tab* t = model_.find(id); t && v->page && !v->page->url().empty())
        t->url = narrow(v->page->url());   // remembered for Ctrl+Shift+T
    const bool wasActive = model_.activeId() == id;
    // A page still downloading is kept (out of sight) until its downloads finish.
    if (v->page && tabHasDownloads(id)) {
        v->page->show(false);
        parked_.push_back({id, std::move(v->page)});
    } else if (v->page) v->page->close();
    gfx_.forgetImage(L"fav:" + std::to_wstring(id) + L":" + std::to_wstring(v->favGen));
    model_.close(id);
    views_.erase(std::remove_if(views_.begin(), views_.end(), [id](const TabView& x) { return x.id == id; }), views_.end());
    if (model_.empty()) {   // the last tab: the window closes, as in other browsers
        PostMessageW(hwnd(), WM_CLOSE, 0, 0);
        return;
    }
    if (wasActive) {
        updateAddressText();
        if (TabView* a = activeView()) {
            if (a->showStart) focusAddressBar();
            else focusPage();
        }
    }
    if (previewTab_ == id) hideTabPreview();
    saveSession();
    updateTitle();
    invalidate();
}

void BrowserWindow::closeTabsToRight(int id) {
    std::vector<int> right;
    bool past = false;
    for (const Tab& t : model_.tabs()) {
        if (past) right.push_back(t.id);
        if (t.id == id) past = true;
    }
    if (std::find(right.begin(), right.end(), model_.activeId()) != right.end()) activateTab(id);
    for (int r : right) closeTab(r);
}

// ---------------------------------------------------------------- a tab into its own window (0.9)

void BrowserWindow::detachTab(int id, POINT screen) {
    TabView* v = view(id);
    const Tab* t = model_.find(id);
    if (!v || !t || model_.size() < 2) return;
    if (id == popTab_) { flash(L"Put the video back in its tab before moving the tab to another window.", 6); return; }
    if (tabHasDownloads(id)) { flash(L"This tab is still downloading. Move it once its downloads have finished.", 6); return; }
    auto owned = std::make_unique<BrowserWindow>(engine_, settings_, settingsPath_);
    BrowserWindow* nw = owned.get();
    if (!nw->create(GetModuleHandleW(nullptr), SW_SHOWNORMAL, true)) return;
    spawned().push_back(std::move(owned));
    // Where it was dropped, about the size of this one.
    RECT rc;
    GetWindowRect(hwnd(), &rc);
    const int w = std::max(640, (int)((rc.right - rc.left) * 0.8)), h = std::max(480, (int)((rc.bottom - rc.top) * 0.8));
    SetWindowPos(nw->hwnd(), nullptr, screen.x - w / 4, std::max(0L, screen.y - 20), w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    // Out of this window without closing its page, into the new one.
    TabView moved = std::move(*v);
    const Tab tab = *t;
    const bool wasActive = model_.activeId() == id;
    gfx_.forgetImage(L"fav:" + std::to_wstring(id) + L":" + std::to_wstring(moved.favGen));
    model_.close(id, false);
    views_.erase(std::remove_if(views_.begin(), views_.end(), [id](const TabView& x) { return x.id == id; }), views_.end());
    if (previewTab_ == id) hideTabPreview();
    if (wasActive) {
        updateAddressText();
        if (TabView* a = activeView()) {
            if (a->showStart) focusAddressBar();
            else focusPage();
        }
    }
    updateTitle();
    invalidate();
    nw->adoptTab(std::move(moved), tab);
    ShowWindow(nw->hwnd(), SW_SHOWNORMAL);
    SetForegroundWindow(nw->hwnd());
    saveSession();
}

void BrowserWindow::adoptTab(TabView&& v, const Tab& t) {
    const int id = model_.add(t.url, true, false);
    if (Tab* nt = model_.find(id)) {
        nt->title = t.title;
        nt->loading = t.loading;
    }
    v.id = id;
    v.snap.clear();
    if (v.page) {
        v.page->reparent(hwnd());
        v.page->setEvents(eventsFor(id));
    }
    views_.push_back(std::move(v));
    if (TabView* a = view(id); a && !a->deferredUrl.empty()) {   // restored but not opened yet: open it here
        const std::wstring u = std::move(a->deferredUrl);
        a->deferredUrl.clear();
        navigateTab(*a, u);
    }
    updateAddressText();
    updateTitle();
    if (TabView* a = activeView()) {
        if (a->showStart) focusAddressBar();
        else focusPage();
    }
    if (anyLoading()) SetTimer(hwnd(), kSpinTimer, 33, nullptr);
    invalidate();
}

// ---------------------------------------------------------------- last time's tabs (0.9)

void BrowserWindow::saveSession() {
    if (closing_) return;
    settings_.session.clear();
    settings_.sessionActive = 0;
    int i = 0;
    // Every open window's tabs, this one's first (they come back in one window).
    std::vector<BrowserWindow*> order{this};
    for (BrowserWindow* b : all())
        if (b != this && b->live()) order.push_back(b);
    for (BrowserWindow* b : order)
    for (const Tab& t : b->model_.tabs()) {
        const TabView* v = b->view(t.id);
        std::string url = t.url;
        if (v && !v->deferredUrl.empty()) url = narrow(v->deferredUrl);
        if (v && v->showStart) continue;   // start, bookmarks, history... pages aren't kept
        UrlParts p;
        if (url.empty() || !splitUrl(url, p) || (p.scheme != "http" && p.scheme != "https" && p.scheme != "file")) continue;
        if (b == this && t.id == model_.activeId()) settings_.sessionActive = i;
        settings_.session.push_back({url, t.title});
        ++i;
    }
    settings_.save(settingsPath_);
}

bool BrowserWindow::restoreSession() {
    if (!settings_.restoreTabs || settings_.session.empty()) return false;
    const auto list = settings_.session;
    const int active = std::clamp(settings_.sessionActive, 0, (int)list.size() - 1);
    int activeId = 0;
    for (int i = 0; i < (int)list.size(); ++i) {
        const int id = model_.add(list[(size_t)i].url, false, false);
        TabView v;
        v.id = id;
        v.showStart = false;
        v.deferredUrl = widen(list[(size_t)i].url);
        views_.push_back(std::move(v));
        if (Tab* t = model_.find(id)) t->title = list[(size_t)i].title;
        if (i == active) activeId = id;
    }
    if (!activeId) return false;
    model_.activate(activeId);
    if (TabView* v = view(activeId)) {
        const std::wstring u = std::move(v->deferredUrl);
        v->deferredUrl.clear();
        navigateTab(*v, u);
    }
    updateAddressText();
    updateTitle();
    return true;
}

// ---------------------------------------------------------------- each site's zoom (0.9)

void BrowserWindow::applySiteZoom(TabView& v) {
    if (!v.page) return;
    const std::string host = hostOf(narrow(v.page->url()));
    if (host.empty() || host == v.zoomHost) return;
    v.zoomHost = host;
    auto it = settings_.siteZoom.find(host);
    const double want = it != settings_.siteZoom.end() ? it->second : 1.0;
    if (std::fabs(v.page->zoom() - want) > 0.001) v.page->setZoom(want);
}

void BrowserWindow::closeOtherTabs(int id) {
    std::vector<int> others;
    for (const Tab& t : model_.tabs())
        if (t.id != id) others.push_back(t.id);
    activateTab(id);
    for (int o : others) closeTab(o);
}

void BrowserWindow::activateTab(int id) {
    if (model_.activeId() == id) return;
    if (TabView* cur = activeView()) {
        cur->editing = editFocused_;
        takeSnapshot(cur->id);   // for its hover preview
    }
    model_.activate(id);
    if (TabView* v = view(id); v && !v->deferredUrl.empty()) {   // restored from last time: open it now
        const std::wstring u = std::move(v->deferredUrl);
        v->deferredUrl.clear();
        navigateTab(*v, u);
    }
    updateAddressText();
    if (TabView* v = activeView()) {
        if (v->editing || (v->showStart && !v->page)) focusAddressBar();
        else focusPage();
    }
    updateTitle();
    invalidate();
}

void BrowserWindow::activateNeighbour(int delta) {
    const int n = (int)model_.size();
    if (n < 2) return;
    const int i = std::max(0, model_.indexOf(model_.activeId()));
    activateTab(model_.tabs()[(size_t)(((i + delta) % n + n) % n)].id);
}

void BrowserWindow::reopenClosedTab() {
    if (!model_.hasClosed()) return;
    const ClosedTab c = model_.popClosed();
    const int id = newTab(widen(c.url), true, false);
    model_.move(id, c.index);
}

void BrowserWindow::duplicateTab(int id) {
    TabView* v = view(id);
    if (!v || !v->page || v->page->url().empty()) return;
    model_.activate(id);
    newTab(v->page->url(), true, true, id);
}

Page* BrowserWindow::ensurePage(TabView& v, bool forNewWindow) {
    if (v.page) return v.page.get();
    if (!engineOk_) return nullptr;
    Page::Options o;
    o.background = toColorRef(pal_.paper);
    o.savePasswords = settings_.savePasswords;
    o.fillForms = settings_.fillForms;
    for (const std::string& f : blockerFilters()) o.blockFilters.push_back(widen(f));
    o.block = [this](const std::wstring& request, const std::wstring& page) {
        return blockedOn(page) && shouldBlock(narrow(request), narrow(page));
    };
    v.page = std::make_unique<Page>(engine_, hwnd(), o, eventsFor(v.id));
    v.page->show(false);
    if (!forNewWindow) v.page->create();
    return v.page.get();
}

PageEvents BrowserWindow::eventsFor(int id) {
    PageEvents e;
    e.ready = [this, id] {
        syncTabFromPage(id);
        if (clearPasswordsWhenReady_) {
            clearPasswordsWhenReady_ = false;
            if (TabView* v = view(id); v && v->page)
                v->page->clearSavedPasswords(true, [this](bool ok) {
                    flash(ok ? L"Saved passwords and form entries deleted." : L"The saved passwords couldn't be deleted.", 8);
                });
        }
        // A page that's just been created for the active tab takes the keyboard,
        // unless the person has gone back to the address bar.
        if (id == model_.activeId() && !editFocused_)
            if (TabView* v = view(id); v && !v->showStart && !v->error) v->page->focus();
        invalidate();
    };
    e.createFailed = [this, id](bool, const std::wstring& why) {
        if (TabView* v = view(id)) {
            v->error = LoadErrorText{"This tab couldn't be opened", narrow(why), true, false, false};
            v->showStart = false;
        }
        invalidate();
    };
    e.changed = [this, id] { syncTabFromPage(id); };
    e.navigationStarting = [this, id](const std::wstring& uri) {
        if (TabView* v = view(id)) {
            v->error.reset();
            v->crashed = false;
            // A new view starts at about:blank, and Home loads it to empty the
            // page; only real pages replace the start page.
            if (uri != L"about:blank") v->showStart = false;
        }
        if (!anyLoading()) SetTimer(hwnd(), kSpinTimer, 33, nullptr);
        invalidate();
    };
    e.loadFailed = [this, id](int failure, const std::wstring& uri) {
        TabView* v = view(id);
        if (!v) return;
        const auto f = (LoadFailure)failure;
        if (isSilentLoadFailure(f)) return;
        v->error = describeLoadFailure(f, narrow(hostLabel(uri)));
        v->errorUrl = uri;
        invalidate();
    };
    e.loaded = [this, id](const std::wstring& uri) {
        if (TabView* v = view(id)) {
            v->error.reset();
            applySiteZoom(*v);
        }
        recordVisit(id, uri);
        saveSession();
        invalidate();
    };
    e.downloadStarted = [this](const std::wstring& name) {
        flash(name.empty() ? L"Downloading. Ctrl+J shows your downloads." : L"Downloading " + name + L". Ctrl+J shows your downloads.", 6);
    };
    e.download = [this, id](std::shared_ptr<DownloadHandle> h) { downloadStarted(id, std::move(h)); };
    e.savePicture = [this](const std::wstring& picture, const std::wstring& page, const std::wstring& title) { savePicture(picture, page, title); };
    e.pickColour = [this] { startColourPick(); };
    e.popoutVideo = [this, id] { popOutVideo(id); };
    e.blocked = [this, id](int n) {
        if (TabView* v = view(id); v && v->blocked != n) {
            v->blocked = n;
            if (id == model_.activeId()) invalidate();
        }
    };
    e.statusText = [this, id](const std::wstring& s) {
        if (TabView* v = view(id)) v->status = s;
        invalidate();
    };
    e.favicon = [this, id](int w, int h, std::vector<unsigned char> rgba) {
        TabView* v = view(id);
        if (!v) return;
        gfx_.forgetImage(L"fav:" + std::to_wstring(id) + L":" + std::to_wstring(v->favGen));
        ++v->favGen;
        v->favW = w;
        v->favH = h;
        v->favicon = std::move(rgba);
        // Bookmarks of this site without an icon take it (0.9), so the bookmarks bar shows site icons once a
        // site has been visited, including bookmarks imported from another browser.
        if (bookmarks_ && v->page && !v->favicon.empty() && w > 0 && h > 0) {
            const std::string host = hostOf(narrow(v->page->url()));
            if (!host.empty())
                for (const Bookmark& b : bookmarks_->search(host, 50))
                    if (b.icon.empty() && hostOf(b.url) == host) bookmarks_->setIcon(b.id, w, h, v->favicon);
        }
        invalidate();
    };
    e.fullscreen = [this](bool on) { setFullscreen(on, true); };
    e.closeRequested = [this, id] { closeTab(id); };
    e.processFailed = [this, id](bool crashed) {
        TabView* v = view(id);
        if (!v) return;
        if (crashed) {
            v->crashed = true;
            v->error = LoadErrorText{"This page stopped working",
                                     "Its process ended unexpectedly. Reloading usually brings it back; anything typed into "
                                     "the page may have been lost.",
                                     true, false, false};
        } else {
            flash(L"The page isn't responding. Wait, or reload it with F5.", 8);
        }
        invalidate();
    };
    e.newWindow = [this, id](ICoreWebView2NewWindowRequestedEventArgs* args) {
        // A pop-up or a target=_blank link: a new tab right after its opener,
        // still connected to it (window.opener), so sign-in pop-ups work.
        model_.activate(id);
        const int nid = model_.add("", true, true, id);
        TabView nv;
        nv.id = nid;
        nv.showStart = false;
        views_.push_back(std::move(nv));
        TabView* v = view(nid);
        if (Page* p = ensurePage(*v, true)) p->createForNewWindow(args);
        updateAddressText();
        updateTitle();
        invalidate();
    };
    e.key = [this](UINT vk, bool ctrl, bool shift, bool alt) { return shortcut(vk, ctrl, shift, alt); };
    e.gotFocus = [this] { editFocused_ = false; invalidate(); };
    e.audio = [this] { invalidate(); };
    e.zoomed = [this, id](double z) {
        TabView* v = view(id);
        if (!v || !v->page) return;
        const std::string host = hostOf(narrow(v->page->url()));
        if (host.empty() || host != v->zoomHost) return;   // only once the site's own zoom is in place
        const bool same = std::fabs(z - 1.0) < 0.001;
        auto it = settings_.siteZoom.find(host);
        if (same ? it == settings_.siteZoom.end() : (it != settings_.siteZoom.end() && std::fabs(it->second - z) < 0.001)) return;
        if (same) settings_.siteZoom.erase(host);
        else settings_.siteZoom[host] = z;
        settings_.save(settingsPath_);
        flash(L"Zoom " + std::to_wstring((int)std::lround(z * 100)) + L"% for " + widen(host) + L". Ctrl+0 puts it back.", 3);
    };
    return e;
}

void BrowserWindow::syncTabFromPage(int id) {
    TabView* v = view(id);
    Tab* t = model_.find(id);
    if (!v || !t || !v->page) return;
    const std::wstring url = v->page->url();
    if (!v->showStart) {   // the start and bookmarks pages keep their own names
        t->url = narrow(url);
        const std::string title = narrow(v->page->title());
        // The title often arrives after the page opened: give it to the history visit too.
        if (title != t->title && history_ && v->visitId && v->visitUrl == t->url) history_->setTitle(v->visitId, title);
        t->title = title;
    }
    t->loading = v->page->loading();
    if (id == model_.activeId()) {
        if (!v->editing) updateAddressText();
        updateTitle();
    }
    if (anyLoading()) SetTimer(hwnd(), kSpinTimer, 33, nullptr);
    invalidate();
}

bool BrowserWindow::anyLoading() const {
    for (const Tab& t : model_.tabs())
        if (t.loading) return true;
    return false;
}

// ---------------------------------------------------------------- navigation

void BrowserWindow::navigateTab(TabView& v, const std::wstring& url) {
    if (v.bookmarksPage && v.id == model_.activeId()) finishRename(true);
    v.bookmarksPage = false;
    v.historyPage = false;
    v.downloadsPage = false;
    v.referencesPage = false;
    v.blocked = 0;
    v.error.reset();
    v.crashed = false;
    v.showStart = false;
    v.editing = false;
    if (Tab* t = model_.find(v.id)) {
        t->url = narrow(url);
        t->loading = true;
    }
    if (!engineOk_) {
        if (!engineFailed_) v.pendingUrl = url;   // the engine is still starting
        invalidate();
        return;
    }
    Page* p = ensurePage(v);
    if (!p) return;
    p->navigate(url);
    SetTimer(hwnd(), kSpinTimer, 33, nullptr);
    if (v.id == model_.activeId()) updateAddressText();
    invalidate();
}

void BrowserWindow::submitAddress() {
    TabView* v = activeView();
    if (!v) return;
    const int n = GetWindowTextLengthW(edit_);
    std::wstring text((size_t)n, L'\0');
    if (n) GetWindowTextW(edit_, text.data(), n + 1);
    submitText(text);
}

void BrowserWindow::submitText(const std::wstring& text) {
    TabView* v = activeView();
    if (!v) return;
    // NorthRay's own pages.
    std::wstring low = text;
    for (auto& ch : low) ch = (wchar_t)towlower(ch);
    while (!low.empty() && (low.back() == L' ' || low.back() == L'/')) low.pop_back();
    if (low == L"northray://bookmarks" || low == L"northray:bookmarks") { v->editing = false; showBookmarksPage(); return; }
    if (low == L"northray://history" || low == L"northray:history") { v->editing = false; showHistoryPage(); return; }
    if (low == L"northray://downloads" || low == L"northray:downloads") { v->editing = false; showDownloadsPage(); return; }
    if (low == L"northray://references" || low == L"northray:references") { v->editing = false; showReferencesPage(); return; }
    if (low == L"northray://start" || low == L"northray://newtab") { v->editing = false; goHome(); return; }
    const SearchEngine* se = findSearchEngine(settings_.searchEngine);
    const InputResult r = interpretInput(narrow(text), se ? *se : defaultSearchEngine());
    switch (r.kind) {
        case InputKind::Empty: return;
        case InputKind::Refused: flash(widen(r.reason), 10); return;
        case InputKind::Url:
        case InputKind::Search:
            v->lastTyped = text;
            navigateTab(*v, widen(r.url));
            focusPage();
            return;
    }
}

void BrowserWindow::goHome() {
    TabView* v = activeView();
    if (!v) return;
    if (v->page) v->page->navigate(L"about:blank");
    v->showStart = true;
    v->bookmarksPage = false;
    v->historyPage = false;
    v->downloadsPage = false;
    v->referencesPage = false;
    if (Tab* t = model_.find(v->id)) { t->title.clear(); t->url.clear(); }
    v->error.reset();
    updateAddressText();
    focusAddressBar();
    invalidate();
}

void BrowserWindow::stopOrReload() {
    TabView* v = activeView();
    if (!v) return;
    const Tab* t = model_.find(v->id);
    if (t && t->loading && v->page) { v->page->stop(); return; }
    if (v->error && !v->errorUrl.empty() && v->page) {
        v->error.reset();
        v->page->navigate(v->errorUrl);
        return;
    }
    if (v->page) v->page->reload();
}

// The address bar's right-click menu (0.9): the usual editing commands, and Paste and go.
void BrowserWindow::showAddressMenu(POINT screen) {
    if (!edit_) return;
    if (!editFocused_) focusAddressBar();
    DWORD a = 0, b = 0;
    SendMessageW(edit_, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    const bool sel = b > a, any = GetWindowTextLengthW(edit_) > 0;
    const std::wstring clip = clipboardText(hwnd());
    using cjui::MenuItem;
    enum : UINT { Undo = 1, Cut, Copy, Paste, PasteGo, Del, All };
    const std::vector<MenuItem> items = {
        MenuItem::item(L"Undo", Undo, L"Ctrl+Z", SendMessageW(edit_, EM_CANUNDO, 0, 0) != 0),
        MenuItem::sep(),
        MenuItem::item(L"Cut", Cut, L"Ctrl+X", sel),
        MenuItem::item(L"Copy", Copy, L"Ctrl+C", sel),
        MenuItem::item(L"Paste", Paste, L"Ctrl+V", !clip.empty()),
        MenuItem::item(L"Paste and go", PasteGo, {}, !clip.empty()),
        MenuItem::item(L"Delete", Del, {}, sel),
        MenuItem::sep(),
        MenuItem::item(L"Select all", All, L"Ctrl+A", any),
    };
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    switch (cmd) {
        case Undo: SendMessageW(edit_, EM_UNDO, 0, 0); break;
        case Cut: SendMessageW(edit_, WM_CUT, 0, 0); break;
        case Copy: SendMessageW(edit_, WM_COPY, 0, 0); break;
        case Paste: SendMessageW(edit_, WM_PASTE, 0, 0); break;
        case PasteGo: submitText(clip); break;
        case Del: SendMessageW(edit_, WM_CLEAR, 0, 0); break;
        case All: SendMessageW(edit_, EM_SETSEL, 0, -1); break;
        default: break;
    }
    if (cmd && cmd != PasteGo) SetFocus(edit_);
}

void BrowserWindow::focusAddressBar() {
    if (!edit_) return;
    if (!IsWindowVisible(edit_)) ShowWindow(edit_, SW_SHOWNA);   // it's hidden while the address is drawn (0.9)
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
    editFocused_ = true;
    invalidate();
}

void BrowserWindow::focusPage() {
    TabView* v = activeView();
    editFocused_ = false;
    if (v && v->page && v->page->ready() && !v->showStart && !v->error) v->page->focus();
    else SetFocus(hwnd());   // the page gets the keyboard once it exists (PageEvents::ready)
    invalidate();
}

void BrowserWindow::updateAddressText() {
    if (!edit_) return;
    TabView* v = activeView();
    std::wstring text;
    if (v) {
        if (v->editing) text = v->typed;
        else if (v->showStart) text = v->bookmarksPage ? L"northray://bookmarks" : v->historyPage ? L"northray://history" : v->downloadsPage ? L"northray://downloads" : v->referencesPage ? L"northray://references" : L"";
        else if (v->error && !v->errorUrl.empty()) text = v->errorUrl;
        else if (v->page && !v->page->url().empty()) text = editFocused_ ? v->page->url() : widen(displayUrl(narrow(v->page->url())));
        else if (const Tab* t = model_.find(v->id)) text = editFocused_ ? widen(t->url) : widen(displayUrl(t->url));
    }
    const int n = GetWindowTextLengthW(edit_);
    std::wstring cur((size_t)n, L'\0');
    if (n) GetWindowTextW(edit_, cur.data(), n + 1);
    if (cur != text) {
        settingText_ = true;   // not the person typing
        SetWindowTextW(edit_, text.c_str());
        settingText_ = false;
    }
}

void BrowserWindow::updateTitle() {
    std::wstring t = L"NorthRay Browser";
    if (const Tab* tab = model_.active())
        if (!tab->title.empty()) t = widen(tab->title) + L" – NorthRay Browser";
    if (t != windowTitle_) {
        windowTitle_ = t;
        SetWindowTextW(hwnd(), t.c_str());
    }
}

// ---------------------------------------------------------------- keyboard

bool BrowserWindow::shortcut(UINT vk, bool ctrl, bool shift, bool alt) {
    TabView* v = activeView();
    if (ctrl && !alt) {
        switch (vk) {
            case 'T': if (shift) reopenClosedTab(); else newTab(L"", true, false); return true;
            case 'W': case VK_F4: if (v) closeTab(v->id); return true;
            case 'L': focusAddressBar(); return true;
            case 'D': if (!shift) { starClicked(); return true; } break;
            case 'B': if (shift) { toggleBookmarksBar(); return true; } break;
            case 'O': if (shift) { showBookmarksPage(); return true; } break;
            case 'H': if (!shift) { showHistoryPage(); return true; } break;
            case 'J': if (!shift) { showDownloadsPage(); return true; } break;
            case 'R': if (v && v->page) v->page->reload(shift); return true;
            case VK_F5: if (v && v->page) v->page->reload(true); return true;
            case VK_TAB: activateNeighbour(shift ? -1 : 1); return true;
            case VK_NEXT: activateNeighbour(1); return true;
            case VK_PRIOR: activateNeighbour(-1); return true;
            default: break;
        }
        if (vk >= '1' && vk <= '9' && !shift) {
            const size_t i = vk == '9' ? model_.size() - 1 : (size_t)(vk - '1');
            if (i < model_.size()) activateTab(model_.tabs()[i].id);
            return true;
        }
        return false;
    }
    if (alt && !ctrl) {
        switch (vk) {
            case VK_LEFT: if (v && v->page) v->page->back(); return true;
            case VK_RIGHT: if (v && v->page) v->page->forward(); return true;
            case VK_HOME: goHome(); return true;
            case 'D': focusAddressBar(); return true;
            case 'P': if (popTab_) popBackIn(); else if (v) popOutVideo(v->id); return true;
            default: return false;
        }
    }
    if (!ctrl && !alt) {
        switch (vk) {
            case VK_F5: if (v && v->page) v->page->reload(shift); return true;
            case VK_F6: if (editFocused_) focusPage(); else focusAddressBar(); return true;
            case VK_F11: setFullscreen(!fullscreen_, false); return true;
            case VK_BROWSER_BACK: if (v && v->page) v->page->back(); return true;
            case VK_BROWSER_FORWARD: if (v && v->page) v->page->forward(); return true;
            case VK_BROWSER_REFRESH: if (v && v->page) v->page->reload(); return true;
            case VK_BROWSER_HOME: goHome(); return true;
            default: return false;
        }
    }
    return false;
}

bool BrowserWindow::onKeyDown(WPARAM vk, bool ctrl, bool shift) {
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (pick_.on && vk == VK_ESCAPE) { finishColourPick(false); return true; }
    if (shortcut((UINT)vk, ctrl, shift, alt)) return true;
    if (vk == VK_ESCAPE) {
        if (fullscreen_ && !pageFullscreen_) { setFullscreen(false, false); return true; }
        if (const Tab* t = model_.active(); t && t->loading) { stopOrReload(); return true; }
    }
    return false;
}

bool BrowserWindow::preTranslate(MSG& msg) {
    if (msg.message == WM_SYSKEYDOWN && (msg.hwnd == hwnd() || msg.hwnd == edit_)) {
        // Alt shortcuts arrive as system keys.
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (msg.wParam != VK_MENU && shortcut((UINT)msg.wParam, ctrl, shift, true)) return true;
        return false;
    }
    if (msg.message == WM_KEYDOWN && msg.hwnd && (msg.hwnd == startSearch_ || msg.hwnd == pageSearch_ || msg.hwnd == renameEdit_)) {
        if (msg.wParam == VK_RETURN) {
            if (msg.hwnd == startSearch_) {
                const int n = GetWindowTextLengthW(startSearch_);
                std::wstring t((size_t)n, L'\0');
                if (n) GetWindowTextW(startSearch_, t.data(), n + 1);
                SetWindowTextW(startSearch_, L"");
                submitText(t);
            } else if (msg.hwnd == renameEdit_) {
                finishRename(true);
            }
            return true;
        }
        if (msg.wParam == VK_ESCAPE) {
            if (msg.hwnd == renameEdit_) finishRename(false);
            else SetWindowTextW(msg.hwnd, L"");
            return true;
        }
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && (msg.wParam == 'T' || msg.wParam == 'W' || msg.wParam == 'L' || msg.wParam == VK_TAB))
            return shortcut((UINT)msg.wParam, ctrl, shift, false);
        return false;
    }
    if (msg.hwnd != edit_ || msg.message != WM_KEYDOWN) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (msg.wParam) {
        case VK_RETURN: submitAddress(); return true;
        case VK_ESCAPE:
            if (TabView* v = activeView()) { v->editing = false; v->typed.clear(); }
            updateAddressText();
            SendMessageW(edit_, EM_SETSEL, 0, -1);
            if (TabView* v = activeView(); v && v->page && !v->showStart) focusPage();
            return true;
        case VK_TAB:
            if (!ctrl) { focusPage(); return true; }
            break;
        default: break;
    }
    // Editing keys (Ctrl+C/V/X/Z/A, arrows) belong to the field; browser
    // shortcuts still work from it.
    if (ctrl && (msg.wParam == 'C' || msg.wParam == 'V' || msg.wParam == 'X' || msg.wParam == 'Z' || msg.wParam == 'A' ||
                 msg.wParam == 'Y' || msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT || msg.wParam == VK_BACK ||
                 msg.wParam == VK_DELETE || msg.wParam == VK_HOME || msg.wParam == VK_END))
        return false;
    return shortcut((UINT)msg.wParam, ctrl, shift, false);
}

// ---------------------------------------------------------------- messages

bool BrowserWindow::maximized() const { return IsZoomed(hwnd()) != FALSE; }

float BrowserWindow::frameBorder() const {
    const UINT dpi = GetDpiForWindow(hwnd());
    const int px = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    return (float)px / gfx_.dpiScale();
}

// Where the pointer is, as Windows asks: the strip's empty space moves the window,
// its top edge resizes it, and maximise is reported as such (Windows 11 shows its
// snap layouts there). Everything else is ordinary client area, handled by cjui.
LRESULT BrowserWindow::hitTestFrame(HWND h, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(h, &pt);
    const float s = gfx_.dpiScale();
    const float x = pt.x / s, y = pt.y / s;
    RECT rc;
    GetClientRect(h, &rc);
    const float W = rc.right / s;
    if (!maximized() && y < frameBorder()) {
        const float corner = frameBorder() * 2;
        if (x < corner) return HTTOPLEFT;
        if (x > W - corner) return HTTOPRIGHT;
        return HTTOP;
    }
    if (y >= kStripH) return HTCLIENT;
    if (maxButton_.contains(x, y)) return HTMAXBUTTON;
    if (!hitAt(x, y).empty()) return HTCLIENT;   // a tab, new tab, minimise or close
    return HTCAPTION;
}

LRESULT BrowserWindow::onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp && !fullscreen_) {
                // Keep Windows' sizing borders on the left, right and bottom; take the caption.
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                const LONG top = p->rgrc[0].top;
                DefWindowProcW(hwnd(), msg, wp, lp);
                // Maximised, the window hangs over the screen edge by its border: keep that much off the top.
                p->rgrc[0].top = maximized() ? top + (LONG)std::lround(frameBorder() * gfx_.dpiScale()) : top;
                handled = true;
                return 0;
            }
            break;
        case WM_NCHITTEST:
            if (!fullscreen_) {
                const LRESULT r = DefWindowProcW(hwnd(), msg, wp, lp);
                handled = true;
                if (r != HTCLIENT) return r;   // the side and bottom borders
                return hitTestFrame(hwnd(), lp);
            }
            break;
        case WM_NCMOUSEMOVE: {
            const bool over = wp == HTMAXBUTTON;
            if (over != maxHover_) { maxHover_ = over; invalidate(); }
            if (!trackingNc_) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE | TME_NONCLIENT, hwnd(), 0};
                trackingNc_ = TrackMouseEvent(&t) != FALSE;
            }
            break;
        }
        case WM_NCMOUSELEAVE:
            trackingNc_ = false;
            if (maxHover_ || maxPressed_) { maxHover_ = maxPressed_ = false; invalidate(); }
            break;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            if (wp == HTMAXBUTTON) {
                maxPressed_ = true;
                invalidate();
                handled = true;   // not Windows' own (it would draw an old-style button)
                return 0;
            }
            break;
        case WM_NCLBUTTONUP:
            if (wp == HTMAXBUTTON) {
                if (maxPressed_) ShowWindow(hwnd(), maximized() ? SW_RESTORE : SW_MAXIMIZE);
                maxPressed_ = false;
                invalidate();
                handled = true;
                return 0;
            }
            maxPressed_ = false;
            break;
        case WM_ACTIVATE:
            activeWindow_ = LOWORD(wp) != WA_INACTIVE;
            invalidate();
            break;
        case WM_COMMAND:
            if (lp && (HWND)lp == notesEdit_) {
                if (HIWORD(wp) == EN_CHANGE) SetTimer(hwnd(), 5, 1200, nullptr);   // saved after a pause
                if (HIWORD(wp) == EN_KILLFOCUS) { saveNotes(); notesActive_ = false; invalidate(); }
                handled = true;
                return 0;
            }
            if (lp && ((HWND)lp == startSearch_ || (HWND)lp == pageSearch_ || (HWND)lp == renameEdit_)) {
                const int code = HIWORD(wp);
                if ((HWND)lp == pageSearch_ && code == EN_CHANGE) {
                    const int n = GetWindowTextLengthW(pageSearch_);
                    std::wstring t((size_t)n, L'\0');
                    if (n) GetWindowTextW(pageSearch_, t.data(), n + 1);
                    pageQuery_ = t;
                    while (!pageQuery_.empty() && pageQuery_.back() == L' ') pageQuery_.pop_back();
                    resetScroll();
                }
                if ((HWND)lp == renameEdit_ && code == EN_KILLFOCUS) finishRename(true);
                if (code == EN_SETFOCUS || code == EN_KILLFOCUS || code == EN_CHANGE) invalidate();
                handled = true;
                return 0;
            }
            if ((HWND)lp == edit_ && edit_) {
                const int code = HIWORD(wp);
                if (code == EN_SETFOCUS) {
                    editFocused_ = true;
                    TabView* v = activeView();
                    if (v && !v->editing) updateAddressText();   // the full address while editing
                    PostMessageW(edit_, EM_SETSEL, 0, -1);
                    invalidate();
                } else if (code == EN_KILLFOCUS) {
                    editFocused_ = false;
                    if (TabView* v = activeView()) {
                        if (!v->editing) updateAddressText();
                    }
                    invalidate();
                } else if (code == EN_CHANGE && editFocused_ && !settingText_) {
                    if (TabView* v = activeView()) {
                        const int n = GetWindowTextLengthW(edit_);
                        std::wstring t((size_t)n, L'\0');
                        if (n) GetWindowTextW(edit_, t.data(), n + 1);
                        v->typed = t;
                        v->editing = true;
                    }
                }
                handled = true;
                return 0;
            }
            break;
        case WM_CTLCOLOREDIT:
            if ((HWND)lp == notesEdit_) {
                HDC dc = (HDC)wp;
                SetTextColor(dc, toColorRef(pal_.ink));
                SetBkColor(dc, toColorRef(pal_.surface));
                if (!notesBrush_) notesBrush_ = CreateSolidBrush(toColorRef(pal_.surface));
                handled = true;
                return (LRESULT)notesBrush_;
            }
            if ((HWND)lp == edit_ || (HWND)lp == startSearch_ || (HWND)lp == pageSearch_ || (HWND)lp == renameEdit_) {
                HDC dc = (HDC)wp;
                SetTextColor(dc, toColorRef(pal_.ink));
                SetBkColor(dc, toColorRef(pal_.sunken));
                handled = true;
                return (LRESULT)editBrush_;
            }
            break;
        case WM_MOVE:
        case WM_MOVING:
            for (TabView& v : views_)
                if (v.page) v.page->parentMoved();
            if (note_ && IsWindowVisible(note_)) updateNote();
            break;
        case WM_MBUTTONUP: {
            // Middle-click a tab to close it.
            const float s = gfx_.dpiScale();
            const float x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
            bool done = false;
            for (auto& [id, r] : tabRects_)
                if (r.contains(x, y)) { closeTab(id); done = true; break; }
            if (!done) bookmarkMiddleClick(x, y);
            handled = true;
            return 0;
        }
        case WM_XBUTTONUP: {
            TabView* v = activeView();
            if (v && v->page) {
                if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) v->page->back();
                else v->page->forward();
            }
            handled = true;
            return TRUE;
        }
        case WM_COPYDATA: {
            const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (cd && cd->dwData == kCopyDataOpen && cd->cbData % sizeof(wchar_t) == 0 && cd->cbData < 256 * 1024) {
                std::wstring all(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t));
                std::vector<std::wstring> args;
                size_t start = 0;
                for (size_t i = 0; i <= all.size(); ++i)
                    if (i == all.size() || all[i] == L'\n') {
                        if (i > start) args.push_back(all.substr(start, i - start));
                        start = i + 1;
                    }
                if (args.empty() && model_.size()) newTab(L"", true, false);
                openFromCommandLine(args);
                handled = true;
                return TRUE;
            }
            break;
        }
        case WM_SETFOCUS:
            // Coming back to the window: put the keyboard where it was.
            if (TabView* v = activeView(); v && !editFocused_ && v->page && !v->showStart && !v->error) v->page->focus();
            break;
        case WM_APP_ADDRESS_MENU: {
            handled = true;
            showAddressMenu({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        }
        case WM_CLOSE: {
            handled = true;
            if (closing_) return 0;
            if (settings_.askBeforeClosing && model_.size() >= 5 && IsWindowVisible(hwnd())) {
                const std::wstring n = std::to_wstring(model_.size());
                const std::wstring more = settings_.restoreTabs ? L"\n\nThey'll open again the next time you start NorthRay Browser." : L"";
                if (cjui::messageBox(hwnd(), L"Close the window and its " + n + L" tabs?" + more, L"NorthRay Browser", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                    return 0;
            }
            saveSession();
            hideTabPreview();
            closing_ = true;
            ShowWindow(hwnd(), SW_HIDE);
            for (TabView& v : views_)
                if (v.page) v.page->close();
            {   // Other windows still open: the engine stays for them.
                bool others = false;
                for (BrowserWindow* b : all())
                    if (b != this && b->live()) others = true;
                if (others) {
                    DestroyWindow(hwnd());
                    return 0;
                }
            }
            // Wait (briefly) for the engine's processes to finish, so none are
            // left behind; then go.
            const HWND h = hwnd();
            engine_.shutdown([h] { PostMessageW(h, WM_APP_ENGINE_EXITED, 0, 0); });
            SetTimer(hwnd(), kShutdownTimer, 4000, nullptr);
            return 0;
        }
        case WM_APP_ENGINE_EXITED:
            handled = true;
            DestroyWindow(hwnd());
            return 0;
        case WM_APP + 22:   // a fetch finished (browser_start.cpp)
            handled = true;
            fetched(reinterpret_cast<Fetch*>(lp));
            return 0;
        case WM_APP + 21:   // a picture for a reference board arrived (browser_references.cpp)
            handled = true;
            pictureSaved(reinterpret_cast<PictureJob*>(lp));
            return 0;
        default: break;
    }
    return 0;
}

void BrowserWindow::onTimer(UINT_PTR id) {
    if (id == kSpinTimer) {
        if (!anyLoading()) KillTimer(hwnd(), kSpinTimer);
        invalidate();
    } else if (id == 4) {
        if (TabView* v = activeView(); v && v->showStart && !v->bookmarksPage && !v->historyPage && !v->downloadsPage && !v->referencesPage) {
            refreshWeather(false);
            invalidate();
        }
    } else if (id == 5) {
        KillTimer(hwnd(), 5);
        saveNotes();
    } else if (id == kDownloadsTimer) {
        KillTimer(hwnd(), kDownloadsTimer);
        releaseParkedPages();
    } else if (id == kPreviewTimer) {
        POINT pt;
        GetCursorPos(&pt);
        int over = 0;
        if (WindowFromPoint(pt) == hwnd()) {
            ScreenToClient(hwnd(), &pt);
            const float s = gfx_.dpiScale();
            for (auto& [tid, r] : tabRects_)
                if (r.contains(pt.x / s, pt.y / s)) over = tid;
        }
        if (!over || over != hoverTab_) {
            KillTimer(hwnd(), kPreviewTimer);
            hoverTab_ = 0;
            hideTabPreview();
        } else if (!previewTab_ && (int)(GetTickCount() - hoverSince_) >= 650) {
            previewTab_ = over;
            updateTabPreview();
        }
    } else if (id == kShutdownTimer) {
        KillTimer(hwnd(), kShutdownTimer);
        DestroyWindow(hwnd());
    }
}

// ---------------------------------------------------------------- mouse

bool BrowserWindow::onMouseDown(float x, float y) {
    hideTabPreview();
    hoverSince_ = GetTickCount() + 100000;   // not again until the pointer moves to another tab
    if (pick_.on && pick_.shown.contains(x, y)) {   // the colour picker takes the click
        pick_.mx = x;
        pick_.my = y;
        finishColourPick(true);
        return true;
    }
    for (auto& [id, r] : tabRects_)
        if (r.contains(x, y) && hitAt(x, y).rfind("tabx:", 0) != 0 && hitAt(x, y).rfind("tabsnd:", 0) != 0) {
            dragTab_ = id;
            dragStartX_ = x;
            dragStartY_ = y;
            dragging_ = false;
            activateTab(id);
            return true;
        }
    return false;
}

void BrowserWindow::onMouseMove(float x, float y, bool leftDown) {
    if (!leftDown) {   // resting on a tab: its preview (0.9)
        int over = 0;
        for (auto& [id, r] : tabRects_)
            if (r.contains(x, y)) over = id;
        if (over != hoverTab_) {
            hoverTab_ = over;
            hoverSince_ = GetTickCount();
            if (over && previewTab_) {   // already showing one: follow at once
                previewTab_ = over;
                updateTabPreview();
            }
        }
        if (over) SetTimer(hwnd(), kPreviewTimer, 100, nullptr);
    }
    if (maxHover_) { maxHover_ = false; invalidate(); }
    if (pick_.on) { pick_.mx = x; pick_.my = y; invalidate(); }
    if (!dragTab_ || !leftDown) return;
    const bool side = sideW() > 0;
    if (!dragging_ && std::abs(side ? y - dragStartY_ : x - dragStartX_) < 6) return;
    dragging_ = true;
    // Drop position: the tab under the pointer (along the strip, or down the side list).
    for (size_t i = 0; i < tabRects_.size(); ++i) {
        const Rect& r = tabRects_[i].second;
        const bool over = side ? (y >= r.y && y < r.bottom()) : (x >= r.x && x < r.right());
        if (over && tabRects_[i].first != dragTab_) {
            const auto& tabs = model_.tabs();
            for (size_t k = 0; k < tabs.size(); ++k)
                if (tabs[k].id == tabRects_[i].first) { model_.move(dragTab_, k); break; }
            invalidate();
            break;
        }
    }
}

bool BrowserWindow::onMouseUp(float x, float y) {
    // A tab dragged well away from the tabs: it gets a window of its own (0.9).
    if (dragTab_ && dragging_ && model_.size() > 1 && !fullscreen_) {
        const bool side = sideW() > 0;
        RECT rc;
        GetClientRect(hwnd(), &rc);
        const float s = gfx_.dpiScale();
        const bool outside = x < 0 || y < 0 || x > rc.right / s || y > rc.bottom / s;
        const bool away = side ? (x > sideRect_.right() + 60) : (y > kStripH + 50 || y < -20);
        if (outside || away) {
            const int id = dragTab_;
            dragTab_ = 0;
            dragging_ = false;
            POINT pt{(LONG)std::lround(x * s), (LONG)std::lround(y * s)};
            ClientToScreen(hwnd(), &pt);
            detachTab(id, pt);
            return true;
        }
    }
    const bool was = dragTab_ != 0;
    dragTab_ = 0;
    dragging_ = false;
    if (was) invalidate();
    return was;
}

bool BrowserWindow::onRightClick(float x, float y) {
    if (pick_.on) { finishColourPick(false); return true; }
    if (bookmarkRightClick(x, y)) return true;
    for (auto& [id, r] : tabRects_)
        if (r.contains(x, y)) {
            const float s = gfx_.dpiScale();
            POINT pt{(LONG)(x * s), (LONG)(y * s)};
            ClientToScreen(hwnd(), &pt);
            showTabMenu(id, pt);
            return true;
        }
    return false;
}

// ---------------------------------------------------------------- menus

void BrowserWindow::showMenu() {
    TabView* v = activeView();
    const bool hasPage = v && v->page && v->page->ready() && !v->showStart;
    using cjui::MenuItem;
    const auto& engines = builtInSearchEngines();
    std::vector<MenuItem> searchItems;
    for (size_t i = 0; i < engines.size(); ++i)
        searchItems.push_back(MenuItem::item(widen(engines[i].name), CmdEngineFirst + (UINT)i, {}, true,
                                             engines[i].id == settings_.searchEngine));
    std::vector<std::int64_t> bookmarkIds;
    const std::vector<MenuItem> items = {
        MenuItem::item(L"New tab", CmdNewTab, L"Ctrl+T"),
        MenuItem::item(L"Reopen closed tab", CmdReopen, L"Ctrl+Shift+T", model_.hasClosed()),
        MenuItem::sep(),
        MenuItem::item(L"History", CmdHistory, L"Ctrl+H"),
        MenuItem::item(L"Downloads", CmdDownloads, L"Ctrl+J"),
        MenuItem::item(L"Reference boards", CmdReferences),
        MenuItem::item(L"Pick a colour from the page", CmdPickColour, {}, hasPage),
        MenuItem::sub(L"Bookmarks", bookmarksSubmenu(bookmarkIds)),
        MenuItem::sub(L"Back up and restore",
                      {MenuItem::item(L"Back up to a file\u2026", CmdBackup), MenuItem::item(L"Restore from a backup\u2026", CmdRestore)}),
        MenuItem::sub(L"Passwords and forms",
                      {MenuItem::item(L"Offer to save passwords", CmdPwSave, {}, true, settings_.savePasswords),
                       MenuItem::item(L"Fill in forms automatically", CmdPwForms, {}, true, settings_.fillForms),
                       MenuItem::sep(),
                       MenuItem::item(L"Delete saved passwords...", CmdPwClear),
                       MenuItem::item(L"How passwords are kept", CmdPwAbout)}),
        MenuItem::sep(),
        MenuItem::item(L"Print...", CmdPrint, L"Ctrl+P", hasPage),
        MenuItem::item(L"Full screen", CmdFullscreen, L"F11", true, fullscreen_),
        MenuItem::item(L"Pop out the video", CmdPopout, L"Alt+P", hasPage || popTab_ != 0, popTab_ != 0),
        MenuItem::item(L"Developer tools", CmdDevTools, L"F12", hasPage),
        MenuItem::sep(),
        MenuItem::sub(L"Search engine", searchItems),
        MenuItem::item(L"Tabs down the side", CmdSideTabs, {}, true, settings_.verticalTabs),
        MenuItem::item(L"Block ads and trackers", CmdBlock, {}, true, settings_.blockTrackers),
        MenuItem::sub(L"Interface size", [&] {
            // Suite 0.172: the browser's own buttons, tabs and menus; Ctrl +/- still zoom the web page.
            std::vector<MenuItem> z;
            const auto& steps = cjui::zoomSteps();
            for (size_t i = 0; i < steps.size(); ++i)
                z.push_back(MenuItem::item(std::to_wstring((int)std::lround(steps[i] * 100)) + L"%" + (steps[i] == 1.0f ? L" (default)" : L""),
                                           CmdZoomBase + (UINT)i, i == 5 ? L"Ctrl+Alt+0" : L"", true, std::fabs(steps[i] - interfaceZoom()) < 0.005f));
            return z;
        }()),
        MenuItem::sub(L"Theme", {MenuItem::item(L"As Windows", CmdThemeSystem, {}, true, settings_.theme == Theme::System),
                                 MenuItem::item(L"Light", CmdThemeLight, {}, true, settings_.theme == Theme::Light),
                                 MenuItem::item(L"Dark", CmdThemeDark, {}, true, settings_.theme == Theme::Dark)}),
        MenuItem::sep(),
        MenuItem::item(L"Make it the default browser\u2026", CmdDefault, {}, !isDefaultBrowser()),
        MenuItem::item(L"NorthRay Software's website", CmdWebsite),
        MenuItem::item(L"About NorthRay Browser", CmdAbout),
    };

    // Below the menu button, right-aligned.
    const float s = gfx_.dpiScale();
    RECT rc;
    GetClientRect(hwnd(), &rc);
    POINT pt{rc.right - (LONG)(10 * s), (LONG)((kStripH + kToolbarH - 6) * s)};
    ClientToScreen(hwnd(), &pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_, cjui::MenuAnchor::TopRight);
    const std::wstring& settingsPath = settingsPath_;
    if (cmd >= CmdZoomBase && cmd < CmdZoomBase + (UINT)cjui::zoomSteps().size()) { setInterfaceZoom(cjui::zoomSteps()[cmd - CmdZoomBase]); return; }
    if (cmd == CmdNewTab) newTab(L"", true, false);
    else if (cmd == CmdReopen) reopenClosedTab();
    else if (cmd == CmdHistory) showHistoryPage();
    else if (cmd == CmdDownloads) showDownloadsPage();
    else if (cmd == CmdReferences) showReferencesPage();
    else if (cmd == CmdDefault) makeDefaultBrowser();
    else if (cmd == CmdBackup) backUpToFile();
    else if (cmd == CmdRestore) restoreFromFile();
    else if (cmd == CmdPickColour) startColourPick();
    else if (cmd == CmdBlock) {
        settings_.blockTrackers = !settings_.blockTrackers;
        settings_.save(settingsPath_);
        if (TabView* a = activeView(); a && a->page && !a->showStart) a->page->reload();
        flash(settings_.blockTrackers ? L"Ads and trackers are blocked." : L"Ads and trackers aren't blocked now.", 5);
    }
    else if (cmd == CmdSideTabs) setVerticalTabs(!settings_.verticalTabs);
    else if (cmd == 1) starClicked();
    else if (cmd == 2) toggleBookmarksBar();
    else if (cmd == 3) importBookmarks();
    else if (cmd == 4) exportBookmarks();
    else if (cmd == 9 || cmd >= 10000) openFromMenuId(cmd, bookmarkIds, false);
    else if (cmd == CmdPwSave) setFormSaving(!settings_.savePasswords, settings_.fillForms);
    else if (cmd == CmdPwForms) setFormSaving(settings_.savePasswords, !settings_.fillForms);
    else if (cmd == CmdPwClear) clearSavedPasswords();
    else if (cmd == CmdPwAbout) explainPasswords();
    else if (cmd == CmdPrint && hasPage) v->page->print();
    else if (cmd == CmdDevTools && hasPage) v->page->openDevTools();
    else if (cmd == CmdFullscreen) setFullscreen(!fullscreen_, false);
    else if (cmd == CmdPopout) { if (popTab_) popBackIn(); else if (v) popOutVideo(v->id); }
    else if (cmd == CmdAbout) showAbout();
    else if (cmd == CmdWebsite) newTab(std::wstring(cjui::northray::kWebsite) + L"browser/", true, true);   // in a tab of its own
    else if (cmd >= CmdThemeSystem && cmd <= CmdThemeDark) {
        settings_.theme = (Theme)(cmd - CmdThemeSystem);
        settings_.save(settingsPath);
        applyTheme();
    } else if (cmd >= CmdEngineFirst && cmd < CmdEngineFirst + engines.size()) {
        settings_.searchEngine = engines[cmd - CmdEngineFirst].id;
        settings_.searchEngineChosen = true;
        settings_.save(settingsPath);
        const std::wstring cue = L"Search with " + widen(engines[cmd - CmdEngineFirst].name) + L" or type an address";
        SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
        flash(L"Searches now use " + widen(engines[cmd - CmdEngineFirst].name) + L".");
    }
    invalidate();
}

void BrowserWindow::showTabMenu(int id, POINT screen) {
    TabView* v = view(id);
    if (!v) return;
    const bool hasPage = v->page && !v->page->url().empty() && !v->showStart;
    using cjui::MenuItem;
    const std::vector<MenuItem> items = {
        MenuItem::item(L"Reload", CmdTabReload, {}, hasPage),
        MenuItem::item(L"Duplicate", CmdTabDuplicate, {}, hasPage),
        MenuItem::item(popTab_ == id ? L"Put the video back" : L"Pop out the video", CmdPopout, {}, hasPage),
        MenuItem::item(v->page && v->page->muted() ? L"Unmute tab" : L"Mute tab", CmdTabMute, {}, v->page != nullptr),
        MenuItem::sep(),
        MenuItem::item(L"Close tab", CmdTabClose, L"Ctrl+W"),
        MenuItem::item(L"Close other tabs", CmdTabCloseOthers, {}, model_.size() > 1),
        MenuItem::item(L"Close tabs to the right", CmdTabCloseRight, {}, model_.indexOf(id) + 1 < (int)model_.size()),
        MenuItem::sep(),
        MenuItem::item(L"Reopen closed tab", CmdReopen, L"Ctrl+Shift+T", model_.hasClosed()),
        MenuItem::sep(),
        MenuItem::item(settings_.verticalTabs ? L"Show tabs across the top" : L"Show tabs down the side", CmdSideTabs),
    };
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (cmd == CmdSideTabs) { setVerticalTabs(!settings_.verticalTabs); return; }
    if (cmd == CmdTabReload && hasPage) v->page->reload();
    else if (cmd == CmdTabDuplicate) duplicateTab(id);
    else if (cmd == CmdPopout) popOutVideo(id);
    else if (cmd == CmdTabClose) closeTab(id);
    else if (cmd == CmdTabCloseOthers) closeOtherTabs(id);
    else if (cmd == CmdTabCloseRight) closeTabsToRight(id);
    else if (cmd == CmdTabMute && v->page) v->page->setMuted(!v->page->muted());
    else if (cmd == CmdReopen) reopenClosedTab();
    invalidate();
}

// ---------------------------------------------------------------- tabs down the side

float BrowserWindow::sideW() const {
    if (!settings_.verticalTabs || fullscreen_) return 0;
    RECT rc;
    GetClientRect(hwnd(), &rc);
    const float W = rc.right / std::max(0.5f, const_cast<BrowserWindow*>(this)->gfx_.dpiScale());
    return W < 640 ? 0 : std::clamp(W * 0.18f, 200.0f, 260.0f);   // too narrow a window: back to the top
}

void BrowserWindow::setVerticalTabs(bool on) {
    settings_.verticalTabs = on;
    settings_.save(settingsPath_);
    sideScroll_ = 0;
    invalidate();
}

void BrowserWindow::drawSideTabs(const Rect& r) {
    nr::nightSky(gfx_, r, 0, 0.6f, 9);
    sideRect_ = r;
    gfx_.line(r.right() - 0.5f, r.y, r.right() - 0.5f, r.bottom(), nr::kNightInk.withAlpha(0.10f));
    const float pad = 8, rowH = 38;
    float y = r.y + pad;
    // New tab
    const Rect nb{r.x + pad, y, r.w - pad * 2, 34};
    addHit("tb:newtab", nb, [this] { newTab(L"", true, false); });
    if (float hv = hoverAmount("tb:newtab"); hv > 0.01f) gfx_.fillRounded(nb, 8, nr::kNightInk.withAlpha(0.10f * hv));
    {
        const float cx = nb.x + 20, cy = nb.y + nb.h / 2;
        gfx_.line(cx - 6, cy, cx + 6, cy, nr::kNightInk.withAlpha(0.85f), 1.6f);
        gfx_.line(cx, cy - 6, cx, cy + 6, nr::kNightInk.withAlpha(0.85f), 1.6f);
        gfx_.text(L"New tab", {nb.x + 36, nb.y, nb.w - 44, nb.h}, Font::Small, nr::kNightInk.withAlpha(0.85f), {Align::Left, false, true, true});
    }
    if (focused("tb:newtab")) focusRing(nb);
    y += 34 + 6;
    gfx_.line(r.x + pad + 4, y, r.right() - pad - 4, y, nr::kNightInk.withAlpha(0.12f));
    y += 6;

    // The tabs, scrolled with the wheel when there are more than fit.
    const Rect list{r.x, y, r.w, r.bottom() - y};
    const auto& tabs = model_.tabs();
    sideContentH_ = tabs.size() * (rowH + 2) + pad;
    sideScroll_ = std::clamp(sideScroll_, 0.0f, std::max(0.0f, sideContentH_ - list.h));
    // Keep the active tab in view after keyboard switching.
    for (size_t i = 0; i < tabs.size(); ++i)
        if (tabs[i].id == model_.activeId() && !dragging_) {
            const float ty = i * (rowH + 2);
            if (ty < sideScroll_) sideScroll_ = ty;
            else if (ty + rowH > sideScroll_ + list.h) sideScroll_ = ty + rowH - list.h;
        }
    gfx_.pushClip(list);
    setHitClip(list);
    drawingSide_ = true;
    float ty = list.y - sideScroll_;
    for (const Tab& t : tabs) {
        const Rect tr{r.x + pad, ty, r.w - pad * 2, rowH};
        if (tr.bottom() > list.y && tr.y < list.bottom()) {
            tabRects_.push_back({t.id, tr});
            drawTab(t, tr, t.id == model_.activeId());
        }
        ty += rowH + 2;
    }
    drawingSide_ = false;
    clearHitClip();
    gfx_.popClip();
    // A fade where the list runs on below.
    if (sideContentH_ - sideScroll_ > list.h + 1)
        gfx_.fillGradient({list.x, list.bottom() - 18, list.w - 1, 18}, 0, nr::kNightBottom.withAlpha(0), nr::kNightBottom, true);
}

bool BrowserWindow::onWheel(float x, float y, float deltaLines) {
    // Ctrl + wheel on a reference board: bigger or smaller pictures (0.9).
    if ((GetKeyState(VK_CONTROL) & 0x8000) && refs_ && deltaLines != 0)
        if (TabView* v = activeView(); v && v->showStart && v->referencesPage) {
            const std::string board = std::to_string(refBoard_);
            double z = settings_.refZoom.count(board) ? settings_.refZoom[board] : 1.0;
            z = std::clamp(z * (deltaLines > 0 ? 1.0 / 1.12 : 1.12), 0.5, 3.0);
            if (std::fabs(z - 1.0) < 0.03) settings_.refZoom.erase(board);
            else settings_.refZoom[board] = z;
            invalidate();
            return true;
        }
    // Over the tabs across the top: the wheel moves between them (0.9).
    if (sideW() <= 0 && !fullscreen_ && y < kStripH && deltaLines != 0) {
        activateNeighbour(deltaLines > 0 ? 1 : -1);   // down (towards you) is the next tab
        return true;
    }
    if (sideRect_.w > 0 && sideRect_.contains(x, y)) {
        sideScroll_ -= deltaLines * 40;
        invalidate();
        return true;
    }
    return false;
}

void BrowserWindow::showAbout() {
    std::wstring text = L"NorthRay Browser " CJ_VERSION_WSTR L" (preview)\n\n";
    text += engine_.version().empty() ? std::wstring(L"Engine: not running") : L"Engine: Microsoft Edge WebView2 Runtime " + engine_.version();
    text += L"\n\nDeveloped by the " CJ_DEVELOPER_WSTR L" project.\nNorthRaySoftware@protonmail.com\n\n"
            L"Web pages are shown by Microsoft's WebView2 (Chromium), which Windows keeps up to date. "
            L"Licences: chrome://credits (the engine's) and the licences folder next to NorthRay Browser.";
    cjui::messageBox(hwnd(), text.c_str(), L"About NorthRay Browser", MB_OK | MB_ICONINFORMATION);
}

void BrowserWindow::setFullscreen(bool on, bool fromPage) {
    if (on == fullscreen_) { if (!on) pageFullscreen_ = false; return; }
    fullscreen_ = on;
    pageFullscreen_ = on && fromPage;
    HWND h = hwnd();
    if (on) {
        GetWindowPlacement(h, &placement_);
        savedStyle_ = GetWindowLongPtrW(h, GWL_STYLE);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(h, GWL_STYLE, (savedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        if (!fromPage) flash(L"Full screen. Press F11 or Esc to leave.", 5);
    } else {
        SetWindowLongPtrW(h, GWL_STYLE, savedStyle_);
        SetWindowPlacement(h, &placement_);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    invalidate();
}

// ---------------------------------------------------------------- drawing

void BrowserWindow::drawFrame(float W, float H) {
    if (gfx_.dpiScale() != editScale_) layoutAddressFont();
    tabRects_.clear();
    gfx_.fill({0, 0, W, H}, pal_.paper);
    const bool chrome = !fullscreen_;
    const bool bar = chrome && settings_.showBookmarksBar && bookmarks_;
    const float top = chrome ? kStripH + kToolbarH + (bar ? kBookmarksBarH : 0) : 0;
    const float side = chrome ? sideW() : 0;
    // Interface size (suite 0.173): the browser's own pages (start, bookmarks, history...) are drawn at the zoom; the tabs,
    // toolbar and bookmarks bar keep their size, and a web page has its own zoom (Ctrl +/-).
    const Rect content = beginZoomArea({side, top, std::max(1.0f, W - side), std::max(1.0f, H - top)});

    TabView* v = activeView();
    const bool ownPage = v && v->showStart && (v->bookmarksPage || v->historyPage || v->downloadsPage || v->referencesPage);
    if (pick_.on) drawColourPick(content);
    else if (v && popTab_ && v->id == popTab_) drawPopoutPlaceholder(content);
    else if (engineFailed_ && !ownPage) drawEngineProblem(content);   // NorthRay's own pages work without the engine
    else if (v && v->error) drawErrorPage(content, *v);
    else if (v && v->showStart && v->bookmarksPage) drawBookmarksPage(content);
    else if (v && v->showStart && v->historyPage) drawHistoryPage(content);
    else if (v && v->showStart && v->downloadsPage) drawDownloadsPage(content);
    else if (v && v->showStart && v->referencesPage) drawReferencesPage(content);
    else if (v && v->showStart) drawStartPage(content);
    else if (v && (!v->page || !v->page->ready())) {
        gfx_.fill(content, pal_.paper);   // the page is on its way
    }
    if (!v || !v->showStart || v->error || engineFailed_) { tileRects_.clear(); rowRects_.clear(); }
    placePages(content);
    endZoomArea();

    if (chrome) {
        if (side > 0) drawSideTabs({0, top, side, H - top});
        else sideRect_ = {};
        drawTabStrip({0, 0, W, kStripH});
        drawToolbar({0, kStripH, W, kToolbarH});
        if (bar) drawBookmarksBar({0, kStripH + kToolbarH, W, kBookmarksBarH});
        else barRects_.clear();
    } else if (edit_ && IsWindowVisible(edit_)) {
        if (GetFocus() == edit_) SetFocus(hwnd());
        ShowWindow(edit_, SW_HIDE);
    }
    if (!chrome) startSearchVisible_ = pageSearchVisible_ = renameVisible_ = notesVisible_ = false;
    positionExtraEdits();
    updateNote();
}

void BrowserWindow::placePages(const Rect& contentIn) {
    const float s = gfx_.dpiScale();
    const Rect content = toWindow(contentIn);   // the whole page area in the window
    const RECT rc{(LONG)std::lround(content.x * s), (LONG)std::lround(content.y * s), (LONG)std::lround(content.right() * s),
                  (LONG)std::lround(content.bottom() * s)};
    TabView* a = activeView();
    for (TabView& v : views_) {
        if (!v.page || v.id == popTab_) continue;   // in the pop-out
        const bool show = &v == a && !v.showStart && !v.error && !engineFailed_ && !pick_.on;
        if (show) v.page->setBounds(rc);
        v.page->show(show);
    }
}

void BrowserWindow::drawTabStrip(const Rect& r) {
    nr::nightSky(gfx_, r, 0, 0.35f, 5);   // a quiet aurora: the tabs' words come first
    nr::auroraBand(gfx_, {r.x, r.y, r.w, 3}, pal_.dark);
    if (sideW() > 0) {
        // Tabs are down the side: the strip is the title bar, with the page's name in the middle.
        std::wstring title = windowTitle_.empty() ? L"NorthRay Browser" : windowTitle_;
        gfx_.text(title, {r.x + 16 + kCaptionW * 3, r.y + 4, r.w - 32 - kCaptionW * 6, r.h - 6}, Font::BodyStrong,
                  nr::kNightInk.withAlpha(activeWindow_ ? 1.0f : 0.6f), {Align::Center, false, true, true});
        drawCaptionButtons(r);
        return;
    }
    const auto& tabs = model_.tabs();
    const float buttons = kCaptionW * 3;
    const float x0 = r.x + 8, avail = r.w - 16 - kNewTabW - 8 - buttons - kDragMin;
    const float w = tabs.empty() ? 0 : std::clamp(avail / (float)tabs.size(), 40.0f, 232.0f);
    float x = x0;
    const int active = model_.activeId();
    for (size_t i = 0; i < tabs.size(); ++i) {
        const Rect tr{x, r.y + kTabTop, w, r.h - kTabTop};
        tabRects_.push_back({tabs[i].id, tr});
        // A thin divider between inactive neighbours, like a well-set row.
        if (i > 0 && tabs[i].id != active && tabs[i - 1].id != active)
            gfx_.line(x, tr.y + 9, x, tr.bottom() - 9, nr::kNightInk.withAlpha(0.14f));
        drawTab(tabs[i], tr, tabs[i].id == active);
        x += w;
        if (x > r.right() - buttons - kDragMin - kNewTabW - 16) break;   // the rest are reachable with Ctrl+Tab (overflow comes in 0.2)
    }
    // New tab
    const Rect nb{x + 6, r.y + kTabTop + 3, 28, 28};
    addHit("tb:newtab", nb, [this] { newTab(L"", true, false); });
    const float hv = hoverAmount("tb:newtab");
    if (hv > 0.01f) gfx_.fillRounded(nb, 8, nr::kNightInk.withAlpha(0.10f * hv));
    const float cx = nb.x + nb.w / 2, cy = nb.y + nb.h / 2;
    gfx_.line(cx - 6, cy, cx + 6, cy, nr::kNightInk.withAlpha(0.85f), 1.6f);
    gfx_.line(cx, cy - 6, cx, cy + 6, nr::kNightInk.withAlpha(0.85f), 1.6f);
    if (focused("tb:newtab")) focusRing(nb);
    drawCaptionButtons(r);
}

// Minimise, maximise (or restore) and close, in the night of the strip.
void BrowserWindow::drawCaptionButtons(const Rect& strip) {
    const float h = std::min(strip.h - 8, 36.0f);
    const float top = strip.y;
    const Color glyph = nr::kNightInk.withAlpha(activeWindow_ ? 0.92f : 0.5f);
    const float x0 = strip.right() - kCaptionW * 3;
    auto box = [&](int i) { return Rect{x0 + kCaptionW * i, top, kCaptionW, h}; };
    const float px = 1.0f / gfx_.dpiScale();   // one physical pixel
    auto snap = [&](float v) { return std::floor(v * gfx_.dpiScale()) / gfx_.dpiScale() + px / 2; };

    // Minimise
    const Rect mn = box(0);
    addHit("cap:min", mn, [this] { ShowWindow(hwnd(), SW_MINIMIZE); }, false);
    if (float hv = hoverAmount("cap:min"); hv > 0.01f) gfx_.fill(mn, nr::kNightInk.withAlpha((pressed("cap:min") ? 0.16f : 0.10f) * hv));
    {
        const float cx = mn.x + mn.w / 2, cy = snap(mn.y + mn.h / 2);
        gfx_.line(cx - 5, cy, cx + 5, cy, glyph, 1.0f);
    }
    // Maximise / restore: hit-tested by Windows (HTMAXBUTTON), so Windows 11 can show snap layouts.
    maxButton_ = box(1);
    {
        const Rect& mx = maxButton_;
        const float hv = anim("cap:max#hover", maxHover_ ? 1.0f : 0.0f, 18.0f);
        if (hv > 0.01f) gfx_.fill(mx, nr::kNightInk.withAlpha((maxPressed_ ? 0.16f : 0.10f) * hv));
        const float cx = snap(mx.x + mx.w / 2), cy = snap(mx.y + mx.h / 2);
        if (maximized()) {
            gfx_.strokeRounded({cx - 5, cy - 3, 8, 8}, 1, glyph, 1.0f);
            gfx_.line(cx - 3, cy - 5, cx + 4, cy - 5, glyph, 1.0f);
            gfx_.line(cx + 5, cy - 4, cx + 5, cy + 3, glyph, 1.0f);
        } else {
            gfx_.strokeRounded({cx - 5, cy - 5, 10, 10}, 1, glyph, 1.0f);
        }
    }
    // Close: Windows' red on hover.
    const Rect cl = box(2);
    addHit("cap:close", cl, [this] { PostMessageW(hwnd(), WM_CLOSE, 0, 0); }, false);
    {
        const float hv = hoverAmount("cap:close");
        if (hv > 0.01f) gfx_.fill(cl, Color{0.769f, 0.169f, 0.110f, 1}.withAlpha(hv * (pressed("cap:close") ? 0.85f : 1.0f)));
        const float cx = cl.x + cl.w / 2, cy = cl.y + cl.h / 2;
        const Color g = glyph.mix(Color{1, 1, 1, 1}, hv);
        gfx_.line(cx - 5, cy - 5, cx + 5, cy + 5, g, 1.1f);
        gfx_.line(cx + 5, cy - 5, cx - 5, cy + 5, g, 1.1f);
    }
}

void BrowserWindow::drawTab(const Tab& t, const Rect& r, bool active) {
    const std::string id = "tab:" + std::to_string(t.id);
    addHit(id, r, [this, tid = t.id] { activateTab(tid); });
    const float hv = hoverAmount(id);
    if (active) {
        gfx_.fillRounded(drawingSide_ ? r : Rect{r.x, r.y, r.w, r.h + 10}, 8, pal_.surface);
    } else if (hv > 0.01f) {
        gfx_.fillRounded({r.x + 2, r.y + 2, r.w - 4, r.h - 6}, 8, nr::kNightInk.withAlpha(0.08f * hv));
    }
    const Color ink = active ? pal_.ink : nr::kNightInk.withAlpha(0.8f + 0.2f * hv);
    const bool roomy = r.w >= 96;
    const bool showClose = active ? r.w >= 56 : (roomy && hv > 0.01f);
    float x = r.x + 12;
    const float cy = r.y + (r.h - 2) / 2;

    // Site icon, a spinner while loading, or a small globe.
    const Rect ic{x, cy - 8, 16, 16};
    TabView* v = view(t.id);
    if (t.loading) {
        const double secs = (double)GetTickCount64() / 1000.0;
        const float a0 = (float)std::fmod(secs * 6.0, 6.2831853);
        gfx_.circle(ic.x + 8, ic.y + 8, 6.5f, ink.withAlpha(0.18f), false, 2);
        arcLines(gfx_, ic.x + 8, ic.y + 8, 6.5f, a0, a0 + 1.9f, active ? pal_.ribbon : nr::kAuroraGreen, 2);
    } else if (v && !v->showStart && !v->favicon.empty() &&
               gfx_.drawPixels(L"fav:" + std::to_wstring(t.id) + L":" + std::to_wstring(v->favGen), v->favW, v->favH,
                               v->favicon.data(), ic, true)) {
    } else if (v && v->showStart && v->bookmarksPage) {
        drawStar(ic.x + 8, ic.y + 8, 7.5f, active ? pal_.ribbon : nr::kAuroraGreen, true);
    } else if (v && v->showStart && v->referencesPage) {
        // A small picture: a frame with a hill.
        const Color c = active ? pal_.ribbon : nr::kAuroraGreen;
        gfx_.strokeRounded({ic.x + 1.5f, ic.y + 2.5f, 13, 11}, 2, c, 1.5f);
        gfx_.line(ic.x + 3, ic.y + 12, ic.x + 7, ic.y + 7.5f, c, 1.4f);
        gfx_.line(ic.x + 7, ic.y + 7.5f, ic.x + 13, ic.y + 12, c, 1.4f);
        gfx_.circle(ic.x + 11, ic.y + 6, 1.4f, c, true);
    } else if (v && v->showStart && v->downloadsPage) {
        // An arrow into a tray.
        const Color c = active ? pal_.ribbon : nr::kAuroraGreen;
        gfx_.line(ic.x + 8, ic.y + 1.5f, ic.x + 8, ic.y + 10, c, 1.6f);
        gfx_.line(ic.x + 4.5f, ic.y + 6.5f, ic.x + 8, ic.y + 10, c, 1.6f);
        gfx_.line(ic.x + 11.5f, ic.y + 6.5f, ic.x + 8, ic.y + 10, c, 1.6f);
        gfx_.line(ic.x + 2, ic.y + 13.5f, ic.x + 14, ic.y + 13.5f, c, 1.6f);
    } else if (v && v->showStart && v->historyPage) {
        // A clock.
        const Color c = active ? pal_.ribbon : nr::kAuroraGreen;
        gfx_.circle(ic.x + 8, ic.y + 8, 6.5f, c, false, 1.5f);
        gfx_.line(ic.x + 8, ic.y + 8, ic.x + 8, ic.y + 4, c, 1.5f);
        gfx_.line(ic.x + 8, ic.y + 8, ic.x + 11, ic.y + 9.5f, c, 1.5f);
    } else if (v && v->showStart) {
        // The start page: a tiny NorthRay tile, night with a rising globe.
        gfx_.fillRounded(ic, 4, nr::kNightTop.mix(nr::kNightBottom, 0.5f));
        gfx_.pushClip(ic);
        gfx_.fill({ic.x, ic.y + 6, 16, 2}, nr::kAuroraTeal.withAlpha(0.8f));
        gfx_.circle(ic.x + 8, ic.y + 21, 11, nr::kAuroraGreen, false, 1.6f);
        gfx_.popClip();
        gfx_.circle(ic.x + 11.5f, ic.y + 3.5f, 1.1f, nr::kNightInk, true);
        if (!active) gfx_.strokeRounded(ic, 4, nr::kNightInk.withAlpha(0.25f), 1);
    } else {
        gfx_.circle(ic.x + 8, ic.y + 8, 6.5f, ink.withAlpha(0.7f), false, 1.2f);
        gfx_.line(ic.x + 1.5f, ic.y + 8, ic.x + 14.5f, ic.y + 8, ink.withAlpha(0.6f), 1);
        gfx_.curve(ic.x + 8, ic.y + 1.5f, ic.x + 4, ic.y + 5, ic.x + 4, ic.y + 11, ic.x + 8, ic.y + 14.5f, ink.withAlpha(0.6f), 1);
        gfx_.curve(ic.x + 8, ic.y + 1.5f, ic.x + 12, ic.y + 5, ic.x + 12, ic.y + 11, ic.x + 8, ic.y + 14.5f, ink.withAlpha(0.6f), 1);
    }
    x += 24;

    // Sound: a speaker while the page plays any (crossed out when muted); click it to mute (0.9).
    if (v && v->page && (v->page->playingAudio() || v->page->muted()) && r.w >= 72) {
        const std::string sid = "tabsnd:" + std::to_string(t.id);
        const Rect sb{x - 4, cy - 11, 24, 22};
        addHit(sid, sb, [this, tid = t.id] {
            if (TabView* tv = view(tid); tv && tv->page) tv->page->setMuted(!tv->page->muted());
        }, false);
        const float sh = hoverAmount(sid);
        if (sh > 0.01f) gfx_.fillRounded(sb, 5, (active ? pal_.ink : nr::kNightInk).withAlpha(0.10f * sh));
        const bool muted = v->page->muted();
        const Color c = muted ? ink.withAlpha(0.6f) : (active ? pal_.ribbon : nr::kAuroraGreen);
        const float sx = x, sy = cy;
        gfx_.fill({sx, sy - 2.5f, 3.5f, 5}, c);   // the box, then the cone
        gfx_.fillTriangle(sx + 3, sy - 2.5f, sx + 7.5f, sy - 6.5f, sx + 7.5f, sy + 6.5f, c);
        gfx_.fillTriangle(sx + 3, sy - 2.5f, sx + 7.5f, sy + 6.5f, sx + 3, sy + 2.5f, c);
        if (muted) {
            gfx_.line(sx + 10, sy - 3, sx + 15, sy + 3, c, 1.4f);
            gfx_.line(sx + 15, sy - 3, sx + 10, sy + 3, c, 1.4f);
        } else {
            arcLines(gfx_, sx + 7.5f, sy, 4.0f, -0.9f, 0.9f, c, 1.3f);
            arcLines(gfx_, sx + 7.5f, sy, 7.0f, -0.9f, 0.9f, c, 1.3f);
        }
        if (focused(sid)) focusRing(sb);
        x += 22;
    }

    // Title
    if (r.w >= 72) {
        std::wstring title = widen(t.title);
        if (v && v->showStart) title = v->bookmarksPage ? L"Bookmarks" : v->historyPage ? L"History" : v->downloadsPage ? L"Downloads" : v->referencesPage ? L"Reference boards" : L"New tab";
        if (title.empty() && v && v->error) title = hostLabel(v->errorUrl);
        if (title.empty()) title = widen(hostOf(t.url));
        if (title.empty()) title = t.loading ? L"Loading..." : L"New tab";
        const float tw = r.right() - x - (showClose ? 34 : 10);
        gfx_.text(title, {x, r.y, tw, r.h - 2}, active ? Font::SmallStrong : Font::Small, ink, {Align::Left, false, true, true});
    }

    // Close
    if (showClose) {
        const std::string xid = "tabx:" + std::to_string(t.id);
        // 24 DIPs square (0.9: easier to hit), the cross itself kept small.
        const Rect xb{r.right() - 32, cy - 12, 24, 24};
        addHit(xid, xb, [this, tid = t.id] { closeTab(tid); }, false);
        const float xh = hoverAmount(xid);
        if (xh > 0.01f) gfx_.fillRounded(xb, 6, (active ? pal_.ink : nr::kNightInk).withAlpha(0.12f * xh));
        const float c = xb.x + 12, m = xb.y + 12;
        gfx_.line(c - 4.5f, m - 4.5f, c + 4.5f, m + 4.5f, ink, 1.5f);
        gfx_.line(c + 4.5f, m - 4.5f, c - 4.5f, m + 4.5f, ink, 1.5f);
    }
    if (focused(id)) focusRing(r.inset(2));
}

void BrowserWindow::toolButton(const std::string& id, const Rect& r, int icon, bool enabled, std::function<void()> act,
                               const wchar_t*) {
    if (enabled) addHit(id, r, std::move(act));
    const float hv = enabled ? hoverAmount(id) : 0;
    if (hv > 0.01f) gfx_.fillRounded(r, 8, pal_.ink.withAlpha((pressed(id) ? 0.12f : 0.07f) * hv));
    const Color c = enabled ? pal_.inkMuted.mix(pal_.ink, hv) : pal_.inkFaint.withAlpha(0.6f);
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    switch (icon) {
        case IconBack:
        case IconForward: {
            const float d = icon == IconBack ? 1.0f : -1.0f;
            gfx_.line(cx - 7 * d, cy, cx + 7 * d, cy, c, 1.7f);
            gfx_.line(cx - 7 * d, cy, cx - 2 * d, cy - 5, c, 1.7f);
            gfx_.line(cx - 7 * d, cy, cx - 2 * d, cy + 5, c, 1.7f);
            break;
        }
        case IconReload:
            // A circular arrow: most of a circle, with its head at the top right.
            arcLines(gfx_, cx, cy, 6.5f, -0.75f, 4.3f, c, 1.7f);
            gfx_.fillTriangle(cx + 2.2f, cy - 9.8f, cx + 8.6f, cy - 6.2f, cx + 2.6f, cy - 2.4f, c);
            break;
        case IconStop:
            gfx_.line(cx - 6, cy - 6, cx + 6, cy + 6, c, 1.7f);
            gfx_.line(cx + 6, cy - 6, cx - 6, cy + 6, c, 1.7f);
            break;
        case IconHome:
            gfx_.line(cx - 8, cy - 0.5f, cx, cy - 7.5f, c, 1.6f);
            gfx_.line(cx, cy - 7.5f, cx + 8, cy - 0.5f, c, 1.6f);
            gfx_.line(cx - 5.5f, cy - 2.5f, cx - 5.5f, cy + 7, c, 1.6f);
            gfx_.line(cx + 5.5f, cy - 2.5f, cx + 5.5f, cy + 7, c, 1.6f);
            gfx_.line(cx - 5.5f, cy + 7, cx + 5.5f, cy + 7, c, 1.6f);
            break;
        case IconMenu:
            for (int i = -1; i <= 1; ++i) gfx_.circle(cx + i * 6.0f, cy, 1.6f, c, true);
            break;
        default: break;
    }
    if (enabled && focused(id)) focusRing(r);
}

void BrowserWindow::drawToolbar(const Rect& r) {
    gfx_.fill(r, pal_.surface);
    gfx_.line(r.x, r.bottom() - 0.5f, r.right(), r.bottom() - 0.5f, pal_.rule);
    TabView* v = activeView();
    Page* p = v && v->page && !v->showStart ? v->page.get() : nullptr;
    const Tab* t = model_.active();
    const bool loading = t && t->loading && p;
    const float by = r.y + 8, bs = 32;
    float x = r.x + 10;
    auto onPage = [this](void (Page::*fn)()) {
        return [this, fn] { if (TabView* a = activeView(); a && a->page) (a->page.get()->*fn)(); };
    };
    toolButton("tb:back", {x, by, bs, bs}, IconBack, p && p->canGoBack(), onPage(&Page::back), L"Back");
    x += bs + 2;
    toolButton("tb:fwd", {x, by, bs, bs}, IconForward, p && p->canGoForward(), onPage(&Page::forward), L"Forward");
    x += bs + 2;
    toolButton("tb:reload", {x, by, bs, bs}, loading ? IconStop : IconReload, p != nullptr || (v && v->error),
               [this] { stopOrReload(); }, loading ? L"Stop" : L"Reload");
    x += bs + 2;
    toolButton("tb:home", {x, by, bs, bs}, IconHome, true, [this] { goHome(); }, L"Start page");
    x += bs + 10;
    const float menuX = r.right() - 10 - bs;
    float barEnd = menuX - 10;
    if (downloadsSeen_) {
        drawDownloadsButton({menuX - 2 - bs, by, bs, bs});
        barEnd -= bs + 2;
    }
    drawAddressBar({x, by - 1, barEnd - x, bs + 2});
    toolButton("tb:menu", {menuX, by, bs, bs}, IconMenu, true, [this] { showMenu(); }, L"Menu");
}

void BrowserWindow::drawAddressBar(const Rect& r) {
    gfx_.fillRounded(r, 8, pal_.sunken);
    if (editFocused_) gfx_.strokeRounded(r, 8, pal_.focus, 1.5f);
    else gfx_.strokeRounded(r, 8, pal_.rule, 1);

    // The site indicator: what kind of connection this is, in words when it matters.
    TabView* v = activeView();
    float x = r.x + 10;
    if (v && !v->showStart && !editFocused_) {
        std::string url = v->error ? narrow(v->errorUrl) : v->page ? narrow(v->page->url()) : std::string();
        if (url.empty()) {
            if (const Tab* t = model_.find(v->id)) url = t->url;
        }
        SecurityInfo sec;
        if (v->error && v->error->isCertificate) { sec.known = true; sec.secureConnection = true; sec.certificateError = true; }
        // Nothing loaded: no padlock for a page that isn't there.
        const SiteKind kind = v->error && !v->error->isCertificate ? SiteKind::Blank : classifySite(url, sec);
        if (kind != SiteKind::Blank) {
            const std::wstring label = widen(siteLabel(kind));
            const bool warn = kind == SiteKind::Insecure || kind == SiteKind::CertProblem || kind == SiteKind::MixedContent;
            const Color c = warn ? (pal_.dark ? cjui::hexColor("#F0A35E") : cjui::hexColor("#A8540F")) : pal_.inkMuted;
            const float lw = label.empty() ? 0 : gfx_.measureWidth(label, Font::SmallStrong) + 8;
            const Rect chip{x - 4, r.y + 5, 24 + lw, r.h - 10};
            addHit("tb:site", chip, [this, kind] { flash(widen(siteExplanation(kind)), 12); });
            const float hv = hoverAmount("tb:site");
            if (hv > 0.01f) gfx_.fillRounded(chip, 6, pal_.ink.withAlpha(0.07f * hv));
            const float cx = x + 7, cy = r.y + r.h / 2;
            if (kind == SiteKind::Secure) {
                gfx_.fillRounded({cx - 5, cy - 1, 10, 8}, 2, c);   // padlock
                arcLines(gfx_, cx, cy - 1, 3.2f, 3.1416f, 6.2832f, c, 1.5f);
                gfx_.line(cx - 3.2f, cy - 1, cx - 3.2f, cy + 1, c, 1.5f);
                gfx_.line(cx + 3.2f, cy - 1, cx + 3.2f, cy + 1, c, 1.5f);
            } else if (warn) {
                gfx_.fillTriangle(cx, cy - 7, cx + 7, cy + 6, cx - 7, cy + 6, c);
                gfx_.line(cx, cy - 2, cx, cy + 2, pal_.sunken, 1.6f);
                gfx_.circle(cx, cy + 4, 0.9f, pal_.sunken, true);
            } else {
                gfx_.circle(cx, cy, 6.5f, c, false, 1.4f);   // information
                gfx_.line(cx, cy - 1, cx, cy + 3.5f, c, 1.6f);
                gfx_.circle(cx, cy - 3.5f, 0.9f, c, true);
            }
            if (!label.empty())
                gfx_.text(label, {x + 18, r.y, lw, r.h}, Font::SmallStrong, c, {Align::Left, false, false, true});
            x += 22 + lw;
            gfx_.line(x, r.y + 9, x, r.bottom() - 9, pal_.rule);
            x += 8;
        }
    }
    // The star: bookmark this page, or (filled) it's bookmarked.
    float right = r.right() - 10;
    if (bookmarks_ && currentBookmarkable()) {
        std::string url;
        currentBookmarkable(&url);
        const bool marked = bookmarks_->findUrl(url).has_value();
        const Rect sr{r.right() - 34, r.y + 3, 30, r.h - 6};
        addHit("tb:star", sr, [this] { starClicked(); });
        const float hv = hoverAmount("tb:star");
        if (hv > 0.01f) gfx_.fillRounded(sr, 6, pal_.ink.withAlpha(0.07f * hv));
        drawStar(sr.x + sr.w / 2, sr.y + sr.h / 2 + 0.5f, 7.5f, marked ? pal_.ribbon : pal_.inkMuted.mix(pal_.ink, hv), marked);
        if (focused("tb:star")) focusRing(sr);
        right = sr.x - 4;
    }
    // The shield: ads and trackers blocked on this page (or blocking turned off for the site).
    if (v && !v->showStart && !v->error && v->page && !editFocused_) {
        const std::wstring pageUrl = v->page->url();
        const bool on = blockedOn(pageUrl);
        if (v->blocked > 0 || !on) {
            const std::wstring n = on ? std::to_wstring(v->blocked) : std::wstring();
            const float nw = n.empty() ? 0 : gfx_.measureWidth(n, Font::SmallStrong) + 4;
            const Rect br{right - 30 - nw, r.y + 3, 30 + nw, r.h - 6};
            addHit("tb:shield", br, [this, br] {
                const float s = gfx_.dpiScale();
                POINT pt{(LONG)std::lround(br.right() * s), (LONG)std::lround((br.bottom() + 6) * s)};
                ClientToScreen(hwnd(), &pt);
                blockerMenu(pt);
            });
            const float hv = hoverAmount("tb:shield");
            if (hv > 0.01f) gfx_.fillRounded(br, 6, pal_.ink.withAlpha(0.07f * hv));
            const Color c = on ? pal_.ribbon : pal_.inkFaint;
            const float cx = br.x + 15, cy = br.y + br.h / 2;
            // A shield outline.
            gfx_.line(cx - 6, cy - 6, cx, cy - 8, c, 1.5f);
            gfx_.line(cx + 6, cy - 6, cx, cy - 8, c, 1.5f);
            gfx_.line(cx - 6, cy - 6, cx - 6, cy, c, 1.5f);
            gfx_.line(cx + 6, cy - 6, cx + 6, cy, c, 1.5f);
            gfx_.curve(cx - 6, cy, cx - 6, cy + 4, cx - 3, cy + 7, cx, cy + 8.5f, c, 1.5f);
            gfx_.curve(cx + 6, cy, cx + 6, cy + 4, cx + 3, cy + 7, cx, cy + 8.5f, c, 1.5f);
            if (!on) gfx_.line(cx - 7, cy + 7, cx + 7, cy - 8, c, 1.5f);
            if (!n.empty()) gfx_.text(n, {br.x + 25, br.y, nw, br.h}, Font::SmallStrong, pal_.inkMuted, {Align::Left, false, false, true});
            if (focused("tb:shield")) focusRing(br);
            right = br.x - 2;
        }
    }
    // Copy link (0.9): the page's whole address on the clipboard.
    if (v && !v->showStart && !v->error && v->page && !v->page->url().empty() && !editFocused_) {
        const Rect cr{right - 30, r.y + 3, 30, r.h - 6};
        addHit("tb:copylink", cr, [this] {
            if (TabView* tv = activeView(); tv && tv->page) {
                putTextOnClipboard(hwnd(), tv->page->url());
                flash(L"Link copied.", 3);
            }
        });
        const float hv = hoverAmount("tb:copylink");
        if (hv > 0.01f) gfx_.fillRounded(cr, 6, pal_.ink.withAlpha(0.07f * hv));
        const Color c = pal_.inkMuted.mix(pal_.ink, hv);
        // Two links of a chain, on a slant.
        const float cx = cr.x + 15, cy = cr.y + cr.h / 2;
        auto link = [&](float ox, float oy) {
            const float ax = cx + ox - 3.5f, ay = cy + oy + 3.5f, bx = cx + ox + 3.5f, by = cy + oy - 3.5f;
            gfx_.line(ax, ay, bx, by, c, 5.5f);
            gfx_.line(ax, ay, bx, by, pal_.sunken, 2.5f);
        };
        link(-2.5f, 2.5f);
        link(2.5f, -2.5f);
        if (focused("tb:copylink")) focusRing(cr);
        right = cr.x - 2;
    }
    // Not being edited: the address is drawn, the site's own name standing out and the rest fainter (0.9).
    // A click on it gives the edit box back.
    std::wstring shown;
    if (v && !v->showStart && !editFocused_ && !v->editing && edit_) {
        const int n = GetWindowTextLengthW(edit_);
        shown.assign((size_t)n, L'\0');
        if (n) GetWindowTextW(edit_, shown.data(), n + 1);
    }
    if (!shown.empty()) {
        {   // kept in place (hidden), so it appears exactly there when clicked
            const float s = gfx_.dpiScale();
            const RECT rc{(LONG)std::lround(x * s), (LONG)std::lround((r.y + (r.h - 20) / 2) * s), (LONG)std::lround(right * s),
                          (LONG)std::lround((r.y + (r.h + 20) / 2) * s)};
            if (memcmp(&rc, &editRect_, sizeof rc) != 0 || IsWindowVisible(edit_)) {
                editRect_ = rc;
                SetWindowPos(edit_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
            }
        }
        const Rect ar{x, r.y, right - x, r.h};
        addHit("tb:address", ar, [this] { focusAddressBar(); }, false);
        std::string url = v->error ? narrow(v->errorUrl) : v->page ? narrow(v->page->url()) : std::string();
        if (url.empty()) if (const Tab* t = model_.find(v->id)) url = t->url;
        const std::wstring host = widen(hostOf(url));
        std::size_t at = host.empty() ? std::wstring::npos : shown.find(host);
        if (at == std::wstring::npos && host.rfind(L"www.", 0) == 0) {
            at = shown.find(host.substr(4));
        }
        const std::wstring hostShown = at == std::wstring::npos ? std::wstring() : (shown.find(host) == at ? host : host.substr(4));
        gfx_.pushClip(ar);
        float tx = x + 1;
        auto part = [&](const std::wstring& t, bool strong) {
            if (t.empty()) return;
            gfx_.text(t, {tx, r.y, 4000, r.h}, Font::Body, strong ? pal_.ink : pal_.inkFaint, {Align::Left, false, false, true});
            tx += gfx_.measureWidth(t, Font::Body);
        };
        if (at == std::wstring::npos) part(shown, true);
        else {
            part(shown.substr(0, at), false);
            part(hostShown, true);
            part(shown.substr(at + hostShown.size()), false);
        }
        gfx_.popClip();
        return;
    }
    // The edit control sits inside the drawn frame.
    const float s = gfx_.dpiScale();
    const float th = 20;
    const RECT rc{(LONG)std::lround(x * s), (LONG)std::lround((r.y + (r.h - th) / 2) * s), (LONG)std::lround(right * s),
                  (LONG)std::lround((r.y + (r.h + th) / 2) * s)};
    if (memcmp(&rc, &editRect_, sizeof rc) != 0 || !IsWindowVisible(edit_)) {
        editRect_ = rc;
        SetWindowPos(edit_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// The interface zoom changed (suite 0.172): the address box's font now; the pages are placed again as it's drawn.
void BrowserWindow::onInterfaceZoomChanged() { layoutAddressFont(); }

void BrowserWindow::layoutAddressFont() {
    editScale_ = gfx_.dpiScale();
    if (editFont_) DeleteObject(editFont_);
    editFont_ = CreateFontW(-(int)std::lround(14 * editScale_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (edit_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
}

void BrowserWindow::drawErrorPage(const Rect& r, TabView& v) {
    gfx_.fill(r, pal_.paper);
    const float w = std::min(r.w - 80, 620.0f);
    const float x = r.x + (r.w - w) / 2;
    float y = r.y + std::max(40.0f, r.h * 0.16f);
    const LoadErrorText& e = *v.error;
    gfx_.text(v.crashed ? L"PAGE STOPPED" : e.isCertificate ? L"NOT OPENED, FOR YOUR SAFETY" : L"COULDN'T OPEN THE PAGE",
              {x, y, w, 16}, Font::Label, e.isCertificate ? (pal_.dark ? cjui::hexColor("#F0A35E") : cjui::hexColor("#A8540F")) : pal_.ribbon,
              {Align::Left, false});
    y += 22;
    y += gfx_.text(widen(e.title), {x, y, w, 80}, Font::Title, pal_.ink) + 10;
    if (!e.detail.empty()) y += gfx_.text(widen(e.detail), {x, y, w, 200}, Font::Body, pal_.inkMuted) + 10;
    if (!v.errorUrl.empty())
        y += gfx_.text(v.errorUrl, {x, y, w, 20}, Font::Small, pal_.inkFaint, {Align::Left, false, true}) + 18;
    else
        y += 8;
    float bx = x;
    if (e.canRetry) {
        const std::wstring label = v.crashed ? L"Reload" : L"Try again";
        const float bw = gfx_.measureWidth(label, Font::BodyStrong) + 40;
        button("tb:retry", {bx, y, bw, 36}, label, [this] { stopOrReload(); }, true);
        bx += bw + 10;
    }
    if (e.offerSearch && !v.lastTyped.empty()) {
        const std::wstring label = L"Search for “" + v.lastTyped + L"”";
        const float bw = std::min(gfx_.measureWidth(label, Font::Body) + 36, x + w - bx);
        button("tb:search", {bx, y, bw, 36}, label, [this, id = v.id] {
            TabView* t = view(id);
            if (!t) return;
            const SearchEngine* se = findSearchEngine(settings_.searchEngine);
            navigateTab(*t, widen(searchUrl(narrow(t->lastTyped), se ? *se : defaultSearchEngine())));
        });
    }
}

void BrowserWindow::drawEngineProblem(const Rect& r) {
    gfx_.fill(r, pal_.paper);
    const float w = std::min(r.w - 80, 620.0f);
    const float x = r.x + (r.w - w) / 2;
    float y = r.y + std::max(40.0f, r.h * 0.16f);
    gfx_.text(L"BROWSER ENGINE", {x, y, w, 16}, Font::Label, pal_.ribbon, {Align::Left, false});
    y += 22;
    y += gfx_.text(L"Web pages can't be shown", {x, y, w, 40}, Font::Title, pal_.ink) + 10;
    y += gfx_.text(engineProblem_, {x, y, w, 200}, Font::Body, pal_.inkMuted) + 22;
    if (runtimeMissing_) {
        const std::wstring label = L"Get the WebView2 Runtime from Microsoft";
        button("tb:runtime", {x, y, gfx_.measureWidth(label, Font::BodyStrong) + 40, 36}, label, [] {
            ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/microsoft-edge/webview2/", nullptr, nullptr, SW_SHOWNORMAL);
        }, true);
    }
}

// ------------------------------------------------------------------ the note

namespace {
LRESULT CALLBACK noteProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;   // clicks go through to the page
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        const bool dark = GetWindowLongPtrW(h, GWLP_USERDATA) != 0;
        HBRUSH bg = CreateSolidBrush(dark ? RGB(0x3A, 0x3D, 0x46) : RGB(0x26, 0x2A, 0x33));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        const int len = GetWindowTextLengthW(h);
        std::wstring t((size_t)len, L'\0');
        if (len) GetWindowTextW(h, t.data(), len + 1);
        const UINT dpi = GetDpiForWindow(h);
        HFONT f = CreateFontW(-MulDiv(14, (int)dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0xF2, 0xF4, 0xF8));
        DrawTextW(dc, t.c_str(), (int)t.size(), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, old);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

// Shows the flash message (if any) as a small rounded note near the bottom of the window, fading with it.
void BrowserWindow::updateNote() {
    const std::wstring& t = flashText();
    const float a = t.empty() ? 0.0f : flashColor(Color{1, 1, 1, 1}).a;
    if (a <= 0.01f || IsIconic(hwnd()) || !IsWindowVisible(hwnd())) {
        if (note_ && IsWindowVisible(note_)) ShowWindow(note_, SW_HIDE);
        return;
    }
    if (!note_) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = noteProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"NorthRayBrowserNote";
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            registered = true;
        }
        note_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"NorthRayBrowserNote", L"",
                                WS_POPUP, 0, 0, 10, 10, hwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!note_) return;
    }
    SetWindowLongPtrW(note_, GWLP_USERDATA, pal_.dark ? 1 : 0);
    const float s = gfx_.dpiScale();
    const float w = std::min(gfx_.measureWidth(t, Font::Body) + 40, 620.0f), h = 40;
    RECT rc;
    GetClientRect(hwnd(), &rc);
    POINT origin{0, 0};
    ClientToScreen(hwnd(), &origin);
    const int pw = (int)std::lround(w * s), ph = (int)std::lround(h * s);
    const int x = origin.x + (rc.right - pw) / 2;
    const int y = origin.y + rc.bottom - ph - (int)std::lround((28 - (1 - a) * 8) * s);
    if (t != noteText_) {
        noteText_ = t;
        SetWindowTextW(note_, t.c_str());
        InvalidateRect(note_, nullptr, TRUE);
    }
    SetLayeredWindowAttributes(note_, 0, (BYTE)std::lround(245 * a), LWA_ALPHA);
    SetWindowPos(note_, HWND_TOP, x, y, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HRGN rgn = CreateRoundRectRgn(0, 0, pw + 1, ph + 1, (int)(20 * s), (int)(20 * s));
    SetWindowRgn(note_, rgn, TRUE);   // the window owns the region now
}


// ---------------------------------------------------------------- tab hover preview (0.9)

// How the page looks as its tab is left, made small (at most 320 pixels wide), kept as BGRA for GDI.
void BrowserWindow::takeSnapshot(int id) {
    TabView* v = view(id);
    if (!v || !v->page || v->showStart || v->error || !v->page->ready()) return;
    v->page->capture([this, id](int w, int h, std::vector<unsigned char> rgba) {
        TabView* tv = view(id);
        if (!tv || w <= 0 || h <= 0 || rgba.size() < (size_t)w * h * 4) return;
        const int tw = std::min(320, w), th = std::max(1, (int)std::lround((double)h * tw / w));
        std::vector<unsigned char> out((size_t)tw * th * 4);
        for (int y = 0; y < th; ++y) {
            const int y0 = y * h / th, y1 = std::max(y0 + 1, (y + 1) * h / th);
            for (int x = 0; x < tw; ++x) {
                const int x0 = x * w / tw, x1 = std::max(x0 + 1, (x + 1) * w / tw);
                unsigned sum[3] = {0, 0, 0}, n = 0;
                for (int yy = y0; yy < y1; yy += std::max(1, (y1 - y0) / 4))
                    for (int xx = x0; xx < x1; xx += std::max(1, (x1 - x0) / 4)) {
                        const unsigned char* p = &rgba[((size_t)yy * w + xx) * 4];
                        sum[0] += p[0], sum[1] += p[1], sum[2] += p[2], ++n;
                    }
                unsigned char* q = &out[((size_t)y * tw + x) * 4];
                q[0] = (unsigned char)(sum[2] / n), q[1] = (unsigned char)(sum[1] / n), q[2] = (unsigned char)(sum[0] / n), q[3] = 255;
            }
        }
        tv->snap = std::move(out);
        tv->snapW = tw;
        tv->snapH = th;
        if (previewTab_ == id) updateTabPreview();
    });
}

LRESULT CALLBACK previewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<BrowserWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            // Drawn off-screen first, so it never flickers.
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            if (self) self->paintTabPreview(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void BrowserWindow::paintTabPreview(HDC dc, const RECT& rc) {
    const float s = gfx_.dpiScale();
    HBRUSH bg = CreateSolidBrush(toColorRef(pal_.surface));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    const Tab* t = model_.find(previewTab_);
    TabView* v = view(previewTab_);
    if (!t || !v) return;
    std::wstring title = widen(t->title);
    if (v->showStart) title = v->bookmarksPage ? L"Bookmarks" : v->historyPage ? L"History" : v->downloadsPage ? L"Downloads" : v->referencesPage ? L"Reference boards" : L"New tab";
    std::wstring where = v->showStart ? L"NorthRay" : widen(hostOf(t->url));
    if (title.empty()) title = where.empty() ? L"New tab" : where;
    const int pad = (int)std::lround(12 * s);
    SetBkMode(dc, TRANSPARENT);
    HFONT bold = CreateFontW(-(int)std::lround(13 * s), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT small = CreateFontW(-(int)std::lround(12 * s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, bold);
    SetTextColor(dc, toColorRef(pal_.ink));
    RECT tr{pad, pad, rc.right - pad, pad + (int)std::lround(20 * s)};
    DrawTextW(dc, title.c_str(), (int)title.size(), &tr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, small);
    SetTextColor(dc, toColorRef(pal_.inkMuted));
    std::wstring line = where;
    if (v->page && v->page->muted()) line += L"  ·  muted";
    else if (v->page && v->page->playingAudio()) line += L"  ·  playing sound";
    RECT hr{pad, tr.bottom + (int)std::lround(2 * s), rc.right - pad, tr.bottom + (int)std::lround(20 * s)};
    DrawTextW(dc, line.c_str(), (int)line.size(), &hr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    DeleteObject(bold);
    DeleteObject(small);
    // The picture of the page, under the words.
    if (!v->snap.empty() && v->snapW > 0 && v->snapH > 0) {
        const int top = hr.bottom + (int)std::lround(8 * s);
        const int w = rc.right - pad * 2, h = rc.bottom - top - pad;
        if (w > 0 && h > 0) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = v->snapW;
            bi.bmiHeader.biHeight = -v->snapH;   // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            // Shown at the window's width, cut at the bottom if the page is tall.
            const int srcH = std::min(v->snapH, (int)std::lround((double)h * v->snapW / w));
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, pad, top, w, std::min(h, (int)std::lround((double)srcH * w / v->snapW)), 0, v->snapH - srcH, v->snapW, srcH,
                          v->snap.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            HBRUSH frame = CreateSolidBrush(toColorRef(pal_.rule));
            RECT fr{pad, top, pad + w, top + std::min(h, (int)std::lround((double)srcH * w / v->snapW))};
            FrameRect(dc, &fr, frame);
            DeleteObject(frame);
        }
    }
    HBRUSH edge = CreateSolidBrush(toColorRef(pal_.rule));
    FrameRect(dc, &rc, edge);
    DeleteObject(edge);
}

void BrowserWindow::updateTabPreview() {
    const Rect* tr = nullptr;
    for (auto& [id, r] : tabRects_)
        if (id == previewTab_) tr = &r;
    TabView* v = view(previewTab_);
    if (!tr || !v || dragging_ || closing_ || !IsWindowVisible(hwnd()) || GetForegroundWindow() != hwnd()) { hideTabPreview(); return; }
    if (!preview_) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = previewProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"NorthRayBrowserTabPreview";
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            registered = true;
        }
        preview_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"NorthRayBrowserTabPreview", L"", WS_POPUP, 0, 0, 10, 10, hwnd(), nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
        if (!preview_) return;
        SetWindowLongPtrW(preview_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    const float s = gfx_.dpiScale();
    const float w = 280;
    float h = 58;
    if (!v->snap.empty() && v->snapW > 0) h += std::min(170.0f, (w - 24) * v->snapH / v->snapW) + 8;
    const bool side = sideW() > 0;
    POINT pt{(LONG)std::lround((side ? tr->right() + 6 : tr->x) * s), (LONG)std::lround((side ? tr->y : tr->bottom() + 4) * s)};
    ClientToScreen(hwnd(), &pt);
    const int pw = (int)std::lround(w * s), ph = (int)std::lround(h * s);
    SetWindowPos(preview_, HWND_TOP, pt.x, pt.y, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HRGN rgn = CreateRoundRectRgn(0, 0, pw + 1, ph + 1, (int)(16 * s), (int)(16 * s));
    SetWindowRgn(preview_, rgn, TRUE);
    InvalidateRect(preview_, nullptr, FALSE);
}

void BrowserWindow::hideTabPreview() {
    previewTab_ = 0;
    if (preview_ && IsWindowVisible(preview_)) ShowWindow(preview_, SW_HIDE);
}

}  // namespace nrb

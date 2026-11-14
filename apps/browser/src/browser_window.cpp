#include "browser_window.hpp"

#include "nrb/url.hpp"
#include "resource.h"
#include "version.hpp"

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
constexpr float kStatusH = 28;
constexpr float kTabTop = 8;
constexpr float kNewTabW = 36;
constexpr UINT_PTR kSpinTimer = 1, kShutdownTimer = 2;
constexpr UINT WM_APP_ENGINE_EXITED = WM_APP + 10;
constexpr ULONG_PTR kCopyDataOpen = 0x4E524231;  // "NRB1": addresses from a second launch

enum Icon { IconBack, IconForward, IconReload, IconStop, IconHome, IconMenu };

// Menu command ids.
enum : UINT {
    CmdNewTab = 100, CmdReopen, CmdPrint, CmdDevTools, CmdAbout, CmdFullscreen,
    CmdThemeSystem = 200, CmdThemeLight, CmdThemeDark,
    CmdEngineFirst = 300,   // + index in builtInSearchEngines()
    CmdTabReload = 400, CmdTabDuplicate, CmdTabClose, CmdTabCloseOthers,
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

// Edit-control subclass: no beep on Enter or Escape (the window handles them).
LRESULT CALLBACK addressProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
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
    : engine_(engine), settings_(settings), settingsPath_(std::move(settingsPath)) {}

BrowserWindow::~BrowserWindow() {
    if (editFont_) DeleteObject(editFont_);
    if (editBrush_) DeleteObject(editBrush_);
}

bool BrowserWindow::create(HINSTANCE inst, int show) {
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
    if (!createWindow(spec)) return false;

    edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd(), nullptr, inst, nullptr);
    SetWindowSubclass(edit_, addressProc, 1, 0);
    SendMessageW(edit_, EM_SETLIMITTEXT, 32 * 1024, 0);
    const SearchEngine* se = findSearchEngine(settings_.searchEngine);
    const std::wstring cue = L"Search with " + widen((se ? *se : defaultSearchEngine()).name) + L" or type an address";
    SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));

    applyTheme();
    newTab(L"", true, false);
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
    KillTimer(hwnd(), kSpinTimer);
    KillTimer(hwnd(), kShutdownTimer);
    views_.clear();
}

void BrowserWindow::applyTheme() {
    const bool dark = settings_.theme == Theme::Dark || (settings_.theme == Theme::System && cjui::systemPrefersDark());
    applyPalette(nr::palette(dark));
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush(toColorRef(pal_.sunken));
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
    TabView* v = view(id);
    if (!v) return;
    if (Tab* t = model_.find(id); t && v->page && !v->page->url().empty())
        t->url = narrow(v->page->url());   // remembered for Ctrl+Shift+T
    const bool wasActive = model_.activeId() == id;
    if (v->page) v->page->close();
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
    updateTitle();
    invalidate();
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
    if (TabView* cur = activeView()) cur->editing = editFocused_;
    model_.activate(id);
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
    v.page = std::make_unique<Page>(engine_, hwnd(), o, eventsFor(v.id));
    v.page->show(false);
    if (!forNewWindow) v.page->create();
    return v.page.get();
}

PageEvents BrowserWindow::eventsFor(int id) {
    PageEvents e;
    e.ready = [this, id] {
        syncTabFromPage(id);
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
    return e;
}

void BrowserWindow::syncTabFromPage(int id) {
    TabView* v = view(id);
    Tab* t = model_.find(id);
    if (!v || !t || !v->page) return;
    const std::wstring url = v->page->url();
    t->url = narrow(url);
    t->title = narrow(v->page->title());
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

void BrowserWindow::focusAddressBar() {
    if (!edit_) return;
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
        else if (v->showStart) text = L"";
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

LRESULT BrowserWindow::onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    switch (msg) {
        case WM_COMMAND:
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
            if ((HWND)lp == edit_) {
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
            break;
        case WM_MBUTTONUP: {
            // Middle-click a tab to close it.
            const float s = gfx_.dpiScale();
            const float x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
            for (auto& [id, r] : tabRects_)
                if (r.contains(x, y)) { closeTab(id); break; }
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
        case WM_CLOSE: {
            handled = true;
            if (closing_) return 0;
            closing_ = true;
            ShowWindow(hwnd(), SW_HIDE);
            for (TabView& v : views_)
                if (v.page) v.page->close();
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
        default: break;
    }
    return 0;
}

void BrowserWindow::onTimer(UINT_PTR id) {
    if (id == kSpinTimer) {
        if (!anyLoading()) KillTimer(hwnd(), kSpinTimer);
        invalidate();
    } else if (id == kShutdownTimer) {
        KillTimer(hwnd(), kShutdownTimer);
        DestroyWindow(hwnd());
    }
}

// ---------------------------------------------------------------- mouse

bool BrowserWindow::onMouseDown(float x, float y) {
    for (auto& [id, r] : tabRects_)
        if (r.contains(x, y) && hitAt(x, y).rfind("tabx:", 0) != 0) {
            dragTab_ = id;
            dragStartX_ = x;
            dragging_ = false;
            activateTab(id);
            return true;
        }
    return false;
}

void BrowserWindow::onMouseMove(float x, float, bool leftDown) {
    if (!dragTab_ || !leftDown) return;
    if (!dragging_ && std::abs(x - dragStartX_) < 6) return;
    dragging_ = true;
    // Drop position: the tab under the pointer.
    for (size_t i = 0; i < tabRects_.size(); ++i) {
        const Rect& r = tabRects_[i].second;
        if (x >= r.x && x < r.right() && tabRects_[i].first != dragTab_) {
            model_.move(dragTab_, i);
            invalidate();
            break;
        }
    }
}

bool BrowserWindow::onMouseUp(float, float) {
    const bool was = dragTab_ != 0;
    dragTab_ = 0;
    dragging_ = false;
    if (was) invalidate();
    return was;
}

bool BrowserWindow::onRightClick(float x, float y) {
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
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CmdNewTab, L"New tab\tCtrl+T");
    AppendMenuW(m, MF_STRING | (model_.hasClosed() ? 0 : MF_GRAYED), CmdReopen, L"Reopen closed tab\tCtrl+Shift+T");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (hasPage ? 0 : MF_GRAYED), CmdPrint, L"Print...\tCtrl+P");
    AppendMenuW(m, MF_STRING | (fullscreen_ ? MF_CHECKED : 0), CmdFullscreen, L"Full screen\tF11");
    AppendMenuW(m, MF_STRING | (hasPage ? 0 : MF_GRAYED), CmdDevTools, L"Developer tools\tF12");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU se = CreatePopupMenu();
    const auto& engines = builtInSearchEngines();
    for (size_t i = 0; i < engines.size(); ++i)
        AppendMenuW(se, MF_STRING | (engines[i].id == settings_.searchEngine ? MF_CHECKED : 0), CmdEngineFirst + (UINT)i,
                    widen(engines[i].name).c_str());
    AppendMenuW(m, MF_POPUP, (UINT_PTR)se, L"Search engine");
    HMENU th = CreatePopupMenu();
    AppendMenuW(th, MF_STRING | (settings_.theme == Theme::System ? MF_CHECKED : 0), CmdThemeSystem, L"As Windows");
    AppendMenuW(th, MF_STRING | (settings_.theme == Theme::Light ? MF_CHECKED : 0), CmdThemeLight, L"Light");
    AppendMenuW(th, MF_STRING | (settings_.theme == Theme::Dark ? MF_CHECKED : 0), CmdThemeDark, L"Dark");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)th, L"Theme");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CmdAbout, L"About NorthRay Browser");

    // Below the menu button, right-aligned.
    const float s = gfx_.dpiScale();
    RECT rc;
    GetClientRect(hwnd(), &rc);
    POINT pt{rc.right - (LONG)(10 * s), (LONG)((kStripH + kToolbarH - 6) * s)};
    ClientToScreen(hwnd(), &pt);
    const UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd(), nullptr);
    DestroyMenu(m);
    const std::wstring& settingsPath = settingsPath_;
    if (cmd == CmdNewTab) newTab(L"", true, false);
    else if (cmd == CmdReopen) reopenClosedTab();
    else if (cmd == CmdPrint && hasPage) v->page->print();
    else if (cmd == CmdDevTools && hasPage) v->page->openDevTools();
    else if (cmd == CmdFullscreen) setFullscreen(!fullscreen_, false);
    else if (cmd == CmdAbout) showAbout();
    else if (cmd >= CmdThemeSystem && cmd <= CmdThemeDark) {
        settings_.theme = (Theme)(cmd - CmdThemeSystem);
        settings_.save(settingsPath);
        applyTheme();
    } else if (cmd >= CmdEngineFirst && cmd < CmdEngineFirst + engines.size()) {
        settings_.searchEngine = engines[cmd - CmdEngineFirst].id;
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
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (hasPage ? 0 : MF_GRAYED), CmdTabReload, L"Reload");
    AppendMenuW(m, MF_STRING | (hasPage ? 0 : MF_GRAYED), CmdTabDuplicate, L"Duplicate");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CmdTabClose, L"Close tab\tCtrl+W");
    AppendMenuW(m, MF_STRING | (model_.size() > 1 ? 0 : MF_GRAYED), CmdTabCloseOthers, L"Close other tabs");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (model_.hasClosed() ? 0 : MF_GRAYED), CmdReopen, L"Reopen closed tab\tCtrl+Shift+T");
    const UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0, hwnd(), nullptr);
    DestroyMenu(m);
    if (cmd == CmdTabReload && hasPage) v->page->reload();
    else if (cmd == CmdTabDuplicate) duplicateTab(id);
    else if (cmd == CmdTabClose) closeTab(id);
    else if (cmd == CmdTabCloseOthers) closeOtherTabs(id);
    else if (cmd == CmdReopen) reopenClosedTab();
    invalidate();
}

void BrowserWindow::showAbout() {
    std::wstring text = L"NorthRay Browser " CJ_VERSION_WSTR L" (preview)\n\n";
    text += engine_.version().empty() ? std::wstring(L"Engine: not running") : L"Engine: Microsoft Edge WebView2 Runtime " + engine_.version();
    text += L"\n\nDeveloped by the " CJ_DEVELOPER_WSTR L" project.\nNorthRaySoftware@protonmail.com\n\n"
            L"Web pages are shown by Microsoft's WebView2 (Chromium), which Windows keeps up to date. "
            L"Licences: chrome://credits (the engine's) and the licences folder next to NorthRay Browser.";
    MessageBoxW(hwnd(), text.c_str(), L"About NorthRay Browser", MB_OK | MB_ICONINFORMATION);
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
    const float top = chrome ? kStripH + kToolbarH : 0;
    const float statusH = chrome ? kStatusH : 0;
    const Rect content{0, top, W, H - top - statusH};

    TabView* v = activeView();
    if (engineFailed_) drawEngineProblem(content);
    else if (v && v->error) drawErrorPage(content, *v);
    else if (v && v->showStart) drawStartPage(content);
    else if (v && (!v->page || !v->page->ready())) {
        gfx_.fill(content, pal_.paper);   // the page is on its way
    }
    placePages(content);

    if (chrome) {
        drawTabStrip({0, 0, W, kStripH});
        drawToolbar({0, kStripH, W, kToolbarH});
        drawStatusBar({0, H - kStatusH, W, kStatusH});
    } else if (edit_ && IsWindowVisible(edit_)) {
        if (GetFocus() == edit_) SetFocus(hwnd());
        ShowWindow(edit_, SW_HIDE);
    }
}

void BrowserWindow::placePages(const Rect& content) {
    const float s = gfx_.dpiScale();
    const RECT rc{(LONG)std::lround(content.x * s), (LONG)std::lround(content.y * s), (LONG)std::lround(content.right() * s),
                  (LONG)std::lround(content.bottom() * s)};
    TabView* a = activeView();
    for (TabView& v : views_) {
        if (!v.page) continue;
        const bool show = &v == a && !v.showStart && !v.error && !engineFailed_;
        if (show) v.page->setBounds(rc);
        v.page->show(show);
    }
}

void BrowserWindow::drawTabStrip(const Rect& r) {
    nr::nightSky(gfx_, r, 0, 0.75f, 5);
    nr::auroraBand(gfx_, {r.x, r.y, r.w, 3}, pal_.dark);
    const auto& tabs = model_.tabs();
    const float x0 = r.x + 8, avail = r.w - 16 - kNewTabW - 8;
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
        if (x > r.right() - kNewTabW - 16) break;   // the rest are reachable with Ctrl+Tab (overflow comes in 0.2)
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
}

void BrowserWindow::drawTab(const Tab& t, const Rect& r, bool active) {
    const std::string id = "tab:" + std::to_string(t.id);
    addHit(id, r, [this, tid = t.id] { activateTab(tid); });
    const float hv = hoverAmount(id);
    if (active) {
        gfx_.fillRounded({r.x, r.y, r.w, r.h + 10}, 8, pal_.surface);
    } else if (hv > 0.01f) {
        gfx_.fillRounded({r.x + 2, r.y + 2, r.w - 4, r.h - 6}, 8, nr::kNightInk.withAlpha(0.08f * hv));
    }
    const Color ink = active ? pal_.ink : nr::kNightMuted.mix(nr::kNightInk, hv * 0.6f);
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
    } else if (v && !v->favicon.empty() &&
               gfx_.drawPixels(L"fav:" + std::to_wstring(t.id) + L":" + std::to_wstring(v->favGen), v->favW, v->favH,
                               v->favicon.data(), ic, true)) {
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

    // Title
    if (r.w >= 72) {
        std::wstring title = widen(t.title);
        if (title.empty() && v && v->showStart) title = L"New tab";
        if (title.empty() && v && v->error) title = hostLabel(v->errorUrl);
        if (title.empty()) title = widen(hostOf(t.url));
        if (title.empty()) title = t.loading ? L"Loading..." : L"New tab";
        const float tw = r.right() - x - (showClose ? 30 : 10);
        gfx_.text(title, {x, r.y, tw, r.h - 2}, active ? Font::SmallStrong : Font::Small, ink, {Align::Left, false, true, true});
    }

    // Close
    if (showClose) {
        const std::string xid = "tabx:" + std::to_string(t.id);
        const Rect xb{r.right() - 28, cy - 9, 18, 18};
        addHit(xid, xb, [this, tid = t.id] { closeTab(tid); }, false);
        const float xh = hoverAmount(xid);
        if (xh > 0.01f) gfx_.fillRounded(xb, 5, (active ? pal_.ink : nr::kNightInk).withAlpha(0.10f * xh));
        const float c = xb.x + 9, m = xb.y + 9;
        gfx_.line(c - 4, m - 4, c + 4, m + 4, ink, 1.4f);
        gfx_.line(c + 4, m - 4, c - 4, m + 4, ink, 1.4f);
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
    drawAddressBar({x, by - 1, menuX - 10 - x, bs + 2});
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
    // The edit control sits inside the drawn frame.
    const float s = gfx_.dpiScale();
    const float th = 20;
    const RECT rc{(LONG)std::lround(x * s), (LONG)std::lround((r.y + (r.h - th) / 2) * s), (LONG)std::lround((r.right() - 10) * s),
                  (LONG)std::lround((r.y + (r.h + th) / 2) * s)};
    if (memcmp(&rc, &editRect_, sizeof rc) != 0 || !IsWindowVisible(edit_)) {
        editRect_ = rc;
        SetWindowPos(edit_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void BrowserWindow::layoutAddressFont() {
    editScale_ = gfx_.dpiScale();
    if (editFont_) DeleteObject(editFont_);
    editFont_ = CreateFontW(-(int)std::lround(14 * editScale_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (edit_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
}

void BrowserWindow::drawStartPage(const Rect& r) {
    gfx_.fill(r, pal_.paper);
    const float pad = r.w > 900 ? 44 : 28;
    const float w = std::min(r.w - pad * 2, 880.0f);
    const float x = r.x + (r.w - w) / 2;
    float y = r.y + 36;
    const Rect hero{x, y, w, 150};
    nr::nightSky(gfx_, hero, 12, 1.0f, 3, pal_.paper);
    gfx_.text(L"NORTHRAY SOFTWARE", {hero.x + 32, hero.y + 30, hero.w - 64, 16}, Font::Label, nr::kAuroraGreen, {Align::Left, false});
    gfx_.text(L"NorthRay Browser", {hero.x + 32, hero.y + 50, hero.w - 64, 40}, Font::Title, nr::kNightInk, {Align::Left, false});
    const SearchEngine* se = findSearchEngine(settings_.searchEngine);
    const std::wstring line = L"Search with " + widen((se ? *se : defaultSearchEngine()).name) +
                              L", or type an address, in the bar above.";
    gfx_.text(line, {hero.x + 32, hero.y + 96, hero.w - 64, 40}, Font::Body, nr::kNightMuted);
    y = hero.bottom() + 32;

    y += sectionLabel(x, y, w, L"KEYBOARD") + 6;
    struct Row { const wchar_t* keys; const wchar_t* what; };
    const Row rows[] = {{L"Ctrl+L", L"Go to the address bar"},
                        {L"Ctrl+T", L"Open a new tab"},
                        {L"Ctrl+W", L"Close this tab"},
                        {L"Ctrl+Shift+T", L"Reopen the tab you closed last"},
                        {L"Ctrl+Tab", L"Go to the next tab"},
                        {L"Alt+Left", L"Go back"}};
    for (const Row& row : rows) {
        gfx_.text(row.keys, {x, y, 150, 24}, Font::SmallStrong, pal_.ink, {Align::Left, false, false, true});
        gfx_.text(row.what, {x + 150, y, w - 150, 24}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        y += 26;
        gfx_.line(x, y - 1, x + w, y - 1, pal_.rule.withAlpha(0.6f));
    }
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

void BrowserWindow::drawStatusBar(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.x, r.y + 0.5f, r.right(), r.y + 0.5f, pal_.rule);
    TabView* v = activeView();
    const Tab* t = model_.active();
    std::wstring left;
    if (v && !v->status.empty()) left = v->status;   // the link under the pointer
    else if (t && t->loading && !hostOf(t->url).empty()) left = L"Loading " + widen(hostOf(t->url)) + L"...";
    else if (engineOffForDevelopment_) left = L"Development build: the engine is off (--no-engine). Pages won't load.";
    else if (!engineOk_ && !engineFailed_) left = L"Starting the browser engine...";
    else left = L"NorthRay Browser " CJ_VERSION_WSTR;
    gfx_.text(left, {r.x + 14, r.y, r.w * 0.62f, r.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
    gfx_.text(flashText(), {r.x + r.w * 0.3f, r.y, r.w * 0.7f - 14, r.h}, Font::Small, flashColor(pal_.ink),
              {Align::Right, false, true, true});
}

}  // namespace nrb

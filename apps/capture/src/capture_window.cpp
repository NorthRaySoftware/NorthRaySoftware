#include "capture_window.hpp"

#include "log.hpp"
#include "resource.h"
#include "version.hpp"

#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"
#include "cjui/northray.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::Rect;
using cjui::narrow;
using cjui::widen;
namespace nr = cjui::northray;

namespace {

constexpr float kTopH = 54;
constexpr float kStatusH = 30;
constexpr float kSplit = 7;
constexpr float kHeaderH = 38;
constexpr UINT_PTR kStatsTimer = 1;
constexpr UINT_PTR kSaveTimer = 2;
constexpr UINT WM_APP_ENCODERS = WM_APP + 60;

enum : UINT {
    CmdRename = 100, CmdDuplicate, CmdUp, CmdDown, CmdDelete, CmdNewScene,
    CmdPanel0 = 200,   // + panel: show or hide
    CmdPreview = 220, CmdResetLayout, CmdSaveLayout, CmdLayout0 = 300, CmdDeleteLayout0 = 400,
    CmdHide = 500, CmdLeft, CmdRight,
};

COLORREF toColorRef(Color c) {
    auto b = [](float v) { return (BYTE)std::clamp((int)std::lround(v * 255), 0, 255); };
    return RGB(b(c.r), b(c.g), b(c.b));
}

LRESULT CALLBACK quietEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == 1 /* Ctrl+A */)) {
        if (w == 1) SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return DefSubclassProc(h, m, w, l);
}

std::wstring megabytes(std::uint64_t bytes) {
    wchar_t b[32];
    swprintf(b, 32, L"%.0f MB", (double)bytes / (1024.0 * 1024.0));
    return b;
}

std::wstring percent(double v) {
    if (v < 0) return L"…";
    wchar_t b[32];
    swprintf(b, 32, v < 10 ? L"%.1f%%" : L"%.0f%%", v);
    return b;
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

CaptureWindow::CaptureWindow(Settings& settings, std::wstring settingsPath, std::wstring stateFolder)
    : settings_(settings), settingsPath_(std::move(settingsPath)), stateFolder_(std::move(stateFolder)) {
    layout_ = Layout::fromJson(settings_.layout);
}

CaptureWindow::~CaptureWindow() {
    if (askFont_) DeleteObject(askFont_);
    if (askBrush_) DeleteObject(askBrush_);
}

bool CaptureWindow::create(HINSTANCE inst, int show) {
    cjui::WindowSpec spec;
    spec.instance = inst;
    spec.className = kClassName;
    spec.title = L"NorthRay Capture";
    spec.iconId = IDI_APP;
    spec.width = 1280;
    spec.height = 800;
    spec.minWidth = 900;
    spec.minHeight = 560;
    spec.show = show;
    spec.style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    spec.customFrame = true;
    spec.captionHeight = kTopH;
    setAmbientAllowed(false);   // no twinkling: an idle Capture window shouldn't redraw
    if (!createWindow(spec)) return false;
    ask_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd(), nullptr, inst, nullptr);
    SetWindowSubclass(ask_, quietEditProc, 1, 0);
    SendMessageW(ask_, EM_SETLIMITTEXT, 120, 0);
    applyTheme();
    loadScenes();
    startEngine();
    SetTimer(hwnd(), kStatsTimer, settings_.lowResource ? 2000 : 1000, nullptr);
    usage_ = sampler_.sample();
    sys::findEncodersAsync(hwnd(), WM_APP_ENCODERS);
    ShowWindow(hwnd(), show);
    UpdateWindow(hwnd());
    log::info("Window open; scene collection \"" + settings_.sceneCollection + "\" with " + std::to_string(scenes_.scenes().size()) + " scene(s)");
    return true;
}

void CaptureWindow::onDestroy() {
    engine_.stop();
    live_.clear();   // stops the captures
    saveScenes();
    saveLayout();
}

void CaptureWindow::applyTheme() {
    const bool dark = settings_.theme == Theme::Dark || (settings_.theme == Theme::System && cjui::systemPrefersDark());
    applyPalette(nr::palette(dark));
    if (askBrush_) DeleteObject(askBrush_);
    askBrush_ = CreateSolidBrush(toColorRef(pal_.sunken));
    invalidate();
}

void CaptureWindow::saveLayout() {
    settings_.layout = layout_.toJson();
    settings_.save(settingsPath_);
}

// ---------------------------------------------------------------- scenes on disk

void CaptureWindow::loadScenes() {
    const std::wstring dir = stateFolder_ + L"\\scenes";
    CreateDirectoryW(dir.c_str(), nullptr);
    scenesPath_ = dir + L"\\" + widen(settings_.sceneCollection) + L".json";
    std::ifstream f(scenesPath_.c_str(), std::ios::binary);
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        if (!scenes_.fromJson(ss.str())) {
            // Kept aside rather than overwritten: it may be from a newer version, or mendable.
            const std::wstring aside = scenesPath_ + L".unreadable";
            f.close();
            MoveFileExW(scenesPath_.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING);
            log::warn("The scene collection couldn't be read; it was kept as " + narrow(aside) + " and a new one started");
            flash(L"Your scenes file couldn't be read, so it was kept aside (“.unreadable”) and a new one started.", 10);
        }
    }
    savedChanges_ = scenes_.changes();
}

void CaptureWindow::saveScenes() {
    if (scenesPath_.empty() || scenes_.changes() == savedChanges_) return;
    const std::wstring tmp = scenesPath_ + L".new";
    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        f << scenes_.toJson();
        if (!f.good()) { log::error("Scenes couldn't be saved (writing failed)"); return; }
    }
    if (!MoveFileExW(tmp.c_str(), scenesPath_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log::error("Scenes couldn't be saved (replacing the file failed)");
        return;
    }
    savedChanges_ = scenes_.changes();
}

void CaptureWindow::scenesChanged() {
    // Saved shortly after the last change: quick changes in a row write once.
    SetTimer(hwnd(), kSaveTimer, 600, nullptr);
    invalidate();
}

// ---------------------------------------------------------------- scene commands

void CaptureWindow::newScene() {
    askText(L"Name the new scene", widen(scenes_.uniqueSceneName("Scene")), [this](const std::wstring& name) {
        const Id id = scenes_.addScene(narrow(name));
        scenes_.setCurrent(id);
        log::info("Scene added: " + scenes_.scene(id)->name);
        scenesChanged();
    });
}

void CaptureWindow::renameScene(const Id& id) {
    const Scene* s = scenes_.scene(id);
    if (!s) return;
    askText(L"Rename the scene", widen(s->name), [this, id](const std::wstring& name) {
        if (!scenes_.renameScene(id, narrow(name))) {
            flash(name.find_first_not_of(L' ') == std::wstring::npos ? L"A scene needs a name." : L"Another scene already has that name.", 5);
            return;
        }
        scenesChanged();
    });
}

void CaptureWindow::deleteScene(const Id& id) {
    const Scene* s = scenes_.scene(id);
    if (!s) return;
    if (scenes_.scenes().size() <= 1) { flash(L"There's always at least one scene.", 4); return; }
    const std::wstring q = L"Delete the scene “" + widen(s->name) + L"”?" +
                           (s->items.empty() ? L"" : L"\n\nIts sources stay available to other scenes.");
    if (cjui::messageBox(hwnd(), q, L"Delete scene", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    log::info("Scene deleted: " + s->name);
    scenes_.removeScene(id);
    scenesChanged();
}

void CaptureWindow::sceneMenu(const Id& id, POINT screen) {
    const auto& all = scenes_.scenes();
    const bool first = !all.empty() && all.front().id == id, last = !all.empty() && all.back().id == id;
    const std::vector<MenuItem> items = {
        MenuItem::item(L"Rename", CmdRename, L"F2"),
        MenuItem::item(L"Duplicate", CmdDuplicate),
        MenuItem::sep(),
        MenuItem::item(L"Move up", CmdUp, {}, !first),
        MenuItem::item(L"Move down", CmdDown, {}, !last),
        MenuItem::sep(),
        MenuItem::item(L"Delete", CmdDelete, L"Delete", all.size() > 1),
        MenuItem::sep(),
        MenuItem::item(L"New scene…", CmdNewScene),
    };
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (cmd == CmdRename) renameScene(id);
    else if (cmd == CmdDuplicate) { scenes_.setCurrent(scenes_.duplicateScene(id)); scenesChanged(); }
    else if (cmd == CmdUp) { scenes_.moveScene(id, -1); scenesChanged(); }
    else if (cmd == CmdDown) { scenes_.moveScene(id, 1); scenesChanged(); }
    else if (cmd == CmdDelete) deleteScene(id);
    else if (cmd == CmdNewScene) newScene();
}

// ---------------------------------------------------------------- layout commands

void CaptureWindow::layoutMenu(POINT screen) {
    std::vector<MenuItem> items;
    for (int p = 0; p < Layout::kPanels; ++p)
        items.push_back(MenuItem::item(widen(Layout::panelName(p)), CmdPanel0 + (UINT)p, {}, true, layout_.shows(p)));
    items.push_back(MenuItem::item(L"Preview", CmdPreview, {}, true, layout_.previewVisible));
    items.push_back(MenuItem::sep());
    std::vector<std::string> names;
    for (auto& [k, v] : settings_.savedLayouts) names.push_back(k);
    if (!names.empty()) {
        for (size_t i = 0; i < names.size() && i < 90; ++i) items.push_back(MenuItem::item(widen(names[i]), CmdLayout0 + (UINT)i));
        std::vector<MenuItem> del;
        for (size_t i = 0; i < names.size() && i < 90; ++i) del.push_back(MenuItem::item(widen(names[i]), CmdDeleteLayout0 + (UINT)i));
        items.push_back(MenuItem::sub(L"Delete a saved layout", del));
        items.push_back(MenuItem::sep());
    }
    items.push_back(MenuItem::item(L"Save this layout…", CmdSaveLayout));
    items.push_back(MenuItem::item(L"Reset the layout", CmdResetLayout));
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (cmd >= CmdPanel0 && cmd < CmdPanel0 + (UINT)Layout::kPanels) {
        const int p = (int)(cmd - CmdPanel0);
        if (layout_.shows(p)) {
            if (layout_.order.size() == 1) flash(L"One panel always stays.", 4);
            layout_.hide(p);
        } else layout_.show(p);
        saveLayout();
    } else if (cmd == CmdPreview) {
        layout_.previewVisible = !layout_.previewVisible;
        saveLayout();
    } else if (cmd == CmdResetLayout) {
        layout_ = Layout{};
        saveLayout();
    } else if (cmd == CmdSaveLayout) {
        askText(L"Name this layout", L"My layout", [this](const std::wstring& name) {
            std::wstring n = name;
            while (!n.empty() && n.back() == L' ') n.pop_back();
            if (n.empty()) return;
            settings_.savedLayouts[narrow(n)] = layout_.toJson();
            saveLayout();
            flash(L"Saved the layout “" + n + L"”. Choose it from Layout to bring it back.", 5);
        });
    } else if (cmd >= CmdLayout0 && cmd < CmdLayout0 + names.size()) {
        layout_ = Layout::fromJson(settings_.savedLayouts[names[cmd - CmdLayout0]]);
        saveLayout();
    } else if (cmd >= CmdDeleteLayout0 && cmd < CmdDeleteLayout0 + names.size()) {
        settings_.savedLayouts.erase(names[cmd - CmdDeleteLayout0]);
        saveLayout();
    }
    invalidate();
}

void CaptureWindow::panelMenu(int panel, POINT screen) {
    const auto it = std::find(layout_.order.begin(), layout_.order.end(), panel);
    const long at = it - layout_.order.begin();
    const std::vector<MenuItem> items = {
        MenuItem::item(L"Move left", CmdLeft, {}, at > 0),
        MenuItem::item(L"Move right", CmdRight, {}, at + 1 < (long)layout_.order.size()),
        MenuItem::sep(),
        MenuItem::item(L"Hide this panel", CmdHide, {}, layout_.order.size() > 1),
    };
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (cmd == CmdLeft) layout_.move(panel, -1);
    else if (cmd == CmdRight) layout_.move(panel, 1);
    else if (cmd == CmdHide) {
        layout_.hide(panel);
        flash(std::wstring(L"Hidden. Layout in the top bar brings ") + widen(Layout::panelName(panel)) + L" back.", 5);
    }
    if (cmd) saveLayout();
    invalidate();
}

void CaptureWindow::openSettings(bool on) {
    settingsOpen_ = on;
    resetScroll();
    invalidate();
}

void CaptureWindow::showAbout() {
    std::wstring text = L"NorthRay Capture " CJ_VERSION_WSTR L" (in development: Milestone 2, video capture)\n\n"
                        L"Livestreaming and recording for Windows.\n\n"
                        L"Developed by " CJ_DEVELOPER_WSTR L".\nNorthRaySoftware@protonmail.com\n\n"
                        L"Licences: the licences folder next to NorthRay Capture.";
    cjui::messageBox(hwnd(), text, L"About NorthRay Capture", MB_OK | MB_ICONINFORMATION);
}

void CaptureWindow::copyDiagnostics() {
    std::wstring t = L"NorthRay Capture " CJ_VERSION_WSTR L"\r\n" + sys::windowsVersion() + L"\r\nCPU: " + sys::cpuName() + L"\r\nGPU: " + sys::gpuNames() +
                     L"\r\nEncoders:";
    if (!encodersKnown_) t += L" still being checked";
    for (auto& e : encoders_) t += L"\r\n  " + widen(e.codec) + L"  " + e.name + (e.hardware ? L"  (hardware)" : L"  (software)");
    t += L"\r\nScenes: " + std::to_wstring(scenes_.scenes().size()) + L"\r\nLog: " + log::todaysFile();
    t = widen(redact(narrow(t)));
    if (!OpenClipboard(hwnd())) { flash(L"The clipboard is busy. Try again.", 4); return; }
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(h), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
    flash(L"Copied the diagnostics (no stream keys or personal details).", 4);
}

// ---------------------------------------------------------------- input

bool CaptureWindow::preTranslate(MSG& msg) {
    if (msg.hwnd == ask_ && msg.message == WM_KEYDOWN) {
        if (msg.wParam == VK_RETURN) { endAsk(true); return true; }
        if (msg.wParam == VK_ESCAPE) { endAsk(false); return true; }
    }
    return false;
}

bool CaptureWindow::onKeyDown(WPARAM vk, bool ctrl, bool shift) {
    if (vk == VK_ESCAPE && settingsOpen_) { openSettings(false); return true; }
    if (ctrl && vk == VK_OEM_COMMA) { openSettings(!settingsOpen_); return true; }
    if (settingsOpen_) return false;
    if (sourceKey(vk, ctrl, shift)) return true;
    const auto& all = scenes_.scenes();
    auto at = std::find_if(all.begin(), all.end(), [&](const Scene& s) { return s.id == scenes_.current(); });
    switch (vk) {
        case VK_F2: renameScene(scenes_.current()); return true;
        case VK_DELETE: deleteScene(scenes_.current()); return true;
        case VK_UP:
            if (at != all.end() && at != all.begin()) { scenes_.setCurrent((at - 1)->id); scenesChanged(); }
            return true;
        case VK_DOWN:
            if (at != all.end() && at + 1 != all.end()) { scenes_.setCurrent((at + 1)->id); scenesChanged(); }
            return true;
        default: break;
    }
    // Ctrl+1..9: switch to that scene.
    if (ctrl && vk >= '1' && vk <= '9' && (size_t)(vk - '1') < all.size()) {
        scenes_.setCurrent(all[(size_t)(vk - '1')].id);
        scenesChanged();
        return true;
    }
    if (ctrl && vk == 'N') { newScene(); return true; }
    return false;
}

bool CaptureWindow::onMouseDown(float x, float y) {
    if (asking_) {
        if (askCard_.contains(x, y)) return false;
        endAsk(false);
        return true;
    }
    if (settingsOpen_) return false;
    // Dividers: between the preview and the panels, and between panels.
    if (hSplit_.contains(x, y)) {
        drag_ = Drag::Horizontal;
        dragStart_ = y;
        dragStartValue_ = layout_.dockHeight;
        return true;
    }
    for (size_t i = 0; i < vSplits_.size(); ++i)
        if (vSplits_[i].contains(x, y)) {
            drag_ = Drag::Vertical;
            dragIndex_ = (int)i;
            dragStart_ = x;
            return true;
        }
    if (previewMouseDown(x, y)) return true;
    // A panel's header: dragged sideways, it trades places with its neighbours.
    for (auto& [p, r] : panelRects_)
        if (Rect{r.x, r.y, r.w - 80, kHeaderH}.contains(x, y)) {
            drag_ = Drag::Panel;
            dragPanel_ = p;
            dragPanelX_ = x;
            return false;   // the header's own buttons still work
        }
    // Scenes: a click switches; a quick second click renames.
    for (auto& [id, r] : sceneRows_)
        if (r.contains(x, y)) {
            const DWORD now = GetMessageTime();
            if (id == lastClickScene_ && now - lastClickTime_ <= GetDoubleClickTime()) {
                lastClickScene_.clear();
                renameScene(id);
                return true;
            }
            lastClickScene_ = id;
            lastClickTime_ = now;
            focus_ = Focus::Scenes;
            if (scenes_.setCurrent(id)) scenesChanged();
            return true;
        }
    // Sources: a click selects (the eye and lock buttons at the right work on their own); a quick
    // second click renames.
    for (auto& [id, r] : sourceRows_)
        if (r.contains(x, y)) {
            focus_ = Focus::Sources;
            if (x > r.right() - 64) return false;
            const DWORD now = GetMessageTime();
            if (id == lastClickItem_ && now - lastClickItemTime_ <= GetDoubleClickTime()) {
                lastClickItem_.clear();
                if (const SceneItem* i = scenes_.item(scenes_.current(), id)) renameSource(i->source);
                return true;
            }
            lastClickItem_ = id;
            lastClickItemTime_ = now;
            selected_ = id;
            invalidate();
            return true;
        }
    return false;
}

void CaptureWindow::onMouseMove(float x, float y, bool leftDown) {
    if (drag_ == Drag::None && !asking_ && !settingsOpen_ && previewMouseMove(x, y, leftDown)) return;
    if (!leftDown) {
        if (drag_ != Drag::None) { drag_ = Drag::None; saveLayout(); }
        // The resize cursor over a divider.
        if (hSplit_.contains(x, y)) SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
        for (auto& r : vSplits_)
            if (r.contains(x, y)) SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return;
    }
    if (drag_ == Drag::Horizontal) {
        const float space = dockRect_.bottom() - previewRect_.y;
        if (space > 0) layout_.dockHeight = std::clamp(dragStartValue_ + (dragStart_ - y) / space, 0.15f, 0.8f);
        SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
        invalidate();
    } else if (drag_ == Drag::Vertical) {
        if (dockRect_.w > 0) layout_.resizeDivider(dragIndex_, (x - dragStart_) / dockRect_.w);
        dragStart_ = x;
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        invalidate();
    } else if (drag_ == Drag::Panel) {
        // Over a neighbour's middle: trade places.
        for (size_t i = 0; i < panelRects_.size(); ++i) {
            const auto& [p, r] = panelRects_[i];
            if (p == dragPanel_ || !r.contains(x, r.y + 1)) continue;
            const bool right = x > dragPanelX_;
            if ((right && x > r.x + r.w * 0.5f) || (!right && x < r.x + r.w * 0.5f)) {
                layout_.move(dragPanel_, right ? 1 : -1);
                dragPanelX_ = x;
                cancelPress();
                invalidate();
            }
        }
    }
}

bool CaptureWindow::onMouseUp(float, float) {
    if (pdrag_ != PreviewDrag::None) {
        pdrag_ = PreviewDrag::None;
        guidesX_.clear();
        guidesY_.clear();
        scenesChanged();
        return true;
    }
    if (drag_ == Drag::None) return false;
    const bool consumed = drag_ != Drag::Panel;
    drag_ = Drag::None;
    saveLayout();
    return consumed;
}

bool CaptureWindow::onRightClick(float x, float y) {
    if (settingsOpen_ || asking_) return false;
    const float s = gfx_.dpiScale();
    POINT pt{(LONG)(x * s), (LONG)(y * s)};
    ClientToScreen(hwnd(), &pt);
    for (auto& [id, r] : sceneRows_)
        if (r.contains(x, y)) {
            scenes_.setCurrent(id);
            invalidate();
            sceneMenu(id, pt);
            return true;
        }
    for (auto& [id, r] : sourceRows_)
        if (r.contains(x, y)) {
            focus_ = Focus::Sources;
            itemMenu(id, pt);
            return true;
        }
    if (previewLive_ && previewInner_.contains(x, y)) {
        focus_ = Focus::Preview;
        if (const Scene* sc = scenes_.scene(scenes_.current())) {
            const Id hit = itemAt(sc->items, sizesOf(*sc), toCanvas(x, y));
            if (!hit.empty()) itemMenu(hit, pt);
        }
        return true;
    }
    for (auto& [p, r] : panelRects_)
        if (Rect{r.x, r.y, r.w, kHeaderH}.contains(x, y)) {
            panelMenu(p, pt);
            return true;
        }
    return false;
}

bool CaptureWindow::onWheel(float, float, float) { return false; }

LRESULT CaptureWindow::onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    if (msg == WM_APP_ENCODERS) {
        auto* list = reinterpret_cast<std::vector<sys::Encoder>*>(lp);
        encoders_ = std::move(*list);
        delete list;
        encodersKnown_ = true;
        for (auto& e : encoders_) log::info("Encoder found: " + e.codec + " " + narrow(e.name) + (e.hardware ? " (hardware)" : " (software)"));
        if (encoders_.empty()) log::warn("Media Foundation offered no encoders");
        invalidate();
        handled = true;
        return 0;
    }
    if (msg == WM_CTLCOLOREDIT && (HWND)lp == ask_) {
        HDC dc = (HDC)wp;
        SetTextColor(dc, toColorRef(pal_.ink));
        SetBkColor(dc, toColorRef(pal_.sunken));
        handled = true;
        return (LRESULT)askBrush_;
    }
    (void)wp;
    return 0;
}

void CaptureWindow::onTimer(UINT_PTR id) {
    if (id == kStatsTimer) {
        usage_ = sampler_.sample();
        checkPendingFits();
        invalidate();
    } else if (id == kSaveTimer) {
        KillTimer(hwnd(), kSaveTimer);
        saveScenes();
    }
}

// ---------------------------------------------------------------- the ask card

void CaptureWindow::askText(const std::wstring& title, const std::wstring& initial, std::function<void(const std::wstring&)> done) {
    askTitle_ = title;
    askDone_ = std::move(done);
    asking_ = true;
    SetWindowTextW(ask_, initial.c_str());
    invalidate();
    UpdateWindow(hwnd());
    ShowWindow(ask_, SW_SHOW);
    SetFocus(ask_);
    SendMessageW(ask_, EM_SETSEL, 0, -1);
}

void CaptureWindow::endAsk(bool commit) {
    if (!asking_) return;
    asking_ = false;
    const int n = GetWindowTextLengthW(ask_);
    std::wstring text((size_t)n, L'\0');
    if (n) GetWindowTextW(ask_, text.data(), n + 1);
    ShowWindow(ask_, SW_HIDE);
    SetFocus(hwnd());
    auto done = std::move(askDone_);
    askDone_ = nullptr;
    invalidate();
    if (commit && done) done(text);
}

void CaptureWindow::drawAsk(float W, float H) {
    gfx_.fill({0, 0, W, H}, cjui::hexColor("#000000").withAlpha(pal_.dark ? 0.45f : 0.25f));
    const float cw = std::min(440.0f, W - 40), ch = 172;
    const Rect card{std::floor((W - cw) / 2), std::floor(std::max(80.0f, H * 0.3f)), cw, ch};
    askCard_ = card;
    softShadow(card, 12, 1);
    gfx_.fillRounded(card, 12, pal_.surface);
    gfx_.strokeRounded(card, 12, pal_.rule);
    gfx_.text(askTitle_, {card.x + 22, card.y + 18, card.w - 44, 26}, Font::Heading, pal_.ink, {Align::Left, false, true});
    const Rect field{card.x + 22, card.y + 58, card.w - 44, 38};
    gfx_.fillRounded(field, 8, pal_.sunken);
    gfx_.strokeRounded(field, 8, pal_.focus, 1.5f);
    const float s = gfx_.dpiScale(), th = 20;
    if (askScale_ != s || !askFont_) {
        askScale_ = s;
        if (askFont_) DeleteObject(askFont_);
        askFont_ = CreateFontW(-(int)std::lround(14 * s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(ask_, WM_SETFONT, reinterpret_cast<WPARAM>(askFont_), TRUE);
    }
    SetWindowPos(ask_, HWND_TOP, (int)std::lround((field.x + 12) * s), (int)std::lround((field.y + (field.h - th) / 2) * s),
                 (int)std::lround((field.w - 24) * s), (int)std::lround(th * s), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const float by = card.bottom() - 54;
    button("ask:ok", {card.right() - 22 - 96, by, 96, 36}, L"OK", [this] { endAsk(true); }, true);
    button("ask:cancel", {card.right() - 22 - 96 - 10 - 96, by, 96, 36}, L"Cancel", [this] { endAsk(false); });
}

// ---------------------------------------------------------------- drawing

void CaptureWindow::drawFrame(float W, float H) {
    gfx_.clear(pal_.paper);
    drawTopBar({0, 0, W, kTopH});
    const Rect body{0, kTopH, W, H - kTopH - kStatusH};
    previewRect_ = dockRect_ = hSplit_ = previewInner_ = {};
    panelRects_.clear();
    vSplits_.clear();
    sceneRows_.clear();
    sourceRows_.clear();
    // The selection belongs to the current scene.
    if (!selected_.empty() && !scenes_.item(scenes_.current(), selected_)) selected_.clear();
    if (!hoverItem_.empty() && !scenes_.item(scenes_.current(), hoverItem_)) hoverItem_.clear();
    const Scene* cur = scenes_.scene(scenes_.current());
    const int previewFps = settings_.lowResource ? std::min(settings_.previewFps, 10) : settings_.previewFps;
    // The live preview window covers the preview area; it hides whenever something is drawn over it.
    previewLive_ = !settingsOpen_ && !asking_ && layout_.previewVisible && engine_.running() && engineError_.empty() && cur && !cur->items.empty() &&
                   previewFps > 0;
    if (settingsOpen_) {
        gfx_.pushClip(body);
        drawSettings(body);
        gfx_.popClip();
    } else {
        const float pad = 8;
        const Rect area{body.x + pad, body.y + pad, body.w - pad * 2, body.h - pad * 2};
        float dockH = layout_.previewVisible ? std::floor(area.h * layout_.dockHeight) : area.h;
        dockH = std::clamp(dockH, std::min(150.0f, area.h), area.h);
        if (layout_.previewVisible) {
            previewRect_ = {area.x, area.y, area.w, area.h - dockH - kSplit};
            hSplit_ = {area.x, previewRect_.bottom(), area.w, kSplit};
            drawPreview(previewRect_);
            const float hv = drag_ == Drag::Horizontal ? 1.0f : (hSplit_.contains(mouseX(), mouseY()) ? 0.6f : 0.0f);
            if (hv > 0) gfx_.fillRounded({hSplit_.x + hSplit_.w / 2 - 24, hSplit_.y + 2, 48, 3}, 1.5f, pal_.inkFaint.withAlpha(hv));
        }
        dockRect_ = {area.x, area.bottom() - dockH, area.w, dockH};
        // The panels, left to right, with a divider between each.
        const size_t n = layout_.order.size();
        const float usable = dockRect_.w - kSplit * (float)(n - 1);
        float x = dockRect_.x;
        for (size_t i = 0; i < n; ++i) {
            const int p = layout_.order[i];
            const float w = i + 1 == n ? dockRect_.right() - x : std::floor(usable * layout_.widths[(size_t)p]);
            const Rect r{x, dockRect_.y, w, dockRect_.h};
            panelRects_.push_back({p, r});
            drawPanel(p, r);
            x += w;
            if (i + 1 < n) {
                vSplits_.push_back({x, dockRect_.y, kSplit, dockRect_.h});
                x += kSplit;
            }
        }
    }
    drawStatusBar({0, H - kStatusH, W, kStatusH});
    drawToast({0, kTopH, W, H - kTopH - kStatusH});
    if (asking_) drawAsk(W, H);
    if (previewInner_.w <= 0) previewLive_ = false;
    placePreview();
    pushSnapshot();
}

void CaptureWindow::drawTopBar(const Rect& r) {
    nr::nightSky(gfx_, r, 0, 0.55f, 21);
    nr::auroraBand(gfx_, {r.x, r.y, r.w, 3}, pal_.dark);
    struct Pixels { int w = 0, h = 0; std::vector<std::uint8_t> rgba; };
    static Pixels mark{};
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        if (HICON h = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 64, 64, 0)) {
            ICONINFO ii{};
            if (GetIconInfo(h, &ii)) {
                BITMAP bm{};
                GetObjectW(ii.hbmColor, sizeof bm, &bm);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof bi.bmiHeader;
                bi.bmiHeader.biWidth = bm.bmWidth;
                bi.bmiHeader.biHeight = -bm.bmHeight;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                mark.w = bm.bmWidth;
                mark.h = bm.bmHeight;
                mark.rgba.resize((size_t)mark.w * mark.h * 4);
                HDC dc = GetDC(nullptr);
                GetDIBits(dc, ii.hbmColor, 0, (UINT)mark.h, mark.rgba.data(), &bi, DIB_RGB_COLORS);
                ReleaseDC(nullptr, dc);
                for (size_t i = 0; i < mark.rgba.size(); i += 4) std::swap(mark.rgba[i], mark.rgba[i + 2]);
                DeleteObject(ii.hbmColor);
                DeleteObject(ii.hbmMask);
            }
            DestroyIcon(h);
        }
    }
    float x = r.x + 16;
    if (!mark.rgba.empty()) {
        gfx_.drawPixels(L"capture:mark", mark.w, mark.h, mark.rgba.data(), {x, r.y + 10, 34, 34}, true);
        x += 44;
    }
    gfx_.text(L"NorthRay", {x, r.y + 9, 140, 22}, Font::Wordmark, nr::kNightInk, {Align::Left, false});
    gfx_.text(L"CAPTURE", {x + 1, r.y + 32, 140, 14}, Font::Label, nr::kAuroraGreen.withAlpha(0.9f), {Align::Left, false});
    x += 132;
    // The profile and scene collection (one of each until profiles arrive).
    const std::wstring prof = L"Profile: Default   ·   Scenes: " + widen(settings_.sceneCollection);
    gfx_.text(prof, {x, r.y, 360, r.h}, Font::Small, nr::kNightMuted, {Align::Left, false, true, true});
    // Commands on the right, before the window buttons.
    const float capW = drawCaptionButtons(r, true);
    float bx = r.right() - capW - 8;
    auto topButton = [&](const std::string& id, const std::wstring& label, std::function<void()> act, bool on = false) {
        const float w = gfx_.measureWidth(label, Font::Body) + 26;
        bx -= w;
        const Rect b{bx, r.y + 11, w, 32};
        addHit(id, b, std::move(act));
        const float hv = hoverAmount(id);
        if (on || hv > 0.01f) gfx_.fillRounded(b, 8, nr::kNightInk.withAlpha(on ? 0.14f : 0.08f * hv));
        gfx_.text(label, b, Font::Body, nr::kNightInk.withAlpha(0.75f + 0.25f * hv), {Align::Center, false, false, true});
        if (focused(id)) focusRing(b);
        bx -= 4;
    };
    topButton("top:help", L"About", [this] { showAbout(); });
    topButton("top:settings", L"Settings", [this] { openSettings(!settingsOpen_); }, settingsOpen_);
    topButton("top:layout", L"Layout", [this] {
        POINT pt;
        GetCursorPos(&pt);
        layoutMenu(pt);
    });
}

void CaptureWindow::drawPreview(const Rect& r) {
    gfx_.fillRounded(r, 10, pal_.dark ? cjui::hexColor("#05080F") : cjui::hexColor("#0B1120"));
    previewInner_ = {r.x + 12, r.y + 12, std::max(0.0f, r.w - 24), std::max(0.0f, r.h - 44)};
    const Scene* sc = scenes_.scene(scenes_.current());
    const std::wstring name = sc ? widen(sc->name) : L"";
    if (!previewLive_) {
        // The canvas, its shape kept, with a word on why there's no live picture.
        const video::Fit f = video::fitCanvas((float)scenes_.canvasWidth, (float)scenes_.canvasHeight, previewInner_.w, previewInner_.h, 0);
        const Rect c{previewInner_.x + f.x, previewInner_.y + f.y, std::floor(scenes_.canvasWidth * f.scale), std::floor(scenes_.canvasHeight * f.scale)};
        if (c.w > 4 && c.h > 4) {
            gfx_.fill(c, cjui::hexColor("#000000"));
            gfx_.stroke({c.x - 0.5f, c.y - 0.5f, c.w + 1, c.h + 1}, cjui::hexColor("#2A3550"), 1);
            const int pfps = settings_.lowResource ? std::min(settings_.previewFps, 10) : settings_.previewFps;
            std::wstring msg;
            if (!engineError_.empty()) msg = L"Video capture couldn’t start: " + engineError_;
            else if (sc && sc->items.empty()) msg = L"“" + name + L"” is empty.\nAdd a display, a window or a camera with + in Sources.";
            else if (pfps <= 0) msg = L"The preview is off (Settings › Preview frame rate).\nWhat you stream and record isn’t affected.";
            if (!msg.empty())
                gfx_.text(msg, {c.x + 20, c.y + c.h / 2 - 24, c.w - 40, 64}, Font::Small, cjui::hexColor("#8A94AA"), {Align::Center});
        }
    }
    // Under it: the scene, the canvas, the rate, and how the video thread is keeping up.
    std::wstring info = name + L"   ·   " + std::to_wstring(scenes_.canvasWidth) + L" × " + std::to_wstring(scenes_.canvasHeight) +
                        L"   ·   " + std::to_wstring(settings_.outputFps) + L" fps";
    if (engine_.running()) {
        const video::EngineStats st = engine_.stats();
        wchar_t b[96];
        swprintf(b, 96, L"   ·   composing at %.0f fps", st.fps);
        info += b;
        if (st.late > 0) info += L", " + std::to_wstring(st.late) + L" late";
    }
    gfx_.text(info, {r.x + 14, r.bottom() - 28, r.w - 100, 20}, Font::Small, cjui::hexColor("#8A94AA"), {Align::Left, false, true, true});
    gfx_.text(L"PREVIEW", {r.x + 14, r.bottom() - 28, r.w - 28, 20}, Font::Label, cjui::hexColor("#5A6680"), {Align::Right, false, false, true});
}

float CaptureWindow::panelHeader(const Rect& r, int panel, const std::wstring& title) {
    const bool dragging = drag_ == Drag::Panel && dragPanel_ == panel;
    gfx_.text(title, {r.x + 14, r.y, r.w - 60, kHeaderH}, Font::Label, dragging ? pal_.ribbon : pal_.inkMuted, {Align::Left, false, true, true});
    // A grip, so it's clear the header can be dragged.
    const float gx = r.x + 6, gy = r.y + kHeaderH / 2 - 5;
    for (int i = 0; i < 3; ++i) gfx_.circle(gx, gy + i * 5.0f, 0.9f, pal_.inkFaint, true);
    gfx_.line(r.x + 10, r.y + kHeaderH - 0.5f, r.right() - 10, r.y + kHeaderH - 0.5f, pal_.rule);
    return kHeaderH;
}

void CaptureWindow::note(const Rect& r, const std::wstring& title, const std::wstring& text) {
    float y = r.y + 14;
    if (!title.empty()) {
        gfx_.text(title, {r.x + 14, y, r.w - 28, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        y += 26;
    }
    gfx_.text(text, {r.x + 14, y, r.w - 28, r.bottom() - y - 8}, Font::Small, pal_.inkMuted);
}

void CaptureWindow::drawPanel(int panel, const Rect& r) {
    gfx_.fillRounded(r, 10, pal_.surface);
    gfx_.strokeRounded(r, 10, drag_ == Drag::Panel && dragPanel_ == panel ? pal_.ribbon : pal_.rule, 1);
    std::wstring title = widen(Layout::panelName(panel));
    for (auto& c : title) c = (wchar_t)towupper(c);
    const float hh = panelHeader(r, panel, title);
    const Rect body{r.x, r.y + hh, r.w, r.h - hh};
    gfx_.pushClip(body);
    setHitClip(body);
    switch (panel) {
        case Layout::Scenes: drawScenes(body); break;
        case Layout::Sources: drawSources(body); break;
        case Layout::Mixer: drawMixer(body); break;
        case Layout::Controls: drawControls(body); break;
        default: break;
    }
    clearHitClip();
    gfx_.popClip();
    // Header buttons (drawn after the body, outside its clip).
    auto plus = [&](const std::string& id, std::function<void()> act, bool enabled = true) {
        const Rect b{r.right() - 40, r.y + 5, 30, 28};
        if (enabled) addHit(id, b, std::move(act));
        const float hv = enabled ? hoverAmount(id) : 0;
        if (hv > 0.01f) gfx_.fillRounded(b, 6, pal_.ink.withAlpha(0.07f * hv));
        const Color c = enabled ? pal_.inkMuted.mix(pal_.ink, hv) : pal_.inkFaint;
        gfx_.line(b.x + 15, b.y + 8, b.x + 15, b.y + 20, c, 1.5f);
        gfx_.line(b.x + 9, b.y + 14, b.x + 21, b.y + 14, c, 1.5f);
        if (focused(id)) focusRing(b);
    };
    if (panel == Layout::Scenes) plus("scenes:add", [this] { newScene(); });
    if (panel == Layout::Sources)
        plus("sources:add", [this] {
            POINT pt;
            GetCursorPos(&pt);
            addSourceMenu(pt);
        });
}

void CaptureWindow::drawScenes(const Rect& b) {
    float y = b.y + 6;
    const auto& all = scenes_.scenes();
    for (size_t i = 0; i < all.size(); ++i) {
        const Scene& s = all[i];
        const Rect row{b.x + 6, y, b.w - 12, 32};
        if (row.y > b.bottom()) break;
        sceneRows_.push_back({s.id, row});
        const std::string id = "scene:" + s.id;
        addHit(id, row, nullptr, false);
        const bool on = s.id == scenes_.current();
        const float hv = hoverAmount(id);
        if (on) {
            gfx_.fillRounded(row, 7, pal_.ribbon.withAlpha(pal_.dark ? 0.20f : 0.14f));
            gfx_.fillRounded({row.x, row.y + 8, 3, row.h - 16}, 1.5f, pal_.ribbon);
        } else if (hv > 0.01f) {
            gfx_.fillRounded(row, 7, pal_.ink.withAlpha(0.05f * hv));
        }
        gfx_.text(widen(s.name), {row.x + 14, row.y, row.w - 60, row.h}, on ? Font::BodyStrong : Font::Body, pal_.ink, {Align::Left, false, true, true});
        if (i < 9)
            gfx_.text(L"Ctrl+" + std::to_wstring(i + 1), {row.x, row.y, row.w - 10, row.h}, Font::Small, pal_.inkFaint.withAlpha(0.6f + 0.4f * hv),
                      {Align::Right, false, false, true});
        y += 34;
    }
}

void CaptureWindow::drawSources(const Rect& b) {
    const Scene* s = scenes_.scene(scenes_.current());
    if (!s || s->items.empty()) {
        note(b, L"Nothing in this scene yet",
             L"Use + above to add a whole display, a window (a game, a browser) or a camera or capture card. "
             L"Images, text and sound arrive in later versions; + shows when.");
        return;
    }
    float y = b.y + 6;
    for (auto it = s->items.rbegin(); it != s->items.rend(); ++it) {   // top of the scene first
        const Source* src = scenes_.source(it->source);
        if (!src) continue;
        const Rect row{b.x + 6, y, b.w - 12, 44};
        if (row.y > b.bottom()) break;
        sourceRows_.push_back({it->id, row});
        const std::string id = "src:" + it->id;
        addHit(id, row, nullptr, false);
        const bool on = it->id == selected_;
        const float hv = hoverAmount(id);
        if (on) {
            gfx_.fillRounded(row, 7, pal_.ribbon.withAlpha(pal_.dark ? 0.20f : 0.14f));
            gfx_.fillRounded({row.x, row.y + 10, 3, row.h - 20}, 1.5f, pal_.ribbon);
        } else if (hv > 0.01f || it->id == hoverItem_) {
            gfx_.fillRounded(row, 7, pal_.ink.withAlpha(0.05f * std::max(hv, it->id == hoverItem_ ? 0.7f : 0.0f)));
        }
        const cjui::Color ink = it->visible ? pal_.ink : pal_.inkFaint;
        gfx_.text(widen(src->name), {row.x + 14, row.y + 4, row.w - 84, 20}, on ? Font::BodyStrong : Font::Body, ink, {Align::Left, false, true, true});
        bool problem = false;
        std::wstring st = sourceStatus(src->id, &problem);
        if (!it->visible) st = L"Hidden" + (st.empty() ? L"" : L" · " + st);
        gfx_.text(st, {row.x + 14, row.y + 23, row.w - 84, 17}, Font::Small, problem ? cjui::hexColor("#E0566A") : pal_.inkMuted,
                  {Align::Left, false, true, true});
        // Show/hide and lock.
        const Id item = it->id;
        const Rect eye{row.right() - 60, row.y + 9, 26, 26}, lock{row.right() - 31, row.y + 9, 26, 26};
        addHit(id + ":eye", eye, [this, item, vis = it->visible] {
            scenes_.setItemVisible(scenes_.current(), item, !vis);
            scenesChanged();
        });
        addHit(id + ":lock", lock, [this, item, locked = it->locked] {
            scenes_.setItemLocked(scenes_.current(), item, !locked);
            scenesChanged();
        });
        const float he = hoverAmount(id + ":eye"), hl = hoverAmount(id + ":lock");
        if (he > 0.01f) gfx_.fillRounded(eye, 6, pal_.ink.withAlpha(0.08f * he));
        if (hl > 0.01f) gfx_.fillRounded(lock, 6, pal_.ink.withAlpha(0.08f * hl));
        const cjui::Color ic = pal_.inkMuted.mix(pal_.ink, std::max(he, 0.0f));
        // The eye: an outline with a pupil; crossed out when hidden.
        gfx_.strokeRounded({eye.x + 5, eye.y + 8, 16, 10}, 5, ic, 1.4f);
        gfx_.circle(eye.x + 13, eye.y + 13, 2.4f, ic, true);
        if (!it->visible) gfx_.line(eye.x + 5, eye.y + 20, eye.x + 21, eye.y + 6, ic, 1.6f);
        // The lock: solid when locked, faint and open when not.
        const cjui::Color lc = it->locked ? pal_.ink : pal_.inkFaint.mix(pal_.ink, hl);
        gfx_.strokeRounded({lock.x + 9, lock.y + (it->locked ? 5 : 3), 8, 9}, 4, lc, 1.4f);
        gfx_.fillRounded({lock.x + 7, lock.y + 11, 12, 9}, 2, it->locked ? lc : pal_.surface);
        if (!it->locked) gfx_.strokeRounded({lock.x + 7, lock.y + 11, 12, 9}, 2, lc, 1.4f);
        if (focused(id + ":eye")) focusRing(eye);
        if (focused(id + ":lock")) focusRing(lock);
        y += 46;
    }
}

void CaptureWindow::drawMixer(const Rect& b) {
    note(b, L"No audio sources yet",
         L"Coming in Milestone 3 (in development): microphones, desktop audio and individual applications, each with its own fader and meter. "
         L"For example, Minecraft and your microphone on stream while Spotify and Discord stay in your headphones only.");
}

void CaptureWindow::drawControls(const Rect& b) {
    struct C { const wchar_t* label; const wchar_t* when; };
    const C cs[] = {{L"Start streaming", L"Milestone 6"}, {L"Start recording", L"Milestone 5"}, {L"Replay buffer", L"Milestone 11"},
                    {L"Studio mode", L"Milestone 8"},     {L"Virtual camera", L"Milestone 11"}};
    float y = b.y + 10;
    const float w = b.w - 24;
    for (const C& c : cs) {
        if (y + 34 > b.bottom()) break;
        const Rect r{b.x + 12, y, w, 34};
        gfx_.fillRounded(r, 8, pal_.ink.withAlpha(pal_.dark ? 0.05f : 0.035f));
        gfx_.strokeRounded(r, 8, pal_.rule, 1);
        gfx_.text(c.label, {r.x + 12, r.y, r.w - 24, r.h}, Font::Body, pal_.inkFaint, {Align::Left, false, true, true});
        if (w > 220) gfx_.text(c.when, {r.x, r.y, r.w - 12, r.h}, Font::Small, pal_.inkFaint, {Align::Right, false, false, true});
        y += 40;
    }
    if (y + 40 < b.bottom())
        gfx_.text(L"Not built yet: each starts working in the milestone shown.", {b.x + 14, y + 2, b.w - 28, 40}, Font::Small, pal_.inkFaint);
}

void CaptureWindow::drawStatusBar(const Rect& r) {
    gfx_.fill(r, pal_.surface);
    gfx_.line(r.x, r.y + 0.5f, r.right(), r.y + 0.5f, pal_.rule);
    float x = r.x + 14;
    const std::wstring lowLabel = L"LOW-RESOURCE MODE";
    const float limit = r.right() - 14 - (settings_.lowResource ? gfx_.measureWidth(lowLabel, Font::Label) + 20 : 0);
    auto item = [&](const std::wstring& label, const std::wstring& value, const Color* dot = nullptr) {
        // Items that don't fit are left off (the later ones matter least).
        const float need = (dot ? 14 : 0) + gfx_.measureWidth(label, Font::Small) + 6 + gfx_.measureWidth(value, Font::Small);
        if (x + need > limit) { x = limit; return; }
        if (dot) {
            gfx_.circle(x + 4, r.y + r.h / 2, 3.5f, *dot, true);
            x += 14;
        }
        const float lw = gfx_.measureWidth(label, Font::Small);
        gfx_.text(label, {x, r.y, lw + 4, r.h}, Font::Small, pal_.inkFaint, {Align::Left, false, false, true});
        x += lw + 6;
        const float vw = gfx_.measureWidth(value, Font::Small);
        gfx_.text(value, {x, r.y, vw + 4, r.h}, Font::Small, pal_.inkMuted, {Align::Left, false, false, true});
        x += vw + 22;
    };
    item(L"Stream", L"off", &pal_.inkFaint);
    item(L"Recording", L"off", &pal_.inkFaint);
    item(L"CPU", percent(usage_.processCpu) + L" (system " + percent(usage_.systemCpu) + L")");
    item(L"Memory", megabytes(usage_.privateBytes ? usage_.privateBytes : usage_.workingSet));
    {
        std::wstring gpu = L"not running";
        if (engine_.running()) {
            const video::EngineStats st = engine_.stats();
            wchar_t b[64];
            if (st.gpuMs >= 0) swprintf(b, 64, L"%.1f ms a frame", st.gpuMs);
            else swprintf(b, 64, L"timing not available");
            gpu = b;
        }
        item(L"GPU", gpu);
    }
    std::wstring enc = encodersKnown_ ? L"none found" : L"checking…";
    if (encodersKnown_) {
        int hw = 0;
        std::wstring first;
        for (auto& e : encoders_)
            if (e.hardware && (e.codec == "H.264" || e.codec == "HEVC")) {
                ++hw;
                if (first.empty()) first = e.vendor.empty() ? e.name : e.vendor + L" " + widen(e.codec);
            }
        if (hw) enc = first + (hw > 1 ? L" + " + std::to_wstring(hw - 1) + L" more" : L"");
        else if (!encoders_.empty()) enc = L"software only";
    }
    item(L"Hardware encoder", enc);
    if (settings_.lowResource) gfx_.text(lowLabel, {r.x, r.y, r.w - 14, r.h}, Font::Label, pal_.ribbon, {Align::Right, false, false, true});
}

void CaptureWindow::toggle(const std::string& id, float x, float y, bool on, std::function<void()> act) {
    const Rect t{x, y, 40, 22};
    addHit(id, t, std::move(act));
    const float k = anim(id + "#on", on ? 1.0f : 0.0f);
    gfx_.fillRounded(t, 11, pal_.inkFaint.withAlpha(0.35f).mix(pal_.ribbon, k));
    gfx_.circle(t.x + 11 + 18 * k, t.y + 11, 8, pal_.dark ? cjui::hexColor("#0B1120") : cjui::hexColor("#FFFFFF"), true);
    if (focused(id)) focusRing(t);
}

void CaptureWindow::drawSettings(const Rect& area) {
    gfx_.fill(area, pal_.paper);
    const float colW = std::min(760.0f, area.w - 48);
    const float x = area.x + (area.w - colW) / 2;
    const Rect clip{area.x, area.y, area.w, area.h};
    const float top = beginScroll(clip);
    float y = top + 24;
    gfx_.text(L"Settings", {x, y, colW, 34}, Font::Title, pal_.ink, {Align::Left, false});
    button("set:close", {x + colW - 110, y + 2, 110, 34}, L"Done", [this] { openSettings(false); }, true);
    y += 54;
    auto section = [&](const std::wstring& t) {
        y += 10;
        y += sectionLabel(x, y, colW, t) + 10;
    };
    auto row = [&](const std::wstring& label, const std::wstring& help, float controlW) {
        gfx_.text(label, {x, y, colW - controlW - 20, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        float h = 24;
        if (!help.empty()) h += gfx_.text(help, {x, y + 24, colW - controlW - 20, 80}, Font::Small, pal_.inkMuted);
        return h;
    };

    section(L"APPEARANCE");
    {
        const float h = row(L"Theme", L"", 300);
        segmented("set:theme", {x + colW - 300, y - 2, 300, 32}, {L"As Windows", L"Light", L"Dark"}, (int)settings_.theme, [this](int i) {
            settings_.theme = (Theme)i;
            settings_.save(settingsPath_);
            applyTheme();
        });
        y += std::max(h, 34.0f) + 14;
    }

    section(L"PERFORMANCE");
    {
        const float h = row(L"Low-resource mode",
                            L"Keeps the interface, preview and meters to a minimum so more of the computer goes to the game and the encoder. "
                            L"It never lowers the quality of what you stream or record.",
                            60);
        toggle("set:low", x + colW - 44, y, settings_.lowResource, [this] {
            settings_.lowResource = !settings_.lowResource;
            settings_.save(settingsPath_);
            SetTimer(hwnd(), kStatsTimer, settings_.lowResource ? 2000 : 1000, nullptr);
            log::info(std::string("Low-resource mode ") + (settings_.lowResource ? "on" : "off"));
        });
        y += std::max(h, 34.0f) + 14;
        const float h2 = row(L"Preview frame rate", L"How often the preview redraws. Off saves the most; streams and recordings aren\u2019t affected.", 300);
        const int fps[] = {0, 10, 30, 60};
        int sel = 2;
        for (int i = 0; i < 4; ++i) if (settings_.previewFps == fps[i]) sel = i;
        segmented("set:pfps", {x + colW - 300, y - 2, 300, 32}, {L"Off", L"10", L"30", L"60"}, sel, [this](int i) {
            const int v[] = {0, 10, 30, 60};
            settings_.previewFps = v[i];
            settings_.save(settingsPath_);
        });
        y += std::max(h2, 34.0f) + 14;
    }

    section(L"VIDEO");
    {
        const float h = row(L"Canvas", L"The picture every scene is laid out on. Vertical (9:16) is for Shorts, TikTok and the like.", 330);
        struct Res { int w, h; const wchar_t* label; };
        const Res rs[] = {{1920, 1080, L"1080p"}, {1280, 720, L"720p"}, {2560, 1440, L"1440p"}, {1080, 1920, L"Vertical"}};
        int sel = -1;
        for (int i = 0; i < 4; ++i) if (scenes_.canvasWidth == rs[i].w && scenes_.canvasHeight == rs[i].h) sel = i;
        segmented("set:canvas", {x + colW - 330, y - 2, 330, 32}, {rs[0].label, rs[1].label, rs[2].label, rs[3].label}, sel, [this](int i) {
            const int W[] = {1920, 1280, 2560, 1080}, Hh[] = {1080, 720, 1440, 1920};
            scenes_.canvasWidth = W[i];
            scenes_.canvasHeight = Hh[i];
            scenesChanged();
            saveScenes();
        });
        y += std::max(h, 34.0f) + 14;
        const float h2 = row(L"Frame rate", L"For streams and recordings (from Milestone 5). 30 is kinder to older computers.", 300);
        const int fps[] = {24, 30, 50, 60};
        int fsel = 3;
        for (int i = 0; i < 4; ++i) if (settings_.outputFps == fps[i]) fsel = i;
        segmented("set:ofps", {x + colW - 300, y - 2, 300, 32}, {L"24", L"30", L"50", L"60"}, fsel, [this](int i) {
            const int v[] = {24, 30, 50, 60};
            settings_.outputFps = v[i];
            settings_.save(settingsPath_);
            engine_.setOutputFps(v[i]);
        });
        y += std::max(h2, 34.0f) + 14;
    }

    section(L"ENCODERS ON THIS PC");
    {
        if (!encodersKnown_) {
            gfx_.text(L"Checking…", {x, y, colW, 22}, Font::Body, pal_.inkMuted);
            y += 30;
        } else if (encoders_.empty()) {
            y += gfx_.text(L"Windows didn't offer any encoders. Recording and streaming need one; a graphics driver update usually adds the hardware ones.",
                           {x, y, colW, 60}, Font::Body, pal_.inkMuted) + 12;
        } else {
            y += gfx_.text(L"Found through Windows' Media Foundation. Recording and streaming (Milestones 5 and 6) will offer only these.",
                           {x, y, colW, 40}, Font::Small, pal_.inkMuted) + 10;
            for (auto& e : encoders_) {
                const Rect rr{x, y, colW, 34};
                gfx_.fillRounded(rr, 8, pal_.surface);
                gfx_.text(widen(e.codec), {rr.x + 12, rr.y, 70, rr.h}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
                gfx_.text(e.name, {rr.x + 90, rr.y, rr.w - 220, rr.h}, Font::Body, pal_.ink, {Align::Left, false, true, true});
                float tw = 0;
                tag(rr.right() - 110, rr.y + 8, e.hardware ? L"HARDWARE" : L"SOFTWARE", e.hardware ? pal_.ribbon : pal_.inkFaint, &tw);
                y += 40;
            }
        }
        y += 8;
    }

    section(L"DIAGNOSTICS");
    {
        y += gfx_.text(L"Logs are kept for 14 days on this PC. Stream keys and passwords are never written to them.",
                       {x, y, colW, 40}, Font::Small, pal_.inkMuted) + 10;
        button("set:logs", {x, y, 170, 34}, L"Open the logs folder", [this] {
            ShellExecuteW(hwnd(), L"open", log::folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
        button("set:diag", {x + 180, y, 180, 34}, L"Copy diagnostics", [this] { copyDiagnostics(); });
        y += 50;
    }

    section(L"ABOUT");
    {
        const std::wstring t = L"NorthRay Capture " CJ_VERSION_WSTR L", in development (Milestone 2: video capture). "
                               L"Developed by " CJ_DEVELOPER_WSTR L" · NorthRaySoftware@protonmail.com";
        y += gfx_.text(t, {x, y, colW, 60}, Font::Small, pal_.inkMuted) + 30;
    }
    endScroll(y - top + 20);
}

}  // namespace ncap

#include "files_window.hpp"

#include "nrf/paths.hpp"
#include "resource.h"
#include "version.hpp"

#include "cjui/northray.hpp"

#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <set>

namespace nrf {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::Rect;
using cjui::narrow;
using cjui::widen;
namespace nr = cjui::northray;

namespace {

constexpr float kSidebarW = 236;
constexpr float kToolbarH = 56;
constexpr float kHeaderH = 32;
constexpr UINT_PTR kSpinTimer = 1;

enum NavIcon { IconBack, IconForward, IconUp, IconRefresh, IconDetails, IconTiles, IconMenu };

enum : UINT {
    CmdViewDetails = 100, CmdViewIcons, CmdSortName, CmdSortDate, CmdSortType, CmdSortSize, CmdSortAsc, CmdSortDesc,
    CmdHidden, CmdProtected, CmdThemeSystem, CmdThemeLight, CmdThemeDark, CmdAbout, CmdOpen, CmdCopyPath, CmdRefresh,
};

COLORREF toColorRef(Color c) {
    auto b = [](float v) { return (BYTE)std::clamp((int)std::lround(v * 255), 0, 255); };
    return RGB(b(c.r), b(c.g), b(c.b));
}

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

bool samePath(const std::wstring& a, const std::wstring& b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL; }

LRESULT CALLBACK quietEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == 1 /* Ctrl+A */)) {
        if (w == 1) SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return DefSubclassProc(h, m, w, l);
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

FilesWindow::FilesWindow(Settings& settings, std::wstring settingsPath) : settings_(settings), settingsPath_(std::move(settingsPath)) {}

FilesWindow::~FilesWindow() {
    if (addressFont_) DeleteObject(addressFont_);
    if (addressBrush_) DeleteObject(addressBrush_);
}

bool FilesWindow::create(HINSTANCE inst, int show, const std::wstring& startPath) {
    cjui::WindowSpec spec;
    spec.instance = inst;
    spec.className = kClassName;
    spec.title = L"NorthRay Files";
    spec.iconId = IDI_APP;
    spec.width = 1180;
    spec.height = 760;
    spec.minWidth = 640;
    spec.minHeight = 420;
    spec.show = show;
    spec.style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    spec.customFrame = true;   // minimise, maximise and close sit at the end of the toolbar
    spec.captionHeight = kToolbarH;
    if (!createWindow(spec)) return false;

    address_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd(), nullptr, inst, nullptr);
    SetWindowSubclass(address_, quietEditProc, 1, 0);
    SendMessageW(address_, EM_SETLIMITTEXT, 32767, 0);

    thumbs_ = std::make_unique<Thumbnails>(hwnd());
    applyTheme();
    places_ = knownPlaces();
    ShowWindow(hwnd(), show);
    UpdateWindow(hwnd());

    // Where to start: the folder asked for, else the last one, else Home.
    std::wstring start = startPath;
    if (start.empty()) start = settings_.lastFolder;
    if (start == L"::thispc") { go({true, L""}, false); return true; }
    if (start.empty() && !places_.empty()) start = places_[0].path;
    auto n = start.empty() ? std::nullopt : normalizePath(start, L"");
    if (n) go({false, *n}, false);
    else go({true, L""}, false);
    // The drives for the sidebar.
    listDrivesAsync(hwnd(), 0, std::make_shared<std::atomic<std::uint64_t>>(0));
    return true;
}

void FilesWindow::onDestroy() {
    latest_->store(~0ull);   // any worker still reading gives up
    settings_.lastFolder = here_.thisPC ? L"::thispc" : here_.path;
    settings_.save(settingsPath_);
}

void FilesWindow::applyTheme() {
    const bool dark = settings_.theme == Theme::Dark || (settings_.theme == Theme::System && cjui::systemPrefersDark());
    applyPalette(nr::palette(dark));
    if (addressBrush_) DeleteObject(addressBrush_);
    addressBrush_ = CreateSolidBrush(toColorRef(pal_.sunken));
    if (address_) InvalidateRect(address_, nullptr, TRUE);
    invalidate();
}

// ---------------------------------------------------------------- navigation

void FilesWindow::go(const Location& to, bool remember, const std::wstring& select) {
    if (editing_) endEditingAddress(false);
    if (remember && loadedOnce_ && !(to == here_)) {
        backStack_.push_back(here_);
        if (backStack_.size() > 200) backStack_.erase(backStack_.begin());
        forwardStack_.clear();
    }
    loadedOnce_ = true;
    if (thumbs_) thumbs_->forgetQueue();
    here_ = to;
    listed_.clear();
    shown_.clear();
    sel_.reset(0);
    error_ = ListError::None;
    errorCode_ = 0;
    partial_ = false;
    selectAfterLoad_ = select;
    typed_.clear();
    resetScroll();
    loading_ = true;
    loadStarted_ = GetTickCount();
    latest_->store(++request_);
    if (to.thisPC) listDrivesAsync(hwnd(), request_, latest_);
    else listFolderAsync(hwnd(), to.path, request_, latest_);
    SetTimer(hwnd(), kSpinTimer, 120, nullptr);

    const std::wstring name = to.thisPC ? L"This PC" : (isDriveRoot(to.path) ? to.path.substr(0, 2) : nameOf(to.path));
    const std::wstring title = name + L" – NorthRay Files";
    if (title != windowTitle_) {
        windowTitle_ = title;
        SetWindowTextW(hwnd(), title.c_str());
    }
    invalidate();
}

void FilesWindow::back() {
    if (backStack_.empty()) return;
    forwardStack_.push_back(here_);
    const Location from = here_, to = backStack_.back();
    backStack_.pop_back();
    // Coming back out of a folder: it's selected, as in File Explorer.
    const bool outOf = !from.thisPC && !to.thisPC && samePath(parentOf(from.path), to.path);
    go(to, false, outOf ? nameOf(from.path) : L"");
}

void FilesWindow::forward() {
    if (forwardStack_.empty()) return;
    backStack_.push_back(here_);
    const Location to = forwardStack_.back();
    forwardStack_.pop_back();
    go(to, false);
}

void FilesWindow::up() {
    if (here_.thisPC) return;
    if (isRoot(here_.path)) {
        if (isDriveRoot(here_.path)) go({true, L""}, true, here_.path);
        return;
    }
    go({false, parentOf(here_.path)}, true, nameOf(here_.path));
}

void FilesWindow::refresh() {
    std::wstring keep;
    if (sel_.focus() >= 0 && (std::size_t)sel_.focus() < shown_.size()) keep = shown_[(std::size_t)sel_.focus()].name;
    go(here_, false, keep);
}

std::wstring FilesWindow::rowPath(std::size_t row) const {
    if (here_.thisPC || row >= shown_.size()) return L"";
    return join(here_.path, shown_[row].name);
}

void FilesWindow::open(std::size_t row) {
    if (here_.thisPC || row >= shown_.size()) return;
    const Entry& e = shown_[row];
    const std::wstring path = rowPath(row);
    if (e.folder()) { goPath(path); return; }
    const std::wstring problem = openWithDefaultApp(path, hwnd());
    if (!problem.empty()) flash(problem, 8);
}

void FilesWindow::openSelection() {
    const auto rows = sel_.indices();
    if (rows.empty()) return;
    if (rows.size() == 1) { open(rows[0]); return; }
    std::vector<std::size_t> files;
    for (std::size_t r : rows)
        if (!shown_[r].folder()) files.push_back(r);
    if (files.empty()) { flash(L"Select one folder to open it.", 5); return; }
    if (files.size() > 10) {
        const std::wstring q = L"Open " + std::to_wstring(files.size()) + L" files, each in its own app?";
        if (MessageBoxW(hwnd(), q.c_str(), L"NorthRay Files", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    }
    for (std::size_t r : files) open(r);
}

// ---------------------------------------------------------------- results

void FilesWindow::receive(ListBatch* b) {
    if (b->request != request_ || here_.thisPC) return;
    if (b->error == ListError::NotAFolder) {
        // A file's path: its folder, with it selected.
        here_.path = parentOf(here_.path);
        go(here_, false, b->fileToSelect);
        return;
    }
    if (!b->canonical.empty() && b->canonical != here_.path && samePath(b->canonical, here_.path)) {
        here_.path = b->canonical;
        const std::wstring title = (isDriveRoot(here_.path) ? here_.path.substr(0, 2) : nameOf(here_.path)) + L" \u2013 NorthRay Files";
        windowTitle_ = title;
        SetWindowTextW(hwnd(), title.c_str());
    }
    for (Entry& e : b->entries) listed_.push_back(std::move(e));
    if (b->done) {
        loading_ = false;
        if (b->error != ListError::None) {
            if (listed_.empty()) { error_ = b->error; errorCode_ = b->code; }
            else { partial_ = true; errorCode_ = b->code; }
        }
    }
    showHere();
}

void FilesWindow::receive(DriveBatch* b) {
    drives_ = std::move(b->drives);
    if (here_.thisPC && b->request == request_) loading_ = false;
    if (here_.thisPC && !selectAfterLoad_.empty()) {
        selectAfterLoad_.clear();
    }
    invalidate();
}

void FilesWindow::showHere() {
    // Keep what was selected (by name) through re-sorting and new rows.
    std::set<std::wstring> keep;
    for (std::size_t i : sel_.indices()) keep.insert(shown_[i].name);
    std::wstring focusName;
    if (sel_.focus() >= 0 && (std::size_t)sel_.focus() < shown_.size()) focusName = shown_[(std::size_t)sel_.focus()].name;

    const Visibility vis{settings_.showHidden, settings_.showProtected};
    shown_.clear();
    shown_.reserve(listed_.size());
    for (const Entry& e : listed_)
        if (isVisible(e, vis)) shown_.push_back(e);
    sortEntries(shown_, {(SortKey)settings_.sortKey, settings_.sortDescending});

    sel_.reset(shown_.size());
    for (std::size_t i = 0; i < shown_.size(); ++i) {
        if (keep.count(shown_[i].name)) sel_.click(i, true, false);
    }
    for (std::size_t i = 0; i < shown_.size() && !focusName.empty(); ++i)
        if (shown_[i].name == focusName) { sel_.moveTo(i, true, false); break; }
    if (!selectAfterLoad_.empty()) {
        for (std::size_t i = 0; i < shown_.size(); ++i)
            if (CompareStringOrdinal(shown_[i].name.c_str(), -1, selectAfterLoad_.c_str(), -1, TRUE) == CSTR_EQUAL) {
                sel_.select(i);
                ensureVisible(i);
                if (!loading_) selectAfterLoad_.clear();
                break;
            }
        if (!loading_) selectAfterLoad_.clear();
    }
    invalidate();
}

// ---------------------------------------------------------------- the address field

void FilesWindow::beginEditingAddress() {
    editing_ = true;
    SetWindowTextW(address_, here_.thisPC ? L"This PC" : here_.path.c_str());
    invalidate();
    UpdateWindow(hwnd());
    ShowWindow(address_, SW_SHOW);
    SetFocus(address_);
    SendMessageW(address_, EM_SETSEL, 0, -1);
}

void FilesWindow::endEditingAddress(bool goThere) {
    if (!editing_) return;
    editing_ = false;
    std::wstring text;
    if (goThere) {
        const int n = GetWindowTextLengthW(address_);
        text.assign((size_t)n, L'\0');
        if (n) GetWindowTextW(address_, text.data(), n + 1);
    }
    ShowWindow(address_, SW_HIDE);
    if (GetFocus() == address_) SetFocus(hwnd());
    invalidate();
    if (!goThere) return;
    std::wstring t = text;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    if (_wcsicmp(t.c_str(), L"This PC") == 0) { go({true, L""}); return; }
    // %USERPROFILE% and the like, as in File Explorer.
    wchar_t expanded[32768];
    const DWORD n = ExpandEnvironmentStringsW(t.c_str(), expanded, (DWORD)std::size(expanded));
    if (n > 0 && n <= std::size(expanded)) t = expanded;
    std::wstring problem;
    auto path = normalizePath(t, here_.thisPC ? L"" : here_.path, &problem);
    if (!path) { flash(problem, 8); return; }
    goPath(*path);
}

bool FilesWindow::preTranslate(MSG& msg) {
    if (msg.hwnd == address_ && msg.message == WM_KEYDOWN) {
        if (msg.wParam == VK_RETURN) { endEditingAddress(true); return true; }
        if (msg.wParam == VK_ESCAPE) { endEditingAddress(false); return true; }
    }
    return false;
}

// ---------------------------------------------------------------- keyboard and mouse

void FilesWindow::ensureVisible(std::size_t row) {
    if (listClip_.h <= 0) return;
    const float rowH = settings_.view == ViewMode::Details ? rowH_ : tileH_;
    const std::size_t line = settings_.view == ViewMode::Details ? row : row / (std::size_t)std::max(1, columns_);
    const float top = listTop_ + (float)line * rowH;
    if (top < listClip_.y) scrollBy(top - listClip_.y - 4);
    else if (top + rowH > listClip_.bottom()) scrollBy(top + rowH - listClip_.bottom() + 4);
}

void FilesWindow::typeAhead(wchar_t c) {
    if (here_.thisPC || shown_.empty()) return;
    const DWORD now = GetTickCount();
    if (now - typedAt_ > 1000) typed_.clear();
    typedAt_ = now;
    typed_ += (wchar_t)towlower(c);
    // The first name starting with what's been typed, after the focused row
    // when the same letter is pressed again.
    const bool repeat = typed_.size() > 1 && std::all_of(typed_.begin(), typed_.end(), [&](wchar_t x) { return x == typed_[0]; });
    const std::wstring want = repeat ? typed_.substr(0, 1) : typed_;
    const std::size_t n = shown_.size();
    const std::size_t start = repeat && sel_.focus() >= 0 ? (std::size_t)sel_.focus() + 1 : (sel_.focus() >= 0 && typed_.size() > 1 ? (std::size_t)sel_.focus() : 0);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t i = (start + k) % n;
        const std::wstring& name = shown_[i].name;
        if (name.size() >= want.size() && CompareStringOrdinal(name.c_str(), (int)want.size(), want.c_str(), (int)want.size(), TRUE) == CSTR_EQUAL) {
            sel_.select(i);
            ensureVisible(i);
            invalidate();
            return;
        }
    }
}

bool FilesWindow::onKeyDown(WPARAM vk, bool ctrl, bool shift) {
    const std::size_t n = here_.thisPC ? 0 : shown_.size();
    const bool icons = settings_.view == ViewMode::Icons;
    const int cols = icons ? std::max(1, columns_) : 1;
    const int focus = sel_.focus();
    const float rowH = icons ? tileH_ : rowH_;
    const int page = std::max(1, (int)(listClip_.h / std::max(1.0f, rowH)) - 1) * cols;
    auto moveTo = [&](long long i) {
        if (n == 0) return;
        i = std::clamp<long long>(i, 0, (long long)n - 1);
        sel_.moveTo((std::size_t)i, ctrl, shift);
        ensureVisible((std::size_t)i);
    };
    if (ctrl && !shift) {
        switch (vk) {
            case 'A': sel_.selectAll(); return true;
            case 'L': beginEditingAddress(); return true;
            case 'R': refresh(); return true;
            case VK_SPACE: sel_.toggleFocused(); return true;
            default: break;
        }
    }
    if (ctrl && shift) {
        switch (vk) {
            case 'C': copyPaths(); return true;
            case '6': setView(ViewMode::Details); return true;
            case '2': setView(ViewMode::Icons); return true;
            default: break;
        }
    }
    switch (vk) {
        case VK_F4: beginEditingAddress(); return true;
        case VK_F5: refresh(); return true;
        case VK_BACK: back(); return true;
        case VK_BROWSER_BACK: back(); return true;
        case VK_BROWSER_FORWARD: forward(); return true;
        case VK_BROWSER_REFRESH: refresh(); return true;
        case VK_RETURN: openSelection(); return true;
        case VK_ESCAPE: sel_.clear(); return true;
        case VK_DOWN: moveTo(focus < 0 ? 0 : focus + cols); return true;
        case VK_UP: moveTo(focus < 0 ? 0 : focus - cols); return true;
        case VK_RIGHT: if (icons) { moveTo(focus < 0 ? 0 : focus + 1); return true; } break;
        case VK_LEFT: if (icons) { moveTo(focus < 0 ? 0 : focus - 1); return true; } break;
        case VK_HOME: moveTo(0); return true;
        case VK_END: moveTo((long long)n - 1); return true;
        case VK_NEXT: moveTo(focus < 0 ? 0 : focus + page); return true;
        case VK_PRIOR: moveTo(focus < 0 ? 0 : focus - page); return true;
        case VK_APPS: {
            POINT pt{(LONG)((listClip_.x + 40) * gfx_.dpiScale()), (LONG)((listClip_.y + 40) * gfx_.dpiScale())};
            ClientToScreen(hwnd(), &pt);
            showItemMenu(pt);
            return true;
        }
        default: break;
    }
    return false;
}

bool FilesWindow::onMouseDown(float x, float y) {
    if (editing_) endEditingAddress(false);
    if (here_.thisPC || !listClip_.contains(x, y) || error_ != ListError::None) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    long long row = -1;
    if (settings_.view == ViewMode::Details) {
        const long long r = (long long)std::floor((y - listTop_) / rowH_);
        if (r >= 0 && r < (long long)shown_.size() && x < listClip_.right() - 8) row = r;
    } else {
        const long long line = (long long)std::floor((y - listTop_) / tileH_);
        const long long col = (long long)std::floor((x - listX_) / tileW_);
        if (line >= 0 && col >= 0 && col < columns_) {
            const long long r = line * columns_ + col;
            if (r < (long long)shown_.size()) row = r;
        }
    }
    if (row < 0) {
        sel_.clickEmpty(ctrl);
        lastClickRow_ = -1;
        return true;
    }
    // A second click on the same row, soon and close by: open it.
    const DWORD now = GetMessageTime();
    POINT pt;
    GetCursorPos(&pt);
    const bool dbl = row == lastClickRow_ && now - lastClickTime_ <= GetDoubleClickTime() &&
                     std::abs(pt.x - lastClickPt_.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                     std::abs(pt.y - lastClickPt_.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
    if (dbl && !ctrl && !shift) {
        lastClickRow_ = -1;
        sel_.select((std::size_t)row);
        open((std::size_t)row);
        return true;
    }
    lastClickRow_ = (int)row;
    lastClickTime_ = now;
    lastClickPt_ = pt;
    sel_.click((std::size_t)row, ctrl, shift);
    return true;
}

bool FilesWindow::onRightClick(float x, float y) {
    if (here_.thisPC || !listClip_.contains(x, y) || error_ != ListError::None) return false;
    long long row = -1;
    if (settings_.view == ViewMode::Details) {
        const long long r = (long long)std::floor((y - listTop_) / rowH_);
        if (r >= 0 && r < (long long)shown_.size()) row = r;
    } else {
        const long long line = (long long)std::floor((y - listTop_) / tileH_), col = (long long)std::floor((x - listX_) / tileW_);
        if (line >= 0 && col >= 0 && col < columns_ && line * columns_ + col < (long long)shown_.size()) row = line * columns_ + col;
    }
    if (row >= 0 && !sel_.selected((std::size_t)row)) sel_.select((std::size_t)row);
    if (row < 0) sel_.clear();
    const float s = gfx_.dpiScale();
    POINT pt{(LONG)(x * s), (LONG)(y * s)};
    ClientToScreen(hwnd(), &pt);
    invalidate();
    UpdateWindow(hwnd());
    showItemMenu(pt);
    return true;
}

LRESULT FilesWindow::onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    switch (msg) {
        case WM_APP_LIST: {
            auto* b = reinterpret_cast<ListBatch*>(lp);
            receive(b);
            delete b;
            handled = true;
            return 0;
        }
        case WM_APP_THUMB: {
            auto* t = reinterpret_cast<ThumbResult*>(lp);
            if (thumbs_) {
                thumbs_->received(t);
                for (std::uint64_t serial : thumbs_->takeEvicted()) gfx_.forgetImage(L"thumb:" + std::to_wstring(serial));
                if (settings_.view == ViewMode::Icons) invalidate();
            }
            delete t;
            handled = true;
            return 0;
        }
        case WM_APP_DRIVES: {
            auto* b = reinterpret_cast<DriveBatch*>(lp);
            receive(b);
            delete b;
            handled = true;
            return 0;
        }
        case WM_SYSKEYDOWN:
            switch (wp) {
                case VK_LEFT: back(); handled = true; return 0;
                case VK_RIGHT: forward(); handled = true; return 0;
                case VK_UP: up(); handled = true; return 0;
                case 'D': beginEditingAddress(); handled = true; return 0;
                default: break;
            }
            break;
        case WM_CHAR:
            if (wp >= 32 && wp != 127 && !(GetKeyState(VK_CONTROL) & 0x8000)) {
                typeAhead((wchar_t)wp);
                handled = true;
                return 0;
            }
            break;
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) back();
            else forward();
            handled = true;
            return TRUE;
        case WM_DEVICECHANGE:
            // A drive came or went: the sidebar (and This PC) catch up.
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
                if (here_.thisPC) refresh();
                else listDrivesAsync(hwnd(), 0, std::make_shared<std::atomic<std::uint64_t>>(0));
            }
            break;
        case WM_COMMAND:
            if ((HWND)lp == address_ && HIWORD(wp) == EN_KILLFOCUS) {
                endEditingAddress(false);
                handled = true;
                return 0;
            }
            break;
        case WM_CTLCOLOREDIT:
            if ((HWND)lp == address_) {
                HDC dc = (HDC)wp;
                SetTextColor(dc, toColorRef(pal_.ink));
                SetBkColor(dc, toColorRef(pal_.sunken));
                handled = true;
                return (LRESULT)addressBrush_;
            }
            break;
        default: break;
    }
    return 0;
}

void FilesWindow::onTimer(UINT_PTR id) {
    if (id == kSpinTimer) {
        if (!loading_) KillTimer(hwnd(), kSpinTimer);
        invalidate();
    }
}

// ---------------------------------------------------------------- commands

void FilesWindow::setView(ViewMode v) {
    settings_.view = v;
    settings_.save(settingsPath_);
    if (sel_.focus() >= 0) {
        invalidate();
        UpdateWindow(hwnd());
        ensureVisible((std::size_t)sel_.focus());
    }
    invalidate();
}

void FilesWindow::sortBy(SortKey k) {
    if ((SortKey)settings_.sortKey == k) settings_.sortDescending = !settings_.sortDescending;
    else {
        settings_.sortKey = (int)k;
        // Newest and largest first are what people usually want for those.
        settings_.sortDescending = k == SortKey::Modified || k == SortKey::Size;
    }
    settings_.save(settingsPath_);
    showHere();
}

void FilesWindow::toggleHidden() {
    settings_.showHidden = !settings_.showHidden;
    settings_.save(settingsPath_);
    showHere();
    flash(settings_.showHidden ? L"Hidden items are shown, a little faded." : L"Hidden items are hidden.");
}

void FilesWindow::toggleProtected() {
    if (!settings_.showProtected &&
        MessageBoxW(hwnd(),
                    L"Show protected operating system files?\n\nThese are files Windows needs to start and run. Changing or deleting "
                    L"them can stop Windows working. They'll be shown faded.",
                    L"NorthRay Files", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
        return;
    settings_.showProtected = !settings_.showProtected;
    settings_.save(settingsPath_);
    showHere();
}

void FilesWindow::copyPaths() {
    std::wstring text;
    const auto rows = sel_.indices();
    if (rows.empty()) text = here_.thisPC ? L"" : here_.path;
    for (std::size_t r : rows) {
        if (!text.empty()) text += L"\r\n";
        text += L"\"" + rowPath(r) + L"\"";
    }
    if (text.empty() || !OpenClipboard(hwnd())) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(g), text.c_str(), bytes);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
    flash(rows.size() > 1 ? L"Paths copied." : L"Path copied.");
}

void FilesWindow::showMenu() {
    const SortKey k = (SortKey)settings_.sortKey;
    const std::vector<MenuItem> items = {
        MenuItem::sub(L"View", {MenuItem::item(L"Details", CmdViewDetails, L"Ctrl+Shift+6", true, settings_.view == ViewMode::Details),
                                MenuItem::item(L"Icons", CmdViewIcons, L"Ctrl+Shift+2", true, settings_.view == ViewMode::Icons)}),
        MenuItem::sub(L"Sort by", {MenuItem::item(L"Name", CmdSortName, {}, true, k == SortKey::Name),
                                   MenuItem::item(L"Date modified", CmdSortDate, {}, true, k == SortKey::Modified),
                                   MenuItem::item(L"Type", CmdSortType, {}, true, k == SortKey::Type),
                                   MenuItem::item(L"Size", CmdSortSize, {}, true, k == SortKey::Size), MenuItem::sep(),
                                   MenuItem::item(L"Ascending", CmdSortAsc, {}, true, !settings_.sortDescending),
                                   MenuItem::item(L"Descending", CmdSortDesc, {}, true, settings_.sortDescending)}),
        MenuItem::item(L"Refresh", CmdRefresh, L"F5"),
        MenuItem::sep(),
        MenuItem::item(L"Show hidden items", CmdHidden, {}, true, settings_.showHidden),
        MenuItem::item(L"Show protected operating system files", CmdProtected, {}, true, settings_.showProtected),
        MenuItem::sep(),
        MenuItem::sub(L"Theme", {MenuItem::item(L"As Windows", CmdThemeSystem, {}, true, settings_.theme == Theme::System),
                                 MenuItem::item(L"Light", CmdThemeLight, {}, true, settings_.theme == Theme::Light),
                                 MenuItem::item(L"Dark", CmdThemeDark, {}, true, settings_.theme == Theme::Dark)}),
        MenuItem::item(L"About NorthRay Files", CmdAbout),
    };
    const float s = gfx_.dpiScale();
    RECT rc;
    GetClientRect(hwnd(), &rc);
    POINT pt{rc.right - (LONG)(10 * s), (LONG)((kToolbarH - 6) * s)};
    ClientToScreen(hwnd(), &pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_, cjui::MenuAnchor::TopRight);
    switch (cmd) {
        case CmdViewDetails: setView(ViewMode::Details); break;
        case CmdViewIcons: setView(ViewMode::Icons); break;
        case CmdSortName: sortBy(SortKey::Name); break;
        case CmdSortDate: sortBy(SortKey::Modified); break;
        case CmdSortType: sortBy(SortKey::Type); break;
        case CmdSortSize: sortBy(SortKey::Size); break;
        case CmdSortAsc: settings_.sortDescending = false; settings_.save(settingsPath_); showHere(); break;
        case CmdSortDesc: settings_.sortDescending = true; settings_.save(settingsPath_); showHere(); break;
        case CmdRefresh: refresh(); break;
        case CmdHidden: toggleHidden(); break;
        case CmdProtected: toggleProtected(); break;
        case CmdThemeSystem: case CmdThemeLight: case CmdThemeDark:
            settings_.theme = (Theme)(cmd - CmdThemeSystem);
            settings_.save(settingsPath_);
            applyTheme();
            break;
        case CmdAbout:
            MessageBoxW(hwnd(),
                        L"NorthRay Files " CJ_VERSION_WSTR L" (preview)\n\nDeveloped by the " CJ_DEVELOPER_WSTR
                        L" project.\nNorthRaySoftware@protonmail.com\n\nThis first version browses folders and opens files. It "
                        L"doesn't change, move or delete anything yet; copying, moving, renaming and deleting come next.",
                        L"About NorthRay Files", MB_OK | MB_ICONINFORMATION);
            break;
        default: break;
    }
    invalidate();
}

void FilesWindow::showItemMenu(POINT screen) {
    const std::size_t n = sel_.count();
    std::vector<MenuItem> items;
    if (n > 0) {
        items.push_back(MenuItem::item(L"Open", CmdOpen, L"Enter"));
        items.push_back(MenuItem::item(n > 1 ? L"Copy paths" : L"Copy path", CmdCopyPath, L"Ctrl+Shift+C"));
    } else {
        items.push_back(MenuItem::sub(L"View", {MenuItem::item(L"Details", CmdViewDetails, {}, true, settings_.view == ViewMode::Details),
                                                MenuItem::item(L"Icons", CmdViewIcons, {}, true, settings_.view == ViewMode::Icons)}));
        items.push_back(MenuItem::item(L"Refresh", CmdRefresh, L"F5"));
        items.push_back(MenuItem::item(L"Copy this folder's path", CmdCopyPath, L"Ctrl+Shift+C"));
        items.push_back(MenuItem::sep());
        items.push_back(MenuItem::item(L"Show hidden items", CmdHidden, {}, true, settings_.showHidden));
    }
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    switch (cmd) {
        case CmdOpen: openSelection(); break;
        case CmdCopyPath: copyPaths(); break;
        case CmdViewDetails: setView(ViewMode::Details); break;
        case CmdViewIcons: setView(ViewMode::Icons); break;
        case CmdRefresh: refresh(); break;
        case CmdHidden: toggleHidden(); break;
        default: break;
    }
    invalidate();
}

// ---------------------------------------------------------------- drawing

std::wstring FilesWindow::dateText(std::uint64_t ft) const {
    if (!ft) return L"";
    FILETIME f{(DWORD)(ft & 0xFFFFFFFF), (DWORD)(ft >> 32)};
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&f, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return L"";
    wchar_t d[64] = L"", t[64] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, t, 64);
    return std::wstring(d) + L"  " + t;
}

void FilesWindow::drawIcon(int index, const Rect& r, float opacity) {
    // The system image list's size nearest the drawn size at this DPI.
    const float px = r.w * gfx_.dpiScale();
    const int size = px <= 20 ? 16 : px <= 40 ? 32 : 48;
    const IconPixels* p = systemIcon(index, size);
    if (!p) {
        gfx_.fillRounded(r.inset(r.w * 0.15f), 2, pal_.inkFaint.withAlpha(0.5f * opacity));
        return;
    }
    const std::wstring key = L"sys:" + std::to_wstring(index) + L":" + std::to_wstring(size);
    if (opacity < 0.99f) {
        // Hidden items: faded, as in File Explorer.
        gfx_.drawPixels(key, p->w, p->h, p->rgba.data(), r, true);
        gfx_.fill(r, pal_.paper.withAlpha(1.0f - opacity));
        return;
    }
    gfx_.drawPixels(key, p->w, p->h, p->rgba.data(), r, true);
}

void FilesWindow::navButton(const std::string& id, const Rect& r, int icon, bool enabled, std::function<void()> act) {
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
        case IconUp:
            gfx_.line(cx, cy - 7, cx, cy + 7, c, 1.7f);
            gfx_.line(cx, cy - 7, cx - 5, cy - 2, c, 1.7f);
            gfx_.line(cx, cy - 7, cx + 5, cy - 2, c, 1.7f);
            break;
        case IconRefresh:
            arcLines(gfx_, cx, cy, 6.5f, -0.75f, 4.3f, c, 1.7f);
            gfx_.fillTriangle(cx + 2.2f, cy - 9.8f, cx + 8.6f, cy - 6.2f, cx + 2.6f, cy - 2.4f, c);
            break;
        case IconDetails:
            for (int i = -1; i <= 1; ++i) {
                gfx_.fill({cx - 7, cy + i * 5.0f - 1, 2.5f, 2.5f}, c);
                gfx_.line(cx - 2.5f, cy + i * 5.0f + 0.25f, cx + 7, cy + i * 5.0f + 0.25f, c, 1.6f);
            }
            break;
        case IconTiles:
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) gfx_.strokeRounded({cx - 7 + i * 8.0f, cy - 7 + j * 8.0f, 6, 6}, 1.5f, c, 1.5f);
            break;
        case IconMenu:
            for (int i = -1; i <= 1; ++i) gfx_.circle(cx + i * 6.0f, cy, 1.6f, c, true);
            break;
        default: break;
    }
    if (enabled && focused(id)) focusRing(r);
}

void FilesWindow::drawFrame(float W, float H) {
    if (gfx_.dpiScale() != addressScale_) {
        addressScale_ = gfx_.dpiScale();
        if (addressFont_) DeleteObject(addressFont_);
        addressFont_ = CreateFontW(-(int)std::lround(14 * addressScale_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(address_, WM_SETFONT, reinterpret_cast<WPARAM>(addressFont_), TRUE);
    }
    const float side = W >= 760 ? kSidebarW : 0;   // a narrow window gives the files the room
    if (side > 0) drawSidebar({0, 0, side, H});
    drawToolbar({side, 0, W - side, kToolbarH});
    drawContent({side, kToolbarH, W - side, H - kToolbarH});
    if (side <= 0) drawStatusNote({side, kToolbarH, W - side, H - kToolbarH});
    drawToast({side, kToolbarH, W - side, H - kToolbarH});
}

void FilesWindow::drawSidebar(const Rect& r) {
    nr::nightSky(gfx_, r, 0, 0.9f, 11);
    nr::auroraBand(gfx_, {r.x, r.y, r.w, 3}, pal_.dark);
    // The app's mark and name, as in the Launcher.
    static IconPixels mark = [] {
        HICON h = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 64, 64, 0);
        IconPixels p = iconPixels(h);
        if (h) DestroyIcon(h);
        return p;
    }();
    float x = r.x + 20;
    if (!mark.rgba.empty()) {
        gfx_.drawPixels(L"files:mark", mark.w, mark.h, mark.rgba.data(), {x, r.y + 22, 36, 36}, true);
        x += 46;
    }
    gfx_.text(L"NorthRay", {x, r.y + 18, r.w - x, 24}, Font::Wordmark, nr::kNightInk, {Align::Left, false});
    gfx_.text(L"FILES", {x + 1, r.y + 43, r.w - x, 16}, Font::Label, nr::kAuroraGreen.withAlpha(0.85f), {Align::Left, false});

    float y = r.y + 84;
    auto row = [&](const std::string& id, const std::wstring& label, int icon, bool selected, std::function<void()> act,
                   const std::wstring& note = L"") {
        const Rect rr{r.x + 10, y, r.w - 20, 32};
        addHit(id, rr, std::move(act));
        const float hv = hoverAmount(id);
        if (selected) {
            gfx_.fillRounded(rr, 8, nr::kNightInk.withAlpha(0.10f));
            gfx_.fillRounded({rr.x, rr.y + 9, 3, 14}, 1.5f, nr::kAuroraGreen);
        } else if (hv > 0.01f) {
            gfx_.fillRounded(rr, 8, nr::kNightInk.withAlpha(0.06f * hv));
        }
        drawIcon(icon, {rr.x + 12, rr.y + 8, 16, 16});
        const float noteW = note.empty() ? 0 : gfx_.measureWidth(note, Font::Small) + 8;
        gfx_.text(label, {rr.x + 38, rr.y, rr.w - 46 - noteW, rr.h}, selected ? Font::BodyStrong : Font::Body,
                  selected ? nr::kNightInk : nr::kNightMuted.mix(nr::kNightInk, 0.35f + 0.4f * hv), {Align::Left, false, true, true});
        if (!note.empty())
            gfx_.text(note, {rr.right() - noteW - 6, rr.y, noteW, rr.h}, Font::Small, nr::kNightMuted.withAlpha(0.8f),
                      {Align::Right, false, false, true});
        if (focused(id)) focusRing(rr);
        y += 34;
    };
    gfx_.text(L"PLACES", {r.x + 22, y, r.w - 44, 16}, Font::Label, nr::kNightMuted.withAlpha(0.75f), {Align::Left, false});
    y += 22;
    for (const Place& p : places_)
        row("tb:place:" + narrow(p.label), p.label, p.icon, !here_.thisPC && samePath(here_.path, p.path), [this, p] { goPath(p.path); });
    y += 12;
    gfx_.text(L"THIS PC", {r.x + 22, y, r.w - 44, 16}, Font::Label, nr::kNightMuted.withAlpha(0.75f), {Align::Left, false});
    y += 22;
    int pcIcon = -1;
    for (const Drive& d : drives_)
        if (d.icon >= 0) { pcIcon = d.icon; break; }
    row("tb:thispc", L"This PC", pcIcon, here_.thisPC, [this] { go({true, L""}); });
    // What's here, at the foot of the sidebar.
    const std::wstring status = statusText();
    const float statusH = status.empty() ? 0 : gfx_.measureHeight(status, r.w - 44, Font::Small) + 30;
    for (const Drive& d : drives_) {
        if (y > r.bottom() - 40 - statusH) break;
        const bool inHere = !here_.thisPC && here_.path.size() >= 3 && samePath(here_.path.substr(0, 3), d.root) && isDriveRoot(here_.path);
        row("tb:drive:" + narrow(d.root), d.label + L" (" + d.root.substr(0, 2) + L")", d.icon, inHere,
            [this, root = d.root] { goPath(root); }, d.ready ? L"" : L"empty");
    }
    if (!status.empty()) {
        gfx_.line(r.x + 22, r.bottom() - statusH + 4, r.right() - 22, r.bottom() - statusH + 4, nr::kNightInk.withAlpha(0.10f));
        gfx_.text(status, {r.x + 22, r.bottom() - statusH + 14, r.w - 44, statusH}, Font::Small, nr::kNightMuted);
    }
}

void FilesWindow::drawToolbar(const Rect& r) {
    gfx_.fill(r, pal_.surface);
    gfx_.line(r.x, r.bottom() - 0.5f, r.right(), r.bottom() - 0.5f, pal_.rule);
    const float bs = 34, by = r.y + (r.h - bs) / 2;
    float x = r.x + 12;
    navButton("tb:back", {x, by, bs, bs}, IconBack, !backStack_.empty(), [this] { back(); });
    x += bs + 2;
    navButton("tb:fwd", {x, by, bs, bs}, IconForward, !forwardStack_.empty(), [this] { forward(); });
    x += bs + 2;
    navButton("tb:up", {x, by, bs, bs}, IconUp, !here_.thisPC, [this] { up(); });
    x += bs + 2;
    navButton("tb:refresh", {x, by, bs, bs}, IconRefresh, true, [this] { refresh(); });
    x += bs + 10;
    const float right = r.right() - drawCaptionButtons(r, false) - 8;
    const float menuX = right - bs, iconsX = menuX - 10 - bs, detailsX = iconsX - bs - 2;
    drawAddress({x, by - 1, detailsX - 12 - x, bs + 2});
    // Views: the current one is marked.
    const bool icons = settings_.view == ViewMode::Icons;
    gfx_.fillRounded({icons ? iconsX : detailsX, by, bs, bs}, 8, pal_.ribbon.withAlpha(pal_.dark ? 0.20f : 0.14f));
    navButton("tb:details", {detailsX, by, bs, bs}, IconDetails, true, [this] { setView(ViewMode::Details); });
    navButton("tb:icons", {iconsX, by, bs, bs}, IconTiles, true, [this] { setView(ViewMode::Icons); });
    navButton("tb:menu", {menuX, by, bs, bs}, IconMenu, true, [this] { showMenu(); });
}

void FilesWindow::drawAddress(const Rect& r) {
    gfx_.fillRounded(r, 8, pal_.sunken);
    gfx_.strokeRounded(r, 8, editing_ ? pal_.focus : pal_.rule, editing_ ? 1.5f : 1);
    addressRect_ = r;
    const float s = gfx_.dpiScale();
    if (editing_) {
        const float th = 20;
        SetWindowPos(address_, nullptr, (int)std::lround((r.x + 12) * s), (int)std::lround((r.y + (r.h - th) / 2) * s),
                     (int)std::lround((r.w - 24) * s), (int)std::lround(th * s), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return;
    }
    // Clicking the field's empty space types a path.
    addHit("tb:address", r, [this] { beginEditingAddress(); });
    struct Part { std::wstring label; Location to; };
    std::vector<Part> parts;
    if (here_.thisPC) {
        parts.push_back({L"This PC", {true, L""}});
    } else {
        for (const Crumb& c : breadcrumbs(here_.path)) {
            std::wstring label = c.label;
            if (isDriveRoot(c.path))
                for (const Drive& d : drives_)
                    if (samePath(d.root, c.path)) label = d.label + L" (" + c.label + L")";
            parts.push_back({label, {false, c.path}});
        }
    }
    // As many as fit, from the end; "..." for the rest (which opens the field).
    const float chev = 18, pad = 8;
    std::vector<float> widths;
    for (size_t i = 0; i < parts.size(); ++i)   // the last is drawn bold
        widths.push_back(std::min(gfx_.measureWidth(parts[i].label, i + 1 == parts.size() ? Font::BodyStrong : Font::Body), 240.0f) + pad * 2 + 2);
    const float avail = r.w - 16;
    size_t first = 0;
    float total = 0;
    for (size_t i = parts.size(); i-- > 0;) {
        if (total + widths[i] + (i + 1 < parts.size() ? chev : 0) > avail - 40 && i + 1 < parts.size()) { first = i + 1; break; }
        total += widths[i] + (i + 1 < parts.size() ? chev : 0);
    }
    float x = r.x + 6;
    gfx_.pushClip(r);
    if (first > 0) {
        gfx_.text(L"…", {x, r.y, 24, r.h}, Font::Body, pal_.inkMuted, {Align::Center, false, false, true});
        x += 26;
    }
    for (size_t i = first; i < parts.size(); ++i) {
        const std::string id = "tb:crumb:" + std::to_string(i);
        const Rect cr{x, r.y + 4, widths[i], r.h - 8};
        const bool last = i + 1 == parts.size();
        addHit(id, cr, [this, to = parts[i].to, last] {
            if (last) beginEditingAddress();
            else go(to);
        });
        const float hv = hoverAmount(id);
        if (hv > 0.01f) gfx_.fillRounded(cr, 6, pal_.ink.withAlpha(0.07f * hv));
        gfx_.text(parts[i].label, {cr.x + pad, cr.y, cr.w - pad * 2, cr.h}, last ? Font::BodyStrong : Font::Body,
                  last ? pal_.ink : pal_.inkMuted.mix(pal_.ink, hv), {Align::Left, false, true, true});
        if (focused(id)) focusRing(cr);
        x = cr.right();
        if (!last) {
            const float cx = x + chev / 2, cy = r.y + r.h / 2;
            gfx_.line(cx - 2, cy - 4, cx + 2, cy, pal_.inkFaint, 1.3f);
            gfx_.line(cx + 2, cy, cx - 2, cy + 4, pal_.inkFaint, 1.3f);
            x += chev;
        }
    }
    gfx_.popClip();
}

void FilesWindow::drawMessage(const Rect& r, const std::wstring& title, const std::wstring& text, bool withRetry) {
    const float w = std::min(r.w - 80, 560.0f);
    const float x = r.x + (r.w - w) / 2;
    float y = r.y + std::max(40.0f, r.h * 0.2f);
    gfx_.text(title, {x, y, w, 32}, Font::Heading, pal_.ink, {Align::Center, false});
    y += 36;
    if (!text.empty()) y += gfx_.text(text, {x, y, w, 120}, Font::Body, pal_.inkMuted, {Align::Center}) + 20;
    if (withRetry) {
        float bx = r.x + r.w / 2 - (here_.thisPC || isRoot(here_.path) ? 60 : 125);
        button("tb:retry", {bx, y, 120, 36}, L"Try again", [this] { refresh(); }, true);
        if (!here_.thisPC && !parentOf(here_.path).empty()) button("tb:goup", {bx + 130, y, 120, 36}, L"Go up a level", [this] { up(); });
    }
}

void FilesWindow::drawContent(const Rect& r) {
    gfx_.fill(r, pal_.paper);
    listClip_ = {};
    driveRects_.clear();
    if (here_.thisPC) { drawThisPC(r); return; }
    if (error_ != ListError::None) {
        std::wstring title;
        std::wstring text = describeListError(error_, errorCode_, here_.path, &title);
        if (error_ == ListError::NotFound && isDriveRoot(here_.path)) {
            // A drive letter that isn't on this PC (now).
            title = L"There's no " + here_.path.substr(0, 2) + L" drive";
            text = L"No drive on this PC is using that letter right now. A USB drive or network drive may have been disconnected.";
        }
        drawMessage(r, title, text, true);
        return;
    }
    if (shown_.empty()) {
        if (loading_) {
            if (GetTickCount() - loadStarted_ > 250) {
                const double secs = (double)GetTickCount64() / 1000.0;
                const float a0 = (float)std::fmod(secs * 6.0, 6.2831853);
                const float cx = r.x + r.w / 2, cy = r.y + std::max(60.0f, r.h * 0.25f);
                gfx_.circle(cx, cy, 11, pal_.inkFaint.withAlpha(0.3f), false, 2.5f);
                arcLines(gfx_, cx, cy, 11, a0, a0 + 1.9f, pal_.ribbon, 2.5f);
                gfx_.text(L"Reading this folder...", {r.x, cy + 22, r.w, 24}, Font::Body, pal_.inkMuted, {Align::Center, false});
            }
            return;
        }
        const std::size_t hidden = listed_.size();
        drawMessage(r, L"This folder is empty",
                    hidden ? (hidden == 1 ? L"It has 1 hidden item. Show hidden items from the menu to see it."
                                          : L"It has " + std::to_wstring(hidden) + L" hidden items. Show hidden items from the menu to see them.")
                           : L"",
                    false);
        return;
    }
    if (settings_.view == ViewMode::Details) drawDetails(r);
    else drawIcons(r);
}

void FilesWindow::drawRowBackground(const Rect& r, std::size_t row, float radius) {
    const bool sel = sel_.selected(row);
    const bool hov = hoverId() == "row:" + std::to_string(row);
    if (sel) gfx_.fillRounded(r, radius, pal_.ribbon.withAlpha(pal_.dark ? (hov ? 0.30f : 0.22f) : (hov ? 0.22f : 0.15f)));
    else if (hov) gfx_.fillRounded(r, radius, pal_.ink.withAlpha(pal_.dark ? 0.06f : 0.04f));
    if ((int)row == sel_.focus() && GetFocus() == hwnd()) gfx_.strokeRounded(r, radius, pal_.ribbon.withAlpha(0.7f), 1);
}

void FilesWindow::drawDetails(const Rect& r) {
    const float pad = 16;
    const float x0 = r.x + pad, w = r.w - pad * 2;
    // Columns: Name takes what's left; Type then Date go when it's narrow.
    const bool showType = w >= 620, showDate = w >= 430;
    const float sizeW = 100, typeW = showType ? 170 : 0, dateW = showDate ? 170 : 0;
    const float nameW = w - sizeW - typeW - dateW;
    const float xName = x0, xDate = xName + nameW, xType = xDate + dateW, xSize = xType + typeW;

    // Header: click to sort.
    const Rect hr{r.x, r.y, r.w, kHeaderH};
    gfx_.line(hr.x + pad, hr.bottom() - 0.5f, hr.right() - pad, hr.bottom() - 0.5f, pal_.rule);
    auto head = [&](const std::string& id, const std::wstring& label, float x, float cw, SortKey k, bool right) {
        if (cw <= 0) return;
        const Rect c{x, hr.y, cw, hr.h};
        addHit(id, c, [this, k] { sortBy(k); });
        const float hv = hoverAmount(id);
        if (hv > 0.01f) gfx_.fill(c, pal_.ink.withAlpha(0.04f * hv));
        const bool on = (SortKey)settings_.sortKey == k;
        const float tx = right ? c.x : c.x + 10;
        gfx_.text(label, {tx, c.y, cw - 24, c.h}, on ? Font::SmallStrong : Font::Small, on ? pal_.ink : pal_.inkMuted,
                  {right ? Align::Right : Align::Left, false, true, true});
        if (on) {
            const float ax = right ? c.right() - 10 : c.x + 16 + gfx_.measureWidth(label, Font::SmallStrong), ay = c.y + c.h / 2;
            const float d = settings_.sortDescending ? 1.0f : -1.0f;
            gfx_.line(ax - 3.5f, ay - 1.5f * d, ax, ay + 1.5f * d, pal_.inkMuted, 1.3f);
            gfx_.line(ax, ay + 1.5f * d, ax + 3.5f, ay - 1.5f * d, pal_.inkMuted, 1.3f);
        }
        if (focused(id)) focusRing(c);
    };
    head("tb:sort:name", L"Name", xName, nameW, SortKey::Name, false);
    head("tb:sort:date", L"Date modified", xDate, dateW, SortKey::Modified, false);
    head("tb:sort:type", L"Type", xType, typeW, SortKey::Type, false);
    head("tb:sort:size", L"Size", xSize, sizeW - 14, SortKey::Size, true);

    const Rect clip{r.x, hr.bottom() + 2, r.w, r.h - kHeaderH - 2};
    listClip_ = clip;
    const float top = beginScroll(clip);
    listTop_ = top + 4;
    const std::size_t n = shown_.size();
    const std::size_t first = (std::size_t)std::max(0.0f, std::floor((clip.y - listTop_) / rowH_));
    const std::size_t last = std::min(n, (std::size_t)std::max(0.0f, std::ceil((clip.bottom() - listTop_) / rowH_)) + 1);
    for (std::size_t i = first; i < last; ++i) {
        const Entry& e = shown_[i];
        const Rect row{r.x + pad - 6, listTop_ + (float)i * rowH_, r.w - pad * 2 + 12, rowH_ - 2};
        addHit("row:" + std::to_string(i), row, nullptr, false);
        drawRowBackground(row, i, 6);
        const float fade = (e.hidden() || e.protectedSystem()) ? 0.55f : 1.0f;
        drawIcon(e.icon, {xName + 4, row.y + (row.h - 16) / 2, 16, 16}, fade);
        if (e.link()) {
            // A link (shortcut-style arrow), so a junction isn't mistaken for a real folder.
            const float lx = xName + 4, ly = row.y + (row.h - 16) / 2 + 9;
            gfx_.fillRounded({lx - 1, ly - 1, 9, 9}, 2, pal_.surface);
            gfx_.line(lx + 1.5f, ly + 5.5f, lx + 5.5f, ly + 1.5f, pal_.ink, 1.2f);
            gfx_.line(lx + 3, ly + 1.5f, lx + 5.5f, ly + 1.5f, pal_.ink, 1.2f);
            gfx_.line(lx + 5.5f, ly + 1.5f, lx + 5.5f, ly + 4, pal_.ink, 1.2f);
        }
        const Color ink = fade < 1 ? pal_.inkMuted : pal_.ink;
        gfx_.text(e.name, {xName + 30, row.y, nameW - 40, row.h}, Font::Body, ink, {Align::Left, false, true, true});
        if (dateW > 0) gfx_.text(dateText(e.modified), {xDate + 10, row.y, dateW - 14, row.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        if (typeW > 0) gfx_.text(e.type, {xType + 10, row.y, typeW - 14, row.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        if (!e.folder())
            gfx_.text(formatSizeKb(e.size), {xSize, row.y, sizeW - 14, row.h}, Font::Small, pal_.inkMuted, {Align::Right, false, true, true});
    }
    endScroll((float)n * rowH_ + 12);
}

void FilesWindow::drawIcons(const Rect& r) {
    const float pad = 16;
    const Rect clip{r.x, r.y + 4, r.w, r.h - 4};
    listClip_ = clip;
    const float avail = r.w - pad * 2;
    columns_ = std::max(1, (int)(avail / 156));
    tileW_ = avail / (float)columns_;
    tileH_ = 172;
    listX_ = r.x + pad;
    const float top = beginScroll(clip);
    listTop_ = top + 8;
    // Pictures are made at the size they're shown on this display.
    const float boxW = 136, boxH = 102;
    const int thumbPx = boxW * gfx_.dpiScale() <= 128 ? 128 : 256;
    const std::size_t n = shown_.size();
    const std::size_t lines = (n + (std::size_t)columns_ - 1) / (std::size_t)columns_;
    const std::size_t firstLine = (std::size_t)std::max(0.0f, std::floor((clip.y - listTop_) / tileH_));
    const std::size_t lastLine = std::min(lines, (std::size_t)std::max(0.0f, std::ceil((clip.bottom() - listTop_) / tileH_)) + 1);
    for (std::size_t line = firstLine; line < lastLine; ++line)
        for (int c = 0; c < columns_; ++c) {
            const std::size_t i = line * (std::size_t)columns_ + (std::size_t)c;
            if (i >= n) break;
            const Entry& e = shown_[i];
            const Rect t{listX_ + (float)c * tileW_ + 3, listTop_ + (float)line * tileH_, tileW_ - 6, tileH_ - 6};
            addHit("row:" + std::to_string(i), t, nullptr, false);
            drawRowBackground(t, i, 8);
            const float fade = (e.hidden() || e.protectedSystem()) ? 0.55f : 1.0f;
            const Rect box{t.x + (t.w - boxW) / 2, t.y + 8, boxW, boxH};
            const Thumb* th = nullptr;
            if (thumbs_ && !e.folder() && !e.cloudOnly() && Thumbnails::isImage(e.name))
                th = thumbs_->get(rowPath(i), e.modified, e.size, thumbPx);
            if (th && th->w > 0 && th->h > 0) {
                // The picture itself, its shape kept, never enlarged past its own size.
                const float ds = gfx_.dpiScale();
                const float s = std::min({boxW / th->w, boxH / th->h, 1.0f / ds});
                const float w = std::max(1.0f, th->w * s), h = std::max(1.0f, th->h * s);
                const Rect p{std::round(box.x + (boxW - w) / 2), std::round(box.y + (boxH - h) / 2), std::round(w), std::round(h)};
                gfx_.drawPixels(L"thumb:" + std::to_wstring(th->serial), th->w, th->h, th->rgba.data(), p, true);
                gfx_.stroke({p.x - 0.5f, p.y - 0.5f, p.w + 1, p.h + 1}, pal_.rule, 1);
                if (fade < 1) gfx_.fill(p, pal_.paper.withAlpha(1.0f - fade));
            } else {
                drawIcon(e.icon, {t.x + (t.w - 48) / 2, box.y + (boxH - 48) / 2, 48, 48}, fade);
            }
            gfx_.text(e.name, {t.x + 6, box.bottom() + 6, t.w - 12, 38}, Font::Small, fade < 1 ? pal_.inkMuted : pal_.ink,
                      {Align::Center, true, true});
        }
    endScroll((float)lines * tileH_ + 16);
}

void FilesWindow::drawThisPC(const Rect& r) {
    const float pad = 28;
    const float x = r.x + pad, w = r.w - pad * 2;
    float y = r.y + 24;
    gfx_.text(L"This PC", {x, y, w, 36}, Font::Title, pal_.ink, {Align::Left, false});
    y += 50;
    y += sectionLabel(x, y, w, L"DRIVES") + 10;
    if (drives_.empty()) {
        gfx_.text(loading_ ? L"Looking for drives..." : L"No drives found.", {x, y, w, 24}, Font::Body, pal_.inkMuted);
        return;
    }
    const float cardW = 300, cardH = 86, gap = 14;
    const int cols = std::max(1, (int)((w + gap) / (cardW + gap)));
    int i = 0;
    for (const Drive& d : drives_) {
        const Rect c{x + (float)(i % cols) * (cardW + gap), y + (float)(i / cols) * (cardH + gap), cardW, cardH};
        const std::string id = "drive:" + narrow(d.root);
        addHit(id, c, [this, root = d.root] { goPath(root); });
        const float hv = hoverAmount(id);
        softShadow(c, 10, hv);
        gfx_.fillRounded(c, 10, pal_.surface);
        gfx_.strokeRounded(c, 10, pal_.rule.mix(pal_.inkFaint, 0.3f * hv), 1);
        drawIcon(d.icon, {c.x + 16, c.y + 18, 32, 32});
        gfx_.text(d.label + L" (" + d.root.substr(0, 2) + L")", {c.x + 60, c.y + 14, c.w - 74, 22}, Font::BodyStrong, pal_.ink,
                  {Align::Left, false, true});
        if (d.ready && d.total > 0) {
            const float frac = 1.0f - (float)((double)d.free / (double)d.total);
            const Rect bar{c.x + 60, c.y + 42, c.w - 76, 8};
            gfx_.fillRounded(bar, 4, pal_.sunken);
            gfx_.fillRounded({bar.x, bar.y, std::max(8.0f, bar.w * frac), bar.h}, 4,
                             frac > 0.9f ? cjui::hexColor(pal_.dark ? "#F0A35E" : "#C2410C") : pal_.ribbon);
            gfx_.text(formatSize(d.free) + L" free of " + formatSize(d.total), {c.x + 60, c.y + 56, c.w - 74, 20}, Font::Small,
                      pal_.inkMuted, {Align::Left, false, true});
        } else {
            gfx_.text(d.ready ? L"" : L"Nothing in the drive", {c.x + 60, c.y + 42, c.w - 74, 20}, Font::Small, pal_.inkMuted,
                      {Align::Left, false, true});
        }
        if (focused(id)) focusRing(c);
        ++i;
    }
}

// What's here and what's selected (there's no status bar: it sits at the foot of the
// sidebar, or in a small note at the corner of the list when the sidebar is tucked away).
std::wstring FilesWindow::statusText() const {
    std::wstring left;
    if (here_.thisPC) {
        left = std::to_wstring(drives_.size()) + (drives_.size() == 1 ? L" drive" : L" drives");
    } else if (error_ == ListError::None) {
        const std::size_t n = shown_.size(), hidden = listed_.size() - shown_.size();
        left = std::to_wstring(n) + (n == 1 ? L" item" : L" items");
        if (hidden) left += L" (" + std::to_wstring(hidden) + L" hidden)";
        if (loading_) left += L"\nStill reading...";
        const auto sel = sel_.indices();
        if (!sel.empty()) {
            std::uint64_t bytes = 0;
            bool anyFile = false;
            for (std::size_t i : sel)
                if (!shown_[i].folder()) { bytes += shown_[i].size; anyFile = true; }
            left += L"\n" + std::to_wstring(sel.size()) + L" selected";
            if (anyFile) left += L" (" + formatSize(bytes) + L")";
        }
        if (partial_) left += L"\nStopped part-way (error " + std::to_wstring(errorCode_) + L"); press F5 to try again";
    }
    return left;
}

void FilesWindow::drawStatusNote(const Rect& content) {
    std::wstring t = statusText();
    for (auto& c : t) if (c == L'\n') c = L' ';
    if (t.empty()) return;
    const float tw = gfx_.measureWidth(t, Font::Small) + 22;
    const Rect r{content.right() - tw - 14, content.bottom() - 36, tw, 26};
    gfx_.fillRounded(r, 13, pal_.surface.withAlpha(0.94f));
    gfx_.strokeRounded(r, 13, pal_.rule);
    gfx_.text(t, r, Font::Small, pal_.inkMuted, {Align::Center, false, true, true});
}

}  // namespace nrf

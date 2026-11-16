#include "files_window.hpp"
#include "cjui/dialog.hpp"

#include "nrf/operations.hpp"
#include "nrf/paths.hpp"
#include "operations.hpp"
#include "resource.h"
#include "search.hpp"
#include "dragdrop.hpp"
#include "nrf/search.hpp"
#include "nrf/art.hpp"
#include "nrf/labels.hpp"
#include "version.hpp"

#include "cjui/northray.hpp"

#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <set>
#include <thread>

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
constexpr UINT_PTR kRefreshTimer = 2;   // the folder changed: list it again after a moment
constexpr UINT_PTR kSearchTimer = 3;    // typing in the search box: search once it pauses
constexpr UINT WM_APP_CHANGED = WM_APP + 50;
constexpr UINT WM_APP_PREVIEW = WM_APP + 51;   // lParam: TextPreview*

// A text file's start, read on a worker for the preview pane.
struct TextPreview {
    std::wstring key;
    std::wstring text;
    bool truncated = false, notText = false;
};

bool isTextName(const std::wstring& name) {
    static const wchar_t* exts[] = {L".txt", L".md", L".markdown", L".log", L".csv", L".tsv", L".json", L".xml", L".ini", L".cfg", L".conf",
                                    L".yaml", L".yml", L".toml", L".html", L".htm", L".css", L".js", L".ts", L".py", L".c", L".cpp", L".h",
                                    L".hpp", L".cs", L".java", L".lua", L".bat", L".cmd", L".ps1", L".sh", L".srt", L".vtt", L".fountain",
                                    L".tex", L".nfo", L".gitignore", L".svg"};
    const std::wstring e = extensionOf(name);
    for (const wchar_t* x : exts)
        if (e == x) return true;
    return false;
}

enum NavIcon { IconBack, IconForward, IconUp, IconRefresh, IconDetails, IconTiles, IconMenu, IconPanes, IconPreview, IconGallery };

enum : UINT {
    CmdViewDetails = 100, CmdViewIcons, CmdSortName, CmdSortDate, CmdSortType, CmdSortSize, CmdSortAsc, CmdSortDesc,
    CmdHidden, CmdProtected, CmdThemeSystem, CmdThemeLight, CmdThemeDark, CmdAbout, CmdOpen, CmdCopyPath, CmdRefresh,
    CmdCut, CmdCopy, CmdPaste, CmdPasteInto, CmdRename, CmdBatchRename, CmdDelete, CmdDeleteForever, CmdNewFolder, CmdMore, CmdBinOpen, CmdBinEmpty,
    CmdNewTab, CmdNewTabHere, CmdTwoPanes, CmdPreview, CmdViewGallery, CmdBinRestore, CmdBinDelete, CmdNewTag, CmdNewCollection, CmdCollRemove, CmdCollRename, CmdCollDelete, CmdCollOpen, CmdCollTab, CmdPin, CmdUnpin, CmdCopyOther, CmdMoveOther, CmdPinnedOpen, CmdPinnedTab,
};

constexpr UINT CmdColour0 = 1000;   // + labels::Colour
constexpr UINT CmdTag0 = 1100;      // + the tag's place in Store::tags()
constexpr UINT CmdColl0 = 3000;     // + the collection's place

Color labelColour(labels::Colour c) {
    switch (c) {
        case labels::Colour::Red: return cjui::hexColor("#E5484D");
        case labels::Colour::Orange: return cjui::hexColor("#F27B2E");
        case labels::Colour::Yellow: return cjui::hexColor("#E2B714");
        case labels::Colour::Green: return cjui::hexColor("#30A46C");
        case labels::Colour::Blue: return cjui::hexColor("#3E8EDE");
        case labels::Colour::Purple: return cjui::hexColor("#8E4EC6");
        case labels::Colour::Grey: return cjui::hexColor("#8B8D98");
        default: return cjui::hexColor("#8B8D98");
    }
}

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
std::wstring lowerKey(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

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

    search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd(), nullptr, inst, nullptr);
    SetWindowSubclass(search_, quietEditProc, 3, 0);
    SendMessageW(search_, EM_SETLIMITTEXT, 260, 0);

    ask_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd(), nullptr, inst, nullptr);
    SetWindowSubclass(ask_, quietEditProc, 4, 0);
    SendMessageW(ask_, EM_SETLIMITTEXT, 120, 0);
    for (auto& f : batch_.fields) {
        f = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd(), nullptr, inst, nullptr);
        SetWindowSubclass(f, quietEditProc, 5, 0);
        SendMessageW(f, EM_SETLIMITTEXT, 200, 0);
    }
    loadLabels();
    {
        // Files dragged in from other programs.
        dnd::DropHandlers h;
        auto toClient = [this](POINT screen) {
            ScreenToClient(hwnd(), &screen);
            const float s = gfx_.dpiScale();
            return std::pair<float, float>{screen.x / s, screen.y / s};
        };
        h.over = [this, toClient](const std::vector<std::wstring>& paths, POINT screen, DWORD keys, DWORD) -> DWORD {
            const auto [x, y] = toClient(screen);
            if (!drag_.active) {
                drag_ = Drag{};
                drag_.paths = paths;
                drag_.fromPane = -1;
                drag_.external = true;
            }
            drag_.armed = drag_.active = true;
            drag_.x = x;
            drag_.y = y;
            invalidate();
            return dropAt(drag_.paths, x, y, (keys & MK_CONTROL) != 0, (keys & MK_SHIFT) != 0, false);
        };
        h.leave = [this] {
            if (drag_.external) drag_ = Drag{};
            invalidate();
        };
        h.drop = [this, toClient](const std::vector<std::wstring>& paths, POINT screen, DWORD keys, DWORD) -> DWORD {
            const auto [x, y] = toClient(screen);
            drag_ = Drag{};
            invalidate();
            return dropAt(paths, x, y, (keys & MK_CONTROL) != 0, (keys & MK_SHIFT) != 0, true);
        };
        dnd::registerTarget(hwnd(), std::move(h));
    }

    rename_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd(), nullptr, inst, nullptr);
    SetWindowSubclass(rename_, quietEditProc, 2, 0);
    SendMessageW(rename_, EM_SETLIMITTEXT, 255, 0);
    {
        // The Recycle Bin's own icon, for the sidebar.
        PIDLIST_ABSOLUTE bin = nullptr;
        if (SUCCEEDED(SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &bin)) && bin) {
            SHFILEINFOW fi{};
            if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(bin), 0, &fi, sizeof fi, SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) recycleIcon_ = fi.iIcon;
            CoTaskMemFree(bin);
        }
    }
    updateRecycleBin();
    thumbs_ = std::make_unique<Thumbnails>(hwnd());
    applyTheme();
    places_ = knownPlaces();
    ShowWindow(hwnd(), show);
    UpdateWindow(hwnd());

    // Tabs from last time (each pane), unless a folder was asked for.
    auto toLocation = [&](const std::wstring& t) -> std::optional<Location> {
        if (t == L"::thispc") return Location{true, L""};
        if (t == L"::bin") { Location b; b.bin = true; return b; }
        if (t.rfind(L"::collection:", 0) == 0) {
            Location c;
            c.collection = labels::toUtf8(t.substr(13));
            if (labels_.collection(c.collection)) return c;
            return std::nullopt;
        }
        auto n = normalizePath(t, L"");
        if (n) return Location{false, *n};
        return std::nullopt;
    };
    if (startPath.empty() && !settings_.tabs[0].empty()) {
        tabs_[0].assign(1, View{});
        bool first = true;
        for (auto& t : settings_.tabs[0]) {
            auto loc = toLocation(t);
            if (!loc) continue;
            if (first) { go(*loc, false); first = false; }
            else newTab(*loc);
        }
        if (first) go({true, L""}, false);
        if (settings_.twoPanes && !settings_.tabs[1].empty()) {
            setTwoPanes(true);
            bool f2 = true;
            for (auto& t : settings_.tabs[1]) {
                auto loc = toLocation(t);
                if (!loc) continue;
                if (f2) { go(*loc, false); f2 = false; }
                else newTab(*loc);
            }
            activate(1, std::clamp(settings_.activeTab[1], 0, (int)tabs_[1].size() - 1));
        }
        activate(0, std::clamp(settings_.activeTab[0], 0, (int)tabs_[0].size() - 1));
        listDrivesAsync(hwnd(), 0, std::make_shared<std::atomic<std::uint64_t>>(0));
        return true;
    }
    tabs_[0].assign(1, View{});
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
    dnd::revokeTarget(hwnd());
    settings_.lastFolder = here_.thisPC ? L"::thispc" : here_.path;
    if (!here_.collection.empty() || here_.bin) settings_.lastFolder.clear();
    for (int p = 0; p < 2; ++p) {
        settings_.tabs[p].clear();
        for (int t = 0; t < (int)tabs_[p].size(); ++t) {
            const Location& l = tabLocation(p, t);
            settings_.tabs[p].push_back(l.thisPC ? L"::thispc" : l.bin ? L"::bin" : !l.collection.empty() ? L"::collection:" + labels::fromUtf8(l.collection) : l.path);
        }
        settings_.activeTab[p] = activeTab_[p];
    }
    settings_.twoPanes = twoPanes_;
    settings_.save(settingsPath_);
    latest_->store(~0ull);   // any worker still reading gives up
    for (auto& pane : tabs_) for (auto& v : pane) if (v.latest) v.latest->store(~0ull);
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
    if (renaming_) endRename(false);
    quiet_ = false;
    pending_.clear();
    KillTimer(hwnd(), kRefreshTimer);
    if (remember && loadedOnce_ && !(to == here_)) {
        backStack_.push_back(here_);
        if (backStack_.size() > 200) backStack_.erase(backStack_.begin());
        forwardStack_.clear();
    }
    loadedOnce_ = true;
    if (thumbs_) thumbs_->forgetQueue();
    // Searching again for something else in the same place: what's shown stays (narrowed to the new
    // words) until the first new results come, so the list doesn't blink empty at every pause in typing.
    const bool again = to.searching() && here_.searching() && !here_.thisPC && samePath(to.path, here_.path) && to.everywhere == here_.everywhere;
    here_ = to;
    searchReplace_ = again;
    if (!again) {
        listed_.clear();
        shown_.clear();
        sel_.reset(0);
    }
    error_ = ListError::None;
    errorCode_ = 0;
    partial_ = false;
    selectAfterLoad_ = select;
    typed_.clear();
    if (!again) resetScroll();
    loading_ = true;
    loadStarted_ = GetTickCount();
    request_ = nextRequest();
    latest_->store(request_);
    searchLimited_ = searchNoIndex_ = false;
    searchFolders_ = 0;
    KillTimer(hwnd(), kSearchTimer);
    startListing();
    SetTimer(hwnd(), kSpinTimer, 120, nullptr);
    if (!swappedIn_) showSearchText();

    const std::wstring name = nameFor(to);
    const std::wstring title = name + L" – NorthRay Files";
    if (title != windowTitle_ && !swappedIn_) {
        windowTitle_ = title;
        SetWindowTextW(hwnd(), title.c_str());
    }
    invalidate();
}

void FilesWindow::startListing() {
    const Visibility vis{settings_.showHidden, settings_.showProtected};
    if (here_.thisPC) listDrivesAsync(hwnd(), request_, latest_);
    else if (here_.bin) {
        // The Recycle Bin, as the Shell keeps it.
        HWND win = hwnd();
        const std::uint64_t request = request_;
        auto latest = latest_;
        std::thread([win, request, latest] {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            auto* b = new ListBatch;
            b->request = request;
            b->done = true;
            std::vector<ops::BinItem> items;
            std::wstring problem;
            if (!ops::listRecycleBin(items, problem)) {
                b->error = ListError::Other;
                b->code = ERROR_ACCESS_DENIED;
            }
            for (auto& it : items) {
                if (latest->load() != request) break;
                Entry e;
                e.name = it.name;
                e.location = it.from;
                e.size = it.folder ? 0 : it.size;
                e.modified = it.deleted;
                e.attributes = it.folder ? attr::Directory : 0;
                e.binId = it.id;
                e.binFile = it.file;
                describeEntry(e, it.file);
                b->entries.push_back(std::move(e));
            }
            postToWindow(win, WM_APP_LIST, b, [b] { delete b; });
            if (SUCCEEDED(co)) CoUninitialize();
        }).detach();
    } else if (!here_.collection.empty()) {
        // A collection: its things, wherever they are (and the ones that aren't there any more).
        const labels::Collection* c = labels_.collection(here_.collection);
        listPathsAsync(hwnd(), c ? c->items : std::vector<std::wstring>{}, L"", vis, true, request_, latest_);
    } else if (here_.searching() && search::parse(here_.query).byLabel()) {
        // Tags and colours are NorthRay Files' own, so they're looked up rather than searched for.
        const search::Query q = search::parse(here_.query);
        std::vector<std::wstring> paths;
        for (const labels::Label* l : labels_.all()) {
            if (!search::matchesLabels(q, l->tags, (int)l->colour)) continue;
            if (!here_.everywhere && (!labels::within(l->path, here_.path) || labels::pathKey(l->path) == labels::pathKey(here_.path))) continue;
            paths.push_back(l->path);
        }
        listPathsAsync(hwnd(), paths, here_.query, vis, false, request_, latest_);
    } else if (here_.searching() && here_.everywhere) {
        // The whole PC: the Windows Search index, or every drive's folders when it can't be asked.
        std::vector<std::wstring> roots;
        for (const Drive& d : drives_) if (d.ready && (d.type == DRIVE_FIXED || d.type == DRIVE_REMOVABLE)) roots.push_back(d.root);
        if (roots.empty()) roots.push_back(here_.path);
        searchIndexAsync(hwnd(), L"", here_.query, vis, roots, request_, latest_);
    } else if (here_.searching()) searchFoldersAsync(hwnd(), {here_.path}, here_.query, vis, request_, latest_);
    else listFolderAsync(hwnd(), here_.path, request_, latest_);
}

std::wstring FilesWindow::nameFor(const Location& l) const {
    if (l.bin) return L"Recycle Bin";
    if (!l.collection.empty()) {
        const labels::Collection* c = labels_.collection(l.collection);
        return c ? c->name : L"Collection";
    }
    if (l.searching()) return L"\u201C" + l.query + L"\u201D";
    if (l.thisPC) return L"This PC";
    return isDriveRoot(l.path) ? l.path.substr(0, 2) : nameOf(l.path);
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
    if (here_.thisPC || !here_.collection.empty() || here_.bin) return;
    if (here_.searching()) { endSearch(); return; }
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
    if (!shown_[row].binFile.empty()) return shown_[row].binFile;   // in the Recycle Bin: where its contents are kept
    return join(shown_[row].location.empty() ? here_.path : shown_[row].location, shown_[row].name);
}

void FilesWindow::open(std::size_t row) {
    if (here_.thisPC || row >= shown_.size()) return;
    if (here_.bin) { flash(L"Restore it to open it (right-click, or Restore in the preview pane).", 6); return; }
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
        if (cjui::messageBox(hwnd(), q.c_str(), L"NorthRay Files", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    }
    for (std::size_t r : files) open(r);
}

// ---------------------------------------------------------------- results

void FilesWindow::receive(ListBatch* b) {
    if (b->request != request_) {
        // Another tab's listing: give it to that tab.
        if (View* v = viewForRequest(b->request)) {
            swapView(*v);
            receive(b);
            swapView(*v);
            invalidate();
        }
        return;
    }
    if (here_.thisPC) return;
    if (here_.searching()) {
        searchLimited_ = searchLimited_ || b->limited;
        searchNoIndex_ = b->noIndex;
        searchFolders_ = std::max(searchFolders_, b->foldersRead);
    }
    if (b->error == ListError::NotAFolder) {
        // A file's path: its folder, with it selected.
        here_.path = parentOf(here_.path);
        go(here_, false, b->fileToSelect);
        return;
    }
    if (!b->canonical.empty() && b->canonical != here_.path && samePath(b->canonical, here_.path)) {
        here_.path = b->canonical;
        const std::wstring title = (isDriveRoot(here_.path) ? here_.path.substr(0, 2) : nameOf(here_.path)) + L" \u2013 NorthRay Files";
        if (!swappedIn_) {
            windowTitle_ = title;
            SetWindowTextW(hwnd(), title.c_str());
        }
    }
    if (quiet_) {
        // Listing again in the background: swap in the new list when it's all here.
        for (Entry& e : b->entries) pending_.push_back(std::move(e));
        if (!b->done) return;
        quiet_ = false;
        if (b->error == ListError::None || !pending_.empty()) listed_.swap(pending_);
        pending_.clear();
    } else {
        if (searchReplace_ && (!b->entries.empty() || b->done)) { listed_.clear(); searchReplace_ = false; }
        for (Entry& e : b->entries) listed_.push_back(std::move(e));
    }
    if (b->done) {
        loading_ = false;
        if (b->error != ListError::None) {
            if (listed_.empty()) { error_ = b->error; errorCode_ = b->code; }
            else { partial_ = true; errorCode_ = b->code; }
        } else {
            error_ = ListError::None;
        }
        // Changes made here or by any other program show by themselves.
        if (error_ == ListError::None && !here_.searching() && here_.collection.empty() && !here_.bin) ops::watchFolder(hwnd(), WM_APP_CHANGED, here_.path, request_, latest_);
    }
    showHere();
    if (b->done && !selectNames_.empty()) {
        sel_.clear();
        int lastRow = -1;
        for (std::size_t i = 0; i < shown_.size(); ++i)
            for (auto& nm : selectNames_)
                if (CompareStringOrdinal(shown_[i].name.c_str(), -1, nm.c_str(), -1, TRUE) == CSTR_EQUAL) { sel_.click(i, true, false); lastRow = (int)i; }
        if (lastRow >= 0) { sel_.moveTo((std::size_t)lastRow, true, false); ensureVisible((std::size_t)lastRow); }
        selectNames_.clear();
        invalidate();
    }
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
    // A search narrows what it found as more is typed (before it searches again).
    const search::Query q = search::parse(here_.query);
    for (const Entry& e : listed_)
        if (isVisible(e, vis) && (!here_.searching() || search::matches(q, e.name, e.folder()))) shown_.push_back(e);
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
    if (msg.hwnd == rename_ && msg.message == WM_KEYDOWN) {
        if (msg.wParam == VK_RETURN) { endRename(true); return true; }
        if (msg.wParam == VK_ESCAPE) { endRename(false); return true; }
    }
    if (msg.hwnd == ask_ && msg.message == WM_KEYDOWN) {
        if (msg.wParam == VK_RETURN) { endAsk(true); return true; }
        if (msg.wParam == VK_ESCAPE) { endAsk(false); return true; }
    }
    if (batch_.open && msg.message == WM_KEYDOWN) {
        for (int i = 0; i < 5; ++i) {
            if (msg.hwnd != batch_.fields[i]) continue;
            if (msg.wParam == VK_ESCAPE) { closeBatchRename(); return true; }
            if (msg.wParam == VK_RETURN) {
                if (batch::canDo(batchPlan())) doBatchRename();
                return true;
            }
            if (msg.wParam == VK_TAB) {
                const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                HWND next = batch_.fields[(i + (back ? 4 : 1)) % 5];
                SetFocus(next);
                SendMessageW(next, EM_SETSEL, 0, -1);
                invalidate();
                return true;
            }
        }
    }
    if (msg.hwnd == search_ && msg.message == WM_KEYDOWN) {
        if (msg.wParam == VK_RETURN) {
            KillTimer(hwnd(), kSearchTimer);
            if (here_.searching()) go(here_, false);   // search again now
            return true;
        }
        if (msg.wParam == VK_ESCAPE) { endSearch(); SetFocus(hwnd()); return true; }
        if (msg.wParam == VK_DOWN && !shown_.empty()) {
            SetFocus(hwnd());
            if (sel_.focus() < 0) sel_.select(0);
            invalidate();
            return true;
        }
    }
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
    if (batch_.open) {   // the card has the keys
        if (vk == VK_ESCAPE) closeBatchRename();
        return true;
    }
    const std::size_t n = here_.thisPC ? 0 : shown_.size();
    const bool icons = settings_.view != ViewMode::Details;
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
            case 'C': copyOrCut(false); return true;
            case 'X': copyOrCut(true); return true;
            case 'T': newTab(here_); return true;
            case 'W': closeTab(pane_, activeTab_[pane_]); return true;
            case VK_TAB: activate(pane_, (activeTab_[pane_] + 1) % (int)tabs_[pane_].size()); return true;
            case VK_OEM_5: setTwoPanes(!twoPanes_); return true;   // Ctrl and the backslash key
            case 'V': paste(); return true;
            case 'L': beginEditingAddress(); return true;
            case 'F': focusSearch(); return true;
            case 'E': focusSearch(); return true;
            case 'R': refresh(); return true;
            case 'Z': if (!undoBatchRename()) flash(L"Nothing to undo here.", 4); return true;
            case VK_SPACE: sel_.toggleFocused(); return true;
            default: break;
        }
    }
    if (ctrl && shift) {
        switch (vk) {
            case 'C': copyPaths(); return true;
            case 'N': newFolder(); return true;
            case VK_F2: openBatchRename(); return true;
            case VK_TAB: activate(pane_, (activeTab_[pane_] + (int)tabs_[pane_].size() - 1) % (int)tabs_[pane_].size()); return true;
            case '6': setView(ViewMode::Details); return true;
            case '2': setView(ViewMode::Icons); return true;
            case '1': setView(ViewMode::Gallery); return true;
            default: break;
        }
    }
    switch (vk) {
        case VK_F4: beginEditingAddress(); return true;
        case VK_F3: focusSearch(); return true;
        case VK_F2: beginRename(); return true;
        case VK_F6: if (twoPanes_) { activate(1 - pane_, activeTab_[1 - pane_]); return true; } break;
        case VK_DELETE: deleteSelection(shift); return true;
        case VK_F5: refresh(); return true;
        case VK_BACK: back(); return true;
        case VK_BROWSER_BACK: back(); return true;
        case VK_BROWSER_FORWARD: forward(); return true;
        case VK_BROWSER_REFRESH: refresh(); return true;
        case VK_RETURN: openSelection(); return true;
        case VK_ESCAPE:
            if (sel_.count() == 0 && here_.searching()) endSearch();
            else sel_.clear();
            return true;
        case VK_DOWN: moveTo(focus < 0 ? 0 : focus + cols); return true;
        case VK_UP: moveTo(focus < 0 ? 0 : focus - cols); return true;
        case VK_RIGHT: if (icons) { moveTo(focus < 0 ? 0 : focus + 1); return true; } break;
        case VK_LEFT: if (icons) { moveTo(focus < 0 ? 0 : focus - 1); return true; } break;
        case VK_HOME: moveTo(0); return true;
        case VK_END: moveTo((long long)n - 1); return true;
        case VK_NEXT: moveTo(focus < 0 ? 0 : focus + page); return true;
        case VK_PRIOR: moveTo(focus < 0 ? 0 : focus - page); return true;
        case VK_APPS:
        case VK_F10: {
            if (vk == VK_F10 && !shift) break;
            POINT pt{(LONG)((listClip_.x + 40) * gfx_.dpiScale()), (LONG)((listClip_.y + 40) * gfx_.dpiScale())};
            ClientToScreen(hwnd(), &pt);
            if (vk == VK_F10) moreOptions(pt);   // Shift+F10: Windows' own menu
            else showItemMenu(pt);
            return true;
        }
        default: break;
    }
    return false;
}

bool FilesWindow::onMouseDown(float x, float y) {
    if (batch_.open) {
        if (batchCard_.contains(x, y)) return false;   // its buttons
        closeBatchRename();
        return true;
    }
    if (asking_) {
        if (askCard_.contains(x, y)) return false;   // its buttons
        endAsk(false);
        return true;
    }
    if (editing_) endEditingAddress(false);
    if (renaming_) endRename(true);
    drag_ = Drag{};
    // A click in the other pane makes it the active one first.
    if (const int p = paneAt(x, y); p >= 0 && p != pane_) {
        activate(p, activeTab_[p]);
        invalidate();
    }
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
    // Pressing on something already selected keeps the selection (it may be dragged); otherwise select.
    const bool keep = sel_.selected((std::size_t)row) && !ctrl && !shift;
    if (!keep) sel_.click((std::size_t)row, ctrl, shift);
    drag_.armed = true;
    drag_.sx = drag_.x = x;
    drag_.sy = drag_.y = y;
    drag_.paths = here_.bin ? std::vector<std::wstring>{} : selectedPaths();   // nothing is dragged out of the Recycle Bin
    drag_.fromPane = pane_;
    drag_.clickRow = keep ? (int)row : -1;
    return true;
}

bool FilesWindow::onRightClick(float x, float y) {
    if (const int p = paneAt(x, y); p >= 0 && p != pane_) activate(p, activeTab_[p]);
    // The Recycle Bin in the sidebar.
    if (binRect_.contains(x, y)) {
        const float s = gfx_.dpiScale();
        POINT pt{(LONG)(x * s), (LONG)(y * s)};
        ClientToScreen(hwnd(), &pt);
        recycleBinMenu(pt);
        return true;
    }
    // A collection in the sidebar.
    for (auto& [id, rr] : collectionRects_)
        if (rr.contains(x, y)) {
            const float s = gfx_.dpiScale();
            POINT pt{(LONG)(x * s), (LONG)(y * s)};
            ClientToScreen(hwnd(), &pt);
            collectionMenu(id, pt);
            return true;
        }
    // A pinned folder in the sidebar.
    for (auto& [path, rr] : pinnedRects_)
        if (rr.contains(x, y)) {
            const float s = gfx_.dpiScale();
            POINT pt{(LONG)(x * s), (LONG)(y * s)};
            ClientToScreen(hwnd(), &pt);
            sidebarMenu(path, pt);
            return true;
        }
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
        case WM_APP_PREVIEW: {
            auto* t = reinterpret_cast<TextPreview*>(lp);
            if (t->key == previewKey_) {
                previewText_ = std::move(t->text);
                previewTruncated_ = t->truncated;
                previewState_ = t->notText ? PreviewNotText : PreviewText;
                invalidate();
            }
            delete t;
            handled = true;
            return 0;
        }
        case WM_APP_THUMB: {
            auto* t = reinterpret_cast<ThumbResult*>(lp);
            if (thumbs_) {
                thumbs_->received(t);
                for (std::uint64_t serial : thumbs_->takeEvicted()) gfx_.forgetImage(L"thumb:" + std::to_wstring(serial));
                invalidate();   // (Icons and Gallery views, and the preview pane)
            }
            delete t;
            handled = true;
            return 0;
        }
        case WM_APP_CHANGED:
            // Something in the folder changed (here or in another program): list it again shortly, keeping the place.
            if ((std::uint64_t)wp == request_ && !here_.thisPC) treeKids_.erase(lowerKey(here_.path));   // its sub-folders may have changed
            if ((std::uint64_t)wp == request_ && !here_.thisPC && !renaming_) SetTimer(hwnd(), kRefreshTimer, 350, nullptr);
            else if (View* v = viewForRequest((std::uint64_t)wp); v && !v->here.thisPC) {
                swapView(*v);
                reloadQuietly();   // another tab's folder: listed again in the background
                swapView(*v);
            }
            handled = true;
            return 0;
        case WM_INITMENUPOPUP:
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
        case WM_MENUCHAR: {
            // Windows' own right-click menu's submenus ("Send to", "Open with").
            LRESULT r = 0;
            if (ops::forwardMenuMessage(msg, wp, lp, r)) { handled = true; return r; }
            break;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) updateRecycleBin();
            break;
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
                case 'P': togglePreview(); handled = true; return 0;
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
            if ((HWND)lp == rename_ && HIWORD(wp) == EN_KILLFOCUS) {
                endRename(true);
                handled = true;
                return 0;
            }
            if (batch_.open && std::find(std::begin(batch_.fields), std::end(batch_.fields), (HWND)lp) != std::end(batch_.fields)) {
                if (HIWORD(wp) == EN_CHANGE || HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) invalidate();
                handled = true;
                return 0;
            }
            if ((HWND)lp == search_ && HIWORD(wp) == EN_CHANGE) {
                if (!settingSearch_) searchTyped();
                handled = true;
                return 0;
            }
            if ((HWND)lp == search_ && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)) {
                invalidate();
                handled = true;
                return 0;
            }
            if ((HWND)lp == address_ && HIWORD(wp) == EN_KILLFOCUS) {
                endEditingAddress(false);
                handled = true;
                return 0;
            }
            break;
        case WM_CTLCOLOREDIT:
            if ((HWND)lp == address_ || (HWND)lp == rename_ || (HWND)lp == search_ || (HWND)lp == ask_ ||
                std::find(std::begin(batch_.fields), std::end(batch_.fields), (HWND)lp) != std::end(batch_.fields)) {
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
    if (id == kSearchTimer) {
        KillTimer(hwnd(), kSearchTimer);
        if (here_.searching()) go(here_, false);
        return;
    }
    if (id == kRefreshTimer) {
        KillTimer(hwnd(), kRefreshTimer);
        if (!renaming_) reloadQuietly();
        return;
    }
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
    treeKids_.clear();
    settings_.showHidden = !settings_.showHidden;
    settings_.save(settingsPath_);
    showHere();
    flash(settings_.showHidden ? L"Hidden items are shown, a little faded." : L"Hidden items are hidden.");
}

void FilesWindow::toggleProtected() {
    if (!settings_.showProtected &&
        cjui::messageBox(hwnd(),
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
                                MenuItem::item(L"Icons", CmdViewIcons, L"Ctrl+Shift+2", true, settings_.view == ViewMode::Icons),
                                MenuItem::item(L"Gallery", CmdViewGallery, L"Ctrl+Shift+1", true, settings_.view == ViewMode::Gallery)}),
        MenuItem::sub(L"Sort by", {MenuItem::item(L"Name", CmdSortName, {}, true, k == SortKey::Name),
                                   MenuItem::item(L"Date modified", CmdSortDate, {}, true, k == SortKey::Modified),
                                   MenuItem::item(L"Type", CmdSortType, {}, true, k == SortKey::Type),
                                   MenuItem::item(L"Size", CmdSortSize, {}, true, k == SortKey::Size), MenuItem::sep(),
                                   MenuItem::item(L"Ascending", CmdSortAsc, {}, true, !settings_.sortDescending),
                                   MenuItem::item(L"Descending", CmdSortDesc, {}, true, settings_.sortDescending)}),
        MenuItem::item(L"Refresh", CmdRefresh, L"F5"),
        MenuItem::sep(),
        MenuItem::item(L"New tab", CmdNewTab, L"Ctrl+T"),
        MenuItem::item(L"Two panes side by side", CmdTwoPanes, L"Ctrl+\\", true, twoPanes_),
        MenuItem::item(L"Preview pane", CmdPreview, L"Alt+P", true, settings_.previewPane),
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
        case CmdViewGallery: setView(ViewMode::Gallery); break;
        case CmdSortName: sortBy(SortKey::Name); break;
        case CmdSortDate: sortBy(SortKey::Modified); break;
        case CmdSortType: sortBy(SortKey::Type); break;
        case CmdSortSize: sortBy(SortKey::Size); break;
        case CmdSortAsc: settings_.sortDescending = false; settings_.save(settingsPath_); showHere(); break;
        case CmdSortDesc: settings_.sortDescending = true; settings_.save(settingsPath_); showHere(); break;
        case CmdRefresh: refresh(); break;
        case CmdNewTab: newTab(here_); break;
        case CmdTwoPanes: setTwoPanes(!twoPanes_); break;
        case CmdPreview: togglePreview(); break;
        case CmdHidden: toggleHidden(); break;
        case CmdProtected: toggleProtected(); break;
        case CmdThemeSystem: case CmdThemeLight: case CmdThemeDark:
            settings_.theme = (Theme)(cmd - CmdThemeSystem);
            settings_.save(settingsPath_);
            applyTheme();
            break;
        case CmdAbout:
            cjui::messageBox(hwnd(),
                        L"NorthRay Files " CJ_VERSION_WSTR L" (preview)\n\nDeveloped by the " CJ_DEVELOPER_WSTR
                        L" project.\nNorthRaySoftware@protonmail.com\n\nCopying, moving, renaming and deleting go through Windows' own file "
                        L"operations, so they behave as in File Explorer and deleted items go to the Recycle Bin.",
                        L"About NorthRay Files", MB_OK | MB_ICONINFORMATION);
            break;
        default: break;
    }
    invalidate();
}

void FilesWindow::showItemMenu(POINT screen) {
    const std::size_t n = sel_.count();
    const bool canPaste = ops::clipboardHasFiles();
    std::vector<MenuItem> items;
    if (here_.bin) {
        // The Recycle Bin's own menu.
        if (n > 0) {
            items.push_back(MenuItem::item(L"Restore", CmdBinRestore));
            items.push_back(MenuItem::item(L"Delete permanently\u2026", CmdBinDelete, L"Delete"));
            items.push_back(MenuItem::sep());
        }
        items.push_back(MenuItem::item(L"Empty the Recycle Bin\u2026", CmdBinEmpty, {}, !shown_.empty()));
        items.push_back(MenuItem::item(L"Refresh", CmdRefresh, L"F5"));
        const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
        if (cmd == CmdBinRestore) binCommand(true);
        else if (cmd == CmdBinDelete) binCommand(false);
        else if (cmd == CmdBinEmpty) {
            const ops::Result r = ops::emptyRecycleBin(hwnd());
            if (!r.ok) flash(L"The Recycle Bin couldn't be emptied. " + r.message, 8);
            updateRecycleBin();
            reloadQuietly();
        } else if (cmd == CmdRefresh) refresh();
        invalidate();
        return;
    }
    if (n > 0) {
        const auto rows = sel_.indices();
        const bool oneFolder = n == 1 && shown_[rows[0]].folder();
        items.push_back(MenuItem::item(L"Open", CmdOpen, L"Enter"));
        if (oneFolder) items.push_back(MenuItem::item(L"Open in a new tab", CmdNewTabHere, L""));
        if (oneFolder) items.push_back(MenuItem::item(L"Pin to the sidebar", CmdPin));
        if (twoPanes_) {
            items.push_back(MenuItem::sep());
            items.push_back(MenuItem::item(L"Copy to the other pane", CmdCopyOther));
            items.push_back(MenuItem::item(L"Move to the other pane", CmdMoveOther));
        }
        items.push_back(MenuItem::sep());
        items.push_back(MenuItem::item(L"Cut", CmdCut, L"Ctrl+X"));
        items.push_back(MenuItem::item(L"Copy", CmdCopy, L"Ctrl+C"));
        if (oneFolder) items.push_back(MenuItem::item(L"Paste into this folder", CmdPasteInto, {}, canPaste));
        items.push_back(MenuItem::item(L"Rename", CmdRename, L"F2", n == 1));
        if (n >= 2) items.push_back(MenuItem::item(L"Rename these together\u2026", CmdBatchRename, L"Ctrl+Shift+F2", !here_.bin));
        items.push_back(MenuItem::item(L"Delete", CmdDelete, L"Delete"));
        items.push_back(MenuItem::sep());
        items.push_back(MenuItem::item(n > 1 ? L"Copy paths" : L"Copy path", CmdCopyPath, L"Ctrl+Shift+C"));
        items.push_back(MenuItem::sep());
        // Labels (0.5): the same for everything selected is ticked.
        const auto paths = selectedPaths();
        std::vector<MenuItem> colours;
        for (int c = 0; c < labels::kColours; ++c) {
            const bool all = std::all_of(paths.begin(), paths.end(), [&](const std::wstring& pth) { return (int)labels_.colourOf(pth) == c; });
            colours.push_back(MenuItem::item(c == 0 ? L"No colour" : labels::colourName((labels::Colour)c), CmdColour0 + (UINT)c, {}, true, all));
        }
        items.push_back(MenuItem::sub(L"Colour label", colours));
        std::vector<MenuItem> tags;
        tagChoices_ = labels_.tags();
        for (std::size_t t = 0; t < tagChoices_.size() && t < 900; ++t) {
            const auto& tag = tagChoices_[t].first;
            const bool all = std::all_of(paths.begin(), paths.end(), [&](const std::wstring& pth) { return labels_.hasTag(pth, tag); });
            tags.push_back(MenuItem::item(tag, CmdTag0 + (UINT)t, {}, true, all));
        }
        if (!tags.empty()) tags.push_back(MenuItem::sep());
        tags.push_back(MenuItem::item(L"New tag\u2026", CmdNewTag));
        items.push_back(MenuItem::sub(L"Tags", tags));
        std::vector<MenuItem> cols;
        const auto& all = labels_.collections();
        for (std::size_t c = 0; c < all.size() && c < 900; ++c) {
            const bool in = std::all_of(paths.begin(), paths.end(), [&](const std::wstring& pth) {
                auto w = labels_.collectionsWith(pth);
                return std::find(w.begin(), w.end(), all[c].id) != w.end();
            });
            cols.push_back(MenuItem::item(all[c].name, CmdColl0 + (UINT)c, {}, true, in));
        }
        if (!cols.empty()) cols.push_back(MenuItem::sep());
        cols.push_back(MenuItem::item(L"New collection\u2026", CmdNewCollection));
        items.push_back(MenuItem::sub(L"Add to collection", cols));
        if (!here_.collection.empty()) items.push_back(MenuItem::item(L"Remove from this collection", CmdCollRemove));
        items.push_back(MenuItem::sep());
        items.push_back(MenuItem::item(L"More options", CmdMore, L"Shift+F10"));
    } else {
        items.push_back(MenuItem::item(L"Paste", CmdPaste, L"Ctrl+V", canPaste));
        items.push_back(MenuItem::item(L"New folder", CmdNewFolder, L"Ctrl+Shift+N"));
        items.push_back(MenuItem::sep());
        items.push_back(MenuItem::sub(L"View", {MenuItem::item(L"Details", CmdViewDetails, {}, true, settings_.view == ViewMode::Details),
                                                MenuItem::item(L"Icons", CmdViewIcons, {}, true, settings_.view == ViewMode::Icons),
                                                MenuItem::item(L"Gallery", CmdViewGallery, {}, true, settings_.view == ViewMode::Gallery)}));
        items.push_back(MenuItem::item(L"Refresh", CmdRefresh, L"F5"));
        items.push_back(MenuItem::item(L"Copy this folder's path", CmdCopyPath, L"Ctrl+Shift+C"));
        items.push_back(MenuItem::item(L"Show hidden items", CmdHidden, {}, true, settings_.showHidden));
        items.push_back(MenuItem::sep());
        items.push_back(MenuItem::item(L"More options", CmdMore, L"Shift+F10"));
    }
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (cmd >= CmdColour0 && cmd < CmdColour0 + labels::kColours) { setColourOnSelection((labels::Colour)(cmd - CmdColour0)); return; }
    if (cmd >= CmdTag0 && cmd < CmdTag0 + 900) {
        if (cmd - CmdTag0 < tagChoices_.size()) toggleTagOnSelection(tagChoices_[cmd - CmdTag0].first);
        return;
    }
    if (cmd >= CmdColl0 && cmd < CmdColl0 + 900) {
        const auto& all = labels_.collections();
        if (cmd - CmdColl0 < all.size()) addSelectionToCollection(all[cmd - CmdColl0].id);
        return;
    }
    switch (cmd) {
        case CmdNewTag: newTagForSelection(); break;
        case CmdNewCollection: newCollectionFromSelection(); break;
        case CmdCollRemove: {
            const int n2 = labels_.removeFromCollection(here_.collection, selectedPaths());
            saveLabels();
            if (n2) reloadQuietly();
            break;
        }
        case CmdOpen: openSelection(); break;
        case CmdNewTabHere: { const auto rows = sel_.indices(); if (rows.size() == 1) newTab({false, rowPath(rows[0])}); break; }
        case CmdPin: { const auto rows = sel_.indices(); if (rows.size() == 1) pinFolder(rowPath(rows[0])); break; }
        case CmdCopyOther: transferToOtherPane(false); break;
        case CmdMoveOther: transferToOtherPane(true); break;
        case CmdCut: copyOrCut(true); break;
        case CmdCopy: copyOrCut(false); break;
        case CmdPaste: paste(); break;
        case CmdPasteInto: { const auto rows = sel_.indices(); if (rows.size() == 1) paste(rowPath(rows[0])); break; }
        case CmdRename: beginRename(); break;
        case CmdBatchRename: openBatchRename(); break;
        case CmdDelete: deleteSelection(false); break;
        case CmdNewFolder: newFolder(); break;
        case CmdCopyPath: copyPaths(); break;
        case CmdMore: moreOptions(screen); break;
        case CmdViewDetails: setView(ViewMode::Details); break;
        case CmdViewIcons: setView(ViewMode::Icons); break;
        case CmdViewGallery: setView(ViewMode::Gallery); break;
        case CmdRefresh: refresh(); break;
        case CmdHidden: toggleHidden(); break;
        default: break;
    }
    invalidate();
}

// ---------------------------------------------------------------- file actions

std::vector<std::wstring> FilesWindow::selectedPaths() const {
    std::vector<std::wstring> out;
    for (std::size_t r : sel_.indices()) out.push_back(rowPath(r));
    return out;
}

std::vector<std::wstring> FilesWindow::namesHere() const {
    std::vector<std::wstring> out;
    for (const Entry& e : listed_) out.push_back(e.name);
    return out;
}

static std::wstring lowerPath(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

void FilesWindow::copyOrCut(bool cut) {
    const auto paths = selectedPaths();
    if (paths.empty() || here_.thisPC) return;
    if (here_.bin) { flash(L"Restore things from the Recycle Bin before copying or moving them.", 6); return; }
    if (!ops::putOnClipboard(hwnd(), paths, cut)) { flash(L"The clipboard is busy. Try again.", 5); return; }
    cutPaths_.clear();
    if (cut) for (auto& p : paths) cutPaths_.insert(lowerPath(p));
    const std::wstring what = paths.size() == 1 ? L"\u201C" + nameOf(paths[0]) + L"\u201D" : std::to_wstring(paths.size()) + L" items";
    flash(cut ? L"Cut " + what + L". Paste (Ctrl+V) in another folder to move it there." : L"Copied " + what + L". Paste (Ctrl+V) where you want it.", 5);
    invalidate();
}

void FilesWindow::paste(const std::wstring& into) {
    if ((here_.thisPC || here_.searching() || !here_.collection.empty() || here_.bin) && into.empty()) { flash(L"Open a folder to paste into it.", 5); return; }
    const std::wstring dest = into.empty() ? here_.path : into;
    const auto clip = ops::readClipboard(hwnd());
    if (clip.paths.empty()) { flash(L"There are no files on the clipboard. Copy (Ctrl+C) or cut (Ctrl+X) some first.", 6); return; }
    const TransferPlan plan = planTransfer(clip.paths, dest, clip.cut);
    if (plan.nothingToDo) { flash(L"They're in this folder already.", 5); return; }
    if (!plan.problem.empty()) flash(plan.problem, 6);
    if (plan.sources.empty()) return;
    const auto before = namesHere();
    // Copies of things already in this folder get their own names ("cover - Copy.png"); the rest go as they are.
    std::vector<std::wstring> plain, arrived;
    std::vector<std::pair<std::wstring, std::wstring>> beside;
    std::vector<std::wstring> taken = dest == here_.path ? before : std::vector<std::wstring>{};
    for (auto& src : plan.sources) {
        if (!clip.cut && CompareStringOrdinal(parentOf(src).c_str(), -1, dest.c_str(), -1, TRUE) == CSTR_EQUAL) {
            const DWORD a = GetFileAttributesW(extendedPath(src).c_str());
            const std::wstring nm = copyName(nameOf(src), taken, a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY));
            taken.push_back(nm);
            beside.push_back({src, join(dest, nm)});
            arrived.push_back(nm);
        } else {
            plain.push_back(src);
            arrived.push_back(nameOf(src));
        }
    }
    ops::Result r = ops::transfer(hwnd(), plain, dest, clip.cut);
    if (clip.cut) noteMoved(plain, dest);
    if (!beside.empty() && !r.cancelled) {
        const ops::Result r2 = ops::copyAs(hwnd(), beside);
        if (!r2.ok) r = r2;
    }
    if (clip.cut && r.ok) {
        // Moved: the clipboard's files aren't where it says any more.
        if (OpenClipboard(hwnd())) { EmptyClipboard(); CloseClipboard(); }
        cutPaths_.clear();
    }
    if (!r.ok && !r.cancelled) cjui::messageBox(hwnd(), (L"Some items couldn't be " + std::wstring(clip.cut ? L"moved" : L"copied") + L".\n\n" + r.message).c_str(),
                                                L"NorthRay Files", MB_OK | MB_ICONWARNING);
    if (dest == here_.path) {
        reloadQuietly(arrived);   // what arrived, selected
    } else if (r.ok) {
        flash((clip.cut ? L"Moved into \u201C" : L"Copied into \u201C") + nameOf(dest) + L"\u201D.", 5);
        if (clip.cut) reloadQuietly();
    }
    (void)before;
}

void FilesWindow::binCommand(bool restore) {
    std::vector<std::wstring> ids;
    for (std::size_t r : sel_.indices()) ids.push_back(shown_[r].binId);
    if (ids.empty()) return;
    const std::size_t n = ids.size();
    const ops::Result r = ops::recycleBinCommand(hwnd(), ids, restore);
    if (!r.ok && !r.cancelled) flash((restore ? L"It couldn't be restored. " : L"It couldn't be deleted. ") + r.message, 8);
    else if (r.ok && restore) flash(n == 1 ? L"Restored to where it was." : L"Restored " + std::to_wstring(n) + L" items to where they were.", 5);
    updateRecycleBin();
    reloadQuietly();
}

void FilesWindow::deleteSelection(bool permanently) {
    if (here_.bin) { binCommand(false); return; }
    const auto paths = selectedPaths();
    if (paths.empty() || here_.thisPC) return;
    const std::wstring what = paths.size() == 1 ? L"\u201C" + nameOf(paths[0]) + L"\u201D" : std::to_wstring(paths.size()) + L" items";
    if (permanently &&
        cjui::messageBox(hwnd(), (L"Delete " + what + L" permanently?\n\nThey won't go to the Recycle Bin, so they can't be brought back.").c_str(),
                         L"Delete permanently", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
        return;
    // The row after the last one deleted keeps the focus.
    std::wstring after;
    const auto rows = sel_.indices();
    if (!rows.empty() && rows.back() + 1 < shown_.size()) after = shown_[rows.back() + 1].name;
    else if (!rows.empty() && rows.front() > 0) after = shown_[rows.front() - 1].name;
    const ops::Result r = ops::remove(hwnd(), paths, permanently);
    noteRemoved(paths);
    if (!r.ok && !r.cancelled)
        cjui::messageBox(hwnd(), (L"Some items couldn't be deleted.\n\n" + r.message).c_str(), L"NorthRay Files", MB_OK | MB_ICONWARNING);
    else if (r.ok)
        flash(permanently ? L"Deleted " + what + L"." : L"Moved " + what + L" to the Recycle Bin.", 5);
    updateRecycleBin();
    reloadQuietly(after.empty() ? std::vector<std::wstring>{} : std::vector<std::wstring>{after});
}

void FilesWindow::newFolder() {
    if (here_.thisPC || here_.searching() || !here_.collection.empty() || here_.bin) { flash(L"Open a folder to make a new one in it.", 5); return; }
    const std::wstring name = uniqueName(L"New folder", namesHere(), true);
    const ops::Result r = ops::newFolder(join(here_.path, name));
    if (!r.ok) { flash(L"A folder couldn't be made here. " + r.message, 8); return; }
    // Into the list straight away, selected, with its name ready to type.
    Entry e;
    e.name = name;
    e.attributes = attr::Directory;
    e.type = L"File folder";
    for (const Entry& x : listed_) if (x.folder() && x.icon >= 0) { e.icon = x.icon; break; }
    listed_.push_back(e);
    showHere();
    for (std::size_t i = 0; i < shown_.size(); ++i)
        if (shown_[i].name == name) { sel_.select(i); ensureVisible(i); break; }
    invalidate();
    UpdateWindow(hwnd());
    beginRename();
}

void FilesWindow::beginRename() {
    const auto rows = sel_.indices();
    if (rows.size() != 1 || here_.thisPC) return;
    if (here_.bin) { flash(L"Restore it to rename it.", 5); return; }
    if (renaming_) endRename(false);
    renameRow_ = (int)rows[0];
    renameName_ = shown_[rows[0]].name;
    renameIn_ = shown_[rows[0]].location.empty() ? here_.path : shown_[rows[0]].location;
    ensureVisible(rows[0]);
    renaming_ = true;
    renameRect_ = {};
    SetWindowTextW(rename_, renameName_.c_str());
    invalidate();
    UpdateWindow(hwnd());   // places the field
    ShowWindow(rename_, SW_SHOW);
    SetFocus(rename_);
    // The name without its extension is selected, as in File Explorer.
    const std::wstring ext = shown_[rows[0]].folder() ? L"" : extensionOf(renameName_);
    SendMessageW(rename_, EM_SETSEL, 0, (LPARAM)(renameName_.size() - (ext.size() < renameName_.size() ? ext.size() : 0)));
}

void FilesWindow::endRename(bool commit) {
    if (!renaming_) return;
    renaming_ = false;
    std::wstring text;
    const int n = GetWindowTextLengthW(rename_);
    text.assign((size_t)n, L'\0');
    if (n) GetWindowTextW(rename_, text.data(), n + 1);
    ShowWindow(rename_, SW_HIDE);
    if (GetFocus() == rename_) SetFocus(hwnd());
    invalidate();
    // Spaces at the ends aren't part of a name.
    while (!text.empty() && text.back() == L' ') text.pop_back();
    while (!text.empty() && text.front() == L' ') text.erase(0, 1);
    if (!commit || text == renameName_) return;
    std::vector<std::wstring> others;
    for (const Entry& e : listed_)
        if (e.name != renameName_ && (e.location.empty() ? here_.path : e.location) == renameIn_) others.push_back(e.name);
    const std::wstring problem = renameProblem(renameName_, text, others);
    if (!problem.empty()) { flash(problem, 7); return; }
    const ops::Result r = ops::rename(hwnd(), join(renameIn_, renameName_), text);
    if (!r.ok && !r.cancelled) { flash(L"It couldn't be renamed. " + r.message, 8); return; }
    if (r.ok) {
        labels_.moved(join(renameIn_, renameName_), join(renameIn_, text));
        saveLabels();
        if (!here_.collection.empty()) { reloadQuietly(); return; }
        reloadQuietly({text});
    }
}

void FilesWindow::reloadQuietly(std::vector<std::wstring> select) {
    if (here_.thisPC) { refresh(); return; }
    quiet_ = true;
    pending_.clear();
    selectNames_.clear();
    for (auto& n : select) selectNames_.insert(n);
    request_ = nextRequest();
    latest_->store(request_);
    searchReplace_ = false;
    startListing();
}

void FilesWindow::moreOptions(POINT screen) {
    if (here_.thisPC || here_.bin) return;
    const bool extended = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const auto paths = selectedPaths();
    std::wstring folder = here_.path;
    if (here_.searching() || !here_.collection.empty()) {
        // Windows' menu is for things in one folder.
        if (paths.empty()) return;
        folder = parentOf(paths[0]);
        for (auto& p : paths)
            if (!samePath(parentOf(p), folder)) { flash(L"Windows' own menu needs things from one folder. Select fewer.", 6); return; }
    }
    ops::showShellMenu(hwnd(), folder, paths, screen, extended);
    // Whatever it did (a new file, Send to, Properties...) shows when the folder changes.
}

void FilesWindow::updateRecycleBin() {
    std::uint64_t items = 0, bytes = 0;
    if (ops::recycleBinInfo(items, bytes)) {
        if (items != recycleItems_ || bytes != recycleBytes_) {
            invalidate();
            if (here_.bin && !loading_) reloadQuietly();
        }
        recycleItems_ = items;
        recycleBytes_ = bytes;
    }
}

void FilesWindow::recycleBinMenu(POINT screen) {
    updateRecycleBin();
    const std::vector<MenuItem> items = {
        MenuItem::item(L"Open", CmdBinRestore),
        MenuItem::item(L"Open in File Explorer", CmdBinOpen),
        MenuItem::item(L"Empty the Recycle Bin\u2026", CmdBinEmpty, {}, recycleItems_ > 0),
    };
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (cmd == CmdBinRestore) { Location b; b.bin = true; go(b); }
    if (cmd == CmdBinOpen) ShellExecuteW(hwnd(), L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL);
    if (cmd == CmdBinEmpty) {
        const ops::Result r = ops::emptyRecycleBin(hwnd());
        if (!r.ok) flash(L"The Recycle Bin couldn't be emptied. " + r.message, 8);
        updateRecycleBin();
        if (here_.bin) reloadQuietly();
    }
    invalidate();
}



// ---------------------------------------------------------------- the folder tree

// A folder's sub-folders (names, in File Explorer's order), read once and kept until it's opened again.
const std::vector<std::wstring>& FilesWindow::treeChildren(const std::wstring& path) {
    const std::wstring key = lowerKey(path);
    if (auto it = treeKids_.find(key); it != treeKids_.end()) return it->second;
    std::vector<Entry> found;
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = extendedPath(join(path, L"*"));
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            Entry e;
            e.name = fd.cFileName;
            e.attributes = fd.dwFileAttributes;
            if (!isVisible(e, {settings_.showHidden, settings_.showProtected})) continue;
            found.push_back(e);
        } while (found.size() < 2000 && FindNextFileW(h, &fd));
        FindClose(h);
    }
    sortEntries(found, {SortKey::Name, false});
    std::vector<std::wstring> names;
    for (auto& e : found) names.push_back(e.name);
    return treeKids_[key] = std::move(names);
}

// The drives as the roots of a tree: a triangle opens or closes a folder, a click opens it. The tree
// opens its way to the folder you're in.
void FilesWindow::drawTree(const Rect& r, float& y, float bottom) {
    if (!here_.thisPC && here_.path != treeFollows_) {
        treeFollows_ = here_.path;
        for (std::wstring p = parentOf(here_.path); !p.empty(); p = parentOf(p)) treeOpen_.insert(lowerKey(p));
        // Scroll so the folder you're in shows (worked out below from where its row lands, once the
        // drives and folders it sits in have been read).
        treeReveal_ = true;
    }
    const Rect area{r.x, y, r.w, std::max(0.0f, bottom - y)};
    treeRect_ = area;
    gfx_.pushClip(area);
    setHitClip(area);
    float ty = y - treeScroll_;
    float hereY = -1;
    const float rowH = 28;
    std::function<void(const std::wstring&, const std::wstring&, int, int)> draw = [&](const std::wstring& path, const std::wstring& label, int icon, int depth) {
        const bool open = treeOpen_.count(lowerKey(path)) != 0;
        const bool selected = !here_.thisPC && samePath(here_.path, path);
        if (selected) hereY = ty;
        const float indent = 14.0f * (float)depth;
        const Rect rr{r.x + 10, ty, r.w - 20, rowH - 2};
        if (ty + rowH >= area.y && ty <= area.bottom()) {
            const std::string id = "tb:tree:" + narrow(path);
            addHit(id, {rr.x + 22 + indent, rr.y, rr.w - 22 - indent, rr.h}, [this, path] { goPath(path); });
            const float hv = hoverAmount(id);
            if (selected) {
                gfx_.fillRounded(rr, 7, nr::kNightInk.withAlpha(0.10f));
                gfx_.fillRounded({rr.x, rr.y + 7, 3, rr.h - 14}, 1.5f, nr::kAuroraGreen);
            } else if (hv > 0.01f) {
                gfx_.fillRounded(rr, 7, nr::kNightInk.withAlpha(0.06f * hv));
            }
            // The triangle: open or closed.
            const std::string tid = id + ":open";
            const Rect tri{rr.x + 4 + indent, rr.y + (rr.h - 18) / 2, 18, 18};
            addHit(tid, tri, [this, path, open] {
                const std::wstring k = lowerKey(path);
                if (open) treeOpen_.erase(k);
                else { treeKids_.erase(k); treeOpen_.insert(k); }   // read again when opened
                invalidate();
            }, false);
            const float cx = tri.x + 9, cy = tri.y + 9;
            const Color tc = nr::kNightMuted.mix(nr::kNightInk, hoverAmount(tid) * 0.6f);
            if (open) { gfx_.line(cx - 3.5f, cy - 1.5f, cx, cy + 2, tc, 1.3f); gfx_.line(cx, cy + 2, cx + 3.5f, cy - 1.5f, tc, 1.3f); }
            else { gfx_.line(cx - 1.5f, cy - 3.5f, cx + 2, cy, tc, 1.3f); gfx_.line(cx + 2, cy, cx - 1.5f, cy + 3.5f, tc, 1.3f); }
            drawIcon(icon, {rr.x + 24 + indent, rr.y + (rr.h - 16) / 2, 16, 16});
            gfx_.text(label, {rr.x + 46 + indent, rr.y, rr.w - 52 - indent, rr.h}, selected ? Font::BodyStrong : Font::Body,
                      selected ? nr::kNightInk : nr::kNightMuted.mix(nr::kNightInk, 0.35f + 0.4f * hv), {Align::Left, false, true, true});
            if (focused(id)) focusRing(rr);
        }
        ty += rowH;
        if (!open || depth > 14) return;
        int folderIcon = icon;
        for (const Place& p : places_) if (p.icon >= 0) { folderIcon = p.icon; break; }
        for (const std::wstring& name : treeChildren(path)) draw(join(path, name), name, folderIcon, depth + 1);
    };
    for (const Drive& d : drives_) {
        if (!d.ready) continue;   // an empty card reader has nothing to open
        draw(d.root, d.label + L" (" + d.root.substr(0, 2) + L")", d.icon, 0);
    }
    treeContent_ = ty + treeScroll_ - y;
    clearHitClip();
    gfx_.popClip();
    if (treeReveal_ && hereY > -1) {
        // Bring the folder you're in into view.
        treeReveal_ = false;
        if (hereY < area.y || hereY + rowH > area.bottom()) {
            treeScroll_ = std::max(0.0f, treeScroll_ + hereY - area.y - area.h / 2);
            treeScroll_ = std::min(treeScroll_, std::max(0.0f, treeContent_ - area.h));
            invalidate();
        }
    }
    treeScroll_ = std::clamp(treeScroll_, 0.0f, std::max(0.0f, treeContent_ - area.h));
    y = area.bottom();
}

// ---------------------------------------------------------------- the preview pane

void FilesWindow::togglePreview() {
    settings_.previewPane = !settings_.previewPane;
    settings_.save(settingsPath_);
    RECT rc;
    GetClientRect(hwnd(), &rc);
    if (settings_.previewPane && (rc.right - rc.left) / gfx_.dpiScale() < kSidebarW + 760)
        flash(L"The preview pane shows when the window is wider.", 5);
    invalidate();
}

void FilesWindow::drawPreview(const Rect& r) {
    gfx_.fill(r, pal_.surface.mix(pal_.paper, 0.35f));
    gfx_.fill({r.x, r.y, 1, r.h}, pal_.rule);
    const float pad = 20;
    const float x = r.x + pad, w = r.w - pad * 2;
    float y = r.y + 18;
    const auto rows = here_.thisPC ? std::vector<std::size_t>{} : sel_.indices();
    auto line = [&](const std::wstring& label, const std::wstring& value) {
        if (value.empty()) return;
        gfx_.text(label, {x, y, 84, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        const float h = gfx_.text(value, {x + 88, y, w - 88, 60}, Font::Small, pal_.ink, {Align::Left, true});
        y += std::max(20.0f, h) + 6;
    };
    if (rows.empty()) {
        previewKey_.clear();
        gfx_.text(nameFor(here_), {x, y, w, 26}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
        y += 36;
        gfx_.text(L"Select a file to preview it here.", {x, y, w, 40}, Font::Small, pal_.inkMuted, {Align::Left, true});
        return;
    }
    if (rows.size() > 1) {
        previewKey_.clear();
        std::uint64_t bytes = 0;
        std::size_t folders = 0;
        for (std::size_t i : rows) {
            if (shown_[i].folder()) ++folders;
            else bytes += shown_[i].size;
        }
        gfx_.text(std::to_wstring(rows.size()) + L" items selected", {x, y, w, 26}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
        y += 40;
        if (folders) line(L"Folders", std::to_wstring(folders));
        if (rows.size() > folders) line(L"Files", std::to_wstring(rows.size() - folders) + L" (" + formatSize(bytes) + L")");
        return;
    }
    const std::size_t row = rows[0];
    const Entry& e = shown_[row];
    const std::wstring path = rowPath(row);
    const std::wstring key = path + L"|" + std::to_wstring(e.modified) + L"|" + std::to_wstring(e.size);

    // The picture, the start of a text file, or the big icon.
    const float boxH = std::min(r.h * 0.5f, w * 0.85f + 20);
    const Rect box{x, y, w, boxH};
    bool drawn = false, picture = false;
    const bool canPicture = thumbs_ && !e.folder() && !e.cloudOnly() && Thumbnails::hasPicture(e.name);
    const Thumb* th = nullptr;
    const int previewPx = w * gfx_.dpiScale() <= 256 ? 256 : 512;
    if (canPicture) {
        th = thumbs_->get(path, e.modified, e.size, previewPx);
        if (th && th->w > 0 && th->h > 0) {
            const float ds = gfx_.dpiScale();
            const float s = std::min({box.w / th->w, box.h / th->h, 2.0f / ds});
            const float pw = std::max(1.0f, th->w * s), ph = std::max(1.0f, th->h * s);
            const Rect p{std::round(box.x + (box.w - pw) / 2), std::round(box.y + (box.h - ph) / 2), std::round(pw), std::round(ph)};
            gfx_.drawPixels(L"thumb:" + std::to_wstring(th->serial), th->w, th->h, th->rgba.data(), p, true);
            gfx_.stroke({p.x - 0.5f, p.y - 0.5f, p.w + 1, p.h + 1}, pal_.rule, 1);
            drawn = picture = true;
        }
    }
    const bool textual = !picture && !e.folder() && !e.cloudOnly() && isTextName(e.name) && e.size > 0;
    if (textual) {
        if (key != previewKey_) {
            previewKey_ = key;
            previewState_ = PreviewLoading;
            previewText_.clear();
            HWND win = hwnd();
            std::thread([win, key, path] {
                auto* t = new TextPreview;
                t->key = key;
                HANDLE h = CreateFileW(extendedPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                std::vector<unsigned char> buf(48 * 1024);
                DWORD got = 0;
                if (h != INVALID_HANDLE_VALUE) {
                    if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr)) got = 0;
                    CloseHandle(h);
                }
                if (got == 0 || !art::looksLikeText(buf.data(), got)) t->notText = true;
                else t->text = art::decodeText(buf.data(), got, 120, &t->truncated);
                if (got == buf.size()) t->truncated = true;
                if (!PostMessageW(win, WM_APP_PREVIEW, 0, reinterpret_cast<LPARAM>(t))) delete t;
            }).detach();
        }
        if (previewState_ == PreviewText) {
            gfx_.fillRounded(box, 8, pal_.paper);
            gfx_.strokeRounded(box, 8, pal_.rule);
            const Rect tr{box.x + 12, box.y + 10, box.w - 24, box.h - 20};
            gfx_.pushClip(tr);
            gfx_.text(previewText_.substr(0, 6000), tr, Font::Small, pal_.ink, {Align::Left, true});
            gfx_.popClip();
            drawn = true;
        }
    } else if (!picture) {
        previewKey_.clear();
    }
    if (!drawn) {
        drawIcon(e.icon, {box.x + (box.w - 64) / 2, box.y + (box.h - 64) / 2 - 10, 64, 64});
        const bool waiting = (canPicture && !th && !thumbs_->failed(path, e.modified, e.size, previewPx)) || (textual && previewState_ == PreviewLoading);
        if (waiting)
            gfx_.text(L"Making a preview…", {box.x, box.y + box.h / 2 + 36, box.w, 20}, Font::Small, pal_.inkFaint, {Align::Center, false});
        else if (canPicture)
            gfx_.text(e.name.size() > 4 && Thumbnails::kindOf(e.name) == Thumbnails::PictureKind::Shell
                          ? L"Windows has no picture for this file."
                          : L"There's no picture in this file to show.",
                      {box.x, box.y + box.h / 2 + 36, box.w, 40}, Font::Small, pal_.inkFaint, {Align::Center, true});
    }
    y += boxH + 18;

    y += gfx_.text(e.name, {x, y, w, 60}, Font::Heading, pal_.ink, {Align::Left, true}) + 12;
    line(L"Type", e.type);
    if (!e.folder()) line(L"Size", formatSize(e.size));
    line(L"Modified", dateText(e.modified));
    if (here_.bin) line(L"Deleted from", e.location);
    else if (here_.searching()) line(L"Folder", e.location.empty() ? here_.path : e.location);
    if (e.cloudOnly()) line(L"Online only", L"Opening it downloads it.");
    if (e.icon < 0 && e.attributes == 0 && !here_.collection.empty()) line(L"Missing", L"It isn't there any more (moved, renamed or deleted outside NorthRay Files).");
    y += 4;
    y = drawPreviewLabels(x, y, w, path);
    if (textual && previewState_ == PreviewText && previewTruncated_) line(L"", L"Only the start is shown.");
    y += 8;
    if (here_.bin) {
        button("tb:preview:restore", {x, y, 110, 34}, L"Restore", [this] { binCommand(true); }, true);
        return;
    }
    button("tb:preview:open", {x, y, 96, 34}, L"Open", [this, row] { open(row); }, true);
    if (here_.searching())
        button("tb:preview:where", {x + 106, y, 150, 34}, L"Open its folder", [this, row] {
            if (row >= shown_.size()) return;
            const std::wstring at = shown_[row].location, nm = shown_[row].name;
            if (!at.empty()) go({false, at}, true, nm);
        });
}

// ---------------------------------------------------------------- labels, tags and collections (0.5)

void FilesWindow::loadLabels() {
    labelsPath_ = join(parentOf(settingsPath_), L"labels.json");
    HANDLE h = CreateFileW(labelsPath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string text((std::size_t)std::min<LONGLONG>(size.QuadPart, 64ll << 20), '\0');
    DWORD got = 0;
    if (!text.empty() && ReadFile(h, text.data(), (DWORD)text.size(), &got, nullptr)) text.resize(got);
    CloseHandle(h);
    labels_.fromJson(text);
}

void FilesWindow::saveLabels() {
    if (labelsPath_.empty()) return;
    const std::string text = labels_.toJson();
    const std::wstring tmp = labelsPath_ + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { flash(L"Labels couldn't be saved (the settings folder can't be written to).", 6); return; }
    DWORD put = 0;
    const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr) && put == text.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), labelsPath_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        flash(L"Labels couldn't be saved.", 6);
    }
}

static bool exists(const std::wstring& p) { return GetFileAttributesW(extendedPath(p).c_str()) != INVALID_FILE_ATTRIBUTES; }

void FilesWindow::noteMoved(const std::vector<std::wstring>& sources, const std::wstring& dest) {
    bool any = false;
    for (const auto& src : sources) {
        const std::wstring dst = join(dest, nameOf(src));
        if (!exists(src) && exists(dst)) {
            labels_.moved(src, dst);
            any = true;
        }
    }
    if (any) saveLabels();
}

void FilesWindow::noteRemoved(const std::vector<std::wstring>& paths) {
    bool any = false;
    for (const auto& p : paths)
        if (!exists(p)) {
            labels_.removed(p);
            any = true;
        }
    if (any) saveLabels();
}

void FilesWindow::setColourOnSelection(labels::Colour c) {
    const auto paths = selectedPaths();
    for (const auto& p : paths) labels_.setColour(p, c);
    saveLabels();
    invalidate();
}

void FilesWindow::toggleTagOnSelection(const std::wstring& tag) {
    const auto paths = selectedPaths();
    const bool all = std::all_of(paths.begin(), paths.end(), [&](const std::wstring& p) { return labels_.hasTag(p, tag); });
    for (const auto& p : paths) {
        if (all) labels_.removeTag(p, tag);
        else labels_.addTag(p, tag);
    }
    saveLabels();
    invalidate();
}

void FilesWindow::newTagForSelection() {
    const auto paths = selectedPaths();
    if (paths.empty()) return;
    askText(paths.size() == 1 ? L"A tag for \u201C" + nameOf(paths[0]) + L"\u201D" : L"A tag for " + std::to_wstring(paths.size()) + L" items", L"",
            [this, paths](const std::wstring& text) {
                const std::wstring tag = labels::normaliseTag(text);
                if (tag.empty()) return;
                for (const auto& p : paths) labels_.addTag(p, tag);
                saveLabels();
                invalidate();
            });
}

void FilesWindow::addSelectionToCollection(const std::string& id) {
    const auto paths = selectedPaths();
    const labels::Collection* c = labels_.collection(id);
    if (!c || paths.empty()) return;
    const std::wstring name = c->name;
    const int added = labels_.addToCollection(id, paths);
    saveLabels();
    flash(added ? L"Added to \u201C" + name + L"\u201D. They stay where they are." : L"They're in \u201C" + name + L"\u201D already.", 5);
    invalidate();
}

void FilesWindow::newCollectionFromSelection(bool empty) {
    const auto paths = empty ? std::vector<std::wstring>{} : selectedPaths();
    askText(L"Name the new collection", L"", [this, paths, empty](const std::wstring& text) {
        if (labels::normaliseTag(text).empty()) return;
        const std::string id = labels_.createCollection(text);
        labels_.addToCollection(id, paths);
        saveLabels();
        if (empty) openCollection(id);
        else flash(L"Made the collection \u201C" + labels_.collection(id)->name + L"\u201D. The files stay where they are.", 5);
        invalidate();
    });
}

void FilesWindow::openCollection(const std::string& id, bool inNewTab) {
    Location c;
    c.collection = id;
    if (inNewTab) newTab(c);
    else go(c);
}

void FilesWindow::collectionMenu(const std::string& id, POINT screen) {
    const labels::Collection* c = labels_.collection(id);
    if (!c) return;
    const std::vector<MenuItem> items = {MenuItem::item(L"Open", CmdCollOpen), MenuItem::item(L"Open in a new tab", CmdCollTab), MenuItem::sep(),
                                         MenuItem::item(L"Rename\u2026", CmdCollRename), MenuItem::item(L"Delete the collection", CmdCollDelete)};
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (cmd == CmdCollOpen) openCollection(id);
    else if (cmd == CmdCollTab) openCollection(id, true);
    else if (cmd == CmdCollRename) {
        askText(L"Rename the collection", c->name, [this, id](const std::wstring& text) {
            if (labels_.renameCollection(id, text)) saveLabels();
            invalidate();
        });
    } else if (cmd == CmdCollDelete) {
        const std::wstring q = L"Delete the collection \u201C" + c->name + L"\u201D?\n\nOnly the collection goes: the files and folders in it stay where they are.";
        if (cjui::messageBox(hwnd(), q, L"NorthRay Files", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
        labels_.deleteCollection(id);
        saveLabels();
        // Tabs showing it go to This PC.
        if (here_.collection == id) go({true, L""}, false);
        for (auto& pane : tabs_)
            for (auto& v : pane)
                if (v.here.collection == id) v.here = Location{true, L""};
    }
    invalidate();
}

void FilesWindow::openLabelSearch(const std::wstring& query) {
    std::wstring root = here_.path;
    if (here_.thisPC || !here_.collection.empty() || root.empty()) {
        root = L"C:\\";
        for (const Drive& d : drives_) if (d.ready) { root = d.root; break; }
    }
    go({false, root, query, true});
}

void FilesWindow::tagsMenu() {
    tagChoices_ = labels_.tags();
    if (tagChoices_.empty()) {
        flash(L"No tags yet. Right-click a file and choose Tags to give it one.", 6);
        return;
    }
    std::vector<MenuItem> items;
    for (std::size_t t = 0; t < tagChoices_.size() && t < 900; ++t)
        items.push_back(MenuItem::item(tagChoices_[t].first, CmdTag0 + (UINT)t, std::to_wstring(tagChoices_[t].second)));
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (cmd >= CmdTag0 && cmd - CmdTag0 < tagChoices_.size()) {
        const std::wstring& tag = tagChoices_[cmd - CmdTag0].first;
        openLabelSearch(tag.find(L' ') != std::wstring::npos ? L"tag:\"" + tag + L"\"" : L"tag:" + tag);
    }
}

float FilesWindow::drawPreviewLabels(float x, float y, float w, const std::wstring& path) {
    const labels::Label* lab = labels_.find(path);
    const labels::Colour cur = lab ? lab->colour : labels::Colour::None;
    // Colour: a swatch for each, the one it has ringed.
    gfx_.text(L"Colour", {x, y, 84, 24}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
    float sx = x + 88;
    if (x + 88 + labels::kColours * 26 > x + w) { y += 24; sx = x - 2; }   // a narrow pane: the swatches go under
    for (int c = 0; c < labels::kColours; ++c) {
        const Rect sw{sx, y, 24, 24};
        const std::string id = "tb:pv:colour:" + std::to_string(c);
        addHit(id, sw, [this, c] { setColourOnSelection((labels::Colour)c); });
        const float hv = hoverAmount(id);
        const float cx = sw.x + 12, cy = sw.y + 12;
        if ((int)cur == c) gfx_.circle(cx, cy, 10.5f, pal_.ink.withAlpha(0.7f), false, 1.5f);
        else if (hv > 0.01f) gfx_.circle(cx, cy, 10.5f, pal_.ink.withAlpha(0.25f * hv), false, 1.2f);
        if (c == 0) {
            gfx_.circle(cx, cy, 6.5f, pal_.inkFaint, false, 1.3f);
            gfx_.line(cx - 4.5f, cy + 4.5f, cx + 4.5f, cy - 4.5f, pal_.inkFaint, 1.3f);
        } else {
            gfx_.circle(cx, cy, 7, labelColour((labels::Colour)c), true);
        }
        sx += 26;
        if (sx + 24 > x + w) break;
    }
    y += 32;
    // Tags: a chip for each (x takes it off), and one to add more.
    gfx_.text(L"Tags", {x, y, 84, 24}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
    float tx = x + 88;
    auto chip = [&](const std::string& id, const std::wstring& text, bool removable, std::function<void()> act) {
        const float cw = std::min(w - 88, gfx_.measureWidth(text, Font::Small) + (removable ? 34 : 20));
        if (tx + cw > x + w && tx > x + 88) { tx = x + 88; y += 28; }
        const Rect c{tx, y, cw, 24};
        addHit(id, c, std::move(act));
        const float hv = hoverAmount(id);
        gfx_.fillRounded(c, 12, removable ? pal_.ink.withAlpha((pal_.dark ? 0.10f : 0.07f) + 0.05f * hv) : pal_.ribbon.withAlpha(0.10f + 0.10f * hv));
        gfx_.text(text, {c.x + 10, c.y, c.w - (removable ? 30 : 20), c.h}, Font::Small, removable ? pal_.ink : pal_.ribbon, {Align::Left, false, true, true});
        if (removable) {
            const float ex = c.right() - 14, ey = c.y + 12;
            gfx_.line(ex - 3, ey - 3, ex + 3, ey + 3, pal_.inkMuted, 1.2f);
            gfx_.line(ex + 3, ey - 3, ex - 3, ey + 3, pal_.inkMuted, 1.2f);
        }
        if (focused(id)) focusRing(c);
        tx += cw + 6;
    };
    if (lab)
        for (std::size_t k = 0; k < lab->tags.size(); ++k) {
            const std::wstring tag = lab->tags[k];
            chip("tb:pv:tag:" + std::to_string(k), tag, true, [this, path, tag] {
                labels_.removeTag(path, tag);
                saveLabels();
                invalidate();
            });
        }
    chip("tb:pv:tag:add", L"+ Tag", false, [this] {
        // The tags in use (to add one again), and a new one.
        tagChoices_ = labels_.tags();
        std::vector<MenuItem> items;
        const auto paths = selectedPaths();
        for (std::size_t t = 0; t < tagChoices_.size() && t < 900; ++t) {
            const bool all = std::all_of(paths.begin(), paths.end(), [&](const std::wstring& p) { return labels_.hasTag(p, tagChoices_[t].first); });
            items.push_back(MenuItem::item(tagChoices_[t].first, CmdTag0 + (UINT)t, {}, true, all));
        }
        if (!items.empty()) items.push_back(MenuItem::sep());
        items.push_back(MenuItem::item(L"New tag\u2026", CmdNewTag));
        POINT pt;
        GetCursorPos(&pt);
        const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
        if (cmd == CmdNewTag) newTagForSelection();
        else if (cmd >= CmdTag0 && cmd - CmdTag0 < tagChoices_.size()) toggleTagOnSelection(tagChoices_[cmd - CmdTag0].first);
    });
    y += 34;
    // The collections it's in.
    std::wstring in;
    for (const auto& id : labels_.collectionsWith(path))
        if (const labels::Collection* c = labels_.collection(id)) in += (in.empty() ? L"" : L", ") + c->name;
    if (!in.empty()) {
        gfx_.text(L"In", {x, y, 84, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        y += std::max(20.0f, gfx_.text(in, {x + 88, y, w - 88, 60}, Font::Small, pal_.ink, {Align::Left, true})) + 6;
    }
    return y;
}

// Gallery view: big pictures filling square tiles (cropped to fit), the name over the foot of the
// picture when it's pointed at or selected; folders and other files as a card with their icon and name.
void FilesWindow::drawGalleryTile(const Rect& t, std::size_t i, int px, float fade) {
    const Entry& e = shown_[i];
    const Rect box{t.x + 4, t.y + 4, t.w - 8, t.h - 8};
    const bool sel = sel_.selected(i);
    const bool hov = hoverId() == idPrefix_ + "row:" + std::to_string(i);
    if (renaming_ && (int)i == renameRow_) renameRect_ = {box.x + 4, box.bottom() - 34, box.w - 8, 28};
    const Thumb* th = nullptr;
    if (thumbs_ && !e.folder() && !e.cloudOnly() && Thumbnails::hasPicture(e.name)) th = thumbs_->get(rowPath(i), e.modified, e.size, px);
    bool picture = false;
    if (th && th->w > 0 && th->h > 0) {
        const float s = std::max(box.w / th->w, box.h / th->h);
        const float w = th->w * s, h = th->h * s;
        gfx_.pushClip(box);
        gfx_.drawPixels(L"thumb:" + std::to_wstring(th->serial), th->w, th->h, th->rgba.data(),
                        {std::round(box.x + (box.w - w) / 2), std::round(box.y + (box.h - h) / 2), std::round(w), std::round(h)}, true);
        if (fade < 1) gfx_.fill(box, pal_.paper.withAlpha(1.0f - fade));
        gfx_.popClip();
        picture = true;
    } else {
        gfx_.fillRounded(box, 8, pal_.sunken);
        drawIcon(e.icon, {box.x + (box.w - 48) / 2, box.y + (box.h - 48) / 2 - 14, 48, 48}, fade);
    }
    if (!picture || hov || sel) {
        const Rect band{box.x, box.bottom() - 32, box.w, 32};
        if (picture) gfx_.fill(band, cjui::hexColor("#000000").withAlpha(0.55f));
        gfx_.text(e.name, {band.x + 10, band.y, band.w - 20, band.h}, Font::Small, picture ? cjui::hexColor("#FFFFFF") : pal_.ink,
                  {picture ? Align::Left : Align::Center, false, true, true});
    }
    if (sel) gfx_.strokeRounded({box.x - 2, box.y - 2, box.w + 4, box.h + 4}, 8, pal_.ribbon, 2.5f);
    else if (hov) gfx_.strokeRounded({box.x - 1, box.y - 1, box.w + 2, box.h + 2}, 8, pal_.ink.withAlpha(0.25f), 1);
    if ((int)i == sel_.focus() && GetFocus() == hwnd() && !sel) gfx_.strokeRounded(box, 8, pal_.ribbon.withAlpha(0.7f), 1);
}

// ---------------------------------------------------------------- asking for a name

void FilesWindow::askText(const std::wstring& title, const std::wstring& initial, std::function<void(const std::wstring&)> done) {
    if (renaming_) endRename(false);
    askTitle_ = title;
    askDone_ = std::move(done);
    asking_ = true;
    SetWindowTextW(ask_, initial.c_str());
    invalidate();
    UpdateWindow(hwnd());   // places the field
    ShowWindow(ask_, SW_SHOW);
    SetFocus(ask_);
    SendMessageW(ask_, EM_SETSEL, 0, -1);
}

void FilesWindow::endAsk(bool commit) {
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

void FilesWindow::drawAsk(float W, float H) {
    gfx_.fill({0, 0, W, H}, cjui::hexColor("#000000").withAlpha(pal_.dark ? 0.45f : 0.25f));
    const float cw = std::min(440.0f, W - 40), ch = 172;
    const Rect card{std::floor((W - cw) / 2), std::floor(std::max(80.0f, H * 0.3f)), cw, ch};
    askCard_ = card;
    softShadow(card, 12, 1);
    gfx_.fillRounded(card, 12, pal_.surface);
    gfx_.strokeRounded(card, 12, pal_.rule);
    gfx_.text(askTitle_, {card.x + 22, card.y + 18, card.w - 44, 26}, Font::Heading, pal_.ink, {Align::Left, false, true, true});
    const Rect field{card.x + 22, card.y + 58, card.w - 44, 38};
    gfx_.fillRounded(field, 8, pal_.sunken);
    gfx_.strokeRounded(field, 8, pal_.focus, 1.5f);
    const float s = gfx_.dpiScale(), th = 20;
    SetWindowPos(ask_, HWND_TOP, (int)std::lround((field.x + 12) * s), (int)std::lround((field.y + (field.h - th) / 2) * s),
                 (int)std::lround((field.w - 24) * s), (int)std::lround(th * s), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const float by = card.bottom() - 54;
    button("tb:ask:ok", {card.right() - 22 - 96, by, 96, 36}, L"OK", [this] { endAsk(true); }, true);
    button("tb:ask:cancel", {card.right() - 22 - 96 - 10 - 96, by, 96, 36}, L"Cancel", [this] { endAsk(false); });
}

// ---------------------------------------------------------------- searching

void FilesWindow::showSearchText() {
    if (!search_) return;
    const int n = GetWindowTextLengthW(search_);
    std::wstring cur((size_t)n, L'\0');
    if (n) GetWindowTextW(search_, cur.data(), n + 1);
    if (cur == here_.query) return;   // (setting it again would move the caret)
    settingSearch_ = true;
    SetWindowTextW(search_, here_.query.c_str());
    settingSearch_ = false;
    invalidate();
}

void FilesWindow::focusSearch() {
    if (searchRect_.w <= 0) { flash(L"Make the window a little wider to search.", 5); return; }
    if (editing_) endEditingAddress(false);
    ShowWindow(search_, SW_SHOW);
    SetFocus(search_);
    SendMessageW(search_, EM_SETSEL, 0, -1);
    invalidate();
}

void FilesWindow::searchTyped() {
    const int n = GetWindowTextLengthW(search_);
    std::wstring text((size_t)n, L'\0');
    if (n) GetWindowTextW(search_, text.data(), n + 1);
    std::wstring trimmed = text;
    while (!trimmed.empty() && iswspace(trimmed.back())) trimmed.pop_back();
    while (!trimmed.empty() && iswspace(trimmed.front())) trimmed.erase(0, 1);
    if (!here_.collection.empty() || here_.bin) {
        // In a collection (or the Recycle Bin) the box narrows what's in it.
        here_.query = search::parse(trimmed).empty() ? std::wstring() : trimmed;
        showHere();
        return;
    }
    if (search::parse(trimmed).empty()) {
        // Nothing to look for (yet): the folder, as it was.
        KillTimer(hwnd(), kSearchTimer);
        if (here_.searching()) endSearch();
        return;
    }
    if (!here_.searching()) {
        // The first letter: results start at once (the folder and its sub-folders; from This PC, the whole PC).
        if (here_.thisPC) {
            std::wstring root = L"C:\\";
            for (const Drive& d : drives_) if (d.ready) { root = d.root; break; }
            go({false, root, trimmed, true});
        } else {
            go({false, here_.path, trimmed, false});
        }
        return;
    }
    const std::wstring old = here_.query;
    here_.query = trimmed;
    windowTitle_ = nameFor(here_) + L" \u2013 NorthRay Files";
    SetWindowTextW(hwnd(), windowTitle_.c_str());
    // More letters on the end of plain words only narrow what's found: no need to look again.
    const bool narrows = trimmed.size() > old.size() && trimmed.compare(0, old.size(), old) == 0 &&
                         old.find_first_of(L":*?\".") == std::wstring::npos && trimmed.find_first_of(L":*?\".") == std::wstring::npos &&
                         !searchLimited_;
    showHere();
    if (!narrows) SetTimer(hwnd(), kSearchTimer, 220, nullptr);
}

void FilesWindow::runSearch(const std::wstring& text, bool everywhere) {
    if (text.empty() || here_.thisPC) return;
    go({false, here_.path, text, everywhere}, false);
}

void FilesWindow::endSearch() {
    KillTimer(hwnd(), kSearchTimer);
    if (!here_.collection.empty() || here_.bin) {
        here_.query.clear();
        showHere();
        showSearchText();
        return;
    }
    if (!here_.searching()) { showSearchText(); return; }
    const Location folder{false, here_.path};
    if (!backStack_.empty() && backStack_.back() == folder) {
        backStack_.pop_back();
        go(folder, false);
    } else {
        go(folder, true);
    }
    showSearchText();
}

std::wstring FilesWindow::searchFolderOf(std::size_t row) const {
    if (row >= shown_.size()) return L"";
    const std::wstring& at = shown_[row].location;
    if (at.empty()) return L"";
    if (here_.everywhere || !here_.collection.empty() || here_.bin) return at;
    const std::wstring rel = search::relativeFolder(here_.path, at);
    return rel.empty() ? (isDriveRoot(here_.path) ? here_.path : nameOf(here_.path)) : rel;
}

void FilesWindow::drawSearchBox(const Rect& r) {
    searchRect_ = r;
    const bool focusedBox = GetFocus() == search_;
    const bool hasText = GetWindowTextLengthW(search_) > 0;
    gfx_.fillRounded(r, 8, pal_.sunken);
    gfx_.strokeRounded(r, 8, focusedBox ? pal_.focus : pal_.rule, focusedBox ? 1.5f : 1);
    // A magnifying glass.
    const float cx = r.x + 17, cy = r.y + r.h / 2 - 1;
    gfx_.circle(cx, cy, 5.5f, pal_.inkMuted, false, 1.4f);
    gfx_.line(cx + 4, cy + 4, cx + 8, cy + 8, pal_.inkMuted, 1.6f);
    const float s = gfx_.dpiScale();
    const float th = 20;
    const float clearW = hasText ? 26 : 0;
    if (focusedBox || hasText) {
        SetWindowPos(search_, nullptr, (int)std::lround((r.x + 32) * s), (int)std::lround((r.y + (r.h - th) / 2) * s),
                     (int)std::lround((r.w - 40 - clearW) * s), (int)std::lround(th * s), SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(search_, SW_HIDE);
        const std::wstring where = here_.thisPC ? L"this PC" : (!here_.collection.empty() || here_.bin) ? nameFor(here_) : (isDriveRoot(here_.path) ? here_.path.substr(0, 2) : nameOf(here_.path));
        gfx_.text(L"Search " + where, {r.x + 32, r.y, r.w - 40, r.h}, Font::Body, pal_.inkFaint, {Align::Left, false, true, true});
        addHit("tb:search", r, [this] { focusSearch(); });
    }
    if (hasText) {
        const Rect xb{r.right() - 28, r.y + (r.h - 22) / 2, 22, 22};
        addHit("tb:search:clear", xb, [this] { endSearch(); });
        const float hv = hoverAmount("tb:search:clear");
        if (hv > 0.01f) gfx_.fillRounded(xb, 5, pal_.ink.withAlpha(0.08f * hv));
        const float xc = xb.x + 11, yc = xb.y + 11;
        gfx_.line(xc - 4, yc - 4, xc + 4, yc + 4, pal_.inkMuted, 1.3f);
        gfx_.line(xc + 4, yc - 4, xc - 4, yc + 4, pal_.inkMuted, 1.3f);
    }
}

float FilesWindow::drawSearchBar(const Rect& r) {
    const float h = 46;
    const Rect bar{r.x, r.y, r.w, h};
    gfx_.fill(bar, pal_.surface.mix(pal_.paper, 0.6f));
    gfx_.line(bar.x, bar.bottom() - 0.5f, bar.right(), bar.bottom() - 0.5f, pal_.rule);
    const std::wstring folder = isDriveRoot(here_.path) ? here_.path.substr(0, 2) : nameOf(here_.path);
    const std::size_t n = shown_.size();
    std::wstring found = n == 1 ? L"1 result" : std::to_wstring(n) + L" results";
    if (searchLimited_) found = L"The first " + std::to_wstring(n) + L" results";
    std::wstring where = here_.everywhere ? L" on this PC" : L" in \u201C" + folder + L"\u201D and the folders in it";
    std::wstring text = (loading_ ? L"Searching" + where + L"\u2026 " + found + L" so far" : found + where);
    if (here_.everywhere && searchNoIndex_)
        text += loading_ ? L" (Windows Search isn't available, so the drives are being read)" : L" (Windows Search isn't available, so the drives were read)";
    else if (here_.everywhere && !loading_ && !search::parse(here_.query).byLabel())
        text += L", from Windows Search (places it indexes)";
    // The other place to look.
    const std::wstring other = here_.everywhere ? L"Only in \u201C" + folder + L"\u201D" : L"Search the whole PC";
    const float bw = std::min(gfx_.measureWidth(other, Font::Small) + 28, 240.0f);
    const Rect b{bar.right() - bw - 14, bar.y + 8, bw, h - 16};
    button(idPrefix_ + "tb:search:scope", b, other, [this] { runSearch(here_.query, !here_.everywhere); });
    if (loading_) {
        const double secs = (double)GetTickCount64() / 1000.0;
        const float a0 = (float)std::fmod(secs * 6.0, 6.2831853);
        gfx_.circle(bar.x + 26, bar.y + h / 2, 7, pal_.inkFaint.withAlpha(0.3f), false, 2);
        arcLines(gfx_, bar.x + 26, bar.y + h / 2, 7, a0, a0 + 1.9f, pal_.ribbon, 2);
    }
    gfx_.text(text, {bar.x + (loading_ ? 42 : 18), bar.y, b.x - bar.x - 30, h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
    return h;
}

// ---------------------------------------------------------------- tabs and panes

void FilesWindow::swapView(View& v) {
    using std::swap;
    swap(here_, v.here);
    swap(backStack_, v.backStack);
    swap(forwardStack_, v.forwardStack);
    swap(listed_, v.listed);
    swap(shown_, v.shown);
    swap(sel_, v.sel);
    swap(selectAfterLoad_, v.selectAfterLoad);
    swap(loading_, v.loading);
    swap(loadedOnce_, v.loadedOnce);
    swap(partial_, v.partial);
    swap(quiet_, v.quiet);
    swap(error_, v.error);
    swap(errorCode_, v.errorCode);
    swap(request_, v.request);
    swap(latest_, v.latest);
    swap(loadStarted_, v.loadStarted);
    swap(pending_, v.pending);
    swap(selectNames_, v.selectNames);
    swap(listClip_, v.listClip);
    swap(listTop_, v.listTop);
    swap(tileW_, v.tileW);
    swap(listX_, v.listX);
    swap(columns_, v.columns);
    swap(searchLimited_, v.searchLimited);
    swap(searchNoIndex_, v.searchNoIndex);
    swap(searchReplace_, v.searchReplace);
    swap(searchFolders_, v.searchFolders);
    const ScrollState mine = scrollState();
    setScrollState(v.scroll);
    v.scroll = mine;
    // Swapped in from outside (anything but the active tab): don't touch the window's title.
    swappedIn_ += (&v == &tabs_[pane_][(size_t)activeTab_[pane_]]) ? 0 : 1;
    if (swappedIn_ > 1) swappedIn_ -= 2;   // the second swap of a pair
}

FilesWindow::View* FilesWindow::viewForRequest(std::uint64_t request) {
    for (int p = 0; p < 2; ++p)
        for (int t = 0; t < (int)tabs_[p].size(); ++t) {
            if (p == pane_ && t == activeTab_[p]) continue;   // the active tab's slot holds nothing (it's in the window)
            if (tabs_[p][(size_t)t].request == request) return &tabs_[p][(size_t)t];
        }
    return nullptr;
}

const FilesWindow::Location& FilesWindow::tabLocation(int pane, int tab) const {
    // The tab whose view is in the window's fields: the active one, or the other pane's while it's drawn.
    if (tab == activeTab_[pane] && (swappedPane_ >= 0 ? pane == swappedPane_ : pane == pane_)) return here_;
    return tabs_[pane][(size_t)tab].here;
}

void FilesWindow::activate(int pane, int tab) {
    if (pane < 0 || pane > 1 || tab < 0 || tab >= (int)tabs_[pane].size()) return;
    if (pane == pane_ && tab == activeTab_[pane]) return;
    if (renaming_) endRename(true);
    if (editing_) endEditingAddress(false);
    View& from = tabs_[pane_][(size_t)activeTab_[pane_]];
    swappedIn_ = 0;
    {
        // The active tab's state goes into its slot...
        using std::swap;
        View tmp;
        swap(tmp, from);   // (the slot's leftovers, unused)
        swapView(from);    // window fields -> slot
        swappedIn_ = 0;
    }
    pane_ = pane;
    activeTab_[pane] = tab;
    swapView(tabs_[pane][(size_t)tab]);   // ...and the chosen one comes out of its slot
    swappedIn_ = 0;
    windowTitle_ = nameFor(here_) + L" – NorthRay Files";
    showSearchText();
    SetWindowTextW(hwnd(), windowTitle_.c_str());
    invalidate();
}

void FilesWindow::newTab(Location at) {
    tabs_[pane_].push_back(View{});
    activate(pane_, (int)tabs_[pane_].size() - 1);
    go(at, false);
}

void FilesWindow::closeTab(int pane, int tab) {
    if (pane < 0 || pane > 1 || tab < 0 || tab >= (int)tabs_[pane].size()) return;
    if (tabs_[pane].size() == 1) {
        if (pane == 1 || twoPanes_) { setTwoPanes(false); return; }   // the second pane's last tab closes the pane
        PostMessageW(hwnd(), WM_CLOSE, 0, 0);                         // the last tab: the window closes
        return;
    }
    const bool active = pane == pane_ && tab == activeTab_[pane];
    if (active) {
        // Show a neighbour first, then let the closed one go.
        const int next = tab + 1 < (int)tabs_[pane].size() ? tab + 1 : tab - 1;
        activate(pane, next);
    }
    View& v = tabs_[pane][(size_t)tab];
    if (v.latest) v.latest->store(~0ull);   // its workers give up
    tabs_[pane].erase(tabs_[pane].begin() + tab);
    if (activeTab_[pane] > tab) --activeTab_[pane];
    invalidate();
}

void FilesWindow::setTwoPanes(bool on) {
    if (on == twoPanes_) return;
    if (on) {
        twoPanes_ = true;
        if (tabs_[1].empty()) {
            const Location here = here_;
            tabs_[1].push_back(View{});
            activate(1, 0);
            go(here, false);   // the second pane starts where the first one is
        } else {
            activate(1, activeTab_[1]);
        }
    } else {
        if (pane_ == 1) activate(0, activeTab_[0]);
        for (auto& v : tabs_[1]) if (v.latest) v.latest->store(~0ull);
        tabs_[1].clear();
        activeTab_[1] = 0;
        twoPanes_ = false;
    }
    settings_.twoPanes = twoPanes_;
    settings_.save(settingsPath_);
    invalidate();
}

int FilesWindow::paneAt(float x, float y) const {
    if (!twoPanes_ || tabs_[1].empty()) return paneRect_[0].contains(x, y) ? 0 : -1;
    for (int p = 0; p < 2; ++p) if (paneRect_[p].contains(x, y)) return p;
    return -1;
}

void FilesWindow::transferToOtherPane(bool move) {
    if (!twoPanes_) return;
    const int other = 1 - pane_;
    const Location& to = tabLocation(other, activeTab_[other]);
    if (to.thisPC) { flash(L"Open a folder in the other pane first.", 5); return; }
    const auto paths = selectedPaths();
    if (paths.empty()) return;
    const TransferPlan plan = planTransfer(paths, to.path, move);
    if (plan.nothingToDo) { flash(L"They're in that folder already.", 5); return; }
    if (!plan.problem.empty()) flash(plan.problem, 6);
    if (plan.sources.empty()) return;
    const ops::Result r = ops::transfer(hwnd(), plan.sources, to.path, move);
    if (move) noteMoved(plan.sources, to.path);
    if (!r.ok && !r.cancelled) cjui::messageBox(hwnd(), (L"Some items couldn't be " + std::wstring(move ? L"moved" : L"copied") + L".\n\n" + r.message).c_str(),
                                                L"NorthRay Files", MB_OK | MB_ICONWARNING);
    else if (r.ok) flash((move ? L"Moved to “" : L"Copied to “") + (isDriveRoot(to.path) ? to.path : nameOf(to.path)) + L"”.", 5);
    if (move) reloadQuietly();
}

// The tabs along the top of a pane: each folder's name, a close button on the hovered or active one,
// and "+" for a new tab.
void FilesWindow::drawTabs(const Rect& r, int pane) {
    gfx_.fill(r, pal_.surface.mix(pal_.paper, 0.5f));
    gfx_.line(r.x, r.bottom() - 0.5f, r.right(), r.bottom() - 0.5f, pal_.rule);
    const int n = (int)tabs_[pane].size();
    const float plusW = 34;
    const float tw = n ? std::clamp((r.w - 16 - plusW) / (float)n, 60.0f, 220.0f) : 0;
    float x = r.x + 8;
    for (int t = 0; t < n; ++t) {
        const Location& l = tabLocation(pane, t);
        const std::wstring name = nameFor(l);
        const Rect tr{x, r.y + 4, tw - 4, r.h - 4};
        const std::string id = "tab:" + std::to_string(pane) + ":" + std::to_string(t);
        addHit(id, {tr.x, tr.y, tr.w - 24, tr.h}, [this, pane, t] { activate(pane, t); });
        const bool on = t == activeTab_[pane];
        const float hv = hoverAmount(id);
        if (on) {
            gfx_.fillRounded({tr.x, tr.y, tr.w, tr.h + 6}, 7, pal_.paper);
            gfx_.strokeRounded({tr.x, tr.y, tr.w, tr.h + 6}, 7, pal_.rule);
            gfx_.fill({tr.x + 1, tr.bottom() - 1, tr.w - 2, 6}, pal_.paper);
        } else if (hv > 0.01f) {
            gfx_.fillRounded(tr, 7, pal_.ink.withAlpha(0.05f * hv));
        }
        gfx_.text(name, {tr.x + 12, tr.y, tr.w - 38, tr.h}, on ? Font::SmallStrong : Font::Small, on ? pal_.ink : pal_.inkMuted, {Align::Left, false, true, true});
        if (n > 1 || pane == 1) {
            const Rect xb{tr.right() - 24, tr.y + (tr.h - 18) / 2, 18, 18};
            const std::string xid = "tabx:" + std::to_string(pane) + ":" + std::to_string(t);
            addHit(xid, xb, [this, pane, t] { closeTab(pane, t); }, false);
            const float xh = hoverAmount(xid);
            if (on || hv > 0.01f || xh > 0.01f) {
                if (xh > 0.01f) gfx_.fillRounded(xb, 4, pal_.ink.withAlpha(0.08f * xh));
                const float cx = xb.x + 9, cy = xb.y + 9;
                gfx_.line(cx - 3.5f, cy - 3.5f, cx + 3.5f, cy + 3.5f, pal_.inkMuted, 1.2f);
                gfx_.line(cx + 3.5f, cy - 3.5f, cx - 3.5f, cy + 3.5f, pal_.inkMuted, 1.2f);
            }
        }
        if (focused(id)) focusRing(tr);
        x += tw;
    }
    const Rect pb{x + 2, r.y + 6, 28, r.h - 10};
    const std::string pid = "tab+:" + std::to_string(pane);
    addHit(pid, pb, [this, pane] {
        if (pane != pane_) activate(pane, activeTab_[pane]);
        newTab(here_);
    });
    const float ph = hoverAmount(pid);
    if (ph > 0.01f) gfx_.fillRounded(pb, 6, pal_.ink.withAlpha(0.06f * ph));
    gfx_.line(pb.x + 14, pb.y + pb.h / 2 - 5, pb.x + 14, pb.y + pb.h / 2 + 5, pal_.inkMuted, 1.4f);
    gfx_.line(pb.x + 9, pb.y + pb.h / 2, pb.x + 19, pb.y + pb.h / 2, pal_.inkMuted, 1.4f);
    if (focused(pid)) focusRing(pb);
}

// ---------------------------------------------------------------- pinned folders and dragging

void FilesWindow::pinFolder(const std::wstring& path) {
    for (auto& p : settings_.pinned) if (samePath(p, path)) { flash(L"It's pinned already.", 4); return; }
    settings_.pinned.push_back(path);
    settings_.save(settingsPath_);
    flash(L"Pinned “" + (isDriveRoot(path) ? path : nameOf(path)) + L"” to the sidebar.", 4);
    invalidate();
}

void FilesWindow::unpinFolder(const std::wstring& path) {
    auto& v = settings_.pinned;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const std::wstring& p) { return samePath(p, path); }), v.end());
    settings_.save(settingsPath_);
    invalidate();
}

void FilesWindow::sidebarMenu(const std::wstring& path, POINT screen) {
    const std::vector<MenuItem> items = {MenuItem::item(L"Open", CmdPinnedOpen), MenuItem::item(L"Open in a new tab", CmdPinnedTab), MenuItem::sep(),
                                         MenuItem::item(L"Unpin from the sidebar", CmdUnpin)};
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (cmd == CmdPinnedOpen) goPath(path);
    else if (cmd == CmdPinnedTab) newTab({false, path});
    else if (cmd == CmdUnpin) unpinFolder(path);
    invalidate();
}

std::wstring FilesWindow::dropTargetFolder(float x, float y, int* paneOut) {
    const int p = paneAt(x, y);
    if (paneOut) *paneOut = p;
    if (p < 0) return {};
    const bool swapped = p != pane_;
    if (swapped) swapView(tabs_[p][(size_t)activeTab_[p]]);
    std::wstring out;
    if (!here_.thisPC && error_ == ListError::None) {
        long long row = -1;
        if (listClip_.contains(x, y)) {
            if (settings_.view == ViewMode::Details) {
                const long long r = (long long)std::floor((y - listTop_) / rowH_);
                if (r >= 0 && r < (long long)shown_.size()) row = r;
            } else {
                const long long line = (long long)std::floor((y - listTop_) / tileH_), col = (long long)std::floor((x - listX_) / tileW_);
                if (line >= 0 && col >= 0 && col < columns_ && line * columns_ + col < (long long)shown_.size()) row = line * columns_ + col;
            }
        }
        std::wstring rowFolder;
        if (row >= 0 && shown_[(size_t)row].folder()) rowFolder = rowPath((size_t)row);
        bool dragged = false;
        for (auto& d : drag_.paths) if (samePath(d, rowFolder)) dragged = true;
        if (!rowFolder.empty() && !dragged) out = rowFolder;
        else if (p != drag_.fromPane && !here_.searching() && here_.collection.empty() && !here_.bin) out = here_.path;   // the other pane's folder
    }
    if (swapped) swapView(tabs_[p][(size_t)activeTab_[p]]);
    return out;
}

void FilesWindow::onMouseMove(float x, float y, bool leftDown) {
    if (!leftDown) { if (drag_.armed && !drag_.active) drag_ = Drag{}; return; }
    if (!drag_.armed) return;
    drag_.x = x;
    drag_.y = y;
    if (!drag_.active && std::hypot(x - drag_.sx, y - drag_.sy) > 6 && !drag_.paths.empty()) drag_.active = true;
    if (drag_.active && !drag_.external) {
        // Out of the window: Windows takes the drag over, so any program that takes files can have them.
        RECT rc;
        GetClientRect(hwnd(), &rc);
        const float s = gfx_.dpiScale();
        if (x < 0 || y < 0 || x * s >= rc.right || y * s >= rc.bottom) {
            const auto paths = drag_.paths;
            drag_ = Drag{};
            invalidate();
            ReleaseCapture();
            const DWORD effect = dnd::dragOut(paths);
            if (effect & DROPEFFECT_MOVE) reloadQuietly();   // moved by the other program
            drag_ = Drag{};
            invalidate();
            return;
        }
    }
    if (drag_.active) invalidate();
}

bool FilesWindow::onMouseUp(float x, float y) {
    if (!drag_.armed) return false;
    Drag d = drag_;
    drag_ = Drag{};
    if (!d.active) {
        // A press on something already selected, without dragging: select just it.
        if (d.clickRow >= 0 && (size_t)d.clickRow < shown_.size()) sel_.click((size_t)d.clickRow, false, false);
        invalidate();
        return false;
    }
    invalidate();
    dropAt(d.paths, x, y, (GetKeyState(VK_CONTROL) & 0x8000) != 0, (GetKeyState(VK_SHIFT) & 0x8000) != 0, true);
    return true;
}

// Things dropped at (x, y), from inside Files or from another program: onto a collection (added), the
// sidebar (folders pinned), a folder row or a pane's folder (moved or copied as File Explorer would).
// Returns what was done (DROPEFFECT_...); with `doIt` false, only what would be.
DWORD FilesWindow::dropAt(const std::vector<std::wstring>& paths, float x, float y, bool ctrl, bool shift, bool doIt) {
    for (auto& [id, rr] : collectionRects_)
        if (rr.contains(x, y)) {
            if (!doIt) return DROPEFFECT_LINK;
            const int added = labels_.addToCollection(id, paths);
            saveLabels();
            const labels::Collection* c = labels_.collection(id);
            flash(added ? L"Added to \u201C" + (c ? c->name : std::wstring()) + L"\u201D. They stay where they are." : L"They're in that collection already.", 5);
            invalidate();
            return DROPEFFECT_LINK;
        }
    if (sidebarRect_.contains(x, y)) {
        bool anyFolder = false;
        for (auto& p : paths) {
            const DWORD a = GetFileAttributesW(extendedPath(p).c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                anyFolder = true;
                if (doIt) pinFolder(p);
            }
        }
        if (doIt && !anyFolder) flash(L"Only folders can be pinned to the sidebar.", 5);
        return anyFolder ? DROPEFFECT_LINK : DROPEFFECT_NONE;
    }
    const Drag keep = drag_;
    drag_.paths = paths;   // dropTargetFolder looks at what's dragged
    int tp = -1;
    const std::wstring target = dropTargetFolder(x, y, &tp);
    drag_ = keep;
    if (target.empty()) return DROPEFFECT_NONE;
    // As in File Explorer: a move on the same drive, a copy to another (Ctrl copies, Shift moves).
    const bool sameDrive = !paths.empty() && paths[0].size() > 1 && target.size() > 1 && towupper(paths[0][0]) == towupper(target[0]) &&
                           paths[0][1] == L':' && target[1] == L':';
    const bool move = shift || (!ctrl && sameDrive);
    if (!doIt) return move ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    const TransferPlan plan = planTransfer(paths, target, move);
    if (plan.nothingToDo || plan.sources.empty()) { if (!plan.problem.empty()) flash(plan.problem, 6); return DROPEFFECT_NONE; }
    if (!plan.problem.empty()) flash(plan.problem, 6);
    const ops::Result r = ops::transfer(hwnd(), plan.sources, target, move);
    if (move) noteMoved(plan.sources, target);
    if (!r.ok && !r.cancelled) cjui::messageBox(hwnd(), (L"Some items couldn't be " + std::wstring(move ? L"moved" : L"copied") + L".\n\n" + r.message).c_str(),
                                                L"NorthRay Files", MB_OK | MB_ICONWARNING);
    else if (r.ok) flash((move ? L"Moved to \u201C" : L"Copied to \u201C") + (isDriveRoot(target) ? target : nameOf(target)) + L"\u201D.", 5);
    reloadQuietly();
    return r.ok ? (move ? DROPEFFECT_MOVE : DROPEFFECT_COPY) : DROPEFFECT_NONE;
}

bool FilesWindow::onWheel(float x, float y, float deltaLines) {
    if (sidebarRect_.contains(x, y) && !treeRect_.contains(x, y) && sideContent_ > sideViewH_) {
        sideScroll_ = std::clamp(sideScroll_ + deltaLines * 20.0f, 0.0f, std::max(0.0f, sideContent_ - sideViewH_));
        invalidate();
        return true;
    }
    if (treeRect_.contains(x, y)) {
        treeScroll_ = std::clamp(treeScroll_ + deltaLines * 20.0f, 0.0f, std::max(0.0f, treeContent_ - treeRect_.h));
        invalidate();
        return true;
    }
    const int p = paneAt(x, y);
    if (p < 0 || p == pane_) return false;   // the active pane scrolls as usual
    swapView(tabs_[p][(size_t)activeTab_[p]]);
    scrollBy(deltaLines * 20.0f);
    swapView(tabs_[p][(size_t)activeTab_[p]]);
    invalidate();
    return true;
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
        case IconPanes:   // two panes side by side
            gfx_.strokeRounded({cx - 8, cy - 6, 16, 12}, 2, c, 1.4f);
            gfx_.line(cx, cy - 6, cx, cy + 6, c, 1.4f);
            break;
        case IconGallery:   // big pictures: a mountain in a frame
            gfx_.strokeRounded({cx - 8, cy - 6, 16, 12}, 2, c, 1.4f);
            gfx_.line(cx - 6, cy + 4, cx - 2, cy - 1, c, 1.3f);
            gfx_.line(cx - 2, cy - 1, cx + 1, cy + 2, c, 1.3f);
            gfx_.line(cx + 1, cy + 2, cx + 3, cy, c, 1.3f);
            gfx_.line(cx + 3, cy, cx + 6, cy + 4, c, 1.3f);
            gfx_.circle(cx + 3, cy - 3, 1.3f, c, true);
            break;
        case IconPreview:   // a pane at the right with a picture in it
            gfx_.strokeRounded({cx - 8, cy - 6, 16, 12}, 2, c, 1.4f);
            gfx_.line(cx + 2, cy - 6, cx + 2, cy + 6, c, 1.4f);
            gfx_.fill({cx + 3.5f, cy - 3, 3, 3}, c);
            gfx_.line(cx + 3.5f, cy + 2.5f, cx + 6.5f, cy + 2.5f, c, 1.2f);
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
        SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(addressFont_), TRUE);
        SendMessageW(ask_, WM_SETFONT, reinterpret_cast<WPARAM>(addressFont_), TRUE);
    }
    const float side = W >= 760 ? kSidebarW : 0;   // a narrow window gives the files the room
    sidebarRect_ = {0, 0, side, H};
    if (side > 0) drawSidebar({0, 0, side, H});
    drawToolbar({side, 0, W - side, kToolbarH});
    // The panes: one, or two side by side, each with its tabs along the top. The other pane is drawn
    // first with its own tab swapped in, then the active one (whose geometry the mouse and keys use).
    Rect area{side, kToolbarH, W - side, H - kToolbarH};
    previewRect_ = {};
    if (settings_.previewPane && area.w >= 760) {
        const float pw = std::clamp(std::floor(area.w * 0.28f), 260.0f, 420.0f);
        previewRect_ = {area.right() - pw, area.y, pw, area.h};
        area.w -= pw;
    }
    const bool two = twoPanes_ && area.w >= 700 && !tabs_[1].empty();
    if (two) {
        const float half = std::floor(area.w / 2);
        paneRect_[0] = {area.x, area.y, half - 1, area.h};
        paneRect_[1] = {area.x + half, area.y, area.w - half, area.h};
    } else {
        paneRect_[0] = area;
        paneRect_[1] = {};
    }
    constexpr float kTabsH = 34;
    for (int k = 0; k < (two ? 2 : 1); ++k) {
        const int p = two ? (k == 0 ? 1 - pane_ : pane_) : pane_;   // the active pane last
        const Rect pr = two ? paneRect_[p] : area;
        const bool swapped = p != pane_;
        if (swapped) { swapView(tabs_[p][(size_t)activeTab_[p]]); swappedPane_ = p; }
        idPrefix_ = p == 1 ? "p1:" : "";
        drawTabs({pr.x, pr.y, pr.w, kTabsH}, p);
        drawContent({pr.x, pr.y + kTabsH, pr.w, pr.h - kTabsH});
        if (two && p == pane_) gfx_.fill({pr.x, pr.y + kTabsH - 2, pr.w, 2}, pal_.ribbon.withAlpha(0.8f));   // the active pane
        if (swapped) { swapView(tabs_[p][(size_t)activeTab_[p]]); swappedPane_ = -1; }
        idPrefix_.clear();
    }
    if (two) gfx_.fill({paneRect_[1].x - 1, area.y, 1, area.h}, pal_.rule);
    if (previewRect_.w > 0) drawPreview(previewRect_);
    if (side <= 0) drawStatusNote({side, kToolbarH, W - side, H - kToolbarH});
    // Dragging: what's being dragged, and where it would go.
    if (drag_.active) {
        int tp = -1;
        const std::wstring target = dropTargetFolder(drag_.x, drag_.y, &tp);
        const bool toSidebar = sidebarRect_.contains(drag_.x, drag_.y);
        std::wstring what = drag_.paths.size() == 1 ? nameOf(drag_.paths[0]) : std::to_wstring(drag_.paths.size()) + L" items";
        std::wstring label = what;
        std::wstring toCollection;
        for (auto& [id, rr] : collectionRects_)
            if (rr.contains(drag_.x, drag_.y))
                if (const labels::Collection* c = labels_.collection(id)) toCollection = c->name;
        if (!toCollection.empty()) label = L"Add " + what + L" to \u201C" + toCollection + L"\u201D";
        else if (toSidebar) label = L"Pin " + what + L" to the sidebar";
        else if (!target.empty()) {
            const bool copy = (GetKeyState(VK_CONTROL) & 0x8000) || (drag_.paths.size() && towupper(drag_.paths[0][0]) != towupper(target[0]));
            label = (copy ? L"Copy " : L"Move ") + what + L" to " + (isDriveRoot(target) ? target : nameOf(target));
        }
        const float lw = std::min(gfx_.measureWidth(label, Font::Small) + 24, 420.0f);
        const Rect g{drag_.x + 14, drag_.y + 16, lw, 28};
        gfx_.fillRounded(g, 8, pal_.ink.withAlpha(0.88f));
        gfx_.text(label, g, Font::Small, pal_.paper, {Align::Center, false, true, true});
    }
    drawToast({side, kToolbarH, W - side, H - kToolbarH});
    if (asking_) drawAsk(W, H);
    if (batch_.open) drawBatchRename(W, H);
    if (renaming_) {
        if (renameRect_.w <= 0) { endRename(false); return; }
        const float s = gfx_.dpiScale();
        SendMessageW(rename_, WM_SETFONT, reinterpret_cast<WPARAM>(addressFont_), FALSE);
        SetWindowPos(rename_, HWND_TOP, (int)std::lround(renameRect_.x * s), (int)std::lround(renameRect_.y * s), (int)std::lround(renameRect_.w * s),
                     (int)std::lround(renameRect_.h * s), SWP_NOACTIVATE);
        renameRect_ = {};
    }
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

    // What's here, at the foot of the sidebar; above it the places, collections and labels, which
    // scroll (with the wheel) when the window is too short for them all, and the folder tree.
    const std::wstring status = statusText();
    const float statusH = status.empty() ? 0 : gfx_.measureHeight(status, r.w - 44, Font::Small) + 30;
    const float top = r.y + 80, limit = r.bottom() - statusH - 6;
    sideViewH_ = std::max(0.0f, limit - top);
    gfx_.pushClip({r.x, top, r.w, sideViewH_});
    setHitClip({r.x, top, r.w, sideViewH_});
    float y = r.y + 84 - sideScroll_;
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
    pinnedRects_.clear();
    if (!settings_.pinned.empty()) {
        gfx_.text(L"PINNED", {r.x + 22, y, r.w - 44, 16}, Font::Label, nr::kNightMuted.withAlpha(0.75f), {Align::Left, false});
        y += 22;
        int folderIcon = -1;
        for (const Place& p : places_) if (p.icon >= 0) { folderIcon = p.icon; break; }
        for (const auto& path : settings_.pinned) {
            pinnedRects_.push_back({path, {r.x + 10, y, r.w - 20, 32}});
            row("tb:pin:" + narrow(path), isDriveRoot(path) ? path : nameOf(path), folderIcon, !here_.thisPC && samePath(here_.path, path),
                [this, path] { goPath(path); });
        }
        y += 12;
    }
    gfx_.text(L"PLACES", {r.x + 22, y, r.w - 44, 16}, Font::Label, nr::kNightMuted.withAlpha(0.75f), {Align::Left, false});
    y += 22;
    for (const Place& p : places_)
        row("tb:place:" + narrow(p.label), p.label, p.icon, !here_.thisPC && samePath(here_.path, p.path), [this, p] { goPath(p.path); });
    binRect_ = {r.x + 10, y, r.w - 20, 32};
    row("tb:bin", L"Recycle Bin", recycleIcon_, here_.bin, [this] {
        Location b;
        b.bin = true;
        go(b);
    }, recycleItems_ ? std::to_wstring(recycleItems_) : L"");
    y += 12;
    // Collections, then the colour labels and tags (0.5).
    collectionRects_.clear();
    {
        gfx_.text(L"COLLECTIONS", {r.x + 22, y, r.w - 44, 16}, Font::Label, nr::kNightMuted.withAlpha(0.75f), {Align::Left, false});
        const Rect plus{r.right() - 40, y - 6, 26, 26};
        addHit("tb:coll:new", plus, [this] { newCollectionFromSelection(true); });
        const float ph = hoverAmount("tb:coll:new");
        if (ph > 0.01f) gfx_.fillRounded(plus, 6, nr::kNightInk.withAlpha(0.08f * ph));
        gfx_.line(plus.x + 13, plus.y + 8, plus.x + 13, plus.y + 18, nr::kNightMuted, 1.4f);
        gfx_.line(plus.x + 8, plus.y + 13, plus.x + 18, plus.y + 13, nr::kNightMuted, 1.4f);
        y += 22;
        int folderIcon = -1;
        for (const Place& p : places_) if (p.icon >= 0) { folderIcon = p.icon; break; }
        for (const labels::Collection& c : labels_.collections()) {
            collectionRects_.push_back({c.id, {r.x + 10, y, r.w - 20, 32}});
            row("tb:coll:" + c.id, c.name, folderIcon, here_.collection == c.id, [this, id = c.id] { openCollection(id); },
                c.items.empty() ? L"" : std::to_wstring(c.items.size()));
        }
        if (labels_.collections().empty()) {
            gfx_.text(L"Groups of files from anywhere", {r.x + 22, y, r.w - 44, 20}, Font::Small, nr::kNightMuted.withAlpha(0.7f), {Align::Left, false, true});
            y += 24;
        }
        y += 10;
        gfx_.text(L"LABELS", {r.x + 22, y, r.w - 44, 16}, Font::Label, nr::kNightMuted.withAlpha(0.75f), {Align::Left, false});
        y += 22;
        float dx = r.x + 26;
        for (int c = 1; c < labels::kColours; ++c) {
            const Rect dot{dx - 2, y, 22, 22};
            const std::string id = "tb:label:" + std::to_string(c);
            addHit(id, dot, [this, c] { openLabelSearch(L"colour:" + labels::lowerName((labels::Colour)c)); });
            const float hv = hoverAmount(id);
            if (hv > 0.01f) gfx_.circle(dot.x + 11, dot.y + 11, 10, nr::kNightInk.withAlpha(0.15f * hv), true);
            gfx_.circle(dot.x + 11, dot.y + 11, 6, labelColour((labels::Colour)c), true);
            dx += 23;
        }
        const std::wstring tagsLabel = L"Tags";
        const float tw = gfx_.measureWidth(tagsLabel, Font::Small) + 18;
        const Rect tb{r.right() - 14 - tw, y, tw, 22};
        addHit("tb:tags", tb, [this] { tagsMenu(); });
        const float th2 = hoverAmount("tb:tags");
        gfx_.fillRounded(tb, 11, nr::kNightInk.withAlpha(0.07f + 0.08f * th2));
        gfx_.text(tagsLabel, tb, Font::Small, nr::kNightMuted.mix(nr::kNightInk, 0.5f * th2), {Align::Center, false, true, true});
        y += 34;
    }
    gfx_.text(L"THIS PC", {r.x + 22, y, r.w - 44, 16}, Font::Label, nr::kNightMuted.withAlpha(0.75f), {Align::Left, false});
    y += 22;
    int pcIcon = -1;
    for (const Drive& d : drives_)
        if (d.icon >= 0) { pcIcon = d.icon; break; }
    row("tb:thispc", L"This PC", pcIcon, here_.thisPC, [this] { go({true, L""}); });
    clearHitClip();
    gfx_.popClip();
    // The tree has what's left, at least enough for a few folders (scroll the sidebar to reach it).
    sideContent_ = y + sideScroll_ - (r.y + 84) + 130;
    sideScroll_ = std::clamp(sideScroll_, 0.0f, std::max(0.0f, sideContent_ - sideViewH_));
    float ty = std::max(y, top);
    if (limit - ty >= 40) drawTree(r, ty, limit);
    else treeRect_ = {};
    if (sideScroll_ > 0.5f) {   // more above: the night sky fades in under the logo
        gfx_.line(r.x + 22, top, r.right() - 22, top, nr::kNightInk.withAlpha(0.12f));
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
    const float menuX = right - bs, galleryX = menuX - 10 - bs, iconsX = galleryX - bs - 2, detailsX = iconsX - bs - 2, panesX = detailsX - 10 - bs,
                previewX = panesX - 2 - bs;
    const float avail = previewX - 12 - x;
    if (settings_.previewPane) gfx_.fillRounded({previewX, by, bs, bs}, 8, pal_.ribbon.withAlpha(pal_.dark ? 0.20f : 0.14f));
    navButton("tb:preview", {previewX, by, bs, bs}, IconPreview, true, [this] { togglePreview(); });
    const float searchW = avail >= 380 ? std::clamp(avail * 0.36f, 170.0f, 280.0f) : 0;   // a narrow window keeps the address
    drawAddress({x, by - 1, avail - (searchW > 0 ? searchW + 8 : 0), bs + 2});
    if (searchW > 0) drawSearchBox({x + avail - searchW, by - 1, searchW, bs + 2});
    else { searchRect_ = {}; ShowWindow(search_, SW_HIDE); }
    if (twoPanes_) gfx_.fillRounded({panesX, by, bs, bs}, 8, pal_.ribbon.withAlpha(pal_.dark ? 0.20f : 0.14f));
    navButton("tb:panes", {panesX, by, bs, bs}, IconPanes, true, [this] { setTwoPanes(!twoPanes_); });
    // Views: the current one is marked.
    const float onX = settings_.view == ViewMode::Icons ? iconsX : settings_.view == ViewMode::Gallery ? galleryX : detailsX;
    gfx_.fillRounded({onX, by, bs, bs}, 8, pal_.ribbon.withAlpha(pal_.dark ? 0.20f : 0.14f));
    navButton("tb:gallery", {galleryX, by, bs, bs}, IconGallery, true, [this] { setView(ViewMode::Gallery); });
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
    } else if (here_.bin) {
        parts.push_back({L"Recycle Bin", here_});
    } else if (!here_.collection.empty()) {
        parts.push_back({L"Collections", here_});
        parts.push_back({nameFor(here_), here_});
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

void FilesWindow::drawContent(const Rect& rIn) {
    gfx_.fill(rIn, pal_.paper);
    listClip_ = {};
    driveRects_.clear();
    if (here_.thisPC) { drawThisPC(rIn); return; }
    if (error_ != ListError::None) {
        std::wstring title;
        std::wstring text = describeListError(error_, errorCode_, here_.path, &title);
        if (error_ == ListError::NotFound && isDriveRoot(here_.path)) {
            // A drive letter that isn't on this PC (now).
            title = L"There's no " + here_.path.substr(0, 2) + L" drive";
            text = L"No drive on this PC is using that letter right now. A USB drive or network drive may have been disconnected.";
        }
        drawMessage(rIn, title, text, true);
        return;
    }
    Rect r = rIn;
    if (here_.searching() && here_.collection.empty() && !here_.bin) {
        const float bh = drawSearchBar(r);
        r = {r.x, r.y + bh, r.w, r.h - bh};
    }
    if (shown_.empty() && here_.bin && !loading_) {
        if (!here_.searching() && recycleItems_ > 0) {
            // Windows says there's something in it, but its Recycle Bin folder didn't list it.
            drawMessage(r, L"The Recycle Bin couldn't be listed here",
                        L"Windows says it holds " + std::to_wstring(recycleItems_) +
                            (recycleItems_ == 1 ? L" item" : L" items") + L", but didn't say what. File Explorer can show them.",
                        false);
            const float bw = 190;
            button("tb:bin:explorer", {r.x + (r.w - bw) / 2, r.y + std::max(40.0f, r.h * 0.2f) + 110, bw, 36}, L"Open in File Explorer",
                   [this] { ShellExecuteW(hwnd(), L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL); }, true);
            return;
        }
        drawMessage(r, here_.searching() ? L"Nothing found" : L"The Recycle Bin is empty",
                    here_.searching() ? L"Nothing in the Recycle Bin matches \u201C" + here_.query + L"\u201D."
                                      : L"Things you delete wait here until you empty it, so they can be put back.",
                    false);
        return;
    }
    if (shown_.empty() && !here_.collection.empty() && !loading_) {
        drawMessage(r, here_.searching() ? L"Nothing found" : L"This collection is empty",
                    here_.searching() ? L"Nothing in this collection matches \u201C" + here_.query + L"\u201D."
                                      : L"Right-click files or folders anywhere and choose \u201CAdd to collection\u201D, or drag them onto it in the sidebar. They stay where they are.",
                    false);
        return;
    }
    if (shown_.empty() && here_.searching()) {
        if (!loading_)
            drawMessage(r, L"Nothing found",
                        here_.everywhere ? L"No names on this PC match “" + here_.query + L"”. Try fewer or shorter words."
                                         : L"No names in this folder or the folders in it match “" + here_.query +
                                               L"”. Try fewer or shorter words, or search the whole PC.",
                        false);
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
    const bool hov = hoverId() == idPrefix_ + "row:" + std::to_string(row);
    if (sel) gfx_.fillRounded(r, radius, pal_.ribbon.withAlpha(pal_.dark ? (hov ? 0.30f : 0.22f) : (hov ? 0.22f : 0.15f)));
    else if (hov) gfx_.fillRounded(r, radius, pal_.ink.withAlpha(pal_.dark ? 0.06f : 0.04f));
    if ((int)row == sel_.focus() && GetFocus() == hwnd()) gfx_.strokeRounded(r, radius, pal_.ribbon.withAlpha(0.7f), 1);
}

void FilesWindow::drawDetails(const Rect& r) {
    const float pad = 16;
    const float x0 = r.x + pad, w = r.w - pad * 2;
    // Columns: Name takes what's left; Type then Date go when it's narrow.
    const bool showType = w >= 620, showDate = w >= 430;
    // Search results: where each one is, in place of its type.
    const bool folderCol = here_.searching() || !here_.collection.empty() || here_.bin;
    const float sizeW = 100, typeW = showType ? (folderCol ? std::min(260.0f, w * 0.3f) : 170) : 0, dateW = showDate ? 170 : 0;
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
    head(idPrefix_ + "tb:sort:name", L"Name", xName, nameW, SortKey::Name, false);
    head(idPrefix_ + "tb:sort:date", here_.bin ? L"Date deleted" : L"Date modified", xDate, dateW, SortKey::Modified, false);
    if (folderCol) {
        if (typeW > 0) gfx_.text(here_.bin ? L"Deleted from" : L"Folder", {xType + 10, hr.y, typeW - 24, hr.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
    } else {
        head(idPrefix_ + "tb:sort:type", L"Type", xType, typeW, SortKey::Type, false);
    }
    head(idPrefix_ + "tb:sort:size", L"Size", xSize, sizeW - 14, SortKey::Size, true);

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
        addHit(idPrefix_ + "row:" + std::to_string(i), row, nullptr, false);
        drawRowBackground(row, i, 6);
        const float fade = (e.hidden() || e.protectedSystem() || (!cutPaths_.empty() && cutPaths_.count(lowerPath(rowPath(i))))) ? 0.55f : 1.0f;
        if (renaming_ && (int)i == renameRow_) renameRect_ = {xName + 26, row.y + 2, std::max(120.0f, nameW - 36), row.h - 4};
        // Its colour label (a bar at the row's edge) and tags (chips at the end of its name).
        const std::wstring rp = rowPath(i);
        const labels::Label* lab = labels_.find(rp);
        float chipsW = 0;
        if (lab) {
            if (lab->colour != labels::Colour::None)
                gfx_.fillRounded({row.x + 1, row.y + 6, 4, row.h - 12}, 2, labelColour(lab->colour));
            float cx = xName + nameW - 12;
            const std::size_t shownTags = std::min<std::size_t>(lab->tags.size(), 2);
            std::vector<std::wstring> chips(lab->tags.begin(), lab->tags.begin() + (long)shownTags);
            if (lab->tags.size() > shownTags) chips.push_back(L"+" + std::to_wstring(lab->tags.size() - shownTags));
            for (std::size_t k = chips.size(); k-- > 0;) {
                const float cw = std::min(110.0f, gfx_.measureWidth(chips[k], Font::Small) + 16);
                if (cx - cw < xName + 120) break;
                cx -= cw;
                gfx_.fillRounded({cx, row.y + (row.h - 20) / 2, cw, 20}, 10, pal_.ink.withAlpha(pal_.dark ? 0.10f : 0.07f));
                gfx_.text(chips[k], {cx + 8, row.y, cw - 16, row.h}, Font::Small, pal_.inkMuted, {Align::Center, false, true, true});
                cx -= 4;
                chipsW = xName + nameW - 12 - cx;
            }
        }
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
        gfx_.text(e.name, {xName + 30, row.y, nameW - 40 - chipsW, row.h}, Font::Body, e.icon < 0 && e.attributes == 0 ? pal_.inkFaint : ink,
                  {Align::Left, false, true, true});
        if (dateW > 0) gfx_.text(dateText(e.modified), {xDate + 10, row.y, dateW - 14, row.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
        if (typeW > 0)
            gfx_.text(folderCol ? searchFolderOf(i) : e.type, {xType + 10, row.y, typeW - 14, row.h}, Font::Small, pal_.inkMuted,
                      {Align::Left, false, true, true});
        if (!e.folder())
            gfx_.text(formatSizeKb(e.size), {xSize, row.y, sizeW - 14, row.h}, Font::Small, pal_.inkMuted, {Align::Right, false, true, true});
    }
    endScroll((float)n * rowH_ + 12);
}

void FilesWindow::drawIcons(const Rect& r) {
    const bool gallery = settings_.view == ViewMode::Gallery;
    const float pad = 16;
    const Rect clip{r.x, r.y + 4, r.w, r.h - 4};
    listClip_ = clip;
    const float avail = r.w - pad * 2;
    columns_ = std::max(1, (int)(avail / (gallery ? 200 : 156)));
    tileW_ = avail / (float)columns_;
    tileH_ = gallery ? tileW_ : 172;
    listX_ = r.x + pad;
    const float top = beginScroll(clip);
    listTop_ = top + 8;
    // Pictures are made at the size they're shown on this display.
    const float boxW = 136, boxH = 102;
    const int thumbPx = gallery ? (tileW_ * gfx_.dpiScale() <= 256 ? 256 : 512) : (boxW * gfx_.dpiScale() <= 128 ? 128 : 256);
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
            addHit(idPrefix_ + "row:" + std::to_string(i), t, nullptr, false);
            drawRowBackground(t, i, 8);
            const float fade = (e.hidden() || e.protectedSystem() || (!cutPaths_.empty() && cutPaths_.count(lowerPath(rowPath(i))))) ? 0.55f : 1.0f;
            const labels::Colour lc = labels_.colourOf(rowPath(i));
            if (gallery) {
                drawGalleryTile(t, i, thumbPx, fade);
                if (lc != labels::Colour::None) {
                    gfx_.circle(t.x + 14, t.y + 14, 7, pal_.paper, true);
                    gfx_.circle(t.x + 14, t.y + 14, 5.5f, labelColour(lc), true);
                }
                continue;
            }
            const Rect box{t.x + (t.w - boxW) / 2, t.y + 8, boxW, boxH};
            if (renaming_ && (int)i == renameRow_) renameRect_ = {t.x + 2, box.bottom() + 4, t.w - 4, 28};
            const Thumb* th = nullptr;
            if (thumbs_ && !e.folder() && !e.cloudOnly() && Thumbnails::hasPicture(e.name))
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
            if (lc != labels::Colour::None) {   // its colour label, at the corner
                gfx_.circle(t.x + 13, t.y + 13, 7, pal_.paper, true);
                gfx_.circle(t.x + 13, t.y + 13, 5.5f, labelColour(lc), true);
            }
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
        if (hidden && !here_.searching()) left += L" (" + std::to_wstring(hidden) + L" hidden)";
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

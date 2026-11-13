#include "studio_app.hpp"

#include "cjui/packages.hpp"
#include "resource.h"
#include "version.hpp"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <cstring>
#include <dwmapi.h>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include "cj/json_safe.hpp"
#include <sstream>

namespace nrs {

using nlohmann::json;
using namespace studio;

static const UINT_PTR kSaveTimer = 1;
static const UINT_PTR kTickTimer = 2;
static const UINT_PTR kCloseTimer = 3;   // waiting for the editor's last save
static const UINT_PTR kTypesetTimer = 4; // a pause after a design change: set the pages again


enum Cmd {
    CmdNewWorld = 100, CmdNewSeries, CmdNewBook, CmdOpenLibrary, CmdShowFolder, CmdExport, CmdBackup, CmdExit,
    CmdRename, CmdTrash, CmdUndoTrash, CmdEmptyTrash,
    CmdGallery, CmdList, CmdPanel, CmdThemeSystem, CmdThemeLight, CmdThemeDark,
    CmdAbout, CmdFormatDoc, CmdImport, CmdImportInto, CmdWriting, CmdSettings,
    // Popup menus
    CmdMoveWorldBase = 1000,      // + index into liveWorlds, or CmdMoveIndependent
    CmdMoveIndependent = 1999,
    CmdAddSeriesBase = 2000,      // + index into all live series
    CmdRemoveFromSeries = 2999,
    CmdMakeStandalone = 3000,
    CmdOpenBook, CmdNewBookHere, CmdNewSeriesHere, CmdRestore, CmdSeriesWorldBase = 3100,
    CmdStatusBase = 4000, CmdKindBase = 4100, CmdSortBase = 4200, CmdFilterStatusBase = 4300, CmdFilterAll = 4399,
    CmdFilterTagBase = 4400,
};

// ------------------------------------------------------------------ helpers

static std::wstring w(const std::string& s) { return widen(s); }

static LRESULT CALLBACK wheelScroll(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEWHEEL) {
        const int lines = -GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3;
        SendMessageW(h, EM_LINESCROLL, 0, lines);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, wheelScroll, 1);
    return DefSubclassProc(h, msg, wp, lp);
}

static std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

static bool readText(const std::wstring& path, std::string& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// ---------------------------------------------------------------- lifecycle

std::wstring StudioApp::stateDir() const {
    return knownFolder(FOLDERID_LocalAppData) + L"\\NorthRay\\Writing Studio";
}

std::string StudioApp::defaultLibraryFolder() const {
    // The user profile root isn't synced by OneDrive (Documents and Desktop often are).
    return narrow(knownFolder(FOLDERID_Profile) + L"\\Writing Studio Library");
}

void StudioApp::loadSettings() {
    std::string text;
    if (!readText(stateDir() + L"\\settings.json", text)) return;
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    settings_.libraryFolder = cj::valueOr(j, "libraryFolder", "");
    settings_.gallery = cj::valueOr(j, "gallery", true);
    settings_.showPanel = cj::valueOr(j, "showPanel", true);
    settings_.sort = std::clamp(cj::valueOr(j, "sort", 0), 0, 4);
    settings_.theme = std::clamp(cj::valueOr(j, "theme", 0), 0, 2);
    if (j.contains("collapsedWorlds") && j["collapsedWorlds"].is_array())
        for (auto& x : j["collapsedWorlds"]) if (x.is_string()) settings_.collapsedWorlds.push_back(x.get<std::string>());
    settings_.units = cj::valueOr(j, "units", "in");
    settings_.ebookDevice = cj::valueOr(j, "ebookDevice", "ereader");
    settings_.ebookTheme = cj::valueOr(j, "ebookTheme", "white");
    settings_.ebookFont = cj::valueOr(j, "ebookFont", "book");
    settings_.ebookScale = std::clamp(cj::valueOr(j, "ebookScale", 1.0), 0.7, 2.0);
    settings_.lastBook = cj::valueOr(j, "lastBook", "");
    settings_.lastNode = cj::valueOr(j, "lastNode", "");
    settings_.lastInWriting = cj::valueOr(j, "lastInWriting", false);
}

void StudioApp::saveSettings() {
    json j;
    j["libraryFolder"] = settings_.libraryFolder;
    j["gallery"] = settings_.gallery;
    j["showPanel"] = settings_.showPanel;
    j["sort"] = settings_.sort;
    j["theme"] = settings_.theme;
    j["collapsedWorlds"] = settings_.collapsedWorlds;
    j["units"] = settings_.units;
    j["ebookDevice"] = settings_.ebookDevice;
    j["ebookTheme"] = settings_.ebookTheme;
    j["ebookFont"] = settings_.ebookFont;
    j["ebookScale"] = settings_.ebookScale;
    j["lastBook"] = settings_.lastBook;
    j["lastNode"] = settings_.lastNode;
    j["lastInWriting"] = settings_.lastInWriting;
    std::string err;
    atomicWrite(narrow(stateDir() + L"\\settings.json"), j.dump(2), err);
}

bool StudioApp::create(HINSTANCE inst, int show) {
    loadSettings();
    WindowSpec spec;
    spec.instance = inst;
    spec.className = L"NorthRayWritingStudio";
    spec.title = L"NorthRay Writing Studio";
    spec.iconId = IDI_APP;
    spec.width = 1280;
    spec.height = 800;
    spec.minWidth = 980;
    spec.minHeight = 600;
    spec.show = show;
    if (!createWindow(spec)) return false;
    // Text fields are child windows: keep the page's drawing off them.
    SetWindowLongPtrW(hwnd(), GWL_STYLE, GetWindowLongPtrW(hwnd(), GWL_STYLE) | WS_CLIPCHILDREN);
    applyTheme();
    buildMenu();
    createFields();
    const std::string folder = settings_.libraryFolder.empty() ? defaultLibraryFolder() : settings_.libraryFolder;
    if (openLibrary(folder, false)) restorePlace();
    else if (folder != defaultLibraryFolder()) openLibrary(defaultLibraryFolder(), false);
    ShowWindow(hwnd(), show);
    UpdateWindow(hwnd());
    SetTimer(hwnd(), kTickTimer, 30000, nullptr);
    return true;
}

void StudioApp::handleCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    std::wstring file;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--import") continue;
        if (!a.empty() && a[0] != L'-') file = a;
    }
    LocalFree(argv);
    if (!file.empty() && lib_) importPath(file, {});
}

void StudioApp::onDestroy() {
    commitFields();
    editor_.destroy();
    preview_.destroy();
    rememberPlace();
    saveSettings();
    lib_.reset();  // closes the database cleanly (checkpoints the journal)
    if (fieldFont_) DeleteObject(fieldFont_);
    if (fieldBrush_) DeleteObject(fieldBrush_);
    if (accel_) DestroyAcceleratorTable(accel_);
}

void StudioApp::applyTheme() {
    const bool dark = settings_.theme == 2 || (settings_.theme == 0 && systemPrefersDark());
    Palette p = makePalette(dark);
    // The Studio's own identity: deep ink blue instead of the suite's ribbon red.
    p.ribbon = dark ? hexColor("#8FB0D9") : hexColor("#2D4A6E");
    applyPalette(p);
    if (fieldBrush_) DeleteObject(fieldBrush_);
    const Color s = pal_.surface;
    fieldBrush_ = CreateSolidBrush(RGB((int)(s.r * 255), (int)(s.g * 255), (int)(s.b * 255)));
    for (HWND h : fields_) if (h) InvalidateRect(h, nullptr, TRUE);
    if (editor_.ready()) editor_.send(json{{"type", "theme"}, {"dark", dark}}.dump());
    BOOL darkTitle = dark;
    DwmSetWindowAttribute(hwnd(), 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkTitle, sizeof darkTitle);
    invalidate();
}

// ------------------------------------------------------------------ library

bool StudioApp::openLibrary(const std::string& folder, bool interactive) {
    commitFields();
    OpenResult r;
    auto lib = Library::open(folder, r);
    if (!r.ok) {
        if (interactive)
            MessageBoxW(hwnd(), (L"That library couldn't be opened.\n\n" + w(r.error)).c_str(), L"NorthRay Writing Studio",
                        MB_OK | MB_ICONWARNING);
        libraryNote_ = w(r.error);
        return false;
    }
    lib_ = std::move(lib);
    if (settings_.libraryFolder != folder) { settings_.lastBook.clear(); settings_.lastNode.clear(); settings_.lastInWriting = false; }
    settings_.libraryFolder = folder;
    saveSettings();
    libraryNote_.clear();
    for (auto& n : r.notes)
        if (n.find("cloud-sync") != std::string::npos) libraryNote_ = w(n);
    view_ = View::AllBooks;
    viewId_.clear();
    focus_ = {};
    ws_ = Workspace::Library;
    writingBook_.clear();
    currentNode_.clear();
    importPlan_.reset();
    editor_.setObjectsFolder(w(lib_->folder()) + L"\\objects");
    preview_.setObjectsFolder(w(lib_->folder()) + L"\\objects");
    pv_ = {};
    pb_ = {};
    wb_ = {};
    pl_ = {};
    rv_ = {};
    if (ws_ == Workspace::Preview || ws_ == Workspace::Typesetting || ws_ == Workspace::Publishing || ws_ == Workspace::Worldbuilding ||
        ws_ == Workspace::Planning || ws_ == Workspace::Review)
        ws_ = Workspace::Library;
    loadedFocus_ = {Focus::None, "-", ""};
    resetScroll();
    flash(r.created ? L"Created a new, empty library." : L"Library opened. A backup copy was made.", 5);
    invalidate();
    return true;
}

void StudioApp::chooseLibraryFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose a library folder (an existing library, or an empty folder for a new one)");
    std::string chosen;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { chosen = narrow(p); CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    if (chosen.empty()) return;
    const std::wstring wc = w(chosen);
    const bool hasLibrary = GetFileAttributesW((wc + L"\\library.db").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!hasLibrary) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((wc + L"\\*").c_str(), &fd);
        int entries = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) ++entries; } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (entries > 0) {
            const std::wstring q = L"There's no library in that folder, and it already has other files in it.\n\n"
                                   L"Create a new library there anyway? It'll add library.db, objects and backups next to them.";
            if (MessageBoxW(hwnd(), q.c_str(), L"New library", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
        }
    }
    openLibrary(chosen, true);
}

void StudioApp::exportLibrary() {
    if (!lib_) return;
    commitFields();
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Library export (JSON)", L"*.json"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(L"json");
    dlg->SetFileName(L"Writing Studio library.json");
    std::string path;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = narrow(p); CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    if (path.empty()) return;
    std::string err;
    if (lib_->exportJson(path, err)) flash(L"Exported the library structure and book details.", 6);
    else MessageBoxW(hwnd(), w("The export couldn't be written.\n\n" + err).c_str(), L"Export library", MB_OK | MB_ICONWARNING);
}

void StudioApp::backupNow() {
    if (!lib_) return;
    commitFields();
    std::string err;
    if (lib_->backup(err)) flash(L"Backup made in the library's backups folder.", 5);
    else MessageBoxW(hwnd(), w("The backup couldn't be made.\n\n" + err).c_str(), L"Back up", MB_OK | MB_ICONWARNING);
}

// ------------------------------------------------------------------- menus

// Keyboard shortcuts. There's no menu bar: every command has a place in the
// window itself, and the Settings page lists these shortcuts.
void StudioApp::buildMenu() {
    ACCEL acc[] = {
        {FCONTROL | FVIRTKEY, 'N', CmdNewBook},
        {FCONTROL | FSHIFT | FVIRTKEY, 'N', CmdNewSeries},
        {FCONTROL | FSHIFT | FVIRTKEY, 'W', CmdNewWorld},
        {FCONTROL | FVIRTKEY, 'O', CmdOpenLibrary},
        {FCONTROL | FVIRTKEY, 'E', CmdExport},
        {FCONTROL | FSHIFT | FVIRTKEY, 'I', CmdImport},
        {FCONTROL | FVIRTKEY, 'W', CmdWriting},
        {FVIRTKEY, VK_F4, CmdPanel},
        {FCONTROL | FVIRTKEY, '1', CmdGallery},
        {FCONTROL | FVIRTKEY, '2', CmdList},
        {FCONTROL | FVIRTKEY, VK_UP, WPrev},
        {FCONTROL | FVIRTKEY, VK_DOWN, WNext},
        {FCONTROL | FVIRTKEY, VK_OEM_COMMA, CmdSettings},
    };
    accel_ = CreateAcceleratorTableW(acc, (int)std::size(acc));
}

void StudioApp::command(int id) {
    if (!lib_ && id != CmdOpenLibrary && id != CmdExit && id != CmdAbout) return;
    if (id >= 5000 && id < 6000) {
        // From the menu while the Library is showing: Ctrl+Up / Ctrl+Down mean nothing there.
        if ((id == WPrev || id == WNext) && ws_ != Workspace::Writing) return;
        writingCommand(id);
        return;
    }
    auto currentBook = [&]() -> Id {
        if (ws_ == Workspace::Writing && lib_->book(writingBook_)) return writingBook_;
        if (focus_.kind == Focus::Book) return focus_.id;
        if (view_ == View::Book) return viewId_;
        return {};
    };
    switch (id) {
        case CmdImport: startImport({}); break;
        case CmdImportInto: {
            const Id b = currentBook();
            if (b.empty()) flash(L"Select a book first, or use Import a book to make a new one.", 6);
            else startImport(b);
            break;
        }
        case CmdWriting: {
            const Id b = currentBook();
            if (b.empty()) flash(L"Choose a book in the Library first, then open Writing.", 5);
            else openWriting(b);
            break;
        }
        case CmdNewWorld: newWorld(); break;
        case CmdNewSeries:
            newSeries(view_ == View::World ? viewId_ : view_ == View::Series && lib_->seriesById(viewId_) ? lib_->seriesById(viewId_)->worldId : Id{});
            break;
        case CmdNewBook:
            if (view_ == View::Series) newBook({}, viewId_);
            else newBook(view_ == View::World ? viewId_ : Id{}, {});
            break;
        case CmdOpenLibrary: chooseLibraryFolder(); break;
        case CmdShowFolder: ShellExecuteW(hwnd(), L"open", w(lib_->folder()).c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case CmdExport: exportLibrary(); break;
        case CmdBackup: backupNow(); break;
        case CmdExit: PostMessageW(hwnd(), WM_CLOSE, 0, 0); break;
        case CmdRename: renameFocused(); break;
        case CmdTrash: trashFocused(); break;
        case CmdUndoTrash: undoTrash(); break;
        case CmdEmptyTrash: emptyTrash(); break;
        case CmdGallery: settings_.gallery = true; saveSettings(); break;
        case CmdList: settings_.gallery = false; saveSettings(); break;
        case CmdPanel: settings_.showPanel = !settings_.showPanel; saveSettings(); break;
        case CmdThemeSystem: settings_.theme = 0; saveSettings(); applyTheme(); break;
        case CmdThemeLight: settings_.theme = 1; saveSettings(); applyTheme(); break;
        case CmdThemeDark: settings_.theme = 2; saveSettings(); applyTheme(); break;
        case CmdAbout: showAbout(); break;
        case CmdSettings: openSettings(); break;
        case CmdFormatDoc: {
            const std::wstring t = L"Your library is a folder:\n\n" + (lib_ ? w(lib_->folder()) : std::wstring(L"(none open)")) +
                L"\n\n- library.db holds your worlds, series and books (SQLite; every change is saved at once and can't be half-written).\n"
                L"- objects holds covers and images, each stored once.\n"
                L"- backups holds a full copy made every time the library opens (the newest 10).\n\n"
                L"Keep the folder out of OneDrive or Dropbox while the Studio is open. Use Export on the Settings page for a portable copy.";
            MessageBoxW(hwnd(), t.c_str(), L"Where my work is stored", MB_OK | MB_ICONINFORMATION);
            break;
        }
    }
    invalidate();
}

void StudioApp::openSettings() {
    commitFields();
    if (ws_ == Workspace::Import) cancelImport();
    ws_ = Workspace::Library;
    goTo(View::Settings);
}

void StudioApp::showAbout() {
    const std::wstring t = L"NorthRay Writing Studio " CJ_VERSION_WSTR L"\n"
                           L"Developed by the " CJ_DEVELOPER_WSTR L" project.\n\n"
                           L"For writing and publishing books: the Author Library (worlds, series and books), the manuscript editor "
                           L"with saved versions and comments, planning, worldbuilding, review, typesetting, print and ebook previews, "
                           L"and print-ready PDF and EPUB files. Imports EPUB, Word, OpenDocument, web pages, Markdown and text "
                           L"(PDF importing is experimental).\n\n"
                           L"No account or subscription is needed; your work stays on this PC.\n\n"
                           L"Uses SQLite (public domain), ProseMirror (MIT), miniz (MIT), PDFium (BSD/Apache), HarfBuzz (MIT), FreeType "
                           L"(FreeType Licence), stb_image and stb_image_write (public domain), hyph-utf8 hyphenation patterns, the Crimson "
                           L"Text and Gentium Book fonts (SIL Open Font License) and Microsoft Edge WebView2. Licences are in the licenses "
                           L"folder next to the program.";
    MessageBoxW(hwnd(), t.c_str(), L"About NorthRay Writing Studio", MB_OK | MB_ICONINFORMATION);
}

// ------------------------------------------------------------- navigation

void StudioApp::goTo(View v, const Id& id) {
    commitFields();
    view_ = v;
    viewId_ = id;
    resetScroll();
    panelScroll_ = 0;
    switch (v) {
        case View::World: setFocus({Focus::World, id, {}}); break;
        case View::Series: setFocus({Focus::Series, id, {}}); break;
        case View::Book: setFocus({Focus::Book, id, {}}); break;
        default: setFocus({}); break;
    }
    invalidate();
}

void StudioApp::setFocus(const Focus& f) {
    if (f == focus_) return;
    commitFields();
    focus_ = f;
    panelScroll_ = 0;
    invalidate();
}

// ---------------------------------------------------------------- creating

void StudioApp::newWorld() {
    const Id id = lib_->createWorld("New world");
    if (id.empty()) { flash(w("Couldn't create the world: " + lib_->lastError()), 8); return; }
    goTo(View::World, id);
    renameFocused();
}

void StudioApp::newSeries(const Id& worldId) {
    const Id id = lib_->createSeries("New series", worldId);
    if (id.empty()) { flash(w("Couldn't create the series: " + lib_->lastError()), 8); return; }
    goTo(View::Series, id);
    renameFocused();
}

void StudioApp::newBook(const Id& worldId, const Id& seriesId) {
    const Id id = lib_->createBook("Untitled book", worldId, seriesId);
    if (id.empty()) { flash(w("Couldn't create the book: " + lib_->lastError()), 8); return; }
    setFocus({Focus::Book, id, seriesId});
    renameFocused();
}

void StudioApp::renameFocused() {
    if (focus_.kind == Focus::None) return;
    settings_.showPanel = true;
    loadFields();
    invalidate();
    UpdateWindow(hwnd());  // lays the field out before focusing it
    SetFocus(fields_[FName]);
    SendMessageW(fields_[FName], EM_SETSEL, 0, -1);
}

void StudioApp::trashFocused() {
    if (focus_.kind == Focus::None) return;
    if (focus_.kind == Focus::Node) { writingCommand(5006 /* WTrash */); return; }
    if (focus_.kind == Focus::Comment) { deleteComment(focus_.id); return; }
    if (focus_.kind == Focus::Picture) {
        if (pic_.pos >= 0 && editor_.ready()) editor_.send(json{{"type", "removePicture"}, {"pos", pic_.pos}}.dump());
        return;
    }
    if (focus_.kind == Focus::Artwork) {
        const Artwork* a = lib_->artwork(focus_.id);
        if (!a) return;
        const size_t used = lib_->artworkUsage(a->blob).size();
        if (used) {
            const std::wstring q = L"“" + w(a->name) + L"” is used in " + std::to_wstring(used) + (used == 1 ? L" chapter" : L" chapters") +
                                   L". Move it to the Trash anyway?\n\nThe chapters keep showing it; it just leaves the artwork library.";
            if (MessageBoxW(hwnd(), q.c_str(), L"Move to Trash", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
        }
        commitFields();
        const std::wstring name = w(a->name);
        if (!lib_->trashArtwork(focus_.id)) { flash(L"Couldn't move that to the Trash.", 6); return; }
        recentlyTrashed_.push_back(focus_.id);
        setFocus({});
        flash(L"Moved “" + name + L"” to the Trash. Ctrl+Z puts it back.", 8);
        return;
    }
    commitFields();
    bool ok = false;
    std::wstring what;
    if (focus_.kind == Focus::Book) {
        if (const Book* b = lib_->book(focus_.id)) what = w(b->title);
        ok = lib_->trashBook(focus_.id);
    } else if (focus_.kind == Focus::Series) {
        if (const Series* s = lib_->seriesById(focus_.id)) what = w(s->name);
        ok = lib_->trashSeries(focus_.id);
    } else if (focus_.kind == Focus::World) {
        if (const World* x = lib_->world(focus_.id)) what = w(x->name);
        ok = lib_->trashWorld(focus_.id);
    }
    if (!ok) { flash(L"Couldn't move that to the Trash.", 6); return; }
    recentlyTrashed_.push_back(focus_.id);
    if ((view_ == View::World || view_ == View::Series || view_ == View::Book) && viewId_ == focus_.id) goTo(View::AllBooks);
    else setFocus({});
    flash(L"Moved “" + what + L"” to the Trash. Ctrl+Z puts it back.", 8);
}

void StudioApp::undoTrash() {
    if (recentlyTrashed_.empty()) return;
    const Id id = recentlyTrashed_.back();
    recentlyTrashed_.pop_back();
    std::wstring what;
    if (const Book* b = lib_->book(id)) what = w(b->title);
    else if (const Series* x = lib_->seriesById(id)) what = w(x->name);
    else if (const World* x = lib_->world(id)) what = w(x->name);
    else if (const Artwork* x = lib_->artwork(id)) what = w(x->name);
    if (lib_->restore(id)) flash(what.empty() ? L"Restored." : L"Restored “" + what + L"”.", 4);
}

void StudioApp::emptyTrash() {
    const auto c = lib_->trashCounts();
    int pictures = 0;
    for (auto& a : lib_->artworks()) if (a.deleted) ++pictures;
    if (!c.worlds && !c.series && !c.books && !pictures) { flash(L"The Trash is already empty.", 4); return; }
    int booksGone = c.books;
    for (auto& b : lib_->books())
        if (!b.deleted && !b.worldId.empty()) {
            const World* x = lib_->world(b.worldId);
            if (x && x->deleted) ++booksGone;
        }
    const std::wstring q = L"Permanently delete everything in the Trash?\n\n" + std::to_wstring(c.worlds) + L" world(s), " +
                           std::to_wstring(c.series) + L" series and " + std::to_wstring(booksGone) +
                           L" book(s), including books inside deleted worlds" +
                           (pictures ? L", and " + std::to_wstring(pictures) + L" picture(s) from the artwork library" : std::wstring()) +
                           L".\n\nThis can't be undone. "
                           L"(The library's backups folder still has copies from earlier sessions.)";
    if (MessageBoxW(hwnd(), q.c_str(), L"Empty Trash", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
    commitFields();
    if (lib_->emptyTrash()) { recentlyTrashed_.clear(); setFocus({}); flash(L"The Trash was emptied.", 5); }
    else flash(w("Couldn't empty the Trash: " + lib_->lastError()), 8);
}

// ------------------------------------------------------------------ images

void StudioApp::chooseImage(Focus::Kind kind, const Id& id, int which) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"Images", L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.webp"}};
    dlg->SetFileTypes(1, spec);
    std::wstring path;
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    if (path.empty()) return;
    std::string bytes;
    if (!readText(path, bytes) || bytes.empty()) { flash(L"That image couldn't be read.", 6); return; }
    if (bytes.size() > 200u * 1024 * 1024) { flash(L"That image is over 200 MB, which is too large.", 8); return; }
    // The original file stays where it is; the library keeps its own copy.
    const std::string hash = lib_->storeBlob(bytes);
    if (hash.empty()) { flash(w("Couldn't store the image: " + lib_->lastError()), 8); return; }
    if (gfx_.imageSize(w(lib_->blobPath(hash))).first == 0) {
        flash(L"Windows couldn't read that file as an image.", 8);
        return;
    }
    commitFields();
    if (kind == Focus::Book) {
        if (const Book* b = lib_->book(id)) { Book x = *b; x.coverBlob = hash; lib_->updateBook(x); }
    } else if (kind == Focus::Series) {
        if (const Series* s = lib_->seriesById(id)) { Series x = *s; (which ? x.logoBlob : x.coverBlob) = hash; lib_->updateSeries(x); }
    } else if (kind == Focus::World) {
        if (const World* s = lib_->world(id)) { World x = *s; (which ? x.logoBlob : x.bannerBlob) = hash; lib_->updateWorld(x); }
    } else if (kind == Focus::Entry) {
        if (const Entry* s = lib_->entry(id)) { Entry x = *s; x.imageBlob = hash; lib_->updateEntry(x); }
    }
    const auto [pw, ph] = gfx_.imageSize(w(lib_->blobPath(hash)));
    flash(L"Image added (" + std::to_wstring(pw) + L" × " + std::to_wstring(ph) + L" px).", 5);
    invalidate();
}

// -------------------------------------------------------------- moving

void StudioApp::moveBookTo(const Id& bookId, const Id& worldId) {
    const Book* b = lib_->book(bookId);
    if (!b || b->worldId == worldId) return;
    const auto lost = lib_->seriesLostByMove(bookId, worldId);
    if (!lost.empty()) {
        std::wstring names;
        for (auto* s : lost) names += L"\n  • " + w(s->name);
        const std::wstring q = L"“" + w(b->title) + L"” will leave these series, which belong to its current world:" + names +
                               L"\n\nThe book itself and everything in it are kept. Move it?";
        if (MessageBoxW(hwnd(), q.c_str(), L"Move book", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    }
    commitFields();
    if (lib_->moveBookToWorld(bookId, worldId)) {
        const World* x = lib_->world(worldId);
        flash(L"Moved to " + (x ? w(x->name) : std::wstring(L"Independent books")) + L".", 5);
    } else {
        flash(w("Couldn't move it: " + lib_->lastError()), 8);
    }
}

void StudioApp::addToSeries(const Id& bookId, const Id& seriesId) {
    const Book* b = lib_->book(bookId);
    const Series* s = lib_->seriesById(seriesId);
    if (!b || !s) return;
    if (!s->worldId.empty() && s->worldId != b->worldId) {
        const auto lost = lib_->seriesLostByMove(bookId, s->worldId);
        if (!lost.empty()) {
            const World* x = lib_->world(s->worldId);
            std::wstring names;
            for (auto* l : lost) names += L"\n  • " + w(l->name);
            const std::wstring q = L"Adding it to “" + w(s->name) + L"” moves the book into " + (x ? w(x->name) : L"") +
                                   L", so it leaves:" + names + L"\n\nContinue?";
            if (MessageBoxW(hwnd(), q.c_str(), L"Add to series", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
        }
    }
    commitFields();
    if (lib_->addBookToSeries(bookId, seriesId)) flash(L"Added to “" + w(s->name) + L"”.", 5);
    else flash(w("Couldn't add it: " + lib_->lastError()), 8);
}

void StudioApp::moveSeriesTo(const Id& seriesId, const Id& worldId) {
    const Series* s = lib_->seriesById(seriesId);
    if (!s || s->worldId == worldId) return;
    int books = 0;
    for (auto* b : lib_->booksInSeries(seriesId)) if (b->worldId == s->worldId) ++books;
    const World* x = lib_->world(worldId);
    const std::wstring q = L"Move “" + w(s->name) + L"” to " + (x ? w(x->name) : std::wstring(L"no world")) + L"?\n\n" +
                           std::to_wstring(books) + L" book(s) in it move too. Books that are also in other series of the old world leave those series.";
    if (MessageBoxW(hwnd(), q.c_str(), L"Move series", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    commitFields();
    if (lib_->moveSeriesToWorld(seriesId, worldId)) flash(L"Series moved.", 4);
    else flash(w("Couldn't move it: " + lib_->lastError()), 8);
}

// ------------------------------------------------------------ context menus

void StudioApp::contextMenuFor(const std::string& hitId, float, float) {
    if (!lib_) return;
    if (hitId.rfind("node:", 0) == 0) { nodeContextMenu(hitId.substr(5)); return; }
    auto idAfter = [&](const std::string& prefix) -> std::string {
        if (hitId.rfind(prefix, 0) != 0) return {};
        std::string rest = hitId.substr(prefix.size());
        return rest.substr(0, rest.find_first_of("|@"));
    };
    auto contextSeries = [&]() -> std::string {
        const size_t bar = hitId.find('|');
        if (bar == std::string::npos) return std::string{};
        const size_t at = hitId.find('@', bar);
        return hitId.substr(bar + 1, at == std::string::npos ? std::string::npos : at - bar - 1);
    };
    const auto worlds = lib_->liveWorlds();
    std::vector<const Series*> allSeries;
    for (auto& s : lib_->series()) if (lib_->seriesVisible(s)) allSeries.push_back(&s);

    HMENU m = CreatePopupMenu();
    Id bookId = idAfter("book:"), seriesId = idAfter("series:"), worldId = idAfter("world:");
    if (bookId.empty()) bookId = idAfter("side:book:");
    if (seriesId.empty()) seriesId = idAfter("side:series:");
    if (worldId.empty()) worldId = idAfter("side:world:");
    const std::string inSeries = contextSeries();

    if (!bookId.empty() && lib_->book(bookId)) {
        setFocus({Focus::Book, bookId, inSeries});
        const Book* b = lib_->book(bookId);
        AppendMenuW(m, MF_STRING, CmdOpenBook, L"&Open book page");
        AppendMenuW(m, MF_STRING, CmdRename, L"&Rename\tF2");
        HMENU addS = CreatePopupMenu();
        for (size_t i = 0; i < allSeries.size(); ++i) {
            const World* x = allSeries[i]->worldId.empty() ? nullptr : lib_->world(allSeries[i]->worldId);
            const bool already = lib_->membership(allSeries[i]->id, bookId) != nullptr;
            AppendMenuW(addS, MF_STRING | (already ? MF_CHECKED | MF_GRAYED : 0), CmdAddSeriesBase + (UINT)i,
                        (w(allSeries[i]->name) + (x ? L"  (" + w(x->name) + L")" : L"")).c_str());
        }
        if (allSeries.empty()) AppendMenuW(addS, MF_STRING | MF_GRAYED, 0, L"No series yet");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)addS, L"&Add to series");
        if (!inSeries.empty()) AppendMenuW(m, MF_STRING, CmdRemoveFromSeries, L"Remove from &this series");
        if (!lib_->seriesOfBook(bookId).empty()) AppendMenuW(m, MF_STRING, CmdMakeStandalone, L"Make &standalone (leave every series)");
        HMENU mv = CreatePopupMenu();
        for (size_t i = 0; i < worlds.size(); ++i)
            AppendMenuW(mv, MF_STRING | (worlds[i]->id == b->worldId ? MF_CHECKED | MF_GRAYED : 0), CmdMoveWorldBase + (UINT)i,
                        w(worlds[i]->name).c_str());
        AppendMenuW(mv, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(mv, MF_STRING | (b->worldId.empty() ? MF_CHECKED | MF_GRAYED : 0), CmdMoveIndependent, L"Independent books (no world)");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)mv, L"Move to &world");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CmdTrash, L"Move to &Trash\tDel");
    } else if (!seriesId.empty() && lib_->seriesById(seriesId)) {
        setFocus({Focus::Series, seriesId, {}});
        const Series* s = lib_->seriesById(seriesId);
        AppendMenuW(m, MF_STRING, CmdNewBookHere, L"New &book in this series");
        AppendMenuW(m, MF_STRING, CmdRename, L"&Rename\tF2");
        HMENU mv = CreatePopupMenu();
        for (size_t i = 0; i < worlds.size(); ++i)
            AppendMenuW(mv, MF_STRING | (worlds[i]->id == s->worldId ? MF_CHECKED | MF_GRAYED : 0), CmdSeriesWorldBase + (UINT)i,
                        w(worlds[i]->name).c_str());
        AppendMenuW(mv, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(mv, MF_STRING | (s->worldId.empty() ? MF_CHECKED | MF_GRAYED : 0), CmdSeriesWorldBase + 99, L"No world");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)mv, L"Move to &world");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CmdTrash, L"Move to &Trash (its books are kept)\tDel");
    } else if (!worldId.empty() && lib_->world(worldId)) {
        setFocus({Focus::World, worldId, {}});
        AppendMenuW(m, MF_STRING, CmdNewSeriesHere, L"New &series in this world");
        AppendMenuW(m, MF_STRING, CmdNewBookHere, L"New &book in this world");
        AppendMenuW(m, MF_STRING, CmdRename, L"&Rename\tF2");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CmdTrash, L"Move to &Trash\tDel");
    } else if (const std::string r = idAfter("trash:restore:"); !r.empty()) {
        AppendMenuW(m, MF_STRING, CmdRestore, L"&Restore");
    } else {
        DestroyMenu(m);
        return;
    }
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd(), nullptr);
    DestroyMenu(m);
    if (!cmd) return;
    if (cmd == CmdOpenBook) goTo(View::Book, bookId);
    else if (cmd == CmdRename) renameFocused();
    else if (cmd == CmdTrash) trashFocused();
    else if (cmd == CmdRemoveFromSeries) { commitFields(); lib_->removeBookFromSeries(bookId, inSeries); setFocus({Focus::Book, bookId, {}}); }
    else if (cmd == CmdMakeStandalone) { commitFields(); lib_->makeStandalone(bookId); flash(L"It's now a standalone book.", 4); }
    else if (cmd >= CmdAddSeriesBase && cmd < CmdAddSeriesBase + (int)allSeries.size()) addToSeries(bookId, allSeries[cmd - CmdAddSeriesBase]->id);
    else if (cmd == CmdMoveIndependent) moveBookTo(bookId, {});
    else if (cmd >= CmdMoveWorldBase && cmd < CmdMoveWorldBase + (int)worlds.size()) moveBookTo(bookId, worlds[cmd - CmdMoveWorldBase]->id);
    else if (cmd == CmdSeriesWorldBase + 99) moveSeriesTo(seriesId, {});
    else if (cmd >= CmdSeriesWorldBase && cmd < CmdSeriesWorldBase + (int)worlds.size()) moveSeriesTo(seriesId, worlds[cmd - CmdSeriesWorldBase]->id);
    else if (cmd == CmdNewBookHere) { if (!seriesId.empty()) newBook({}, seriesId); else newBook(worldId, {}); }
    else if (cmd == CmdNewSeriesHere) newSeries(worldId);
    else if (cmd == CmdRestore) { lib_->restore(idAfter("trash:restore:")); }
    invalidate();
}

void StudioApp::popupStatusMenu() {
    if (focus_.kind != Focus::Book && focus_.kind != Focus::Series) return;
    HMENU m = CreatePopupMenu();
    if (focus_.kind == Focus::Book) {
        const Book* b = lib_->book(focus_.id);
        for (int i = 0; i <= (int)BookStatus::OnHold; ++i)
            AppendMenuW(m, MF_STRING | (b && (int)b->status == i ? MF_CHECKED : 0), CmdStatusBase + i, w(label((BookStatus)i)).c_str());
    } else {
        const Series* s = lib_->seriesById(focus_.id);
        for (int i = 0; i <= (int)SeriesStatus::OnHold; ++i)
            AppendMenuW(m, MF_STRING | (s && (int)s->status == i ? MF_CHECKED : 0), CmdStatusBase + i, w(label((SeriesStatus)i)).c_str());
    }
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd(), nullptr);
    DestroyMenu(m);
    if (cmd < CmdStatusBase) return;
    commitFields();
    if (focus_.kind == Focus::Book) {
        if (const Book* b = lib_->book(focus_.id)) { Book x = *b; x.status = (BookStatus)(cmd - CmdStatusBase); lib_->updateBook(x); }
    } else if (const Series* s = lib_->seriesById(focus_.id)) {
        Series x = *s;
        x.status = (SeriesStatus)(cmd - CmdStatusBase);
        lib_->updateSeries(x);
    }
    invalidate();
}

void StudioApp::popupKindMenu() {
    const Membership* mem = lib_->membership(focus_.contextSeries, focus_.id);
    if (!mem) return;
    HMENU m = CreatePopupMenu();
    for (int i = 0; i <= (int)Instalment::Collection; ++i)
        AppendMenuW(m, MF_STRING | ((int)mem->kind == i ? MF_CHECKED : 0), CmdKindBase + i, w(label((Instalment)i)).c_str());
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd(), nullptr);
    DestroyMenu(m);
    if (cmd < CmdKindBase) return;
    commitFields();
    Membership x = *lib_->membership(focus_.contextSeries, focus_.id);
    x.kind = (Instalment)(cmd - CmdKindBase);
    lib_->updateMembership(x);
    invalidate();
}

void StudioApp::popupSortMenu(float, float) {
    static const wchar_t* names[] = {L"Title", L"Series order", L"Author or pen name", L"Status", L"Recently changed"};
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < 5; ++i) AppendMenuW(m, MF_STRING | (settings_.sort == i ? MF_CHECKED : 0), CmdSortBase + i, names[i]);
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd(), nullptr);
    DestroyMenu(m);
    if (cmd >= CmdSortBase && cmd < CmdSortBase + 5) { settings_.sort = cmd - CmdSortBase; saveSettings(); }
    invalidate();
}

void StudioApp::popupFilterMenu(float, float) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (statusFilter_ < 0 && tagFilter_.empty() ? MF_CHECKED : 0), CmdFilterAll, L"Everything");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i <= (int)BookStatus::OnHold; ++i)
        AppendMenuW(m, MF_STRING | (statusFilter_ == i ? MF_CHECKED : 0), CmdFilterStatusBase + i, w(label((BookStatus)i)).c_str());
    std::vector<std::string> tags;
    for (auto* b : lib_->liveBooks())
        for (auto& t : b->tags) if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
    std::sort(tags.begin(), tags.end());
    if (!tags.empty()) {
        HMENU tm = CreatePopupMenu();
        for (size_t i = 0; i < tags.size() && i < 90; ++i)
            AppendMenuW(tm, MF_STRING | (tagFilter_ == tags[i] ? MF_CHECKED : 0), CmdFilterTagBase + (UINT)i, w(tags[i]).c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)tm, L"Tag");
    }
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd(), nullptr);
    DestroyMenu(m);
    if (cmd == CmdFilterAll) { statusFilter_ = -1; tagFilter_.clear(); }
    else if (cmd >= CmdFilterStatusBase && cmd <= CmdFilterStatusBase + (int)BookStatus::OnHold) statusFilter_ = cmd - CmdFilterStatusBase;
    else if (cmd >= CmdFilterTagBase && cmd < CmdFilterTagBase + (int)tags.size()) tagFilter_ = tags[cmd - CmdFilterTagBase];
    invalidate();
}

// ----------------------------------------------------------------- input

bool StudioApp::onKeyDown(WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (!lib_) return false;
    if (ws_ == Workspace::Review && !ctrl && vk == VK_ESCAPE) { ws_ = Workspace::Library; goTo(View::Book, rv_.book); return true; }
    if (ws_ == Workspace::Planning && !ctrl && vk == VK_ESCAPE) {
        if (!pl_.node.empty()) selectCard({});
        else { ws_ = Workspace::Library; goTo(View::Book, pl_.book); }
        return true;
    }
    if (ws_ == Workspace::Worldbuilding && !ctrl) {
        if (vk == VK_ESCAPE) {
            if (!wb_.entry.empty()) openEntry({});
            else { ws_ = Workspace::Library; if (!wb_.world.empty()) goTo(View::World, wb_.world); else goTo(View::Book, wb_.book); }
            return true;
        }
    }
    if (ws_ == Workspace::Publishing) {
        if (vk == VK_ESCAPE || vk == VK_BACK) { ws_ = Workspace::Library; goTo(View::Book, pb_.book); return true; }
        if (vk == VK_NEXT || vk == VK_PRIOR || vk == VK_HOME || vk == VK_END) {
            const float page = pageRect_.h - 60;
            pb_.scroll = vk == VK_HOME ? 0 : vk == VK_END ? pb_.content : pb_.scroll + (vk == VK_NEXT ? page : -page);
            pb_.scroll = std::clamp(pb_.scroll, 0.0f, std::max(0.0f, pb_.content - pageRect_.h));
            invalidate();
            return true;
        }
    }
    if (ws_ == Workspace::Typesetting) {
        if (vk == VK_RIGHT || vk == VK_NEXT || vk == VK_SPACE) { turnPages(1); return true; }
        if (vk == VK_LEFT || vk == VK_PRIOR) { turnPages(-1); return true; }
        if (vk == VK_HOME) { showPage(0); return true; }
        if (vk == VK_END) { showPage(ts_.result ? (int)ts_.result->pages.size() - 1 : 0); return true; }
        if (vk == 'G' && !ctrl) { ts_.guides = !ts_.guides; invalidate(); return true; }
        if (vk == VK_OEM_PLUS || vk == VK_ADD) { ts_.zoom = std::min(2, ts_.zoom + 1); invalidate(); return true; }
        if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) { ts_.zoom = std::max(0, ts_.zoom - 1); invalidate(); return true; }
        if (vk == VK_ESCAPE || vk == VK_BACK) { ws_ = Workspace::Library; goTo(View::Book, ts_.book); return true; }
        return false;
    }
    if (ws_ == Workspace::Writing && focus_.kind == Focus::Node) {
        if (vk == VK_F2) { renameFocused(); return true; }
        if (vk == VK_DELETE) { writingCommand(5006); return true; }
        if ((vk == VK_UP || vk == VK_DOWN) && (GetKeyState(VK_MENU) & 0x8000)) { moveNodeBy(focus_.id, vk == VK_UP ? -1 : 1); return true; }
        // Up / Down walk the outline and keep the keyboard here; Ctrl+Up / Ctrl+Down (also in the text) go by chapter.
        if (vk == VK_UP || vk == VK_DOWN) { stepNode(vk == VK_UP ? -1 : 1, ctrl, false); return true; }
        if (vk == VK_RETURN) { editor_.focus(); editor_.send(R"({"type":"focus"})"); return true; }
        if (vk == VK_ESCAPE) return true;
    }
    if (ws_ == Workspace::Import && vk == VK_ESCAPE && !importBusy_) { cancelImport(); return true; }
    if (vk == VK_F2) { renameFocused(); return true; }
    if (vk == VK_DELETE) { trashFocused(); return true; }
    if (ctrl && vk == 'Z') { undoTrash(); return true; }
    if (vk == VK_ESCAPE && drag_.active) { drag_ = {}; return true; }
    if (vk == VK_BACK || (vk == VK_LEFT && (GetKeyState(VK_MENU) & 0x8000))) {
        if (view_ == View::Book) {
            const Book* b = lib_->book(viewId_);
            if (b && !lib_->seriesOfBook(b->id).empty()) goTo(View::Series, lib_->seriesOfBook(b->id)[0]->id);
            else if (b && !b->worldId.empty()) goTo(View::World, b->worldId);
            else goTo(View::AllBooks);
            return true;
        }
    }
    // Alt+Up / Alt+Down: move the selected book within the series being shown.
    if ((vk == VK_UP || vk == VK_DOWN) && (GetKeyState(VK_MENU) & 0x8000) && view_ == View::Series && focus_.kind == Focus::Book) {
        auto books = lib_->booksInSeries(viewId_, chrono_);
        std::vector<Id> ids;
        for (auto* b : books) ids.push_back(b->id);
        auto it = std::find(ids.begin(), ids.end(), focus_.id);
        if (it == ids.end()) return true;
        const long i = it - ids.begin();
        const long j = vk == VK_UP ? i - 1 : i + 1;
        if (j < 0 || j >= (long)ids.size()) return true;
        std::swap(ids[i], ids[j]);
        lib_->reorderSeriesBooks(viewId_, ids, chrono_);
        return true;
    }
    if (vk == VK_RETURN && focus_.kind == Focus::Book && view_ != View::Book && hoverId().empty()) {
        goTo(View::Book, focus_.id);
        return true;
    }
    return false;
}

void StudioApp::onTimer(UINT_PTR id) {
    if (id == kCloseTimer) {
        KillTimer(hwnd(), kCloseTimer);
        closePending_ = false;
        DestroyWindow(hwnd());
        return;
    }
    if (id == kTypesetTimer) { startTypeset(); return; }
    if (id == kSaveTimer) {
        KillTimer(hwnd(), kSaveTimer);
        commitFields();
    }
    invalidate();
}

LRESULT StudioApp::onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    handled = true;
    switch (msg) {
        case WM_COMMAND: {
            const int code = HIWORD(wp);
            const HWND ctl = reinterpret_cast<HWND>(lp);
            if (ctl) {
                for (int f = 0; f < FieldCount; ++f) {
                    if (fields_[f] != ctl) continue;
                    if (code == EN_CHANGE && !suppressChange_) {
                        if (f == FSearch) { search_ = fieldText(FSearch); invalidate(); }
                        else { dirty_ = true; SetTimer(hwnd(), kSaveTimer, 700, nullptr); }
                    } else if (code == EN_KILLFOCUS && f != FSearch) {
                        commitFields();
                    }
                    return 0;
                }
                handled = false;
                return 0;
            }
            command(LOWORD(wp));
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            const Color t = pal_.ink, s = pal_.surface;
            SetTextColor(dc, RGB((int)(t.r * 255), (int)(t.g * 255), (int)(t.b * 255)));
            SetBkColor(dc, RGB((int)(s.r * 255), (int)(s.g * 255), (int)(s.b * 255)));
            return reinterpret_cast<LRESULT>(fieldBrush_);
        }
        case WM_SYSCOMMAND:
            // Alt on its own: no menu bar to go to, so don't enter menu mode.
            if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;
            handled = false;
            return 0;
        case WM_DPICHANGED:
            layoutFont();
            handled = false;
            return 0;
        case WM_CLOSE:
            commitFields();
            // Let the editor hand over its last few keystrokes before closing.
            if (editor_.ready() && !closePending_) {
                closePending_ = true;
                editor_.send(R"({"type":"flush"})");
                SetTimer(hwnd(), kCloseTimer, 1500, nullptr);
                return 0;
            }
            handled = false;
            return 0;
        default:
            if (msg == WM_APP_EDITION) {
                editionTypesetFinished((int)wp, reinterpret_cast<studio::typeset::Book*>(lp));
                return 0;
            }
            if (msg == WM_APP_TYPESET) {
                typesetFinished((int)wp, reinterpret_cast<studio::typeset::Book*>(lp));
                return 0;
            }
            if (msg == WM_APP_IMPORT) {
                importFinished(reinterpret_cast<studio::import::Plan*>(lp));
                return 0;
            }
            break;
    }
    handled = false;
    return 0;
}

// ------------------------------------------------------------- drag & drop

bool StudioApp::onMouseDown(float x, float y) {
    drag_ = {};
    // Cropping a picture in the details panel: a corner resizes, inside moves.
    if (focus_.kind == Focus::Picture && cropBox_.w > 0 && cropBox_.inset(-8).contains(x, y)) {
        const Rect& b = cropBox_;
        const Placement& p = pic_.place;
        const float l = b.x + b.w * (float)p.cropX, t = b.y + b.h * (float)p.cropY;
        const float rr = l + b.w * (float)p.cropW, bt = t + b.h * (float)p.cropH;
        auto closeTo = [&](float cx, float cy) { return std::abs(x - cx) <= 10 && std::abs(y - cy) <= 10; };
        int mode = 0;
        if (closeTo(l, t)) mode = 2;
        else if (closeTo(rr, t)) mode = 3;
        else if (closeTo(l, bt)) mode = 4;
        else if (closeTo(rr, bt)) mode = 5;
        else if (x >= l && x <= rr && y >= t && y <= bt) mode = 1;
        if (mode) {
            crop_ = {mode, x, y, pic_.place};
            return true;
        }
    }
    const std::string id = hitAt(x, y);
    auto take = [&](const char* prefix, DragState::Kind k) {
        if (id.rfind(prefix, 0) != 0) return false;
        std::string rest = id.substr(std::strlen(prefix));
        drag_.id = rest.substr(0, rest.find_first_of("|@"));
        drag_.kind = k;
        drag_.armed = true;
        drag_.sx = x;
        drag_.sy = y;
        return true;
    };
    take("node:", DragState::Node) || take("book:", DragState::Book) || take("side:book:", DragState::Book) || take("series:", DragState::Series) ||
        take("side:series:", DragState::Series) || take("side:world:", DragState::World);
    if (drag_.armed && lib_) {
        if (drag_.kind == DragState::Node) {
            const ManuscriptNode* n = lib_->node(drag_.id);
            if (n && !n->deleted) drag_.label = n->title.empty() ? w(label(n->kind)) : w(n->title);
            else drag_ = {};
        } else if (drag_.kind == DragState::Book) { if (auto* b = lib_->book(drag_.id)) drag_.label = w(b->title); }
        else if (drag_.kind == DragState::Series) { if (auto* s = lib_->seriesById(drag_.id)) drag_.label = w(s->name); }
        else if (auto* wd = lib_->world(drag_.id)) drag_.label = w(wd->name);
    }
    return false;  // the normal click handling still runs
}

void StudioApp::onMouseMove(float x, float y, bool leftDown) {
    if (crop_.mode && leftDown && cropBox_.w > 0) {
        const double dx = (x - crop_.sx) / cropBox_.w, dy = (y - crop_.sy) / cropBox_.h;
        Placement p = crop_.start;
        const double x0 = p.cropX, y0 = p.cropY, x1 = p.cropX + p.cropW, y1 = p.cropY + p.cropH;
        double nx0 = x0, ny0 = y0, nx1 = x1, ny1 = y1;
        switch (crop_.mode) {
            case 1: {
                const double mx = std::clamp(dx, -x0, 1 - x1), my = std::clamp(dy, -y0, 1 - y1);
                nx0 += mx; nx1 += mx; ny0 += my; ny1 += my;
                break;
            }
            case 2: nx0 = std::clamp(x0 + dx, 0.0, x1 - 0.05); ny0 = std::clamp(y0 + dy, 0.0, y1 - 0.05); break;
            case 3: nx1 = std::clamp(x1 + dx, x0 + 0.05, 1.0); ny0 = std::clamp(y0 + dy, 0.0, y1 - 0.05); break;
            case 4: nx0 = std::clamp(x0 + dx, 0.0, x1 - 0.05); ny1 = std::clamp(y1 + dy, y0 + 0.05, 1.0); break;
            case 5: nx1 = std::clamp(x1 + dx, x0 + 0.05, 1.0); ny1 = std::clamp(y1 + dy, y0 + 0.05, 1.0); break;
        }
        p.cropX = nx0; p.cropY = ny0; p.cropW = nx1 - nx0; p.cropH = ny1 - ny0;
        pic_.place = clampPlacement(p);
        invalidate();
        return;
    }
    if (!drag_.armed || !leftDown) return;
    if (!drag_.active && std::hypot(x - drag_.sx, y - drag_.sy) > 6) {
        drag_.active = true;
        cancelPress();
    }
    if (drag_.active) invalidate();
}

const StudioApp::Drop* StudioApp::dropAt(float x, float y) const {
    const Drop* found = nullptr;
    for (auto& d : lastDrops_) {
        if (!d.rect.contains(x, y)) continue;
        const bool fits = (drag_.kind == DragState::Book &&
                           (d.kind == Drop::ToWorld || d.kind == Drop::ToIndependent || d.kind == Drop::ToSeries ||
                            d.kind == Drop::ReorderSeriesBooks || d.kind == Drop::ReorderStandalone)) ||
                          (drag_.kind == DragState::Series && (d.kind == Drop::ReorderSeriesList || d.kind == Drop::ToWorld)) ||
                          (drag_.kind == DragState::World && d.kind == Drop::ReorderWorlds) ||
                          (drag_.kind == DragState::Node && d.kind == Drop::NodeRow && d.id != drag_.id);
        if (fits) found = &d;
    }
    return found;
}

bool StudioApp::onMouseUp(float x, float y) {
    if (crop_.mode) {
        crop_.mode = 0;
        sendPicture();   // the text shows the new crop
        return true;
    }
    const bool wasDrag = drag_.active;
    if (wasDrag) {
        if (const Drop* d = dropAt(x, y)) applyDrop(*d, x, y);
    } else if (drag_.armed) {
        // A click on a book: select it, or open it on a double-click.
        const std::string id = hitAt(x, y);
        const DWORD t = GetTickCount();
        if (id == lastClickId_ && t - lastClickTime_ <= GetDoubleClickTime() && drag_.kind == DragState::Book) {
            const Id book = drag_.id;
            drag_ = {};
            lastClickId_.clear();
            openWriting(book);
            return true;
        }
        lastClickId_ = id;
        lastClickTime_ = t;
    }
    drag_ = {};
    return wasDrag;
}

void StudioApp::applyDrop(const Drop& d, float x, float y) {
    commitFields();
    const Id moving = drag_.id;
    auto reordered = [&](std::vector<Id> list) {
        list.erase(std::remove(list.begin(), list.end(), moving), list.end());
        auto it = std::find(list.begin(), list.end(), d.id);
        const bool after = d.horizontal ? x > d.rect.x + d.rect.w / 2 : y > d.rect.y + d.rect.h / 2;
        if (it == list.end()) list.push_back(moving);
        else list.insert(after ? it + 1 : it, moving);
        return list;
    };
    switch (d.kind) {
        case Drop::ToWorld:
            if (drag_.kind == DragState::Book) moveBookTo(moving, d.id);
            else moveSeriesTo(moving, d.id);
            break;
        case Drop::ToIndependent: moveBookTo(moving, {}); break;
        case Drop::ToSeries: addToSeries(moving, d.id); break;
        case Drop::ReorderSeriesBooks:
            if (lib_->membership(d.listOwner, moving)) lib_->reorderSeriesBooks(d.listOwner, reordered(d.list), d.chrono);
            else addToSeries(moving, d.listOwner);
            break;
        case Drop::ReorderStandalone: {
            const Book* b = lib_->book(moving);
            if (b && b->worldId == d.listOwner && lib_->seriesOfBook(moving).empty()) lib_->reorderStandaloneBooks(reordered(d.list));
            else if (b && b->worldId != d.listOwner) moveBookTo(moving, d.listOwner);
            break;
        }
        case Drop::ReorderSeriesList: {
            const Series* s = lib_->seriesById(moving);
            if (s && s->worldId == d.listOwner) lib_->reorderSeries(reordered(d.list));
            break;
        }
        case Drop::ReorderWorlds: lib_->reorderWorlds(reordered(d.list)); break;
        case Drop::NodeRow: {
            // Middle of a part or chapter: move inside it. Top or bottom half: before or after it.
            const ManuscriptNode* target = lib_->node(d.id);
            const ManuscriptNode* moving2 = lib_->node(moving);
            if (!target || !moving2) break;
            const float third = d.rect.h / 3;
            const bool middle = y > d.rect.y + third && y < d.rect.bottom() - third;
            bool ok = false;
            if (middle && Library::canContain(target->kind, moving2->kind)) ok = lib_->moveNode(moving, target->id, -1);
            else {
                auto sibs = lib_->childrenOf(target->bookId, target->parentId);
                int idx = 0;
                for (int i = 0, k = 0; i < (int)sibs.size(); ++i) {
                    if (sibs[i]->id == moving) continue;
                    if (sibs[i]->id == target->id) idx = k;
                    ++k;
                }
                if (y > d.rect.y + d.rect.h / 2) ++idx;
                ok = lib_->moveNode(moving, target->parentId, idx);
            }
            if (!ok) flash(w(lib_->lastError()), 6);
            break;
        }
    }
    invalidate();
}

bool StudioApp::onRightClick(float x, float y) {
    const std::string id = hitAt(x, y);
    if (id.empty()) return false;
    contextMenuFor(id, x, y);
    return true;
}

bool StudioApp::onWheel(float x, float y, float lines) {
    if (ws_ == Workspace::Preview) {
        if (sideRect_.contains(x, y)) pv_.sideScroll = std::clamp(pv_.sideScroll + lines * 20, 0.0f, std::max(0.0f, pv_.sideContent - sideRect_.h));
        invalidate();
        return true;
    }
    if (ws_ == Workspace::Review) {
        if (sideRect_.contains(x, y)) rv_.sideScroll = std::clamp(rv_.sideScroll + lines * 20, 0.0f, std::max(0.0f, rv_.sideContent - sideRect_.h));
        else if (pageRect_.contains(x, y)) rv_.scroll = std::clamp(rv_.scroll + lines * 30, 0.0f, std::max(0.0f, rv_.content - pageRect_.h));
        invalidate();
        return true;
    }
    if (ws_ == Workspace::Planning) {
        if (panelRect_.contains(x, y)) pl_.panelScroll = std::clamp(pl_.panelScroll + lines * 20, 0.0f, std::max(0.0f, pl_.panelContent - panelRect_.h));
        else if (sideRect_.contains(x, y)) pl_.sideScroll = std::clamp(pl_.sideScroll + lines * 20, 0.0f, std::max(0.0f, pl_.sideContent - sideRect_.h));
        else if (pageRect_.contains(x, y)) pl_.scroll = std::clamp(pl_.scroll + lines * 30, 0.0f, std::max(0.0f, pl_.content - pageRect_.h));
        invalidate();
        return true;
    }
    if (ws_ == Workspace::Worldbuilding) {
        if (panelRect_.contains(x, y)) wb_.panelScroll = std::clamp(wb_.panelScroll + lines * 20, 0.0f, std::max(0.0f, wb_.panelContent - panelRect_.h));
        else if (sideRect_.contains(x, y)) wb_.sideScroll = std::clamp(wb_.sideScroll + lines * 20, 0.0f, std::max(0.0f, wb_.sideContent - sideRect_.h));
        else if (pageRect_.contains(x, y)) wb_.scroll = std::clamp(wb_.scroll + lines * 30, 0.0f, std::max(0.0f, wb_.content - pageRect_.h));
        invalidate();
        return true;
    }
    if (ws_ == Workspace::Publishing) {
        if (panelRect_.contains(x, y)) pb_.panelScroll = std::clamp(pb_.panelScroll + lines * 20, 0.0f, std::max(0.0f, pb_.panelContent - panelRect_.h));
        else if (sideRect_.contains(x, y)) pb_.sideScroll = std::clamp(pb_.sideScroll + lines * 20, 0.0f, std::max(0.0f, pb_.sideContent - sideRect_.h));
        else if (pageRect_.contains(x, y)) pb_.scroll = std::clamp(pb_.scroll + lines * 30, 0.0f, std::max(0.0f, pb_.content - pageRect_.h));
        invalidate();
        return true;
    }
    if (ws_ == Workspace::Typesetting) {
        if (sideRect_.contains(x, y)) ts_.sideScroll = std::clamp(ts_.sideScroll + lines * 20, 0.0f, std::max(0.0f, ts_.sideContent - sideRect_.h));
        else if (panelRect_.contains(x, y)) ts_.panelScroll = std::clamp(ts_.panelScroll + lines * 20, 0.0f, std::max(0.0f, ts_.panelContent - panelRect_.h));
        else if (pageRect_.contains(x, y)) {
            if (ts_.zoom > 0) {
                // Zoomed in: the wheel scrolls (Shift: sideways).
                if (GetKeyState(VK_SHIFT) & 0x8000) ts_.panX += lines * 40;
                else ts_.panY += lines * 40;
            } else {
                // The wheel turns the pages, one spread per notch.
                static float acc = 0;
                acc += lines;
                if (acc >= 3) { turnPages(1); acc = 0; }
                else if (acc <= -3) { turnPages(-1); acc = 0; }
            }
        }
        invalidate();
        return true;
    }
    if (sideRect_.contains(x, y)) {
        if (ws_ == Workspace::Writing)
            binderScroll_ = std::clamp(binderScroll_ + lines * 20, 0.0f, std::max(0.0f, binderContent_ - sideRect_.h));
        else
            sideScroll_ = std::clamp(sideScroll_ + lines * 20, 0.0f, std::max(0.0f, sideContent_ - sideRect_.h));
        return true;
    }
    if (settings_.showPanel && panelRect_.contains(x, y)) {
        panelScroll_ = std::clamp(panelScroll_ + lines * 20, 0.0f, std::max(0.0f, panelContent_ - panelRect_.h));
        return true;
    }
    return false;
}

// ----------------------------------------------------------- native fields

bool StudioApp::preTranslate(const MSG& msg) {
    if (msg.message != WM_KEYDOWN) return false;
    int current = -1;
    for (int f = 0; f < FieldCount; ++f) if (fields_[f] == msg.hwnd) current = f;
    if (current < 0) return false;
    const bool multi = current == FDescription || current == FNotes || current == FSynopsis;
    if (msg.wParam == VK_TAB) {
        // Visible fields in reading order: top to bottom, then left to right.
        std::vector<int> order;
        for (int f = 0; f < FieldCount; ++f) if (IsWindowVisible(fields_[f])) order.push_back(f);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            if (std::abs(fieldRects_[a].top - fieldRects_[b].top) > 4) return fieldRects_[a].top < fieldRects_[b].top;
            return fieldRects_[a].left < fieldRects_[b].left;
        });
        auto it = std::find(order.begin(), order.end(), current);
        if (it == order.end() || order.empty()) return true;
        const bool back = GetKeyState(VK_SHIFT) & 0x8000;
        long i = (it - order.begin()) + (back ? -1 : 1);
        if (i < 0 || i >= (long)order.size()) { commitFields(); SetFocus(hwnd()); invalidate(); return true; }
        SetFocus(fields_[order[i]]);
        SendMessageW(fields_[order[i]], EM_SETSEL, 0, -1);
        invalidate();
        return true;
    }
    if (msg.wParam == VK_RETURN && current == FName && focus_.kind == Focus::Node && ws_ == Workspace::Writing && editor_.ready()) {
        // Naming a chapter, then straight on to writing it.
        commitFields();
        editor_.focus();
        editor_.send(R"({"type":"focus"})");
        invalidate();
        return true;
    }
    if (msg.wParam == VK_ESCAPE || (msg.wParam == VK_RETURN && !multi)) {
        commitFields();
        if (current == FSearch && msg.wParam == VK_ESCAPE) { setFieldText(FSearch, ""); search_.clear(); }
        SetFocus(hwnd());
        invalidate();
        return true;
    }
    return false;
}

void StudioApp::createFields() {
    for (int f = 0; f < FieldCount; ++f) {
        const bool multi = f == FDescription || f == FNotes || f == FSynopsis;
        DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (f == FTarget ? ES_NUMBER : 0);
        if (multi) style = WS_CHILD | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
        fields_[f] = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, hwnd(), nullptr, GetModuleHandleW(nullptr), nullptr);
        // No scroll bar on the longer fields; the wheel still scrolls them.
        if (multi) SetWindowSubclass(fields_[f], wheelScroll, 1, 0);
        SendMessageW(fields_[f], EM_SETLIMITTEXT, multi ? 200000 : 2000, 0);
    }
    SendMessageW(fields_[FSearch], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search titles, authors, tags"));
    SendMessageW(fields_[FTags], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"fantasy, dragons"));
    SendMessageW(fields_[FAccent], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"#2D4A6E"));
    SendMessageW(fields_[FNumber], EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Unnumbered"));
    layoutFont();
}

void StudioApp::layoutFont() {
    if (fieldFont_) DeleteObject(fieldFont_);
    const UINT dpi = GetDpiForWindow(hwnd()) ? GetDpiForWindow(hwnd()) : 96;
    fieldFont_ = CreateFontW(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (HWND h : fields_) if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(fieldFont_), TRUE);
}

void StudioApp::placeField(Field f, const Rect& r) {
    const float s = gfx_.dpiScale();
    const bool multi = f == FDescription || f == FNotes || f == FSynopsis;
    // The text sits inside a drawn frame; the control itself is borderless.
    const RECT rc{(LONG)std::lround((r.x + 8) * s), (LONG)std::lround((r.y + (multi ? 6 : (r.h - 20) / 2)) * s),
                  (LONG)std::lround((r.right() - 6) * s), (LONG)std::lround((r.bottom() - (multi ? 6 : (r.h - 20) / 2)) * s)};
    placed_[f] = true;
    if (memcmp(&rc, &fieldRects_[f], sizeof rc) != 0 || !IsWindowVisible(fields_[f])) {
        fieldRects_[f] = rc;
        SetWindowPos(fields_[f], nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void StudioApp::hideUnplacedFields() {
    for (int f = 0; f < FieldCount; ++f) {
        if (!placed_[f] && IsWindowVisible(fields_[f])) {
            if (GetFocus() == fields_[f]) SetFocus(hwnd());
            ShowWindow(fields_[f], SW_HIDE);
        }
        placed_[f] = false;
    }
}

void StudioApp::setFieldText(Field f, const std::string& utf8) {
    suppressChange_ = true;
    SetWindowTextW(fields_[f], w(utf8).c_str());
    suppressChange_ = false;
}

std::string StudioApp::fieldText(Field f) const {
    const int n = GetWindowTextLengthW(fields_[f]);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(fields_[f], s.data(), n + 1);
    std::string out = narrow(s);
    // Multi-line edit controls use CRLF; store plain LF.
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    return out;
}

static std::string withCrLf(const std::string& s) {
    std::string out;
    for (char c : s) { if (c == '\n') out += '\r'; out += c; }
    return out;
}

void StudioApp::loadFields() {
    loadedFocus_ = focus_;
    dirty_ = false;
    if (!lib_) return;
    for (int f = 0; f < FieldCount; ++f) if (f != FSearch) setFieldText((Field)f, "");
    // The tags field doubles as an entry's other names.
    SendMessageW(fields_[FTags], EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(focus_.kind == Focus::Entry ? L"the Captain, Mara" : L"fantasy, dragons"));
    if (focus_.kind == Focus::World) {
        if (const World* x = lib_->world(focus_.id)) {
            setFieldText(FName, x->name);
            setFieldText(FDescription, withCrLf(x->description));
            setFieldText(FNotes, withCrLf(x->notes));
            setFieldText(FAccent, x->accent);
        }
    } else if (focus_.kind == Focus::Series) {
        if (const Series* x = lib_->seriesById(focus_.id)) {
            setFieldText(FName, x->name);
            setFieldText(FDescription, withCrLf(x->description));
            setFieldText(FNotes, withCrLf(x->notes));
        }
    } else if (focus_.kind == Focus::Node) {
        if (const ManuscriptNode* n = lib_->node(focus_.id)) {
            setFieldText(FName, n->title);
            setFieldText(FSynopsis, withCrLf(n->synopsis));
            setFieldText(FTarget, n->targetWords > 0 ? std::to_string(n->targetWords) : std::string());
        }
    } else if (focus_.kind == Focus::Comment) {
        if (const Comment* c = lib_->comment(focus_.id)) setFieldText(FDescription, withCrLf(c->body));
    } else if (focus_.kind == Focus::Picture) {
        setFieldText(FCaption, pic_.caption);
        setFieldText(FAlt, pic_.alt);
    } else if (focus_.kind == Focus::Snapshot) {
        for (const Snapshot& s : lib_->snapshotsOf(rv_.book)) if (s.id == focus_.id) setFieldText(FName, s.name);
    } else if (focus_.kind == Focus::Goal) {
        const std::int64_t g = dailyGoal();
        setFieldText(FTarget, g > 0 ? std::to_string(g) : std::string());
    } else if (focus_.kind == Focus::Plan) {
        const std::int64_t t = bookTarget(focus_.id);
        setFieldText(FTarget, t > 0 ? std::to_string(t) : std::string());
    } else if (focus_.kind == Focus::Entry) {
        if (const Entry* e = lib_->entry(focus_.id)) {
            setFieldText(FName, e->name);
            std::string al;
            for (auto& a : e->aliases) al += (al.empty() ? "" : ", ") + a;
            setFieldText(FTags, al);
            setFieldText(FDescription, withCrLf(e->summary));
            setFieldText(FSynopsis, withCrLf(e->details));
            setFieldText(FNotes, withCrLf(e->notes));
        }
    } else if (focus_.kind == Focus::Edition) {
        if (const Edition* e = lib_->edition(focus_.id)) {
            setFieldText(FName, e->name);
            setFieldText(FIsbn, e->isbn);
        }
    } else if (focus_.kind == Focus::Artwork) {
        if (const Artwork* a = lib_->artwork(focus_.id)) {
            setFieldText(FName, a->name);
            setFieldText(FNotes, withCrLf(a->notes));
        }
    } else if (focus_.kind == Focus::Book) {
        if (const Book* b = lib_->book(focus_.id)) {
            setFieldText(FName, b->title);
            setFieldText(FSubtitle, b->subtitle);
            setFieldText(FAuthor, b->author);
            setFieldText(FPenName, b->penName);
            setFieldText(FGenre, b->genre);
            setFieldText(FLanguage, b->language);
            setFieldText(FIsbn, b->isbn);
            setFieldText(FEdition, b->edition);
            std::string tags;
            for (auto& t : b->tags) tags += (tags.empty() ? "" : ", ") + t;
            setFieldText(FTags, tags);
            setFieldText(FSynopsis, withCrLf(b->synopsis));
            if (const Membership* m = lib_->membership(focus_.contextSeries, b->id)) setFieldText(FNumber, m->number);
        }
    }
}

void StudioApp::commitFields() {
    KillTimer(hwnd(), kSaveTimer);
    if (!dirty_ || !lib_) return;
    dirty_ = false;
    const Focus f = loadedFocus_;
    bool ok = true;
    if (f.kind == Focus::World) {
        if (const World* x = lib_->world(f.id)) {
            World n = *x;
            n.name = fieldText(FName);
            n.description = fieldText(FDescription);
            n.notes = fieldText(FNotes);
            const std::string a = fieldText(FAccent);
            if (a.empty() || (a.size() == 7 && a[0] == '#' && a.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos))
                n.accent = a;
            ok = lib_->updateWorld(n);
        }
    } else if (f.kind == Focus::Series) {
        if (const Series* x = lib_->seriesById(f.id)) {
            Series n = *x;
            n.name = fieldText(FName);
            n.description = fieldText(FDescription);
            n.notes = fieldText(FNotes);
            ok = lib_->updateSeries(n);
        }
    } else if (f.kind == Focus::Comment) {
        if (lib_->comment(f.id)) ok = lib_->updateComment(f.id, fieldText(FDescription));
    } else if (f.kind == Focus::Picture) {
        pic_.caption = fieldText(FCaption);
        pic_.alt = fieldText(FAlt);
        sendPicture();
    } else if (f.kind == Focus::Snapshot) {
        const std::string nm = fieldText(FName);
        if (!nm.empty()) ok = lib_->renameSnapshot(f.id, nm);
    } else if (f.kind == Focus::Goal) {
        const std::string t = fieldText(FTarget);
        const std::int64_t g = t.empty() ? 0 : std::strtoll(t.c_str(), nullptr, 10);
        lib_->setSetting("goal:daily", g > 0 ? std::to_string(g) : std::string());
    } else if (f.kind == Focus::Plan) {
        if (lib_->book(f.id)) {
            const std::string t = fieldText(FTarget);
            setBookTarget(f.id, t.empty() ? 0 : std::strtoll(t.c_str(), nullptr, 10));
        }
    } else if (f.kind == Focus::Entry) {
        if (const Entry* x = lib_->entry(f.id)) {
            Entry n = *x;
            n.name = fieldText(FName);
            if (n.name.empty()) n.name = x->name;   // never nameless
            n.aliases.clear();
            const std::string al = fieldText(FTags);
            size_t at = 0;
            while (at <= al.size()) {
                size_t c = al.find(',', at);
                if (c == std::string::npos) c = al.size();
                n.aliases.push_back(al.substr(at, c - at));
                at = c + 1;
            }
            n.summary = fieldText(FDescription);
            n.details = fieldText(FSynopsis);
            n.notes = fieldText(FNotes);
            ok = lib_->updateEntry(n);
        }
    } else if (f.kind == Focus::Edition) {
        if (const Edition* x = lib_->edition(f.id)) {
            Edition n = *x;
            n.name = fieldText(FName);
            n.isbn = fieldText(FIsbn);
            const bool changed = n.name != x->name || n.isbn != x->isbn;
            ok = lib_->updateEdition(n);
            if (ok && changed && ws_ == Workspace::Publishing) editionChanged();
        }
    } else if (f.kind == Focus::Artwork) {
        if (const Artwork* x = lib_->artwork(f.id)) {
            Artwork n = *x;
            n.name = fieldText(FName);
            n.notes = fieldText(FNotes);
            ok = lib_->updateArtwork(n);
        }
    } else if (f.kind == Focus::Node) {
        if (const ManuscriptNode* x = lib_->node(f.id)) {
            ManuscriptNode n = *x;
            n.title = fieldText(FName);
            n.synopsis = fieldText(FSynopsis);
            {
                const std::string t = fieldText(FTarget);
                n.targetWords = t.empty() ? 0 : std::strtoll(t.c_str(), nullptr, 10);
            }
            ok = lib_->updateNode(n);
            if (ok && n.id == currentNode_ && editor_.ready())
                editor_.send(json{{"type", "title"}, {"title", n.title}}.dump());
        }
    } else if (f.kind == Focus::Book) {
        if (const Book* b = lib_->book(f.id)) {
            Book n = *b;
            n.title = fieldText(FName);
            n.subtitle = fieldText(FSubtitle);
            n.author = fieldText(FAuthor);
            n.penName = fieldText(FPenName);
            n.genre = fieldText(FGenre);
            n.language = fieldText(FLanguage);
            n.isbn = fieldText(FIsbn);
            n.edition = fieldText(FEdition);
            n.synopsis = fieldText(FSynopsis);
            n.tags.clear();
            std::stringstream ss(fieldText(FTags));
            std::string t;
            while (std::getline(ss, t, ',')) n.tags.push_back(t);
            ok = lib_->updateBook(n);
            if (ok && !f.contextSeries.empty())
                if (const Membership* m = lib_->membership(f.contextSeries, f.id)) {
                    Membership x = *m;
                    x.number = fieldText(FNumber);
                    ok = lib_->updateMembership(x);
                }
        }
    }
    saveFailed_ = !ok;  // the status bar shows Saved (or that it wasn't)
    if (!ok) flash(w("Couldn't save: " + lib_->lastError()), 10);
    invalidate();
}

}  // namespace nrs

// NorthRay Capture's window: the media library (top bar › Library), the suite's one library
// (%LOCALAPPDATA%\NorthRay\Library: library.json and files\; cj/medialib). Add files (copied in), folders,
// tags and search; anything in it goes into the current scene as the right source, and overlay designs can be
// saved into it from the designer. Other NorthRay apps may change it, so it's read again when its file changes.
#include "capture_window.hpp"

#include "log.hpp"

#include "cj/medialib.hpp"
#include "ncap/design.hpp"
#include "video/design_render.hpp"

#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include <nlohmann/json.hpp>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include "video/sources.hpp"

namespace ncap {

using cjui::Align;
using cjui::Color;
using cjui::Font;
using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
using nlohmann::json;
namespace ml = cj::medialib;
namespace fs = std::filesystem;

namespace {
std::wstring libraryRoot() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) out = std::wstring(p) + L"\\NorthRay\\Library";
    CoTaskMemFree(p);
    return out;
}
std::wstring filesDir() { return libraryRoot() + L"\\files"; }
std::wstring indexPath() { return libraryRoot() + L"\\library.json"; }
std::wstring thumbsDir() { return libraryRoot() + L"\\thumbs"; }

bool readAll(const std::wstring& path, std::string& out) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
bool writeAll(const std::wstring& path, const std::string& bytes) {
    std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), (std::streamsize)bytes.size());
    return (bool)f;
}

// A .nrpack to save or open (the file dialogs).
bool pickPack(HWND owner, bool save, const std::wstring& suggested, std::wstring& out) {
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    const COMDLG_FILTERSPEC spec[] = {{L"NorthRay template packs", L"*.nrpack"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(L"nrpack");
    if (save) dlg->SetFileName(suggested.c_str());
    dlg->SetTitle(save ? L"Save the template pack" : L"Open a template pack");
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                CoTaskMemFree(p);
                ok = true;
            }
            it->Release();
        }
    }
    dlg->Release();
    return ok;
}

std::string today() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[16];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return b;
}

bool pickFiles(HWND owner, std::vector<std::wstring>& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    const COMDLG_FILTERSPEC spec[] = {
        {L"Pictures, videos, sounds and designs",
         L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.tif;*.tiff;*.mp4;*.mov;*.mkv;*.avi;*.wmv;*.webm;*.m4v;*.mp3;*.wav;*.flac;*.ogg;*.m4a;*.aac;*.wma;*.opus;*.nrdesign"},
        {L"Everything", L"*.*"}};
    dlg->SetFileTypes(2, spec);
    dlg->SetTitle(L"Add to the NorthRay library (copies are kept there)");
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* it = nullptr;
                if (FAILED(items->GetItemAt(i, &it))) continue;
                PWSTR path = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out.push_back(path);
                    CoTaskMemFree(path);
                }
                it->Release();
            }
            items->Release();
            ok = !out.empty();
        }
    }
    dlg->Release();
    return ok;
}

// What a card without a picture says (plain words: not every font has symbols).
const wchar_t* kindWord(ml::Kind k) {
    switch (k) {
        case ml::Kind::Picture: return L"PICTURE";
        case ml::Kind::Video: return L"VIDEO";
        case ml::Kind::Music: return L"MUSIC";
        case ml::Kind::Sound: return L"SOUND";
        case ml::Kind::Design: return L"OVERLAY DESIGN";
        default: return L"FILE";
    }
}
}  // namespace

// Read library.json when it changed (another app may have added to it).
void CaptureWindow::loadLibrary(bool force) {
    std::error_code ec;
    const auto t = fs::last_write_time(fs::path(indexPath()), ec);
    const long long stamp = ec ? 0 : (long long)t.time_since_epoch().count();
    if (!force && stamp == libStamp_) return;
    libStamp_ = stamp;
    ml::Library l;
    if (stamp) {
        std::ifstream f(fs::path(indexPath()), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        if (!ml::Library::fromJson(ss.str(), l)) {
            log::warn("Library: library.json couldn't be read; leaving it as it is");
            libBroken_ = true;
            return;
        }
    }
    libBroken_ = false;
    library_ = l;
}

bool CaptureWindow::saveLibrary() {
    if (libBroken_) {
        flash(L"The library's list couldn't be read, so it isn't changed (see the log).", 6);
        return false;
    }
    std::error_code ec;
    fs::create_directories(fs::path(filesDir()), ec);
    const std::wstring tmp = indexPath() + L".tmp";
    {
        std::ofstream f(fs::path(tmp), std::ios::binary | std::ios::trunc);
        f << library_.toJson();
        if (!f) return false;
    }
    if (!MoveFileExW(tmp.c_str(), indexPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
    const auto t = fs::last_write_time(fs::path(indexPath()), ec);
    libStamp_ = ec ? 0 : (long long)t.time_since_epoch().count();
    return true;
}

void CaptureWindow::openLibrary() {
    loadLibrary(true);
    // Back in the folder you were in last time, if it's still there.
    const auto fl = library_.folders();
    libFolder_ = std::find(fl.begin(), fl.end(), settings_.libraryFolder) != fl.end() ? settings_.libraryFolder : std::string();
    libSizes_.clear();   // sizes read afresh
    openSettings(true);
    libraryOpen_ = true;
    invalidate();
}

void CaptureWindow::libraryAddFiles() {
    std::vector<std::wstring> picked;
    if (!pickFiles(hwnd(), picked)) return;
    std::error_code ec;
    fs::create_directories(fs::path(filesDir()), ec);
    int added = 0;
    std::wstring failed;
    for (auto& p : picked) {
        const std::string stored = library_.storedName(narrow(fs::path(p).filename().wstring()));
        const std::wstring to = filesDir() + L"\\" + widen(stored);
        if (!CopyFileW(p.c_str(), to.c_str(), TRUE)) {
            failed = fs::path(p).filename().wstring();
            continue;
        }
        const ml::Kind kind = ml::kindOf(stored);
        library_.add(stored, narrow(fs::path(p).stem().wstring()), kind, libFolder_, today());
        ++added;
    }
    if (added && saveLibrary()) {
        log::info("Library: " + std::to_string(added) + " file(s) added");
        flash(L"Added " + std::to_wstring(added) + (added == 1 ? L" file" : L" files") + L" to the library." +
                  (failed.empty() ? L"" : L" “" + failed + L"” couldn't be copied."),
              5);
    } else if (!failed.empty()) {
        flash(L"“" + failed + L"” couldn't be copied into the library.", 6);
    }
    invalidate();
}

// An item into the current scene, as the source that suits it.
void CaptureWindow::libraryUse(const std::string& id) {
    const ml::Item* it = library_.item(id);
    if (!it) return;
    const std::wstring path = filesDir() + L"\\" + widen(it->file);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        flash(L"Its file is missing from the library folder.", 5);
        return;
    }
    libraryOpen_ = false;
    openSettings(false);
    switch (it->kind) {
        case ml::Kind::Picture: addCapture(SourceKind::Image, it->name, json{{"file", narrow(path)}}.dump()); break;
        case ml::Kind::Video:
        case ml::Kind::Music:
        case ml::Kind::Sound: {
            const bool loop = it->kind != ml::Kind::Sound;
            addCapture(SourceKind::Media, it->name,
                       json{{"files", {narrow(path)}}, {"loop", loop}, {"shuffle", false}, {"volume", 1.0}, {"muted", false}}.dump());
            break;
        }
        case ml::Kind::Design: {
            std::ifstream f(fs::path(path), std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            design::Document d = design::fromJson(ss.str());
            if (d.elements.empty() && ss.str().find("\"elements\"") == std::string::npos) {
                flash(L"That design file couldn't be read.", 5);
                return;
            }
            addCapture(SourceKind::Design, it->name, design::toJson(d));
            break;
        }
        case ml::Kind::Other: flash(L"Capture can't show that kind of file.", 5); return;
    }
    log::info("Library item added to the scene: " + it->name);
}

// The designer's "Save to the library": the design as a .nrdesign file in "Overlay designs".
void CaptureWindow::librarySaveDesign() {
    loadLibrary(false);
    std::error_code ec;
    fs::create_directories(fs::path(filesDir()), ec);
    const std::string stored = library_.storedName((designDoc_.name.empty() ? std::string("Design") : designDoc_.name) + ".nrdesign");
    {
        std::ofstream f(fs::path(filesDir() + L"\\" + widen(stored)), std::ios::binary | std::ios::trunc);
        f << design::toJson(designDoc_);
        if (!f) {
            flash(L"The design couldn't be saved into the library.", 5);
            return;
        }
    }
    library_.add(stored, designDoc_.name, ml::Kind::Design, "Overlay designs", today());
    if (saveLibrary()) flash(L"Saved to the library, in Overlay designs.", 4);
}

void CaptureWindow::libraryItemMenu(const std::string& id) {
    const ml::Item* it = library_.item(id);
    if (!it) return;
    enum : UINT { Use = 1, Rename, Tag, Show, Remove, Kind0 = 100, Folder0 = 200, NewFolder = 199, Untag0 = 600 };
    std::vector<MenuItem> kinds, folders, untag;
    for (int k = 0; k < 6; ++k) kinds.push_back(MenuItem::item(widen(ml::kindName((ml::Kind)k)), Kind0 + (UINT)k, {}, true, (int)it->kind == k));
    const auto fl = library_.folders();
    folders.push_back(MenuItem::item(L"(the top)", Folder0, {}, true, it->folder.empty()));
    for (std::size_t i = 0; i < fl.size() && i < 300; ++i) folders.push_back(MenuItem::item(widen(fl[i]), Folder0 + 1 + (UINT)i, {}, true, it->folder == fl[i]));
    folders.push_back(MenuItem::sep());
    folders.push_back(MenuItem::item(L"A new folder…", NewFolder));
    for (std::size_t i = 0; i < it->tags.size(); ++i) untag.push_back(MenuItem::item(widen(it->tags[i]), Untag0 + (UINT)i));
    std::vector<MenuItem> items = {
        MenuItem::item(L"Add to the scene", Use, widen(ml::kindName(it->kind))),
        MenuItem::sep(),
        MenuItem::item(L"Rename…", Rename),
        MenuItem::sub(L"Kind", kinds),
        MenuItem::sub(L"Move to", folders),
        MenuItem::item(L"Add a tag…", Tag),
    };
    if (!untag.empty()) items.push_back(MenuItem::sub(L"Remove a tag", untag));
    items.push_back(MenuItem::item(L"Show the file", Show));
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"Remove from the library", Remove, L"to the Recycle Bin"));
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (!cmd) return;
    loadLibrary(false);   // the newest list (another app may have changed it)
    it = library_.item(id);
    if (!it) return;
    const std::wstring path = filesDir() + L"\\" + widen(it->file);
    auto done = [this] {
        saveLibrary();
        invalidate();
    };
    if (cmd == Use) libraryUse(id);
    else if (cmd == Rename) {
        askText(L"Rename", widen(it->name), [this, id, done](const std::wstring& v) {
            if (library_.rename(id, narrow(v))) done();
        });
    } else if (cmd == Tag) {
        askText(L"A tag (like alvora, cover or holding)", L"", [this, id, done](const std::wstring& v) {
            if (library_.addTag(id, narrow(v))) done();
        });
    } else if (cmd >= Untag0 && cmd < Untag0 + it->tags.size()) {
        if (library_.removeTag(id, it->tags[cmd - Untag0])) done();
    } else if (cmd >= Kind0 && cmd < Kind0 + 6) {
        if (library_.setKind(id, (ml::Kind)(cmd - Kind0))) done();
    } else if (cmd == Folder0) {
        if (library_.moveTo(id, "")) done();
    } else if (cmd > Folder0 && cmd <= Folder0 + fl.size()) {
        if (library_.moveTo(id, fl[cmd - Folder0 - 1])) done();
    } else if (cmd == NewFolder) {
        askText(L"The new folder (use / for one inside another)", widen(it->folder.empty() ? std::string() : it->folder + "/"),
                [this, id, done](const std::wstring& v) {
                    const std::string f = ml::Library::cleanFolder(narrow(v));
                    if (f.empty()) return;
                    library_.addFolder(f);
                    library_.moveTo(id, f);
                    done();
                });
    } else if (cmd == Show) {
        const std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(hwnd(), L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (cmd == Remove) {
        const std::wstring name = widen(it->name);
        if (cjui::messageBox(hwnd(), L"Remove “" + name + L"” from the library? Its file goes to the Recycle Bin; scenes using it lose it.",
                             L"Library", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
        std::wstring from = path + L'\0';
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        SHFileOperationW(&op);
        library_.remove(id);
        done();
        flash(L"Removed “" + name + L"” (its file is in the Recycle Bin).", 5);
    }
}

void CaptureWindow::drawLibrary(const Rect& area) {
    loadLibrary(false);
    gfx_.fill(area, pal_.paper);
    libRows_.clear();
    // The side: kinds, folders, tags.
    const float side = 230;
    const Rect sideR{area.x, area.y, side, area.h};
    gfx_.fill(sideR, pal_.ink.withAlpha(0.03f));
    float sy = area.y + 20;
    auto sideRow = [&](const std::string& hid, const std::wstring& label, bool on, float indent, std::function<void()> act) {
        const Rect r{area.x + 10, sy, side - 20, 30};
        addHit(hid, r, std::move(act));
        if (on) gfx_.fillRounded(r, 6, pal_.ribbon.withAlpha(0.18f));
        else gfx_.fillRounded(r, 6, pal_.ink.withAlpha(0.05f * hoverAmount(hid)));
        gfx_.text(label, {r.x + 10 + indent, r.y, r.w - 14 - indent, r.h}, Font::Body, on ? pal_.ink : pal_.inkMuted, {Align::Left, false, true, true});
        sy += 32;
    };
    auto heading = [&](const wchar_t* t) {
        sy += 10;
        gfx_.text(t, {area.x + 20, sy, side - 30, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
        sy += 24;
    };
    const bool everything = libKind_ < 0 && libFolder_.empty() && libTag_.empty();
    sideRow("lib:all", L"Everything", everything, 0, [this] {
        libKind_ = -1, libFolder_.clear(), libTag_.clear();
        invalidate();
    });
    heading(L"KINDS");
    const wchar_t* kindLabels[] = {L"Pictures", L"Videos", L"Music", L"Sounds", L"Overlay designs"};
    for (int k = 0; k < 5; ++k)
        sideRow("lib:kind:" + std::to_string(k), kindLabels[k], libKind_ == k, 0, [this, k] {
            libKind_ = libKind_ == k ? -1 : k;
            invalidate();
        });
    heading(L"FOLDERS");
    for (auto& f : library_.folders()) {
        const int depth = (int)std::count(f.begin(), f.end(), '/');
        const auto slash = f.find_last_of('/');
        sideRow("lib:folder:" + f, widen(slash == std::string::npos ? f : f.substr(slash + 1)), libFolder_ == f, 14.0f * depth, [this, f] {
            libFolder_ = libFolder_ == f ? std::string() : f;
            invalidate();
        });
    }
    sideRow("lib:newfolder", L"+ New folder…", false, 0, [this] {
        askText(L"The new folder (use / for one inside another)", widen(libFolder_.empty() ? std::string() : libFolder_ + "/"), [this](const std::wstring& v) {
            loadLibrary(false);
            if (library_.addFolder(narrow(v))) {
                libFolder_ = ml::Library::cleanFolder(narrow(v));
                saveLibrary();
            }
            invalidate();
        });
    });
    const auto tags = library_.tags();
    if (!tags.empty()) {
        heading(L"TAGS");
        for (auto& [t, n] : tags)
            sideRow("lib:tag:" + t, L"#" + widen(t) + L"  " + std::to_wstring(n), libTag_ == t, 0, [this, t] {
                libTag_ = libTag_ == t ? std::string() : t;
                invalidate();
            });
    }

    // The main part: the heading, search and buttons, then the cards.
    const Rect main{area.x + side, area.y, area.w - side, area.h};
    const float pad = 24, x = main.x + pad, w = main.w - 2 * pad;
    const float top = beginScroll(main);
    float y = top + 20;
    gfx_.text(L"Library", {x, y, 300, 34}, Font::Title, pal_.ink, {Align::Left, false});
    button("lib:close", {x + w - 90, y + 2, 90, 32}, L"Close", [this] {
        libraryOpen_ = false;
        openSettings(false);
    });
    button("lib:add", {x + w - 250, y + 2, 150, 32}, L"Add files…", [this] { libraryAddFiles(); }, true);
    button("lib:import", {x + w - 410, y + 2, 150, 32}, L"Import a pack…", [this] { libraryImportPack(); });
    button("lib:export", {x + w - 570, y + 2, 150, 32}, libFolder_.empty() ? L"Export all…" : L"Export folder…", [this] { libraryExportPack(); });
    y += 44;
    gfx_.text(L"One library for all the NorthRay apps. Files you add are copied in, so they stay put. Right-click one to add it to "
              L"the scene, tag it or move it.",
              {x, y, w, 40}, Font::Small, pal_.inkMuted);
    y += 30;
    button("lib:search", {x, y, std::min(w, 420.0f), 34}, libQuery_.empty() ? L"Search the library…" : L"Search: " + widen(libQuery_), [this] {
        askText(L"Search (every word must match a name, folder or tag)", widen(libQuery_), [this](const std::wstring& v) {
            libQuery_ = narrow(v);
            invalidate();
        });
    });
    if (!libQuery_.empty())
        button("lib:clearsearch", {x + std::min(w, 420.0f) + 8, y, 80, 34}, L"Clear", [this] {
            libQuery_.clear();
            invalidate();
        });
    y += 50;
    // The folder you're in is remembered for next time (0.10.31).
    if (libFolder_ != settings_.libraryFolder) {
        settings_.libraryFolder = libFolder_;
        settings_.save(settingsPath_);
    }
    const auto found = library_.search({libQuery_, libKind_, libFolder_, libTag_});
    if (libBroken_) y += gfx_.text(L"The library's list (library.json) couldn't be read. Nothing is changed until it can be.", {x, y, w, 40}, Font::Body, pal_.live) + 10;
    if (found.empty()) {
        y += gfx_.text(library_.items().empty() ? L"Nothing here yet. Add files, or save a design from the overlay designer."
                                                : L"Nothing matches. Clear the search or choose Everything.",
                       {x, y, w, 40}, Font::Body, pal_.inkFaint) + 10;
    }
    const float cw = 190, chh = 190, gap = 14;
    const int cols = std::max(1, (int)((w + gap) / (cw + gap)));
    for (std::size_t i = 0; i < found.size(); ++i) {
        const ml::Item& it = *found[i];
        const float cx = x + (float)(i % cols) * (cw + gap), cy = y + (float)(i / cols) * (chh + gap);
        const Rect card{cx, cy, cw, chh};
        const std::string hid = "lib:item:" + it.id;
        const std::string id = it.id;
        addHit(hid, card, [this, id] { libraryUse(id); });
        libRows_.push_back({it.id, card});
        const float hv = hoverAmount(hid);
        gfx_.fillRounded(card, 10, pal_.ink.withAlpha(0.05f + 0.05f * hv));
        const Rect thumb{cx + 8, cy + 8, cw - 16, 120};
        bool drawn = false;
        if (it.kind == ml::Kind::Picture) {
            gfx_.fillRounded(thumb, 8, pal_.ink.withAlpha(0.06f));
            drawn = gfx_.drawImage(filesDir() + L"\\" + widen(it.file), thumb, false);   // the whole picture
        }
        if (it.kind == ml::Kind::Design) {
            const std::wstring t = libraryThumb(it);
            if (!t.empty()) {
                gfx_.fillRounded(thumb, 8, pal_.ink.withAlpha(0.06f));
                drawn = gfx_.drawImage(t, thumb, false);
            }
        }
        if (!drawn) {
            gfx_.fillRounded(thumb, 8, pal_.ribbon.withAlpha(0.12f));
            gfx_.text(kindWord(it.kind), thumb, Font::BodyStrong, pal_.ribbon, {Align::Center, false, true, true});
        }
        gfx_.text(widen(it.name), {cx + 10, cy + 134, cw - 20, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true, true});
        std::wstring sub = widen(ml::kindName(it.kind));
        // Its length (videos and sounds, found in the background) and size.
        const std::wstring path = filesDir() + L"\\" + widen(it.file);
        if (it.kind == ml::Kind::Video || it.kind == ml::Kind::Sound || it.kind == ml::Kind::Music) {
            const double len = libraryLength(path);
            if (len > 0) {
                wchar_t t[32];
                const int sec = (int)std::lround(len);
                if (sec >= 3600) swprintf(t, 32, L"%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
                else swprintf(t, 32, L"%d:%02d", sec / 60, sec % 60);
                sub += L" · " + std::wstring(t);
            }
        }
        if (const std::uint64_t bytes = librarySize(path)) sub += L" · " + sizeWords(bytes);
        if (!it.folder.empty()) sub += L" · " + widen(it.folder);
        for (auto& t : it.tags) sub += L" #" + widen(t);
        gfx_.text(sub, {cx + 10, cy + 158, cw - 20, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
    }
    if (!found.empty()) y += (float)((found.size() + cols - 1) / cols) * (chh + gap);
    y += 10;
    gfx_.text(L"Click to add to the scene · right-click for more", {x, y, w, 20}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
    y += 30;
    endScroll(y - top + 20);
}

// A file's size, read once per file (and again when the Library is reopened).
std::uint64_t CaptureWindow::librarySize(const std::wstring& path) {
    auto it = libSizes_.find(path);
    if (it != libSizes_.end()) return it->second;
    std::error_code ec;
    const auto n = fs::file_size(fs::path(path), ec);
    return libSizes_[path] = ec ? 0 : (std::uint64_t)n;
}

// "4.2 MB"
std::wstring CaptureWindow::sizeWords(std::uint64_t b) {
    wchar_t t[32];
    if (b < 1024) swprintf(t, 32, L"%llu bytes", (unsigned long long)b);
    else if (b < 1024 * 1024) swprintf(t, 32, L"%.0f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) swprintf(t, 32, L"%.1f MB", b / (1024.0 * 1024));
    else swprintf(t, 32, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    return t;
}

// A video's or sound's length: found once, on a worker thread, then the Library redraws. 0 until then.
double CaptureWindow::libraryLength(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(libLenMutex_->m);
    auto it = libLengths_->find(path);
    if (it != libLengths_->end()) return it->second;
    (*libLengths_)[path] = 0;   // asked: not asked again
    auto lengths = libLengths_;
    auto mtx = libLenMutex_;
    HWND w = hwnd();
    std::thread([path, lengths, mtx, w] {
        const double s = video::mediaLength(path);
        {
            std::lock_guard<std::mutex> l(mtx->m);
            (*lengths)[path] = s;
        }
        if (s > 0 && IsWindow(w)) InvalidateRect(w, nullptr, FALSE);
    }).detach();
    return 0;
}

// A design's picture for its card, made once (and again if the design file is newer) in Library\thumbs.
std::wstring CaptureWindow::libraryThumb(const cj::medialib::Item& it) {
    const std::wstring src = filesDir() + L"\\" + widen(it.file);
    const std::wstring png = thumbsDir() + L"\\" + widen(it.file) + L".png";
    std::error_code ec;
    const auto ts = fs::last_write_time(fs::path(src), ec);
    if (ec) return {};
    const auto tt = fs::last_write_time(fs::path(png), ec);
    if (!ec && tt >= ts) return png;
    if (libThumbFailed_.count(it.file)) return {};
    std::string text;
    video::DesignPicture p;
    std::wstring err;
    const design::Document d = readAll(src, text) ? design::fromJson(text) : design::Document{};
    const float scale = std::min(1.0f, 360.0f / (float)std::max(1, d.width));
    fs::create_directories(fs::path(thumbsDir()), ec);
    if (text.empty() || !video::renderDesign(d, 1e9f, scale, profile_, p, err) || !video::savePng(png, p)) {
        libThumbFailed_.insert(it.file);
        return {};
    }
    return png;
}

void CaptureWindow::libraryExportPack() {
    loadLibrary(false);
    const std::string name = libFolder_.empty() ? std::string("My NorthRay library") : libFolder_.substr(libFolder_.find_last_of('/') + 1);
    std::wstring path;
    if (!pickPack(hwnd(), true, widen(name) + L".nrpack", path)) return;
    ml::PackResult r;
    const std::string bytes = ml::buildPack(library_, libFolder_, name, [](const std::string& f, std::string& b) { return readAll(filesDir() + L"\\" + widen(f), b); }, r);
    if (!r.ok) {
        flash(widen(r.error), 6);
        return;
    }
    if (!writeAll(path, bytes)) {
        flash(L"The pack couldn't be saved there.", 6);
        return;
    }
    log::info("Library: template pack exported (" + std::to_string(r.count) + " items)");
    flash(L"Saved a pack of " + std::to_wstring(r.count) + (r.count == 1 ? L" item" : L" items") + L" as " + fs::path(path).filename().wstring() + L".", 6);
}

void CaptureWindow::libraryImportPack() {
    std::wstring path;
    if (!pickPack(hwnd(), false, L"", path)) return;
    std::string bytes;
    if (!readAll(path, bytes)) {
        flash(L"That file couldn't be read.", 5);
        return;
    }
    const std::string name = ml::packName(bytes);
    if (name.empty()) {
        flash(L"That isn't a NorthRay template pack.", 6);
        return;
    }
    loadLibrary(false);
    std::error_code ec;
    fs::create_directories(fs::path(filesDir()), ec);
    const std::string into = "Packs/" + ml::Library::cleanFolder(name);
    const ml::PackResult r = ml::readPack(library_, bytes, into, today(),
                                          [](const std::string& f, const std::string& b) { return writeAll(filesDir() + L"\\" + widen(f), b); });
    if (!r.ok) {
        flash(widen(r.error), 6);
        loadLibrary(true);   // back to what's saved
        return;
    }
    saveLibrary();
    libFolder_ = ml::Library::cleanFolder(into);
    log::info("Library: template pack \"" + name + "\" imported (" + std::to_string(r.count) + " items)");
    flash(L"Added the pack “" + widen(name) + L"”: " + std::to_wstring(r.count) + (r.count == 1 ? L" item" : L" items") + L", in Packs.", 6);
    invalidate();
}

bool CaptureWindow::libraryRightClick(float x, float y) {
    if (!libraryOpen_ || !settingsOpen_) return false;
    for (auto& [id, r] : libRows_)
        if (r.contains(x, y)) {
            libraryItemMenu(id);
            return true;
        }
    return false;
}

}  // namespace ncap

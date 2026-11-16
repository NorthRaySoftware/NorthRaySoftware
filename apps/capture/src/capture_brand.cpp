// NorthRay Capture's window: the brand kit (colours, heading and body fonts, font files and a logo),
// kept in %LOCALAPPDATA%\NorthRay\brand.json for every design (and the other NorthRay apps). Font files
// and the logo are copied into %LOCALAPPDATA%\NorthRay\Brand so the kit keeps working if the originals
// move. Designs link to the kit (core/design: Brand); the renderer applies it (video/design_render).
#include "capture_window.hpp"

#include "log.hpp"
#include "video/design_render.hpp"

#include "ncap/design.hpp"
#include "ncap/sourceutil.hpp"

#include "cjui/menu.hpp"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ncap {

using cjui::MenuItem;
using cjui::narrow;
using cjui::widen;
namespace dz = design;
namespace su = sourceutil;
namespace fs = std::filesystem;

namespace {

std::wstring northRayFolder() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) out = std::wstring(p) + L"\\NorthRay";
    if (p) CoTaskMemFree(p);
    return out;
}
std::wstring brandPath() { return northRayFolder() + L"\\brand.json"; }

bool pickFile(HWND owner, const wchar_t* title, const wchar_t* typeName, const wchar_t* pattern, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dlg->SetTitle(title);
    const COMDLG_FILTERSPEC types[] = {{typeName, pattern}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out = path;
            ok = true;
            CoTaskMemFree(path);
        }
        if (item) item->Release();
    }
    dlg->Release();
    return ok;
}

// Copies a file into the kit's folder (a sub-folder), keeping its name; returns the copy's path, or ""
// with a reason.
std::wstring copyIntoKit(const std::wstring& from, const wchar_t* sub, std::wstring& error) {
    std::error_code ec;
    const fs::path dir = fs::path(northRayFolder()) / L"Brand" / sub;
    fs::create_directories(dir, ec);
    const fs::path to = dir / fs::path(from).filename();
    if (fs::equivalent(fs::path(from), to, ec)) return to.wstring();
    fs::copy_file(fs::path(from), to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = L"It couldn't be copied: " + widen(ec.message());
        return {};
    }
    return to.wstring();
}

const wchar_t* kCommonFonts[] = {L"Segoe UI", L"Segoe UI Black", L"Bahnschrift", L"Arial", L"Arial Black", L"Georgia",
                                 L"Verdana", L"Trebuchet MS", L"Impact", L"Consolas", L"Cambria", L"Comic Sans MS"};

}  // namespace

void CaptureWindow::loadBrand() {
    std::ifstream f(fs::path(brandPath()), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    video::setDesignBrand(ss.str().empty() ? dz::defaultBrand() : dz::brandFromJson(ss.str()));
}

void CaptureWindow::saveBrand(const dz::Brand& b) {
    std::error_code ec;
    fs::create_directories(fs::path(northRayFolder()), ec);
    {
        std::ofstream out(fs::path(brandPath()), std::ios::binary | std::ios::trunc);
        out << dz::toJson(b);
        if (!out) flash(L"The brand kit couldn't be saved.", 5);
    }
    video::setDesignBrand(b);
    brandSkies();
    brandVideos();
    designRendered_.clear();
    invalidate();
}

// Picks a font: the kit's heading and body fonts (linked: they follow the kit), the kit's font files,
// a few common ones, or any font Windows has by name.
void CaptureWindow::pickFont(const std::string& now, bool offerBrand, std::function<void(const std::string&)> done) {
    const dz::Brand b = video::designBrand();
    std::vector<MenuItem> items;
    if (offerBrand) {
        items.push_back(MenuItem::item(L"Brand heading font", 1, widen(b.headingFont), true, now == dz::kBrandHeading));
        items.push_back(MenuItem::item(L"Brand body font", 2, widen(b.bodyFont), true, now == dz::kBrandBody));
        items.push_back(MenuItem::sep());
    }
    std::vector<std::wstring> mine;
    for (auto& f : b.fontFiles)
        for (auto& fam : video::fontFileFamilies(widen(f)))
            if (std::find(mine.begin(), mine.end(), fam) == mine.end()) mine.push_back(fam);
    for (std::size_t i = 0; i < mine.size() && i < 100; ++i) items.push_back(MenuItem::item(mine[i], 100 + (UINT)i, L"your font file", true, narrow(mine[i]) == now));
    if (!mine.empty()) items.push_back(MenuItem::sep());
    const std::size_t nCommon = sizeof kCommonFonts / sizeof kCommonFonts[0];
    for (std::size_t i = 0; i < nCommon; ++i) items.push_back(MenuItem::item(kCommonFonts[i], 300 + (UINT)i, {}, true, narrow(kCommonFonts[i]) == now));
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"Another font by name…", 3));
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (cmd == 1) done(dz::kBrandHeading);
    else if (cmd == 2) done(dz::kBrandBody);
    else if (cmd >= 100 && cmd < 100 + mine.size()) done(narrow(mine[cmd - 100]));
    else if (cmd >= 300 && cmd < 300 + nCommon) done(narrow(kCommonFonts[cmd - 300]));
    else if (cmd == 3)
        askText(L"The font's name, as Windows lists it", now.rfind("brand:", 0) == 0 ? L"" : widen(now), [this, done](const std::wstring& v) {
            std::wstring err;
            const auto all = video::brandFontFamilies(err);
            auto it = std::find_if(all.begin(), all.end(), [&](const std::wstring& f) { return _wcsicmp(f.c_str(), v.c_str()) == 0; });
            if (it == all.end()) {
                flash(L"Windows doesn't have a font called “" + v + L"”. Add its file under Brand kit… › Add a font file.", 7);
                return;
            }
            done(narrow(*it));
        });
}

void CaptureWindow::editBrand() {
    dz::Brand b = video::designBrand();
    std::vector<MenuItem> items;
    items.push_back(MenuItem::item(L"COLOURS", 0, {}, false));
    for (std::size_t i = 0; i < b.colours.size(); ++i)
        items.push_back(MenuItem::item(widen(b.colours[i].name) + L"…", 100 + (UINT)i, widen(su::colourText(b.colours[i].colour))));
    items.push_back(MenuItem::item(L"Back to the NorthRay colours", 9));
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"FONTS", 0, {}, false));
    items.push_back(MenuItem::item(L"Heading font…", 10, widen(b.headingFont)));
    items.push_back(MenuItem::item(L"Body font…", 11, widen(b.bodyFont)));
    items.push_back(MenuItem::item(L"Add a font file…", 12, L".ttf or .otf", b.fontFiles.size() < 16));
    if (!b.fontFiles.empty()) {
        std::vector<MenuItem> rm;
        for (std::size_t i = 0; i < b.fontFiles.size(); ++i) rm.push_back(MenuItem::item(fs::path(widen(b.fontFiles[i])).filename().wstring(), 300 + (UINT)i));
        items.push_back(MenuItem::sub(L"Remove a font file", rm));
    }
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"LOGO", 0, {}, false));
    items.push_back(MenuItem::item(L"Logo picture…", 13, b.logo.empty() ? L"not set" : fs::path(widen(b.logo)).filename().wstring()));
    items.push_back(MenuItem::item(L"Remove the logo", 14, {}, !b.logo.empty()));
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"VIDEOS", 0, {}, false));
    auto fileName = [](const std::string& p) { return p.empty() ? std::wstring(L"not set") : fs::path(widen(p)).filename().wstring(); };
    wchar_t cut[32];
    swprintf(cut, 32, L"%.2g s", b.stingerCutMs / 1000.0);
    items.push_back(MenuItem::item(L"Transition stinger…", 20, fileName(b.stinger)));
    items.push_back(MenuItem::item(L"When the stinger switches scenes…", 21, cut, !b.stinger.empty()));
    items.push_back(MenuItem::item(L"Intro video…", 23, fileName(b.intro)));
    items.push_back(MenuItem::item(L"Outro video…", 25, fileName(b.outro)));
    std::vector<MenuItem> rmv;
    if (!b.stinger.empty()) rmv.push_back(MenuItem::item(L"The stinger", 22));
    if (!b.intro.empty()) rmv.push_back(MenuItem::item(L"The intro", 24));
    if (!b.outro.empty()) rmv.push_back(MenuItem::item(L"The outro", 26));
    if (!rmv.empty()) items.push_back(MenuItem::sub(L"Remove a video", rmv));
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (cmd >= 100 && cmd < 100 + b.colours.size()) {
        const std::size_t i = cmd - 100;
        askText(widen(b.colours[i].name) + L" (#RRGGBB): every design using it changes", widen(su::colourText(b.colours[i].colour)),
                [this, i](const std::wstring& v) {
                    su::Rgba c;
                    if (!su::parseColour(narrow(v), c)) {
                        flash(L"Type a colour like #3FCF8E.", 5);
                        return;
                    }
                    c.a = 255;
                    dz::Brand k = video::designBrand();
                    k.colours[i].colour = c;
                    saveBrand(k);
                });
    } else if (cmd == 9) {
        b.colours = dz::defaultBrand().colours;
        saveBrand(b);
    } else if (cmd == 10 || cmd == 11) {
        const bool heading = cmd == 10;
        pickFont(heading ? b.headingFont : b.bodyFont, false, [this, heading](const std::string& f) {
            dz::Brand k = video::designBrand();
            (heading ? k.headingFont : k.bodyFont) = f;
            saveBrand(k);
        });
    } else if (cmd == 12) {
        std::wstring path;
        if (!pickFile(hwnd(), L"Add a font file to your brand kit", L"Fonts", L"*.ttf;*.otf;*.ttc", path)) return;
        const auto fams = video::fontFileFamilies(path);
        if (fams.empty()) {
            flash(L"That isn't a font file Windows can read (or this version of Windows can't use font files directly).", 7);
            return;
        }
        std::wstring err;
        const std::wstring copy = copyIntoKit(path, L"Fonts", err);
        if (copy.empty()) {
            flash(err, 6);
            return;
        }
        const std::string c = narrow(copy);
        if (std::find(b.fontFiles.begin(), b.fontFiles.end(), c) == b.fontFiles.end()) b.fontFiles.push_back(c);
        saveBrand(b);
        std::wstring names;
        for (auto& f : fams) names += (names.empty() ? L"" : L", ") + f;
        flash(L"Added " + names + L". Choose it as the heading or body font, or for any text.", 7);
        log::info("brand: font file added (" + std::to_string(fams.size()) + " families)");
    } else if (cmd >= 300 && cmd < 300 + b.fontFiles.size()) {
        b.fontFiles.erase(b.fontFiles.begin() + (cmd - 300));
        saveBrand(b);
    } else if (cmd == 13) {
        std::wstring path;
        if (!pickImageFile(path)) return;
        std::wstring err;
        const std::wstring copy = copyIntoKit(path, L"Logo", err);
        if (copy.empty()) {
            flash(err, 6);
            return;
        }
        b.logo = narrow(copy);
        saveBrand(b);
        flash(L"Logo set. Templates' Logo watermark and pictures set to Your brand logo show it.", 6);
    } else if (cmd == 14) {
        b.logo.clear();
        saveBrand(b);
    } else if (cmd == 20 || cmd == 23 || cmd == 25) {
        std::wstring path;
        const wchar_t* title = cmd == 20 ? L"The brand kit's transition stinger" : cmd == 23 ? L"The brand kit's intro video" : L"The brand kit's outro video";
        if (!pickFile(hwnd(), title, L"Videos", L"*.mp4;*.mov;*.mkv;*.avi;*.wmv;*.webm;*.m4v", path)) return;
        std::wstring err;
        const std::wstring copy = copyIntoKit(path, L"Videos", err);
        if (copy.empty()) {
            flash(err, 6);
            return;
        }
        (cmd == 20 ? b.stinger : cmd == 23 ? b.intro : b.outro) = narrow(copy);
        saveBrand(b);
        flash(cmd == 20 ? L"Stinger set. In Transition › Stinger, choose Your brand kit's stinger."
                        : L"Set. Add it with Add a source › Your intro/outro video: it plays from the start each time its scene goes live.",
              7);
        log::info(std::string("brand: ") + (cmd == 20 ? "stinger" : cmd == 23 ? "intro" : "outro") + " video set");
    } else if (cmd == 21) {
        wchar_t now[32];
        swprintf(now, 32, L"%.2g", b.stingerCutMs / 1000.0);
        askText(L"Seconds into the stinger when the scene switches (when it covers the screen)", now, [this](const std::wstring& v) {
            const double secs = _wtof(v.c_str());
            if (secs < 0 || secs > 10) {
                flash(L"Type a number of seconds from 0 to 10.", 5);
                return;
            }
            dz::Brand k = video::designBrand();
            k.stingerCutMs = (int)(secs * 1000 + 0.5);
            saveBrand(k);
        });
    } else if (cmd == 22 || cmd == 24 || cmd == 26) {
        (cmd == 22 ? b.stinger : cmd == 24 ? b.intro : b.outro).clear();
        saveBrand(b);
    }
}

}  // namespace ncap

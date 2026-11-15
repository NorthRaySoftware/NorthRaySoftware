// NorthRay Capture's window: the sources made in Capture (images, text, colours, slideshows,
// screen regions) and the exact controls every item has (opacity, position and size, rotation,
// duplicating). Settings are chosen from the item's menu, with typed values where a list isn't enough.
#include "capture_window.hpp"

#include "log.hpp"

#include "ncap/sourceutil.hpp"

#include <dwrite.h>
#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cmath>

namespace ncap {

using cjui::MenuItem;
using video::ComPtr;
using cjui::narrow;
using cjui::widen;
using nlohmann::json;
namespace su = sourceutil;

namespace {

enum : UINT {
    // Made sources' own settings (madeCommand).
    CmdImageFile = 6000,
    CmdTextEdit, CmdTextBold, CmdTextItalic, CmdTextFontOther, CmdTextSizeOther, CmdTextColourOther, CmdTextOutlineColourOther, CmdTextWrap,
    CmdColourOther, CmdBoxSize, CmdSlideFolder, CmdSlideShuffle, CmdRegionAgain,
    CmdMediaPause, CmdMediaPlay, CmdMediaRestart, CmdMediaNext, CmdMediaLoop, CmdMediaShuffle, CmdMediaFiles, CmdDesignEdit,
    CmdFont0 = 6100, CmdSize0 = 6200, CmdColour0 = 6300, CmdAlign0 = 6400, CmdOutline0 = 6500, CmdOutlineColour0 = 6600, CmdSeconds0 = 6700,
    // Any item (itemExtraCommand).
    CmdOpacity0 = 6800, CmdOpacityOther = 6850, CmdPlacement, CmdRotation, CmdDuplicate, CmdUngroup, CmdBorder,
    CmdGroupWith0 = 7000, CmdAddToGroup0 = 7500,
};

const wchar_t* kFonts[] = {L"Segoe UI", L"Segoe UI Black", L"Bahnschrift", L"Arial", L"Arial Black", L"Calibri", L"Cambria", L"Georgia",
                           L"Times New Roman", L"Trebuchet MS", L"Verdana", L"Consolas", L"Impact", L"Comic Sans MS", L"Segoe Print", L"Segoe Script"};
const int kSizes[] = {24, 32, 48, 64, 96, 128, 200};
struct Named { const wchar_t* name; const char* hex; };
const Named kColours[] = {{L"White", "#FFFFFF"}, {L"Black", "#000000"}, {L"NorthRay teal", "#3FCF8E"}, {L"Sky blue", "#4DA3FF"},
                          {L"Purple", "#7A5CFF"},  {L"Yellow", "#FFD23F"}, {L"Red", "#E0566A"},        {L"Dark blue", "#1E2A44"}};
const char* kAligns[] = {"left", "centre", "right"};
const wchar_t* kAlignNames[] = {L"Left", L"Centre", L"Right"};
const int kOutlines[] = {0, 2, 4, 8};
const wchar_t* kOutlineNames[] = {L"None", L"Thin", L"Medium", L"Thick"};
const int kSeconds[] = {3, 5, 10, 20, 60};
const int kOpacities[] = {100, 90, 75, 50, 25, 10};

std::string str(const json& j, const char* k, const std::string& fallback = {}) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : fallback;
}
double num(const json& j, const char* k, double fallback) { return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<double>() : fallback; }
bool flag(const json& j, const char* k, bool fallback) { return j.is_object() && j.contains(k) && j[k].is_boolean() ? j[k].get<bool>() : fallback; }

bool fontInstalled(const wchar_t* family) {
    static ComPtr<IDWriteFontCollection> fonts = [] {
        ComPtr<IDWriteFactory> f;
        ComPtr<IDWriteFontCollection> c;
        if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(f.GetAddressOf()))))
            f->GetSystemFontCollection(&c);
        return c;
    }();
    if (!fonts) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
}

// Windows' own file or folder picker.
bool pick(HWND owner, bool folder, const wchar_t* title, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    dlg->SetTitle(title);
    if (!folder) {
        const COMDLG_FILTERSPEC types[] = {{L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.heic"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, types);
    }
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

// Several files at once (a playlist).
bool pickMany(HWND owner, const wchar_t* title, std::vector<std::wstring>& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT);
    dlg->SetTitle(title);
    const COMDLG_FILTERSPEC types[] = {{L"Video and sound", L"*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv;*.webm;*.mp3;*.m4a;*.aac;*.wav;*.wma;*.flac"},
                                       {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n && i < 500; ++i) {
                IShellItem* it = nullptr;
                PWSTR p = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    out.push_back(p);
                    CoTaskMemFree(p);
                }
                if (it) it->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    return !out.empty();
}

std::string baseName(const std::wstring& path) {
    std::wstring n = path.substr(path.find_last_of(L"\\/") + 1);
    const std::size_t dot = n.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) n.resize(dot);
    return narrow(n);
}

// ---- the screen region picker: a still of the whole desktop, dimmed, in a full-screen window
// (not a see-through one: those behave differently from PC to PC). Drag a rectangle; the part
// inside shows at full brightness. Esc or a right-click cancels.

struct RegionPick {
    POINT a{}, b{};
    bool dragging = false, done = false, cancelled = false;
    HDC still = nullptr, dim = nullptr;   // the desktop as it was, and the same darkened
    int ox = 0, oy = 0;                   // the virtual desktop's top-left
};

// Where a mouse message happened, on the screen (not where the pointer is by the time it's handled).
POINT at(HWND h, LPARAM l) {
    POINT pt{(short)LOWORD(l), (short)HIWORD(l)};
    ClientToScreen(h, &pt);
    return pt;
}

LRESULT CALLBACK pickProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* p = reinterpret_cast<RegionPick*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_LBUTTONDOWN:
            if (p) {
                p->a = at(h, l);
                p->b = p->a;
                p->dragging = true;
                SetCapture(h);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (p && p->dragging) {
                p->b = at(h, l);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (p && p->dragging) {
                p->b = at(h, l);
                p->dragging = false;
                p->done = true;
                ReleaseCapture();
            }
            return 0;
        case WM_KEYDOWN:
        case WM_RBUTTONDOWN:
            if (p && (m == WM_RBUTTONDOWN || w == VK_ESCAPE)) {
                p->cancelled = true;
                p->done = true;
            }
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            // Drawn off-screen, then shown at once (no flicker while dragging).
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            if (p && p->dim) BitBlt(mem, 0, 0, rc.right, rc.bottom, p->dim, 0, 0, SRCCOPY);
            RECT sel{};
            if (p && (p->dragging || p->done)) {
                POINT a = p->a, b = p->b;
                ScreenToClient(h, &a);
                ScreenToClient(h, &b);
                sel = {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
                if (p->still) BitBlt(mem, sel.left, sel.top, sel.right - sel.left, sel.bottom - sel.top, p->still, sel.left, sel.top, SRCCOPY);
                HBRUSH edge = CreateSolidBrush(RGB(63, 207, 142));
                RECT r = sel;
                FrameRect(mem, &r, edge);
                InflateRect(&r, 1, 1);
                FrameRect(mem, &r, edge);
                DeleteObject(edge);
            }
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, RGB(232, 236, 245));
            HFONT f = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HGDIOBJ oldFont = SelectObject(mem, f);
            RECT t{rc.left, rc.top + 40, rc.right, rc.top + 90};
            DrawTextW(mem, L"Drag across the part of the screen to capture.   Esc or right-click cancels.", -1, &t, DT_CENTER | DT_SINGLELINE);
            if (sel.right > sel.left) {
                wchar_t s[64];
                swprintf(s, 64, L"%ld × %ld", sel.right - sel.left, sel.bottom - sel.top);
                RECT tr{sel.left, sel.bottom + 6, sel.left + 300, sel.bottom + 34};
                DrawTextW(mem, s, -1, &tr, DT_LEFT | DT_SINGLELINE);
            }
            SelectObject(mem, oldFont);
            DeleteObject(f);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        default: return DefWindowProcW(h, m, w, l);
    }
}

bool pickScreenRegion(RECT& out) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = pickProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NorthRayCaptureRegionPick";
        registered = RegisterClassW(&wc) != 0;
    }
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RegionPick p;
    p.ox = x;
    p.oy = y;
    // The desktop as it is now, and a darkened copy of it.
    HDC screen = GetDC(nullptr);
    p.still = CreateCompatibleDC(screen);
    p.dim = CreateCompatibleDC(screen);
    HBITMAP stillBmp = CreateCompatibleBitmap(screen, w, h), dimBmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ s0 = SelectObject(p.still, stillBmp), d0 = SelectObject(p.dim, dimBmp);
    BitBlt(p.still, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    RECT all{0, 0, w, h};
    HBRUSH dark = CreateSolidBrush(RGB(8, 12, 24));
    FillRect(p.dim, &all, dark);
    DeleteObject(dark);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 110, 0};   // the still at ~43% over dark blue
    GdiAlphaBlend(p.dim, 0, 0, w, h, p.still, 0, 0, w, h, bf);

    HWND win = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"NorthRayCaptureRegionPick", L"Pick a region", WS_POPUP, x, y, w, h, nullptr,
                               nullptr, GetModuleHandleW(nullptr), nullptr);
    bool ok = false;
    if (win) {
        SetWindowLongPtrW(win, GWLP_USERDATA, (LONG_PTR)&p);
        SetWindowPos(win, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
        SetForegroundWindow(win);
        SetFocus(win);
        MSG msg;
        while (!p.done && IsWindow(win) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        DestroyWindow(win);
        if (!p.cancelled) {
            out = {std::min(p.a.x, p.b.x), std::min(p.a.y, p.b.y), std::max(p.a.x, p.b.x), std::max(p.a.y, p.b.y)};
            ok = out.right - out.left >= 8 && out.bottom - out.top >= 8;
        }
    }
    SelectObject(p.still, s0);
    SelectObject(p.dim, d0);
    DeleteObject(stillBmp);
    DeleteObject(dimBmp);
    DeleteDC(p.still);
    DeleteDC(p.dim);
    return ok;
}

// The region as settings: the monitor with its centre, and the rectangle within that monitor.
bool regionSettings(const RECT& r, json& out) {
    const POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    for (auto& m : video::listMonitors())
        if (PtInRect(&m.rect, c)) {
            RECT in{};
            IntersectRect(&in, &r, &m.rect);
            out = {{"monitor", narrow(m.device)},
                   {"x", in.left - m.rect.left},
                   {"y", in.top - m.rect.top},
                   {"w", in.right - in.left},
                   {"h", in.bottom - in.top},
                   {"method", "auto"},
                   {"cursor", true}};
            return true;
        }
    return false;
}

}  // namespace

// ---------------------------------------------------------------- adding

bool CaptureWindow::pickImageFile(std::wstring& path) { return pick(hwnd(), false, L"Choose a picture", path); }

void CaptureWindow::addImage() {
    std::wstring path;
    if (!pick(hwnd(), false, L"Choose an image", path)) return;
    addCapture(SourceKind::Image, baseName(path), json{{"file", narrow(path)}}.dump());
}

void CaptureWindow::addText() {
    askText(L"The text (\\n starts a new line)", L"", [this](const std::wstring& v) {
        std::string t = narrow(v);
        for (std::size_t at = 0; (at = t.find("\\n", at)) != std::string::npos;) t.replace(at, 2, "\n");
        if (t.find_first_not_of(" \n") == std::string::npos) return;
        std::string name = t.substr(0, t.find('\n'));
        if (name.size() > 32) name = name.substr(0, 32);
        addCapture(SourceKind::Text, name,
                   json{{"text", t}, {"font", "Segoe UI"}, {"size", 64}, {"colour", "#FFFFFF"}, {"bold", true}, {"outline", 4}, {"outlineColour", "#000000"}}
                       .dump());
    });
}

void CaptureWindow::addColour() {
    askText(L"The colour, as #RRGGBB (or #RRGGBBAA to see through it)", L"#1E2A44", [this](const std::wstring& v) {
        su::Rgba c;
        if (!su::parseColour(narrow(v), c)) {
            flash(L"That isn't a colour: try #1E2A44, or #00000080 for half see-through black.", 6);
            return;
        }
        addCapture(SourceKind::Colour, "Colour " + su::colourText(c),
                   json{{"colour", su::colourText(c)}, {"width", scenes_.canvasWidth}, {"height", scenes_.canvasHeight}}.dump());
    });
}

void CaptureWindow::addSlideshow() {
    std::wstring folder;
    if (!pick(hwnd(), true, L"Choose a folder of images", folder)) return;
    addCapture(SourceKind::Slideshow, "Slideshow: " + baseName(folder + L".x"),
               json{{"folder", narrow(folder)}, {"seconds", 5}, {"shuffle", false}, {"width", scenes_.canvasWidth}, {"height", scenes_.canvasHeight}}.dump());
}

void CaptureWindow::addMedia() {
    std::vector<std::wstring> files;
    if (!pickMany(hwnd(), L"Choose video or sound files (several play in turn)", files)) return;
    json list = json::array();
    for (auto& f : files) list.push_back(narrow(f));
    const std::string name = files.size() == 1 ? baseName(files[0]) : "Playlist (" + std::to_string(files.size()) + " files)";
    addCapture(SourceKind::Media, name, json{{"files", list}, {"loop", true}, {"shuffle", false}, {"volume", 1.0}, {"muted", false}}.dump());
}

// Music: every sound file in a folder (not its sub-folders), shuffled and looping, a little quieter
// than full, and ducking under the first microphone (if there is one) so talking stays clear.
void CaptureWindow::addMusic() {
    std::wstring folder;
    if (!pick(hwnd(), true, L"Choose a folder of songs", folder)) return;
    std::vector<std::string> files;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(std::filesystem::path(folder), ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = narrow(it->path().extension().wstring());
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".mp3" || ext == ".m4a" || ext == ".aac" || ext == ".wav" || ext == ".wma" || ext == ".flac") files.push_back(narrow(it->path().wstring()));
        if (files.size() >= 2000) break;
    }
    if (files.empty()) {
        flash(L"No songs in that folder (MP3, M4A, AAC, WAV, WMA or FLAC).", 6);
        return;
    }
    std::sort(files.begin(), files.end());
    json s{{"files", files}, {"loop", true}, {"shuffle", true}, {"volume", 0.5}, {"muted", false}};
    std::string mic;
    for (auto& src : scenes_.sources())
        if (src.kind == SourceKind::InputAudio && src.settings.find("#synctone") == std::string::npos) {
            mic = src.id;
            s["duck"] = json{{"under", src.id}, {"amount", -12}};
            break;
        }
    addCapture(SourceKind::Media, "Music: " + baseName(folder + L".x"), s.dump());
    flash(std::to_wstring(files.size()) + L" songs, shuffled." +
              (mic.empty() ? std::wstring(L" Add your microphone, then choose Auto-duck in the music's mixer menu so it dips while you talk.")
                           : L" It dips 12 dB while " + widen(scenes_.source(mic)->name) + L" has sound (Auto-duck in its mixer menu)."),
          8);
}

void CaptureWindow::addRegion() {
    RECT r{};
    if (!pickScreenRegion(r)) return;
    json s;
    if (!regionSettings(r, s)) {
        flash(L"That region isn't on a display.", 5);
        return;
    }
    addCapture(SourceKind::Region, "Screen region", s.dump());
}

// ---------------------------------------------------------------- a made source's own settings

void CaptureWindow::changeSourceSetting(const Id& source, const char* key, const json& value) {
    const Source* src = scenes_.source(source);
    if (!src) return;
    json s = json::parse(src->settings, nullptr, false);
    if (!s.is_object()) s = json::object();
    s[key] = value;
    scenes_.setSourceSettings(source, s.dump());
    syncSources();
    scenesChanged();
}

void CaptureWindow::madeMenuItems(const Source& src, std::vector<MenuItem>& menu) {
    const json s = json::parse(src.settings, nullptr, false);
    auto colourMenu = [&](UINT first, UINT other, const std::string& current) {
        std::vector<MenuItem> v;
        for (int i = 0; i < 8; ++i) v.push_back(MenuItem::item(kColours[i].name, first + (UINT)i, {}, true, current == kColours[i].hex));
        v.push_back(MenuItem::item(L"Another colour…", other, widen(current)));
        return v;
    };
    switch (src.kind) {
        case SourceKind::Image: menu.push_back(MenuItem::item(L"Choose another image…", CmdImageFile)); break;
        case SourceKind::Text: {
            const std::string font = str(s, "font", "Segoe UI");
            std::vector<MenuItem> fonts, sizes, aligns, outlines;
            for (int i = 0; i < 16; ++i)
                if (fontInstalled(kFonts[i])) fonts.push_back(MenuItem::item(kFonts[i], CmdFont0 + (UINT)i, {}, true, widen(font) == kFonts[i]));
            fonts.push_back(MenuItem::item(L"Another font…", CmdTextFontOther, widen(font)));
            const int size = (int)num(s, "size", 64);
            for (int i = 0; i < 7; ++i) sizes.push_back(MenuItem::item(std::to_wstring(kSizes[i]) + L" pixels", CmdSize0 + (UINT)i, {}, true, size == kSizes[i]));
            sizes.push_back(MenuItem::item(L"Another size…", CmdTextSizeOther, std::to_wstring(size)));
            const std::string align = str(s, "align", "left");
            for (int i = 0; i < 3; ++i) aligns.push_back(MenuItem::item(kAlignNames[i], CmdAlign0 + (UINT)i, {}, true, align == kAligns[i]));
            const int outline = (int)num(s, "outline", 0);
            for (int i = 0; i < 4; ++i) outlines.push_back(MenuItem::item(kOutlineNames[i], CmdOutline0 + (UINT)i, {}, true, outline == kOutlines[i]));
            outlines.push_back(MenuItem::sep());
            outlines.push_back(MenuItem::sub(L"Outline colour", colourMenu(CmdOutlineColour0, CmdTextOutlineColourOther, str(s, "outlineColour", "#000000"))));
            const int wrap = (int)num(s, "wrap", 0);
            std::vector<MenuItem> text = {
                MenuItem::item(L"Edit the text…", CmdTextEdit),
                MenuItem::sub(L"Font", fonts),
                MenuItem::sub(L"Size", sizes),
                MenuItem::sub(L"Colour", colourMenu(CmdColour0, CmdTextColourOther, str(s, "colour", "#FFFFFF"))),
                MenuItem::item(L"Bold", CmdTextBold, {}, true, flag(s, "bold", false)),
                MenuItem::item(L"Italic", CmdTextItalic, {}, true, flag(s, "italic", false)),
                MenuItem::sub(L"Alignment", aligns),
                MenuItem::sub(L"Outline", outlines),
                MenuItem::item(L"Wrap lines at a width…", CmdTextWrap, wrap > 0 ? std::to_wstring(wrap) + L" px" : L"off"),
            };
            menu.push_back(MenuItem::sub(L"Text", text));
            break;
        }
        case SourceKind::Colour:
            if (src.settings.find("\"sync\"") != std::string::npos) break;
            menu.push_back(MenuItem::sub(L"Colour", colourMenu(CmdColour0, CmdColourOther, str(s, "colour", "#1E2A44"))));
            menu.push_back(MenuItem::item(L"Its own size…", CmdBoxSize, std::to_wstring((int)num(s, "width", 1920)) + L"×" + std::to_wstring((int)num(s, "height", 1080))));
            break;
        case SourceKind::Slideshow: {
            std::vector<MenuItem> secs;
            const int sec = (int)num(s, "seconds", 5);
            for (int i = 0; i < 5; ++i) secs.push_back(MenuItem::item(std::to_wstring(kSeconds[i]) + L" seconds", CmdSeconds0 + (UINT)i, {}, true, sec == kSeconds[i]));
            menu.push_back(MenuItem::item(L"Choose another folder…", CmdSlideFolder));
            menu.push_back(MenuItem::sub(L"Each image for", secs));
            menu.push_back(MenuItem::item(L"Shuffle", CmdSlideShuffle, {}, true, flag(s, "shuffle", false)));
            menu.push_back(MenuItem::item(L"Its own size…", CmdBoxSize, std::to_wstring((int)num(s, "width", 1920)) + L"×" + std::to_wstring((int)num(s, "height", 1080))));
            break;
        }
        case SourceKind::Region: menu.push_back(MenuItem::item(L"Pick the region again…", CmdRegionAgain)); break;
        case SourceKind::Design: menu.push_back(MenuItem::item(L"Edit the design…", CmdDesignEdit, L"the overlay designer")); break;
        default: break;
    }
}

bool CaptureWindow::madeCommand(UINT cmd, const Id& source) {
    const Source* src = scenes_.source(source);
    if (!src || cmd < CmdImageFile || cmd >= CmdOpacity0) return false;
    const json s = json::parse(src->settings, nullptr, false);
    auto askColour = [this, source](const char* key, const std::string& current) {
        askText(L"The colour, as #RRGGBB (or #RRGGBBAA)", widen(current), [this, source, key](const std::wstring& v) {
            su::Rgba c;
            if (!su::parseColour(narrow(v), c)) {
                flash(L"That isn't a colour: try #FFD23F.", 5);
                return;
            }
            changeSourceSetting(source, key, su::colourText(c));
        });
    };
    if (cmd == CmdDesignEdit) {
        openDesigner(source);
        return true;
    }
    if (cmd == CmdImageFile) {
        std::wstring path;
        if (pick(hwnd(), false, L"Choose an image", path)) changeSourceSetting(source, "file", narrow(path));
    } else if (cmd == CmdTextEdit) {
        std::string t = str(s, "text");
        for (std::size_t at = 0; (at = t.find('\n', at)) != std::string::npos;) t.replace(at, 1, "\\n");
        askText(L"The text (\\n starts a new line)", widen(t), [this, source](const std::wstring& v) {
            std::string t2 = narrow(v);
            for (std::size_t at = 0; (at = t2.find("\\n", at)) != std::string::npos;) t2.replace(at, 2, "\n");
            changeSourceSetting(source, "text", t2);
        });
    } else if (cmd == CmdTextBold) changeSourceSetting(source, "bold", !flag(s, "bold", false));
    else if (cmd == CmdTextItalic) changeSourceSetting(source, "italic", !flag(s, "italic", false));
    else if (cmd == CmdTextFontOther) {
        askText(L"The font's name, as Windows lists it", widen(str(s, "font", "Segoe UI")), [this, source](const std::wstring& v) {
            if (!fontInstalled(v.c_str())) {
                flash(L"That font isn't installed: check the name in Windows' Fonts settings.", 6);
                return;
            }
            changeSourceSetting(source, "font", narrow(v));
        });
    } else if (cmd == CmdTextSizeOther) {
        askText(L"The size in pixels (6 to 600)", std::to_wstring((int)num(s, "size", 64)), [this, source](const std::wstring& v) {
            const int n = _wtoi(v.c_str());
            if (n < 6 || n > 600) {
                flash(L"Choose a size from 6 to 600 pixels.", 5);
                return;
            }
            changeSourceSetting(source, "size", n);
        });
    } else if (cmd == CmdTextColourOther || cmd == CmdColourOther) askColour("colour", str(s, "colour", "#FFFFFF"));
    else if (cmd == CmdTextOutlineColourOther) askColour("outlineColour", str(s, "outlineColour", "#000000"));
    else if (cmd == CmdTextWrap) {
        askText(L"Wrap lines at this width in pixels (0 for no wrapping)", std::to_wstring((int)num(s, "wrap", 0)), [this, source](const std::wstring& v) {
            const int n = _wtoi(v.c_str());
            if (n < 0 || n > 8192) return;
            changeSourceSetting(source, "wrap", n);
        });
    } else if (cmd == CmdBoxSize) {
        askText(L"Its own size, width × height in pixels", std::to_wstring((int)num(s, "width", 1920)) + L", " + std::to_wstring((int)num(s, "height", 1080)),
                [this, source](const std::wstring& v) {
                    su::Placement p;
                    if (!su::parsePlacement(narrow(v), p) || !p.hasX || !p.hasY || p.x < 1 || p.y < 1 || p.x > 8192 || p.y > 8192) {
                        flash(L"Type the width and height, e.g. 1920, 1080.", 5);
                        return;
                    }
                    changeSourceSetting(source, "width", (int)p.x);
                    changeSourceSetting(source, "height", (int)p.y);
                });
    } else if (cmd == CmdSlideFolder) {
        std::wstring folder;
        if (pick(hwnd(), true, L"Choose a folder of images", folder)) changeSourceSetting(source, "folder", narrow(folder));
    } else if (cmd == CmdSlideShuffle) changeSourceSetting(source, "shuffle", !flag(s, "shuffle", false));
    else if (cmd == CmdRegionAgain) {
        RECT r{};
        json n;
        if (pickScreenRegion(r) && regionSettings(r, n)) {
            for (auto& [k, v] : n.items()) changeSourceSetting(source, k.c_str(), v);
        }
    } else if (cmd == CmdMediaPause || cmd == CmdMediaPlay || cmd == CmdMediaRestart || cmd == CmdMediaNext) {
        auto it = live_.find(source);
        if (it != live_.end() && it->second.capture)
            it->second.capture->command(cmd == CmdMediaPause ? "pause" : cmd == CmdMediaPlay ? "play" : cmd == CmdMediaRestart ? "restart" : "next");
    } else if (cmd == CmdMediaLoop) changeSourceSetting(source, "loop", !flag(s, "loop", true));
    else if (cmd == CmdMediaShuffle) changeSourceSetting(source, "shuffle", !flag(s, "shuffle", false));
    else if (cmd == CmdMediaFiles) {
        std::vector<std::wstring> files;
        if (pickMany(hwnd(), L"Choose video or sound files", files)) {
            json list = json::array();
            for (auto& f : files) list.push_back(narrow(f));
            changeSourceSetting(source, "files", list);
        }
    } else if (cmd >= CmdFont0 && cmd < CmdFont0 + 16) changeSourceSetting(source, "font", narrow(kFonts[cmd - CmdFont0]));
    else if (cmd >= CmdSize0 && cmd < CmdSize0 + 7) changeSourceSetting(source, "size", kSizes[cmd - CmdSize0]);
    else if (cmd >= CmdColour0 && cmd < CmdColour0 + 8) changeSourceSetting(source, "colour", kColours[cmd - CmdColour0].hex);
    else if (cmd >= CmdAlign0 && cmd < CmdAlign0 + 3) changeSourceSetting(source, "align", kAligns[cmd - CmdAlign0]);
    else if (cmd >= CmdOutline0 && cmd < CmdOutline0 + 4) changeSourceSetting(source, "outline", kOutlines[cmd - CmdOutline0]);
    else if (cmd >= CmdOutlineColour0 && cmd < CmdOutlineColour0 + 8) changeSourceSetting(source, "outlineColour", kColours[cmd - CmdOutlineColour0].hex);
    else if (cmd >= CmdSeconds0 && cmd < CmdSeconds0 + 5) changeSourceSetting(source, "seconds", kSeconds[cmd - CmdSeconds0]);
    else return false;
    return true;
}

// ---------------------------------------------------------------- any item: exact placement, opacity, a copy

void CaptureWindow::itemExtraItems(const SceneItem& item, std::vector<MenuItem>& menu) {
    const bool movable = !item.locked;
    std::vector<MenuItem> op;
    const int now = (int)std::lround(item.transform.opacity * 100);
    for (int i = 0; i < 6; ++i) op.push_back(MenuItem::item(std::to_wstring(kOpacities[i]) + L"%", CmdOpacity0 + (UINT)i, {}, movable, now == kOpacities[i]));
    op.push_back(MenuItem::item(L"Another amount…", CmdOpacityOther, std::to_wstring(now) + L"%", movable));
    const ItemSize sz = sizeOf(item.source);
    menu.push_back(MenuItem::item(L"Position and size…", CmdPlacement, widen(su::placementText(item.transform, sz.w, sz.h)), movable && sz.w > 0));
    wchar_t deg[32];
    swprintf(deg, 32, L"%g°", std::round(item.transform.rotation * 10) / 10);
    menu.push_back(MenuItem::item(L"Rotation…", CmdRotation, deg, movable));
    menu.push_back(MenuItem::sub(L"Opacity", op));
    menu.push_back(MenuItem::item(L"Border…", CmdBorder, item.border.on ? L"on" : L"rounded corners, a ring, a glow"));
    menu.push_back(MenuItem::item(L"Duplicate", CmdDuplicate, L"the same source"));
    // Groups.
    const Scene* sc = scenes_.scene(scenes_.current());
    if (!sc) return;
    const bool isGroup = scenes_.isGroupItem(sc->id, item.id);
    std::vector<MenuItem> others;
    for (std::size_t k = 0; k < sc->items.size() && k < 400; ++k) {
        const SceneItem& o = sc->items[k];
        if (o.id == item.id) continue;
        const Source* os = scenes_.source(o.source);
        others.push_back(MenuItem::item(os ? widen(os->name) : L"?", (isGroup ? CmdAddToGroup0 : CmdGroupWith0) + (UINT)k));
    }
    menu.push_back(MenuItem::sep());
    if (isGroup) {
        if (!others.empty()) menu.push_back(MenuItem::sub(L"Add to this group", others));
        menu.push_back(MenuItem::item(L"Ungroup", CmdUngroup, L"its sources back here"));
    } else if (!others.empty()) {
        menu.push_back(MenuItem::sub(L"Group with", others));
    }
}

// Before an item goes into or out of a scaled group: one shown at its own size gets that size written
// down, so it keeps showing the same.
static void pinSize(SceneCollection& scenes, const Id& scene, const SceneItem& i, float w, float h) {
    if ((i.transform.width > 0 && i.transform.height > 0) || w <= 0 || h <= 0) return;
    Transform t = i.transform;
    t.width = w - t.cropLeft - t.cropRight;
    t.height = h - t.cropTop - t.cropBottom;
    scenes.setTransform(scene, i.id, t);
}

bool CaptureWindow::itemExtraCommand(UINT cmd, const Id& itemId) {
    const SceneItem* item = scenes_.item(scenes_.current(), itemId);
    if (!item || cmd < CmdOpacity0) return false;
    const Id scene = scenes_.current();
    if (cmd == CmdBorder) {
        openBorder(itemId);
        return true;
    }
    if (cmd >= CmdOpacity0 && cmd < CmdOpacity0 + 6) {
        Transform t = item->transform;
        t.opacity = kOpacities[cmd - CmdOpacity0] / 100.0f;
        setTransform(itemId, t);
    } else if (cmd == CmdOpacityOther) {
        askText(L"Opacity in percent (0 is invisible, 100 solid)", std::to_wstring((int)std::lround(item->transform.opacity * 100)),
                [this, itemId](const std::wstring& v) {
                    const SceneItem* i = scenes_.item(scenes_.current(), itemId);
                    const int n = _wtoi(v.c_str());
                    if (!i || n < 0 || n > 100) return;
                    Transform t = i->transform;
                    t.opacity = n / 100.0f;
                    setTransform(itemId, t);
                });
    } else if (cmd == CmdPlacement) {
        const ItemSize sz = sizeOf(item->source);
        askText(L"x, y, width, height on the canvas (pixels; - keeps one as it is)", widen(su::placementText(item->transform, sz.w, sz.h)),
                [this, itemId](const std::wstring& v) {
                    const SceneItem* i = scenes_.item(scenes_.current(), itemId);
                    su::Placement p;
                    if (!i || !su::parsePlacement(narrow(v), p)) {
                        flash(L"Type four numbers: x, y, width, height (or just x, y).", 6);
                        return;
                    }
                    Transform t = i->transform;
                    if (p.hasX) t.x = p.x;
                    if (p.hasY) t.y = p.y;
                    if (p.hasSize) {
                        t.width = p.w;
                        t.height = p.h;
                    }
                    setTransform(itemId, t);
                });
    } else if (cmd == CmdRotation) {
        wchar_t now[32];
        swprintf(now, 32, L"%g", std::round(item->transform.rotation * 10) / 10);
        askText(L"Rotation in degrees, clockwise", now, [this, itemId](const std::wstring& v) {
            const SceneItem* i = scenes_.item(scenes_.current(), itemId);
            float d = 0;
            if (!i || !su::parseDegrees(narrow(v), d)) {
                flash(L"Type a number of degrees, e.g. 15 or -90.", 5);
                return;
            }
            Transform t = i->transform;
            t.rotation = d;
            setTransform(itemId, t);
        });
    } else if (cmd >= CmdGroupWith0 && cmd < CmdGroupWith0 + 400) {
        const Scene* sc = scenes_.scene(scene);
        const std::size_t k = cmd - CmdGroupWith0;
        if (!sc || k >= sc->items.size()) return true;
        std::string err;
        const Id g = scenes_.groupItems(scene, {itemId, sc->items[k].id}, err);
        if (g.empty()) flash(widen(err), 6);
        else {
            selected_ = g;
            log::info("Grouped two sources");
            syncSources();
            scenesChanged();
        }
    } else if (cmd >= CmdAddToGroup0 && cmd < CmdAddToGroup0 + 400) {
        const Scene* sc = scenes_.scene(scene);
        const std::size_t k = cmd - CmdAddToGroup0;
        if (!sc || k >= sc->items.size()) return true;
        const SceneItem other = sc->items[k];
        const ItemSize sz = sizeOf(other.source);
        pinSize(scenes_, scene, other, sz.w, sz.h);
        std::string err;
        if (!scenes_.addToGroup(scene, itemId, other.id, err)) flash(widen(err), 7);
        else scenesChanged();
    } else if (cmd == CmdUngroup) {
        const Source* gs = scenes_.source(item->source);
        const Id inner = gs ? SceneCollection::nestedScene(*gs) : Id();
        if (const Scene* in = scenes_.scene(inner))
            for (auto i : in->items) {
                const ItemSize sz = sizeOf(i.source);
                pinSize(scenes_, inner, i, sz.w, sz.h);
            }
        std::string err;
        if (!scenes_.ungroup(scene, itemId, err)) flash(widen(err), 7);
        else {
            selected_.clear();
            scenesChanged();
        }
    } else if (cmd == CmdDuplicate) {
        const Id copy = scenes_.duplicateItem(scene, itemId);
        if (!copy.empty()) {
            selected_ = copy;
            scenesChanged();
        }
    } else {
        return false;
    }
    return true;
}

}  // namespace ncap

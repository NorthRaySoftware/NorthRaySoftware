#include "launcher.hpp"
#include "cjui/dialog.hpp"

#include "cjui/http.hpp"
#include "cjui/icons.hpp"
#include "cjui/menu.hpp"
#include "cjui/northray.hpp"
#include "cjui/packages.hpp"
#include "resource.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <nlohmann/json.hpp>
#include <thread>

namespace nrl {

static const float kNavW = 232;
static const UINT WM_APP_JOBS = WM_APP + 1;
static const UINT WM_APP_CATALOGUE = WM_APP + 2;
static const UINT WM_APP_TRAY = WM_APP + 3;
static const UINT_PTR kRefreshTimer = 1;
static const UINT_PTR kAutoUpdateTimer = 2;   // installs waiting updates once their apps are closed
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"NorthRay Launcher";

static std::wstring w(const std::string& s) { return widen(s); }

// "2026-11-13" as "13 Nov 2026".
static std::wstring shortDate(const std::string& ymd) {
    static const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    const int m = std::atoi(ymd.substr(5, 2).c_str());
    if (ymd.size() != 10 || m < 1 || m > 12) return widen(ymd);
    return std::to_wstring(std::atoi(ymd.substr(8, 2).c_str())) + L" " + months[m - 1] + L" " + widen(ymd.substr(0, 4));
}

static std::wstring formatSize(unsigned long long bytes) {
    wchar_t buf[32];
    if (bytes >= 1024ULL * 1024) swprintf(buf, 32, L"%.1f MB", bytes / (1024.0 * 1024.0));
    else swprintf(buf, 32, L"%.0f KB", bytes / 1024.0);
    return buf;
}

// NorthRay's look: a northern night. The sidebar and banners are night sky with
// aurora light; the pages are cool, quiet neutrals so the apps stand out.
// The palette and the night sky are shared with the other NorthRay apps (cjui/northray).
using namespace cjui::northray;
static Palette northRayPalette(bool dark) { return cjui::northray::palette(dark); }

// A button on the night: aurora green with dark text.
void Launcher::nightButton(const std::string& id, const Rect& r, const std::wstring& label, std::function<void()> act, bool primary) {
    addHit(id, r, std::move(act));
    const float hv = hoverAmount(id);
    if (primary) {
        gfx_.fillRounded(r, 8, kAuroraGreen.mix(kNightInk, 0.25f * hv));
        gfx_.text(label, r, Font::BodyStrong, kNightTop, {Align::Center, false, false, true});
    } else {
        gfx_.fillRounded(r, 8, kNightInk.withAlpha(0.08f + 0.08f * hv));
        gfx_.strokeRounded(r, 8, kNightInk.withAlpha(0.28f), 1);
        gfx_.text(label, r, Font::Body, kNightInk, {Align::Center, false, false, true});
    }
    if (focused(id)) focusRing(r);
}

// ---------------------------------------------------------------- lifecycle

bool Launcher::create(HINSTANCE inst, int show, const std::string& openApp, bool startInTray) {
    model_.load();
    today_ = readToday();
    markPng_ = model_.dataDir + L"\\brand\\northray-mark.png";
    WindowSpec spec;
    spec.instance = inst;
    spec.className = L"NorthRayLauncher";
    spec.title = L"NorthRay Launcher";
    spec.iconId = IDI_APP;
    spec.width = 1120;
    spec.height = 720;
    spec.minWidth = 860;
    spec.minHeight = 560;
    if (startInTray && !model_.firstRun) show = SW_HIDE;   // started with Windows: just the tray icon
    else startInTray = false;
    spec.show = show;
    spec.customFrame = true;   // minimise, maximise and close are drawn in the window's own top area
    spec.captionHeight = 40;
    if (!createWindow(spec)) return false;
    applyTheme();
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (model_.settings.keepInTray || startInTray) addTrayIcon();
    if (model_.settings.startWithWindows) setStartWithWindows(true);   // keeps the path right if the launcher moved
    jobs_.start(hwnd(), WM_APP_JOBS, model_.stateDir + L"\\Downloads");
    if (!openApp.empty() && model_.app(openApp)) navigate(Page::App, openApp);
    else if (model_.firstRun) page_ = Page::Welcome;   // the first time: a short welcome
    ShowWindow(hwnd(), show);
    UpdateWindow(hwnd());
    SetTimer(hwnd(), kRefreshTimer, 6 * 3600 * 1000, nullptr);
    SetTimer(hwnd(), kAutoUpdateTimer, 20 * 60 * 1000, nullptr);
    checkCatalogue(false);
    return true;
}

void Launcher::onDestroy() {
    KillTimer(hwnd(), kRefreshTimer);
    KillTimer(hwnd(), kAutoUpdateTimer);
    removeTrayIcon();
    jobs_.stop();
}

void Launcher::applyTheme() {
    const bool dark = model_.settings.theme == ThemeChoice::Dark ||
                      (model_.settings.theme == ThemeChoice::System && systemPrefersDark());
    setCalmControls(true);   // the calm controls (UI pass, 2026-11)
    applyPalette(northRayPalette(dark));
    BOOL d = dark;
    DwmSetWindowAttribute(hwnd(), 20, &d, sizeof d);
    invalidate();
}

void Launcher::navigate(Page p, const std::string& appId) {
    page_ = p;
    lightbox_ = -1;
    if (!appId.empty()) appId_ = appId;
    resetScroll();
    clearFocus();
    invalidate();
}

// ------------------------------------------------------------ catalogue

namespace {
struct CatalogueResult { std::string body; std::wstring error; std::string url; bool userAsked = false; };
}

void Launcher::checkCatalogue(bool userAsked) {
    if (!model_.catalogueConfigured()) {
        if (userAsked) flash(L"There's no online catalogue set up yet, so updates can't be checked.", 8);
        return;
    }
    if (checking_) return;
    checking_ = true;
    const std::string url = model_.catalogueUrl;
    HWND target = hwnd();
    std::thread([url, target, userAsked] {
        auto* r = new CatalogueResult;
        r->url = url;
        r->userAsked = userAsked;
        cjui::HttpResult h = cjui::httpGet(url, 15000);
        if (h.ok()) r->body = h.body;
        else r->error = h.error.empty() ? L"The catalogue couldn't be reached." : h.error;
        if (!PostMessageW(target, WM_APP_CATALOGUE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
    invalidate();
}

LRESULT Launcher::onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    handled = true;
    if (msg == WM_APP_JOBS) {
        // A job moved on: refresh installs when one finishes.
        for (auto& a : model_.apps) {
            const JobState s = jobs_.state(a.entry.id);
            if ((s.stage == JobState::Done && !jobs_.busy(a.entry.id))) {
                const bool was = a.inst.found;
                model_.refreshInstalls();
                if (model_.app(a.entry.id)->inst.found && s.total) {
                    const std::wstring done = w(a.entry.name) + (s.update || was ? L" was updated." : L" is installed.");
                    flash(done, 6);
                    if (!IsWindowVisible(hwnd()))
                        notifyTray(done, model_.app(a.entry.id)->inst.version.empty() ? L"" : L"Version " + w(model_.app(a.entry.id)->inst.version));
                }
                jobs_.forget(a.entry.id);
            } else if (s.stage == JobState::Failed && !IsWindowVisible(hwnd()) && s.update) {
                notifyTray(w(a.entry.name) + L" couldn't be updated", s.message);
                jobs_.forget(a.entry.id);
            }
        }
        updateTrayState();
        invalidate();
        return 0;
    }
    if (msg == WM_APP_CATALOGUE) {
        std::unique_ptr<CatalogueResult> r(reinterpret_cast<CatalogueResult*>(lp));
        checking_ = false;
        if (!r->error.empty()) {
            model_.catalogueError = r->error;
            if (r->userAsked) flash(L"Couldn't check for updates: " + r->error, 8);
        } else {
            const std::wstring err = model_.applyCatalogue(r->body, r->url);
            model_.catalogueError = err;
            if (err.empty()) {
                model_.catalogueChecked = nowUtc();
                nlohmann::json c;
                c["catalogue"] = r->body;
                c["baseUrl"] = r->url;
                c["checked"] = model_.catalogueChecked;
                const std::wstring path = model_.stateDir + L"\\catalogue-cache.json";
                HANDLE f = CreateFileW((path + L".tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    const std::string t = c.dump();
                    DWORD wr = 0;
                    WriteFile(f, t.data(), (DWORD)t.size(), &wr, nullptr);
                    FlushFileBuffers(f);
                    CloseHandle(f);
                    MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
                }
                if (r->userAsked) {
                    int n = 0;
                    for (auto& a : model_.apps) if (a.updateAvailable()) ++n;
                    flash(n ? std::to_wstring(n) + L" update(s) available." : L"Everything is up to date.", 6);
                }
                if (model_.settings.autoUpdate) autoUpdate();
            } else if (r->userAsked) {
                flash(err, 8);
            }
        }
        updateTrayState();
        invalidate();
        return 0;
    }
    if (msg == WM_CLOSE) {
        // Closing the window leaves the launcher in the tray, unless it's really quitting
        // (its own update, or Quit) or the tray is turned off.
        if (!quitting_ && wp != 1 && trayAdded_) {
            hideToTray();
            return 0;
        }
        quitting_ = true;
        handled = false;
        return 0;
    }
    if (msg == WM_APP_TRAY) {
        switch (LOWORD(lp)) {
            case WM_LBUTTONUP: case NIN_BALLOONUSERCLICK: showFromTray(); break;
            case WM_RBUTTONUP: case WM_CONTEXTMENU: trayMenu(); break;
            default: break;
        }
        return 0;
    }
    if (taskbarCreated_ && msg == taskbarCreated_) {
        if (trayAdded_) { trayAdded_ = false; addTrayIcon(); }
        return 0;
    }
    if (msg == WM_ACTIVATEAPP && wp) today_ = readToday();   // words written or pictures saved meanwhile
    if (msg == WM_ACTIVATEAPP && !jobs_.anyBusy()) {
        model_.refreshInstalls();  // something may have been installed or removed meanwhile
        diskUse_.clear();
        invalidate();
    }
    handled = false;
    return 0;
}

void Launcher::onTimer(UINT_PTR id) {
    if (id == kRefreshTimer) checkCatalogue(false);
    if (id == kAutoUpdateTimer && model_.settings.autoUpdate && !jobs_.anyBusy()) {
        model_.refreshInstalls();
        autoUpdate();   // an update that waited for its app to close
    }
    invalidate();
}

// ------------------------------------------------------------- actions

void Launcher::install(const std::string& appId) {
    AppItem* a = model_.app(appId);
    if (!a || !a->release) return;
    if (a->inst.found && cjui::isAppRunning(appId)) {
        flash(L"Close " + w(a->entry.name) + L" first, then update it.", 6);
        return;
    }
    JobRequest r;
    r.appId = appId;
    r.release = *a->release;
    r.installDir = model_.installDirFor(appId);
    r.update = a->inst.found;
    jobs_.enqueue(r);
}

void Launcher::updateAll() {
    for (auto& a : model_.apps)
        if (a.updateAvailable() && !cjui::isAppRunning(a.entry.id)) install(a.entry.id);
}

void Launcher::autoUpdate() {
    for (auto& a : model_.apps)
        if (a.updateAvailable() && !cjui::isAppRunning(a.entry.id) && !jobs_.busy(a.entry.id)) install(a.entry.id);
}

void Launcher::uninstall(const std::string& appId) {
    AppItem* a = model_.app(appId);
    if (!a) return;
    const std::wstring q = L"Uninstall " + w(a->entry.name) + L"?\n\nIts own settings and documents are kept "
                           L"(for the Writing Studio, your library folder isn't touched).";
    if (cjui::messageBox(hwnd(), q.c_str(), L"Uninstall", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
    const std::wstring name = w(a->entry.name);
    const std::wstring err = model_.uninstall(appId);
    flash(err.empty() ? name + L" was uninstalled." : err, 6);
}

void Launcher::open(const std::string& appId) {
    const std::wstring err = model_.launch(appId);
    if (!err.empty()) flash(err, 6);
    else if (AppItem* a = model_.app(appId)) flash(L"Opening " + w(a->entry.name) + L"...", 4);
}

void Launcher::chooseInstallRoot() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose where NorthRay apps are installed");
    if (SUCCEEDED(dlg->Show(hwnd()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                model_.settings.installRoot = p;
                model_.saveSettings();
                CoTaskMemFree(p);
                flash(L"New installs will go to this folder. Installed apps stay where they are.", 6);
            }
            item->Release();
        }
    }
    dlg->Release();
}

// ---------------------------------------------------------------- tray

void Launcher::addTrayIcon() {
    if (trayAdded_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd();
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &nid.hIcon);
    lstrcpynW(nid.szTip, L"NorthRay Launcher", ARRAYSIZE(nid.szTip));
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (nid.hIcon) DestroyIcon(nid.hIcon);
    trayTip_.clear();
    trayProgress_ = -1;
    updateTrayState();
}

// The tray icon (0.97): while an app downloads or installs, a progress bar along the icon's foot and the app and
// percentage in its tooltip; otherwise the tooltip names the apps with updates ready.
void Launcher::updateTrayState() {
    if (!trayAdded_) return;
    std::wstring tip = L"NorthRay Launcher";
    int progress = -1;
    std::vector<std::wstring> ready;
    for (auto& a : model_.apps) {
        const JobState s = jobs_.state(a.entry.id);
        if (jobs_.busy(a.entry.id) && progress < 0) {
            const int pct = s.stage == JobState::Downloading && s.total ? (int)(s.done * 100 / s.total) : s.stage == JobState::Queued ? 0 : 100;
            progress = std::clamp(pct, 0, 100);
            tip = (s.update ? L"Updating " : L"Installing ") + w(a.entry.name) + L": " +
                  (s.stage == JobState::Downloading ? std::to_wstring(progress) + L"%" : s.stage == JobState::Queued ? std::wstring(L"waiting") : std::wstring(L"finishing"));
        } else if (a.updateAvailable()) {
            ready.push_back(w(a.entry.name));
        }
    }
    if (progress < 0 && !ready.empty()) {
        tip = ready.size() == 1 ? L"Update ready: " + ready[0] : L"Updates ready: ";
        if (ready.size() > 1)
            for (size_t i = 0; i < ready.size(); ++i) tip += (i ? (i + 1 == ready.size() ? L" and " : L", ") : L"") + ready[i];
    }
    const int bucket = progress < 0 ? -1 : progress / 4 * 4;   // the icon is redrawn every few per cent, not every packet
    if (tip == trayTip_ && bucket == trayProgress_) return;
    trayTip_ = tip;
    const bool redrawIcon = bucket != trayProgress_;
    trayProgress_ = bucket;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd();
    nid.uID = 1;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
    HICON made = nullptr, base = nullptr;
    if (redrawIcon) {
        LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &base);
        if (base && bucket >= 0) {
            // The icon drawn into a 32-bit picture, with a bar along its foot: dark track, aurora-green fill.
            const int sz = GetSystemMetrics(SM_CXSMICON);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = sz;
            bi.bmiHeader.biHeight = -sz;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HDC dc = CreateCompatibleDC(nullptr);
            HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (color && bits) {
                HGDIOBJ old = SelectObject(dc, color);
                std::memset(bits, 0, (size_t)sz * sz * 4);
                DrawIconEx(dc, 0, 0, base, sz, sz, 0, nullptr, DI_NORMAL);
                SelectObject(dc, old);
                auto* px = static_cast<std::uint32_t*>(bits);
                const int barH = std::max(3, sz / 5), fill = sz * bucket / 100;
                for (int yy = sz - barH; yy < sz; ++yy)
                    for (int xx = 0; xx < sz; ++xx) px[yy * sz + xx] = xx < fill ? 0xFF3FCF8Eu : 0xFF1B2232u;
                HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
                ICONINFO ii{TRUE, 0, 0, mask, color};
                made = CreateIconIndirect(&ii);
                DeleteObject(mask);
            }
            if (color) DeleteObject(color);
            DeleteDC(dc);
        }
        nid.uFlags |= NIF_ICON;
        nid.hIcon = made ? made : base;
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (made) DestroyIcon(made);
    if (base) DestroyIcon(base);
}

void Launcher::removeTrayIcon() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd();
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void Launcher::notifyTray(const std::wstring& title, const std::wstring& text) {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd();
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &nid.hBalloonIcon);
    lstrcpynW(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, text.empty() ? L" " : text.c_str(), ARRAYSIZE(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (nid.hBalloonIcon) DestroyIcon(nid.hBalloonIcon);
}

void Launcher::showFromTray() {
    ShowWindow(hwnd(), IsIconic(hwnd()) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd());
    model_.refreshInstalls();
    invalidate();
}

void Launcher::hideToTray() {
    ShowWindow(hwnd(), SW_HIDE);
    // The first time ever, say where it went.
    const std::wstring marker = model_.stateDir + L"\\tray-explained";
    if (GetFileAttributesW(marker.c_str()) == INVALID_FILE_ATTRIBUTES) {
        notifyTray(L"NorthRay Launcher is still running", L"It stays here so app updates can install. Right-click this icon to quit, or turn this off in Settings.");
        HANDLE f = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
}

void Launcher::quit() {
    if (jobs_.anyBusy() &&
        cjui::messageBox(hwnd(), L"An install or update is still running. Quit anyway? A download is stopped; a setup that has started finishes on its own.",
                    L"NorthRay Launcher", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
        return;
    quitting_ = true;
    DestroyWindow(hwnd());
}

void Launcher::trayMenu() {
    enum : UINT { IdShow = 1, IdCheck, IdQuit, IdApp = 100, IdBook = 200, IdScene = 300 };
    today_ = readToday();   // what's been written and which scene Capture is on, as of now
    std::vector<MenuItem> items;
    items.push_back(MenuItem::item(L"Open NorthRay Launcher", IdShow));
    // Jump back in (0.99): a recent book at its chapter, or Capture on a scene.
    if (model_.settings.showJumpBack) {
        const AppItem* studio = model_.app("writing-studio");
        const AppItem* capture = model_.app("capture");
        bool sep = false;
        if (studio && studio->inst.found && !today_.books.empty()) {
            std::vector<MenuItem> sub;
            for (std::size_t i = 0; i < today_.books.size(); ++i)
                sub.push_back(MenuItem::item(today_.books[i].title + (today_.books[i].chapter.empty() ? L"" : L"  ·  " + today_.books[i].chapter),
                                             IdBook + (UINT)i));
            items.push_back(MenuItem::sep());
            sep = true;
            items.push_back(MenuItem::sub(L"Continue writing", sub));
        }
        if (capture && capture->inst.found && today_.capture && !today_.scenes.empty()) {
            std::vector<MenuItem> sub;
            for (std::size_t i = 0; i < today_.scenes.size(); ++i)
                sub.push_back(MenuItem::item(widen(today_.scenes[i]), IdScene + (UINT)i, {}, true, today_.scenes[i] == today_.currentScene));
            if (!sep) items.push_back(MenuItem::sep());
            items.push_back(MenuItem::sub(L"Open Capture on", sub));
        }
    }
    std::vector<std::string> ids;
    bool any = false;
    // NorthRay's apps.
    for (int pass = 0; pass < 1; ++pass)
        for (auto& a : model_.apps) {
            if (!a.inst.found) continue;
            if (!any) { items.push_back(MenuItem::sep()); any = true; }
            items.push_back(MenuItem::item(L"Open " + w(a.entry.name), IdApp + (UINT)ids.size()));
            ids.push_back(a.entry.id);
        }
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(checking_ ? L"Checking for updates..." : L"Check for updates", IdCheck, {}, !checking_));
    items.push_back(MenuItem::item(L"Quit NorthRay Launcher", IdQuit));
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd());   // so the menu closes when you click elsewhere
    const UINT id = showMenu(hwnd(), items, pt, pal_, MenuAnchor::TopRight);
    PostMessageW(hwnd(), WM_NULL, 0, 0);
    if (id == IdShow) showFromTray();
    else if (id == IdCheck) checkCatalogue(!IsWindowVisible(hwnd()) ? false : true);
    else if (id == IdQuit) quit();
    else if (id >= IdBook && id < IdScene && id - IdBook < today_.books.size()) continueBook(today_.books[id - IdBook]);
    else if (id >= IdScene && id - IdScene < today_.scenes.size()) openCaptureOn(today_.scenes[id - IdScene]);
    else if (id >= IdApp && id < IdBook && id - IdApp < ids.size()) open(ids[id - IdApp]);
}

void Launcher::setStartWithWindows(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        flash(L"Windows didn't allow the change to start-up apps.", 6);
        return;
    }
    if (on) {
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
        const std::wstring cmd = L"\"" + std::wstring(exe, n) + L"\" --tray";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// A tick box with a label; `night` for the welcome tour's dark background.
void Launcher::checkbox(const std::string& id, const Rect& r, const std::wstring& label, bool on, std::function<void()> toggle, bool night) {
    addHit(id, r, std::move(toggle));
    const float hv = hoverAmount(id);
    const Color edge = night ? kNightInk.withAlpha(0.6f + 0.4f * hv) : pal_.inkMuted.mix(pal_.ink, hv);
    const Color tick = night ? kAuroraGreen : pal_.paper;
    const Rect b{r.x, r.y + 3, 18, 18};
    if (on) gfx_.fillRounded(b, 4, night ? kNightInk.withAlpha(0.12f) : pal_.ribbon);
    gfx_.strokeRounded(b, 4, on && !night ? pal_.ribbon : edge, 1.5f);
    if (on) {
        gfx_.line(b.x + 4, b.y + 9, b.x + 8, b.y + 14, tick, 2);
        gfx_.line(b.x + 8, b.y + 14, b.x + 15, b.y + 4, tick, 2);
    }
    gfx_.text(label, {r.x + 30, r.y + 1, r.w - 30, 22}, Font::Body, night ? kNightInk : pal_.ink, {Align::Left, false});
    if (focused(id)) focusRing(r);
}

bool Launcher::onKeyDown(WPARAM vk, bool ctrl, bool) {
    if (ctrl && vk >= '1' && vk <= '3') {
        navigate(vk == '1' ? Page::Library : vk == '2' ? Page::Updates : Page::Settings);
        return true;
    }
    if (vk == VK_F5) { model_.load(); checkCatalogue(true); return true; }
    if (lightbox_ >= 0) {
        const AppItem* a = model_.app(appId_);
        const int n = a ? (int)a->screenshots.size() : 0;
        if (vk == VK_ESCAPE || vk == VK_BACK) lightbox_ = -1;
        else if (n && (vk == VK_RIGHT || vk == VK_LEFT)) lightbox_ = (lightbox_ + (vk == VK_RIGHT ? 1 : n - 1)) % n;
        invalidate();
        return true;
    }
    if (page_ == Page::Welcome) {
        if (vk == VK_RETURN && hasKeyboardFocus()) return false;   // Enter on a focused control presses it
        if (vk == VK_RETURN) { if (welcomeStep_ < 2) { welcomeStep_++; invalidate(); } else finishWelcome(); return true; }
        if (vk == VK_BACK && welcomeStep_ > 0) { welcomeStep_--; invalidate(); return true; }
        return false;
    }
    if ((vk == VK_BACK || vk == VK_ESCAPE) && page_ == Page::App) { navigate(Page::Library); return true; }
    return false;
}

// --------------------------------------------------------------- chrome

void Launcher::drawAurora(const Rect& r) {
    // A thin band of northern light: the brand's one ornament.
    auroraBand(gfx_, r, pal_.dark);
}

void Launcher::drawIcon(const AppItem& a, const Rect& r) {
    if (!a.iconPath.empty() && gfx_.drawImage(a.iconPath, r, false)) return;
    const Color c = hexColor(a.entry.accent, pal_.ribbon);
    gfx_.fillRounded(r, r.w * 0.2f, c);
    gfx_.text(w(a.entry.name.substr(0, 1)), r, Font::Monogram, hexColor("#FFFFFF"), {Align::Center, false, false, true});
}

void Launcher::drawNav(const Rect& r) {
    // The sidebar is the night sky, whatever the theme.
    nightSky(gfx_, r, 0, 0.9f, 7);
    drawAurora({r.x, r.y, r.w, 3});
    float x = r.x + 20;
    if (gfx_.drawImage(markPng_, {x, r.y + 22, 36, 36}, false)) x += 46;
    gfx_.text(L"NorthRay", {x, r.y + 18, r.w - x, 24}, Font::Wordmark, kNightInk, {Align::Left, false});
    gfx_.text(L"LAUNCHER", {x + 1, r.y + 43, r.w - x, 16}, Font::Label, kAuroraGreen.withAlpha(0.85f), {Align::Left, false});

    int updates = 0;
    for (auto& a : model_.apps) if (a.updateAvailable()) ++updates;
    struct Item { Page page; const wchar_t* label; std::wstring badge; };
    const Item items[] = {{Page::Library, L"Library", L""},
                          {Page::Updates, L"Updates", updates ? std::to_wstring(updates) : L""},
                          {Page::Settings, L"Settings", L""}};
    const float y0 = r.y + 92;
    float y = y0;
    for (int i = 0; i < 3; ++i)
        if (page_ == items[i].page || (page_ == Page::App && items[i].page == Page::Library)) {
            const float sy = slideTo("nav#sel", y0 + 40.0f * i);
            gfx_.fillRounded({r.x + 12, sy, r.w - 24, 36}, 8, kNightInk.withAlpha(0.09f));
            gfx_.fillRounded({r.x + 12, sy + 10, 3, 16}, 1.5f, kAuroraGreen);
        }
    for (const Item& it : items) {
        const std::string id = "nav:" + std::to_string((int)it.page);
        const Rect row{r.x + 12, y, r.w - 24, 36};
        addHit(id, row, [this, p = it.page] { navigate(p); });
        const bool sel = page_ == it.page || (page_ == Page::App && it.page == Page::Library);
        const float hv = hoverAmount(id);
        if (!sel && hv > 0.01f) gfx_.fillRounded(row, 8, kNightInk.withAlpha(0.05f * hv));
        gfx_.text(it.label, {row.x + 18, row.y, row.w - 60, row.h}, sel ? Font::BodyStrong : Font::Body,
                  sel ? kNightInk : kNightMuted.mix(kNightInk, hv * 0.5f), {Align::Left, false, true, true});
        if (!it.badge.empty()) {
            const Rect b{row.right() - 36, row.y + 9, 24, 18};
            gfx_.fillRounded(b, 9, kAuroraGreen);
            gfx_.text(it.badge, b, Font::Label, kNightTop, {Align::Center, false, false, true});
        }
        if (focused(id)) focusRing(row);
        y += 40;
    }
    // The website, at the foot of the menu (2026-11-23): opens in the browser, so it's marked as going out.
    {
        const std::string id = "nav:website";
        const Rect row{r.x + 12, r.bottom() - 100, r.w - 24, 36};
        addHit(id, row, [] { cjui::northray::openWebsite(); });
        const float hv = hoverAmount(id);
        if (hv > 0.01f) gfx_.fillRounded(row, 8, kNightInk.withAlpha(0.05f * hv));
        const Color ink = kNightMuted.mix(kNightInk, hv * 0.5f);
        cjui::drawIcon(gfx_, cjui::Icon::Link, {row.x + 16, row.y + 10, 16, 16}, ink, 1.4f);
        gfx_.text(L"Website", {row.x + 40, row.y, row.w - 70, row.h}, Font::Body, ink, {Align::Left, false, true, true});
        {   // a small arrow out of the window: it opens in the browser
            const float ax = row.right() - 24, ay = row.y + 13, d = 9;
            gfx_.line(ax, ay + d, ax + d, ay, ink.withAlpha(0.8f), 1.3f);
            gfx_.line(ax + 3, ay, ax + d, ay, ink.withAlpha(0.8f), 1.3f);
            gfx_.line(ax + d, ay, ax + d, ay + d - 3, ink.withAlpha(0.8f), 1.3f);
        }
        tooltip(id, L"NorthRay Software's website, in your browser");
        if (focused(id)) focusRing(row);
    }
    gfx_.text(L"Developed by the " CJ_DEVELOPER_WSTR L" project", {r.x + 20, r.bottom() - 52, r.w - 30, 36}, Font::Small,
              kNightMuted.withAlpha(0.8f));
}

float Launcher::drawHeader(const Rect& r, const std::wstring& title, const std::wstring& subtitle) {
    gfx_.text(title, {r.x, r.y, r.w, 36}, Font::Title, pal_.ink, {Align::Left, false, true});
    float h = 40;
    if (!subtitle.empty()) h += gfx_.text(subtitle, {r.x, r.y + h, std::min(r.w, 680.0f), 120}, Font::Body, pal_.inkMuted) + 4;
    return h + 20;
}

void Launcher::progressBar(const Rect& r, float f, const std::string& key) {
    gfx_.fillRounded(r, r.h / 2, pal_.sunken);
    // The bar glides to each new value instead of jumping between chunks.
    const float shown = key.empty() ? f : anim("progress#" + key, std::clamp(f, 0.0f, 1.0f), 8.0f);
    if (shown > 0) {
        const Rect fillR{r.x, r.y, std::max(r.h, r.w * shown), r.h};
        gfx_.fillRounded(fillR, r.h / 2, pal_.ribbon);
        // A soft highlight drifts along the filled part while it's working.
        if (shown < 0.999f && animationsEnabled()) {
            const float t = (float)((GetTickCount64() % 1400) / 1400.0);
            const float hx = fillR.x + (fillR.w + 40) * t - 40;
            gfx_.pushClip(fillR);
            gfx_.fillRounded({hx, fillR.y, 40, fillR.h}, r.h / 2, pal_.paper.withAlpha(0.25f));
            gfx_.popClip();
            invalidate();
        }
    }
}

// When the app's files were last put in place on this PC (its program's date): "Updated today", "Updated 18 Nov".
// ---------------------------------------------------------------- dragging app cards (0.97)

bool Launcher::onMouseDown(float x, float y) {
    dragApp_.clear();
    dragging_ = false;
    if (page_ != Page::Library) return false;
    const std::string hit = hitAt(x, y);
    if (hit.rfind("app:", 0) != 0) return false;   // only the card itself, never its buttons
    dragApp_ = hit.substr(4);
    dragX_ = x;
    dragY_ = y;
    return false;   // a click still opens the app's page
}

void Launcher::onMouseMove(float x, float y, bool leftDown) {
    if (dragApp_.empty() || !leftDown) return;
    if (!dragging_ && std::hypot(x - dragX_, y - dragY_) > 8) {
        dragging_ = true;
        cancelPress();
    }
    if (dragging_) invalidate();
}

bool Launcher::onMouseUp(float x, float y) {
    (void)x;
    if (!dragging_) { dragApp_.clear(); return false; }
    dragging_ = false;
    const std::string moving = dragApp_;
    dragApp_.clear();
    std::vector<std::string> order;
    for (auto& [id, cr] : cardRects_) order.push_back(id);
    std::string target;
    bool after = false;
    for (auto& [id, cr] : cardRects_)
        if (y >= cr.y - 7 && y < cr.bottom() + 7) { target = id; after = y >= cr.y + cr.h / 2; break; }
    if (target.empty() || target == moving) { invalidate(); return true; }
    order.erase(std::remove(order.begin(), order.end(), moving), order.end());
    auto it = std::find(order.begin(), order.end(), target);
    order.insert(after ? it + 1 : it, moving);
    // Apps not shown just now (filtered out) keep their places after these.
    for (const std::string& id : model_.settings.appOrder)
        if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
    model_.settings.appOrder = order;
    model_.saveSettings();
    invalidate();
    return true;
}

std::wstring Launcher::lastAppUpdated(const AppItem& a) const {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (a.inst.exe.empty() || !GetFileAttributesExW(a.inst.exe.c_str(), GetFileExInfoStandard, &fa)) return {};
    FILETIME local;
    SYSTEMTIME st, now;
    if (!FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &st)) return {};
    GetLocalTime(&now);
    if (st.wYear == now.wYear && st.wMonth == now.wMonth && st.wDay == now.wDay) return L"updated today";
    static const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    std::wstring d = L"updated " + std::to_wstring(st.wDay) + L" " + months[(st.wMonth + 11) % 12];
    if (st.wYear != now.wYear) d += L" " + std::to_wstring(st.wYear);
    return d;
}

std::wstring Launcher::statusLine(const AppItem& a) const {
    const JobState s = jobs_.state(a.entry.id);
    switch (s.stage) {
        case JobState::Queued: return L"Waiting to start...";
        case JobState::Downloading: {
            if (!s.message.empty() && s.done == 0) return s.message;   // waiting to try again
            std::wstring t = L"Downloading  " + formatSize(s.done) + L" of " + formatSize(s.total);
            if (s.rate > 1024) {   // speed and time left (0.97)
                t += L"  \u00B7  " + formatSize((unsigned long long)s.rate) + L"/s";
                const double left = s.total > s.done ? (double)(s.total - s.done) / s.rate : 0;
                if (left >= 1) {
                    const int secs = (int)std::ceil(left);
                    t += L"  \u00B7  about " + (secs < 60 ? std::to_wstring(secs) + (secs == 1 ? L" second" : L" seconds")
                                                : std::to_wstring((secs + 30) / 60) + ((secs + 30) / 60 == 1 ? L" minute" : L" minutes")) + L" left";
                }
            }
            if (s.attempt > 1) t += L"  \u00B7  try " + std::to_wstring(s.attempt) + L" of " + std::to_wstring(Jobs::kAttempts);
            return t;
        }
        case JobState::Verifying: return L"Checking the download...";
        case JobState::Installing: return s.update ? L"Updating..." : L"Installing...";
        case JobState::Failed: return s.message;
        case JobState::Cancelled: return L"Cancelled.";
        default: break;
    }
    if (a.updateAvailable()) return L"Update available: " + w(a.release->version) + L" (you have " + w(a.inst.version) + L")";
    if (a.inst.found) {
        std::wstring t = L"Installed" + (a.inst.version.empty() ? std::wstring() : L", version " + w(a.inst.version));
        if (const std::wstring u = lastAppUpdated(a); !u.empty()) t += L"  ·  " + u;
        if (cjui::isAppRunning(a.entry.id)) t += L"  ·  running";
        return t;
    }
    if (a.release) return L"Available, version " + w(a.release->version) + L"  ·  " + formatSize(a.release->size);
    return model_.catalogueConfigured() ? L"Not available to download yet" : L"Not installed";
}

// The buttons for an app: Install / Update / Open (+ Cancel during a download).
void Launcher::actionArea(const AppItem& a, const Rect& r, const std::string& prefix, bool compact) {
    const std::string id = a.entry.id;
    const JobState s = jobs_.state(id);
    const bool busy = jobs_.busy(id);
    const float bw = compact ? 110 : 130;
    if (busy) {
        const float frac = s.stage == JobState::Downloading && s.total ? (float)s.done / s.total
                           : s.stage == JobState::Queued ? 0.0f : 1.0f;
        progressBar({r.x, r.y + r.h / 2 - 3, r.w - bw - 12, 6}, frac, prefix + id);
        if (s.stage == JobState::Queued || s.stage == JobState::Downloading)
            button(prefix + "cancel:" + id, {r.right() - bw, r.y, bw, r.h}, L"Cancel", [this, id] { jobs_.cancel(id); });
        else
            disabledButton({r.right() - bw, r.y, bw, r.h}, s.update ? L"Updating" : L"Installing");
        return;
    }
    float x = r.right() - bw;
    if (a.updateAvailable()) {
        button(prefix + "update:" + id, {x, r.y, bw, r.h}, L"Update", [this, id] { install(id); }, true);
        x -= bw + 8;
        button(prefix + "open:" + id, {x, r.y, bw, r.h}, L"Open", [this, id] { open(id); });
    } else if (a.inst.found) {
        button(prefix + "open:" + id, {x, r.y, bw, r.h}, cjui::isAppRunning(id) ? L"Show" : L"Open", [this, id] { open(id); }, true);
    } else if (a.release) {
        button(prefix + "install:" + id, {x, r.y, bw, r.h}, L"Install", [this, id] { install(id); }, true);
    } else {
        disabledButton({x, r.y, bw, r.h}, L"Install");
    }
}

// ---------------------------------------------------------------- frame

void Launcher::drawFrame(float W, float H) {
    if (page_ == Page::Welcome) { drawWelcome(W, H); return; }
    gfx_.fill({0, 0, W, H}, pal_.paper);
    const float pad = W - kNavW > 900 ? 44 : 30;
    const float capH = 40;   // the top strip: moves the window, holds minimise, maximise and close
    const float top = beginScroll({kNavW, capH, W - kNavW, H - capH});
    Rect page{kNavW + pad, top + 4, std::min(W - kNavW - pad * 2, 960.0f), 0};
    float h = 0;
    switch (page_) {
        case Page::Library: h = pageLibrary(page); break;
        case Page::App: h = model_.app(appId_) ? pageApp(page) : pageLibrary(page); break;
        case Page::Updates: h = pageUpdates(page); break;
        case Page::Settings: h = pageSettings(page); break;
        case Page::Welcome: break;
    }
    endScroll(h + 60);
    gfx_.fill({kNavW, 0, W - kNavW, capH}, pal_.paper);
    drawCaptionButtons({kNavW, 0, W - kNavW, capH}, false);
    drawNav({0, 0, kNavW, H});
    drawToast({kNavW, capH, W - kNavW, H - capH});   // messages (no status bar)
    drawLightbox(W, H);
}

// The first run: a short tour in three steps. What the launcher is, where apps go,
// and how updates and the tray work.
void Launcher::drawWelcome(float W, float H) {
    nightSky(gfx_, {0, 0, W, H}, 0, 1.0f, 21);
    drawAurora({0, 0, W, 3});
    drawCaptionButtons({0, 3, W, 37}, true);
    const float cw = std::min(W - 80, 620.0f), x = (W - cw) / 2;
    float y = std::max(40.0f, (H - 520) / 2);
    const int step = std::clamp(welcomeStep_, 0, 2);
    // Where you are in the tour.
    for (int i = 0; i < 3; ++i) {
        const Rect dot{x + i * 34.0f, y, 26, 4};
        gfx_.fillRounded(dot, 2, i <= step ? kAuroraGreen : kNightInk.withAlpha(0.18f));
    }
    gfx_.text(L"Step " + std::to_wstring(step + 1) + L" of 3", {x + 110, y - 7, 200, 18}, Font::Small, kNightMuted, {Align::Left, false});
    y += 34;

    if (step == 0) {
        gfx_.drawImage(markPng_, {x, y, 64, 64}, false);
        y += 88;
        gfx_.text(L"WELCOME", {x, y, cw, 16}, Font::Label, kAuroraGreen, {Align::Left, false});
        y += 22;
        gfx_.text(L"NorthRay Launcher", {x, y, cw, 44}, Font::Title, kNightInk, {Align::Left, false});
        y += 52;
        y += gfx_.text(L"One place to install, update and open apps from " CJ_DEVELOPER_WSTR L". They're free, work offline, and none of "
                       L"them needs an account.",
                       {x, y, cw, 80}, Font::Body, kNightMuted) + 14;
        y += gfx_.text(L"Every download is checked against the checksum published with it before anything runs, so a damaged "
                       L"or altered installer is never used.",
                       {x, y, cw, 80}, Font::Body, kNightMuted) + 30;
    } else if (step == 1) {
        gfx_.text(L"WHERE APPS GO", {x, y, cw, 16}, Font::Label, kAuroraGreen, {Align::Left, false});
        y += 22;
        gfx_.text(L"Choose an install folder", {x, y, cw, 44}, Font::Title, kNightInk, {Align::Left, false});
        y += 52;
        y += gfx_.text(L"Each app gets its own folder inside this one. The default needs no administrator rights. You can "
                       L"change it later in Settings; apps already installed stay where they are.",
                       {x, y, cw, 80}, Font::Body, kNightMuted) + 20;
        const Rect box{x, y, cw - 130, 36};
        gfx_.fillRounded(box, 6, kNightInk.withAlpha(0.07f));
        gfx_.strokeRounded(box, 6, kNightInk.withAlpha(0.15f));
        gfx_.text(model_.effectiveInstallRoot(), box.inset(12, 0), Font::Small, kNightInk, {Align::Left, false, true, true});
        nightButton("wel:root", {box.right() + 10, y + 1, 120, 34}, L"Change...", [this] { chooseInstallRoot(); invalidate(); }, false);
        y += 50;
        if (!model_.settings.installRoot.empty()) {
            const std::string id = "wel:rootreset";
            const Rect l{x, y, 200, 20};
            addHit(id, l, [this] { model_.settings.installRoot.clear(); invalidate(); });
            gfx_.text(L"Use the default folder", l, Font::Small, kAuroraGreen.mix(kNightInk, 0.4f * hoverAmount(id)), {Align::Left, false});
            if (focused(id)) focusRing(l);
            y += 26;
        }
        y += 20;
    } else {
        gfx_.text(L"UPDATES", {x, y, cw, 16}, Font::Label, kAuroraGreen, {Align::Left, false});
        y += 22;
        gfx_.text(L"Stay up to date by itself", {x, y, cw, 44}, Font::Title, kNightInk, {Align::Left, false});
        y += 56;
        auto& st = model_.settings;
        checkbox("wel:auto", {x, y, cw, 26}, L"Install updates automatically", st.autoUpdate,
                 [this] { model_.settings.autoUpdate = !model_.settings.autoUpdate; invalidate(); }, true);
        y += 28;
        y += gfx_.text(L"Never while an app is open: its update waits until you close it.", {x + 30, y, cw - 30, 40}, Font::Small,
                       kNightMuted) + 14;
        checkbox("wel:tray", {x, y, cw, 26}, L"Keep running in the notification area when closed", st.keepInTray,
                 [this] { model_.settings.keepInTray = !model_.settings.keepInTray; invalidate(); }, true);
        y += 28;
        y += gfx_.text(L"So updates can install while the window is closed. Its icon there opens your apps quickly.",
                       {x + 30, y, cw - 30, 40}, Font::Small, kNightMuted) + 14;
        checkbox("wel:startup", {x, y, cw, 26}, L"Start with Windows", st.startWithWindows,
                 [this] { model_.settings.startWithWindows = !model_.settings.startWithWindows; invalidate(); }, true);
        y += 28;
        y += gfx_.text(L"Starts quietly in the notification area when you sign in. Off unless you turn it on.", {x + 30, y, cw - 30, 40},
                       Font::Small, kNightMuted) + 30;
    }
    const float by = std::max(y, 0.0f);
    if (step < 2) nightButton("wel:next", {x, by, 150, 40}, L"Next", [this] { welcomeStep_++; clearFocus(); invalidate(); });
    else nightButton("wel:go", {x, by, 170, 40}, L"Get started", [this] { finishWelcome(); });
    if (step > 0) nightButton("wel:back", {x + (step < 2 ? 162.0f : 182.0f), by, 110, 40}, L"Back", [this] { welcomeStep_--; clearFocus(); invalidate(); }, false);
    if (step < 2) {
        const std::string id = "wel:skip";
        const Rect l{x + cw - 120, by + 10, 120, 20};
        addHit(id, l, [this] { finishWelcome(); });
        gfx_.text(L"Skip the tour", l, Font::Small, kNightMuted.mix(kNightInk, hoverAmount(id)), {Align::Right, false});
        if (focused(id)) focusRing(l);
    }
}

void Launcher::finishWelcome() {
    model_.saveSettings();   // the settings file now exists: no welcome next time
    if (model_.settings.keepInTray) addTrayIcon(); else removeTrayIcon();
    setStartWithWindows(model_.settings.startWithWindows);
    model_.firstRun = false;
    navigate(Page::Library);
}

// A screenshot shown large over everything; a click or Esc closes it, arrows step.
void Launcher::drawLightbox(float W, float H) {
    const AppItem* a = page_ == Page::App ? model_.app(appId_) : nullptr;
    if (!a || lightbox_ < 0 || lightbox_ >= (int)a->screenshots.size()) { lightbox_ = -1; return; }
    const Screenshot& s = a->screenshots[lightbox_];
    gfx_.fill({0, 0, W, H}, kNightTop.withAlpha(0.88f));
    addHit("lightbox", {0, 0, W, H}, [this] { lightbox_ = -1; invalidate(); }, false);
    const auto [iw, ih] = gfx_.imageSize(s.path);
    const float maxW = W - 160, maxH = H - 140;
    float dw = maxW, dh = maxH;
    if (iw && ih) {
        const float k = std::min(maxW / iw, maxH / ih);
        dw = iw * std::min(k, 1.5f);
        dh = ih * std::min(k, 1.5f);
    }
    const Rect pic{(W - dw) / 2, (H - dh) / 2 - 16, dw, dh};
    gfx_.drawImage(s.path, pic, false);
    gfx_.stroke(pic, kNightInk.withAlpha(0.2f));
    std::wstring cap = w(s.caption);
    cap += (cap.empty() ? L"" : L"   ·   ") + std::to_wstring(lightbox_ + 1) + L" of " + std::to_wstring(a->screenshots.size());
    gfx_.text(cap, {80, pic.bottom() + 14, W - 160, 22}, Font::Small, kNightMuted, {Align::Center, false, true});
    const int n = (int)a->screenshots.size();
    if (n > 1) {
        auto arrow = [&](const std::string& id, float x, const wchar_t* t, int step) {
            const Rect b{x, H / 2 - 30, 48, 60};
            addHit(id, b, [this, step, n] { lightbox_ = (lightbox_ + step + n) % n; invalidate(); });
            gfx_.fillRounded(b, 8, kNightInk.withAlpha(0.06f + 0.1f * hoverAmount(id)));
            gfx_.text(t, b, Font::Title, kNightInk, {Align::Center, false, false, true});
        };
        arrow("lightbox:prev", 20, L"‹", -1);
        arrow("lightbox:next", W - 68, L"›", 1);
    }
}

// ---------------------------------------------------------------- pages

float Launcher::drawToday(const Rect& r) {
    const AppItem* studio = model_.app("writing-studio");
    const bool writing = today_.writing && studio && studio->inst.found;
    const bool pictures = !today_.pictures.empty();
    if (!writing && !pictures) return 0;
    float y = r.y;
    y += sectionLabel(r.x, y, r.w, L"Today");
    const float gap = 16, h = 150;
    const float cw = writing && pictures ? (r.w - gap) / 2 : r.w;
    float x = r.x;
    auto card = [&](const Rect& c) {
        softShadow(c, 10, 0.3f);
        gfx_.fillRounded(c, 12, pal_.surface);
        gfx_.strokeRounded(c, 12, pal_.rule);
    };
    if (writing) {
        const Rect c{x, y, cw, h};
        card(c);
        gfx_.text(L"WRITING", {c.x + 22, c.y + 18, c.w - 44, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        wchar_t b[96];
        swprintf(b, 96, L"%lld %ls today", today_.wordsToday, today_.wordsToday == 1 ? L"word" : L"words");
        gfx_.text(b, {c.x + 22, c.y + 40, c.w - 44, 34}, Font::Title, pal_.ink, {Align::Left, false, true});
        std::wstring sub;
        if (today_.dailyGoal > 0)
            sub = today_.wordsToday >= today_.dailyGoal ? L"Goal of " + std::to_wstring(today_.dailyGoal) + L" reached"
                                                        : std::to_wstring(today_.dailyGoal - today_.wordsToday) + L" to your goal of " +
                                                              std::to_wstring(today_.dailyGoal);
        if (today_.streak > 0) sub += (sub.empty() ? L"" : L"  \u00b7  ") + std::to_wstring(today_.streak) + (today_.streak == 1 ? L" day" : L" days") + L" in a row";
        if (sub.empty()) sub = L"Nothing written yet today.";
        gfx_.text(sub, {c.x + 22, c.y + 80, c.w - 44, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        if (today_.dailyGoal > 0)
            progressBar({c.x + 22, c.y + 108, c.w - 200, 6}, std::min(1.0f, (float)today_.wordsToday / (float)today_.dailyGoal), "today:goal");
        button("today:studio", {c.right() - 160, c.bottom() - 50, 140, 34}, L"Open Writing Studio", [this] { open("writing-studio"); });
        x += cw + gap;
    }
    if (pictures) {
        const Rect c{x, y, cw, h};
        card(c);
        gfx_.text(L"RECENT PICTURES", {c.x + 22, c.y + 18, c.w - 44, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
        const float th = h - 64, tw = th * 16 / 10;
        float px = c.x + 22;
        for (std::size_t i = 0; i < today_.pictures.size(); ++i) {
            const Rect t{px, c.y + 42, tw, th};
            if (t.right() > c.right() - 22) break;
            const std::string id = "today:pic" + std::to_string(i);
            const std::wstring path = today_.pictures[i];
            addHit(id, t, [path] { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
            gfx_.fillRounded(t, 8, pal_.sunken);
            gfx_.pushClip(t);
            gfx_.drawImage(path, t, true);
            gfx_.popClip();
            gfx_.strokeRounded(t, 8, pal_.rule.mix(pal_.ribbon, hoverAmount(id)));
            if (focused(id)) focusRing(t);
            px += tw + 10;
        }
    }
    return y + h - r.y;
}

// Jump back in (0.99): the books last written in, each opening Writing Studio at its chapter, and NorthRay Capture on
// the scene it was on (or another of its scenes). Only for installed apps; read from their files on this PC.
float Launcher::drawJumpBack(const Rect& r) {
    const AppItem* studio = model_.app("writing-studio");
    const AppItem* capture = model_.app("capture");
    const bool books = studio && studio->inst.found && !today_.books.empty();
    const bool cap = capture && capture->inst.found && today_.capture && !today_.scenes.empty();
    if (!books && !cap) return 0;
    float y = r.y;
    y += sectionLabel(r.x, y, r.w, L"Jump back in");
    const float gap = 16, h = 132;
    const int nBooks = books ? (int)std::min<std::size_t>(today_.books.size(), cap ? 3 : 4) : 0;
    const int n = nBooks + (cap ? 1 : 0);
    const float cw = (r.w - gap * (n - 1)) / n;
    float x = r.x;
    auto card = [&](const Rect& c, const std::string& id) {
        softShadow(c, 10, 0.3f + 0.2f * hoverAmount(id));
        gfx_.fillRounded(c, 12, pal_.surface);
        gfx_.strokeRounded(c, 12, pal_.rule.mix(pal_.ribbon, hoverAmount(id)));
    };
    for (int i = 0; i < nBooks; ++i) {
        const Today::Book b = today_.books[(size_t)i];
        const Rect c{x, y, cw, h};
        const std::string id = "jump:book" + std::to_string(i);
        addHit(id, c, [this, b] { continueBook(b); });
        tooltip(id, L"Open Writing Studio at " + (b.chapter.empty() ? b.title : b.chapter));
        card(c, id);
        // The cover, or a plain book in the aurora green with the first letter.
        const Rect cov{c.x + 16, c.y + 16, 66, 100};
        bool drawn = false;
        if (!b.cover.empty() && GetFileAttributesW(b.cover.c_str()) != INVALID_FILE_ATTRIBUTES) {
            gfx_.pushClip(cov);
            drawn = gfx_.drawImage(b.cover, cov, true);
            gfx_.popClip();
        }
        if (!drawn) {
            gfx_.fillRounded(cov, 4, pal_.ribbon.withAlpha(0.16f));
            gfx_.text(b.title.empty() ? L"?" : b.title.substr(0, 1), cov, Font::Title, pal_.ribbon, {Align::Center, false, false, true});
        }
        gfx_.strokeRounded(cov, 4, pal_.rule);
        const float tx = cov.right() + 14, tw = c.right() - 16 - tx;
        gfx_.text(L"WRITING STUDIO", {tx, c.y + 18, tw, 16}, Font::Label, pal_.inkFaint, {Align::Left, false, true});
        gfx_.text(b.title, {tx, c.y + 36, tw, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        float ly = c.y + 58;
        if (!b.series.empty()) { gfx_.text(b.series, {tx, ly, tw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true}); ly += 18; }
        if (!b.chapter.empty()) { gfx_.text(b.chapter, {tx, ly, tw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true}); ly += 18; }
        std::wstring more = i == 0 ? L"Continue ›" : L"Open ›";
        if (b.words > 0) {
            std::wstring num = std::to_wstring(b.words);
            for (int k = (int)num.size() - 3; k > 0; k -= 3) num.insert((size_t)k, L",");
            more = num + L" words  \u00b7  " + more;
        }
        gfx_.text(more, {tx, c.bottom() - 30, tw, 18}, Font::Small, pal_.ribbon, {Align::Left, false, true});
        if (focused(id)) focusRing(c);
        x += cw + gap;
    }
    if (cap) {
        const Rect c{x, y, cw, h};
        const std::string id = "jump:capture";
        const std::string scene = today_.currentScene;
        addHit(id, c, [this, scene] { openCaptureOn(scene); });
        tooltip(id, L"Open NorthRay Capture on " + widen(scene));
        card(c, id);
        gfx_.text(L"NORTHRAY CAPTURE", {c.x + 20, c.y + 18, c.w - 40, 16}, Font::Label, pal_.inkFaint, {Align::Left, false, true});
        gfx_.text(widen(scene), {c.x + 20, c.y + 36, c.w - 40, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        const std::wstring sub = std::to_wstring(today_.scenes.size()) + (today_.scenes.size() == 1 ? L" scene in " : L" scenes in ") + widen(today_.collection);
        gfx_.text(sub, {c.x + 20, c.y + 58, c.w - 40, 18}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        gfx_.text(L"Open on this scene ›", {c.x + 20, c.bottom() - 30, c.w - 150, 18}, Font::Small, pal_.ribbon, {Align::Left, false, true});
        if (today_.scenes.size() > 1)
            linkText("jump:scenes", c.right() - 20 - gfx_.measureWidth(L"Other scenes…", Font::Small), c.bottom() - 31, L"Other scenes…",
                     [this] { captureSceneMenu(); });
        if (focused(id)) focusRing(c);
    }
    return y + h - r.y;
}

void Launcher::continueBook(const Today::Book& b) {
    std::vector<std::wstring> args{L"--open-book", widen(b.id)};
    if (!b.chapterId.empty()) { args.push_back(L"--chapter"); args.push_back(widen(b.chapterId)); }
    if (!today_.library.empty()) { args.push_back(L"--library"); args.push_back(widen(today_.library)); }
    const std::wstring err = model_.launch("writing-studio", args);
    if (!err.empty()) flash(err, 6);
    else flash(L"Opening " + b.title + L" in Writing Studio...", 4);
}

void Launcher::openCaptureOn(const std::string& scene) {
    std::vector<std::wstring> args;
    if (!today_.collection.empty()) { args.push_back(L"--collection"); args.push_back(widen(today_.collection)); }
    if (!scene.empty()) { args.push_back(L"--scene"); args.push_back(widen(scene)); }
    const std::wstring err = model_.launch("capture", args);
    if (!err.empty()) flash(err, 6);
    else flash(L"Opening NorthRay Capture on " + widen(scene) + L"...", 4);
}

void Launcher::captureSceneMenu() {
    std::vector<MenuItem> items{MenuItem::heading(L"Open NorthRay Capture on")};
    for (std::size_t i = 0; i < today_.scenes.size(); ++i)
        items.push_back(MenuItem::item(widen(today_.scenes[i]), (UINT)(1 + i), {}, true, today_.scenes[i] == today_.currentScene));
    POINT pt;
    GetCursorPos(&pt);
    const UINT id = showMenu(hwnd(), items, pt, pal_);
    if (id >= 1 && id - 1 < today_.scenes.size()) openCaptureOn(today_.scenes[id - 1]);
}

float Launcher::pageLibrary(const Rect& r) {
    float y = r.y;
    // The banner: the night, with what needs doing.
    {
        int installed = 0, updates = 0;
        for (auto& a : model_.apps) { if (a.inst.found) ++installed; if (a.updateAvailable()) ++updates; }
        const Rect hero{r.x, y, r.w, 164};
        nightSky(gfx_, hero, 12, 1.0f, 3, pal_.paper);
        const float hx = hero.x + 30;
        gfx_.text(L"NORTHRAY SOFTWARE", {hx, hero.y + 28, hero.w - 60, 16}, Font::Label, kAuroraGreen, {Align::Left, false});
        gfx_.text(L"Your library", {hx, hero.y + 48, hero.w - 60, 40}, Font::Title, kNightInk, {Align::Left, false});
        gfx_.text(L"Install, update and open NorthRay apps. Each one also installs and runs on its own.",
                  {hx, hero.y + 94, std::min(hero.w - 300, 520.0f), 44}, Font::Body, kNightMuted);
        const float sx = hero.right() - 230;
        gfx_.text(std::to_wstring(installed) + L" installed", {sx, hero.y + 34, 200, 24}, Font::Heading, kNightInk, {Align::Right, false});
        gfx_.text(updates ? std::to_wstring(updates) + (updates == 1 ? L" update ready" : L" updates ready") : L"Everything up to date",
                  {sx, hero.y + 62, 200, 20}, Font::Small, updates ? kAuroraGreen : kNightMuted, {Align::Right, false});
        if (updates) nightButton("lib:updall", {hero.right() - 160, hero.y + 98, 130, 34}, L"Update all", [this] { updateAll(); });
        y += hero.h + 28;
    }

    if (model_.settings.showToday) {
        const float th = drawToday({r.x, y, r.w, 0});
        if (th > 0) y += th + 28;
    }
    if (model_.settings.showJumpBack) {
        const float jh = drawJumpBack({r.x, y, r.w, 0});
        if (jh > 0) y += jh + 28;
    }

    // What's new: the latest release of each app, newest first.
    {
        struct Item { const AppItem* app; const ReleaseNote* note; };
        std::vector<Item> news;
        for (auto& a : model_.apps) if (!a.notes.empty()) news.push_back({&a, &a.notes.front()});
        std::stable_sort(news.begin(), news.end(), [](const Item& a, const Item& b) {
            if (a.note->date != b.note->date) return a.note->date > b.note->date;
            return cjui::compareVersions(a.note->version, b.note->version) > 0;
        });
        if (news.size() > 3) news.resize(3);
        if (!news.empty()) {
            y += sectionLabel(r.x, y, r.w, L"What's new");
            const int cols = r.w >= 760 ? (int)news.size() : 1;
            const float gap = 14, cw = (r.w - gap * (cols - 1)) / cols, ch = 132;
            for (size_t i = 0; i < news.size(); ++i) {
                const int c = (int)i % cols;
                const Rect card{r.x + c * (cw + gap), y, cw, ch};
                const std::string id = "news:" + news[i].app->entry.id;
                addHit(id, card, [this, aid = news[i].app->entry.id] { navigate(Page::App, aid); });
                const float hv = hoverAmount(id);
                softShadow(card, 8, 0.3f + 0.7f * hv);
                gfx_.fillRounded(card, 10, pal_.surface);
                gfx_.strokeRounded(card, 10, pal_.rule.mix(pal_.inkFaint, 0.6f * hv));
                // An aurora edge along the top of each card.
                gfx_.pushClip({card.x, card.y, card.w, 3});
                gfx_.fillGradient({card.x, card.y, card.w, 10}, 10, kAuroraTeal, kAuroraViolet, false);
                gfx_.popClip();
                drawIcon(*news[i].app, {card.x + 16, card.y + 18, 32, 32});
                const float tx = card.x + 58, tw = card.w - 74;
                gfx_.text(w(news[i].app->entry.name), {tx, card.y + 16, tw, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
                std::wstring when = L"Version " + w(news[i].note->version);
                if (news[i].note->date.size() == 10) when += L"  ·  " + shortDate(news[i].note->date);
                gfx_.text(when, {tx, card.y + 36, tw, 18}, Font::Small, pal_.inkFaint, {Align::Left, false, true});
                const Rect body{card.x + 16, card.y + 62, card.w - 32, ch - 74};
                gfx_.pushClip(body);
                if (!news[i].note->notes.empty()) gfx_.text(w(news[i].note->notes.front()), body, Font::Small, pal_.inkMuted);
                gfx_.popClip();
                if (focused(id)) focusRing(card);
                if (c == cols - 1 || i + 1 == news.size()) y += ch + gap;
            }
            y += 14;
        }
    }

    y += sectionLabel(r.x, y, r.w, L"Apps");
    segmented("lib:filter", {r.x, y, 330, 30}, {L"All", L"Installed", L"Not installed"}, filter_, [this](int i) { filter_ = i; });
    y += 46;
    auto wanted = [this](const AppItem& a) { return !((filter_ == 1 && !a.inst.found) || (filter_ == 2 && a.inst.found)); };
    int shown = 0;
    // In the order they were dragged into (0.97); apps not placed yet keep the catalogue's order after them.
    std::vector<AppItem*> ordered;
    for (auto& a : model_.apps) ordered.push_back(&a);
    auto rank = [this](const AppItem* a) {
        const auto& o = model_.settings.appOrder;
        const auto it = std::find(o.begin(), o.end(), a->entry.id);
        return it == o.end() ? (int)o.size() : (int)(it - o.begin());
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const AppItem* x, const AppItem* y2) { return rank(x) < rank(y2); });
    cardRects_.clear();
    for (AppItem* ap : ordered) {
        AppItem& a = *ap;
        if (!wanted(a)) continue;
        ++shown;
        const float ch = appCard(a, {r.x, y, r.w, 0});
        cardRects_.push_back({a.entry.id, {r.x, y, r.w, ch}});
        y += ch + 14;
    }
    // Where a dragged card would go: a line in the gap.
    if (dragging_ && !dragApp_.empty()) {
        for (auto& [id, cr] : cardRects_)
            if (mouseY() >= cr.y - 7 && mouseY() < cr.bottom() + 7) {
                const float ly = mouseY() < cr.y + cr.h / 2 ? cr.y - 8 : cr.bottom() + 6;
                gfx_.fillRounded({cr.x, ly, cr.w, 3}, 1.5f, pal_.ribbon);
                gfx_.circle(cr.x + 4, ly + 1.5f, 4.5f, pal_.ribbon, true);
                break;
            }
    }
    if (!shown)
        y += gfx_.text(filter_ == 1 ? L"No NorthRay apps are installed yet." : L"Every NorthRay app is installed.", {r.x, y, r.w, 24},
                       Font::Body, pal_.inkMuted) + 12;
    if (!model_.catalogueConfigured()) {
        y += 12;
        const std::wstring t = L"Downloads become available here once the NorthRay Software website is online. Until then, "
                               L"apps installed with their own setup files appear here automatically.";
        const float th = gfx_.measureHeight(t, std::min(r.w, 680.0f) - 32, Font::Small);
        gfx_.fillRounded({r.x, y, std::min(r.w, 680.0f), th + 24}, 8, pal_.sunken);
        gfx_.text(t, {r.x + 16, y + 12, std::min(r.w, 680.0f) - 32, th + 4}, Font::Small, pal_.inkMuted);
        y += th + 36;
    }
    return y - r.y;
}

// One app in the library: a big card with its artwork, name, what it is, its version and status, and its buttons.
float Launcher::appCard(const AppItem& a, const Rect& at) {
    const bool narrow = at.w < 700;
    const float actW = narrow ? 0 : 300;
    // As tall as the tagline needs (long ones wrapped to three lines, not over the status).
    const float art = 112, tx0 = 26 + art + 24;   // bigger art (0.97)
    const float tw0 = at.w - tx0 - actW - 20;
    const float th = std::min(gfx_.measureHeight(w(a.entry.tagline), tw0, Font::Body), 66.0f);
    const float statusY = 52 + std::max(42.0f, th) + 6;
    const float h = std::max(narrow ? 200.0f : 164.0f, statusY + 20 + (narrow ? 64 : 18));
    const Rect row{at.x, at.y, at.w, h};
    const std::string id = "app:" + a.entry.id;
    // The card opens the app's page; the buttons on it act directly.
    addHit(id, {row.x, row.y, row.w - actW, narrow ? h - 56 : h}, [this, i = a.entry.id] { navigate(Page::App, i); });
    const float hv = hoverAmount(id);
    softShadow(row, 10, 0.3f + 0.7f * hv);
    gfx_.fillRounded(row, 12, pal_.surface);
    gfx_.strokeRounded(row, 12, pal_.rule.mix(pal_.inkFaint, 0.6f * hv));
    // The app's own colour, as a soft light behind its icon.
    gfx_.pushClip(row);
    gfx_.glow(row.x + 26 + art / 2, row.y + 26 + art / 2, 160, 120, hexColor(a.entry.accent, pal_.ribbon).withAlpha(pal_.dark ? 0.26f : 0.15f));
    gfx_.popClip();
    const Rect icon{row.x + 26, row.y + 26, art, art};
    drawIcon(a, icon);
    // An update waiting: a badge on the art's corner (0.97).
    if (a.updateAvailable() && !jobs_.busy(a.entry.id)) {
        const float bx = icon.right() - 6, by = icon.y + 6;
        gfx_.circle(bx, by, 13, pal_.surface, true);
        gfx_.circle(bx, by, 11, pal_.ribbon, true);
        gfx_.line(bx, by - 5, bx, by + 4, pal_.surface, 2);
        gfx_.line(bx - 4, by - 1, bx, by - 5.5f, pal_.surface, 2);
        gfx_.line(bx + 4, by - 1, bx, by - 5.5f, pal_.surface, 2);
        addHit("badge:" + a.entry.id, {bx - 13, by - 13, 26, 26}, nullptr, false);
        tooltip("badge:" + a.entry.id, L"Update ready: version " + w(a.release->version));
    }
    const float tx = row.x + tx0, tw = row.w - tx0 - actW - 20;
    gfx_.text(w(a.entry.name), {tx, row.y + 22, tw, 28}, Font::Heading, pal_.ink, {Align::Left, false, true});
    gfx_.text(w(a.entry.tagline), {tx, row.y + 52, tw, std::max(42.0f, th)}, Font::Body, pal_.inkMuted);
    const JobState s = jobs_.state(a.entry.id);
    gfx_.text(statusLine(a), {tx, row.y + statusY, tw + (narrow ? 0 : 120), 20}, Font::Small,
              s.stage == JobState::Failed ? pal_.live : a.updateAvailable() ? pal_.ribbon : pal_.inkFaint, {Align::Left, false, true});
    if (narrow) actionArea(a, {tx, row.bottom() - 50, row.right() - tx - 20, 36}, "lib:", false);
    else actionArea(a, {row.right() - actW - 4, row.y + h / 2 - 18, actW - 20, 36}, "lib:", false);
    if (focused(id)) focusRing(row);
    return h;
}

float Launcher::pageApp(const Rect& r) {
    AppItem& a = *model_.app(appId_);
    const cj::AppEntry& e = a.entry;
    float y = r.y;
    linkText("app:back", r.x, y, L"‹ Library", [this] { navigate(Page::Library); });
    y += 30;
    // The banner: the night, lit in the app's own colour.
    {
        const Rect hero{r.x, y, r.w, 176};
        nightSky(gfx_, hero, 12, 0.7f, 11, pal_.paper);
        gfx_.pushClip(hero);
        gfx_.glow(hero.x + 90, hero.y + 88, 260, 170, hexColor(e.accent, kAuroraTeal).withAlpha(0.45f));
        gfx_.popClip();
        roundCorners(gfx_, hero, 12, pal_.paper);
        drawIcon(a, {hero.x + 32, hero.y + 40, 96, 96});
        const float tx = hero.x + 152, tw = hero.w - 152 - 30;
        gfx_.text(w(e.category.empty() ? std::string("App") : e.category), {tx, hero.y + 40, tw, 16}, Font::Label, kAuroraGreen,
                  {Align::Left, false, true});
        gfx_.text(w(e.name), {tx, hero.y + 58, tw, 36}, Font::Title, kNightInk, {Align::Left, false, true});
        std::wstring by = L"" CJ_DEVELOPER_WSTR;
        if (!e.associatedCreator.empty()) by += L"  ·  in association with " + w(e.associatedCreator);
        gfx_.text(by, {tx, hero.y + 100, tw, 20}, Font::Small, kNightMuted, {Align::Left, false, true});
        gfx_.text(statusLine(a), {tx, hero.y + 122, tw, 20}, Font::Small,
                  jobs_.state(e.id).stage == JobState::Failed ? hexColor("#FF8A7A") : kNightInk, {Align::Left, false, true});
        y += hero.h + 20;
    }
    actionArea(a, {r.x, y, std::min(r.w, 520.0f), 36}, "page:", false);
    if (a.inst.found && !jobs_.busy(e.id)) {
        const float ux = r.x + std::min(r.w, 520.0f) + 18;
        linkText("page:uninstall", ux, y + 10, L"Uninstall", [this, id = e.id] { uninstall(id); });
    }
    y += 60;

    // Screenshots: a row of pictures; one opens large.
    if (!a.screenshots.empty()) {
        y += sectionLabel(r.x, y, r.w, L"Screenshots");
        const int cols = std::min<int>(3, (int)a.screenshots.size());
        const float gap = 14, sw = (r.w - gap * (cols - 1)) / cols, sh = sw * 10 / 16;
        float rowH = 0;
        for (size_t i = 0; i < a.screenshots.size(); ++i) {
            const int c = (int)i % cols;
            const Rect pic{r.x + c * (sw + gap), y, sw, sh};
            const std::string id = "shot:" + std::to_string(i);
            addHit(id, pic, [this, i] { lightbox_ = (int)i; invalidate(); });
            const float hv = hoverAmount(id);
            softShadow(pic, 8, 0.4f + 0.6f * hv);
            gfx_.fillRounded(pic, 8, pal_.sunken);
            gfx_.pushClip(pic);
            gfx_.drawImage(a.screenshots[i].path, pic, true);
            gfx_.popClip();
            gfx_.strokeRounded(pic, 8, pal_.rule.mix(pal_.ribbon, hv), 1 + hv);
            const float capH = a.screenshots[i].caption.empty() ? 0
                : gfx_.text(w(a.screenshots[i].caption), {pic.x, pic.bottom() + 8, pic.w, 40}, Font::Small, pal_.inkMuted);
            rowH = std::max(rowH, sh + 8 + capH);
            if (focused(id)) focusRing(pic);
            if (c == cols - 1 || i + 1 == a.screenshots.size()) { y += rowH + 18; rowH = 0; }
        }
        y += 10;
    }

    const bool cols = r.w >= 700;
    const float aw = cols ? r.w * 0.6f : r.w, kx = cols ? r.x + aw + 36 : r.x, kw = cols ? r.w - aw - 36 : r.w;
    float ay = y, ky = y;
    ay += sectionLabel(r.x, ay, aw, L"About");
    ay += gfx_.text(w(e.description), {r.x, ay, aw, 600}, Font::Body, pal_.ink) + 16;
    if (!e.features.empty()) {
        ay += sectionLabel(r.x, ay, aw, L"Features");
        for (auto& f : e.features) {
            gfx_.text(L"–", {r.x, ay, 14, 20}, Font::Body, pal_.inkFaint);
            ay += gfx_.text(w(f), {r.x + 18, ay, aw - 18, 120}, Font::Body, pal_.ink) + 6;
        }
        ay += 12;
    }
    ay += sectionLabel(r.x, ay, aw, L"What's new");
    if (a.notes.empty()) ay += gfx_.text(L"No release notes yet.", {r.x, ay, aw, 20}, Font::Body, pal_.inkMuted) + 8;
    int shownNotes = 0;
    for (auto& n : a.notes) {
        if (shownNotes++ == 4) break;
        gfx_.text(L"Version " + w(n.version), {r.x, ay, aw, 20}, Font::BodyStrong, pal_.ink, {Align::Left, false});
        gfx_.text(w(n.date), {r.x, ay, aw, 20}, Font::Small, pal_.inkFaint, {Align::Right, false});
        ay += 24;
        for (auto& line : n.notes) {
            gfx_.text(L"–", {r.x, ay, 14, 20}, Font::Small, pal_.inkFaint);
            ay += gfx_.text(w(line), {r.x + 16, ay, aw - 16, 120}, Font::Small, pal_.inkMuted) + 4;
        }
        ay += 12;
    }

    if (!cols) ky = ay + 12;
    ky += sectionLabel(kx, ky, kw, L"Details");
    auto kv = [&](const std::wstring& k, const std::wstring& v) {
        if (v.empty()) return;
        gfx_.text(k, {kx, ky, kw, 18}, Font::Small, pal_.inkMuted, {Align::Left, false});
        ky += 18;
        ky += gfx_.text(v, {kx, ky, kw, 120}, Font::Body, pal_.ink) + 10;
    };
    kv(L"Publisher", L"" CJ_DEVELOPER_WSTR);
    kv(L"In association with", w(e.associatedCreator));
    kv(L"Category", w(e.category));
    kv(L"Status", w(cj::appStatusLabel(e.status)));
    kv(L"Installed version", a.inst.found ? (a.inst.version.empty() ? L"Unknown" : w(a.inst.version)) : L"Not installed");
    if (a.release) kv(L"Latest version", w(a.release->version));
    if (a.release) kv(L"Download size", formatSize(a.release->size));
    kv(L"Install folder", a.inst.found ? a.inst.dir : model_.installDirFor(e.id) + L" (when installed)");
    if (a.inst.found && !a.inst.dir.empty()) {
        auto it = diskUse_.find(e.id);
        if (it == diskUse_.end()) it = diskUse_.emplace(e.id, folderSize(a.inst.dir)).first;
        kv(L"On disk", formatSize(it->second) + L" (the app itself; your own files are kept elsewhere)");
    }
    if (a.inst.legacy) kv(L"Note", L"Installed by an older setup from before the NorthRay name. Updating moves it to NorthRay's folder.");
    return std::max(ay, ky) - r.y;
}

float Launcher::pageUpdates(const Rect& r) {
    std::wstring sub = model_.catalogueConfigured()
                           ? L"Updates are checked when the launcher starts and every few hours, also while it waits in the notification area."
                           : L"There's no online catalogue yet, so updates can't be checked. Installed apps keep working.";
    if (checking_) sub += L" Checking now...";
    else if (model_.catalogueChecked) {
        std::wstring when = cjui::formatRelative(model_.catalogueChecked, nowUtc());
        if (when == L"Today" || when == L"Yesterday") when[0] = (wchar_t)towlower(when[0]);
        sub += L" Last checked " + when + L".";
    }
    float y = r.y + drawHeader(r, L"Updates", sub);
    if (model_.catalogueConfigured()) {
        button("upd:check", {r.x, y, 170, 32}, checking_ ? L"Checking..." : L"Check now", [this] { checkCatalogue(true); });
        int n = 0;
        for (auto& a : model_.apps) if (a.updateAvailable()) ++n;
        if (n) button("upd:all", {r.x + 182, y, 150, 32}, L"Update all", [this] { updateAll(); }, true);
        y += 50;
        if (!model_.catalogueError.empty())
            y += gfx_.text(L"Last check: " + model_.catalogueError, {r.x, y, r.w, 40}, Font::Small, pal_.live) + 10;
    }
    int listed = 0;
    for (auto& a : model_.apps) {
        if (!a.updateAvailable() && !jobs_.busy(a.entry.id)) continue;
        ++listed;
        const Rect row{r.x, y, r.w, 74};
        gfx_.fillRounded(row, 6, pal_.surface);
        gfx_.strokeRounded(row, 6, pal_.rule);
        drawIcon(a, {row.x + 16, row.y + 15, 44, 44});
        gfx_.text(w(a.entry.name), {row.x + 76, row.y + 14, row.w - 380, 22}, Font::BodyStrong, pal_.ink, {Align::Left, false, true});
        gfx_.text(statusLine(a), {row.x + 76, row.y + 38, row.w - 380, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        actionArea(a, {row.right() - 290, row.y + 20, 272, 34}, "upd:", true);
        y += row.h + 10;
    }
    if (!listed) y += gfx_.text(L"Every installed app is up to date.", {r.x, y, r.w, 24}, Font::Body, pal_.inkMuted) + 16;

    y += 18;
    y += sectionLabel(r.x, y, r.w, L"NorthRay Launcher");
    std::wstring self = L"Version " CJ_VERSION_WSTR L".";
    if (model_.launcherRelease && cjui::compareVersions(model_.launcherRelease->version, CJ_VERSION_STR) > 0) {
        self += L" Version " + w(model_.launcherRelease->version) + L" is available.";
        gfx_.text(self, {r.x, y, r.w - 200, 30}, Font::Body, pal_.ink, {Align::Left, false, true, true});
        if (!jobs_.busy("launcher"))
            button("upd:self", {r.right() - 190, y, 190, 32}, L"Update the launcher", [this] {
                JobRequest jr;
                jr.appId = "launcher";
                jr.release = *model_.launcherRelease;
                jr.update = true;
                jobs_.enqueue(jr);
            }, true);
        y += 44;
    } else {
        y += gfx_.text(self + L" " + (model_.launcherRelease ? L"It's up to date." : L""), {r.x, y, r.w, 24}, Font::Body,
                       pal_.inkMuted) + 12;
    }
    return y - r.y;
}

float Launcher::pageSettings(const Rect& r) {
    float y = r.y + drawHeader(r, L"Settings", L"");
    const float w2 = std::min(r.w, 680.0f);

    y += sectionLabel(r.x, y, w2, L"Installing apps");
    gfx_.text(L"Install folder for new apps", {r.x, y, w2, 20}, Font::Small, pal_.inkMuted, {Align::Left, false});
    y += 22;
    const Rect pathBox{r.x, y, w2 - 130, 32};
    gfx_.fillRounded(pathBox, 4, pal_.surface);
    gfx_.strokeRounded(pathBox, 4, pal_.rule);
    gfx_.text(model_.effectiveInstallRoot(), pathBox.inset(10, 0), Font::Small, pal_.ink, {Align::Left, false, true, true});
    button("set:root", {pathBox.right() + 10, y, 120, 32}, L"Change...", [this] { chooseInstallRoot(); });
    y += 40;
    if (!model_.settings.installRoot.empty()) {
        linkText("set:rootreset", r.x, y, L"Use the default folder", [this] {
            model_.settings.installRoot.clear();
            model_.saveSettings();
        });
        y += 26;
    }
    y += gfx_.text(L"Each app gets its own folder inside it. Apps that are already installed stay where they are; their updates go to the same place.",
                   {r.x, y, w2, 60}, Font::Small, pal_.inkMuted) + 18;

    y += sectionLabel(r.x, y, w2, L"Updates");
    checkbox("set:auto", {r.x, y, w2, 26}, L"Install updates automatically", model_.settings.autoUpdate,
             [this] { model_.settings.autoUpdate = !model_.settings.autoUpdate; model_.saveSettings(); if (model_.settings.autoUpdate) autoUpdate(); }, false);
    y += 28;
    y += gfx_.text(L"Updates install after each check, every few hours. Never while an app is open: its update waits until you close it.",
                   {r.x + 30, y, w2 - 30, 60}, Font::Small, pal_.inkMuted) + 24;

    y += sectionLabel(r.x, y, w2, L"Running in the background");
    checkbox("set:tray", {r.x, y, w2, 26}, L"Keep running in the notification area when closed", model_.settings.keepInTray, [this] {
        model_.settings.keepInTray = !model_.settings.keepInTray;
        model_.saveSettings();
        if (model_.settings.keepInTray) addTrayIcon(); else removeTrayIcon();
    }, false);
    y += 28;
    y += gfx_.text(L"Closing the window leaves NorthRay Launcher's icon in the notification area, so updates still install. "
                   L"Its menu opens your apps and quits the launcher.",
                   {r.x + 30, y, w2 - 30, 60}, Font::Small, pal_.inkMuted) + 14;
    checkbox("set:startup", {r.x, y, w2, 26}, L"Start with Windows", model_.settings.startWithWindows, [this] {
        model_.settings.startWithWindows = !model_.settings.startWithWindows;
        model_.saveSettings();
        setStartWithWindows(model_.settings.startWithWindows);
        if (model_.settings.startWithWindows) addTrayIcon();
    }, false);
    y += 28;
    y += gfx_.text(L"Starts quietly in the notification area when you sign in to Windows, with no window.", {r.x + 30, y, w2 - 30, 60},
                   Font::Small, pal_.inkMuted) + 24;

    y += sectionLabel(r.x, y, w2, L"Appearance");
    const int t = (int)model_.settings.theme;
    segmented("set:theme", {r.x, y, 300, 30}, {L"As Windows", L"Light", L"Dark"}, t, [this](int i) {
        model_.settings.theme = (ThemeChoice)i;
        model_.saveSettings();
        applyTheme();
    });
    y += 46;
    checkbox("set:today", {r.x, y, w2, 26}, L"Show Today on the home page", model_.settings.showToday, [this] {
        model_.settings.showToday = !model_.settings.showToday;
        model_.saveSettings();
    }, false);
    y += 28;
    y += gfx_.text(L"Your words today from NorthRay Writing Studio, and your newest screenshots and pictures saved by NorthRay Capture. "
                   L"Read from this PC only.",
                   {r.x + 30, y, w2 - 30, 60}, Font::Small, pal_.inkMuted) + 24;
    checkbox("set:jump", {r.x, y, w2, 26}, L"Show Jump back in on the home page and in the tray menu", model_.settings.showJumpBack, [this] {
        model_.settings.showJumpBack = !model_.settings.showJumpBack;
        model_.saveSettings();
    }, false);
    y += 28;
    y += gfx_.text(L"The books you last wrote in, to open Writing Studio at the chapter you were on, and NorthRay Capture's scenes, to open it on "
                   L"one. Read from this PC only.",
                   {r.x + 30, y, w2 - 30, 60}, Font::Small, pal_.inkMuted) + 24;

    y += sectionLabel(r.x, y, w2, L"About");
    y += gfx_.text(L"NorthRay Launcher " CJ_VERSION_WSTR L". Developed by the " CJ_DEVELOPER_WSTR L" project.", {r.x, y, w2, 40},
                   Font::Body, pal_.ink) + 8;
    {
        const std::wstring lbl = L"Visit the website";
        const float bw = gfx_.measureWidth(lbl, Font::Body) + 26 + 21;
        button("set:website", {r.x, y, bw, 32}, lbl, [] { cjui::northray::openWebsite(); });
        y += 44;
    }
    // The launcher's own changes (0.97): its newest releases, with what each brought.
    if (!model_.launcherNotes.empty()) {
        y += 6;
        gfx_.text(L"What's new in the launcher", {r.x, y, w2, 20}, Font::SmallStrong, pal_.ink, {Align::Left, false});
        y += 26;
        const size_t shown = std::min<size_t>(model_.launcherNotes.size(), 4);
        for (size_t i = 0; i < shown; ++i) {
            const ReleaseNote& n = model_.launcherNotes[i];
            std::wstring head = L"Version " + w(n.version);
            if (n.date.size() == 10) head += L"  \u00B7  " + shortDate(n.date);
            if (n.version == CJ_VERSION_STR) head += L"  \u00B7  this version";
            gfx_.text(head, {r.x, y, w2, 18}, Font::Small, n.version == CJ_VERSION_STR ? pal_.ribbon : pal_.inkFaint, {Align::Left, false, true});
            y += 20;
            for (const std::string& line : n.notes) {
                gfx_.circle(r.x + 5, y + 9, 2, pal_.inkFaint, true);
                y += gfx_.text(w(line), {r.x + 16, y, w2 - 16, 200}, Font::Small, pal_.inkMuted) + 4;
            }
            y += 10;
        }
    }
    y += gfx_.text(L"Installs come from each app's own setup file, downloaded over HTTPS and checked against its published size and "
                   L"SHA-256 before anything runs. Nothing needs an account or subscription.",
                   {r.x, y, w2, 80}, Font::Small, pal_.inkMuted) + 10;
    if (!model_.contactEmail.empty())
        y += gfx_.text(L"Contact: " + w(model_.contactEmail), {r.x, y, w2, 20}, Font::Small, pal_.inkMuted) + 8;
    y += gfx_.text(L"Catalogue: " + (model_.catalogueConfigured() ? w(model_.catalogueUrl) : std::wstring(L"not set up yet")),
                   {r.x, y, w2, 40}, Font::Small, pal_.inkFaint) + 8;
    return y - r.y;
}

}  // namespace nrl

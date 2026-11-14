#include "launcher.hpp"

#include "cjui/http.hpp"
#include "cjui/northray.hpp"
#include "cjui/packages.hpp"
#include "resource.h"

#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <thread>

namespace nrl {

static const float kNavW = 232, kStatusH = 28;
static const UINT WM_APP_JOBS = WM_APP + 1;
static const UINT WM_APP_CATALOGUE = WM_APP + 2;
static const UINT_PTR kRefreshTimer = 1;

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

bool Launcher::create(HINSTANCE inst, int show, const std::string& openApp) {
    model_.hideCjPhilipApps = true;   // CJ Radio and the like are installed and opened from CJ Philip Community
    model_.load();
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
    spec.show = show;
    if (!createWindow(spec)) return false;
    applyTheme();
    jobs_.start(hwnd(), WM_APP_JOBS, model_.stateDir + L"\\Downloads");
    if (!openApp.empty() && model_.app(openApp)) navigate(Page::App, openApp);
    else if (model_.firstRun) page_ = Page::Welcome;   // the first time: a short welcome
    ShowWindow(hwnd(), show);
    UpdateWindow(hwnd());
    SetTimer(hwnd(), kRefreshTimer, 6 * 3600 * 1000, nullptr);
    checkCatalogue(false);
    return true;
}

void Launcher::onDestroy() {
    KillTimer(hwnd(), kRefreshTimer);
    jobs_.stop();
}

void Launcher::applyTheme() {
    const bool dark = model_.settings.theme == ThemeChoice::Dark ||
                      (model_.settings.theme == ThemeChoice::System && systemPrefersDark());
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

LRESULT Launcher::onMessage(UINT msg, WPARAM, LPARAM lp, bool& handled) {
    handled = true;
    if (msg == WM_APP_JOBS) {
        // A job moved on: refresh installs when one finishes.
        for (auto& a : model_.apps) {
            const JobState s = jobs_.state(a.entry.id);
            if ((s.stage == JobState::Done && !jobs_.busy(a.entry.id))) {
                const bool was = a.inst.found;
                model_.refreshInstalls();
                if (model_.app(a.entry.id)->inst.found && s.total)
                    flash(w(a.entry.name) + (s.update || was ? L" was updated." : L" is installed."), 6);
                jobs_.forget(a.entry.id);
            }
        }
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
        invalidate();
        return 0;
    }
    if (msg == WM_ACTIVATEAPP && !jobs_.anyBusy()) {
        model_.refreshInstalls();  // something may have been installed or removed meanwhile
        invalidate();
    }
    handled = false;
    return 0;
}

void Launcher::onTimer(UINT_PTR id) {
    if (id == kRefreshTimer) checkCatalogue(false);
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
    if (MessageBoxW(hwnd(), q.c_str(), L"Uninstall", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
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
        if (vk == VK_RETURN) { finishWelcome(); return true; }
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
    gfx_.text(L"Developed by the " CJ_DEVELOPER_WSTR L" project", {r.x + 20, r.bottom() - 52, r.w - 30, 36}, Font::Small,
              kNightMuted.withAlpha(0.8f));
}

void Launcher::drawStatusBar(const Rect& r) {
    gfx_.fill(r, pal_.sunken);
    gfx_.line(r.x, r.y + 0.5f, r.right(), r.y + 0.5f, pal_.rule);
    std::wstring left = L"NorthRay Launcher " CJ_VERSION_WSTR;
    if (checking_) left += L"  ·  Checking for updates...";
    else if (model_.catalogueChecked) left += L"  ·  Checked " + cjui::formatRelative(model_.catalogueChecked, nowUtc());
    else if (!model_.catalogueConfigured()) left += L"  ·  Offline catalogue";
    gfx_.text(left, {r.x + 14, r.y, r.w * 0.6f, r.h}, Font::Small, pal_.inkMuted, {Align::Left, false, true, true});
    gfx_.text(flashText(), {r.x + r.w * 0.35f, r.y, r.w * 0.65f - 14, r.h}, Font::Small, flashColor(pal_.ink), {Align::Right, false, true, true});
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

std::wstring Launcher::statusLine(const AppItem& a) const {
    const JobState s = jobs_.state(a.entry.id);
    switch (s.stage) {
        case JobState::Queued: return L"Waiting to start...";
        case JobState::Downloading:
            return L"Downloading  " + formatSize(s.done) + L" of " + formatSize(s.total);
        case JobState::Verifying: return L"Checking the download...";
        case JobState::Installing: return s.update ? L"Updating..." : L"Installing...";
        case JobState::Failed: return s.message;
        case JobState::Cancelled: return L"Cancelled.";
        default: break;
    }
    if (a.updateAvailable()) return L"Update available: " + w(a.release->version) + L" (you have " + w(a.inst.version) + L")";
    if (a.inst.found) {
        std::wstring t = L"Installed" + (a.inst.version.empty() ? std::wstring() : L", version " + w(a.inst.version));
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
    const float top = beginScroll({kNavW, 0, W - kNavW, H - kStatusH});
    Rect page{kNavW + pad, top + 34, std::min(W - kNavW - pad * 2, 960.0f), 0};
    float h = 0;
    switch (page_) {
        case Page::Library: h = pageLibrary(page); break;
        case Page::App: h = model_.app(appId_) ? pageApp(page) : pageLibrary(page); break;
        case Page::Updates: h = pageUpdates(page); break;
        case Page::Settings: h = pageSettings(page); break;
        case Page::Welcome: break;
    }
    endScroll(h + 60);
    drawNav({0, 0, kNavW, H - kStatusH});
    drawStatusBar({0, H - kStatusH, W, kStatusH});
    drawLightbox(W, H);
}

// The first run: what the launcher is, where apps go, and whether updates install by themselves.
void Launcher::drawWelcome(float W, float H) {
    nightSky(gfx_, {0, 0, W, H}, 0, 1.0f, 21);
    drawAurora({0, 0, W, 3});
    const float cw = std::min(W - 80, 620.0f), x = (W - cw) / 2;
    float y = std::max(40.0f, (H - 560) / 2);
    gfx_.drawImage(markPng_, {x, y, 64, 64}, false);
    y += 88;
    gfx_.text(L"WELCOME", {x, y, cw, 16}, Font::Label, kAuroraGreen, {Align::Left, false});
    y += 22;
    gfx_.text(L"NorthRay Launcher", {x, y, cw, 44}, Font::Title, kNightInk, {Align::Left, false});
    y += 50;
    y += gfx_.text(L"One place to install, update and open apps from " CJ_DEVELOPER_WSTR L". They're free, work offline, and none of "
                   L"them needs an account.",
                   {x, y, cw, 80}, Font::Body, kNightMuted) + 22;
    // Where apps go.
    gfx_.text(L"Apps are installed in", {x, y, cw, 18}, Font::Small, kNightMuted, {Align::Left, false});
    y += 22;
    const Rect box{x, y, cw - 130, 34};
    gfx_.fillRounded(box, 6, kNightInk.withAlpha(0.07f));
    gfx_.strokeRounded(box, 6, kNightInk.withAlpha(0.15f));
    gfx_.text(model_.effectiveInstallRoot(), box.inset(12, 0), Font::Small, kNightInk, {Align::Left, false, true, true});
    nightButton("wel:root", {box.right() + 10, y, 120, 34}, L"Change...", [this] { chooseInstallRoot(); }, false);
    y += 52;
    // Updates.
    const std::string cid = "wel:auto";
    const Rect cb{x, y, cw, 26};
    addHit(cid, cb, [this] { model_.settings.autoUpdate = !model_.settings.autoUpdate; invalidate(); });
    gfx_.strokeRounded({x, y + 3, 18, 18}, 4, kNightInk.withAlpha(0.6f + 0.4f * hoverAmount(cid)), 1.5f);
    if (model_.settings.autoUpdate) {
        gfx_.line(x + 4, y + 12, x + 8, y + 17, kAuroraGreen, 2);
        gfx_.line(x + 8, y + 17, x + 15, y + 7, kAuroraGreen, 2);
    }
    gfx_.text(L"Install updates automatically", {x + 30, y + 1, cw - 30, 22}, Font::Body, kNightInk, {Align::Left, false});
    if (focused(cid)) focusRing(cb);
    y += 28;
    y += gfx_.text(L"Never while an app is open. You can change this, and the folder, in Settings.", {x + 30, y, cw - 30, 40},
                   Font::Small, kNightMuted) + 30;
    nightButton("wel:go", {x, y, 170, 40}, L"Get started", [this] { finishWelcome(); });
}

void Launcher::finishWelcome() {
    model_.saveSettings();   // the settings file now exists: no welcome next time
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
    int shown = 0;
    for (auto& a : model_.apps) {
        if ((filter_ == 1 && !a.inst.found) || (filter_ == 2 && a.inst.found)) continue;
        ++shown;
        const Rect row{r.x, y, r.w, 100};
        const std::string id = "app:" + a.entry.id;
        // The row opens the app's page; the buttons on it act directly.
        addHit(id, {row.x, row.y, row.w - 290, row.h}, [this, i = a.entry.id] { navigate(Page::App, i); });
        const float hv = hoverAmount(id);
        softShadow(row, 8, 0.25f + 0.75f * hv);
        gfx_.fillRounded(row, 10, pal_.surface);
        gfx_.strokeRounded(row, 10, pal_.rule.mix(pal_.inkFaint, 0.6f * hv));
        // The app's own colour, as a soft light behind its icon.
        gfx_.pushClip(row);
        gfx_.glow(row.x + 48, row.y + 50, 90, 70, hexColor(a.entry.accent, pal_.ribbon).withAlpha(pal_.dark ? 0.22f : 0.12f));
        gfx_.popClip();
        drawIcon(a, {row.x + 20, row.y + 20, 60, 60});
        const float tx = row.x + 100, tw = row.w - 100 - 300;
        gfx_.text(w(a.entry.name), {tx, row.y + 18, tw, 24}, Font::Heading, pal_.ink, {Align::Left, false, true});
        if (a.entry.id == "cj-philip-community") {
            // CJ Philip's apps are an optional extra, got through here rather than the website.
            const float nw = std::min(tw - 80, gfx_.measureWidth(w(a.entry.name), Font::Heading));
            const Rect tag{tx + nw + 10, row.y + 21, 68, 19};
            gfx_.strokeRounded(tag, 4, pal_.rule.mix(pal_.inkFaint, 0.4f));
            gfx_.text(L"OPTIONAL", tag, Font::Label, pal_.inkMuted, {Align::Center, false, false, true});
        }
        gfx_.text(w(a.entry.tagline), {tx, row.y + 44, tw, 20}, Font::Small, pal_.inkMuted, {Align::Left, false, true});
        const JobState s = jobs_.state(a.entry.id);
        gfx_.text(statusLine(a), {tx, row.y + 66, tw + 120, 18}, Font::Small,
                  s.stage == JobState::Failed ? pal_.live : a.updateAvailable() ? pal_.ribbon : pal_.inkFaint,
                  {Align::Left, false, true});
        actionArea(a, {row.right() - 290, row.y + 33, 272, 34}, "lib:", true);
        if (focused(id)) focusRing(row);
        y += row.h + 12;
    }
    if (!shown)
        y += gfx_.text(filter_ == 1 ? L"Nothing is installed yet." : L"Everything here is installed.", {r.x, y, r.w, 24},
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
    if (a.inst.legacy) kv(L"Note", L"Installed by the older CJ Community setup. Updating moves it to NorthRay's folder.");
    return std::max(ay, ky) - r.y;
}

float Launcher::pageUpdates(const Rect& r) {
    float y = r.y + drawHeader(r, L"Updates", model_.catalogueConfigured()
                                                  ? L"Updates are checked when the launcher starts and every few hours."
                                                  : L"There's no online catalogue yet, so updates can't be checked. Installed apps keep working.");
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

    const std::string cid = "set:auto";
    const Rect box{r.x, y, w2, 26};
    addHit(cid, box, [this] { model_.settings.autoUpdate = !model_.settings.autoUpdate; model_.saveSettings(); });
    gfx_.strokeRounded({r.x, y + 3, 18, 18}, 3, pal_.inkMuted.mix(pal_.ink, hoverAmount(cid)), 1.5f);
    if (model_.settings.autoUpdate) {
        gfx_.line(r.x + 4, y + 12, r.x + 8, y + 17, pal_.ink, 2);
        gfx_.line(r.x + 8, y + 17, r.x + 15, y + 7, pal_.ink, 2);
    }
    gfx_.text(L"Install updates automatically", {r.x + 28, y + 2, w2 - 28, 20}, Font::Body, pal_.ink, {Align::Left, false});
    if (focused(cid)) focusRing(box);
    y += 28;
    y += gfx_.text(L"Off by default. When it's on, updates install after each check, but never while an app is open.",
                   {r.x + 28, y, w2 - 28, 60}, Font::Small, pal_.inkMuted) + 24;

    y += sectionLabel(r.x, y, w2, L"Appearance");
    const int t = (int)model_.settings.theme;
    segmented("set:theme", {r.x, y, 300, 30}, {L"As Windows", L"Light", L"Dark"}, t, [this](int i) {
        model_.settings.theme = (ThemeChoice)i;
        model_.saveSettings();
        applyTheme();
    });
    y += 54;

    y += sectionLabel(r.x, y, w2, L"About");
    y += gfx_.text(L"NorthRay Launcher " CJ_VERSION_WSTR L". Developed by the " CJ_DEVELOPER_WSTR L" project.", {r.x, y, w2, 40},
                   Font::Body, pal_.ink) + 8;
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

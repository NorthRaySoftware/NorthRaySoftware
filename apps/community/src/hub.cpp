#include "hub.hpp"

#include "cjui/http.hpp"
#include "cjui/packages.hpp"
#include "resource.h"
#include "version.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include "cj/json_safe.hpp"
#include <sstream>
#include <thread>

namespace cjp {

using nlohmann::json;
using nrl::AppItem;
using nrl::JobState;

static const UINT_PTR kRefreshTimer = 1, kSoonTimer = 2;
static const UINT WM_APP_JOBS = WM_APP + 1;
static const UINT WM_APP_CATALOGUE = WM_APP + 2;
static const float kHeaderH = 118, kFooterH = 52;

// CJ Philip Community's own look: deep wine, as the suite's ribbon colour.
static const char* kWine = "#6A2E3F";

static std::wstring w(const std::string& s) { return widen(s); }

static bool readFile(const std::wstring& path, std::string& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static std::wstring formatSize(unsigned long long bytes) {
    wchar_t buf[32];
    if (bytes >= 1024ULL * 1024) swprintf(buf, 32, L"%.1f MB", bytes / (1024.0 * 1024.0));
    else swprintf(buf, 32, L"%.0f KB", bytes / 1024.0);
    return buf;
}

struct CatalogueResult {
    std::string url, body;
    std::wstring error;
    bool userAsked = false;
};

// ---------------------------------------------------------------- lifecycle

CommunityApp::~CommunityApp() { jobs_.stop(); }

bool CommunityApp::create(HINSTANCE inst, int show) {
    load();
    WindowSpec spec;
    spec.instance = inst;
    spec.className = L"CJPhilipCommunityWindow";
    spec.title = L"CJ Philip Community";
    spec.iconId = IDI_APP;
    spec.width = 820;
    spec.height = 560;
    spec.minWidth = 560;
    spec.minHeight = 420;
    spec.show = show;
    if (!createWindow(spec)) return false;
    applyTheme();
    ShowWindow(hwnd(), show);
    UpdateWindow(hwnd());
    jobs_.start(hwnd(), WM_APP_JOBS, model_.stateDir + L"\\Downloads");
    SetTimer(hwnd(), kRefreshTimer, 4000, nullptr);  // notices apps being opened or closed
    checkCatalogue(false);
    return true;
}

void CommunityApp::load() {
    // The catalogue, installs and release information are NorthRay Launcher's own
    // (shared metadata next to this program, the same online catalogue).
    model_.load();
    stateDir_ = cjui::northRayDataDir() + L"\\CJ Philip Community";
    CreateDirectoryW(stateDir_.c_str(), nullptr);
    std::string text;
    if (readFile(stateDir_ + L"\\settings.json", text)) {
        json j = json::parse(text, nullptr, false);
        // 0/1/2; older versions wrote "system", "light" or "dark".
        if (j.is_object() && j.contains("theme")) {
            const json& t = j["theme"];
            if (t.is_number_integer()) theme_ = std::clamp(t.get<int>(), 0, 2);
            else if (t.is_string()) theme_ = t == "light" ? 1 : t == "dark" ? 2 : 0;
        }
    }
    if (readFile(model_.dataDir + L"\\brand\\identity.json", text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object()) developer_ = cj::valueOr(j, "developer", developer_);
    }
    downloadPage_ = model_.downloadPage;
    refresh();
}

void CommunityApp::refresh() {
    model_.refreshInstalls();
    const auto nr = cjui::findInstalledApp("launcher", "NorthRayLauncher.exe");
    launcherExe_ = nr.found ? nr.exe : std::wstring{};
    invalidate();
}

std::vector<AppItem*> CommunityApp::cjApps() {
    std::vector<AppItem*> out;
    for (auto& a : model_.apps) if (nrl::Model::isCjPhilipApp(a.entry)) out.push_back(&a);
    std::sort(out.begin(), out.end(), [](auto* x, auto* y) { return x->entry.name < y->entry.name; });
    return out;
}

void CommunityApp::checkCatalogue(bool userAsked) {
    if (!model_.catalogueConfigured()) {
        if (userAsked) flash(L"There's no online catalogue set up yet, so CJ apps can't be downloaded.", 8);
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
}

void CommunityApp::applyTheme() {
    const bool dark = theme_ == 2 || (theme_ == 0 && systemPrefersDark());
    Palette p = makePalette(dark);
    p.ribbon = dark ? hexColor("#C98A97") : hexColor(kWine);
    applyPalette(p);
}

void CommunityApp::saveSettings() {
    json j = {{"theme", theme_}};
    std::ofstream f((stateDir_ + L"\\settings.json").c_str(), std::ios::binary | std::ios::trunc);
    f << j.dump(2);
}

// ------------------------------------------------------------------ actions

static bool start(const std::wstring& exe, const std::wstring& args, const std::wstring& dirIn = {}) {
    std::wstring cmd = L"\"" + exe + L"\"" + (args.empty() ? L"" : L" " + args);
    const std::wstring dir = dirIn.empty() ? exe.substr(0, exe.find_last_of(L"\\/")) : dirIn;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        // Some setups refuse CreateProcess for a per-user app folder; the shell can still open it.
        const auto r = (INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(), dir.c_str(), SW_SHOWNORMAL);
        return r > 32;
    }
    // Let the app come to the front (it brings an already-open window forward itself).
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void CommunityApp::open(const std::string& id) {
    AppItem* a = model_.app(id);
    if (!a) return;
    if (!a->inst.found || GetFileAttributesW(a->inst.exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        flash(w(a->entry.name) + L" isn't installed. Install it here.", 8);
        refresh();
        return;
    }
    const bool running = cjui::isAppRunning(id);
    if (start(a->inst.exe, L"", a->inst.dir)) {
        flash((running ? L"Bringing " : L"Opening ") + w(a->entry.name) + (running ? L" to the front." : L"..."), 4);
        SetTimer(hwnd(), kSoonTimer, 2500, nullptr);   // check back, so the card says Running
    } else {
        wchar_t msg[256] = L"";
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, GetLastError(), 0, msg, 256, nullptr);
        flash(L"Couldn't open " + w(a->entry.name) + L": " + msg, 10);
    }
}

void CommunityApp::install(const std::string& id) {
    AppItem* a = model_.app(id);
    if (!a) return;
    if (!a->release) {
        flash(w(a->entry.name) + L" can't be downloaded right now. " +
                  (model_.catalogueError.empty() ? std::wstring(L"Try again in a moment.") : model_.catalogueError), 8);
        checkCatalogue(true);
        return;
    }
    if (a->inst.found && cjui::isAppRunning(id)) {
        flash(L"Close " + w(a->entry.name) + L" first, then update it.", 6);
        return;
    }
    nrl::JobRequest r;
    r.appId = id;
    r.release = *a->release;
    r.installDir = model_.installDirFor(id);
    r.update = a->inst.found;
    jobs_.enqueue(r);
    invalidate();
}

void CommunityApp::uninstall(const std::string& id) {
    AppItem* a = model_.app(id);
    if (!a || !a->inst.found) return;
    const std::wstring q = L"Uninstall " + w(a->entry.name) + L"? Its settings are kept, so it picks up where it left off if you install it again.";
    if (MessageBoxW(hwnd(), q.c_str(), L"Uninstall", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
    const std::wstring name = w(a->entry.name);
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const std::wstring err = model_.uninstall(id);
    flash(err.empty() ? name + L" was uninstalled." : err, 6);
    refresh();
}

void CommunityApp::openLauncher() {
    if (!launcherExe_.empty()) start(launcherExe_, L"");
    else if (!downloadPage_.empty()) cjui::openInBrowser(downloadPage_ + "#launcher");
}

// -------------------------------------------------------------------- input

bool CommunityApp::onKeyDown(WPARAM vk, bool ctrl, bool) {
    // Ctrl+1..9 opens the nth app.
    if (ctrl && vk >= '1' && vk <= '9') {
        const auto apps = cjApps();
        const size_t i = vk - '1';
        if (i < apps.size()) { if (apps[i]->inst.found) open(apps[i]->entry.id); else install(apps[i]->entry.id); }
        return true;
    }
    if (vk == VK_F5) { load(); checkCatalogue(true); flash(L"Checking for updates...", 3); return true; }
    return false;
}

void CommunityApp::onTimer(UINT_PTR id) {
    if (id == kSoonTimer) KillTimer(hwnd(), id);
    if (!jobs_.anyBusy()) refresh();
    else invalidate();
}

LRESULT CommunityApp::onMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    handled = true;
    if (msg == WM_APP_JOBS) {
        for (AppItem* a : cjApps()) {
            const JobState s = jobs_.state(a->entry.id);
            if (s.stage == JobState::Done && !jobs_.busy(a->entry.id)) {
                const bool was = a->inst.found;
                const std::wstring name = w(a->entry.name);
                const std::string id = a->entry.id;
                model_.refreshInstalls();
                const AppItem* now = model_.app(id);
                if (now && now->inst.found && s.total)
                    flash(name + (s.update || was ? L" was updated." : L" is installed. Open it here."), 6);
                jobs_.forget(id);
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
                model_.catalogueChecked = nrl::nowUtc();
                // Shared with NorthRay Launcher, so either has the latest offline.
                json c;
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
                    for (AppItem* a : cjApps()) if (a->updateAvailable()) ++n;
                    flash(n ? (n == 1 ? std::wstring(L"1 update available.") : std::to_wstring(n) + L" updates available.")
                            : std::wstring(L"Everything is up to date."), 6);
                }
            } else if (r->userAsked) {
                flash(err, 8);
            }
        }
        invalidate();
        return 0;
    }
    if (msg == WM_ACTIVATEAPP && wp) {
        if (!jobs_.anyBusy()) refresh();
        handled = false;
        return 0;
    }
    handled = false;
    return 0;
}

// ------------------------------------------------------------------ drawing

void CommunityApp::progressBar(const Rect& r, float f, const std::string& key) {
    gfx_.fillRounded(r, r.h / 2, pal_.sunken);
    const float shown = anim("progress#" + key, std::clamp(f, 0.0f, 1.0f), 8.0f);
    if (shown > 0) gfx_.fillRounded({r.x, r.y, std::max(r.h, r.w * shown), r.h}, r.h / 2, pal_.ribbon);
}

std::wstring CommunityApp::statusLine(const AppItem& a) const {
    const JobState s = jobs_.state(a.entry.id);
    switch (s.stage) {
        case JobState::Queued: return L"Waiting to start...";
        case JobState::Downloading: return L"Downloading  " + formatSize(s.done) + L" of " + formatSize(s.total);
        case JobState::Verifying: return L"Checking the download...";
        case JobState::Installing: return s.update ? L"Updating..." : L"Installing...";
        case JobState::Failed: return s.message;
        case JobState::Cancelled: return L"Cancelled.";
        default: break;
    }
    if (a.updateAvailable()) return L"Update available: " + w(a.release->version);
    if (a.inst.found) {
        if (cjui::isAppRunning(a.entry.id)) return L"Running";
        return L"Installed" + (a.inst.version.empty() ? std::wstring() : L", version " + w(a.inst.version));
    }
    if (a.release) return L"Not installed  ·  " + formatSize(a.release->size);
    return L"Not installed";
}

void CommunityApp::drawCard(const Rect& r, const AppItem& a) {
    const std::string id = "app:" + a.entry.id;
    const float hv = hoverAmount(id);
    const Rect card{r.x, r.y - 2 * hv, r.w, r.h};
    softShadow(card, 8, 0.35f + 0.65f * hv);
    gfx_.fillRounded(card, 8, pal_.surface);
    gfx_.strokeRounded(card, 8, pal_.rule.mix(pal_.inkFaint, 0.5f * hv));
    const std::string appId = a.entry.id;
    const bool busy = jobs_.busy(appId);
    const bool running = a.inst.found && cjui::isAppRunning(appId);
    // The whole card opens the app (or installs it); the button says which.
    auto act = [this, appId] {
        AppItem* x = model_.app(appId);
        if (!x || jobs_.busy(appId)) return;
        if (x->inst.found) open(appId);
        else install(appId);
    };
    addHit(id, card, act, false);

    // Icon.
    const Rect icon{card.x + 22, card.y + (card.h - 84) / 2, 84, 84};
    if (a.iconPath.empty() || !gfx_.drawImage(a.iconPath, icon, true)) {
        gfx_.fillRounded(icon, 18, hexColor(a.entry.accent.empty() ? "#1B1917" : a.entry.accent));
        gfx_.text(w(a.entry.name.substr(0, 1)), icon, Font::Title, pal_.paper, {Align::Center, false, false, true});
    }
    const float tx = icon.right() + 22;
    const float bw = 120;
    const float tw = card.right() - tx - bw - 40;
    float y = card.y + 22;
    gfx_.text(w(a.entry.name), {tx, y, tw, 26}, Font::Heading, pal_.ink, {Align::Left, false, true});
    y += 28;
    y += gfx_.text(w(a.entry.tagline), {tx, y, tw, 44}, Font::Small, pal_.inkMuted) + 8;
    // Status line with a dot, and what else can be done.
    const JobState s = jobs_.state(appId);
    Color dot = pal_.inkFaint;
    if (s.stage == JobState::Failed) dot = pal_.live;
    else if (a.updateAvailable()) dot = hexColor("#C9922E");
    else if (running) dot = hexColor("#3E8E5E");
    else if (a.inst.found) dot = pal_.ribbon;
    const std::wstring status = statusLine(a);
    gfx_.circle(tx + 4, y + 9, 3.5f, dot, true);
    const bool failed = s.stage == JobState::Failed;
    const float sw = std::min(tw - 14, gfx_.measureWidth(status, failed ? Font::SmallStrong : Font::Small) + 4);
    gfx_.text(status, {tx + 14, y, sw, 18}, failed ? Font::SmallStrong : Font::Small, failed ? pal_.live : pal_.inkMuted,
              {Align::Left, false, true});
    if (!busy && a.inst.found) {
        float lx = tx + 14 + sw + 14;
        if (a.updateAvailable()) {
            linkText("upd:" + appId, lx, y, L"Update", [this, appId] { install(appId); });
            lx += gfx_.measureWidth(L"Update", Font::Small) + 14;
        }
        if (!running) linkText("rm:" + appId, lx, y, L"Uninstall", [this, appId] { uninstall(appId); });
    }

    // Button, or progress while it downloads and installs.
    const Rect b{card.right() - bw - 22, card.y + (card.h - 36) / 2, bw, 36};
    if (busy) {
        const float frac = s.stage == JobState::Downloading && s.total ? (float)s.done / s.total : s.stage == JobState::Queued ? 0.0f : 1.0f;
        progressBar({b.x - 150, b.y + b.h / 2 - 3, 138, 6}, frac, appId);
        if (s.stage == JobState::Queued || s.stage == JobState::Downloading)
            button("cancel:" + appId, b, L"Cancel", [this, appId] { jobs_.cancel(appId); });
        else
            disabledButton(b, s.update ? L"Updating" : L"Installing");
        invalidate();   // keep the progress moving
        return;
    }
    const std::wstring label = a.inst.found ? (running ? L"Show" : L"Open") : L"Install";
    if (!a.inst.found && !a.release) disabledButton(b, L"Install");
    else button("btn:" + appId, b, label, act, true);
}

float CommunityApp::drawCards(const Rect& r) {
    float y = r.y;
    const auto apps = cjApps();
    if (apps.empty()) {
        y += gfx_.text(L"No CJ apps were found. Reinstall CJ Philip Community from NorthRay Launcher.", {r.x, y, r.w, 60},
                       Font::Body, pal_.inkMuted) + 10;
        return y - r.y;
    }
    for (const AppItem* a : apps) {
        drawCard({r.x, y, r.w, 132}, *a);
        y += 132 + 16;
    }
    y += 4;
    y += gfx_.text(apps.size() == 1 ? L"More CJ apps will appear here as they're released."
                                    : L"New CJ apps appear here as they're released.",
                   {r.x, y, r.w, 22}, Font::Small, pal_.inkFaint) + 8;
    return y - r.y;
}

void CommunityApp::drawFrame(float W, float H) {
    // Header: the wordmark on a deep band, with a fine rule of the accent.
    const Rect head{0, 0, W, kHeaderH};
    gfx_.fill(head, pal_.dark ? hexColor("#1E1418") : hexColor("#2A1A20"));
    gfx_.fill({0, kHeaderH - 3, W, 3}, pal_.ribbon);
    const float pad = std::max(28.0f, (W - 720) / 2);
    gfx_.bookmark({pad, 0, 12, 34}, pal_.ribbon.mix(hexColor("#FFFFFF"), 0.2f));
    gfx_.text(L"CJ Philip", {pad + 26, 30, 400, 34}, Font::SerifTitle, hexColor("#F4EDE6"), {Align::Left, false});
    gfx_.text(L"COMMUNITY", {pad + 28, 68, 300, 16}, Font::Label, hexColor("#CDB9BF"), {Align::Left, false});
    // Theme switch: one small button that cycles.
    {
        static const wchar_t* names[] = {L"Theme: as Windows", L"Theme: light", L"Theme: dark"};
        const float tw = gfx_.measureWidth(names[theme_], Font::Small) + 24;
        const Rect tb{W - pad - tw, 36, tw, 28};
        const std::string id = "theme";
        addHit(id, tb, [this] { theme_ = (theme_ + 1) % 3; saveSettings(); applyTheme(); });
        const float hv = hoverAmount(id);
        gfx_.strokeRounded(tb, 14, hexColor("#CDB9BF").withAlpha(0.35f + 0.4f * hv));
        gfx_.text(names[theme_], tb, Font::Small, hexColor("#E8DCD6"), {Align::Center, false, false, true});
        if (focused(id)) focusRing(tb);
    }

    // Cards.
    const Rect area{0, kHeaderH, W, H - kHeaderH - kFooterH};
    const float top = beginScroll(area);
    float y = top + 28;
    gfx_.text(L"YOUR CJ APPS", {pad, y, 400, 16}, Font::Label, pal_.inkFaint, {Align::Left, false});
    {
        const std::wstring chk = checking_ ? L"Checking..." : L"Check for updates";
        const float cw = gfx_.measureWidth(chk, Font::Small);
        if (!checking_) linkText("check", W - pad - cw, y - 2, chk, [this] { checkCatalogue(true); });
        else gfx_.text(chk, {W - pad - cw, y - 2, cw + 4, 18}, Font::Small, pal_.inkFaint, {Align::Left, false});
    }
    y += 28;
    y += drawCards({pad, y, W - pad * 2, 0});
    endScroll(y - top + 20);

    // Footer.
    const Rect foot{0, H - kFooterH, W, kFooterH};
    gfx_.fill(foot, pal_.sunken);
    gfx_.line(0, foot.y + 0.5f, W, foot.y + 0.5f, pal_.rule);
    const std::wstring left = !flashText().empty() ? flashText() : w(developer_) + L"  ·  Version " CJ_VERSION_WSTR;
    gfx_.text(left, {pad, foot.y, W - pad * 2 - 260, foot.h}, Font::Small,
              !flashText().empty() ? flashColor(pal_.ink) : pal_.inkFaint, {Align::Left, false, true, true});
    const std::wstring more = launcherExe_.empty() ? L"Get NorthRay Launcher" : L"Open NorthRay Launcher";
    const float mw = gfx_.measureWidth(more, Font::Small);
    if (!launcherExe_.empty() || !downloadPage_.empty())
        linkText("launcher", W - pad - mw, foot.y + (foot.h - 18) / 2, more, [this] { openLauncher(); });
}

}  // namespace cjp

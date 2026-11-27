#include "cjui/window.hpp"
#include "cjui/icons.hpp"
#include "cjui/menu.hpp"

#include <dwmapi.h>
#include <windowsx.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cmath>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace cjui {

static const UINT_PTR kFlashTimer = 0xC001;
static const UINT_PTR kAnimTimer = 0xC002;
static const UINT_PTR kTipTimer = 0xC00A;
static const ULONGLONG kTipDelayMs = 550;
static const UINT_PTR kAmbientTimer = 0xC003;

static double g_ambientTime = -1;
static bool g_ambientWanted = false;
double ambientTime() { return g_ambientTime; }
void wantAmbientFrame() { g_ambientWanted = true; }

Palette makePalette(bool dark) {
    Palette p;
    if (!dark) {
        p.paper = hexColor("#F3EFE6"); p.surface = hexColor("#FBF9F4"); p.sunken = hexColor("#E9E4D8");
        p.ink = hexColor("#1E1D1A"); p.inkMuted = hexColor("#5E5A50"); p.inkFaint = hexColor("#8C8778");
        p.rule = hexColor("#D6D0C2"); p.ribbon = hexColor("#6A2E3F"); p.focus = hexColor("#1F5FA8");
        p.live = hexColor("#C3311F"); p.demo = hexColor("#8A6A12");
    } else {
        p.paper = hexColor("#18181A"); p.surface = hexColor("#202023"); p.sunken = hexColor("#121214");
        p.ink = hexColor("#ECE8DE"); p.inkMuted = hexColor("#A9A498"); p.inkFaint = hexColor("#76726A");
        p.rule = hexColor("#34333A"); p.ribbon = hexColor("#C98A97"); p.focus = hexColor("#7FB0EE");
        p.live = hexColor("#F0533E"); p.demo = hexColor("#D9B65A");
    }
    // The console look stays dark in both themes; in dark mode it's lifted and
    // outlined so it doesn't sink into the page.
    p.console = dark ? hexColor("#26221F") : hexColor("#1B1917");
    p.consoleInk = hexColor("#F1ECE2");
    p.consoleMuted = hexColor("#A39C8F");
    p.consoleRule = dark ? hexColor("#443D37") : hexColor("#3A3530");
    p.dark = dark;
    return p;
}

bool systemPrefersDark() {
    DWORD value = 1, size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

LRESULT CALLBACK AppWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppWindow* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self) return self->handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------- interface zoom

namespace {
const wchar_t* kZoomProp = L"NorthRay.InterfaceZoom";   // on the window, in thousandths, for its menus and boxes
const wchar_t* kWholeProp = L"NorthRay.ScaleWholeWindow";   // set when the bars, menus and boxes scale too (0.185)

// %LOCALAPPDATA%\NorthRay\interface.ini: [Zoom] <window class>=1.25. Per app, on this PC only.
std::wstring zoomFile() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir = p;
    if (p) CoTaskMemFree(p);
    if (dir.empty()) return {};
    dir += L"\\NorthRay";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\interface.ini";
}
float clampZoom(float z) { return std::clamp(z, 0.5f, 2.0f); }
}  // namespace

const std::vector<float>& zoomSteps() {
    static const std::vector<float> s{0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f};
    return s;
}

float interfaceZoomOf(HWND window) {
    if (!window) return 1.0f;
    const HWND root = GetAncestor(window, GA_ROOT);
    const auto v = reinterpret_cast<INT_PTR>(GetPropW(root ? root : window, kZoomProp));
    return v > 0 ? clampZoom(v / 1000.0f) : 1.0f;
}

UINT uiDpiOf(HWND window) {
    UINT d = window ? GetDpiForWindow(window) : 0;
    if (!d) d = 96;
    if (!window) return d;
    const HWND root = GetAncestor(window, GA_ROOT);
    if (!GetPropW(root ? root : window, kWholeProp)) return d;
    return (UINT)std::lround(d * interfaceZoomOf(window));
}

void AppWindow::applyDpi() {
    const UINT d = displayDpi();
    gfx_.setDpi(whole_ ? d * zoom_ : (float)d);
}

void AppWindow::setScaleWholeWindow(bool on) {
    if (on == whole_) return;
    whole_ = on;
    if (hwnd_) {
        if (on) SetPropW(hwnd_, kWholeProp, reinterpret_cast<HANDLE>(1));
        else RemovePropW(hwnd_, kWholeProp);
        if (const std::wstring f = zoomFile(); !f.empty())
            WritePrivateProfileStringW(L"WholeWindow", spec_.className, on ? L"1" : L"0", f.c_str());
        applyDpi();
        onInterfaceZoomChanged();
        invalidate();
    }
}

UINT AppWindow::displayDpi() const {
    const UINT d = hwnd_ ? GetDpiForWindow(hwnd_) : 0;
    return d ? d : 96;
}

void AppWindow::setInterfaceZoom(float zoom, bool announce) {
    zoom = clampZoom(zoom);
    if (std::fabs(zoom - 1.0f) < 0.005f) zoom = 1.0f;
    const bool changed = std::fabs(zoom - zoom_) > 0.001f;
    zoom_ = zoom;
    if (!hwnd_) return;
    SetPropW(hwnd_, kZoomProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(std::lround(zoom_ * 1000))));
    if (!changed) return;
    if (const std::wstring f = zoomFile(); !f.empty()) {
        wchar_t v[16];
        swprintf(v, 16, L"%.2f", zoom_);
        WritePrivateProfileStringW(L"Zoom", spec_.className, v, f.c_str());
    }
    // Room for the layout: a window smaller than its smallest size at this zoom grows, as far as the screen allows.
    {
        RECT wr{}, cr{};
        GetWindowRect(hwnd_, &wr);
        GetClientRect(hwnd_, &cr);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        const float s = displayDpi() / 96.0f * zoom_;
        const int needW = (int)std::lround(spec_.minWidth * s), needH = (int)std::lround(spec_.minHeight * s);
        if (!maximized() && (cr.right < needW || cr.bottom < needH)) {
            const int w = std::min<int>(std::max<int>(wr.right - wr.left, needW + (wr.right - wr.left - cr.right)), mi.rcWork.right - mi.rcWork.left);
            const int h = std::min<int>(std::max<int>(wr.bottom - wr.top, needH + (wr.bottom - wr.top - cr.bottom)), mi.rcWork.bottom - mi.rcWork.top);
            const int x = std::clamp<int>(wr.left, mi.rcWork.left, mi.rcWork.right - w), y = std::clamp<int>(wr.top, mi.rcWork.top, mi.rcWork.bottom - h);
            SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (whole_) applyDpi();
    onInterfaceZoomChanged();
    if (announce) flash(L"Interface size " + std::to_wstring((int)std::lround(zoom_ * 100)) + L"%. " +
                        (ctrlZoomKeysAreApps() ? L"Ctrl+Alt+0" : L"Ctrl+0") + L" goes back to 100%.", 3);
    invalidate();
}

void AppWindow::zoomStep(int direction) {
    if (direction == 0) { setInterfaceZoom(1.0f); return; }
    const auto& s = zoomSteps();
    float next = zoom_;
    if (direction > 0) { for (float v : s) if (v > zoom_ + 0.004f) { next = v; break; } }
    else for (auto it = s.rbegin(); it != s.rend(); ++it) if (*it < zoom_ - 0.004f) { next = *it; break; }
    setInterfaceZoom(next);
}

float AppWindow::zoomControl(const std::string& id, float x, float y, float w) {
    const float y0 = y;
    struct Preset { const wchar_t* name; float zoom; };
    static const Preset presets[] = {{L"Small", 0.9f}, {L"Default", 1.0f}, {L"Large", 1.25f}, {L"Extra large", 1.5f}};
    int sel = -1;
    for (int i = 0; i < 4; ++i) if (std::fabs(presets[i].zoom - zoom_) < 0.005f) sel = i;
    segmented(id + ":presets", {x, y, std::min(w, 440.0f), 30}, {presets[0].name, presets[1].name, presets[2].name, presets[3].name}, sel,
              [this](int i) { setInterfaceZoom(presets[i].zoom); });
    y += 40;
    const float pct = 0.68f * std::min(w, 440.0f);
    // 50% to 200%, in steps of 5%.
    slider(id + ":slider", {x, y, pct, 26}, (zoom_ - 0.5f) / 1.5f, [this](float v) {
        const float z = std::round((0.5f + v * 1.5f) * 20.0f) / 20.0f;
        setInterfaceZoom(z, false);
    });
    gfx_.text(std::to_wstring((int)std::lround(zoom_ * 100)) + L"%", {x + pct + 14, y, 80, 26}, Font::BodyStrong, pal_.ink,
              {Align::Left, false, true, true});
    y += 34;
    const std::wstring keys = ctrlZoomKeysAreApps() ? L"Ctrl+Alt and + or − change it too, and Ctrl+Alt+0 goes back to 100%."
                                                    : L"Ctrl and + or −, or Ctrl with the mouse wheel, change it too; Ctrl+0 goes back to 100%.";
    y += gfx_.text(keys + (whole_ ? L" It sizes the whole window, menus and messages too. Kept for this app on this PC."
                                  : L" It sizes what's in the window; the bars along the top and side stay as they are. Kept for this app on this PC."),
                   {x, y, w, 60}, Font::Small, pal_.inkFaint, {Align::Left, false, true}) + 8;
    // Whole-window scaling (0.185), off by default.
    {
        const std::string cid = id + ":whole";
        const std::wstring label = wholeLabel_.empty() ? L"Also scale the bars along the top and side, menus and messages" : wholeLabel_;
        const float th = std::max(22.0f, gfx_.measureHeight(label, w - 30, Font::Body));
        const Rect r{x, y, w, th + 4};
        addHit(cid, r, [this] { setScaleWholeWindow(!whole_); });
        const float hv = hovered(cid) ? 1.0f : 0.0f;
        const Rect b{x, y + 3, 18, 18};
        if (whole_) gfx_.fillRounded(b, 4, pal_.ribbon);
        gfx_.strokeRounded(b, 4, whole_ ? pal_.ribbon : pal_.inkMuted.mix(pal_.ink, hv), 1.5f);
        if (whole_) {
            gfx_.line(b.x + 4, b.y + 9, b.x + 8, b.y + 14, pal_.paper, 2);
            gfx_.line(b.x + 8, b.y + 14, b.x + 15, b.y + 4, pal_.paper, 2);
        }
        gfx_.text(label, {x + 30, y + 1, w - 30, th}, Font::Body, pal_.ink, {Align::Left, false, true});
        if (focused(cid)) focusRing(r);
        y += th + 10;
    }
    return y - y0;
}

Rect AppWindow::beginZoomArea(const Rect& area) {
    if (inZoom_) endZoomArea();
    inZoom_ = true;
    zoomArea_ = area;
    const float z = contentZoom();
    areaZoom_ = z;
    gfx_.beginZoom(area, z);
    return {area.x, area.y, area.w / z, area.h / z};
}

void AppWindow::endZoomArea() {
    if (!inZoom_) return;
    gfx_.endZoom();
    inZoom_ = false;
}

Rect AppWindow::toWindow(const Rect& r) const {
    const Rect& a = inZoom_ ? zoomArea_ : lastZoomArea_;
    const float z = inZoom_ ? areaZoom_ : lastAreaZoom_;
    if (a.w <= 0 || z == 1.0f) return r;
    // Only what's in the area's own space: a bar's rect (left of it or above it) is already the window's.
    const Rect inside{a.x, a.y, a.w / z, a.h / z};
    if (!inside.contains(r.x + std::min(r.w, 1.0f) * 0.5f, r.y + std::min(r.h, 1.0f) * 0.5f)) return r;
    return {a.x + (r.x - a.x) * z, a.y + (r.y - a.y) * z, r.w * z, r.h * z};
}

void AppWindow::mapPoint(float& x, float& y) const {
    if (lastZoomArea_.w <= 0 || lastAreaZoom_ == 1.0f || !lastZoomArea_.contains(x, y)) return;
    x = lastZoomArea_.x + (x - lastZoomArea_.x) / lastAreaZoom_;
    y = lastZoomArea_.y + (y - lastZoomArea_.y) / lastAreaZoom_;
}

bool AppWindow::createWindow(const WindowSpec& spec) {
    spec_ = spec;
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = spec.instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (spec.iconId) wc.hIcon = LoadIconW(spec.instance, MAKEINTRESOURCEW(spec.iconId));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = spec.className;
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    hwnd_ = CreateWindowExW(0, spec.className, spec.title, spec.style, CW_USEDEFAULT, CW_USEDEFAULT,
                            MulDiv(spec.width, dpi, 96), MulDiv(spec.height, dpi, 96), nullptr, nullptr,
                            spec.instance, this);
    if (!hwnd_) return false;
    if (!gfx_.init(hwnd_)) return false;
    // The interface zoom kept for this app on this PC (suite 0.172).
    {
        float z = 1.0f;
        if (const std::wstring f = zoomFile(); !f.empty()) {
            wchar_t v[32] = {0};
            GetPrivateProfileStringW(L"Zoom", spec.className, L"1", v, 32, f.c_str());
            z = (float)_wtof(v);
            if (!(z > 0)) z = 1.0f;
        }
        zoom_ = clampZoom(z);
        SetPropW(hwnd_, kZoomProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(std::lround(zoom_ * 1000))));
        if (const std::wstring f = zoomFile(); !f.empty())
            whole_ = GetPrivateProfileIntW(L"WholeWindow", spec.className, 0, f.c_str()) != 0;
        if (whole_) SetPropW(hwnd_, kWholeProp, reinterpret_cast<HANDLE>(1));
    }
    applyDpi();
    readAnimationSetting();
    captionHeight_ = spec.captionHeight;
    // Recalculate the frame now that WM_NCCALCSIZE is handled: the title bar goes.
    if (spec.customFrame)
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return true;
}

// ---------------------------------------------------------------- animation

void AppWindow::readAnimationSetting() {
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) on = TRUE;
    animationsOn_ = on != FALSE;
}

double AppWindow::clockSeconds() const {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

float AppWindow::anim(const std::string& key, float target, float speed) {
    auto it = anims_.find(key);
    if (it == anims_.end()) {
        anims_[key] = {target, frame_};
        return target;
    }
    AnimState& a = it->second;
    if (a.frame == frame_) return a.value;  // already advanced this frame
    a.frame = frame_;
    if (!animationsOn_) { a.value = target; return target; }
    const float diff = target - a.value;
    const float eps = std::max(0.002f, std::abs(target) * 0.0008f);
    if (std::abs(diff) <= eps) { a.value = target; return target; }
    a.value += diff * (1.0f - std::exp(-speed * frameDt_));
    if (std::abs(target - a.value) <= eps) a.value = target;
    else animating_ = true;
    return a.value;
}

void AppWindow::setAnim(const std::string& key, float value) { anims_[key] = {value, frame_}; }

float AppWindow::pageProgress() {
    if (!animationsOn_) return 1.0f;
    const double t = (clockSeconds() - pageStart_) / 0.28;   // 280 ms
    if (t >= 1.0) return 1.0f;
    animating_ = true;
    const float x = (float)std::max(0.0, t);
    return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);     // ease-out cubic
}

void AppWindow::softShadow(const Rect& r, float radius, float amount) {
    if (amount <= 0.01f || gfx_.culled({r.x - 4, r.y, r.w + 8, r.h + 8})) return;
    const Color shade = pal_.dark ? Color{0, 0, 0, 1} : Color{0.1f, 0.08f, 0.05f, 1};
    for (int i = 1; i <= 4; ++i) {
        const float spread = (float)i * 1.5f;
        gfx_.fillRounded({r.x - spread * 0.5f, r.y + spread, r.w + spread, r.h}, radius + spread,
                         shade.withAlpha((pal_.dark ? 0.09f : 0.035f) * amount));
    }
}

Color AppWindow::flashColor(Color c) {
    const float a = anim("#flash", flashActive_ ? 1.0f : 0.0f, 12.0f);
    if (!flashActive_ && a <= 0.01f) flashVisible_ = false;
    return c.withAlpha(c.a * a);
}

void AppWindow::drawToast(const Rect& area) {
    if (flashText().empty()) return;
    const float a = flashColor(Color{1, 1, 1, 1}).a;
    if (a <= 0.01f) return;
    const std::wstring& t = flashText();
    if (calm_) {
        // A small note in the bottom-right corner, sliding in from the right; Undo in it when there's something to undo.
        const bool undo = (bool)flashUndo_;
        const float uw = undo ? gfx_.measureWidth(L"Undo", Font::SmallStrong) + 40 : 0;
        const float maxW = std::min(area.w - 48, 440.0f);
        const float tw = std::min(gfx_.measureWidth(t, Font::Small), maxW - 28 - uw);
        const float th = gfx_.measureHeight(t, tw + 1, Font::Small);
        const float w = tw + 28 + uw, h = std::max(36.0f, th + 18);
        const float slide = (1.0f - a) * 28;
        const Rect r{area.right() - w - 20 + slide, area.bottom() - h - 20, w, h};
        softShadow(r, 9, a * 0.8f);
        gfx_.fillRounded(r, 9, pal_.surface.withAlpha(0.98f * a));
        gfx_.strokeRounded(r, 9, pal_.rule.withAlpha(a));
        gfx_.fillRounded({r.x, r.y + 8, 3, r.h - 16}, 1.5f, pal_.ribbon.withAlpha(a));
        gfx_.text(t, {r.x + 14, r.y + (h - th) / 2, tw + 1, th + 2}, Font::Small, pal_.ink.withAlpha(a), {Align::Left, true});
        if (undo) {
            const Rect b{r.right() - uw + 4, r.y + (h - 26) / 2, uw - 12, 26};
            addHit("#toast:undo", b, [this] {
                auto u = flashUndo_;
                flashUndo_ = nullptr;
                flashActive_ = false;
                if (u) u();
                invalidate();
            });
            const float hv = hoverAmount("#toast:undo");
            gfx_.fillRounded(b, 6, pal_.ribbon.withAlpha((0.12f + 0.12f * hv) * a));
            gfx_.text(L"Undo", b, Font::SmallStrong, pal_.ink.withAlpha(a), {Align::Center, false, false, true});
        }
        return;
    }
    const float maxW = std::min(area.w - 48, 620.0f);
    const float tw = std::min(gfx_.measureWidth(t, Font::Body), maxW - 36);
    const float th = gfx_.measureHeight(t, tw + 1, Font::Body);
    const float w = tw + 36, h = std::max(40.0f, th + 20);
    const float rise = (1.0f - a) * 10;
    const Rect r{area.x + (area.w - w) / 2, area.bottom() - h - 24 + rise, w, h};
    softShadow(r, 10, a);
    const Color bg = pal_.dark ? pal_.surface.mix(pal_.ink, 0.12f) : pal_.ink.mix(pal_.paper, 0.06f);
    const Color fg = pal_.dark ? pal_.ink : pal_.paper;
    gfx_.fillRounded(r, 10, bg.withAlpha(0.96f * a));
    gfx_.text(t, {r.x + 18, r.y + (h - th) / 2, tw + 1, th + 2}, Font::Body, fg.withAlpha(a), {Align::Center, true});
}

void AppWindow::applyPalette(const Palette& p) {
    pal_ = p;
    setMenuPalette(p);   // pop-up menus match the window
    BOOL useDark = p.dark;
    if (hwnd_) DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof useDark);
    invalidate();
}

void AppWindow::invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// ------------------------------------------------------------------ scroll

float AppWindow::beginScroll(const Rect& clip) {
    contentClip_ = clip;
    viewportHeight_ = clip.h;
    const float maxScroll = std::max(0.0f, contentHeight_ - viewportHeight_);
    scrollTarget_ = std::clamp(scrollTarget_, 0.0f, maxScroll);
    // Glide towards the target rather than jumping.
    if (!animationsOn_ || std::abs(scrollTarget_ - scroll_) < 0.5f) scroll_ = scrollTarget_;
    else {
        scroll_ += (scrollTarget_ - scroll_) * (1.0f - std::exp(-22.0f * frameDt_));
        animating_ = true;
    }
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
    inScroll_ = true;
    gfx_.pushClip(clip);
    // New pages rise a few pixels as they fade in.
    const float p = pageProgress();
    return clip.y - scroll_ + (1.0f - p) * (calm_ ? 26.0f : 14.0f);
}

void AppWindow::endScroll(float contentHeight) {
    const float p = pageProgress();
    if (p < 1.0f) gfx_.fill(contentClip_, pal_.paper.withAlpha((1.0f - p) * 0.95f));
    gfx_.popClip();
    inScroll_ = false;
    contentHeight_ = contentHeight;
    // No scroll bars anywhere: the wheel, keys and dragging still scroll.
}

void AppWindow::scrollBy(float dy) {
    const float before = scrollTarget_;
    scrollTarget_ = std::clamp(scrollTarget_ + dy, 0.0f, std::max(0.0f, contentHeight_ - viewportHeight_));
    if (scrollTarget_ != before) {
        lastScrollTime_ = clockSeconds();
        invalidate();
    }
}

void AppWindow::resetScroll() {
    scroll_ = scrollTarget_ = 0;
    pageStart_ = clockSeconds();
    invalidate();
}

// ---------------------------------------------------------------- controls

void AppWindow::addHit(const std::string& id, const Rect& r, std::function<void()> action, bool focusable) {
    const Rect clip = inScroll_ ? contentClip_ : hitClipSet_ ? hitClip_ : Rect{-1e6f, -1e6f, 2e6f, 2e6f};
    hits_.push_back({id, r, clip, std::move(action), focusable});
}

void AppWindow::focusRing(const Rect& r) { gfx_.strokeRounded(r.inset(-3), calm_ ? 9.0f : 4.0f, pal_.focus, 2); }

// The calm controls' one look (a button, a small button, a link-button): the surface with an outline, a 7-DIP radius,
// lifting a little and gaining a shadow when pointed at, pressing in when clicked; an icon before the words.
void AppWindow::calmFace(const std::string& id, const Rect& r, const std::wstring& label, Font f, bool enabled) {
    const float h = enabled ? hoverAmount(id) : 0, d = enabled ? anim(id + "#press", pressed(id) ? 1.0f : 0.0f, 30.0f) : 0;
    const Rect b{r.x, r.y - 1.5f * h + 1.0f * d, r.w, r.h};
    if (h > 0.01f) softShadow(b, 7, 0.55f * h * (1 - d));
    gfx_.fillRounded(b, 7, enabled ? pal_.surface.mix(pal_.sunken, 0.35f * h + 0.6f * d) : pal_.sunken);
    gfx_.strokeRounded(b, 7, enabled ? pal_.rule.mix(pal_.inkFaint, 0.55f * h) : pal_.rule);
    const Color ink = enabled ? pal_.ink.mix(pal_.inkMuted, 0.12f * (1 - h)) : pal_.inkFaint;
    std::wstring text = label;
    Icon ic = iconFor(label);
    if (text.rfind(L"+ ", 0) == 0) text.erase(0, 2);
    else if (text.size() > 1 && text[0] == L'\u2039') text.erase(0, text.size() > 1 && text[1] == L' ' ? 2 : 1);
    if (text.size() <= 1) ic = Icon::None;   // "+" and "-" on their own are already the symbol
    // The writer's choice of how commands show (0.87): icons alone (their words as a tooltip), or words alone.
    if (labelStyle() == LabelStyle::WordsOnly) ic = Icon::None;
    if (labelStyle() == LabelStyle::IconsOnly && ic != Icon::None) {
        const float isz = std::min(16.0f, b.h - 8);
        drawIcon(gfx_, ic, {b.x + (b.w - isz) / 2, b.y + (b.h - isz) / 2, isz, isz}, enabled ? pal_.ink.mix(pal_.inkMuted, 0.2f * (1 - h)) : pal_.inkFaint, 1.5f);
        if (!id.empty() && id[0] != '#') tooltip(id, text);
        if (enabled && focused(id)) focusRing(b);
        return;
    }
    float isz = ic == Icon::None ? 0 : std::min(15.0f, b.h - 8);
    if (isz && gfx_.measureWidth(text, f) + isz + 6 + 10 > b.w) isz = 0;   // no room: the words come first
    const float tw = std::min(gfx_.measureWidth(text, f), b.w - (isz ? isz + 16 : 14));
    const float total = (isz ? isz + 6 : 0) + tw;
    const float x0 = b.x + std::max(5.0f, (b.w - total) / 2);
    if (isz) drawIcon(gfx_, ic, {x0, b.y + (b.h - isz) / 2, isz, isz}, enabled ? pal_.inkMuted.mix(pal_.ink, h) : pal_.inkFaint, 1.4f);
    gfx_.text(text, {x0 + (isz ? isz + 6 : 0), b.y, tw + 2, b.h}, f, ink, {Align::Left, false, true, true});
    if (enabled && focused(id)) focusRing(b);
}

float AppWindow::buttonWidth(const std::wstring& label, Font f) {
    std::wstring text = label;
    if (text.rfind(L"+ ", 0) == 0) text.erase(0, 2);
    Icon ic = text.size() <= 1 ? Icon::None : iconFor(label);
    if (labelStyle() == LabelStyle::WordsOnly) ic = Icon::None;
    if (labelStyle() == LabelStyle::IconsOnly && ic != Icon::None) return 36;
    return gfx_.measureWidth(text, f) + 26 + (ic == Icon::None ? 0 : 21);
}

bool AppWindow::button(const std::string& id, const Rect& r, const std::wstring& label, std::function<void()> action,
                       bool primary) {
    addHit(id, r, std::move(action));
    const bool hov = hovered(id), down = pressed(id);
    if (calm_) {
        calmFace(id, r, label, Font::Body, true);
        return hov;
    }
    const float h = hoverAmount(id);
    const float d = anim(id + "#press", down ? 1.0f : 0.0f, 30.0f);
    if (primary) {
        Color bg = pal_.ink.mix(pal_.paper, 0.15f * h + 0.13f * d);
        gfx_.fillRounded(r, 3, bg);
        gfx_.text(label, r, Font::BodyStrong, pal_.paper, {Align::Center, false, true, true});
    } else {
        gfx_.fillRounded(r, 3, pal_.surface.mix(pal_.sunken, 0.5f * h + 0.5f * d));
        gfx_.strokeRounded(r, 3, pal_.rule.mix(pal_.inkFaint, h));
        gfx_.text(label, r, Font::Body, pal_.ink, {Align::Center, false, true, true});
    }
    if (focused(id)) focusRing(r);
    return hov;
}

void AppWindow::disabledButton(const Rect& r, const std::wstring& label) {
    if (calm_) {
        calmFace("#disabled", r, label, Font::Body, false);
        return;
    }
    gfx_.fillRounded(r, 3, pal_.sunken);
    gfx_.text(label, r, Font::Body, pal_.inkFaint, {Align::Center, false, true, true});
}

float AppWindow::linkButton(const std::string& id, float x, float y, const std::wstring& label, std::function<void()> action, Font f) {
    std::wstring text = label;
    if (text.rfind(L"+ ", 0) == 0) text.erase(0, 2);
    Icon ic = text.size() > 1 ? iconFor(label) : Icon::None;
    const LabelStyle st = labelStyle();
    if (st == LabelStyle::WordsOnly || !calm_) ic = Icon::None;
    const bool iconOnly = st == LabelStyle::IconsOnly && ic != Icon::None;
    const float h = gfx_.measureHeight(L"Ag", 1000.0f, f);
    const float isz = std::min(14.0f, h + 1);
    const float tw = iconOnly ? 0 : gfx_.measureWidth(text, f);
    const float w = (ic != Icon::None ? isz : 0) + (ic != Icon::None && !iconOnly ? 6 : 0) + tw;
    const Rect box{x - 7, y - 3.5f, w + 14, h + 7};
    addHit(id, box, std::move(action));
    if (iconOnly) tooltip(id, text);
    const bool hov = hovered(id);
    const float ha = hoverAmount(id), dn = anim(id + "#press", pressed(id) ? 1.0f : 0.0f, 30.0f);
    const Rect b{box.x, box.y - 1.5f * ha + dn, box.w, box.h};
    if (ha > 0.01f) softShadow(b, 7, 0.5f * ha * (1 - dn));
    gfx_.fillRounded(b, 7, pal_.surface.mix(pal_.sunken, 0.35f * ha + 0.6f * dn));
    gfx_.strokeRounded(b, 7, pal_.rule.mix(pal_.inkFaint, 0.55f * ha));
    const Color ink = pal_.ink.mix(pal_.inkMuted, 0.12f * (1 - ha));
    const float dy = b.y - box.y;
    float tx = x;
    if (ic != Icon::None) {
        drawIcon(gfx_, ic, {x, y + dy + (h - isz) / 2, isz, isz}, pal_.inkMuted.mix(pal_.ink, ha), 1.3f);
        tx += isz + 6;
    }
    if (!iconOnly) gfx_.text(text, {tx, y + dy, tw + 2, h}, f, ink, {Align::Left, false});
    if (focused(id)) focusRing(b);
    (void)hov;
    return box.w + 12;
}

bool AppWindow::linkText(const std::string& id, float x, float y, const std::wstring& label,
                         std::function<void()> action, Font f) {
    // Icons only (0.88): a link whose words have an icon shows the icon alone, from where its words would start; the
    // words are its tooltip.
    if (calm_ && labelStyle() == LabelStyle::IconsOnly && label.size() > 2 && label[0] != L'\u2039')
        if (iconFor(label) != Icon::None) {
            linkButton(id, x, y, label, std::move(action), f);
            return hovered(id);
        }
    // A small button: plain text in a soft frame, lifting when pointed at. It's laid out where a
    // link would be (callers measure the label), with its frame 7 DIPs around the text.
    // "‹ Back" and "+ Add" get a drawn chevron or plus in place of the characters.
    const Font strong = f;
    std::wstring text = label;
    int icon = 0;   // 1 chevron, 2 plus
    float iw = 0;
    if (text.size() > 1 && text[0] == L'\u2039') { icon = 1; const size_t n = text[1] == L' ' ? 2 : 1; iw = gfx_.measureWidth(text.substr(0, n) + L"x", f) - gfx_.measureWidth(L"x", f); text.erase(0, n); }
    else if (text.rfind(L"+ ", 0) == 0) { icon = 2; iw = gfx_.measureWidth(L"+ x", f) - gfx_.measureWidth(L"x", f); text.erase(0, 2); }
    if (icon) iw = std::max(iw, 13.0f);   // room for the drawn mark and a gap
    const float tw = gfx_.measureWidth(text, strong);
    const float h = gfx_.measureHeight(text, tw + 2, strong);
    const float w = iw + tw;
    Rect r{x, y, w, h};
    const Rect box{x - 7, y - 3.5f, w + 14, h + 7};
    addHit(id, box, std::move(action));
    const bool hov = hovered(id);
    if (calm_) {   // the one look; only the drawn marks it always had (‹, +), so it keeps the room its caller measured
        const float ha = hoverAmount(id), dn = anim(id + "#press", pressed(id) ? 1.0f : 0.0f, 30.0f);
        const Rect b{box.x, box.y - 1.5f * ha + dn, box.w, box.h};
        if (ha > 0.01f) softShadow(b, 7, 0.5f * ha * (1 - dn));
        gfx_.fillRounded(b, 7, pal_.surface.mix(pal_.sunken, 0.35f * ha + 0.6f * dn));
        gfx_.strokeRounded(b, 7, pal_.rule.mix(pal_.inkFaint, 0.55f * ha));
        const Color ink = pal_.ink.mix(pal_.inkMuted, 0.12f * (1 - ha));
        const float dy = b.y - box.y, cy = y + h / 2 + dy;
        if (icon == 1) {
            const float cx = x + 4;
            gfx_.line(cx + 1.5f, cy - 3.5f, cx - 2, cy, ink, 1.5f);
            gfx_.line(cx - 2, cy, cx + 1.5f, cy + 3.5f, ink, 1.5f);
        } else if (icon == 2) {
            const float cx = x + 4.5f;
            gfx_.line(cx - 3.5f, cy, cx + 3.5f, cy, ink, 1.5f);
            gfx_.line(cx, cy - 3.5f, cx, cy + 3.5f, ink, 1.5f);
        }
        gfx_.text(text, {x + iw, y + dy, tw + 2, h}, strong, ink, {Align::Left, false});
        if (focused(id)) focusRing(b);
        return hov;
    }
    const float a = hoverAmount(id);
    const bool down = pressed(id);
    gfx_.fillRounded(box, 6, pal_.ink.withAlpha((pal_.dark ? 0.06f : 0.04f) + (down ? 0.10f : 0.06f) * a));
    gfx_.strokeRounded(box, 6, pal_.rule.mix(pal_.inkFaint, 0.6f * a), 1);
    const Color ink = pal_.ink.mix(pal_.inkMuted, 0.15f * (1 - a));
    const float cy = y + h / 2;
    if (icon == 1) {
        const float cx = x + 4;
        gfx_.line(cx + 1.5f, cy - 3.5f, cx - 2, cy, ink, 1.5f);
        gfx_.line(cx - 2, cy, cx + 1.5f, cy + 3.5f, ink, 1.5f);
    } else if (icon == 2) {
        const float cx = x + 4.5f;
        gfx_.line(cx - 3.5f, cy, cx + 3.5f, cy, ink, 1.5f);
        gfx_.line(cx, cy - 3.5f, cx, cy + 3.5f, ink, 1.5f);
    }
    gfx_.text(text, {x + iw, y, tw + 2, h}, strong, ink, {Align::Left, false});
    if (focused(id)) focusRing(box);
    return hov;
}

void AppWindow::segmented(const std::string& id, const Rect& r, const std::vector<std::wstring>& options, int selected,
                          std::function<void(int)> onSelect) {
    const float w = r.w / options.size();
    const float rr = calm_ ? 7.0f : 3.0f;
    gfx_.fillRounded(r, rr, pal_.sunken);
    // The selected pill slides between options.
    const float at = anim(id + "#sel", (float)selected, 20.0f);
    for (size_t i = 0; i < options.size(); ++i) {
        const std::string sid = id + ":" + std::to_string(i);
        Rect cell{r.x + i * w, r.y, w, r.h};
        addHit(sid, cell, [onSelect, i] { onSelect((int)i); });
        const float hv = hoverAmount(sid);
        if ((int)i != selected && hv > 0.01f) gfx_.fillRounded(cell.inset(2), 2, pal_.surface.withAlpha(0.5f * hv));
    }
    if (selected >= 0 && selected < (int)options.size()) {
        const Rect pill{r.x + at * w, r.y, w, r.h};
        gfx_.fillRounded(pill.inset(2), rr - 1, pal_.surface);
        gfx_.strokeRounded(pill.inset(2), rr - 1, pal_.rule);
    }
    for (size_t i = 0; i < options.size(); ++i) {
        const std::string sid = id + ":" + std::to_string(i);
        Rect cell{r.x + i * w, r.y, w, r.h};
        const float closeness = std::max(0.0f, 1.0f - std::abs(at - (float)i));
        gfx_.text(options[i], cell, (int)i == selected ? Font::SmallStrong : Font::Small,
                  pal_.inkMuted.mix(pal_.ink, closeness), {Align::Center, false, true, true});
        if (focused(sid)) focusRing(cell);
    }
}

void AppWindow::tag(float x, float y, const std::wstring& label, Color c, float* outWidth) {
    const float w = gfx_.measureWidth(label, Font::Label) + 10;
    Rect r{x, y, w, 18};
    gfx_.strokeRounded(r, 2, c.withAlpha(0.7f));
    gfx_.text(label, r, Font::Label, c, {Align::Center, false, false, true});
    if (outWidth) *outWidth = w;
}

float AppWindow::sectionLabel(float x, float y, float w, const std::wstring& label) {
    std::wstring up = label;
    CharUpperBuffW(up.data(), (DWORD)up.size());
    gfx_.text(up, {x, y, w, 16}, Font::Label, pal_.inkMuted, {Align::Left, false});
    gfx_.line(x, y + 21.5f, x + w, y + 21.5f, pal_.rule);
    return 32;
}

void AppWindow::slider(const std::string& id, const Rect& r, float value, std::function<void(float)> onChange) {
    value = std::clamp(value, 0.0f, 1.0f);
    addHit(id, r, nullptr);
    sliders_.push_back({id, r, value, onChange});
    const float cy = r.y + r.h / 2;
    gfx_.fillRounded({r.x, cy - 2, r.w, 4}, 2, pal_.rule);
    gfx_.fillRounded({r.x, cy - 2, r.w * value, 4}, 2, pal_.ink);
    const float grow = anim(id + "#grow", (hovered(id) || dragging_ == id) ? 1.0f : 0.0f, 18.0f);
    gfx_.circle(r.x + r.w * value, cy, 7.0f + 1.5f * grow, pal_.ink, true);
    gfx_.circle(r.x + r.w * value, cy, 7.0f + 1.5f * grow, pal_.paper, false, 1.5f);
    if (focused(id)) focusRing(r);
}

void AppWindow::flashUndo(const std::wstring& message, std::function<void()> undo, int seconds) {
    flash(message, seconds);
    flashUndo_ = std::move(undo);
}

void AppWindow::flash(const std::wstring& message, int seconds) {
    flashUndo_ = nullptr;
    flash_ = message;
    flashActive_ = true;
    flashVisible_ = true;
    if (animationsOn_) setAnim("#flash", 0.0f);
    if (hwnd_) SetTimer(hwnd_, kFlashTimer, seconds * 1000, nullptr);
    invalidate();
}

// ------------------------------------------------------------------- frame

void AppWindow::paint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    if (gfx_.begin()) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const float s = gfx_.dpiScale();
        // Frame timing for animations. After a quiet spell, start from one
        // ordinary frame rather than a huge jump.
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        const float dt = lastFrameCounter_ ? float(double(c.QuadPart - lastFrameCounter_) / double(f.QuadPart)) : 1.0f / 60;
        lastFrameCounter_ = c.QuadPart;
        frameDt_ = std::clamp(dt, 1.0f / 240, 1.0f / 30);
        ++frame_;
        animating_ = false;
        hits_.clear();
        sliders_.clear();
        tips_.clear();
        g_ambientTime = animationsOn_ && ambientAllowed_ ? std::fmod(double(c.QuadPart) / double(f.QuadPart), 86400.0) : -1;
        g_ambientWanted = false;
        gfx_.clear(pal_.paper);
        zoomArea_ = {};
        areaZoom_ = 1.0f;
        drawFrame((rc.right - rc.left) / s, (rc.bottom - rc.top) / s);
        endZoomArea();   // an app that didn't close its area
        lastZoomArea_ = zoomArea_;   // the mouse is mapped by this frame's area until the next (0.173)
        lastAreaZoom_ = areaZoom_;
        drawTooltip((rc.right - rc.left) / s, (rc.bottom - rc.top) / s);
        gfx_.end();
        // NORTHRAY_FRAME_LOG=<file>: each frame's time (drawing and presenting) appended, for finding slow pages.
        static const std::wstring frameLog = [] {
            wchar_t buf[MAX_PATH] = {};
            const DWORD n = GetEnvironmentVariableW(L"NORTHRAY_FRAME_LOG", buf, MAX_PATH);
            return n > 0 && n < MAX_PATH ? std::wstring(buf) : std::wstring();
        }();
        if (!frameLog.empty()) {
            LARGE_INTEGER e;
            QueryPerformanceCounter(&e);
            if (FILE* fl = _wfopen(frameLog.c_str(), L"a")) {
                std::fprintf(fl, "%.2f\n", double(e.QuadPart - c.QuadPart) * 1000.0 / double(f.QuadPart));
                std::fclose(fl);
            }
        }
        ambient_ = g_ambientWanted && animationsOn_ && ambientAllowed_;
        g_ambientWanted = false;
        lastHits_ = hits_;
        lastSliders_ = sliders_;
    }
    EndPaint(hwnd_, &ps);
    if (animating_) SetTimer(hwnd_, kAnimTimer, 15, nullptr);
    // The stars' slow twinkle (0.72): five frames a second is enough for a breath of 3-10 s, and none while another
    // window is in front, so an idle window costs next to nothing.
    if (ambient_ && windowActive_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) SetTimer(hwnd_, kAmbientTimer, 200, nullptr);
}

void AppWindow::updateHover() {
    std::string now;
    for (auto it = lastHits_.rbegin(); it != lastHits_.rend(); ++it)
        if (it->rect.contains(mouseX_, mouseY_) && it->clip.contains(mouseX_, mouseY_)) { now = it->id; break; }
    if (now != hover_) {
        hover_ = now;
        hoverSince_ = GetTickCount64();
        tipHidden_ = false;
        if (hwnd_) SetTimer(hwnd_, kTipTimer, (UINT)kTipDelayMs + 20, nullptr);
        invalidate();
    }
}

void AppWindow::tooltip(const std::string& id, std::wstring text) {
    if (!text.empty()) tips_.push_back({id, std::move(text)});
}

void AppWindow::drawTooltip(float W, float H) {
    if (hover_.empty() || tipHidden_ || !dragging_.empty() || GetTickCount64() - hoverSince_ < kTipDelayMs) return;
    if (GetKeyState(VK_LBUTTON) < 0 || GetKeyState(VK_RBUTTON) < 0) return;
    const std::wstring* text = nullptr;
    for (auto& [id, t] : tips_)
        if (id == hover_) text = &t;
    if (!text) return;
    const float maxW = 320, pad = 8;
    const float tw = std::min(maxW, gfx_.measureWidth(*text, Font::Small) + 2);
    const float th = gfx_.text(*text, {-10000, -10000, tw, 400}, Font::Small, pal_.ink);   // measures the wrapped height
    Rect r{rawMouseX_ + 14, rawMouseY_ + 20, tw + pad * 2, th + pad * 2 - 2};
    if (r.right() > W - 6) r.x = std::max(6.0f, W - 6 - r.w);
    if (r.bottom() > H - 6) r.y = std::max(6.0f, rawMouseY_ - 8 - r.h);
    gfx_.fillRounded(r, 6, pal_.dark ? hexColor("#1B2A40") : hexColor("#10213A"));
    gfx_.text(*text, {r.x + pad, r.y + pad - 1, tw, th}, Font::Small, hexColor("#EEF3FA"));
}

void AppWindow::moveFocus(int dir) {
    std::vector<const Hit*> order;
    for (const Hit& h : lastHits_)
        if (h.focusable) order.push_back(&h);
    if (order.empty()) return;
    const std::string prefix = navPrefix();
    std::stable_sort(order.begin(), order.end(), [&](const Hit* a, const Hit* b) {
        const bool an = a->id.rfind(prefix, 0) == 0, bn = b->id.rfind(prefix, 0) == 0;
        if (an != bn) return an;
        if (std::abs(a->rect.y - b->rect.y) > 4) return a->rect.y < b->rect.y;
        return a->rect.x < b->rect.x;
    });
    int idx = -1;
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i]->id == focus_) idx = (int)i;
    idx = idx < 0 ? (dir > 0 ? 0 : (int)order.size() - 1) : (idx + dir + (int)order.size()) % (int)order.size();
    focus_ = order[idx]->id;
    keyboardFocus_ = true;
    const Hit* h = order[idx];
    if (h->clip.w < 1e5f) {  // inside the scroll area: keep it visible
        if (h->rect.y < h->clip.y + 10) scrollBy(h->rect.y - h->clip.y - 30);
        else if (h->rect.bottom() > h->clip.bottom() - 10) scrollBy(h->rect.bottom() - h->clip.bottom() + 30);
    }
    invalidate();
}

std::string AppWindow::hitAt(float x, float y) const {
    for (auto it = lastHits_.rbegin(); it != lastHits_.rend(); ++it)
        if (it->rect.contains(x, y) && it->clip.contains(x, y)) return it->id;
    return {};
}

void AppWindow::activateFocused() {
    for (const Hit& h : lastHits_)
        if (h.id == focus_ && h.action) {
            const std::function<void()> act = h.action;   // see WM_LBUTTONUP: the hits may be rebuilt while it runs
            act();
            invalidate();
            return;
        }
}

// ------------------------------------------------------------ custom frame

bool AppWindow::maximized() const { return hwnd_ && IsZoomed(hwnd_) != FALSE; }

float AppWindow::frameBorder() const {
    const UINT dpi = GetDpiForWindow(hwnd_);
    const int px = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    return (float)px / gfx_.dpiScale();
}

LRESULT AppWindow::frameMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    handled = false;
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) {
                // Keep Windows' sizing borders on the left, right and bottom; take the title bar.
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                const LONG top = p->rgrc[0].top;
                DefWindowProcW(hwnd_, msg, wp, lp);
                // Maximised, the window hangs over the screen edge by its border: keep that much off the top.
                p->rgrc[0].top = maximized() ? top + (LONG)std::lround(frameBorder() * gfx_.dpiScale()) : top;
                handled = true;
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            const LRESULT r = DefWindowProcW(hwnd_, msg, wp, lp);
            handled = true;
            if (r != HTCLIENT) return r;   // the side and bottom borders
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            const float s = gfx_.dpiScale();
            const float x = pt.x / s, y = pt.y / s;
            RECT rc;
            GetClientRect(hwnd_, &rc);
            const float W = rc.right / s;
            if (!maximized() && y < frameBorder()) {
                const float corner = frameBorder() * 2;
                if (x < corner) return HTTOPLEFT;
                if (x > W - corner) return HTTOPRIGHT;
                return HTTOP;
            }
            if (maxButton_.w > 0 && maxButton_.contains(x, y)) return HTMAXBUTTON;
            if (y >= captionHeight_) return HTCLIENT;
            float hx = x, hy = y;
            mapPoint(hx, hy);
            if (!hitAt(hx, hy).empty()) return HTCLIENT;   // a button or tab up there
            return HTCAPTION;                             // empty space: moves the window
        }
        case WM_NCMOUSEMOVE: {
            const bool over = wp == HTMAXBUTTON;
            if (over != maxHover_) { maxHover_ = over; invalidate(); }
            if (over && !hover_.empty()) { hover_.clear(); invalidate(); }
            if (!trackingNc_) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
                trackingNc_ = TrackMouseEvent(&t) != FALSE;
            }
            break;
        }
        case WM_NCMOUSELEAVE:
            trackingNc_ = false;
            if (maxHover_ || maxPressed_) { maxHover_ = maxPressed_ = false; invalidate(); }
            break;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            if (wp == HTMAXBUTTON) {
                maxPressed_ = true;
                invalidate();
                handled = true;   // not Windows' own (it would draw an old-style button)
                return 0;
            }
            break;
        case WM_NCLBUTTONUP:
            if (wp == HTMAXBUTTON) {
                if (maxPressed_) ShowWindow(hwnd_, maximized() ? SW_RESTORE : SW_MAXIMIZE);
                maxPressed_ = false;
                invalidate();
                handled = true;
                return 0;
            }
            maxPressed_ = false;
            break;
        case WM_ACTIVATE:
            windowActive_ = LOWORD(wp) != WA_INACTIVE;
            invalidate();
            break;
        case WM_SIZE:
            invalidate();   // the maximise glyph changes
            break;
    }
    return 0;
}

float AppWindow::drawCaptionButtons(const Rect& strip, bool onNight) {
    const float h = strip.h <= 64 ? strip.h : 40;
    const float top = strip.y;
    const Color base = onNight ? Color{0.93f, 0.95f, 1.0f, 1} : pal_.ink;
    const Color glyph = base.withAlpha(windowActive_ ? 0.92f : 0.5f);
    const float x0 = strip.right() - kCaptionButtonW * 3;
    auto box = [&](int i) { return Rect{x0 + kCaptionButtonW * i, top, kCaptionButtonW, h}; };
    const float px = 1.0f / gfx_.dpiScale();   // one physical pixel
    auto snap = [&](float v) { return std::floor(v * gfx_.dpiScale()) / gfx_.dpiScale() + px / 2; };

    // Minimise
    const Rect mn = box(0);
    addHit("cap:min", mn, [this] { ShowWindow(hwnd_, SW_MINIMIZE); }, false);
    if (float hv = hoverAmount("cap:min"); hv > 0.01f) gfx_.fill(mn, base.withAlpha((pressed("cap:min") ? 0.16f : 0.09f) * hv));
    {
        const float cx = mn.x + mn.w / 2, cy = snap(mn.y + mn.h / 2);
        gfx_.line(cx - 5, cy, cx + 5, cy, glyph, 1.0f);
    }
    // Maximise / restore: hit-tested by Windows (HTMAXBUTTON), so Windows 11 can show snap layouts.
    maxButton_ = box(1);
    {
        const Rect& mx = maxButton_;
        const float hv = anim("cap:max#hover", maxHover_ ? 1.0f : 0.0f, 18.0f);
        if (hv > 0.01f) gfx_.fill(mx, base.withAlpha((maxPressed_ ? 0.16f : 0.09f) * hv));
        const float cx = snap(mx.x + mx.w / 2), cy = snap(mx.y + mx.h / 2);
        if (maximized()) {
            gfx_.strokeRounded({cx - 5, cy - 3, 8, 8}, 1, glyph, 1.0f);
            gfx_.line(cx - 3, cy - 5, cx + 4, cy - 5, glyph, 1.0f);
            gfx_.line(cx + 5, cy - 4, cx + 5, cy + 3, glyph, 1.0f);
        } else {
            gfx_.strokeRounded({cx - 5, cy - 5, 10, 10}, 1, glyph, 1.0f);
        }
    }
    // Close: Windows' red on hover.
    const Rect cl = box(2);
    addHit("cap:close", cl, [this] { PostMessageW(hwnd_, WM_CLOSE, 0, 0); }, false);
    {
        const float hv = hoverAmount("cap:close");
        if (hv > 0.01f) gfx_.fill(cl, Color{0.769f, 0.169f, 0.110f, 1}.withAlpha(hv * (pressed("cap:close") ? 0.85f : 1.0f)));
        const float cx = cl.x + cl.w / 2, cy = cl.y + cl.h / 2;
        const Color g = glyph.mix(Color{1, 1, 1, 1}, hv);
        gfx_.line(cx - 5, cy - 5, cx + 5, cy + 5, g, 1.1f);
        gfx_.line(cx + 5, cy - 5, cx - 5, cy + 5, g, 1.1f);
    }
    return kCaptionButtonW * 3;
}

LRESULT AppWindow::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (spec_.customFrame && hwnd_) {
        bool done = false;
        const LRESULT r = frameMessage(msg, wp, lp, done);
        if (done) return r;
    }
    switch (msg) {
        case WM_PAINT: paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            gfx_.resize(LOWORD(lp), HIWORD(lp));
            invalidate();
            return 0;
        case WM_DPICHANGED: {
            gfx_.setDpi(whole_ ? HIWORD(wp) * zoom_ : (float)HIWORD(wp));
            const RECT* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            invalidate();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            UINT dpi = GetDpiForWindow(hwnd);
            if (!dpi) dpi = 96;
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            // The smallest size grows with the interface zoom, but never past the screen it's on.
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            const int wMax = mi.rcWork.right - mi.rcWork.left, hMax = mi.rcWork.bottom - mi.rcWork.top;
            mmi->ptMinTrackSize = {std::min<int>((int)std::lround(spec_.minWidth * dpi / 96.0f * zoom_), wMax > 0 ? wMax : 1 << 20),
                                   std::min<int>((int)std::lround(spec_.minHeight * dpi / 96.0f * zoom_), hMax > 0 ? hMax : 1 << 20)};
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && lstrcmpW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) onSystemThemeChanged();
            readAnimationSetting();
            invalidate();
            return 0;
        case WM_TIMER:
            if (wp == kTipTimer) {
                KillTimer(hwnd, kTipTimer);
                if (!hover_.empty()) invalidate();
                return 0;
            }
            if (wp == kAnimTimer) {
                KillTimer(hwnd, kAnimTimer);
                invalidate();  // the next paint re-arms the timer if still moving
                return 0;
            }
            if (wp == kAmbientTimer) {
                KillTimer(hwnd, kAmbientTimer);
                if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) invalidate();  // re-armed by the paint if still wanted
                return 0;
            }
            if (wp == kFlashTimer) {
                KillTimer(hwnd, kFlashTimer);
                flashActive_ = false;
                invalidate();
                return 0;
            }
            onTimer(wp);
            return 0;
        case WM_MOUSEMOVE: {
            const float s = gfx_.dpiScale();
            rawMouseX_ = mouseX_ = GET_X_LPARAM(lp) / s;
            rawMouseY_ = mouseY_ = GET_Y_LPARAM(lp) / s;
            mapPoint(mouseX_, mouseY_);   // into the zoom area's space when over it (0.173)
            if (!trackingMouse_) {
                TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                trackingMouse_ = true;
            }
            onMouseMove(mouseX_, mouseY_, (wp & MK_LBUTTON) != 0);
            if (!dragging_.empty()) {
                for (auto& sl : lastSliders_)
                    if (sl.id == dragging_ && sl.onChange)
                        sl.onChange(std::clamp((mouseX_ - sl.rect.x) / sl.rect.w, 0.0f, 1.0f));
                invalidate();
                return 0;
            }
            updateHover();
            return 0;
        }
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            if (!hover_.empty() && dragging_.empty()) { hover_.clear(); invalidate(); }
            return 0;
        case WM_LBUTTONDOWN:
            if (!tipHidden_) { tipHidden_ = true; invalidate(); }
            SetFocus(hwnd);  // take keyboard focus back from any child control
            SetCapture(hwnd);
            if (onMouseDown(mouseX_, mouseY_)) { invalidate(); return 0; }
            pressed_ = hover_;
            keyboardFocus_ = false;
            if (!pressed_.empty()) focus_ = pressed_;
            for (auto& sl : lastSliders_)
                if (sl.id == hover_) {
                    dragging_ = sl.id;
                    if (sl.onChange) sl.onChange(std::clamp((mouseX_ - sl.rect.x) / sl.rect.w, 0.0f, 1.0f));
                }
            invalidate();
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            if (onMouseUp(mouseX_, mouseY_)) { pressed_.clear(); invalidate(); return 0; }
            const std::string target = pressed_;
            pressed_.clear();
            if (!dragging_.empty()) { dragging_.clear(); invalidate(); return 0; }
            if (!target.empty() && target == hover_)
                for (const Hit& h : lastHits_)
                    if (h.id == target && h.action) {
                        // A copy: the action may open a menu or dialog whose messages repaint the
                        // window, which rebuilds the hits (and would free this one mid-call).
                        const std::function<void()> act = h.action;
                        act();
                        break;
                    }
            invalidate();
            return 0;
        }
        case WM_RBUTTONUP: {
            const float s = gfx_.dpiScale();
            float rx = GET_X_LPARAM(lp) / s, ry = GET_Y_LPARAM(lp) / s;
            mapPoint(rx, ry);
            if (onRightClick(rx, ry)) { invalidate(); return 0; }
            break;
        }
        case WM_MOUSEWHEEL: {
            // Ctrl and the wheel: the interface's size, unless what's under the pointer has its own zoom (0.172).
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                const float s = gfx_.dpiScale();
                float zx = pt.x / s, zy = pt.y / s;
                mapPoint(zx, zy);
                if (!ownZoomAt(zx, zy)) {
                    const short d = GET_WHEEL_DELTA_WPARAM(wp);
                    if (d) zoomStep(d > 0 ? 1 : -1);
                    return 0;
                }
            }
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                const float s = gfx_.dpiScale();
                float wx = pt.x / s, wy = pt.y / s;
                mapPoint(wx, wy);
                if (onWheel(wx, wy, -GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * (lines == WHEEL_PAGESCROLL ? 20 : lines))) {
                    invalidate();
                    return 0;
                }
            }
            scrollBy(-GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 20.0f * (lines == WHEEL_PAGESCROLL ? 20 : lines));
            return 0;
        }
        case WM_KEYDOWN: {
            const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
            const bool shift = GetKeyState(VK_SHIFT) & 0x8000;
            // A focused slider takes Left/Right in 5% steps.
            if ((wp == VK_LEFT || wp == VK_RIGHT) && keyboardFocus_) {
                for (auto& sl : lastSliders_)
                    if (sl.id == focus_ && sl.onChange) {
                        sl.onChange(std::clamp(sl.value + (wp == VK_LEFT ? -0.05f : 0.05f), 0.0f, 1.0f));
                        invalidate();
                        return 0;
                    }
            }
            // Interface zoom (0.172): Ctrl+Alt with + / - / 0 always; Ctrl alone when the app hasn't used the key itself.
            const bool zoomKey = wp == VK_OEM_PLUS || wp == VK_ADD || wp == VK_OEM_MINUS || wp == VK_SUBTRACT || wp == '0' || wp == VK_NUMPAD0;
            const int zoomDir = wp == VK_OEM_PLUS || wp == VK_ADD ? 1 : (wp == VK_OEM_MINUS || wp == VK_SUBTRACT) ? -1 : 0;
            const bool alt = GetKeyState(VK_MENU) & 0x8000;
            if (zoomKey && ctrl && alt) { zoomStep(zoomDir); return 0; }
            if (onKeyDown(wp, ctrl, shift)) { invalidate(); return 0; }
            if (zoomKey && ctrl && !shift && !ctrlZoomKeysAreApps()) { zoomStep(zoomDir); return 0; }
            switch (wp) {
                case VK_TAB: moveFocus(shift ? -1 : 1); return 0;
                case VK_RETURN: case VK_SPACE: activateFocused(); return 0;
                case VK_ESCAPE: clearFocus(); invalidate(); return 0;
                case VK_DOWN: scrollBy(40); return 0;
                case VK_UP: scrollBy(-40); return 0;
                case VK_NEXT: scrollBy(viewportHeight_ * 0.85f); return 0;
                case VK_PRIOR: scrollBy(-viewportHeight_ * 0.85f); return 0;
                case VK_HOME: scrollBy(-1e6f); return 0;
                case VK_END: scrollBy(1e6f); return 0;
            }
            break;
        }
        case WM_DESTROY:
            onDestroy();
            gfx_.shutdown();
            if (spec_.quitOnDestroy) PostQuitMessage(0);
            return 0;
        default: {
            bool handled = false;
            LRESULT r = onMessage(msg, wp, lp, handled);
            if (handled) return r;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace cjui

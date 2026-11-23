#include "cjui/window.hpp"
#include "cjui/icons.hpp"
#include "cjui/menu.hpp"

#include <dwmapi.h>
#include <windowsx.h>

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
    gfx_.setDpi(GetDpiForWindow(hwnd_));
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
    if (amount <= 0.01f) return;
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
    else if (!text.empty() && text[0] == L'\u2039') text.erase(0, text.size() > 1 && text[1] == L' ' ? 2 : 1);
    float isz = ic == Icon::None ? 0 : std::min(15.0f, b.h - 8);
    if (isz && gfx_.measureWidth(text, f) + isz + 6 + 10 > b.w) isz = 0;   // no room: the words come first
    const float tw = std::min(gfx_.measureWidth(text, f), b.w - (isz ? isz + 16 : 14));
    const float total = (isz ? isz + 6 : 0) + tw;
    const float x0 = b.x + std::max(5.0f, (b.w - total) / 2);
    if (isz) drawIcon(gfx_, ic, {x0, b.y + (b.h - isz) / 2, isz, isz}, enabled ? pal_.inkMuted.mix(pal_.ink, h) : pal_.inkFaint, 1.4f);
    gfx_.text(text, {x0 + (isz ? isz + 6 : 0), b.y, tw + 2, b.h}, f, ink, {Align::Left, false, true, true});
    if (enabled && focused(id)) focusRing(b);
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

bool AppWindow::linkText(const std::string& id, float x, float y, const std::wstring& label,
                         std::function<void()> action, Font f) {
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
        drawFrame((rc.right - rc.left) / s, (rc.bottom - rc.top) / s);
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
    if (ambient_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) SetTimer(hwnd_, kAmbientTimer, 83, nullptr);
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
    Rect r{mouseX_ + 14, mouseY_ + 20, tw + pad * 2, th + pad * 2 - 2};
    if (r.right() > W - 6) r.x = std::max(6.0f, W - 6 - r.w);
    if (r.bottom() > H - 6) r.y = std::max(6.0f, mouseY_ - 8 - r.h);
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
            if (!hitAt(x, y).empty()) return HTCLIENT;   // a button or tab up there
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
            gfx_.setDpi(HIWORD(wp));
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
            mmi->ptMinTrackSize = {MulDiv(spec_.minWidth, dpi, 96), MulDiv(spec_.minHeight, dpi, 96)};
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
            mouseX_ = GET_X_LPARAM(lp) / s;
            mouseY_ = GET_Y_LPARAM(lp) / s;
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
            if (onRightClick(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s)) { invalidate(); return 0; }
            break;
        }
        case WM_MOUSEWHEEL: {
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                const float s = gfx_.dpiScale();
                if (onWheel(pt.x / s, pt.y / s, -GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * (lines == WHEEL_PAGESCROLL ? 20 : lines))) {
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
            if (onKeyDown(wp, ctrl, shift)) { invalidate(); return 0; }
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

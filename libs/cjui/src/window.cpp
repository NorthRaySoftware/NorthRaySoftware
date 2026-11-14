#include "cjui/window.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace cjui {

static const UINT_PTR kFlashTimer = 0xC001;
static const UINT_PTR kAnimTimer = 0xC002;

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
    // The radio console stays dark in both themes; in dark mode it's lifted and
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

void AppWindow::applyPalette(const Palette& p) {
    pal_ = p;
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
    return clip.y - scroll_ + (1.0f - p) * 14.0f;
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

void AppWindow::focusRing(const Rect& r) { gfx_.strokeRounded(r.inset(-3), 4, pal_.focus, 2); }

bool AppWindow::button(const std::string& id, const Rect& r, const std::wstring& label, std::function<void()> action,
                       bool primary) {
    addHit(id, r, std::move(action));
    const bool hov = hovered(id), down = pressed(id);
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
    gfx_.fillRounded(r, 3, pal_.sunken);
    gfx_.text(label, r, Font::Body, pal_.inkFaint, {Align::Center, false, true, true});
}

bool AppWindow::linkText(const std::string& id, float x, float y, const std::wstring& label,
                         std::function<void()> action, Font f) {
    const float w = gfx_.measureWidth(label, f);
    const float h = gfx_.measureHeight(label, w + 2, f);
    Rect r{x, y, w, h};
    addHit(id, r, std::move(action));
    const bool hov = hovered(id);
    const float a = hoverAmount(id);
    gfx_.text(label, {x, y, w + 2, h}, f, pal_.ribbon, {Align::Left, false});
    gfx_.line(x, y + h - 1.5f, x + w, y + h - 1.5f, pal_.ribbon.withAlpha(0.35f + 0.65f * a), 1);
    if (focused(id)) focusRing(r);
    return hov;
}

void AppWindow::segmented(const std::string& id, const Rect& r, const std::vector<std::wstring>& options, int selected,
                          std::function<void(int)> onSelect) {
    const float w = r.w / options.size();
    gfx_.fillRounded(r, 3, pal_.sunken);
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
        gfx_.fillRounded(pill.inset(2), 2, pal_.surface);
        gfx_.strokeRounded(pill.inset(2), 2, pal_.rule);
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

void AppWindow::flash(const std::wstring& message, int seconds) {
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
        gfx_.clear(pal_.paper);
        drawFrame((rc.right - rc.left) / s, (rc.bottom - rc.top) / s);
        gfx_.end();
        lastHits_ = hits_;
        lastSliders_ = sliders_;
    }
    EndPaint(hwnd_, &ps);
    if (animating_) SetTimer(hwnd_, kAnimTimer, 15, nullptr);
}

void AppWindow::updateHover() {
    std::string now;
    for (auto it = lastHits_.rbegin(); it != lastHits_.rend(); ++it)
        if (it->rect.contains(mouseX_, mouseY_) && it->clip.contains(mouseX_, mouseY_)) { now = it->id; break; }
    if (now != hover_) { hover_ = now; invalidate(); }
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
        if (h.id == focus_ && h.action) { h.action(); invalidate(); return; }
}

LRESULT AppWindow::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
            if (wp == kAnimTimer) {
                KillTimer(hwnd, kAnimTimer);
                invalidate();  // the next paint re-arms the timer if still moving
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
                    if (h.id == target && h.action) { h.action(); break; }
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

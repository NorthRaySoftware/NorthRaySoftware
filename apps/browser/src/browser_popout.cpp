// The video pop-out: a tab's page moved into a small window that stays on top of every other app,
// with the page's video (its player, adverts and controls included) filling it and nothing else of
// the page showing. The page itself moves, so nothing is loaded twice and the video carries on where
// it was: Back to the tab (or closing the pop-out) moves it back. Only the video shows: a bar with the
// title, back and close appears over its top while the mouse is on the pop-out (drag it to move the
// pop-out), and the thin edges resize it.
#include "browser_window.hpp"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace nrb {

using cjui::Rect;

namespace {
constexpr int kBar = 30;      // the bar over the top, in pixels at 96 dpi
constexpr int kEdge = 3;      // the border round the page (resizing)
constexpr wchar_t kPopClass[] = L"NorthRayBrowserPopout";
constexpr wchar_t kBarClass[] = L"NorthRayBrowserPopoutBar";
constexpr UINT_PTR kHoverTimer = 7;

int px(HWND h, int v) { return MulDiv(v, (int)GetDpiForWindow(h), 96); }

// Where the two buttons are, in client pixels: 1 back to the tab, 2 close.
RECT buttonRect(HWND h, int which) {
    RECT rc;
    GetClientRect(h, &rc);
    const int b = px(h, kBar);
    const int right = rc.right - (which == 2 ? 0 : b + px(h, 2));
    return {right - b, 0, right, b};
}
}  // namespace

LRESULT CALLBACK popoutProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<BrowserWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, msg, wp, lp);
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    return self->popoutMessage(h, msg, wp, lp);
}

void BrowserWindow::popOutVideo(int id) {
    if (popTab_ == id && pop_) { popBackIn(); return; }
    if (popTab_) popBackIn();   // one pop-out at a time
    TabView* v = view(id);
    if (!v || !v->page || !v->page->ready() || v->showStart || v->error) {
        flash(L"Open a page with a video first.", 5);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = popoutProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kPopClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = (HICON)SendMessageW(hwnd(), WM_GETICON, ICON_BIG, 0);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassW(&wc);
        wc.lpszClassName = kBarClass;
        wc.style = CS_DBLCLKS;
        RegisterClassW(&wc);
        registered = true;
    }
    // Where it was last, or the bottom-right corner of the screen the browser is on.
    HMONITOR mon = MonitorFromWindow(hwnd(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    const UINT dpi = GetDpiForWindow(hwnd()) ? GetDpiForWindow(hwnd()) : 96;
    RECT r{};
    if (settings_.popW > 100 && settings_.popH > 80) {
        r = {settings_.popX, settings_.popY, settings_.popX + settings_.popW, settings_.popY + settings_.popH};
        RECT seen{};
        if (!IntersectRect(&seen, &r, &mi.rcWork) || (seen.right - seen.left) < 80 || (seen.bottom - seen.top) < 60) r = {};
    }
    if (r.right == 0) {
        const int w = MulDiv(480 + kEdge * 2, (int)dpi, 96), hgt = MulDiv(270 + kEdge * 2, (int)dpi, 96), m = MulDiv(24, (int)dpi, 96);
        r = {mi.rcWork.right - w - m, mi.rcWork.bottom - hgt - m, mi.rcWork.right - m, mi.rcWork.bottom - m};
    }
    // Not owned by the browser window, so it stays when the browser is minimised.
    pop_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPopClass, L"Video", WS_POPUP | WS_CLIPCHILDREN, r.left, r.top,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!pop_) { flash(L"The pop-out couldn't be opened.", 6); return; }
    // The bar: its own window, so it can sit over the page (nothing can be drawn on the page itself).
    popBar_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kBarClass, L"", WS_POPUP, 0, 0, 10, 10, pop_, nullptr,
                              GetModuleHandleW(nullptr), this);
    if (popBar_) SetLayeredWindowAttributes(popBar_, 0, 225, LWA_ALPHA);
    SetTimer(pop_, kHoverTimer, 120, nullptr);
    popTab_ = id;
    v->page->reparent(pop_);
    v->page->show(true);
    placePopoutPage();
    ShowWindow(pop_, SW_SHOWNOACTIVATE);
    const int tabId = id;
    v->page->setVideoPopout(true, [this, tabId](bool hasVideo) {
        if (popTab_ != tabId) return;
        if (!hasVideo) flash(L"No video on this page yet. When one plays, it fills the pop-out.", 6);
    });
    invalidate();
}

void BrowserWindow::popBackIn() {
    if (!popTab_) return;
    const int id = popTab_;
    popTab_ = 0;
    if (pop_) {
        RECT r;
        if (GetWindowRect(pop_, &r)) {
            settings_.popX = r.left;
            settings_.popY = r.top;
            settings_.popW = r.right - r.left;
            settings_.popH = r.bottom - r.top;
            settings_.save(settingsPath_);
        }
    }
    if (TabView* v = view(id); v && v->page) {
        v->page->setVideoPopout(false);
        v->page->reparent(hwnd());
    }
    if (pop_) {
        HWND p = pop_;
        pop_ = nullptr;
        popBar_ = nullptr;   // owned by the pop-out: goes with it
        KillTimer(p, kHoverTimer);
        DestroyWindow(p);
    }
    popHover_ = popDown_ = 0;
    invalidate();   // the page goes back in its place on the next frame
}

void BrowserWindow::placePopoutPage() {
    if (!pop_) return;
    TabView* v = view(popTab_);
    if (!v || !v->page) return;
    RECT rc;
    GetClientRect(pop_, &rc);
    const int e = px(pop_, kEdge);
    v->page->setBounds({rc.left + e, rc.top + e, rc.right - e, rc.bottom - e});
    v->page->parentMoved();
    placePopoutBar();
}

// The bar over the top of the video, while the mouse is on the pop-out (or the bar is in use).
void BrowserWindow::placePopoutBar() {
    if (!pop_ || !popBar_) return;
    RECT wr;
    GetWindowRect(pop_, &wr);
    POINT c;
    GetCursorPos(&c);
    const bool over = PtInRect(&wr, c) || popDown_ != 0;
    const bool minimised = IsIconic(pop_);
    if (!over || minimised) {
        if (IsWindowVisible(popBar_)) ShowWindow(popBar_, SW_HIDE);
        if (popHover_) popHover_ = 0;
        return;
    }
    const int e = px(pop_, kEdge);
    SetWindowPos(popBar_, HWND_TOPMOST, wr.left + e, wr.top + e, (wr.right - wr.left) - 2 * e, px(pop_, kBar),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT BrowserWindow::popoutMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (GetWindow(h, GW_OWNER) != nullptr) {   // only the bar has an owner (the pop-out)
        // ---- The bar.
        switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            if (!popTracking_) {
                TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                popTracking_ = true;
            }
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hv = 0;
            for (int i = 1; i <= 2; ++i) { const RECT br = buttonRect(h, i); if (PtInRect(&br, p)) hv = i; }
            if (hv != popHover_) { popHover_ = hv; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_MOUSELEAVE:
            popTracking_ = false;
            if (popHover_) { popHover_ = 0; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (!popHover_) { popBackIn(); return 0; }   // double-click the bar: back to the tab
            return 0;
        case WM_LBUTTONDOWN:
            if (!popHover_) {
                // Dragging the bar moves the pop-out.
                ReleaseCapture();
                SendMessageW(pop_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            popDown_ = popHover_;
            SetCapture(h);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const int was = popDown_;
            popDown_ = 0;
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!was) return 0;
            const RECT br = buttonRect(h, was);
            if (!PtInRect(&br, p)) { InvalidateRect(h, nullptr, FALSE); return 0; }
            if (was == 1) {
                const int id = popTab_;
                popBackIn();
                activateTab(id);
                if (IsIconic(hwnd())) ShowWindow(hwnd(), SW_RESTORE);
                SetForegroundWindow(hwnd());
            } else {
                popBackIn();
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HBRUSH frame = CreateSolidBrush(RGB(0x10, 0x14, 0x22));   // the night of the tab strip
            FillRect(dc, &rc, frame);
            DeleteObject(frame);
            std::wstring title = L"Video";
            if (TabView* v = view(popTab_); v && v->page && !v->page->title().empty()) title = v->page->title();
            HFONT f = CreateFontW(-px(h, 13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HGDIOBJ old = SelectObject(dc, f);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0xD8, 0xDE, 0xEA));
            RECT tr{px(h, 10), 0, buttonRect(h, 1).left - px(h, 6), rc.bottom};
            DrawTextW(dc, title.c_str(), (int)title.size(), &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, old);
            DeleteObject(f);
            // The buttons: back to the tab (a window with an arrow into it), close (a cross).
            for (int i = 1; i <= 2; ++i) {
                const RECT br = buttonRect(h, i);
                if (popHover_ == i) {
                    HBRUSH hb = CreateSolidBrush(i == 2 ? RGB(0xC4, 0x2B, 0x1C) : RGB(0x2A, 0x31, 0x45));
                    FillRect(dc, &br, hb);
                    DeleteObject(hb);
                }
                HPEN pen = CreatePen(PS_SOLID, std::max(1, px(h, 1)), RGB(0xE8, 0xEC, 0xF4));
                HGDIOBJ op = SelectObject(dc, pen);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                const int cx = (br.left + br.right) / 2, cy = (br.top + br.bottom) / 2, s = px(h, 5);
                if (i == 2) {
                    MoveToEx(dc, cx - s, cy - s, nullptr); LineTo(dc, cx + s + 1, cy + s + 1);
                    MoveToEx(dc, cx + s, cy - s, nullptr); LineTo(dc, cx - s - 1, cy + s + 1);
                } else {
                    Rectangle(dc, cx - s - 1, cy - s, cx + s + 2, cy + s + 1);
                    MoveToEx(dc, cx + s + 3, cy - s - 3, nullptr); LineTo(dc, cx, cy);
                    MoveToEx(dc, cx, cy, nullptr); LineTo(dc, cx + px(h, 4), cy);
                    MoveToEx(dc, cx, cy, nullptr); LineTo(dc, cx, cy - px(h, 4));
                }
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(pen);
            }
            EndPaint(h, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(h, msg, wp, lp);
        }
    }
    // ---- The pop-out itself: a thin black frame round the page, for resizing.
    switch (msg) {
    case WM_TIMER:
        if (wp == kHoverTimer) { placePopoutBar(); return 0; }
        break;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        RECT rc;
        GetClientRect(h, &rc);
        const int e = px(h, kEdge) + 2;
        const bool l = p.x < e, r = p.x >= rc.right - e, t = p.y < e, b = p.y >= rc.bottom - e;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (b && l) return HTBOTTOMLEFT;
        if (b && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (b) return HTBOTTOM;
        if (t) return HTTOP;
        return HTCAPTION;   // the frame drags it too
    }
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {px(h, 220), px(h, 150)};
        return 0;
    }
    case WM_SIZE:
        placePopoutPage();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOVE:
    case WM_MOVING:
    case WM_EXITSIZEMOVE:
        placePopoutPage();
        return msg == WM_MOVING ? TRUE : 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_NCLBUTTONDBLCLK:
        if (wp == HTCAPTION) { popBackIn(); return 0; }
        break;
    case WM_CLOSE:
        popBackIn();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(h, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// The tab's place in the browser while its page is in the pop-out.
void BrowserWindow::drawPopoutPlaceholder(const Rect& r) {
    gfx_.fill(r, pal_.paper);
    const float cw = std::min(r.w - 48, 460.0f);
    const float x = r.x + (r.w - cw) / 2, y = r.y + r.h * 0.32f;
    gfx_.text(L"Playing in the pop-out", {x, y, cw, 34}, cjui::Font::Title, pal_.ink, {cjui::Align::Center, false});
    gfx_.text(L"The video stays on top of your other apps. Drag its bar to move it and its edges to resize it.",
              {x, y + 44, cw, 60}, cjui::Font::Body, pal_.inkMuted, {cjui::Align::Center, false});
    button("pop:back", {x + (cw - 200) / 2, y + 112, 200, 36}, L"Back to the tab", [this] { popBackIn(); }, true);
}

}  // namespace nrb

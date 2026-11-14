// A message box in the app's own look: the palette, the aurora band, rounded corners, NorthRay buttons.
// Called like Windows' MessageBoxW (MB_OK / MB_OKCANCEL / MB_YESNO / MB_YESNOCANCEL, MB_ICON*, MB_DEFBUTTON2/3)
// and answers the same way (IDOK, IDCANCEL, IDYES, IDNO), so it replaces it one for one.
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"
#include "cjui/gfx.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace cjui {

namespace {

const wchar_t* kDialogClass = L"CJUIDialog";

struct Btn { std::wstring label; int result; Rect r; };

struct Dialog {
    HWND hwnd = nullptr, owner = nullptr;
    Gfx gfx;
    Palette pal;
    std::wstring title, text;
    int icon = 0;                 // 0 none, 1 info, 2 question, 3 warning, 4 error
    std::vector<Btn> buttons;
    int def = 0, hover = -1, pressed = -1;
    int result = 0;
    bool done = false;
    float scale = 1, w = 460, h = 200, textH = 0;
    int cancelResult = IDCANCEL;

    void layout() {
        const float pad = 24;
        const float tx = icon ? pad + 44 : pad;
        textH = gfx.measureHeight(text, w - tx - pad, Font::Body);
        const float titleH = title.empty() ? 0 : 30;
        h = pad + 6 + titleH + std::max(textH, icon ? 36.0f : 0.0f) + 24 + 38 + pad;
        float bx = w - pad;
        for (int i = (int)buttons.size() - 1; i >= 0; --i) {
            const float bw = std::max(92.0f, gfx.measureWidth(buttons[(size_t)i].label, Font::BodyStrong) + 36);
            bx -= bw;
            buttons[(size_t)i].r = {bx, h - pad - 38, bw, 38};
            bx -= 10;
        }
    }

    void paint() {
        if (!gfx.begin()) return;
        gfx.clear(pal.surface);
        // The aurora band along the top edge, as on every NorthRay surface.
        const Color stops[] = {hexColor("#7CF5B0"), hexColor("#2FC6A0"), hexColor("#3A8FD8"), hexColor("#9B7BE0")};
        for (int i = 0; i < 48; ++i) {
            const float t = i / 47.0f * 3;
            const int k = std::min(2, (int)t);
            gfx.fill({w * i / 48, 0, w / 48 + 1, 4}, stops[k].mix(stops[k + 1], t - k).withAlpha(pal.dark ? 0.85f : 0.95f));
        }
        gfx.strokeRounded({0, 0, w, h}, 10, pal.rule.mix(pal.inkFaint, 0.2f), 1);
        const float pad = 24;
        float y = pad + 6;
        float tx = pad;
        if (icon) {
            const Color c = icon == 3 ? hexColor(pal.dark ? "#F2B84B" : "#B7791F") : icon == 4 ? hexColor(pal.dark ? "#F97066" : "#C4382B")
                                                                                  : pal.ribbon;
            const float cx = pad + 16, cy = y + 16 + (title.empty() ? 0 : 2);
            gfx.circle(cx, cy, 16, c.withAlpha(pal.dark ? 0.22f : 0.14f), true);
            gfx.circle(cx, cy, 16, c, false, 1.6f);
            const wchar_t* glyph = icon == 1 ? L"i" : icon == 2 ? L"?" : L"!";
            gfx.text(glyph, {cx - 16, cy - 16, 32, 32}, Font::Heading, c, {Align::Center, false, false, true});
            tx = pad + 44;
        }
        if (!title.empty()) {
            gfx.text(title, {tx, y, w - tx - pad, 26}, Font::Heading, pal.ink, {Align::Left, false, true});
            y += 30;
        }
        gfx.text(text, {tx, y, w - tx - pad, textH + 4}, Font::Body, pal.inkMuted.mix(pal.ink, 0.45f));
        for (size_t i = 0; i < buttons.size(); ++i) {
            const Btn& b = buttons[i];
            const bool primary = (int)i == def, lit = (int)i == hover, down = (int)i == pressed && lit;
            if (primary) {
                gfx.fillRounded(b.r, 8, pal.ribbon.mix(pal.ink, down ? 0.18f : lit ? 0.08f : 0.0f));
                gfx.text(b.label, b.r, Font::BodyStrong, pal.dark ? hexColor("#06221A") : hexColor("#FFFFFF"), {Align::Center, false, false, true});
            } else {
                gfx.fillRounded(b.r, 8, pal.ink.withAlpha((pal.dark ? 0.06f : 0.04f) + (down ? 0.10f : lit ? 0.06f : 0.0f)));
                gfx.strokeRounded(b.r, 8, pal.rule.mix(pal.inkFaint, lit ? 0.6f : 0.2f), 1);
                gfx.text(b.label, b.r, Font::Body, pal.ink, {Align::Center, false, false, true});
            }
        }
        gfx.end();
    }

    int buttonAt(float x, float y) const {
        for (size_t i = 0; i < buttons.size(); ++i) if (buttons[i].r.contains(x, y)) return (int)i;
        return -1;
    }
    void finish(int r) { result = r; done = true; }
};

LRESULT CALLBACK dialogProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<Dialog*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
    if (!d) return DefWindowProcW(hw, msg, wp, lp);
    const float x = GET_X_LPARAM(lp) / d->scale, y = GET_Y_LPARAM(lp) / d->scale;
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hw, &ps);
            d->paint();
            EndPaint(hw, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int h = d->buttonAt(x, y);
            if (h != d->hover) { d->hover = h; InvalidateRect(hw, nullptr, FALSE); }
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hw, 0};
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE: d->hover = -1; InvalidateRect(hw, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: d->pressed = d->buttonAt(x, y); SetCapture(hw); InvalidateRect(hw, nullptr, FALSE); return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const int b = d->buttonAt(x, y);
            if (b >= 0 && b == d->pressed) d->finish(d->buttons[(size_t)b].result);
            d->pressed = -1;
            InvalidateRect(hw, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) d->finish(d->cancelResult);
            else if (wp == VK_RETURN || wp == VK_SPACE) d->finish(d->buttons[(size_t)d->def].result);
            else if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_TAB) {
                const int n = (int)d->buttons.size();
                const bool back = wp == VK_LEFT || (wp == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000));
                d->def = (d->def + (back ? n - 1 : 1)) % n;
                InvalidateRect(hw, nullptr, FALSE);
            } else if (wp == 'Y' || wp == 'N' || wp == 'O') {
                for (auto& b : d->buttons)
                    if ((wp == 'Y' && b.result == IDYES) || (wp == 'N' && b.result == IDNO) || (wp == 'O' && b.result == IDOK)) d->finish(b.result);
            }
            return 0;
        case WM_CLOSE: d->finish(d->cancelResult); return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

}  // namespace

int messageBox(HWND owner, const std::wstring& text, const std::wstring& title, UINT flags) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = dialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kDialogClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    Dialog d;
    d.owner = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    d.pal = menuPalette();
    d.title = title;
    d.text = text;
    const UINT icon = flags & MB_ICONMASK;
    d.icon = icon == MB_ICONINFORMATION ? 1 : icon == MB_ICONQUESTION ? 2 : icon == MB_ICONWARNING ? 3 : icon == MB_ICONERROR ? 4 : 0;
    switch (flags & MB_TYPEMASK) {
        case MB_OKCANCEL: d.buttons = {{L"OK", IDOK, {}}, {L"Cancel", IDCANCEL, {}}}; break;
        case MB_YESNO: d.buttons = {{L"Yes", IDYES, {}}, {L"No", IDNO, {}}}; d.cancelResult = IDNO; break;
        case MB_YESNOCANCEL: d.buttons = {{L"Yes", IDYES, {}}, {L"No", IDNO, {}}, {L"Cancel", IDCANCEL, {}}}; break;
        case MB_RETRYCANCEL: d.buttons = {{L"Try again", IDRETRY, {}}, {L"Cancel", IDCANCEL, {}}}; break;
        default: d.buttons = {{L"OK", IDOK, {}}}; d.cancelResult = IDOK; break;
    }
    const UINT defb = flags & MB_DEFMASK;
    d.def = std::min((int)d.buttons.size() - 1, defb == MB_DEFBUTTON2 ? 1 : defb == MB_DEFBUTTON3 ? 2 : 0);

    d.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME & 0, kDialogClass, title.c_str(), WS_POPUP, 0, 0, 10, 10, d.owner, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!d.hwnd || !d.gfx.init(d.hwnd)) {   // couldn't draw: Windows' own box still answers
        if (d.hwnd) DestroyWindow(d.hwnd);
        return MessageBoxW(owner, text.c_str(), title.c_str(), flags);
    }
    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&d));
    UINT dpi = d.owner ? GetDpiForWindow(d.owner) : 96;
    if (!dpi) dpi = 96;
    d.scale = dpi / 96.0f;
    // Drawn at the display's scale, so what's seen is where clicks land (without this, at 125% or 150%
    // the buttons were drawn smaller than their click areas: OK only worked from the keyboard).
    d.gfx.setDpi(dpi);
    d.layout();
    // Centred over the owner (or the screen).
    RECT o{};
    if (d.owner && IsWindowVisible(d.owner)) GetWindowRect(d.owner, &o);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &o, 0);
    const int pw = (int)std::lround(d.w * d.scale), ph = (int)std::lround(d.h * d.scale);
    SetWindowPos(d.hwnd, HWND_TOP, (o.left + o.right - pw) / 2, (o.top + o.bottom - ph) / 2 - (int)(20 * d.scale), pw, ph, SWP_NOACTIVATE);
    const DWORD round = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(d.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    d.gfx.resize((UINT)pw, (UINT)ph);
    const bool ownerWasEnabled = d.owner && IsWindowEnabled(d.owner);
    if (ownerWasEnabled) EnableWindow(d.owner, FALSE);
    ShowWindow(d.hwnd, SW_SHOW);
    SetForegroundWindow(d.hwnd);
    SetFocus(d.hwnd);
    MessageBeep(icon == MB_ICONERROR ? MB_ICONHAND : icon == MB_ICONWARNING ? MB_ICONEXCLAMATION : 0);

    MSG msg;
    while (!d.done) {
        if (!GetMessageW(&msg, nullptr, 0, 0)) { PostQuitMessage((int)msg.wParam); d.result = d.cancelResult; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (ownerWasEnabled) EnableWindow(d.owner, TRUE);
    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(d.hwnd);
    if (d.owner) SetForegroundWindow(d.owner);
    return d.result;
}

}  // namespace cjui

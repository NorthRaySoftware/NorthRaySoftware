// A message box in the app's own look: the palette, the aurora band, rounded corners, NorthRay buttons.
// Called like Windows' MessageBoxW (MB_OK / MB_OKCANCEL / MB_YESNO / MB_YESNOCANCEL, MB_ICON*, MB_DEFBUTTON2/3)
// and answers the same way (IDOK, IDCANCEL, IDYES, IDNO), so it replaces it one for one.
#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"
#include "cjui/gfx.hpp"
#include "cjui/window.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <functional>
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
    // Long text (a big import's list, 0.93): the box keeps to the screen and the text scrolls in its own area, by the
    // wheel or the keys, so the buttons always show. No scroll bar: a fade and a note say there's more.
    float maxH = 100000, viewH = 0, scroll = 0;
    bool scrolls = false;
    Rect textR{};
    void scrollBy(float d) {
        const float s2 = std::clamp(scroll + d, 0.0f, std::max(0.0f, textH - viewH));
        if (s2 != scroll) { scroll = s2; InvalidateRect(hwnd, nullptr, FALSE); }
    }
    int cancelResult = IDCANCEL;
    HWND edit = nullptr;          // askText's line (0.91)
    HFONT font = nullptr;
    HBRUSH editBrush = nullptr;
    Rect editR{};

    void layout() {
        const float pad = 24;
        const float tx = icon ? pad + 44 : pad;
        textH = gfx.measureHeight(text, w - tx - pad, Font::Body);
        const float titleH = title.empty() ? 0 : 30;
        // Room for the text: what the screen leaves once the title, buttons (and askText's line) are in.
        const float fixed = pad + 6 + titleH + 24 + 38 + pad + (edit ? 50.0f : 0.0f);
        scrolls = fixed + textH > maxH;
        if (scrolls && w < 620) {   // wider first, so fewer lines need scrolling
            w = 620;
            textH = gfx.measureHeight(text, w - tx - pad - 16, Font::Body);
            scrolls = fixed + textH > maxH;
        }
        viewH = scrolls ? std::max(80.0f, maxH - fixed) : textH;
        textR = {tx, pad + 6 + titleH, w - tx - pad, viewH};
        h = pad + 6 + titleH + std::max(viewH, icon ? 36.0f : 0.0f) + 24 + 38 + pad;
        if (edit) {
            editR = {pad, pad + 6 + titleH + textH + 14, w - 2 * pad, 36};
            h += 50;
        }
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
        if (scrolls) {
            // The text as a list in its own area, scrolled.
            const Rect box = textR.inset(-8, -6);
            gfx.fillRounded(box, 8, pal.paper);
            gfx.strokeRounded(box, 8, pal.rule, 1);
            gfx.pushClip(textR);
            gfx.text(text, {textR.x, textR.y - scroll, textR.w - 16, textH + 4}, Font::Body, pal.inkMuted.mix(pal.ink, 0.45f));
            gfx.popClip();
            if (scroll < textH - viewH - 1) {
                for (int i = 0; i < 12; ++i)
                    gfx.fill({textR.x, textR.bottom() - 24 + i * 2.0f, textR.w, 2.0f}, pal.paper.withAlpha(i / 12.0f));
                gfx.text(L"More below: scroll, or use the arrow keys \u2193", {textR.x, textR.bottom() + 8, textR.w, 16}, Font::Small, pal.inkFaint,
                         {Align::Left, false, true});
            }
            // Where it is in the list: a thin mark at the side (not a bar to drag).
            const float frac = viewH / std::max(1.0f, textH);
            const float my = textR.y + (textR.h - textR.h * frac) * (scroll / std::max(1.0f, textH - viewH));
            gfx.fillRounded({textR.right() - 4, my, 3, textR.h * frac}, 1.5f, pal.inkFaint.withAlpha(0.5f));
        } else
            gfx.text(text, {tx, y, w - tx - pad, textH + 4}, Font::Body, pal.inkMuted.mix(pal.ink, 0.45f));
        if (edit) {
            gfx.fillRounded(editR, 7, pal.paper);
            gfx.strokeRounded(editR, 7, pal.ribbon, 1.5f);
        }
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
        case WM_MOUSEWHEEL:
            if (d->scrolls) d->scrollBy(-(float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 60);
            return 0;
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
            if (d->scrolls && (wp == VK_DOWN || wp == VK_UP || wp == VK_NEXT || wp == VK_PRIOR || wp == VK_HOME || wp == VK_END)) {
                const float page = d->viewH - 30;
                d->scrollBy(wp == VK_DOWN ? 40.0f : wp == VK_UP ? -40.0f : wp == VK_NEXT ? page : wp == VK_PRIOR ? -page : wp == VK_HOME ? -1e9f : 1e9f);
                return 0;
            }
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
        case WM_CTLCOLOREDIT: {
            const HDC dc = reinterpret_cast<HDC>(wp);
            auto rgb = [](Color c) { return RGB((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255)); };
            SetTextColor(dc, rgb(d->pal.ink));
            SetBkColor(dc, rgb(d->pal.paper));
            if (!d->editBrush) d->editBrush = CreateSolidBrush(rgb(d->pal.paper));
            return reinterpret_cast<LRESULT>(d->editBrush);
        }
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

}  // namespace

namespace {
void registerDialogClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = dialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kDialogClass;
    registered = RegisterClassExW(&wc) != 0;
}
// Shows a box whose buttons are set, and waits for its answer. `fallback` answers when it can't be drawn.
int runBox(Dialog& d, HWND /*owner*/, const std::wstring& /*text*/, const std::wstring& title, UINT icon, const std::function<int()>& fallback) {
    d.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME & 0, kDialogClass, title.c_str(), WS_POPUP, 0, 0, 10, 10, d.owner, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!d.hwnd || !d.gfx.init(d.hwnd)) {   // couldn't draw: Windows' own box still answers
        if (d.hwnd) DestroyWindow(d.hwnd);
        return fallback();
    }
    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&d));
    UINT dpi = d.owner ? uiDpiOf(d.owner) : 96;   // (0.185)
    if (!dpi) dpi = 96;
    d.scale = dpi / 96.0f;
    // Drawn at the display's scale, so what's seen is where clicks land (without this, at 125% or 150%
    // the buttons were drawn smaller than their click areas: OK only worked from the keyboard).
    d.gfx.setDpi(dpi);
    {
        // The screen the owner is on: the box never grows past it.
        RECT wa{};
        MONITORINFO mi{sizeof mi};
        if (GetMonitorInfoW(MonitorFromWindow(d.owner ? d.owner : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST), &mi)) wa = mi.rcWork;
        else SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        d.maxH = std::max(260.0f, (wa.bottom - wa.top) / d.scale - 80);
    }
    d.layout();
    // Centred over the owner (or the screen).
    RECT o{};
    if (d.owner && IsWindowVisible(d.owner)) GetWindowRect(d.owner, &o);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &o, 0);
    const int pw = (int)std::lround(d.w * d.scale), ph = (int)std::lround(d.h * d.scale);
    {
        // Kept on the screen even when the owner is near an edge.
        RECT wa{};
        MONITORINFO mi{sizeof mi};
        if (GetMonitorInfoW(MonitorFromWindow(d.owner ? d.owner : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST), &mi)) wa = mi.rcWork;
        else SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int x = (o.left + o.right - pw) / 2, y = (o.top + o.bottom - ph) / 2 - (int)(20 * d.scale);
        x = std::clamp(x, (int)wa.left, std::max((int)wa.left, (int)wa.right - pw));
        y = std::clamp(y, (int)wa.top, std::max((int)wa.top, (int)wa.bottom - ph));
        SetWindowPos(d.hwnd, HWND_TOP, x, y, pw, ph, SWP_NOACTIVATE);
    }
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

}  // namespace

int messageBox(HWND owner, const std::wstring& text, const std::wstring& title, UINT flags) {
    registerDialogClass();
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

    return runBox(d, owner, text, title, icon, [&] { return MessageBoxW(owner, text.c_str(), title.c_str(), flags); });
}

int choiceBox(HWND owner, const std::wstring& text, const std::wstring& title, const std::vector<std::wstring>& choices, int defaultChoice,
              UINT icon) {
    if (choices.empty()) return -1;
    registerDialogClass();
    Dialog d;
    d.owner = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    d.pal = menuPalette();
    d.title = title;
    d.text = text;
    icon &= MB_ICONMASK;
    d.icon = icon == MB_ICONINFORMATION ? 1 : icon == MB_ICONQUESTION ? 2 : icon == MB_ICONWARNING ? 3 : icon == MB_ICONERROR ? 4 : 0;
    for (size_t i = 0; i < choices.size(); ++i) d.buttons.push_back({choices[i], 1000 + (int)i, {}});
    d.buttons.push_back({L"Cancel", IDCANCEL, {}});
    d.cancelResult = IDCANCEL;
    d.def = std::clamp(defaultChoice, 0, (int)choices.size() - 1);
    const int r = runBox(d, owner, text, title, icon, [&] {
        // Windows' own box has only its own buttons: Yes for the first choice, No for the second.
        std::wstring t = text + L"\n\nYes: " + choices[0] + (choices.size() > 1 ? L"\nNo: " + choices[1] : L"");
        const int a = MessageBoxW(owner, t.c_str(), title.c_str(), (choices.size() > 1 ? MB_YESNOCANCEL : MB_OKCANCEL) | icon |
                                                                     (defaultChoice == 1 ? MB_DEFBUTTON2 : 0));
        return a == IDYES || a == IDOK ? 1000 : a == IDNO ? 1001 : IDCANCEL;
    });
    return r >= 1000 && r < 1000 + (int)choices.size() ? r - 1000 : -1;
}

bool askText(HWND owner, const std::wstring& title, const std::wstring& text, std::wstring& value, const std::wstring& okLabel) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = dialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"CJUIAskText";
        registered = RegisterClassExW(&wc) != 0;
    }
    Dialog d;
    d.owner = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    d.pal = menuPalette();
    d.title = title;
    d.text = text;
    d.w = 520;
    d.buttons = {{okLabel, IDOK, {}}, {L"Cancel", IDCANCEL, {}}};
    d.hwnd = CreateWindowExW(0, L"CJUIAskText", title.c_str(), WS_POPUP, 0, 0, 10, 10, d.owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!d.hwnd || !d.gfx.init(d.hwnd)) { if (d.hwnd) DestroyWindow(d.hwnd); return false; }
    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&d));
    UINT dpi = d.owner ? uiDpiOf(d.owner) : 96;   // (0.185)
    if (!dpi) dpi = 96;
    d.scale = dpi / 96.0f;
    d.gfx.setDpi(dpi);
    d.edit = CreateWindowExW(0, L"EDIT", value.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, d.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    d.layout();
    d.font = CreateFontW(-(int)std::lround(14 * d.scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(d.edit, WM_SETFONT, reinterpret_cast<WPARAM>(d.font), FALSE);
    const Rect er = d.editR;
    SetWindowPos(d.edit, nullptr, (int)std::lround((er.x + 10) * d.scale), (int)std::lround((er.y + 8) * d.scale), (int)std::lround((er.w - 20) * d.scale),
                 (int)std::lround(20 * d.scale), SWP_NOZORDER);
    RECT o{};
    if (d.owner && IsWindowVisible(d.owner)) GetWindowRect(d.owner, &o);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &o, 0);
    const int pw = (int)std::lround(d.w * d.scale), ph = (int)std::lround(d.h * d.scale);
    SetWindowPos(d.hwnd, HWND_TOP, (o.left + o.right - pw) / 2, (o.top + o.bottom - ph) / 2 - (int)(20 * d.scale), pw, ph, SWP_NOACTIVATE);
    const DWORD round = 2;
    DwmSetWindowAttribute(d.hwnd, 33, &round, sizeof round);
    d.gfx.resize((UINT)pw, (UINT)ph);
    const bool ownerWasEnabled = d.owner && IsWindowEnabled(d.owner);
    if (ownerWasEnabled) EnableWindow(d.owner, FALSE);
    ShowWindow(d.hwnd, SW_SHOW);
    SetForegroundWindow(d.hwnd);
    SetFocus(d.edit);
    SendMessageW(d.edit, EM_SETSEL, 0, -1);
    MSG msg;
    while (!d.done) {
        if (!GetMessageW(&msg, nullptr, 0, 0)) { PostQuitMessage((int)msg.wParam); d.result = IDCANCEL; break; }
        if (msg.message == WM_KEYDOWN && msg.hwnd == d.edit && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
            d.finish(msg.wParam == VK_RETURN ? IDOK : IDCANCEL);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (d.result == IDOK) {
        const int n = GetWindowTextLengthW(d.edit);
        std::wstring t((size_t)n, L'\0');
        if (n) GetWindowTextW(d.edit, t.data(), n + 1);
        value = t;
    }
    if (ownerWasEnabled) EnableWindow(d.owner, TRUE);
    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(d.hwnd);
    if (d.font) DeleteObject(d.font);
    if (d.editBrush) DeleteObject(d.editBrush);
    if (d.owner) SetForegroundWindow(d.owner);
    return d.result == IDOK;
}

}  // namespace cjui

#include "cjui/menu.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace cjui {

static Palette g_menuPalette = makePalette(false);
void setMenuPalette(const Palette& pal) { g_menuPalette = pal; }
const Palette& menuPalette() { return g_menuPalette; }

static std::vector<MenuItem> fromHmenu(HMENU m, int depth) {
    std::vector<MenuItem> out;
    const int n = GetMenuItemCount(m);
    for (int i = 0; i < n && depth < 6; ++i) {
        wchar_t buf[512] = {};
        MENUITEMINFOW mi{sizeof mi};
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = buf;
        mi.cch = 511;
        if (!GetMenuItemInfoW(m, (UINT)i, TRUE, &mi)) continue;
        if (mi.fType & MFT_SEPARATOR) { out.push_back(MenuItem::sep()); continue; }
        std::wstring text = buf, shortcut;
        if (const size_t tab = text.find(L'\t'); tab != std::wstring::npos) { shortcut = text.substr(tab + 1); text.resize(tab); }
        std::wstring clean;
        for (size_t k = 0; k < text.size(); ++k) {
            if (text[k] == L'&') { if (k + 1 < text.size() && text[k + 1] == L'&') { clean += L'&'; ++k; } continue; }
            clean += text[k];
        }
        if (mi.hSubMenu) out.push_back(MenuItem::sub(clean, fromHmenu(mi.hSubMenu, depth + 1)));
        else out.push_back(MenuItem::item(clean, mi.wID, shortcut, !(mi.fState & MFS_DISABLED), (mi.fState & MFS_CHECKED) != 0));
    }
    return out;
}

UINT trackMenu(HMENU menu, POINT at, HWND owner) {
    return showMenu(owner, fromHmenu(menu, 0), at, g_menuPalette);
}

namespace {

// Layout, in DIPs.
constexpr float kPad = 6;        // around the rows
constexpr float kRowH = 32;
constexpr float kSepH = 9;
constexpr float kGutter = 34;    // the check mark's column
constexpr float kShortcutGap = 36;
constexpr float kArrowW = 24;
constexpr float kRight = 14;
constexpr float kMinW = 200, kMaxW = 440;
constexpr float kRadius = 8;
constexpr UINT_PTR kSubmenuTimer = 1;
constexpr UINT kSubmenuDelayMs = 200;

const wchar_t* kClass = L"CJUIMenu";

struct Level {
    HWND hwnd = nullptr;
    Gfx gfx;
    const std::vector<MenuItem>* items = nullptr;
    std::vector<Rect> rows;   // per item, in DIPs inside the menu
    float w = 0, h = 0;       // DIPs
    RECT screen{};            // pixels
    int hover = -1;
    int open = -1;            // item whose submenu is showing
};

struct Session {
    HWND owner = nullptr;
    Palette pal;
    float scale = 1;
    UINT dpi = 96;
    std::vector<std::unique_ptr<Level>> levels;
    UINT result = 0;
    bool done = false;
    int pendingLevel = -1, pendingItem = -1;   // submenu to open (or deeper menus to close) when the timer fires

    bool selectable(const MenuItem& m) const { return !m.separator; }

    void paint(Level& L) {
        if (!L.gfx.begin()) return;
        const Color bg = pal.surface;
        L.gfx.clear(bg);
        L.gfx.strokeRounded({0, 0, L.w, L.h}, kRadius, pal.rule.mix(pal.inkFaint, pal.dark ? 0.25f : 0.15f), 1);
        const auto& items = *L.items;
        for (size_t i = 0; i < items.size(); ++i) {
            const MenuItem& m = items[i];
            const Rect r = L.rows[i];
            if (m.separator) {
                const float y = std::floor((r.y + r.h / 2) * scale) / scale + 0.5f / scale;
                L.gfx.line(r.x + 8, y, r.right() - 8, y, pal.rule, 1);
                continue;
            }
            const bool lit = (int)i == L.hover || (int)i == L.open;
            if (lit) L.gfx.fillRounded(r, 5, pal.ink.withAlpha(pal.dark ? (m.enabled ? 0.11f : 0.06f) : (m.enabled ? 0.065f : 0.035f)));
            const Color ink = m.enabled ? pal.ink : pal.inkFaint;
            if (m.checked) {
                const float cx = r.x + kGutter / 2, cy = r.y + r.h / 2;
                const Color c = m.enabled ? pal.ribbon : pal.inkFaint;
                L.gfx.line(cx - 5, cy, cx - 1.5f, cy + 3.5f, c, 1.6f);
                L.gfx.line(cx - 1.5f, cy + 3.5f, cx + 5, cy - 4, c, 1.6f);
            }
            const float tx = r.x + kGutter;
            L.gfx.text(m.label, {tx, r.y, r.right() - tx - 8, r.h}, Font::Body, ink, {Align::Left, false, true, true});
            if (!m.submenu.empty()) {
                const float cx = r.right() - kArrowW / 2 - 2, cy = r.y + r.h / 2;
                L.gfx.line(cx - 2.5f, cy - 5, cx + 2.5f, cy, ink.withAlpha(0.75f), 1.4f);
                L.gfx.line(cx + 2.5f, cy, cx - 2.5f, cy + 5, ink.withAlpha(0.75f), 1.4f);
            } else if (!m.shortcut.empty()) {
                L.gfx.text(m.shortcut, {tx, r.y, r.right() - tx - kRight + kPad, r.h}, Font::Small,
                           m.enabled ? pal.inkMuted : pal.inkFaint, {Align::Right, false, false, true});
            }
        }
        L.gfx.end();
    }

    void redraw(Level& L) { InvalidateRect(L.hwnd, nullptr, FALSE); }

    int itemAt(const Level& L, POINT pt) const {
        const float x = (pt.x - L.screen.left) / scale, y = (pt.y - L.screen.top) / scale;
        for (size_t i = 0; i < L.rows.size(); ++i)
            if (L.rows[i].contains(x, y)) return (*L.items)[i].separator ? -1 : (int)i;
        return -1;
    }

    int levelAt(POINT pt) const {
        for (int i = (int)levels.size() - 1; i >= 0; --i)
            if (PtInRect(&levels[(size_t)i]->screen, pt)) return i;
        return -1;
    }

    // Opens a menu. For a submenu, `parent` is its level and `row` the item's row there.
    bool open(const std::vector<MenuItem>& items, POINT at, MenuAnchor anchor, int parent, int row) {
        auto L = std::make_unique<Level>();
        L->items = &items;
        L->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP, 0, 0, 1, 1, owner,
                                  nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!L->hwnd) return false;
        SetWindowLongPtrW(L->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (!L->gfx.init(L->hwnd)) { DestroyWindow(L->hwnd); return false; }
        L->gfx.setDpi(dpi);

        // Size: the widest label and shortcut decide the width.
        float label = 0, shortcut = 0;
        bool anySub = false;
        for (const MenuItem& m : items) {
            if (m.separator) continue;
            label = std::max(label, L->gfx.measureWidth(m.label, Font::Body));
            if (!m.shortcut.empty()) shortcut = std::max(shortcut, L->gfx.measureWidth(m.shortcut, Font::Small));
            anySub = anySub || !m.submenu.empty();
        }
        float w = kPad * 2 + kGutter + label + (shortcut > 0 ? kShortcutGap + shortcut : 0) + (anySub ? kArrowW : 0) + kRight;
        w = std::clamp(std::ceil(w), kMinW, kMaxW);
        float y = kPad;
        for (const MenuItem& m : items) {
            const float h = m.separator ? kSepH : kRowH;
            L->rows.push_back({kPad, y, w - kPad * 2, h});
            y += h;
        }
        const float h = y + kPad;
        L->w = w;
        L->h = h;
        const int pw = (int)std::lround(w * scale), ph = (int)std::lround(h * scale);

        // Place it on the screen the point is on, inside its work area.
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT wa = mi.rcWork;
        int x, top;
        if (parent < 0) {
            x = anchor == MenuAnchor::TopRight ? at.x - pw : at.x;
            top = at.y;
            if (top + ph > wa.bottom) top = std::max<int>(wa.top, at.y - ph);
        } else {
            const Level& P = *levels[(size_t)parent];
            const int overlap = (int)std::lround(3 * scale);
            x = P.screen.right - overlap;
            if (x + pw > wa.right) x = P.screen.left - pw + overlap;   // no room on the right: open to the left
            top = P.screen.top + (int)std::lround((P.rows[(size_t)row].y - kPad) * scale);
            if (top + ph > wa.bottom) top = wa.bottom - ph;
        }
        x = std::clamp<int>(x, wa.left, std::max<int>(wa.left, wa.right - pw));
        top = std::max<int>(top, wa.top);
        L->screen = {x, top, x + pw, top + ph};

        // Windows 11: rounded corners to match; elsewhere they're square, which is fine.
        const DWORD round = 2;   // DWMWCP_ROUND
        DwmSetWindowAttribute(L->hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
        SetWindowPos(L->hwnd, HWND_TOPMOST, x, top, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        L->gfx.resize((UINT)pw, (UINT)ph);
        levels.push_back(std::move(L));
        UpdateWindow(levels.back()->hwnd);
        return true;
    }

    void closeFrom(size_t depth) {
        while (levels.size() > depth) {
            levels.back()->gfx.shutdown();
            DestroyWindow(levels.back()->hwnd);
            levels.pop_back();
        }
        if (depth > 0 && depth <= levels.size()) {
            levels[depth - 1]->open = -1;
            redraw(*levels[depth - 1]);
        }
    }

    void openSubmenu(int level, int item, bool selectFirst) {
        Level& L = *levels[(size_t)level];
        const MenuItem& m = (*L.items)[(size_t)item];
        if (m.submenu.empty() || !m.enabled) return;
        if (L.open == item && (int)levels.size() > level + 1) return;
        closeFrom((size_t)level + 1);
        L.open = item;
        redraw(L);
        if (open(m.submenu, {}, MenuAnchor::TopLeft, level, item) && selectFirst) {
            Level& S = *levels.back();
            for (size_t i = 0; i < S.items->size(); ++i)
                if (selectable((*S.items)[i])) { S.hover = (int)i; break; }
            redraw(S);
        }
    }

    void choose(int level, int item) {
        const MenuItem& m = (*levels[(size_t)level]->items)[(size_t)item];
        if (!m.enabled || m.separator) return;
        if (!m.submenu.empty()) { openSubmenu(level, item, false); return; }
        result = m.id;
        done = true;
    }

    void schedule(int level, int item) {
        pendingLevel = level;
        pendingItem = item;
        SetTimer(levels[0]->hwnd, kSubmenuTimer, kSubmenuDelayMs, nullptr);
    }

    void onTimer() {
        KillTimer(levels[0]->hwnd, kSubmenuTimer);
        if (pendingLevel < 0 || pendingLevel >= (int)levels.size()) return;
        Level& L = *levels[(size_t)pendingLevel];
        if (L.hover != pendingItem) return;   // moved on since
        if (pendingItem >= 0 && !(*L.items)[(size_t)pendingItem].submenu.empty()) openSubmenu(pendingLevel, pendingItem, false);
        else closeFrom((size_t)pendingLevel + 1);
        pendingLevel = pendingItem = -1;
    }

    void mouseMove(POINT pt) {
        const int li = levelAt(pt);
        if (li < 0) {
            // Off the menus: the deepest one forgets its hover (an open submenu's item stays lit).
            Level& D = *levels.back();
            if (D.hover != -1) { D.hover = -1; redraw(D); }
            return;
        }
        Level& L = *levels[(size_t)li];
        const int it = itemAt(L, pt);
        if (it == L.hover) return;
        L.hover = it;
        redraw(L);
        // Deeper menus that aren't this item's go after a moment; this item's submenu opens after one.
        const bool hasSub = it >= 0 && !(*L.items)[(size_t)it].submenu.empty();
        if (hasSub || (int)levels.size() > li + 1) schedule(li, it);
    }

    void move(int dir) {
        Level& D = *levels.back();
        const int n = (int)D.items->size();
        int i = D.hover;
        for (int k = 0; k < n; ++k) {
            i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
            if (selectable((*D.items)[(size_t)i])) break;
        }
        D.hover = i;
        redraw(D);
    }

    void key(WPARAM vk) {
        Level& D = *levels.back();
        const int depth = (int)levels.size() - 1;
        switch (vk) {
            case VK_DOWN: move(+1); break;
            case VK_UP: move(-1); break;
            case VK_HOME: D.hover = -1; move(+1); break;
            case VK_END: D.hover = -1; move(-1); break;
            case VK_RIGHT:
                if (D.hover >= 0) openSubmenu(depth, D.hover, true);
                break;
            case VK_LEFT:
                if (depth > 0) closeFrom((size_t)depth);
                break;
            case VK_ESCAPE:
                if (depth > 0) closeFrom((size_t)depth);
                else done = true;
                break;
            case VK_RETURN:
            case VK_SPACE:
                if (D.hover >= 0) {
                    if (!(*D.items)[(size_t)D.hover].submenu.empty()) openSubmenu(depth, D.hover, true);
                    else choose(depth, D.hover);
                }
                break;
            case VK_MENU:
            case VK_F10:
                done = true;
                break;
        }
    }
};

LRESULT CALLBACK menuProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<Session*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            if (s)
                for (auto& L : s->levels)
                    if (L->hwnd == h) s->paint(*L);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CAPTURECHANGED:
            // Something else took the mouse: the menu goes, as Windows' own does.
            if (s && !s->levels.empty() && s->levels[0]->hwnd == h && (HWND)lp != h) s->done = true;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool isMouseMessage(UINT m) { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
bool isNcMouseMessage(UINT m) { return m >= WM_NCMOUSEMOVE && m <= WM_NCXBUTTONDBLCLK; }

}  // namespace

UINT showMenu(HWND owner, const std::vector<MenuItem>& items, POINT at, const Palette& pal, MenuAnchor anchor) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = menuProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    if (items.empty()) return 0;

    Session s;
    s.owner = owner;
    s.pal = pal;
    s.dpi = GetDpiForWindow(owner);
    if (!s.dpi) s.dpi = 96;
    s.scale = s.dpi / 96.0f;
    if (!s.open(items, at, anchor, -1, -1)) return 0;
    SetCapture(s.levels[0]->hwnd);
    const HWND root = GetAncestor(owner, GA_ROOT);

    MSG msg;
    while (!s.done) {
        if (!GetMessageW(&msg, nullptr, 0, 0)) {   // WM_QUIT: leave, and pass it on
            PostQuitMessage((int)msg.wParam);
            break;
        }
        const UINT m = msg.message;
        if (isMouseMessage(m) || isNcMouseMessage(m)) {
            const POINT pt = msg.pt;
            const int li = s.levelAt(pt);
            switch (m) {
                case WM_MOUSEMOVE: case WM_NCMOUSEMOVE: s.mouseMove(pt); break;
                case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
                case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: case WM_NCMBUTTONDOWN: case WM_NCXBUTTONDOWN:
                    if (li < 0) s.done = true;   // a click elsewhere closes the menu (and isn't passed on)
                    break;
                case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_NCLBUTTONUP: case WM_NCRBUTTONUP:
                    if (li >= 0) {
                        const int it = s.itemAt(*s.levels[(size_t)li], pt);
                        if (it >= 0) s.choose(li, it);
                    }
                    break;
                default: break;   // wheel, double-clicks: ignored while the menu is open
            }
            continue;
        }
        if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) { s.key(msg.wParam); continue; }
        if (m == WM_KEYUP || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR || m == WM_DEADCHAR) continue;
        if (m == WM_TIMER && msg.hwnd == s.levels[0]->hwnd && msg.wParam == kSubmenuTimer) { s.onTimer(); continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(owner) || GetForegroundWindow() != root) s.done = true;   // closed, or switched away
    }

    KillTimer(s.levels[0]->hwnd, kSubmenuTimer);
    const HWND top = s.levels[0]->hwnd;
    SetWindowLongPtrW(top, GWLP_USERDATA, 0);   // no WM_CAPTURECHANGED handling while closing
    if (GetCapture() == top) ReleaseCapture();
    for (auto& L : s.levels) SetWindowLongPtrW(L->hwnd, GWLP_USERDATA, 0);
    s.closeFrom(0);
    return s.result;
}

}  // namespace cjui

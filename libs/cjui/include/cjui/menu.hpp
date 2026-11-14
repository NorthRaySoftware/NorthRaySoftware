// A pop-up menu drawn in the NorthRay look (the app's palette, light or dark),
// used in place of Windows' own, which ignores the app's theme.
//
// Works like TrackPopupMenu with TPM_RETURNCMD: it shows the menu, runs until
// something is chosen or the menu is dismissed, and returns the chosen id (0
// for none). Mouse and keyboard: hover or Up/Down to move, Right or hover to
// open a submenu, Left or Esc to close one, Enter/Space or a click to choose;
// a click anywhere else, Alt, or switching away closes it.
#pragma once

#include "cjui/window.hpp"

#include <string>
#include <vector>

namespace cjui {

struct MenuItem {
    std::wstring label;
    std::wstring shortcut;          // shown on the right, e.g. L"Ctrl+T" (shown only; the app handles the keys)
    UINT id = 0;
    bool enabled = true;
    bool checked = false;
    bool separator = false;
    std::vector<MenuItem> submenu;  // non-empty: this item opens a submenu

    static MenuItem item(std::wstring label, UINT id, std::wstring shortcut = {}, bool enabled = true, bool checked = false) {
        MenuItem m;
        m.label = std::move(label);
        m.id = id;
        m.shortcut = std::move(shortcut);
        m.enabled = enabled;
        m.checked = checked;
        return m;
    }
    static MenuItem sub(std::wstring label, std::vector<MenuItem> items) {
        MenuItem m;
        m.label = std::move(label);
        m.submenu = std::move(items);
        return m;
    }
    static MenuItem sep() { MenuItem m; m.separator = true; return m; }
};

enum class MenuAnchor { TopLeft, TopRight };  // which corner of the menu sits at the point

// `at` is in screen pixels. `pal` is the owner's palette, so the menu matches it.
UINT showMenu(HWND owner, const std::vector<MenuItem>& items, POINT at, const Palette& pal,
              MenuAnchor anchor = MenuAnchor::TopLeft);

// A Windows menu (CreatePopupMenu + AppendMenuW) shown as a NorthRay menu, in the
// palette of the app's window: a drop-in for TrackPopupMenu(..., TPM_RETURNCMD, ...).
// "&" mnemonics are dropped; text after a tab is shown as the shortcut.
UINT trackMenu(HMENU menu, POINT at, HWND owner);
// The palette menus use when none is given (set by AppWindow::applyPalette).
void setMenuPalette(const Palette& pal);

}  // namespace cjui

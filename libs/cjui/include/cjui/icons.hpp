// NorthRay's own small line icons (UI pass, 2026-11): drawn with the window's lines and shapes on a 16-unit grid, so
// they follow the theme's colours and stay sharp at any scaling. `iconFor` picks one for a command from its words, so
// buttons get matching icons without each caller choosing one.
#pragma once

#include "cjui/gfx.hpp"

#include <string>

namespace cjui {

enum class Icon {
    None,
    Library, Writing, Planning, World, Review, Typeset, Preview, Publish, Settings,
    Search, Plus, Trash, Import, Export, Open, Pencil, Back, Forward, Share, Picture, Book, Close, Undo, Check,
    Folder, Sync, Copy, More, Up, Down, Comment, Person, Pin, Link, Eye, Font, Archive, Camera, Target, Filter, Sort,
    Panel, Sidebar, Cards, List, Map, Spark,
};

// Draws `icon` centred in `r` (a square works best), lines `width` DIPs thick (about 1.5 at 16 DIPs).
void drawIcon(Gfx& g, Icon icon, const Rect& r, Color c, float width = 1.5f);
// The icon for a command by its words ("New chapter", "+ Scene", "Import…", "Move to Trash"…), or Icon::None.
Icon iconFor(const std::wstring& label);

}  // namespace cjui

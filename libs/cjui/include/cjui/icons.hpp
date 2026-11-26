// NorthRay's own small line icons (UI pass, 2026-11): drawn with the window's lines and shapes on a 16-unit grid, so
// they follow the theme's colours and stay sharp at any scaling. `iconFor` picks one for a command from its words, so
// buttons get matching icons without each caller choosing one.
#pragma once

#include "cjui/gfx.hpp"

#include <string>
#include <vector>

namespace cjui {

enum class Icon {
    None,
    Library, Writing, Planning, World, Review, Typeset, Preview, Publish, Settings,
    Search, Plus, Trash, Import, Export, Open, Pencil, Back, Forward, Share, Picture, Book, Close, Undo, Check,
    Folder, Sync, Copy, More, Up, Down, Comment, Person, Pin, Link, Eye, Font, Archive, Camera, Target, Filter, Sort,
    Panel, Sidebar, Cards, List, Map, Spark,
    // 0.87 (Writing Studio's visual pass): the bible's kinds and what the pages show.
    Calendar, Moon, Clock, Flag, Scroll, Paw, House, Mountain, Shield, Gem, Star, Speech, People, Tree, Web, Thread, Warning,
    Timeline,
};
// Every icon by a short name ("calendar", "paw"…) and back, for settings and a page's chosen symbol.
const char* iconName(Icon icon);
Icon iconNamed(const std::string& name);
// The icons a writer can choose from as a page's symbol, in order.
const std::vector<Icon>& symbolIcons();

// How buttons and menus show their commands (0.87): icons with their words, icons alone (the words as tooltips), or
// words alone. Set by the app from its settings; buttons and menus read it.
enum class LabelStyle { Both, IconsOnly, WordsOnly };
void setLabelStyle(LabelStyle s);
LabelStyle labelStyle();

// Draws `icon` centred in `r` (a square works best), lines `width` DIPs thick (about 1.5 at 16 DIPs).
void drawIcon(Gfx& g, Icon icon, const Rect& r, Color c, float width = 1.5f);
// The icon for a command by its words ("New chapter", "+ Scene", "Import…", "Move to Trash"…), or Icon::None.
Icon iconFor(const std::wstring& label);

}  // namespace cjui

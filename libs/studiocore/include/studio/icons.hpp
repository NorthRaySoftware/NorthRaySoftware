// Writing Studio: a bible page's icon as data (phase 4, 0.103), shared by the app, the typesetter and the ebook so a
// character looks the same on screen and in print: its colour (its own, else a character's or creature's from a fixed
// set by its id, as on the timeline, else its kind's), the ink that reads on it, its initials, and what it can show.
#pragma once

#include "studio/library.hpp"

#include <string>

namespace studio::icons {

struct Icon {
    std::string name;
    std::string colour = "#3B7DD8";     // "#RRGGBB"
    std::string ink = "#FFFFFF";        // dark or white, whichever reads on the colour
    std::string initials;
    std::string symbol;                 // a character, emoji or ornament ("" = none; drawn "icon:" symbols aren't here)
    std::string symbolImage;            // a picture as its symbol (a blob)
    std::string image;                  // its picture (a blob), unless it shows its symbol alone
};

Icon iconOf(const Entry& e);
std::string colourOf(const Entry& e);           // light-theme colour, as the app's povColour
std::string inkOn(const std::string& hex);      // "#15171E" on a light colour, "#FFFFFF" on a dark one
std::string initialsOf(const std::string& name);
// The viewpoint a chapter is told from: its own, else its first scene's with one ("" = none).
Id viewpointOf(const Library& lib, const ManuscriptNode& n);
// Chapters whose title begins with their viewpoint's symbol, typed (4.9): their ids, for offering to remove it.
std::vector<Id> titlesStartingWithSymbol(const Library& lib, const Id& book);
// The title without that symbol at its start (and the space or punctuation after it).
std::string withoutLeadingSymbol(const std::string& title, const std::string& symbol);

}  // namespace studio::icons

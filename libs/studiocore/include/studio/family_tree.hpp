// Writing Studio: a family tree as a picture (0.45), for saving, the bible's PDF and its website. Portable and tested.
//
// Drawn from Library::familyLayout the way the Worldbuilding tree draws it: a card per person (picture or initials,
// name on up to two lines, a gender sign and a detail such as age), partners joined side by side (a double line for
// a marriage), two parents of the same children joined by a dashed line when they aren't partners, and each set of
// children hanging from their parents. Light colours, for paper.
#pragma once

#include "studio/library.hpp"

#include <string>

namespace studio::familytree {

// "The Yerasuun family" (a family from its founder, by the surname most of its own people share), "Tobias's family"
// (no shared surname), or "Tobias's family tree" (one person's own tree).
std::string title(const Library& lib, const Id& root, bool bloodline);

struct Options {
    bool bloodline = true;       // the whole family from its founder; false: the person's own tree
    double scale = 2.0;          // pixels per point of the card design (cards are 196 × 80 at 1)
    bool heading = true;         // the title and a line of counts above the tree
    bool key = true;             // the key to the lines below it
    bool turnWide = false;       // a tree much wider than tall is turned a quarter (for a book's page)
};

struct Picture {
    bool ok = false;
    std::string error;
    std::string png;
    int width = 0, height = 0;
    int people = 0;
    bool turned = false;
};

// `fontsFolder`: the app's fonts (a list "app|world" works too); names are set in Gentium Book Plus.
Picture render(const Library& lib, const Id& root, const std::string& fontsFolder, const Options& options = {});

}  // namespace studio::familytree

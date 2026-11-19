// Sharing a world bible: as a website (a folder of linked pages to put online or open from disk),
// and as a printable book (typeset like a manuscript: a part for each kind, a chapter for each entry).
// Either can be the author's whole bible, or a reader's guide that keeps to what readers know by the
// end of a chosen book (facts marked as revealed later are left out) and leaves out the author's notes.
#pragma once

#include "studio/conlang.hpp"
#include "studio/library.hpp"
#include "studio/typeset.hpp"

#include <string>
#include <vector>

namespace studio::bible {

struct Options {
    Id worldId, bookId;          // the bible: a world's, or an independent book's
    bool readersGuide = false;   // no notes or notes per book; only what's revealed by `upTo`
    Id upTo;                     // a reader's guide: the last book read ("" = every book)
    // What goes in (0.38; both the website and the PDF).
    bool pictures = true;        // entries' pictures, galleries and maps
    bool family = true;          // family and relationships
    bool languages = true;       // the world's languages, as dictionaries
    bool glossary = true;
};

// How the website looks (0.38), chosen before it's made and kept with the world.
struct Look {
    std::string theme = "parchment";   // see themes()
    std::string mode = "auto";         // "auto" (follows the reader's device), "light" or "dark"
    std::string accent;                // #RRGGBB; "" = the theme's own (the app offers the world's colour too)
    std::string headings = "theme";    // see fonts(); "theme" = the theme's own
    std::string body = "theme";
    std::string title;                 // "" = the world's name
    std::string tagline;               // a line under the title on the home page
    bool cards = true;                 // the home page lists entries as cards with pictures (else name lists)
    bool search = true;                // a search box on every page
    bool kindColours = true;           // each kind of entry in its own colour
};
struct Theme { const char* id; const char* label; const char* description; };
const std::vector<Theme>& themes();
// A theme's colours (#RRGGBB), light or dark, for showing it before it's made.
struct ThemeColours { std::string paper, card, ink, muted, rule, accent, head; bool capitals = false; bool alwaysDark = false; };
ThemeColours themeColours(const std::string& theme, bool dark);
struct FontChoice { const char* id; const char* label; const char* bundled; };   // bundled: a fonts/<id> family copied into the site
const std::vector<FontChoice>& fonts();
std::string toJson(const Look& look);
Look lookFromJson(const std::string& json);

// The bible's languages (Worldbuilding › Languages), as kept in the library.
std::vector<conlang::Language> languagesOf(const Library& lib, const Id& worldId, const Id& bookId);

// The books of the bible's world (or the independent book) in reading order: each series in its
// reading order, then books in no series.
std::vector<Id> booksInOrder(const Library& lib, const Id& worldId, const Id& bookId);

// What a shared bible shows.
class View {
public:
    View(const Library& lib, const Options& options);
    const std::vector<const Entry*>& entries() const { return entries_; }   // by name
    bool shows(const Entry& e, const std::string& key) const;              // a fact ("detail:Age", "section:...", "summary")
    bool shows(const Id& entryId) const;
    const Options& options() const { return options_; }
    std::string title() const;                                             // "Alvora"

private:
    const Library& lib_;
    Options options_;
    std::vector<Id> books_;
    int upToIndex_ = -1;
    std::vector<const Entry*> entries_;
    bool revealedInTime(const Id& book) const;
};

// A website in `folder` (made if missing): index.html, a page for each entry, glossary.html, languages.html,
// style.css, site.js (search; the site works without it), the pictures in images/ and, for a bundled typeface, its
// files in fonts/. `fontsFolder` is the app's fonts folder (for bundled typefaces; "" = system fonts only). Returns
// false with `error` set when it can't be written.
bool exportWebsite(const Library& lib, const Options& options, const std::string& folder, std::string& error, int* pages = nullptr,
                   const Look& look = {}, const std::string& fontsFolder = {});
// The stylesheet a look gives (also used for the app's preview of it).
std::string websiteCss(const Look& look, const std::string& worldAccent, bool bundledHeadings, bool bundledBody);

// A printable bible for the typesetter: a part for each kind, a chapter for each entry, a glossary at
// the end. `title` heads it ("The Alvora Bible").
typeset::Source printableSource(const Library& lib, const Options& options, const std::string& title, const std::string& author);

// "png", "jpg", "gif", "webp" or "" from a picture's first bytes; and its size in pixels (0 if unknown).
std::string pictureType(const std::string& bytes);
std::pair<int, int> pictureSize(const std::string& bytes);

}  // namespace studio::bible

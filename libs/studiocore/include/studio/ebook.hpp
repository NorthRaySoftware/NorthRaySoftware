// Ebooks: a book's manuscript as reflowable HTML, the way e-readers show it.
// The preview (Milestone 6) shows it on simulated devices; the EPUB export
// (Milestone 7) packages the same chapters.
#pragma once

#include "studio/typeset.hpp"

#include <functional>
#include <string>
#include <vector>

namespace studio::ebook {

struct Chapter {
    std::string title;
    bool part = false;          // a part title page
    std::string html;           // the chapter's body: <section> ... </section>
    std::int64_t words = 0;
};

struct Content {
    std::string title, subtitle, author, language;
    std::vector<Chapter> chapters;
};

// How the ebook looks, from the book's Typesetting design (`styleFrom`). The default is the plain
// reading look (titles only, indented paragraphs, no lead-in).
struct Style {
    std::string chapterStyle = "title";   // "title", "number" ("Chapter One" over the title), "numeral" (a large figure)
    bool leadIn = false;                  // a chapter's first words in small capitals
    bool dropCap = false;                 // a chapter's first letter large
    bool justify = true, hyphenate = true;
    double indentEm = 1.2;
    bool spaced = false;                  // paragraphs with space between and no indent
    std::string sceneBreak = "* * *";
    std::string fontFamily;               // the CSS family when the typeface is embedded ("" = the reader's own)
    bool chapterDates = false;            // each chapter's date under its heading (0.85), as the typeset book
    std::string dateForm = "words", dateEra = "abbr";
};
Style styleFrom(const typeset::Design& d);

// Where a picture is found, as a URL for the HTML (the preview's image host, or
// a file inside the EPUB).
using PictureUrl = std::function<std::string(const std::string& blob)>;

// The same, for a picture cropped ahead of time (the EPUB): the URL is of the
// picture as it should be shown, so the HTML carries no crop.
using CroppedPictureUrl = std::function<std::string(const std::string& blob, const Placement& crop)>;

// Each chapter as semantic HTML: h1 for its title, p with paragraph-style
// classes, em/strong/span.sc marks, hr.scene for scene breaks, figure for
// pictures (crop kept as data for the reader's stylesheet).
Content build(const typeset::Source& src, const PictureUrl& pictureUrl, const Style& style = {});
Content buildCropped(const typeset::Source& src, const CroppedPictureUrl& pictureUrl, const Style& style = {});

// One document's body (document JSON) as HTML, without the chapter title.
std::string documentHtml(const std::string& docJson, const PictureUrl& pictureUrl);

// The world typefaces set on words in a book (font marks: runes, handwriting; 0.41), each id once, in order of use.
std::vector<std::string> fontsUsed(const typeset::Source& src);
// The CSS for one of them: an @font-face per style file (`url` gives each file's URL from its path) and the class the
// words carry (span.ff-<id>). "" when the family isn't found.
std::string worldFontCss(const std::string& fontsFolders, const std::string& id, const std::function<std::string(const std::string& path)>& url);

// The reading stylesheet shared by the preview and the EPUB.
std::string stylesheet(const Style& style = {});

// A chapter's opening heading (<h1>) for a style; `number` is the chapter's number, 0 for none.
std::string chapterHeading(const std::string& title, int number, const std::string& numberWord, const std::string& rest, const Style& style);
// The first paragraph of a chapter with its lead-in and drop cap marked (spans), as the style asks.
std::string markOpening(const std::string& bodyHtml, const Style& style);

std::string escape(const std::string& text);   // &, <, >, " as entities

}  // namespace studio::ebook

// Ebook covers made in the Studio: the title, author and an optional picture set in one of a few
// layouts, as a 1600 x 2560 picture (the size ebook stores ask for).
#pragma once

#include <string>
#include <vector>

namespace studio::cover {

struct Spec {
    // "classic" (a coloured cover with a double frame), "picture" (a picture filling it, the words over
    // shaded top and bottom), "band" (the picture above a coloured band with the words), "minimal"
    // (plain paper, a large title and a rule), "arch", "medallion", "sidebar", "stacked", "starfield", "ornate", "modern"
    // (see layouts() for each).
    std::string layout = "classic";
    std::string title, subtitle, author;
    std::string series;                 // a line above the title ("Book Three of The Alvora Chronicles")
    std::string background = "#6B2D3C", ink = "#F4EBDD", accent = "#D9B26B";   // #RRGGBB
    std::string picturePath;            // picture and band layouts ("" = the background colour)
    double focusY = 0.5;                // which part of a picture taller than the space shows: 0 top, 1 bottom
    std::string fontId = "crimson-text";
    bool titleCaps = true;              // the title in capitals
    std::string backText;               // print covers: the blurb on the back
};

struct Layout { const char* id; const char* label; const char* description; };
const std::vector<Layout>& layouts();

// Ready-made colours: background, ink, accent.
struct Colours { const char* id; const char* label; const char* background; const char* ink; const char* accent; };
const std::vector<Colours>& palettes();

struct Result {
    bool ok = false;
    std::string error;
    std::string png;                    // the picture
    int width = 0, height = 0;
};

// Draws the cover. `fontsFolder` holds fonts/<fontId>/*-Regular.ttf and the rest.
Result render(const Spec& spec, const std::string& fontsFolder, int width = 1600, int height = 2560);

// A print cover: back, spine and front in one piece, with bleed all round, for a printer.
struct Wrap {
    double trimW = 6, trimH = 9;   // inches, one page
    double spine = 0.5;            // inches
    double bleed = 0.125;          // inches, each outside edge
    int dpi = 300;
    std::string backText;          // the blurb on the back
    bool backItalic = false;
    bool spineText = true;         // the title and author on the spine, when it's 0.25 in or wider
    bool barcodeArea = true;       // a white space on the back for the printer's ISBN barcode
};
struct WrapResult {
    bool ok = false;
    std::string error;
    std::string pdf, png;          // the PDF (pdf = true), or a PNG to look at
    int width = 0, height = 0;     // pixels
    bool spineText = false;        // the spine had room for its words
};
WrapResult renderWrap(const Spec& spec, const Wrap& wrap, const std::string& fontsFolder, bool pdf);

// A spec as JSON (kept with the book, so the cover can be changed later) and back.
std::string toJson(const Spec& spec);
Spec fromJson(const std::string& json);

}  // namespace studio::cover

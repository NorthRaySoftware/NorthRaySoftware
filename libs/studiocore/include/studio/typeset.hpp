// Typesetting (Milestone 5): from a book's manuscript to finished pages.
//
// HarfBuzz shapes the text (kerning, ligatures, small capitals), Liang's
// patterns hyphenate it, a Knuth–Plass line breaker sets each paragraph as a
// whole, and the page builder lays lines on a baseline grid with chapter
// openings, running heads, folios, pictures, widow and orphan control.
//
// The result is a page display list (Book). The preview (renderPage) and the
// PDF (writePdf) both draw that same list, so they can't disagree.
#pragma once

#include "studio/artwork.hpp"
#include "studio/calendar.hpp"
#include "studio/headers.hpp"
#include "studio/library.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace studio::typeset {

// ------------------------------------------------------------------- design

struct Design {
    std::string trim;                   // trim size id (studio/artwork.hpp); "" = the book's own
    std::string font = "crimson-text";  // a folder under the fonts folder
    double fontSize = 11.5;             // pt
    double leading = 15.0;              // pt, baseline to baseline
    // Margins in inches: inside is the spine side.
    double marginTop = 0.75, marginBottom = 0.85, marginInside = 0.85, marginOutside = 0.6;
    double indentEm = 1.2;              // first-line indent of a paragraph
    bool justify = true;
    bool hyphenate = true;
    bool titlePage = true;              // the book's title, subtitle and author on page 1
    bool copyrightPage = true;          // the back of the title page: copyright line, ISBN, edition (from the book's details)
    bool contents = true;               // a contents page with page numbers
    bool leadIn = true;                 // a chapter's first few words in small capitals
    bool dropCap = false;               // a chapter's first letter large, three lines deep
    // How a chapter opens: "title" (its title), "number" ("CHAPTER ONE" above the title),
    // "numeral" (a large figure, with the title under it).
    std::string chapterStyle = "title";
    bool frontRoman = true;             // front matter numbered i, ii, iii; the story starts at 1 on a right-hand page
    bool chaptersOnRecto = false;       // chapters start on a right-hand page
    bool runningHeads = true;           // book title on left pages, chapter title on right pages
    int chapterSinkLines = 7;           // how far down the page a chapter opens
    // A comic script: Page, Panel, Dialogue, Caption and SFX paragraphs set in the industry layout
    // (PAGE ONE (four panels), Panel 1, numbered balloons), each comic page on a new page.
    bool comicScript = false;
    std::string sceneBreak = "* * *";
    // A small picture for the scene break instead of the mark (0.91): an Artwork picture's blob, set about a fifth of the
    // text's width ("" = the mark above).
    std::string sceneBreakBlob;
    // A chapter's date from Planning's timeline under its heading (0.85), when the book's timeline counts in calendar
    // dates: `dateForm` "words" (4 Bloomtide 2033) or "numbers" (04/04/2033), `dateEra` "abbr", "name" or "none".
    bool chapterDates = false;
    std::string dateForm = "words", dateEra = "abbr";
};

struct Preset {
    const char* id;
    const char* label;
    const char* description;
    Design design;
};
// Ready-made designs (the first is the default).
const std::vector<Preset>& presets();
const Preset& preset(const std::string& id);

struct FontFamilyInfo { std::string id, name; };
// Families found in the fonts folder (each a folder of Regular/Italic/Bold/BoldItalic .ttf files).
std::vector<FontFamilyInfo> fontFamilies(const std::string& fontsFolder);

struct Resources {
    std::string fontsFolder;            // fonts/<family>/<Name>-Regular.ttf ...
    std::string hyphenationFolder;      // hyph-<lang>.pat.txt and .hyp.txt
};

// ------------------------------------------------------------- display list

struct GlyphRun {
    int font = 0;                       // index into Book::fonts
    float size = 0;                     // pt
    float y = 0;                        // baseline, pt from the top of the page
    std::vector<std::uint32_t> glyphs;  // glyph ids in that font
    std::vector<float> xs;              // each glyph's x, pt from the left of the page
    std::vector<float> dy;              // each glyph's vertical offset (marks), pt, up is negative
    bool light = false;                 // set in white (a chapter's title over its darkened header picture, 0.89)
};
struct Rule { float x = 0, y = 0, w = 0, h = 0; };   // a filled rectangle (underlines, strike-through)
struct PicturePlacement {
    std::string blob;
    float x = 0, y = 0, w = 0, h = 0;   // where it prints, pt
    Placement crop;                      // the part of the picture shown
    int tone = 0;                        // 0 as it is, 1 darkened, 2 faded (a header picture with its title over it, 0.89)
    std::string role;                    // "header" or "scenebreak" (0.91, for the print checks); "" = in the text
};

struct Page {
    enum Kind { Title, Blank, PartTitle, ChapterOpening, Text, Copyright, Contents } kind = Text;
    std::string folio;                  // printed page number ("" = none printed)
    std::string chapter;                // the chapter it's in (for the preview's navigation)
    std::vector<GlyphRun> runs;
    std::vector<Rule> rules;
    std::vector<PicturePlacement> pictures;
};

struct FontFile {
    std::string path;                   // the .ttf file
    std::string postscriptName;
    std::map<std::uint32_t, std::string> toUnicode;   // glyph -> UTF-8 text it stands for (copy and search)
};

// What the typesetter needs from the library, copied out, so a book can be set
// on another thread while the library carries on being edited.
struct Source {
    std::string id;                     // the book's id
    std::string title, subtitle, author, trim, language, isbn, edition;
    std::string synopsis, genre;
    // Every edition's ISBN for the copyright page, as (edition, ISBN); empty: just `isbn`.
    std::vector<std::pair<std::string, std::string>> isbns;
    std::string editionKind;            // the edition being made ("paperback", "ebook", ...; "" = none chosen)
    std::string coverPath;              // the cover picture's file ("" = none)
    bool colourPrint = false;           // the edition prints pictures in colour (0.91: the print checks say what goes grey)
    int year = 0;                       // for the copyright line (the year it's typeset)
    struct Section {
        NodeKind kind = NodeKind::Chapter;
        int matter = 0;                 // chapters: 1 front matter, 2 back matter (never numbered)
        std::string title;
        std::string content;            // document JSON
        std::int64_t words = 0;
        // Its day in the book's calendar (0.85): the chapter's own place on the timeline, else its first placed scene's.
        bool dated = false;
        std::int64_t calendarDay = 0;
        // The picture or ornament it opens with (0.89), resolved from its own, its viewpoint's and the book's defaults.
        bool hasHeader = false;
        headers::Header header;
        std::string headerFrom;         // "own", "viewpoint", "chapters" or "parts"
        std::string viewpoint;          // the viewpoint's name, when it came from them
    };
    cal::Calendar calendar;             // the book's calendar, when its timeline counts in calendar dates (0.85)
    std::vector<Section> sections;
    std::map<std::string, std::string> picturePaths;              // blob -> file
    std::map<std::string, std::pair<int, int>> pictureSizes;      // blob -> pixels
};
Source snapshot(const Library& lib, const Id& bookId);

// A chapter title split into its label and the rest: "Chapter 3: The Storm" -> {3, "", "The Storm"};
// "Chapter Three" -> {3, "Three", ""}. Prologues, acknowledgements and the like are `special` (never numbered).
struct ChapterTitle {
    int number = 0;
    std::string word;   // the number as written, when it's a word
    std::string rest;
    bool special = false;
};
ChapterTitle chapterTitleParts(const std::string& title);
std::string spelledNumber(int n);   // 3 -> "Three", 21 -> "Twenty-One"

struct Book {
    double width = 0, height = 0;       // pt
    Design design;
    std::string title, author;
    std::vector<Page> pages;
    std::vector<FontFile> fonts;
    std::map<std::string, std::string> picturePaths;   // blob -> file, for drawing pictures
    std::vector<std::string> warnings;
    std::int64_t words = 0;
    bool ok = false;
    std::string error;
};

// Sets the whole book. Slow parts (shaping, hyphenation) are cached per call.
Book typesetBook(const Source& source, const Design& design, const Resources& res);
inline Book typesetBook(const Library& lib, const Id& bookId, const Design& design, const Resources& res) {
    return typesetBook(snapshot(lib, bookId), design, res);
}
// The design saved with a book (Library::setting "typeset:<book id>"), or the default.
Design designFromJson(const std::string& json);
std::string designToJson(const Design& d);

// --------------------------------------------------------------- rendering

struct Bitmap {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;     // straight RGBA, rows top to bottom
};
// One page as pixels at `dpi`, pictures included (read from the library's objects).
Bitmap renderPage(const Book& book, int pageIndex, double dpi);

// A PDF of the whole book: fonts embedded (the parts used), text searchable and
// copyable, pictures included.
//
// printReady makes it PDF/X-1a:2001, the interior file print-on-demand printers
// ask for: PDF 1.3, text in pure black (DeviceGray), pictures in grey or CMYK,
// no transparency, a TrimBox on every page, an output intent (SWOP, CGATS TR 001)
// and the PDF/X version keys. With bleed, each page is 0.125in larger at the top,
// bottom and outside edge, and the TrimBox marks the finished page.
struct PdfOptions {
    bool printReady = false;
    bool bleed = false;
    bool colourPictures = false;        // CMYK pictures (a colour interior); otherwise grey
};
bool writePdf(const Book& book, const std::string& path, std::string& error, const PdfOptions& options = {});
// The same, as bytes (for tests and checks).
std::string pdfBytes(const Book& book, std::string& error, const PdfOptions& options = {});

// -------------------------------------------------------------- hyphenation

// Liang's algorithm with TeX patterns (hyph-utf8 text format).
class Hyphenator {
public:
    bool load(const std::string& patternsFile, const std::string& exceptionsFile = {});
    bool loaded() const { return !patterns_.empty(); }
    // Positions (in code points) where the word may break: 2 for "hy-phen" means after "hy".
    std::vector<int> points(const std::u32string& word) const;
    int leftMin = 2, rightMin = 3;
private:
    std::map<std::u32string, std::vector<std::uint8_t>> patterns_;
    std::map<std::u32string, std::vector<int>> exceptions_;
    size_t maxLen_ = 0;
};

// --------------------------------------------------------------- utilities

std::u32string utf32(const std::string& utf8);
std::string utf8(const std::u32string& s);

}  // namespace studio::typeset

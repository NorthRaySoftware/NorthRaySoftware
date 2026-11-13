// Publishing (Milestone 7): the files a book goes out as, and the checks before
// it does.
//
//   EPUB 3 (with an EPUB 2 table of contents for older readers), from the same
//   chapter HTML the ebook preview shows, pictures cropped ahead of time.
//   Print-ready PDF/X-1a: typeset::writePdf with PdfOptions::printReady.
//   A pre-flight list: what's missing or likely to be refused, and the spine
//   width for the cover.
#pragma once

#include "studio/typeset.hpp"

#include <string>
#include <vector>

namespace studio::publish {

// ------------------------------------------------------------------- EPUB

struct EpubOptions {
    int maxPicturePixels = 2400;        // a picture's longer side, at most (larger ones are scaled down)
    std::string modified;               // "2026-11-13T10:00:00Z"; empty = now
};

struct EpubResult {
    bool ok = false;
    std::string error;
    std::string bytes;                  // the .epub file
    int chapters = 0, pictures = 0;
    std::vector<std::string> notes;     // things worth knowing (a picture that couldn't be read, ...)
};

EpubResult buildEpub(const typeset::Source& src, const EpubOptions& options = {});
EpubResult writeEpub(const typeset::Source& src, const std::string& path, const EpubOptions& options = {});

// ---------------------------------------------------------------- editions

// What an edition is made from: the book's snapshot with this edition's ISBN,
// trim and kind, and every edition's ISBN for the copyright page.
typeset::Source editionSource(const Library& lib, const Edition& edition);
// The print design for an edition: the book's Typesetting design, or the
// edition's own preset, with the edition's trim.
typeset::Design editionDesign(const Library& lib, const Edition& edition);
std::string editionLabel(const Edition& edition);   // its name, or "paperback", "large print", ...

// --------------------------------------------------------------- pre-flight

struct Check {
    enum Level { Ok, Advice, Problem } level = Ok;
    std::string area;                   // "Details", "Pictures", "Print", "Ebook"
    std::string title;                  // one line
    std::string detail;                 // what to do about it
};

// Checks the book's details and manuscript, and (when given) the typeset pages.
// With src.editionKind set, only what matters for that edition (an ebook gets no
// print checks; a print edition no ebook cover checks).
std::vector<Check> preflight(const typeset::Source& src, const typeset::Book* printed);

// ISBN-10 or ISBN-13 (hyphens and spaces allowed), with a correct check digit.
bool validIsbn(const std::string& isbn);
std::string isbnDigits(const std::string& isbn);   // just the digits (and a final X)

// The spine's width for a paperback, from the page count and paper: an
// estimate for the cover designer (the printer's own cover template is final).
enum class Paper { White, Cream };
double spineInches(int pages, Paper paper);

}  // namespace studio::publish

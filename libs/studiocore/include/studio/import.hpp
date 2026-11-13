// Importing books into the Studio: EPUB, PDF, HTML, Markdown and plain text.
//
// Importing happens in two steps so nothing touches the library until the
// author has seen what was found:
//   1. read*() turns a file into a Plan: metadata, the cover, and a list of
//      sections (parts, chapters, front and back matter) with word counts, a
//      suggested include/exclude for each, and plain-language warnings.
//   2. apply() adds the included sections to a book in one transaction, stores
//      images by content, and (if asked) fills in the book's details and cover.
//
// Safety: ZIP entries are untrusted (size, count and ratio limits; nested
// archives and encrypted entries refused); HTML is converted into the Studio's
// own document model, so no markup, scripts or styles are carried across.
// DRM-protected books (Adobe ADEPT, Apple FairPlay, Readium LCP, Kindle) and
// copy-restricted or password-protected PDFs are refused with an explanation.
#pragma once

#include "studio/library.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace studio::import {

enum class Format { Epub, Pdf, Html, Markdown, Text, Docx, Odt, Unknown };
const char* label(Format f);

// What a section seems to be. Cover pages, tables of contents, title and
// copyright pages are left out by default: the Studio makes its own when the
// book is published.
enum class Role {
    Chapter, Part, Cover, TitlePage, Copyright, Contents, Dedication, Epigraph, Foreword, Preface, Prologue,
    Epilogue, Afterword, Acknowledgements, AboutAuthor, AlsoBy, Appendix, Notes, Other
};
const char* label(Role r);
bool includedByDefault(Role r);

struct Section {
    std::string title;
    Role role = Role::Chapter;
    int level = 0;               // 0: top level; 1: inside the part above it
    bool include = true;
    std::string content;         // document JSON (studio/document.hpp)
    std::int64_t words = 0;
    std::string source;          // where it came from, e.g. "OEBPS/ch03.xhtml" or "pages 12-19"
};

struct Metadata {
    std::string title, subtitle, author, language, description, isbn, publisher;
    std::string coverBytes;      // image file contents, if the source has a cover
    std::string coverType;       // "image/jpeg", "image/png"
};

// A part's own page is kept (as an opening chapter) only when it has more
// text than this; otherwise it's just the part's title.
constexpr std::int64_t kPartPageWords = 40;

struct Plan {
    bool ok = false;
    std::string error;           // why it couldn't be imported (ok == false)
    Format format = Format::Unknown;
    std::string sourceName;      // file name only
    Metadata meta;
    std::vector<Section> sections;
    std::vector<std::string> warnings;   // plain language, shown in the preview
    std::map<std::string, std::string> images;  // SHA-256 -> bytes, for images the sections use
    bool approximate = false;    // structure was guessed (PDF, plain text): check it before importing

    std::int64_t includedWords() const;
    int includedCount() const;
};

struct Limits {
    std::uint64_t maxFileBytes = 512ull << 20;     // 512 MB
    std::uint64_t maxEntryBytes = 64ull << 20;     // one file inside an EPUB
    std::uint64_t maxTotalBytes = 768ull << 20;    // everything unpacked
    int maxEntries = 20000;
    int maxRatio = 200;                            // unpacked:packed, for entries over 1 MB
};

Format detect(const std::string& bytes, const std::string& fileName);
// Reads a file from disk (UTF-8 path) and dispatches on its contents.
Plan readFile(const std::string& path, const Limits& limits = {});
Plan readEpub(const std::string& bytes, const std::string& name, const Limits& limits = {});
Plan readHtml(const std::string& html, const std::string& name);
Plan readDocx(const std::string& bytes, const std::string& name, const Limits& limits = {});   // Word (.docx)
Plan readOdt(const std::string& bytes, const std::string& name, const Limits& limits = {});    // OpenDocument (.odt)
Plan readText(const std::string& text, const std::string& name, bool markdown);
Plan readPdf(const std::string& path, const std::string& name);  // needs PDFium (see pdfiumAvailable)

// PDF import uses PDFium, loaded at run time from next to the program (or the
// given folder). Returns false (with a reason) if it can't be found.
bool pdfiumAvailable(std::string* why = nullptr);
void setPdfiumFolder(const std::string& folder);

// Lines of plain text -> sections, with chapter headings and scene breaks
// detected. Shared by text, Markdown and PDF import.
struct Line {
    std::string text;
    bool heading = false;        // known to be a heading (e.g. larger type in a PDF, "#" in Markdown)
    int headingLevel = 0;
    bool startsParagraph = false;
    bool italic = false;
    std::vector<std::pair<std::string, unsigned>> runs;  // optional styled pieces (doc::Builder marks); else `text`
    std::string source;
};

struct ApplyOptions {
    bool setMetadata = true;     // fill in empty book fields (title, author, synopsis, ...)
    bool setCover = true;        // use the source's cover if the book has none
    Id parentId;                 // where to add (empty: end of the book)
};
// Hands a plan between processes: the Studio reads files in a separate helper
// process, so a file that makes a reader crash can't take the app down with it.
// Images and the cover are written as files next to plan.json.
bool savePlan(const Plan& plan, const std::string& folder, std::string& error);
Plan loadPlan(const std::string& folder);
// readFile, but never throws: anything unexpected becomes plan.error.
Plan readFileSafely(const std::string& path, const Limits& limits = {});

// Returns the ids of the top-level nodes added; empty on failure (library.lastError()).
std::vector<Id> apply(Library& lib, const Id& bookId, const Plan& plan, const ApplyOptions& opt = {});

}  // namespace studio::import

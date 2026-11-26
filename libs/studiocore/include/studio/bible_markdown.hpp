// Writing Studio: the bible as Markdown that Import many pages reads back exactly (0.95). Portable and tested.
//
// The file starts with a marker line (kExportMark). Kinds are "# Characters" headings, entries "## Name", their parts
// "### Summary", "### Details", "### Section: Appearance", "### Notes per book" (a "#### Book title" each), "### Family",
// "### Relationships", "### Notes", "### Spoilers", "### Gallery" and "### Map pins"; fields under the entry's heading
// ("Kind:", "Other name:", one a line, "How it's said:", "Symbol:", "Picture:", "Header picture:", "Map:"). Text is
// kept as written, a line at a time: a line starting with # or \ has a \ put before it, taken off again on import.
// Parts the app works out (Appears in, ages, excerpts) can be added for reading, under headings marked "(generated:
// not imported)", which the import skips.
//
// Imported, a page sets its entry to exactly what it says: other names, how it's said, symbol, summary, details,
// sections (in order), notes per book, notes, spoilers and the relationships it starts (absent parts are empty).
// Pictures, the map and its pins change only when the page has them. So exporting and importing the same file
// changes nothing.
#pragma once

#include "studio/entry_paste.hpp"
#include "studio/library.hpp"

#include <string>
#include <utility>
#include <vector>

namespace studio::paste {

constexpr const char* kExportMark = "<!-- NorthRay Writing Studio bible export";

struct MarkdownOptions {
    std::vector<std::string> kinds;     // only these kinds ("" = every kind)
    std::vector<Id> entries;            // only these entries (empty = all of the kinds chosen)
    bool generated = false;             // add Appears in, ages and excerpts, for reading (not imported)
    std::string picturesFolder = "pictures";   // where pictures go, beside the file ("" = no pictures)
};
struct MarkdownExport {
    std::string text;
    std::vector<std::pair<std::string, std::string>> pictures;   // blob -> path relative to the file, to be copied there
    int entries = 0;
};
MarkdownExport exportMarkdown(const Library& lib, const Id& world, const Id& book, const MarkdownOptions& options = {});

// Whether text is such an export (its first line that isn't blank is the marker).
bool isMarkdownExport(const std::string& text);
// The export's pages, one Batch each (`exact` set), for importPages and previewImport.
std::vector<Batch> exportPages(const std::string& text);
// Used by importPages for exact pages.
bool importExact(Library& lib, const Id& world, const Id& book, const std::vector<Batch>& pages, const std::vector<Id>& booksInOrder,
                 Import& out, const std::string& baseFolder);

}  // namespace studio::paste

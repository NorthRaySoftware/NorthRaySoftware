// Writing Studio: a whole bible page pasted in one go (0.37). Portable and tested.
//
// A character sheet written elsewhere (a document, a wiki, a chat) is pasted onto an entry, and each part goes where
// it belongs: "Name:", "Other names:" and "How it's said:" to those fields; "Summary" to the summary; the lines under
// "Details" (Hair: brown) to the details; headings like "Appearance" or "History" to the page's sections (into a
// template section whose title has that word, "Appearance & personality", else a section of their own); "Notes per
// book" to each book's notes (matched by title, or "Book 2" by reading order); "Family" and "Relationships" lines to
// links with entries already in the bible ("Parents: Ralph Smith" -> Child of Ralph Smith), and the rest to the
// details; "Notes" and "Spoilers" to the private notes. What's pasted replaces what was there (0.38.1).
//
// Headings can be Markdown (## Summary, **Summary**), a line ending in a colon, or a known word on its own line.
// Anything before the first part recognised (a chat's "Here's the entry…") is left out.
#pragma once

#include "studio/library.hpp"

#include <string>
#include <utility>
#include <vector>

namespace studio::paste {

struct Page {
    std::string name, pronunciation, summary, details, notes;
    std::vector<std::string> aliases;
    std::vector<std::pair<std::string, std::string>> sections;    // heading -> text, in order
    std::vector<std::pair<std::string, std::string>> bookNotes;   // "Shards of Light (Book 1)" -> text
    std::vector<std::pair<std::string, std::string>> relations;   // "Parents" -> "Ralph Smith, Maria Susan Smith"
    int parts = 0;                                                // how many parts were recognised
    int known = 0;                                                // of them, parts with a known name (Name, Summary, Details…)
};

// Reads pasted text. Markdown emphasis and list markers are taken off; list items in longer text become "• " lines.
Page parse(const std::string& text);

// Whether text looks like a page rather than a sentence, a list or a few paragraphs with headings: two or more parts
// recognised, one of them with a known name (Name:, Other names:, Summary, Details, Family, Relationships…).
bool looksLikePage(const std::string& text);

struct Report {
    bool name = false, summary = false, pronunciation = false;
    int aliases = 0, details = 0, sections = 0, bookNotes = 0, links = 0, notes = 0;
    std::vector<std::string> unmatchedBooks;   // labels kept in the notes instead
    std::string words() const;                 // "the name, 3 other names, the summary, 18 details, …"
};

// Puts a parsed page onto an entry. Each part the paste has replaces that part of the entry: the name, other names,
// how it's said, summary, details (all of them), each section and each book's notes it names, the notes, and the
// relationships of each kind it lists (old links of that kind go). Parts the paste doesn't have are left as they were.
// `booksInOrder` is the world's books in reading order, for "Book 2". False with the library's error if the entry
// couldn't be saved.
bool apply(Library& lib, const Id& entryId, const Page& page, const std::vector<Id>& booksInOrder, Report& report);

// The relationship a pasted label stands for, read from an entry of `kind` ("Parents" -> "Child of"); "" if none.
std::string relationFor(const std::string& kind, const std::string& label);

}  // namespace studio::paste

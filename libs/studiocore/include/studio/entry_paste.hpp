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

// A blank page to fill in and paste back, for an entry of `kind`: every part the paste understands, the kind's
// sections (`sections`, the world's template titles), a line for each book's notes, and suggested detail, family and
// relationship labels. Lines left blank are ignored when it's pasted.
std::string blankPage(const std::string& kind, const std::vector<std::string>& sections, const std::vector<std::string>& bookTitles);

// Whether text looks like a page rather than a sentence, a list or a few paragraphs with headings: two or more parts
// recognised, one of them with a known name (Name:, Other names:, Summary, Details, Family, Relationships…).
bool looksLikePage(const std::string& text);

struct Report {
    bool name = false, summary = false, pronunciation = false;
    int aliases = 0, details = 0, sections = 0, bookNotes = 0, links = 0, notes = 0;
    std::vector<std::string> unmatchedBooks;   // labels kept in the notes instead
    std::vector<Id> created;                   // entries made for names in Family or Relationships not in the bible yet
    std::string words() const;                 // "the name, 3 other names, the summary, 18 details, …"
};

// Puts a parsed page onto an entry. Each part the paste has replaces that part of the entry: the name, other names,
// how it's said, summary, details (all of them), each section and each book's notes it names, the notes, and the
// relationships of each kind it lists (old links of that kind go). A name in Family or Relationships that isn't in the
// bible becomes a new entry of the kind the relationship points at (Lives in: a place), linked. Parts the paste doesn't
// have are left as they were.
// `booksInOrder` is the world's books in reading order, for "Book 2". False with the library's error if the entry
// couldn't be saved.
bool apply(Library& lib, const Id& entryId, const Page& page, const std::vector<Id>& booksInOrder, Report& report);

// The relationship a pasted label stands for, read from an entry of `kind` ("Parents" -> "Child of"); "" if none.
std::string relationFor(const std::string& kind, const std::string& label);

// ---------------------------------------------------------------- many pages at once (0.39)
//
// A file or a paste holding a whole bible, or a good part of one: pages one after another. Each page starts at a
// "Name:" line, or at a Markdown title ("# Tom Smith"). What kind the pages are comes from a heading naming a kind
// ("# Characters", "**Places**", "== Creatures ==", "Realms:"), from a "Kind: Place" line in the page, or from the
// kind chosen for the import. Text before the first page is left out, and so are lines between pages that are only
// rules (---, ===).

// The kind a word names ("Characters", "people", "Magic systems" -> "character", "character", "magic"); "" if none.
std::string kindNamed(const std::string& word);

struct Batch {
    std::string kind;    // the entry kind
    std::string name;    // as the page gives it
    std::string text;    // the page, as paste::parse reads it
};

// The pages in text. Pages without a name are left out.
std::vector<Batch> splitPages(const std::string& text, const std::string& defaultKind);

// A Word or OpenDocument file's sections (as the importer splits it: title and document JSON) as text for splitPages,
// headings kept as Markdown. The importer's own title for text before the first heading ("Front matter") is left out.
std::string documentPagesText(const std::vector<std::pair<std::string, std::string>>& sections);

// Whether text holds more than one page.
bool looksLikeManyPages(const std::string& text);

struct Import {
    std::vector<Id> made;                         // entries made for the pages
    std::vector<Id> updated;                      // entries the pages replaced parts of
    std::vector<Entry> before;                    // each updated entry as it was (same order)
    std::vector<std::vector<EntryLink>> linksBefore;
    std::vector<Id> extra;                        // entries made for names on the pages that are no page of their own
    int links = 0;
    int unchanged = 0;                            // pages already there as they are
    std::string words() const;                    // "12 new entries, 3 updated, 40 links"
};

// Brings pages into a world's bible (or an independent book's: `world` empty). Each page goes onto the entry of
// its name and kind (else any kind of that name), or a new one. All the new entries are made first, so pages
// naming each other link up. False with the library's error if one couldn't be saved; what was done is in `out`.
// `replaceAll` (0.46): an entry already in the bible has all its text cleared first (see clearEntryText), so it ends up
// with only what its page says; its pictures stay.
bool importPages(Library& lib, const Id& world, const Id& book, const std::vector<Batch>& pages, const std::vector<Id>& booksInOrder,
                 Import& out, bool replaceAll = false);
// Clears everything written on an entry (0.46): other names, how it's said, summary, details, sections, notes per book,
// notes, spoilers, and every relationship and family link it has. Its name, kind, picture, gallery and map stay.
bool clearEntryText(Library& lib, const Id& entryId);

}  // namespace studio::paste

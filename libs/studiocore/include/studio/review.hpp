// Editorial tools (Milestone 9): comparing two texts, and checking a book's
// names against its world's bible.
#pragma once

#include "studio/library.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace studio::review {

// ------------------------------------------------------------- comparing

struct DiffPart {
    enum Kind { Same, Added, Removed } kind = Same;
    std::string text;
};

// Plain text before and after, compared paragraph by paragraph and, within a
// changed paragraph, word by word. Paragraph breaks are kept as "\n\n" in the parts.
std::vector<DiffPart> diffText(const std::string& before, const std::string& after);

struct DiffStats {
    std::int64_t added = 0, removed = 0;   // words
    bool changed() const { return added || removed; }
};
DiffStats stats(const std::vector<DiffPart>& parts);

// --------------------------------------------------------------- names

struct NameIssue {
    enum Kind {
        Variant,    // looks like a misspelling of an entry's name ("Marren" for "Maren")
        Unknown,    // used often, but not in the world's bible
    } kind = Unknown;
    std::string name;
    int count = 0;
    std::string like;                 // Variant: the name it looks like
    Id entryId;                       // Variant: that entry
    std::vector<Id> nodes;            // where it appears, in order
};

// Names (words capitalised in the middle of a sentence, and never written in
// lower case) across a book's chapters and scenes, checked against the
// entries of its world or of the independent book.
std::vector<NameIssue> nameChecks(const Library& lib, const Id& bookId);

int editDistance(const std::u32string& a, const std::u32string& b, int limit);

}  // namespace studio::review

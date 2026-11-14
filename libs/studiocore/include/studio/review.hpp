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

// Changes every whole-word use of `from` to `to` in a book's chapters and scenes (exact case;
// "Stienriket's" too). Each changed chapter keeps its old text as a saved version first.
// Returns how many uses were changed; `nodes` gets the chapters that changed.
int replaceName(Library& lib, const Id& bookId, const std::string& from, const std::string& to, std::vector<Id>* nodes = nullptr);
// The same, in one piece of text (for tests and the editor's open chapter).
std::string replaceWholeWord(const std::string& text, const std::string& from, const std::string& to, int& count);

// ------------------------------------------------------------- review copies

// The book as one HTML file for a beta reader: `templateHtml` (web/review-copy.html) with the
// chapters, title and a fresh copy id filled in. Pictures go inside the file. "" with `error` set.
std::string makeReviewCopy(const Library& lib, const Id& bookId, const std::string& templateHtml, std::string& error);

struct ImportReport {
    std::string reader;
    int placed = 0;       // on the words they were about
    int unplaced = 0;     // on a whole chapter, or on words changed since
    int skipped = 0;      // chapters since deleted, empty comments
    std::vector<Id> nodes;   // the chapters that got comments
    std::string error;
};
// A reader's comments file (.nrcomments): each comment becomes a Studio comment from them, marked
// on the words it was about (found by the words and what's around them).
ImportReport importReviewComments(Library& lib, const Id& bookId, const std::string& fileText);

// Marks `quote` (whitespace-insensitive; the occurrence that best matches `prefix`/`suffix`) in a
// document with a comment mark `id`. False if it isn't there.
bool markQuote(std::string& docJson, const std::string& quote, const std::string& prefix, const std::string& suffix, const std::string& id);

}  // namespace studio::review

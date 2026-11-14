// Sharing a world bible: as a website (a folder of linked pages to put online or open from disk),
// and as a printable book (typeset like a manuscript: a part for each kind, a chapter for each entry).
// Either can be the author's whole bible, or a reader's guide that keeps to what readers know by the
// end of a chosen book (facts marked as revealed later are left out) and leaves out the author's notes.
#pragma once

#include "studio/library.hpp"
#include "studio/typeset.hpp"

#include <string>
#include <vector>

namespace studio::bible {

struct Options {
    Id worldId, bookId;          // the bible: a world's, or an independent book's
    bool readersGuide = false;   // no notes or notes per book; only what's revealed by `upTo`
    Id upTo;                     // a reader's guide: the last book read ("" = every book)
};

// The books of the bible's world (or the independent book) in reading order: each series in its
// reading order, then books in no series.
std::vector<Id> booksInOrder(const Library& lib, const Id& worldId, const Id& bookId);

// What a shared bible shows.
class View {
public:
    View(const Library& lib, const Options& options);
    const std::vector<const Entry*>& entries() const { return entries_; }   // by name
    bool shows(const Entry& e, const std::string& key) const;              // a fact ("detail:Age", "section:...", "summary")
    bool shows(const Id& entryId) const;
    const Options& options() const { return options_; }
    std::string title() const;                                             // "Alvora"

private:
    const Library& lib_;
    Options options_;
    std::vector<Id> books_;
    int upToIndex_ = -1;
    std::vector<const Entry*> entries_;
    bool revealedInTime(const Id& book) const;
};

// A website in `folder` (made if missing): index.html, glossary.html, a page for each entry, style.css
// and the pictures in images/. Returns false with `error` set when it can't be written.
bool exportWebsite(const Library& lib, const Options& options, const std::string& folder, std::string& error, int* pages = nullptr);

// A printable bible for the typesetter: a part for each kind, a chapter for each entry, a glossary at
// the end. `title` heads it ("The Alvora Bible").
typeset::Source printableSource(const Library& lib, const Options& options, const std::string& title, const std::string& author);

// "png", "jpg", "gif", "webp" or "" from a picture's first bytes; and its size in pixels (0 if unknown).
std::string pictureType(const std::string& bytes);
std::pair<int, int> pictureSize(const std::string& bytes);

}  // namespace studio::bible

// Chapter header pictures (0.89): a picture or ornament a chapter or part opens with, in the typeset book, the ebook,
// the review copy and the shared bible's plans.
//
// Kept per book in the setting "headers:<book>": the book's default for chapters, its default for parts, and each
// chapter's or part's own choice. A viewpoint character can have a header picture of their own (Entry::headerBlob),
// else their symbol prints as an ornament, so a chapter seen through their eyes opens with it unless it chose another.
#pragma once

#include "studio/library.hpp"

#include <map>
#include <optional>
#include <string>

namespace studio::headers {

struct Header {
    std::string blob;              // a picture from Artwork
    std::string symbol;            // or an ornament: a character or emoji ("✧", "🗡")
    std::string size = "band";     // "full" (the text's width, as tall as the picture is), "band" (a wide strip), "ornament" (small)
    std::string title = "below";   // where the title sits: "above" the picture, "below" it, or "over" it
    std::string tone = "dark";     // a title over the picture: "dark" (a light title, the picture darkened) or "fade" (ink, the picture faded)
    std::string alt;               // the picture described, for the ebook's readers
    bool none = false;             // chosen: no header (a front-matter page, or a chapter opting out of the default)
    bool empty() const { return blob.empty() && symbol.empty(); }
};

struct BookHeaders {
    Header chapters;               // every chapter that hasn't chosen its own (and whose viewpoint has none)
    Header parts;                  // every part that hasn't chosen its own
    std::map<Id, Header> nodes;    // a chapter's or part's own
};

std::string headerToJson(const Header& h);   // a JSON object
Header headerFromJson(const std::string& json);
std::string toJson(const BookHeaders& b);
BookHeaders fromJson(const std::string& json);
BookHeaders read(const Library& lib, const Id& book);
void write(Library& lib, const Id& book, const BookHeaders& b);

// A symbol that prints: not one of the app's drawn symbols ("icon:paw").
bool printableSymbol(const std::string& symbol);

// What a chapter or part opens with, and where that came from: "own", "viewpoint", "chapters" (the book's default
// for chapters) or "parts". Front and back matter take only their own. A chapter chosen as "none" has nothing.
struct Resolved {
    Header header;
    std::string from;
    Id viewpoint;                  // when from the viewpoint
};
std::optional<Resolved> resolve(const Library& lib, const ManuscriptNode& node, const BookHeaders& b);

// Every picture a book's headers use (own, defaults and its chapters' viewpoints'), for EPUBs and checks.
std::vector<std::string> blobsUsed(const Library& lib, const Id& book);

}  // namespace studio::headers

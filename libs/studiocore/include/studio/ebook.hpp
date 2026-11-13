// Ebooks: a book's manuscript as reflowable HTML, the way e-readers show it.
// The preview (Milestone 6) shows it on simulated devices; the EPUB export
// (Milestone 7) packages the same chapters.
#pragma once

#include "studio/typeset.hpp"

#include <functional>
#include <string>
#include <vector>

namespace studio::ebook {

struct Chapter {
    std::string title;
    bool part = false;          // a part title page
    std::string html;           // the chapter's body: <section> ... </section>
    std::int64_t words = 0;
};

struct Content {
    std::string title, subtitle, author, language;
    std::vector<Chapter> chapters;
};

// Where a picture is found, as a URL for the HTML (the preview's image host, or
// a file inside the EPUB).
using PictureUrl = std::function<std::string(const std::string& blob)>;

// The same, for a picture cropped ahead of time (the EPUB): the URL is of the
// picture as it should be shown, so the HTML carries no crop.
using CroppedPictureUrl = std::function<std::string(const std::string& blob, const Placement& crop)>;

// Each chapter as semantic HTML: h1 for its title, p with paragraph-style
// classes, em/strong/span.sc marks, hr.scene for scene breaks, figure for
// pictures (crop kept as data for the reader's stylesheet).
Content build(const typeset::Source& src, const PictureUrl& pictureUrl);
Content buildCropped(const typeset::Source& src, const CroppedPictureUrl& pictureUrl);

// One document's body (document JSON) as HTML, without the chapter title.
std::string documentHtml(const std::string& docJson, const PictureUrl& pictureUrl);

// The reading stylesheet shared by the preview and the EPUB.
std::string stylesheet();

std::string escape(const std::string& text);   // &, <, >, " as entities

}  // namespace studio::ebook

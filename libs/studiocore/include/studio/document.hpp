// The Studio's manuscript document format.
//
// A chapter's or scene's text is stored as JSON in the shape ProseMirror uses
// (the editor works on it directly), restricted to the Studio's own schema:
//
//   doc            block+
//   paragraph      attrs.style: one of kParagraphStyles      inline*
//   heading        attrs.level: 1..3                          inline*
//   blockquote     block+
//   bullet_list / ordered_list  list_item+   (ordered_list attrs.start)
//   list_item      block+
//   scene_break    (no content: the "* * *" between scenes)
//   image          attrs.blob (SHA-256 in objects/), alt, caption; optional placement:
//                  cropX, cropY, cropW, cropH (0..1), width (of the text width, 0.1..1), align (left/right)
//   text           text, marks[]
//   hard_break
//   marks: em, strong, underline, smallcaps, strike, sup, sub, code, link (attrs.href)
//
// Styles are semantic ("Body", "Epigraph"): the look comes from the book's
// design, so changing a style restyles the whole book.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace studio::doc {

struct StyleInfo { const char* id; const char* label; };
// body, first (no indent), centered, epigraph, attribution, verse, letter, note
extern const std::vector<StyleInfo> kParagraphStyles;

std::string emptyDoc();
// Keeps only what the schema allows; anything else is unwrapped to its text or
// dropped, and described in `problems`. Returns a valid document ("" input -> empty doc).
std::string sanitise(const std::string& docJson, std::vector<std::string>* problems = nullptr);
std::int64_t wordCount(const std::string& docJson);
std::string plainText(const std::string& docJson);   // paragraphs separated by blank lines
// Words in a piece of plain UTF-8 text (letters and digits, joined by ' and -).
std::int64_t countWords(const std::string& text);

// Builds a document block by block (used by the importers).
class Builder {
public:
    enum Mark : unsigned { Em = 1, Strong = 2, Underline = 4, SmallCaps = 8, Strike = 16, Sup = 32, Sub = 64, Code = 128 };

    void paragraph(const std::string& style = "body");   // starts a new paragraph
    void heading(int level);
    void text(const std::string& utf8, unsigned marks = 0, const std::string& href = {});
    void hardBreak();
    void sceneBreak();
    void image(const std::string& blob, const std::string& alt, const std::string& caption = {});
    void beginQuote();
    void endQuote();
    void beginList(bool ordered);
    void listItem();
    void endList();
    void endBlock();             // closes the current paragraph/heading

    bool empty() const;          // no text, images or breaks yet
    std::int64_t words() const;
    std::string toJson() const;  // a complete, valid document
    std::string firstHeadingText() const;

    Builder();
    ~Builder();
    Builder(Builder&&) noexcept;
    Builder& operator=(Builder&&) noexcept;

private:
    struct Impl;
    Impl* d_;
};

}  // namespace studio::doc

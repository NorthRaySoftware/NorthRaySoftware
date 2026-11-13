// Internal: a tolerant HTML/XML tokenizer, a small CSS reader, and the
// HTML -> Studio document converter used by the importers.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace studio::markup {

struct Token {
    enum Kind { Start, End, Text } kind = Text;
    std::string name;            // lower-case, with any prefix ("epub:type", "dc:title")
    std::vector<std::pair<std::string, std::string>> attrs;  // names lower-case; values entity-decoded
    bool selfClosing = false;
    std::string text;            // entity-decoded (Text tokens)
    std::string attr(const std::string& n) const;           // "" when missing; also matches a prefixed name's local part
    std::string local() const;   // name without prefix
};

std::vector<Token> tokenize(const std::string& src);
std::string decodeEntities(const std::string& s);
std::string toUtf8(const std::string& bytes);              // handles UTF-16 BOMs; UTF-8 passes through
std::string percentDecode(const std::string& s);
// Resolves `href` (relative, may contain ../ and #fragment) against the
// directory of `baseFile`; the fragment is returned separately.
std::string resolve(const std::string& baseFile, const std::string& href, std::string* fragment = nullptr);
std::string collapseSpace(const std::string& s);           // runs of whitespace -> one space, trimmed
std::string textOf(const std::vector<Token>& t, size_t from, size_t to);

struct CssFlags {
    int italic = -1, bold = -1, smallcaps = -1, center = -1, noindent = -1, hidden = -1, right = -1;
};
std::map<std::string, CssFlags> parseCss(const std::string& css);   // keyed by class name
CssFlags parseDeclarations(const std::string& decls);

struct HtmlContext {
    std::string filePath;                                  // inside the archive, for resolving links
    std::map<std::string, CssFlags> classes;
    std::set<std::string> splitIds;                        // element ids that start a new section
    int splitHeadingLevel = 0;                             // also split before every hN (0: don't)
    // Returns the stored image's SHA-256 for a resolved path, or "" if it can't be read.
    std::function<std::string(const std::string& path)> loadImage;
};

struct HtmlPart {
    std::string anchorId;                                  // the split id that started it ("" for the first)
    std::string json;                                      // document JSON
    std::int64_t words = 0;
    bool empty = true;                                     // no text and no images
    std::string epubType;                                  // from body/section epub:type
    std::vector<std::string> headings;                     // heading texts in order
    std::string bodyClass;                                 // body/section class names
};

std::vector<HtmlPart> convertHtml(const std::string& html, const HtmlContext& ctx, std::vector<std::string>& warnings);
bool isSceneBreakText(const std::string& text);

}  // namespace studio::markup

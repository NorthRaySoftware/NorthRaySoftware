// NorthRay Files: what a search asks for, and whether a name matches it. Portable, so the rules are
// unit-tested on every platform; the Windows layer walks folders or asks the Windows Search index.
//
// What can be typed:
//   cover sketch       names containing both words (any case, any order)
//   "dragon map"       the words together
//   *.png  ch?pter*    wildcards (* any run of characters, ? one character) against the whole name
//   kind:picture       picture, video, music, document, archive, folder (also type:)
//   ext:psd            an extension (also .psd on its own)
#pragma once

#include <string>
#include <vector>

namespace nrf::search {

enum class Kind { Any, Folder, Picture, Video, Music, Document, Archive };

struct Query {
    std::vector<std::wstring> words;      // lower case; each must be in the name
    std::vector<std::wstring> patterns;   // lower case wildcards; each must match the whole name
    std::vector<std::wstring> exts;       // ".psd"; the name must have one of them
    Kind kind = Kind::Any;
    bool empty() const { return words.empty() && patterns.empty() && exts.empty() && kind == Kind::Any; }
};

Query parse(const std::wstring& text);

// Wildcards, ignoring case: * any run of characters (or none), ? exactly one.
bool wildcardMatch(const std::wstring& pattern, const std::wstring& name);

// What sort of file an extension is (".jpg" -> Picture); Any when it isn't one of the kinds.
Kind kindOf(const std::wstring& ext);

bool matches(const Query& q, const std::wstring& name, bool folder);

// The Windows Search query (SQL for the SystemIndex) for `q`, within `scope` (a folder, and what's in
// it at any depth) or everywhere indexed when `scope` is empty; at most `limit` results.
std::wstring indexSql(const Query& q, const std::wstring& scope, int limit);

// "Sketches" -> the part of `path` below `root` ("Sketches\Old" for root\Sketches\Old\x.png's folder);
// "" when it's `root` itself.
std::wstring relativeFolder(const std::wstring& root, const std::wstring& folder);

}  // namespace nrf::search

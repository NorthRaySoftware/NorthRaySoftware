// Whole-word matching in UTF-8 text: where a name starts and ends. Letters and digits in any script
// continue a word; spaces and punctuation don't, including the typographer's ones written as several
// bytes (curly quotes and apostrophes, dashes, the ellipsis, non-breaking spaces, guillemets), so
// "Marren’s", “Marren” and "Marren—" all hold the name.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace studio::words {

bool wordCodepoint(std::uint32_t cp);
std::uint32_t codepointBefore(const std::string& text, std::size_t i);   // the character ending just before byte i
std::uint32_t codepointAt(const std::string& text, std::size_t i);       // the character starting at byte i
// Whether text[at, at+len) stands as a whole word.
inline bool wholeWord(const std::string& text, std::size_t at, std::size_t len) {
    const std::size_t end = at + len;
    return (at == 0 || !wordCodepoint(codepointBefore(text, at))) && (end >= text.size() || !wordCodepoint(codepointAt(text, end)));
}

}  // namespace studio::words

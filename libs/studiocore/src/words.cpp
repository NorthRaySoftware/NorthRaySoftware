#include "studio/words.hpp"

#include <cctype>

namespace studio::words {

// Whether a character continues a word: letters and digits in any script do; spaces and punctuation
// don't, including the typographer's ones written as several bytes in UTF-8 (curly quotes and
// apostrophes, dashes, the ellipsis, non-breaking spaces, guillemets), so "Marren’s", “Marren” and
// "Marren—" are all the name.
bool wordCodepoint(std::uint32_t cp) {
    if (cp < 0x80) return std::isalnum((int)cp) || cp == '_';
    if (cp >= 0x80 && cp <= 0xBF) return cp == 0xAA || cp == 0xB2 || cp == 0xB3 || cp == 0xB5 || cp == 0xB9 || cp == 0xBA;   // Latin-1 signs
    if (cp == 0xD7 || cp == 0xF7) return false;                  // × ÷
    if (cp >= 0x2000 && cp <= 0x206F) return false;              // general punctuation: spaces, dashes, quotes, …
    if (cp >= 0x2E00 && cp <= 0x2E7F) return false;              // supplemental punctuation
    if (cp >= 0x3000 && cp <= 0x303F) return false;              // CJK punctuation
    if (cp >= 0xFE30 && cp <= 0xFE4F) return false;
    if (cp >= 0xFF00 && cp <= 0xFF0F) return false;              // full-width punctuation
    if (cp == 0xFEFF) return false;
    return true;
}

// The character that ends just before byte `i`, and the one that starts at byte `i`.
std::uint32_t codepointBefore(const std::string& t, size_t i) {
    size_t s = i;
    while (s > 0 && i - s < 4) {
        --s;
        if (((unsigned char)t[s] & 0xC0) != 0x80) break;
    }
    const unsigned char c = (unsigned char)t[s];
    const int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 0;
    std::uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k <= extra && s + (size_t)k < t.size(); ++k) cp = (cp << 6) | ((unsigned char)t[s + (size_t)k] & 0x3F);
    return cp;
}
std::uint32_t codepointAt(const std::string& t, size_t i) {
    const unsigned char c = (unsigned char)t[i];
    const int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 0;
    std::uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k <= extra && i + (size_t)k < t.size(); ++k) cp = (cp << 6) | ((unsigned char)t[i + (size_t)k] & 0x3F);
    return cp;
}


}  // namespace studio::words

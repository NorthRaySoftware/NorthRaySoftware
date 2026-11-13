// Hyphenation: Frank Liang's algorithm (the one TeX uses) over the hyph-utf8
// patterns. Written here; only the pattern files come from hyph-utf8.
#include "studio/typeset.hpp"

#include "internal.hpp"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace studio::typeset {

static char32_t lowerCp(char32_t c) {
    if (c < 0x80) return (char32_t)std::tolower((int)c);
    return (char32_t)std::towlower((wint_t)c);
}

bool Hyphenator::load(const std::string& patternsFile, const std::string& exceptionsFile) {
    patterns_.clear();
    exceptions_.clear();
    maxLen_ = 0;
    std::ifstream in(studio::detail::u8path(patternsFile), std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string tok;
    while (ss >> tok) {
        if (tok[0] == '%') { std::string rest; std::getline(ss, rest); continue; }
        const std::u32string p = utf32(tok);
        std::u32string letters;
        std::vector<std::uint8_t> values(1, 0);
        for (char32_t c : p) {
            if (c >= U'0' && c <= U'9') values.back() = (std::uint8_t)(c - U'0');
            else { letters += lowerCp(c); values.push_back(0); }
        }
        if (letters.empty()) continue;
        maxLen_ = std::max(maxLen_, letters.size());
        patterns_[letters] = std::move(values);
    }
    if (!exceptionsFile.empty()) {
        std::ifstream ex(studio::detail::u8path(exceptionsFile), std::ios::binary);
        std::string w;
        while (ex >> w) {
            const std::u32string u = utf32(w);
            std::u32string word;
            std::vector<int> pts;
            for (char32_t c : u) {
                if (c == U'-') pts.push_back((int)word.size());
                else word += lowerCp(c);
            }
            exceptions_[word] = pts;
        }
    }
    return !patterns_.empty();
}

std::vector<int> Hyphenator::points(const std::u32string& wordIn) const {
    std::vector<int> out;
    const int n = (int)wordIn.size();
    if (n < leftMin + rightMin || patterns_.empty()) return out;
    std::u32string word;
    for (char32_t c : wordIn) {
        // Only plain words: anything with digits or symbols is left whole.
        if (!(std::iswalpha((wint_t)c) || c == U'’' || c == U'\'')) return out;
        word += lowerCp(c);
    }
    if (auto it = exceptions_.find(word); it != exceptions_.end()) {
        for (int p : it->second) if (p >= leftMin && p <= n - rightMin) out.push_back(p);
        return out;
    }
    const std::u32string padded = U"." + word + U".";
    std::vector<std::uint8_t> v(padded.size() + 1, 0);
    for (size_t i = 0; i < padded.size(); ++i) {
        for (size_t len = 1; len <= maxLen_ && i + len <= padded.size(); ++len) {
            auto it = patterns_.find(padded.substr(i, len));
            if (it == patterns_.end()) continue;
            for (size_t k = 0; k < it->second.size(); ++k) v[i + k] = std::max(v[i + k], it->second[k]);
        }
    }
    // v[i + 1] is the value between word[i - 1] and word[i] (the padding shifts it by one).
    for (int i = leftMin; i <= n - rightMin; ++i)
        if (v[i + 1] % 2 == 1) out.push_back(i);
    return out;
}

}  // namespace studio::typeset

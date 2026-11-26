#include "studio/icons.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>
#include <cctype>
#include <algorithm>

namespace studio::icons {

namespace {
double channel(const std::string& hex, int i) {
    if (hex.size() != 7) return 0;
    return std::stoi(hex.substr(1 + i * 2, 2), nullptr, 16) / 255.0;
}
const char* kindColour(const std::string& kind) {
    // The kinds' colours as the app draws them in the light theme (entryKindColor).
    static const std::pair<const char*, const char*> ks[] = {{"character", "#3E7BD6"}, {"creature", "#C26A2E"}, {"place", "#2E9A6C"},
                                                             {"race", "#6B8E23"},      {"realm", "#5B5FC7"},    {"group", "#7D5BD0"},
                                                             {"thing", "#B08A1E"},     {"lore", "#1E97A6"},     {"magic", "#B3439E"},
                                                             {"event", "#C9476A"},     {"language", "#4F7F8F"}};
    for (auto& [k, c] : ks) if (kind == k) return c;
    return "#3E7BD6";
}
bool isHex(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < 7; ++i) if (!std::isxdigit((unsigned char)s[i])) return false;
    return true;
}
}  // namespace

std::string colourOf(const Entry& e) {
    if (isHex(e.colour)) return e.colour;
    if (e.kind == "character" || e.kind == "creature") {
        static const char* light[] = {"#3B7DD8", "#D2513A", "#2E9E6B", "#9B59B6", "#D48A1F", "#1F9CA8", "#C2417E", "#6C7A2E", "#5B5FD6", "#A0522D"};
        std::uint32_t h = 2166136261u;
        for (unsigned char c : e.id) h = (h ^ c) * 16777619u;
        return light[h % 10];
    }
    return kindColour(e.kind);
}

std::string inkOn(const std::string& hex) {
    auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    const double l = 0.2126 * lin(channel(hex, 0)) + 0.7152 * lin(channel(hex, 1)) + 0.0722 * lin(channel(hex, 2));
    return l > 0.36 ? "#15171E" : "#FFFFFF";
}

std::string initialsOf(const std::string& name) {
    std::string out;
    size_t i = 0;
    std::vector<std::string> words;
    while (i < name.size()) {
        while (i < name.size() && name[i] == ' ') ++i;
        size_t j = i;
        while (j < name.size() && name[j] != ' ') ++j;
        if (j > i) words.push_back(name.substr(i, j - i));
        i = j;
    }
    auto first = [](const std::string& w) {   // its first character, whole (UTF-8), upper-cased when ASCII
        if (w.empty()) return std::string();
        const unsigned char c = (unsigned char)w[0];
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        std::string s = w.substr(0, std::min(n, w.size()));
        if (n == 1) s[0] = (char)std::toupper(c);
        return s;
    };
    if (!words.empty()) out += first(words.front());
    if (words.size() > 1) out += first(words.back());
    return out.empty() ? "?" : out;
}

Icon iconOf(const Entry& e) {
    Icon i;
    i.name = e.name;
    i.colour = colourOf(e);
    i.ink = inkOn(i.colour);
    i.initials = initialsOf(e.name);
    if (e.symbol.rfind("blob:", 0) == 0) i.symbolImage = e.symbol.substr(5);
    else if (e.symbol.rfind("icon:", 0) != 0) i.symbol = e.symbol;
    const bool symbolAlone = e.iconStyle == "symbol" && !e.symbol.empty();
    if (!symbolAlone) i.image = e.imageBlob;
    return i;
}

Id viewpointOf(const Library& lib, const ManuscriptNode& n) {
    if (!n.povId.empty()) return n.povId;
    if (n.kind != NodeKind::Chapter) return {};
    for (const ManuscriptNode* s : lib.manuscriptOf(n.bookId))
        if (s->parentId == n.id && !s->povId.empty()) return s->povId;
    return {};
}

std::string withoutLeadingSymbol(const std::string& title, const std::string& symbol) {
    if (symbol.empty() || title.compare(0, symbol.size(), symbol) != 0) return title;
    size_t i = symbol.size();
    // A variation selector typed with it, then spaces and a separator (":", "-", "–", "—", "|", "·").
    if (title.compare(i, 3, "\xEF\xB8\x8F") == 0) i += 3;
    while (i < title.size() && title[i] == ' ') ++i;
    for (const char* sep : {":", "-", "|", "\xE2\x80\x93", "\xE2\x80\x94", "\xC2\xB7"})
        if (title.compare(i, std::string(sep).size(), sep) == 0) { i += std::string(sep).size(); break; }
    while (i < title.size() && title[i] == ' ') ++i;
    return title.substr(i);
}

std::vector<Id> titlesStartingWithSymbol(const Library& lib, const Id& book) {
    std::vector<Id> out;
    for (const ManuscriptNode* n : lib.manuscriptOf(book)) {
        if (n->kind != NodeKind::Chapter) continue;
        const Entry* e = lib.entry(viewpointOf(lib, *n));
        if (!e || e->deleted) continue;
        const Icon ic = iconOf(*e);
        if (!ic.symbol.empty() && withoutLeadingSymbol(n->title, ic.symbol) != n->title) out.push_back(n->id);
    }
    return out;
}

}  // namespace studio::icons

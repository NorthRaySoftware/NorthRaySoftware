// Continuity checks for the world bible: facts that disagree (two lines of an entry's details, or the
// details against what the manuscripts say about a character's eyes, hair or age), and entries whose
// names never appear in the text.
#include "studio/document.hpp"
#include "studio/review.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace studio::review {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t' || s.back() == '.')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
bool letter(unsigned char c) { return std::isalnum(c) || c >= 0x80 || c == '\''; }

// Whole-word, case-sensitive: is `name` in `s`?
bool hasName(const std::string& s, const std::string& name) {
    if (name.empty()) return false;
    for (size_t at = s.find(name); at != std::string::npos; at = s.find(name, at + 1)) {
        const bool a = at == 0 || !letter((unsigned char)s[at - 1]);
        const size_t e = at + name.size();
        const bool z = e >= s.size() || !letter((unsigned char)s[e]);
        if (a && z) return true;
    }
    return false;
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::string w;
    for (char c : s) {
        if (letter((unsigned char)c) || c == '-') w += (char)std::tolower((unsigned char)c);
        else { if (!w.empty()) out.push_back(w); w.clear(); }
    }
    if (!w.empty()) out.push_back(w);
    return out;
}

// Sentences of a chapter's plain text.
std::vector<std::string> sentences(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        cur += c == '\n' ? ' ' : c;
        if (c == '.' || c == '!' || c == '?' || c == '\n') {
            if (trim(cur).size() > 1) out.push_back(trim(cur));
            cur.clear();
        }
    }
    if (trim(cur).size() > 1) out.push_back(trim(cur));
    return out;
}

const std::set<std::string>& colours() {
    static const std::set<std::string> c{"grey", "gray", "blue", "green", "brown", "hazel", "amber", "black", "violet", "red", "gold",
                                         "golden", "silver", "white", "auburn", "blond", "blonde", "copper", "purple", "yellow", "orange",
                                         "pink", "chestnut", "ginger", "dark", "pale", "sapphire", "emerald"};
    return c;
}
std::string sameColour(std::string c) {
    if (c == "gray") return "grey";
    if (c == "golden") return "gold";
    if (c == "blonde") return "blond";
    if (c == "ginger") return "red";
    return c;
}
int numberWord(const std::string& w) {
    static const std::map<std::string, int> n{
        {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10},
        {"eleven", 11}, {"twelve", 12}, {"thirteen", 13}, {"fourteen", 14}, {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17},
        {"eighteen", 18}, {"nineteen", 19}, {"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fifty", 50}, {"sixty", 60}, {"seventy", 70},
        {"eighty", 80}, {"ninety", 90}, {"hundred", 100}};
    if (!w.empty() && std::all_of(w.begin(), w.end(), [](char c) { return std::isdigit((unsigned char)c); })) return std::atoi(w.c_str());
    if (auto it = n.find(w); it != n.end()) return it->second;
    const size_t dash = w.find('-');
    if (dash != std::string::npos) {
        const int a = numberWord(w.substr(0, dash)), b = numberWord(w.substr(dash + 1));
        if (a >= 20 && b > 0 && b < 10) return a + b;
    }
    return -1;
}

// What a sentence says about eyes, hair or age (empty if nothing).
struct Said { std::string eyes, hair; int age = -1; };
Said whatItSays(const std::string& sentence) {
    Said s;
    const auto w = words(sentence);
    for (size_t i = 0; i < w.size(); ++i) {
        const bool eyes = w[i] == "eyes" || w[i] == "eye", hair = w[i] == "hair";
        if (eyes || hair) {
            std::string c;
            // "her grey eyes", "the cold blue eyes": up to two words before.
            for (size_t k = 1; k <= 2 && k <= i && c.empty(); ++k) if (colours().count(w[i - k])) c = w[i - k];
            // "eyes were grey", "her hair, black as".
            if (c.empty())
                for (size_t k = 1; k <= 3 && i + k < w.size() && c.empty(); ++k) if (colours().count(w[i + k])) c = w[i + k];
            if (!c.empty() && c != "dark" && c != "pale") (eyes ? s.eyes : s.hair) = sameColour(c);
        }
        // "a twenty-three-year-old", "34 years old", "aged 54".
        if ((w[i] == "year-old" || w[i] == "years" || w[i] == "year") && i > 0) {
            std::string n = w[i - 1];
            if (n.size() > 5 && n.substr(n.size() - 5) == "-year") n = n.substr(0, n.size() - 5);
            const int v = numberWord(n);
            const bool old = w[i] == "year-old" || (i + 1 < w.size() && (w[i + 1] == "old" || w[i + 1] == "of"));
            if (v > 0 && old) s.age = v;
        }
        if (w[i].size() > 9 && w[i].substr(w[i].size() - 9) == "-year-old") {
            const int v = numberWord(w[i].substr(0, w[i].size() - 9));
            if (v > 0) s.age = v;
        }
        if (w[i] == "aged" && i + 1 < w.size() && numberWord(w[i + 1]) > 0) s.age = numberWord(w[i + 1]);
    }
    return s;
}

// The bible's colour for eyes or hair from a detail line: its colour words.
std::set<std::string> colourWords(const std::string& value) {
    std::set<std::string> out;
    for (auto& w : words(value)) if (colours().count(w)) out.insert(sameColour(w));
    return out;
}

}  // namespace

std::string FactIssue::key() const { return entry + "|" + lower(label) + "|" + lower(bible) + "|" + lower(other); }

Continuity continuityChecks(const Library& lib, const Id& worldId, const Id& bookId) {
    Continuity out;
    const auto entries = lib.entriesOf(worldId, bookId);
    std::vector<const Book*> books;
    if (worldId.empty()) { if (const Book* b = lib.book(bookId)) books.push_back(b); }
    else for (const Book* b : lib.liveBooks()) if (b->worldId == worldId) books.push_back(b);

    // ---- Lines of one entry's details that disagree ("Gender: Male" and "Gender: Man").
    struct Known { std::string eyes, hair; int age = -1; std::string eyesLine, hairLine, ageLine; };
    std::map<Id, Known> known;
    for (const Entry* e : entries) {
        std::map<std::string, std::vector<std::string>> byLabel;
        std::map<std::string, std::string> labelText;
        size_t at = 0;
        while (at <= e->details.size()) {
            size_t nl = e->details.find('\n', at);
            if (nl == std::string::npos) nl = e->details.size();
            const std::string line = e->details.substr(at, nl - at);
            at = nl + 1;
            const size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0 || colon > 40) continue;
            const std::string label = trim(line.substr(0, colon)), value = trim(line.substr(colon + 1));
            if (value.empty()) continue;
            const std::string key = lower(label);
            byLabel[key].push_back(value);
            labelText[key] = label;
            Known& k = known[e->id];
            if (key == "eyes" || key == "eye colour" || key == "eye color") { k.eyes = value; k.eyesLine = label; }
            if (key == "hair" || key == "hair colour" || key == "hair color") { k.hair = value; k.hairLine = label; }
            if (key == "age") { if (const int v = numberWord(lower(words(value).empty() ? std::string() : words(value)[0])); v > 0) { k.age = v; k.ageLine = label; } }
        }
        for (auto& [key, vals] : byLabel) {
            std::set<std::string> distinct;
            for (auto& v : vals) distinct.insert(lower(v));
            if (distinct.size() < 2) continue;
            FactIssue f;
            f.entry = e->id;
            f.label = labelText[key];
            f.bible = vals[0];
            f.other = vals[1];
            f.inBible = true;
            out.facts.push_back(f);
        }
    }

    // ---- The text against the bible: sentences naming one character only.
    std::vector<std::pair<const Entry*, std::vector<std::string>>> people;
    for (const Entry* e : entries) if (e->kind == "character") people.push_back({e, searchNames(*e)});
    std::set<Id> seen;
    std::set<std::string> reported;
    for (const Book* b : books)
        for (const auto& [node, ids] : lib.appearancesIn(b->id)) {
            for (const Id& id : ids) seen.insert(id);
            if (known.empty()) continue;
            const std::string text = doc::plainText(lib.nodeContent(node));
            for (const std::string& s : sentences(text)) {
                const Entry* who = nullptr;
                int n = 0;
                for (auto& [e, names] : people) {
                    bool hit = false;
                    for (auto& nm : names) if (hasName(s, nm)) { hit = true; break; }
                    if (hit) { who = e; ++n; }
                }
                if (n != 1) continue;
                auto k = known.find(who->id);
                if (k == known.end()) continue;
                const Said said = whatItSays(s);
                auto report = [&](const std::string& label, const std::string& bible, const std::string& textSays) {
                    FactIssue f;
                    f.entry = who->id;
                    f.label = label;
                    f.bible = bible;
                    f.other = textSays;
                    f.bookId = b->id;
                    f.nodeId = node;
                    f.snippet = s.size() > 240 ? s.substr(0, 237) + "..." : s;
                    if (reported.insert(f.key()).second) out.facts.push_back(f);
                };
                if (!said.eyes.empty() && !k->second.eyes.empty()) {
                    const auto c = colourWords(k->second.eyes);
                    if (!c.empty() && !c.count(said.eyes)) report(k->second.eyesLine, k->second.eyes, said.eyes + " eyes");
                }
                if (!said.hair.empty() && !k->second.hair.empty()) {
                    const auto c = colourWords(k->second.hair);
                    if (!c.empty() && !c.count(said.hair)) report(k->second.hairLine, k->second.hair, said.hair + " hair");
                }
                if (said.age > 0 && k->second.age > 0 && said.age != k->second.age)
                    report(k->second.ageLine, std::to_string(k->second.age), std::to_string(said.age) + " years old");
            }
        }

    // ---- Entries the manuscripts never name.
    for (const Entry* e : entries) if (!seen.count(e->id)) out.unused.push_back(e->id);
    return out;
}

}  // namespace studio::review

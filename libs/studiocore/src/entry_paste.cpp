#include "studio/entry_paste.hpp"
#include "studio/document.hpp"

#include <algorithm>
#include <set>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace studio::paste {

namespace {

std::string trim(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// Lower case, curly apostrophes and quotes straight, spaces squeezed, a trailing colon or full stop off.
std::string key(const std::string& in) {
    std::string out;
    bool sp = false;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in.compare(i, 3, "\xE2\x80\x99") == 0 || in.compare(i, 3, "\xE2\x80\x98") == 0) { out += '\''; i += 2; sp = false; continue; }
        const unsigned char c = (unsigned char)in[i];
        if (c == ' ' || c == '\t') { if (!sp && !out.empty()) out += ' '; sp = true; continue; }
        out += (char)std::tolower(c);
        sp = false;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == ':' || out.back() == '.')) out.pop_back();
    return out;
}

std::string normalise(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = (unsigned char)in[i];
        if (c == '\r') { if (i + 1 < in.size() && in[i + 1] == '\n') continue; out += '\n'; continue; }
        if (in.compare(i, 3, "\xEF\xBB\xBF") == 0 || in.compare(i, 3, "\xE2\x80\x8B") == 0 || in.compare(i, 3, "\xE2\x80\x8C") == 0 ||
            in.compare(i, 3, "\xE2\x80\x8D") == 0) { i += 2; continue; }                                       // BOM, zero-width
        if (in.compare(i, 2, "\xC2\xA0") == 0) { out += ' '; ++i; continue; }                                // no-break space
        if (in.compare(i, 3, "\xE2\x80\xAF") == 0) { out += ' '; i += 2; continue; }                         // narrow no-break space
        if (c == '\t') { out += ' '; continue; }
        out += (char)c;
    }
    return out;
}

enum class Slot { None, Name, Aliases, Say, Summary, Details, Notes, BookNotes, Family, Relations, Spoilers, Section };

Slot slotOf(const std::string& k) {
    static const std::vector<std::pair<const char*, Slot>> table{
        {"name", Slot::Name}, {"full name", Slot::Name}, {"names", Slot::Name},
        {"other names", Slot::Aliases}, {"other name", Slot::Aliases}, {"aliases", Slot::Aliases}, {"alias", Slot::Aliases},
        {"also known as", Slot::Aliases}, {"aka", Slot::Aliases}, {"nicknames", Slot::Aliases}, {"nickname", Slot::Aliases},
        {"known as", Slot::Aliases}, {"other names and titles", Slot::Aliases},
        {"how it's said", Slot::Say}, {"how it is said", Slot::Say}, {"how to say it", Slot::Say}, {"pronunciation", Slot::Say},
        {"pronounced", Slot::Say}, {"how the name is said", Slot::Say},
        {"summary", Slot::Summary}, {"overview", Slot::Summary}, {"in brief", Slot::Summary}, {"short summary", Slot::Summary},
        {"details", Slot::Details}, {"facts", Slot::Details}, {"quick facts", Slot::Details}, {"at a glance", Slot::Details},
        {"basics", Slot::Details}, {"fields", Slot::Details}, {"vital statistics", Slot::Details}, {"stats", Slot::Details},
        {"notes", Slot::Notes}, {"private notes", Slot::Notes}, {"author's notes", Slot::Notes}, {"author notes", Slot::Notes},
        {"notes per book", Slot::BookNotes}, {"notes by book", Slot::BookNotes}, {"book notes", Slot::BookNotes},
        {"per book", Slot::BookNotes}, {"by book", Slot::BookNotes}, {"in each book", Slot::BookNotes}, {"appearances", Slot::BookNotes},
        {"family", Slot::Family}, {"relationships", Slot::Relations}, {"relations", Slot::Relations}, {"connections", Slot::Relations},
        {"links", Slot::Relations}, {"spoilers", Slot::Spoilers}, {"spoiler", Slot::Spoilers}};
    for (auto& [w, s] : table) if (k == w) return s;
    return Slot::None;
}

bool isSeparator(const std::string& l) {
    if (l.size() < 3) return false;
    return std::all_of(l.begin(), l.end(), [](char c) { return c == '-' || c == '*' || c == '_' || c == '=' || c == ' '; });
}

std::string withoutEmphasis(std::string s) {
    for (const char* m : {"**", "__"}) {
        size_t p;
        while ((p = s.find(m)) != std::string::npos) s.erase(p, 2);
    }
    // A whole line in *italics* (but not a list's "* ").
    if (s.size() > 2 && s.front() == '*' && s.back() == '*') s = s.substr(1, s.size() - 2);
    return trim(s);
}

// "Label: value" with a short label (no sentence before the colon).
bool splitField(const std::string& l, std::string& label, std::string& value) {
    const size_t c = l.find(':');
    if (c == std::string::npos || c == 0 || c > 48) return false;
    const std::string lab = trim(l.substr(0, c));
    if (lab.empty() || std::count(lab.begin(), lab.end(), ' ') > 5) return false;
    if (lab.find_first_of(".!?\"") != std::string::npos) return false;
    if (c + 1 < l.size() && l[c + 1] == '/') return false;   // a web address
    label = lab;
    value = trim(l.substr(c + 1));
    return true;
}

bool endsSentence(const std::string& s) {
    if (s.empty()) return false;
    const char c = s.back();
    if (c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == '"' || c == ')') return true;
    return s.size() >= 3 && (s.compare(s.size() - 3, 3, "\xE2\x80\x9D") == 0 || s.compare(s.size() - 3, 3, "\xE2\x80\xA6") == 0);
}

int words(const std::string& s) {
    int n = 0;
    bool in = false;
    for (char c : s) { if (c == ' ') in = false; else if (!in) { in = true; ++n; } }
    return n;
}

// Text gathered into a block: lines kept, a blank line between paragraphs.
void addLine(std::string& to, bool& gap, const std::string& line) {
    if (to.empty()) to = line;
    else to += (gap ? "\n\n" : "\n") + line;
    gap = false;
}

std::vector<std::string> splitNames(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    auto push = [&] { std::string t = trim(cur); if (!t.empty()) out.push_back(t); cur.clear(); };
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '(') ++depth;
        if (c == ')' && depth) --depth;
        if (!depth && (c == ',' || c == ';')) { push(); continue; }
        cur += c;
    }
    push();
    return out;
}

}  // namespace

Page parse(const std::string& raw) {
    Page p;
    const std::string text = normalise(raw);
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        for (std::string l; std::getline(in, l);) lines.push_back(l);
    }
    Slot block = Slot::None;
    size_t section = 0;       // which of p.sections, when block is Section
    size_t bookNote = 0;      // which of p.bookNotes, when in BookNotes
    bool gap = false;
    bool prevBlank = true;
    bool seenDetails = false;     // a "Details" part already read
    Slot awaiting = Slot::None;   // "Name" on its own line: the value is the next line
    static const std::regex bookLabel(R"(^(.{1,80}?\(\s*book\s+\d+\s*\)|book\s+\d+|book\s+(one|two|three|four|five|six|seven|eight|nine|ten))\s*[.:\-]?\s*(.*)$)",
                                      std::regex::icase);
    auto textOf = [&](Slot s) -> std::string* {
        switch (s) {
            case Slot::Summary: return &p.summary;
            case Slot::Notes: return &p.notes;
            case Slot::Spoilers: return &p.notes;
            case Slot::Section: return section < p.sections.size() ? &p.sections[section].second : nullptr;
            case Slot::BookNotes: return bookNote < p.bookNotes.size() ? &p.bookNotes[bookNote].second : nullptr;
            default: return nullptr;
        }
    };
    auto addDetail = [&](const std::string& l) { p.details += (p.details.empty() ? "" : "\n") + l; };
    auto setSingle = [&](Slot s, const std::string& v) {
        if (v.empty()) return;
        if (s == Slot::Name) p.name = v;
        else if (s == Slot::Say) p.pronunciation = v;
        else if (s == Slot::Aliases)
            for (auto& a : splitNames(v)) {
                std::string t = a;
                if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'')) t = trim(t.substr(1, t.size() - 2));
                if (!t.empty() && std::none_of(p.aliases.begin(), p.aliases.end(), [&](const std::string& x) { return key(x) == key(t); })) p.aliases.push_back(t);
            }
    };
    auto startBlock = [&](Slot s) {
        block = s;
        gap = false;
        ++p.parts;
        if (s != Slot::Section) ++p.known;
        if (s == Slot::Spoilers && !p.notes.empty()) gap = true;
        if (s == Slot::Spoilers) addLine(p.notes, gap, "Spoilers:");
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string l = trim(lines[i]);
        if (l.empty() || isSeparator(l)) {
            if (!prevBlank) gap = true;
            prevBlank = true;
            continue;
        }
        const bool wasBlank = prevBlank;
        prevBlank = false;
        bool marked = false;   // shown as a heading (Markdown # or the whole line in bold)
        if (l[0] == '#') {
            size_t h = 0;
            while (h < l.size() && l[h] == '#') ++h;
            if (h < l.size() && l[h] == ' ') { l = trim(l.substr(h)); marked = true; }
        }
        bool bullet = false;
        for (const char* b : {"- ", "* ", "\xE2\x80\xA2 ", "\xE2\x80\x93 ", "+ "})
            if (startsWith(l, b)) { l = trim(l.substr(std::string(b).size())); bullet = true; break; }
        // An italic or bold label at the start ("*Shards of Light (Book 1).* Tom stabs…").
        std::string lead, after;
        if (!bullet && (startsWith(l, "**") || startsWith(l, "__") || l[0] == '*' || l[0] == '_')) {
            const std::string m = (startsWith(l, "**") || startsWith(l, "__")) ? l.substr(0, 2) : l.substr(0, 1);
            const size_t close = l.find(m, m.size());
            if (close != std::string::npos) {
                lead = trim(l.substr(m.size(), close - m.size()));
                after = trim(l.substr(close + m.size()));
                if (after.empty() && m.size() == 2) marked = true;
            }
        }
        const std::string c = withoutEmphasis(l);
        if (c.empty()) continue;

        if (awaiting != Slot::None) {
            // "Name:" left blank (a template): the next line is another part, not the name.
            std::string lab, val;
            const bool another = slotOf(key(c)) != Slot::None || (splitField(c, lab, val) && slotOf(key(lab)) != Slot::None);
            const Slot was = awaiting;
            awaiting = Slot::None;
            if (!another) {
                setSingle(was, c);
                continue;
            }
        }

        // A known heading on its own ("Summary", "**Details**", "## Notes per book", "Family:").
        const Slot known = slotOf(key(c));
        if (known != Slot::None && words(c) <= 5) {
            if (known == Slot::Name || known == Slot::Aliases || known == Slot::Say) { awaiting = known; ++p.parts; ++p.known; block = Slot::None; continue; }
            if (known == Slot::Details && seenDetails) {
                // A second "Details": a section of that name (Lore's template has one), not more detail lines.
                std::string t = c;
                while (!t.empty() && (t.back() == ':' || t.back() == ' ')) t.pop_back();
                p.sections.emplace_back(t, std::string());
                section = p.sections.size() - 1;
                startBlock(Slot::Section);
                continue;
            }
            if (known == Slot::Details) seenDetails = true;
            startBlock(known);
            continue;
        }

        // Within the notes per book: a book's label starts its note.
        if (block == Slot::BookNotes && !bullet) {
            std::string label, rest;
            std::smatch m;
            const std::string probe = lead.empty() ? c : lead;
            if (std::regex_match(probe, m, bookLabel)) {
                label = trim(m[1].str());
                rest = lead.empty() ? trim(m[3].str()) : trim(m[3].str() + " " + after);
            } else if (!lead.empty() && words(lead) <= 10) {
                label = lead;
                rest = after;
            } else if (std::string lab, val; splitField(c, lab, val) && words(lab) <= 8) {
                label = lab;
                rest = val;
            }
            while (!label.empty() && (label.back() == '.' || label.back() == ':' || label.back() == ' ')) label.pop_back();
            if (!label.empty()) {
                p.bookNotes.emplace_back(label, std::string());
                bookNote = p.bookNotes.size() - 1;
                gap = false;
                if (!rest.empty()) addLine(p.bookNotes[bookNote].second, gap, rest);
                continue;
            }
        }

        // "Label: value".
        std::string label, value;
        if (!bullet && splitField(c, label, value)) {
            const Slot s = slotOf(key(label));
            if (!value.empty() && (s == Slot::Name || s == Slot::Aliases || s == Slot::Say)) {
                setSingle(s, value);
                ++p.parts;
                ++p.known;
                block = Slot::None;
                continue;
            }
            if (!value.empty() && (s == Slot::Summary || s == Slot::Notes || s == Slot::Spoilers)) {
                startBlock(s);
                addLine(*textOf(s), gap, value);
                continue;
            }
            if (value.empty() && block != Slot::Details && block != Slot::Family && block != Slot::Relations && words(label) <= 6) {
                // "Appearance:" on its own: a section.
                p.sections.emplace_back(label, std::string());
                section = p.sections.size() - 1;
                startBlock(Slot::Section);
                continue;
            }
            if (!value.empty()) {
                if (block == Slot::Family || block == Slot::Relations) { p.relations.emplace_back(label, value); continue; }
                if (block == Slot::Details || (block == Slot::None && p.parts > 0)) { addDetail(label + ": " + value); continue; }
            }
        }

        // Another heading: marked as one, or a short line on its own that doesn't end a sentence.
        // (The line before ends something: a blank, a sentence, a field, or another heading. Pages pasted without
        // blank lines between parts still have their headings found.)
        bool boundary = wasBlank;
        if (!boundary && i > 0) {
            const std::string prev = withoutEmphasis(trim(lines[i - 1]));
            std::string pl, pv;
            boundary = prev.empty() || isSeparator(prev) || endsSentence(prev) || splitField(prev, pl, pv) ||
                       (words(prev) <= 5 && prev.find(':') == std::string::npos);
        }
        const bool plainHeading = !bullet && boundary && words(c) <= 5 && c.size() <= 48 && !endsSentence(c) &&
                                  std::isupper((unsigned char)c[0]) && c.find(':') == std::string::npos &&
                                  (i + 1 >= lines.size() || !trim(lines[i + 1]).empty() || block == Slot::Details || block == Slot::Family ||
                                   block == Slot::Relations || block == Slot::Summary || (block == Slot::Section && textOf(block) && textOf(block)->empty()));
        if ((marked && words(c) <= 8) || plainHeading) {
            std::string t = c;
            while (!t.empty() && (t.back() == ':' || t.back() == ' ')) t.pop_back();
            p.sections.emplace_back(t, std::string());
            section = p.sections.size() - 1;
            startBlock(Slot::Section);
            continue;
        }

        // Text in the block it's under ("Age:" left blank in a template is skipped).
        if ((block == Slot::Details || block == Slot::Family || block == Slot::Relations) && !c.empty() && c.back() == ':') continue;
        if (block == Slot::Details) { addDetail(c); continue; }
        if (block == Slot::Family || block == Slot::Relations) { addDetail(c); continue; }
        if (block == Slot::BookNotes && p.bookNotes.empty()) { p.bookNotes.emplace_back(std::string(), std::string()); bookNote = 0; }
        if (std::string* t = textOf(block)) addLine(*t, gap, bullet ? "\xE2\x80\xA2 " + c : c);
        // Before anything recognised (a chat's "Here's the entry…"): left out.
    }
    // A "Spoilers" heading with nothing under it (a template) leaves nothing in the notes.
    {
        std::string t = p.notes;
        while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
        if (t.size() >= 9 && t.compare(t.size() - 9, 9, "Spoilers:") == 0) {
            t.erase(t.size() - 9);
            while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
            p.notes = t;
        }
    }
    // Sections with nothing under them aren't kept.
    p.sections.erase(std::remove_if(p.sections.begin(), p.sections.end(), [](auto& s) { return trim(s.second).empty(); }), p.sections.end());
    p.bookNotes.erase(std::remove_if(p.bookNotes.begin(), p.bookNotes.end(), [](auto& s) { return trim(s.second).empty(); }), p.bookNotes.end());
    return p;
}

std::string blankPage(const std::string& kind, const std::vector<std::string>& sections, const std::vector<std::string>& bookTitles) {
    std::vector<const char*> details;
    if (kind == "character") details = {"Gender", "Race", "Realm", "Age", "Birthday", "Height", "Hair", "Eyes", "Marks", "Clothing", "Weapon", "Home", "Occupation", "Title"};
    else if (kind == "creature") details = {"Realm", "Habitat", "Size", "Diet", "Lifespan", "Danger"};
    else if (kind == "race") details = {"Realm", "Homeland", "Lifespan", "Language", "Population"};
    else if (kind == "place") details = {"Type", "Realm", "Location", "Climate", "Population", "Ruled by", "Founded"};
    else if (kind == "realm") details = {"Ruled by", "Reached by", "Time there"};
    else if (kind == "group") details = {"Leader", "Base", "Founded", "Members", "Symbol", "Colours"};
    else if (kind == "thing") details = {"Type", "Made by", "Made of", "Owner", "Where it is"};
    else if (kind == "magic") details = {"Source", "Colour", "Cost", "Difficulty"};
    else if (kind == "event") details = {"When", "Where", "Outcome"};
    else details = {"When", "Where"};
    std::string t = "Name: \nOther names: \nHow it's said: \n\nSummary\n\n\nDetails\n";
    for (auto* d : details) t += std::string(d) + ": \n";
    for (auto& s : sections) t += "\n" + s + "\n\n";
    t += "\nNotes per book\n";
    if (bookTitles.empty()) t += "Book 1: \n";
    for (auto& b : bookTitles) t += b + ": \n";
    if (kind == "character") t += "\nFamily\nParents: \nPartner: \nSiblings: \nChildren: \n";
    t += "\nRelationships\n";
    std::vector<std::string> rel;
    if (kind == "character") rel = {"Friends", "Enemies", "Taught by", "Member of", "Lives in", "Rides", "Owns"};
    else
        for (auto& c : relationsFor(kind)) if (rel.size() < 7) rel.push_back(c.label);
    for (auto& r : rel) t += r + ": \n";
    t += "\nNotes\n\n\nSpoilers\n\n";
    return t;
}

bool looksLikePage(const std::string& text) {
    if (text.find('\n') == std::string::npos) return false;
    const Page p = parse(text);
    return p.parts >= 2 && p.known >= 1;
}

// ---------------------------------------------------------------- relationships

std::string relationFor(const std::string& kind, const std::string& labelIn) {
    std::string k = key(labelIn);
    // "Former teacher", "late husband", "ex-wife": the same relationship.
    for (const char* pre : {"former ", "ex-", "ex ", "late ", "old ", "one-time ", "current ", "sworn ", "adoptive ", "step-", "step"})
        if (k.rfind(pre, 0) == 0 && k.size() > std::string(pre).size() + 2) { k = k.substr(std::string(pre).size()); break; }
    static const std::vector<std::pair<std::vector<const char*>, const char*>> table{
        {{"parents", "parent", "father", "mother", "mum", "mom", "dad", "birth parents", "adoptive parents"}, "Child of"},
        {{"children", "child", "son", "sons", "daughter", "daughters", "kids"}, "Parent of"},
        {{"siblings", "sibling", "brother", "brothers", "sister", "sisters", "twin", "half-sibling", "half-siblings", "adoptive sister",
          "adoptive brother"}, "Sibling of"},
        {{"partner", "partners", "girlfriend", "boyfriend", "lover", "betrothed", "fiance", "fiancee", "fiancé", "fiancée"}, "Partner of"},
        {{"spouse", "wife", "husband", "married to"}, "Married to"},
        {{"friends", "friend", "best friend", "best friends", "close friends", "family friend", "family friends", "childhood friends"}, "Friend of"},
        {{"enemies", "enemy", "nemesis", "foes", "foe"}, "Enemy of"},
        {{"rivals", "rival"}, "Rival of"},
        {{"allies", "ally"}, "Ally of"},
        {{"mentor", "mentors", "teacher", "teachers", "master", "trained by", "taught by", "instructor", "tutor", "student of", "pupil of",
          "apprentice of"}, "Student of"},
        {{"students", "student", "pupil", "pupils", "former pupil", "former pupils", "apprentice", "apprentices", "trainee"}, "Mentor of"},
        {{"relatives", "relative", "family", "uncle", "uncles", "aunt", "aunts", "cousin", "cousins", "grandparents", "grandparent", "grandmother",
          "grandfather", "grandchildren", "grandchild", "grandson", "granddaughter", "niece", "nieces", "nephew", "nephews", "in-laws",
          "extended family", "ancestor", "ancestors", "descendant", "descendants"}, "Family"},
        {{"crew of", "crew", "ship", "aboard", "serves aboard", "belongs to", "part of", "joined", "order", "guild", "house", "clan"}, "Member of"},
        {{"loves", "in love with", "beloved", "sweetheart"}, "Loves"},
        {{"members", "member"}, "Has member"},
        {{"leader", "leaders"}, "Led by"},
        {{"home", "lives at"}, "Lives in"},
        {{"birthplace", "born"}, "Born in"},
        {{"mount", "mounts", "steed", "dragon"}, "Rides"},
        {{"used by", "users", "known users", "wielded by", "practitioners", "practised by", "practiced by"}, "Practised by"},
        {{"source", "sources", "origin", "comes from"}, "Comes from"},
        {{"opposed by", "opposes", "opposite", "counter", "countered by"}, "Enemy of"},
        {{"wields", "weapon", "weapons", "carries"}, "Owns"},
        {{"associated with", "connected to", "linked to", "tied to", "see also"}, "Related to"}};
    std::string want;
    for (auto& [names, rel] : table)
        if (std::find(names.begin(), names.end(), k) != names.end()) { want = rel; break; }
    const auto& choices = relationsFor(kind);
    auto offered = [&](const std::string& l) -> std::string {
        for (auto& c : choices) if (key(c.label) == key(l)) return c.label;
        return {};
    };
    if (!want.empty())
        if (std::string r = offered(want); !r.empty()) return r;
    if (std::string r = offered(k); !r.empty()) return r;
    if (std::string r = offered(k + " of"); !r.empty()) return r;
    if (k.size() > 3 && k.back() == 's')
        if (std::string r = offered(k.substr(0, k.size() - 1) + " of"); !r.empty()) return r;
    return {};
}

// ---------------------------------------------------------------- applying

namespace {

std::string mergeText(const std::string& a, const std::string& b) {
    if (trim(b).empty()) return a;
    if (trim(a).empty()) return b;
    if (a.find(b) != std::string::npos) return a;
    return a + "\n\n" + b;
}

std::string detailLabel(const std::string& line) {
    std::string l, v;
    return splitField(line, l, v) ? key(l) : std::string();
}

std::string mergeDetails(const std::string& a, const std::string& b, int& added) {
    std::vector<std::string> la;
    {
        std::istringstream in(a);
        for (std::string l; std::getline(in, l);) la.push_back(l);
    }
    std::istringstream in(b);
    for (std::string l; std::getline(in, l);) {
        if (trim(l).empty()) continue;
        const std::string lab = detailLabel(l);
        bool done = false;
        for (auto& x : la)
            if (x == l || (!lab.empty() && detailLabel(x) == lab)) {
                if (x != l) { x = l; ++added; }
                done = true;
                break;
            }
        if (!done) { la.push_back(l); ++added; }
    }
    std::string out;
    for (auto& l : la) out += (out.empty() ? "" : "\n") + l;
    return out;
}

// The parts of a template title a heading can stand for ("Appearance & personality": "appearance", "personality").
std::vector<std::string> titleParts(const std::string& title) {
    std::vector<std::string> out{key(title)};
    std::string t = key(title);
    for (const char* sep : {" & ", ", ", " and ", " / ", "/"}) {
        size_t p;
        while ((p = t.find(sep)) != std::string::npos) t.replace(p, std::string(sep).size(), "|");
    }
    std::string cur;
    for (char c : t + "|") {
        if (c == '|') { if (!trim(cur).empty()) out.push_back(trim(cur)); cur.clear(); }
        else cur += c;
    }
    return out;
}

int numberWord(const std::string& w) {
    static const char* n[] = {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
    for (int i = 0; i < 10; ++i) if (w == n[i]) return i + 1;
    return std::atoi(w.c_str());
}

}  // namespace

bool apply(Library& lib, const Id& entryId, const Page& page, const std::vector<Id>& booksInOrder, Report& r) {
    const Entry* cur = lib.entry(entryId);
    if (!cur) return false;
    Entry e = *cur;
    // What's pasted replaces what was there, part by part; parts the paste doesn't have are left alone.
    if (!page.name.empty() && page.name != e.name) { e.name = page.name; r.name = true; }
    if (!page.aliases.empty()) {
        std::vector<std::string> al;
        for (auto& a : page.aliases)
            if (key(a) != key(e.name) && std::none_of(al.begin(), al.end(), [&](const std::string& x) { return key(x) == key(a); })) al.push_back(a);
        if (al != e.aliases) { e.aliases = al; r.aliases = (int)al.size(); }
    }
    if (!page.pronunciation.empty() && page.pronunciation != e.pronunciation) { e.pronunciation = page.pronunciation; r.pronunciation = true; }
    if (!page.summary.empty() && page.summary != e.summary) { e.summary = page.summary; r.summary = true; }

    // Sections: into the template's, by title or a part of one; else their own. Several headings going into one
    // section ("Appearance" and "Personality" into "Appearance & personality") are put together.
    std::vector<std::string> titles;
    for (auto& [t, text] : lib.entrySections(e)) titles.push_back(t);
    std::vector<std::pair<std::string, std::string>> into;   // title -> text gathered from the paste
    for (auto& [heading, text] : page.sections) {
        const std::string h = key(heading);
        std::string target;
        for (auto& t : titles) if (key(t) == h) { target = t; break; }
        if (target.empty())
            for (auto& t : titles) {
                const auto parts = titleParts(t);
                if (std::find(parts.begin() + 1, parts.end(), h) != parts.end()) { target = t; break; }
            }
        if (target.empty()) target = heading;
        bool found = false;
        for (auto& x : into) if (x.first == target) { x.second = mergeText(x.second, text); found = true; }
        if (!found) into.emplace_back(target, text);
    }
    for (auto& [t, text] : into)
        if (e.section(t) != text) { e.setSection(t, text); ++r.sections; }

    // Notes for each book.
    std::vector<const Book*> books;
    for (const Id& b : booksInOrder) if (const Book* bk = lib.book(b)) books.push_back(bk);
    std::string extraNotes;
    for (auto& [label, text] : page.bookNotes) {
        const Book* hit = nullptr;
        std::string title = label;
        static const std::regex num(R"(book\s+(\d+|one|two|three|four|five|six|seven|eight|nine|ten))", std::regex::icase);
        std::smatch m;
        int n = 0;
        if (std::regex_search(label, m, num)) {
            std::string wd = m[1].str();
            for (auto& ch : wd) ch = (char)std::tolower((unsigned char)ch);
            n = numberWord(wd);
        }
        if (const size_t paren = title.find('('); paren != std::string::npos) title = title.substr(0, paren);
        const std::string tk = key(title);
        for (const Book* b : books) if (!tk.empty() && key(b->title) == tk) { hit = b; break; }
        if (!hit)
            for (const Book* b : books)
                if (!tk.empty() && key(b->title).size() >= 4 && (tk.find(key(b->title)) != std::string::npos || key(b->title).find(tk) != std::string::npos)) {
                    hit = b;
                    break;
                }
        // "Book 2" alone: the second in reading order. A title that's no book's isn't guessed by its number.
        const bool titled = !tk.empty() && tk.rfind("book", 0) != 0;
        if (!hit && !titled && n >= 1 && n <= (int)books.size()) hit = books[(size_t)n - 1];
        if (hit) {
            if (e.bookNote(hit->id) != text) { e.setBookNote(hit->id, text); ++r.bookNotes; }
        } else {
            extraNotes = mergeText(extraNotes, (label.empty() ? std::string() : label + ":\n") + text);
            if (!label.empty()) r.unmatchedBooks.push_back(label);
        }
    }

    // Details: the pasted ones replace them all; family and relationship lines that aren't links replace lines with
    // their label.
    std::string details = page.details.empty() ? e.details : page.details;
    struct NewLink { Id to; std::string label; };
    std::vector<NewLink> links;
    std::vector<std::string> relabels;   // relationships the paste sets, so the old ones of that kind go
    auto others = lib.entriesOf(e.worldId, e.worldId.empty() ? e.bookId : Id{});   // looked up again after an entry is made
    // An entry in the bible by a pasted name: its name or another name it goes by (with or without "the"), or a fuller
    // form of its name ("Yonvaan Akumes Yerasuun" for the entry "Yonvaan Yerasuun"). Never a guess between two.
    auto bare = [&](std::string k) { return k.rfind("the ", 0) == 0 ? k.substr(4) : k; };
    auto findNamed = [&](const std::string& nm) -> const Entry* {
        std::string n = nm;
        if (const size_t paren = n.find('('); paren != std::string::npos) n = trim(n.substr(0, paren));
        const std::string k = bare(key(n));
        if (k.empty()) return nullptr;
        for (const Entry* o : others) if (o->id != e.id && bare(key(o->name)) == k) return o;
        const Entry* only = nullptr;
        bool twice = false;
        auto consider = [&](const Entry* o) { if (only && only != o) twice = true; else only = o; };
        for (const Entry* o : others) {
            if (o->id == e.id) continue;
            for (auto& s : searchNames(*o)) if (bare(key(s)) == k) { consider(o); break; }
        }
        if (only && !twice) return only;
        if (twice) return nullptr;
        // A fuller name: an entry whose whole name (two words or more) is a part of the pasted one.
        if (std::count(k.begin(), k.end(), ' ') >= 1) {
            std::vector<std::string> parts;
            for (auto& p : nameParts(n)) parts.push_back(bare(key(p)));
            for (const Entry* o : others) {
                if (o->id == e.id) continue;
                const std::string ok = bare(key(o->name));
                if (std::count(ok.begin(), ok.end(), ' ') >= 1 && std::find(parts.begin(), parts.end(), ok) != parts.end()) consider(o);
            }
            if (only && !twice) return only;
        }
        return nullptr;
    };
    // Who isn't in the bible yet becomes an entry of the kind the relationship points at (a person by default).
    auto kindFor = [&](const std::string& rel) {
        for (auto& c : relationsFor(e.kind))
            if (rel == c.label) {
                std::string t = c.targets;
                t = t.substr(0, t.find(' '));
                return t.empty() ? std::string("character") : t;
            }
        return std::string("character");
    };
    std::string leftover;
    const Id scopeBook = e.worldId.empty() ? e.bookId : Id{};
    for (auto& [label, value] : page.relations) {
        std::string rel = relationFor(e.kind, label);
        // One the bible doesn't know ("Admires"): a kind of relationship of the world's own, with its other side.
        if (rel.empty()) rel = lib.ensureRelation(e.worldId, scopeBook, label);
        if (!rel.empty() && std::find(relabels.begin(), relabels.end(), rel) == relabels.end()) relabels.push_back(rel);
        std::vector<std::string> rest;
        for (auto& nm : splitNames(value)) {
            if (rel.empty()) { rest.push_back(nm); continue; }
            const Entry* o = findNamed(nm);
            if (!o) {
                std::string n = nm;
                if (const size_t paren = n.find('('); paren != std::string::npos) n = trim(n.substr(0, paren));
                if (!n.empty() && n.size() <= 120 && n.find_first_of(".!?") == std::string::npos) {
                    const Id made = lib.createEntry(e.worldId, e.worldId.empty() ? e.bookId : Id{}, kindFor(rel), n);
                    if (!made.empty()) {
                        r.created.push_back(made);
                        others = lib.entriesOf(e.worldId, e.worldId.empty() ? e.bookId : Id{});   // the list moved
                        o = lib.entry(made);
                    }
                }
            }
            if (o) links.push_back({o->id, rel});
            else rest.push_back(nm);
        }
        if (!rest.empty()) {
            std::string v;
            for (auto& x : rest) v += (v.empty() ? "" : ", ") + x;
            leftover += (leftover.empty() ? "" : "\n") + label + ": " + v;
        }
    }
    int changedLines = 0;
    if (!leftover.empty()) details = mergeDetails(details, leftover, changedLines);
    if (details != e.details) {
        int n = 0;
        std::istringstream in(details);
        for (std::string l; std::getline(in, l);) n += !trim(l).empty();
        e.details = details;
        r.details = std::max(1, n);
    }
    std::string notes = page.notes;
    if (!extraNotes.empty()) notes = mergeText(notes, extraNotes);
    if (!notes.empty() && notes != e.notes) { e.notes = notes; r.notes = 1; }
    if (!lib.updateEntry(e)) return false;

    // A race or realm named in the details that the bible doesn't have yet becomes one of its Races or Realms.
    {
        std::istringstream in(e.details);
        for (std::string l; std::getline(in, l);) {
            std::string lab, val;
            if (!splitField(l, lab, val) || val.empty()) continue;
            const std::string k = key(lab);
            const std::string kind = k == "race" || k == "species" ? "race" : k == "realm" || k == "plane" ? "realm" : std::string();
            if (kind.empty() || val.size() > 80) continue;
            std::string v = val;
            if (const size_t paren = v.find('('); paren != std::string::npos) v = trim(v.substr(0, paren));
            if (v.empty() || lib.findEntryNamed(e.worldId, e.worldId.empty() ? e.bookId : Id{}, v, kind)) continue;
            const Id made = lib.createEntry(e.worldId, e.worldId.empty() ? e.bookId : Id{}, kind, v);
            if (!made.empty()) r.created.push_back(made);
        }
    }

    // Relationships: of each kind the paste names, the old links go and the pasted ones come in.
    for (const EntryLink& x : lib.linksOf(e.id)) {
        const bool mine = x.from == e.id;
        const std::string asMine = mine ? x.label : lib.relationReverse(e.worldId, scopeBook, x.label);
        if (std::find(relabels.begin(), relabels.end(), asMine) == relabels.end()) continue;
        const Id other = mine ? x.to : x.from;
        const bool kept = std::any_of(links.begin(), links.end(), [&](const NewLink& l) { return l.to == other && l.label == asMine; });
        if (!kept) lib.removeLink(x.from, x.to, x.label);
    }
    const auto existing = lib.linksOf(e.id);
    for (auto& l : links) {
        const bool have = std::any_of(existing.begin(), existing.end(), [&](const EntryLink& x) {
            return (x.from == e.id && x.to == l.to && x.label == l.label) || (x.to == e.id && x.from == l.to && x.label == lib.relationReverse(e.worldId, scopeBook, l.label));
        });
        if (!have && lib.addLink(e.id, l.to, l.label)) ++r.links;
    }
    return true;
}

std::string Report::words() const {
    std::vector<std::string> parts;
    auto count = [](int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
    if (name) parts.push_back("the name");
    if (aliases) parts.push_back(count(aliases, "other name", "other names"));
    if (pronunciation) parts.push_back("how it's said");
    if (summary) parts.push_back("the summary");
    if (details) parts.push_back(count(details, "detail", "details"));
    if (sections) parts.push_back(count(sections, "section", "sections"));
    if (bookNotes) parts.push_back(count(bookNotes, "book's notes", "books' notes"));
    if (links) parts.push_back(count(links, "link", "links"));
    if (!created.empty()) parts.push_back(count((int)created.size(), "new entry for something the bible didn't have", "new entries for things the bible didn't have"));
    if (notes) parts.push_back("the notes");
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? (i + 1 == parts.size() ? " and " : ", ") : "") + parts[i];
    return out;
}

// ---------------------------------------------------------------- many pages at once

std::string kindNamed(const std::string& wordIn) {
    std::string w = key(wordIn);
    static const std::vector<std::pair<const char*, const char*>> names = {
        {"character", "character"}, {"characters", "character"}, {"people", "character"}, {"person", "character"}, {"cast", "character"},
        {"race", "race"}, {"races", "race"}, {"peoples", "race"}, {"species", "race"},
        {"creature", "creature"}, {"creatures", "creature"}, {"beasts", "creature"}, {"monsters", "creature"}, {"animals", "creature"},
        {"bestiary", "creature"},
        {"place", "place"}, {"places", "place"}, {"locations", "place"}, {"location", "place"}, {"settings", "place"},
        {"realm", "realm"}, {"realms", "realm"}, {"planes", "realm"}, {"realms and planes", "realm"}, {"realms & planes", "realm"},
        {"group", "group"}, {"groups", "group"}, {"factions", "group"}, {"organisations", "group"}, {"organizations", "group"},
        {"guilds", "group"}, {"families", "group"}, {"houses", "group"}, {"orders", "group"},
        {"thing", "thing"}, {"things", "thing"}, {"objects", "thing"}, {"items", "thing"}, {"artefacts", "thing"},
        {"artifacts", "thing"}, {"weapons", "thing"}, {"ships", "thing"},
        {"lore", "lore"}, {"legends", "lore"}, {"myths", "lore"}, {"beliefs", "lore"}, {"religions", "lore"},
        {"magic", "magic"}, {"magic systems", "magic"}, {"magic and powers", "magic"}, {"magic & powers", "magic"}, {"powers", "magic"},
        {"spells", "magic"},
        {"event", "event"}, {"events", "event"}, {"battles", "event"}, {"wars", "event"}, {"timeline", "event"}, {"history", "event"},
    };
    for (auto& [n, k] : names) if (w == n) return k;
    return {};
}

namespace {

// A heading naming a kind: "# Characters", "**Places**", "== Creatures ==", "Realms:", "PLACES". Marked as a heading,
// or a plural on its own, so a section called "History" inside a page isn't taken for one.
std::string kindHeading(const std::string& raw) {
    std::string l = trim(raw);
    bool marked = false;
    if (!l.empty() && l[0] == '#') { while (!l.empty() && l[0] == '#') l.erase(0, 1); marked = true; }
    for (const char* m : {"==", "--"})
        if (startsWith(l, m)) {
            while (!l.empty() && (l[0] == '=' || l[0] == '-')) l.erase(0, 1);
            while (!l.empty() && (l.back() == '=' || l.back() == '-')) l.pop_back();
            marked = true;
        }
    if (startsWith(l, "**") || startsWith(l, "__")) marked = true;
    l = withoutEmphasis(trim(l));
    if (l.empty() || l.size() > 40) return {};
    if (l.back() == ':') { marked = true; l.pop_back(); }
    const std::string k = kindNamed(l);
    if (k.empty()) return {};
    const std::string w = key(l);
    const bool plural = w.size() > 1 && (w.back() == 's' || w == "people" || w == "lore" || w == "magic" || w == "species" ||
                                         w == "cast" || w == "bestiary" || w == "timeline");
    bool caps = true;
    for (char c : l) if (std::islower((unsigned char)c)) caps = false;
    return (marked || plural || caps) ? k : std::string();
}

// "Name: Tom" -> "Tom"; "Name:" alone -> "" with `bare` set.
bool nameLine(const std::string& raw, std::string& value, bool& bare) {
    const std::string l = withoutEmphasis(trim(raw));
    std::string lab, val;
    if (!l.empty() && key(l) == "name" && (l.back() == ':' || l == "Name")) { value.clear(); bare = true; return true; }
    if (!splitField(l, lab, val) || key(lab) != "name") return false;
    value = trim(val);
    bare = value.empty();
    return true;
}

}  // namespace

std::vector<Batch> splitPages(const std::string& text, const std::string& defaultKind) {
    std::vector<std::string> lines;
    {
        std::string cur;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); lines.push_back(cur); cur.clear(); }
            else cur += text[i];
        }
    }
    // With no "Name:" lines, pages are marked by headings (a Word document's, or Markdown's): the shallowest level of
    // heading that isn't a kind ("Characters") or a part of a page ("Summary") is the pages' titles. They become
    // "# Title", deeper headings "## Heading" (the page's sections), and shallower ones are left out.
    {
        bool names = false;
        int pageLevel = 99;
        auto headingOf = [](const std::string& l, int& level) -> std::string {
            const std::string t = trim(l);
            size_t h = 0;
            while (h < t.size() && t[h] == '#') ++h;
            if (h == 0 || h >= t.size() || t[h] != ' ') return {};
            level = (int)h;
            return withoutEmphasis(trim(t.substr(h)));
        };
        for (auto& l : lines) {
            std::string v;
            bool bare = false;
            if (nameLine(l, v, bare)) { names = true; break; }
            int level = 0;
            const std::string hd = headingOf(l, level);
            if (hd.empty() || !kindNamed(hd).empty() || slotOf(key(hd)) != Slot::None) continue;
            pageLevel = std::min(pageLevel, level);
        }
        if (!names && pageLevel < 99)
            for (auto& l : lines) {
                int level = 0;
                const std::string hd = headingOf(l, level);
                if (hd.empty()) continue;
                if (!kindNamed(hd).empty() && level <= pageLevel) l = "# " + hd;
                else if (level == pageLevel && slotOf(key(hd)) == Slot::None) l = "# " + hd;
                else if (level > pageLevel || slotOf(key(hd)) != Slot::None) l = "## " + hd;
                else l.clear();
            }
    }
    struct Open { std::string kind, ownKind, text; bool named = false, fromTitle = false; int since = 0; };
    std::vector<Open> pages;
    std::string kind = defaultKind;
    auto start = [&]() { pages.push_back({kind, {}, {}, false, false, 0}); };
    auto kindLine = [&](const std::string& t) -> std::string {   // "Kind: Place" -> "place"
        std::string lab, val;
        if (splitField(withoutEmphasis(t), lab, val) && (key(lab) == "kind" || key(lab) == "entry kind" || key(lab) == "category"))
            return kindNamed(val);
        return {};
    };
    // Whether a page starts at the next line with words on it (a Name: line, a "# Title" or a Kind: line).
    auto pageNext = [&](size_t i) {
        for (size_t j = i + 1; j < lines.size(); ++j) {
            const std::string t = trim(lines[j]);
            if (t.empty() || isSeparator(t)) continue;
            std::string v;
            bool bare = false;
            return nameLine(t, v, bare) || (t.size() > 2 && t[0] == '#' && t[1] == ' ') || !kindLine(t).empty() || !kindHeading(t).empty();
        }
        return false;
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& raw = lines[i];
        const std::string t = trim(raw);
        if (t.empty()) { if (!pages.empty()) pages.back().text += "\n"; continue; }
        // A heading naming a kind, with a page after it (a page's own "## Magic" section has its text after it instead).
        if (const std::string k = kindHeading(t); !k.empty() && (pageNext(i) || (t.size() > 2 && t[0] == '#' && t[1] == ' '))) { kind = k; if (!pages.empty()) pages.back().text += "\n"; continue; }
        // "Kind: Place": the page it's in, or the one it comes just before.
        if (const std::string k = kindLine(t); !k.empty()) {
            std::string v;
            bool bare = false;
            bool beforeName = false;
            for (size_t j = i + 1; j < lines.size(); ++j) {
                const std::string n = trim(lines[j]);
                if (n.empty()) continue;
                beforeName = nameLine(n, v, bare) || (n.size() > 2 && n[0] == '#' && n[1] == ' ');
                break;
            }
            if (beforeName) {
                start();   // the page to come (its Name: line joins it)
                pages.back().fromTitle = false;
            } else if (pages.empty()) start();
            pages.back().ownKind = k;
            continue;
        }
        std::string value;
        bool bare = false;
        if (nameLine(t, value, bare)) {
            const bool fresh = !pages.empty() && pages.back().fromTitle && pages.back().since == 0;
            if (pages.empty() || (pages.back().named && !fresh)) start();
            Open& pg = pages.back();
            if (fresh) pg.text.clear();
            pg.named = true;
            pg.fromTitle = false;
            pg.since = 0;
            pg.text += raw + "\n";
            continue;
        }
        // "# Tom Smith": a page of that name (## and deeper are the page's own headings).
        if (t.size() > 2 && t[0] == '#' && t[1] == ' ') {
            const std::string title = withoutEmphasis(trim(t.substr(2)));
            if (!title.empty() && title.size() <= 120) {
                if (pages.empty() || pages.back().named) start();
                pages.back().named = pages.back().fromTitle = true;
                pages.back().text = "Name: " + title + "\n";
                continue;
            }
        }
        if (pages.empty()) continue;   // before the first page
        if (isSeparator(t)) { pages.back().text += "\n"; continue; }
        pages.back().text += raw + "\n";
        ++pages.back().since;
    }
    std::vector<Batch> out;
    for (auto& pg : pages) {
        if (!pg.named) continue;
        const Page parsed = parse(pg.text);
        if (parsed.name.empty()) continue;
        out.push_back({pg.ownKind.empty() ? pg.kind : pg.ownKind, parsed.name, pg.text});
    }
    return out;
}

std::string documentPagesText(const std::vector<std::pair<std::string, std::string>>& sections) {
    std::string text;
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto& [title, json] = sections[i];
        if (!title.empty() && !(i == 0 && (title == "Front matter" || title == "Opening"))) text += "# " + title + "\n\n";
        text += doc::outlineText(json, 1) + "\n\n";
    }
    return text;
}

bool looksLikeManyPages(const std::string& text) { return splitPages(text, "character").size() >= 2; }

bool importPages(Library& lib, const Id& world, const Id& book, const std::vector<Batch>& pages, const std::vector<Id>& booksInOrder,
                 Import& out) {
    const Id scopeBook = world.empty() ? book : Id{};
    std::vector<Id> target(pages.size());
    std::set<Id> seen;
    // Every page's entry first: found, or made, so pages naming each other link.
    for (size_t i = 0; i < pages.size(); ++i) {
        const Batch& b = pages[i];
        const Entry* e = lib.findEntryNamed(world, scopeBook, b.name, b.kind);
        if (!e) e = lib.findEntryNamed(world, scopeBook, b.name);
        if (e) {
            target[i] = e->id;
            if (seen.insert(e->id).second && std::find(out.made.begin(), out.made.end(), e->id) == out.made.end()) {
                out.updated.push_back(e->id);
                out.before.push_back(*e);
                out.linksBefore.push_back(lib.linksOf(e->id));
            }
            continue;
        }
        const Id id = lib.createEntry(world, scopeBook, b.kind, b.name);
        if (id.empty()) return false;
        target[i] = id;
        seen.insert(id);
        out.made.push_back(id);
    }
    std::set<Id> changed;
    for (size_t i = 0; i < pages.size(); ++i) {
        Report r;
        if (!apply(lib, target[i], parse(pages[i].text), booksInOrder, r)) return false;
        out.links += r.links;
        for (const Id& x : r.created) out.extra.push_back(x);
        if (!r.words().empty()) changed.insert(target[i]);
    }
    for (const Id& id : out.updated) if (!changed.count(id)) ++out.unchanged;
    return true;
}

std::string Import::words() const {
    std::vector<std::string> parts;
    auto count = [](size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
    if (!made.empty()) parts.push_back(count(made.size(), "new entry", "new entries"));
    if (updated.size() > (size_t)unchanged) parts.push_back(count(updated.size() - unchanged, "entry updated", "entries updated"));
    if (links) parts.push_back(count((size_t)links, "link", "links"));
    if (!extra.empty()) parts.push_back(count(extra.size(), "entry made for a name the pages mention", "entries made for names the pages mention"));
    std::string o;
    for (size_t i = 0; i < parts.size(); ++i) o += (i ? (i + 1 == parts.size() ? " and " : ", ") : "") + parts[i];
    return o;
}

}  // namespace studio::paste

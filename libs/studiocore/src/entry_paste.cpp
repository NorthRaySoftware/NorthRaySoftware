#include "studio/entry_paste.hpp"

#include <algorithm>
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
            setSingle(awaiting, c);
            awaiting = Slot::None;
            continue;
        }

        // A known heading on its own ("Summary", "**Details**", "## Notes per book", "Family:").
        const Slot known = slotOf(key(c));
        if (known != Slot::None && words(c) <= 5) {
            if (known == Slot::Name || known == Slot::Aliases || known == Slot::Say) { awaiting = known; ++p.parts; ++p.known; block = Slot::None; continue; }
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
                                  (i + 1 >= lines.size() || !trim(lines[i + 1]).empty());
        if ((marked && words(c) <= 8) || plainHeading) {
            std::string t = c;
            while (!t.empty() && (t.back() == ':' || t.back() == ' ')) t.pop_back();
            p.sections.emplace_back(t, std::string());
            section = p.sections.size() - 1;
            startBlock(Slot::Section);
            continue;
        }

        // Text in the block it's under.
        if (block == Slot::Details) { addDetail(c); continue; }
        if (block == Slot::Family || block == Slot::Relations) { addDetail(c); continue; }
        if (block == Slot::BookNotes && p.bookNotes.empty()) { p.bookNotes.emplace_back(std::string(), std::string()); bookNote = 0; }
        if (std::string* t = textOf(block)) addLine(*t, gap, bullet ? "\xE2\x80\xA2 " + c : c);
        // Before anything recognised (a chat's "Here's the entry…"): left out.
    }
    // Sections with nothing under them aren't kept.
    p.sections.erase(std::remove_if(p.sections.begin(), p.sections.end(), [](auto& s) { return trim(s.second).empty(); }), p.sections.end());
    p.bookNotes.erase(std::remove_if(p.bookNotes.begin(), p.bookNotes.end(), [](auto& s) { return trim(s.second).empty(); }), p.bookNotes.end());
    return p;
}

bool looksLikePage(const std::string& text) {
    if (text.find('\n') == std::string::npos) return false;
    const Page p = parse(text);
    return p.parts >= 2 && p.known >= 1;
}

// ---------------------------------------------------------------- relationships

std::string relationFor(const std::string& kind, const std::string& labelIn) {
    std::string k = key(labelIn);
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
        {{"mentor", "mentors", "teacher", "teachers", "master", "trained by", "taught by"}, "Student of"},
        {{"students", "student", "pupil", "pupils", "former pupil", "former pupils", "apprentice", "apprentices", "trainee"}, "Mentor of"},
        {{"relatives", "relative", "family", "uncle", "aunt", "cousin", "cousins", "grandparents", "niece", "nephew"}, "Family"},
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
    if (!want.empty()) return offered(want);
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
    const auto others = lib.entriesOf(e.worldId, e.worldId.empty() ? e.bookId : Id{});
    auto findNamed = [&](const std::string& nm) -> const Entry* {
        std::string n = nm;
        if (const size_t paren = n.find('('); paren != std::string::npos) n = trim(n.substr(0, paren));
        const std::string k = key(n);
        if (k.empty()) return nullptr;
        for (const Entry* o : others) if (o->id != e.id && key(o->name) == k) return o;
        const Entry* only = nullptr;
        for (const Entry* o : others) {
            if (o->id == e.id) continue;
            for (auto& s : searchNames(*o))
                if (key(s) == k) {
                    if (only && only != o) return nullptr;   // two entries go by it: not guessed
                    only = o;
                }
        }
        return only;
    };
    std::string leftover;
    for (auto& [label, value] : page.relations) {
        const std::string rel = relationFor(e.kind, label);
        if (!rel.empty() && std::find(relabels.begin(), relabels.end(), rel) == relabels.end()) relabels.push_back(rel);
        std::vector<std::string> rest;
        for (auto& nm : splitNames(value)) {
            const Entry* o = rel.empty() ? nullptr : findNamed(nm);
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

    // Relationships: of each kind the paste names, the old links go and the pasted ones come in.
    for (const EntryLink& x : lib.linksOf(e.id)) {
        const bool mine = x.from == e.id;
        const std::string asMine = mine ? x.label : reverseRelation(x.label);
        if (std::find(relabels.begin(), relabels.end(), asMine) == relabels.end()) continue;
        const Id other = mine ? x.to : x.from;
        const bool kept = std::any_of(links.begin(), links.end(), [&](const NewLink& l) { return l.to == other && l.label == asMine; });
        if (!kept) lib.removeLink(x.from, x.to, x.label);
    }
    const auto existing = lib.linksOf(e.id);
    for (auto& l : links) {
        const bool have = std::any_of(existing.begin(), existing.end(), [&](const EntryLink& x) {
            return (x.from == e.id && x.to == l.to && x.label == l.label) || (x.to == e.id && x.from == l.to && x.label == reverseRelation(l.label));
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
    if (notes) parts.push_back("the notes");
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? (i + 1 == parts.size() ? " and " : ", ") : "") + parts[i];
    return out;
}

}  // namespace studio::paste

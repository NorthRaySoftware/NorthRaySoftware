#include "studio/entry_paste.hpp"
#include "studio/document.hpp"
#include "studio/artwork.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

enum class Slot { None, Name, Aliases, Say, Summary, Details, Notes, BookNotes, Family, Relations, Spoilers, Section,
                  Picture, Gallery, Formerly, Remove, MergeInto, MergeFrom };

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
        {"links", Slot::Relations}, {"spoilers", Slot::Spoilers}, {"spoiler", Slot::Spoilers},
        // 0.55
        {"picture", Slot::Picture}, {"main picture", Slot::Picture}, {"image", Slot::Picture}, {"portrait", Slot::Picture},
        {"gallery", Slot::Gallery}, {"pictures", Slot::Gallery}, {"images", Slot::Gallery},
        {"formerly", Slot::Formerly}, {"former name", Slot::Formerly}, {"former names", Slot::Formerly}, {"previously", Slot::Formerly},
        {"renamed from", Slot::Formerly}, {"old name", Slot::Formerly}, {"remove", Slot::Remove}, {"delete", Slot::Remove},
        {"merge into", Slot::MergeInto}, {"merged into", Slot::MergeInto}, {"merge with", Slot::MergeInto},
        {"merged from", Slot::MergeFrom}, {"merge in", Slot::MergeFrom}, {"merges in", Slot::MergeFrom}};
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

std::vector<std::string> splitList(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + sep) {
        if (c == sep) {
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
        } else cur += c;
    }
    return out;
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
    auto unquote = [](std::string t) {
        t = trim(t);
        if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'') && t.back() == t.front()) t = trim(t.substr(1, t.size() - 2));
        return t;
    };
    // A gallery line: "path | caption" (or a tab between).
    auto addGallery = [&](const std::string& line) {
        std::string path = line, caption;
        size_t bar = line.find(" | ");
        if (bar == std::string::npos) bar = line.find('|');
        if (bar != std::string::npos) {
            path = line.substr(0, bar);
            caption = trim(line.substr(bar + (line.compare(bar, 3, " | ") == 0 ? 3 : 1)));
        }
        path = unquote(path);
        if (!path.empty()) p.gallery.emplace_back(path, caption);
    };
    auto setSingle = [&](Slot s, const std::string& v) {
        if (v.empty()) return;
        if (s == Slot::Picture) { p.picture = unquote(v); return; }
        if (s == Slot::Gallery) { for (auto& g : splitList(v, ';')) addGallery(g); return; }
        if (s == Slot::Formerly) { for (auto& f : splitNames(v)) p.formerly.push_back(unquote(f)); return; }
        if (s == Slot::MergeFrom) { for (auto& f : splitNames(v)) p.mergeFrom.push_back(unquote(f)); return; }
        if (s == Slot::MergeInto) { p.mergeInto = unquote(v); return; }
        if (s == Slot::Remove) {
            const std::string k = key(v);
            p.remove = k == "yes" || k == "true" || k == "y" || k == "remove" || k == "delete" || k == "remove it" || k == "delete it";
            return;
        }
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
            if (known == Slot::Name || known == Slot::Aliases || known == Slot::Say || known == Slot::Picture || known == Slot::Formerly ||
                known == Slot::MergeInto || known == Slot::MergeFrom || known == Slot::Remove) {
                awaiting = known;
                ++p.parts;
                ++p.known;
                block = Slot::None;
                continue;
            }
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

        // Under "Gallery": each line a picture's path, with " | caption" after it (paths have colons in them, so this comes
        // before "Label: value").
        if (block == Slot::Gallery) {
            std::string lab, val;
            const bool field = splitField(c, lab, val) && slotOf(key(lab)) != Slot::None;   // "Summary: …" ends the gallery
            // A path: a folder in it, or a file's extension ("tom.png"); "Appearance" on its own is a heading.
            std::string path = c.substr(0, c.find('|'));
            while (!path.empty() && path.back() == ' ') path.pop_back();
            const size_t dot = path.rfind('.');
            const bool pathLike = path.find_first_of("/\\") != std::string::npos ||
                                  (dot != std::string::npos && dot + 1 < path.size() && path.size() - dot <= 6 &&
                                   std::all_of(path.begin() + (long)dot + 1, path.end(), [](unsigned char ch) { return std::isalnum(ch); }));
            if (!field && !marked && pathLike) {
                addGallery(c);
                continue;
            }
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
            if (!value.empty() && (s == Slot::Name || s == Slot::Aliases || s == Slot::Say || s == Slot::Picture || s == Slot::Formerly ||
                                   s == Slot::Remove || s == Slot::MergeInto || s == Slot::MergeFrom || s == Slot::Gallery)) {
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
        if (block == Slot::BookNotes) gap = true;   // each line of a book's note is a paragraph of its own (0.56)
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
    if (kind == "character") details = {"Gender", "Sexuality", "Race", "Realm", "Age", "Birthday", "Height", "Hair", "Eyes", "Marks", "Clothing", "Weapon", "Home", "Occupation", "Title"};
    else if (kind == "creature") details = {"Realm", "Habitat", "Size", "Diet", "Lifespan", "Danger"};
    else if (kind == "race") details = {"Realm", "Homeland", "Lifespan", "Language", "Population"};
    else if (kind == "place") details = {"Type", "Realm", "Location", "Climate", "Population", "Ruled by", "Founded"};
    else if (kind == "realm") details = {"Ruled by", "Reached by", "Time there"};
    else if (kind == "group") details = {"Leader", "Base", "Founded", "Members", "Symbol", "Colours"};
    else if (kind == "thing") details = {"Type", "Made by", "Made of", "Owner", "Where it is"};
    else if (kind == "magic") details = {"Source", "Colour", "Cost", "Difficulty"};
    else if (kind == "event") details = {"When", "Where", "Outcome"};
    else details = {"When", "Where"};
    // Picture: and Gallery: take paths to picture files (0.55).
    std::string t = "Name: \nOther names: \nHow it's said: \nPicture: \nGallery: \n\nSummary\n\n\nDetails\n";
    for (auto* d : details) t += std::string(d) + ": \n";
    for (auto& s : sections) t += "\n" + s + "\n\n";
    t += "\nNotes per book\n";
    if (bookTitles.empty()) t += "Book 1: \n";
    for (auto& b : bookTitles) t += b + ": \n";
    // Each kind of family has its own line (0.46); leave any blank.
    if (kind == "character")
        t += "\nFamily\nParents: \nAdoptive parents: \nStep-parents: \nFoster parents: \nPartner: \nSpouse: \nFianc\xC3\xA9: \nFormer partners: \n"
             "Siblings: \nTwin: \nTriplets: \nHalf-siblings: \nStep-siblings: \nAdoptive siblings: \nFoster siblings: \nChildren: \n"
             "Adopted children: \nStepchildren: \nFoster children: \n";
    else if (kind == "creature")
        t += "\nFamily\nParents: \nPartner: \nSiblings: \nTwin: \nChildren: \n";
    t += "\nRelationships\n";
    std::vector<std::string> rel;
    if (kind == "character") rel = {"Friends", "Enemies", "Taught by", "Member of", "From", "Lives in", "Rides", "Owns"};
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
    // A partner who no longer is ("ex-wife", "former partner", "divorced from") is a former partner (0.57); a late one
    // stays a partner.
    {
        static const std::vector<const char*> partnerWords{"partner", "partners", "wife", "wives", "husband", "husbands", "spouse",
                                                           "girlfriend", "boyfriend", "lover", "fiance", "fiancee", "fiancé", "fiancée"};
        for (const char* pre : {"former ", "ex-", "ex ", "one-time ", "estranged "})
            if (k.rfind(pre, 0) == 0) {
                const std::string rest = k.substr(std::string(pre).size());
                if (std::find(partnerWords.begin(), partnerWords.end(), rest) != partnerWords.end()) k = "former partner of";
            }
        static const std::vector<const char*> exes{"ex", "exes", "divorced from", "divorced", "separated from", "former partner of"};
        if (std::find(exes.begin(), exes.end(), k) != exes.end()) {
            for (auto& c : relationsFor(kind)) if (std::string(c.label) == "Former partner of") return c.label;
        }
    }
    // "Former teacher", "late husband", "ex-wife": the same relationship.
    for (const char* pre : {"former ", "ex-", "ex ", "late ", "old ", "one-time ", "current ", "sworn "})
        if (k.rfind(pre, 0) == 0 && k.size() > std::string(pre).size() + 2) { k = k.substr(std::string(pre).size()); break; }
    static const std::vector<std::pair<std::vector<const char*>, const char*>> table{
        {{"parents", "parent", "father", "mother", "mum", "mom", "dad", "birth parents", "birth parent", "birth mother", "birth father",
          "biological parents", "biological mother", "biological father"}, "Child of"},
        {{"children", "child", "son", "sons", "daughter", "daughters", "kids"}, "Parent of"},
        {{"siblings", "sibling", "brother", "brothers", "sister", "sisters", "full brother", "full sister"}, "Sibling of"},
        {{"twin", "twins", "twin brother", "twin sister", "twin sibling", "twin of"}, "Twin of"},
        {{"triplet", "triplets", "triplet brother", "triplet brothers", "triplet sister", "triplet sisters", "triplet sibling",
          "triplet siblings", "triplet of"}, "Triplet of"},
        {{"half-sibling", "half-siblings", "half sibling", "half siblings", "half-brother", "half-brothers", "half brother", "half brothers",
          "half-sister", "half-sisters", "half sister", "half sisters"}, "Half-sibling of"},
        {{"step-sibling", "step-siblings", "stepsibling", "stepsiblings", "step sibling", "stepbrother", "stepbrothers", "step-brother",
          "step brother", "stepsister", "stepsisters", "step-sister", "step sister"}, "Step-sibling of"},
        {{"adoptive sibling", "adoptive siblings", "adoptive brother", "adoptive sister", "adopted brother", "adopted sister",
          "adopted sibling", "adopted siblings"}, "Adoptive sibling of"},
        {{"foster sibling", "foster siblings", "foster brother", "foster brothers", "foster sister", "foster sisters"}, "Foster sibling of"},
        {{"adoptive parents", "adoptive parent", "adoptive father", "adoptive mother", "adoptive mum", "adoptive dad", "adopted by"},
         "Adopted child of"},
        {{"adopted children", "adopted child", "adopted son", "adopted sons", "adopted daughter", "adopted daughters"}, "Adoptive parent of"},
        {{"step-parents", "step-parent", "stepparents", "stepparent", "stepfather", "step-father", "stepmother", "step-mother", "stepmum",
          "stepmom", "stepdad", "step-mum", "step-dad"}, "Stepchild of"},
        {{"stepchildren", "stepchild", "step-children", "step-child", "stepson", "stepsons", "stepdaughter", "stepdaughters", "step-son",
          "step-daughter"}, "Step-parent of"},
        {{"foster parents", "foster parent", "foster father", "foster mother", "foster mum", "foster dad", "fostered by"}, "Foster child of"},
        {{"foster children", "foster child", "foster son", "foster sons", "foster daughter", "foster daughters"}, "Foster parent of"},
        {{"partner", "partners", "girlfriend", "boyfriend", "lover", "mate", "mates"}, "Partner of"},
        {{"betrothed", "fiance", "fiancee", "fiancé", "fiancée", "fiances", "fiancees", "fiancés", "fiancées", "engaged to", "engaged",
          "betrothed to", "intended", "husband-to-be", "wife-to-be"}, "Engaged to"},
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
        {{"uses", "use", "used"}, "Used"},
        {{"used by", "users", "known users", "wielded by", "practitioners", "practised by", "practiced by"}, "Practised by"},
        {{"from", "is from", "hails from", "hometown", "home town", "home realm", "comes from", "origin", "place of origin"}, "From"},
        {{"source", "sources", "origin", "comes from", "from"}, "Comes from"},
        {{"opposed by", "opposes", "opposite", "counter", "countered by"}, "Enemy of"},
        {{"wields", "weapon", "weapons", "carries"}, "Owns"},
        {{"associated with", "connected to", "linked to", "tied to", "see also"}, "Related to"}};
    const auto& choices = relationsFor(kind);
    auto offered = [&](const std::string& l) -> std::string {
        for (auto& c : choices) if (key(c.label) == key(l)) return c.label;
        return {};
    };
    // The first row naming the word whose relationship this kind has (0.60: "from" is From for a character and Comes
    // from for a race).
    for (auto& [names, rel] : table)
        if (std::find(names.begin(), names.end(), k) != names.end())
            if (std::string r = offered(rel); !r.empty()) return r;
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

// Whether a label before a colon reads as a book's title ("Shards of Light", "The Envoy of Telvana", "Book 5") rather
// than the start of a sentence ("Later", "In the end", "After the battle").
bool looksLikeBookTitle(const std::string& label) {
    const std::string k = key(label);
    if (k.empty()) return false;
    static const std::regex bookN(R"(^book\s+(\d+|one|two|three|four|five|six|seven|eight|nine|ten)\b.*)");
    if (std::regex_match(k, bookN) || k.find("(book") != std::string::npos) return true;
    static const std::set<std::string> openers = {"later", "then", "meanwhile", "afterwards", "afterward", "finally", "eventually", "also",
                                                  "however", "note", "notes", "earlier", "before", "after", "now", "here", "there",
                                                  "first", "second", "third", "next", "last", "lastly", "still", "yet", "so", "but",
                                                  "and", "in the end", "at last", "by the end", "in short", "overall", "spoiler", "aside"};
    if (openers.count(k)) return false;
    // Every word of four letters or more starts with a capital (or a digit).
    std::istringstream in(trim(label));
    int words = 0;
    for (std::string wd; in >> wd;) {
        ++words;
        while (!wd.empty() && !std::isalnum((unsigned char)wd[0]) && (unsigned char)wd[0] < 0x80) wd.erase(0, 1);
        if (wd.size() >= 4 && std::islower((unsigned char)wd[0])) return false;
    }
    return words > 0 && words <= 10 && (std::isupper((unsigned char)trim(label)[0]) || std::isdigit((unsigned char)trim(label)[0]) ||
                                        (unsigned char)trim(label)[0] >= 0x80);
}

}  // namespace

const Book* bookForLabel(const std::vector<const Book*>& books, const std::string& label, const Library* lib) {
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
    for (const Book* b : books) if (!tk.empty() && key(b->title) == tk) return b;
    for (const Book* b : books)
        if (!tk.empty() && key(b->title).size() >= 4 && (tk.find(key(b->title)) != std::string::npos || key(b->title).find(tk) != std::string::npos))
            return b;
    // Series (0.60): each counts its own books. The series named in the label ("Alvora Chronicles Book 2", "Book 2 of
    // the Envoy series"), else the one series with a book of that number.
    if (lib && n >= 1) {
        struct In { const Series* s; std::vector<std::pair<const Book*, std::string>> books; };
        std::vector<In> series;
        for (const Book* b : books)
            for (const Series* s : lib->seriesOfBook(b->id)) {
                auto it = std::find_if(series.begin(), series.end(), [&](const In& x) { return x.s->id == s->id; });
                if (it == series.end()) { series.push_back({s, {}}); it = series.end() - 1; }
                const Membership* m = lib->membership(s->id, b->id);
                it->books.push_back({b, m ? trim(m->number) : std::string()});
            }
        auto numbered = [&](const In& in) -> const Book* {
            for (auto& [b, num] : in.books) if (!num.empty() && (num == std::to_string(n) || numberWord(key(num)) == n)) return b;
            return nullptr;
        };
        auto bare = [](std::string s) { s = key(s); if (s.rfind("the ", 0) == 0) s = s.substr(4); return s; };
        const std::string lk = key(label);
        for (const In& in : series) {
            const std::string sn = bare(in.s->name);
            std::string core = sn;
            for (const char* tail : {" series", " chronicles", " saga", " cycle", " trilogy"})
                if (core.size() > std::strlen(tail) + 3 && core.compare(core.size() - std::strlen(tail), std::strlen(tail), tail) == 0) core.erase(core.size() - std::strlen(tail));
            if (!sn.empty() && (lk.find(sn) != std::string::npos || (core.size() >= 4 && lk.find(core) != std::string::npos))) return numbered(in);
        }
        if (series.size() > 1) {
            const Book* one = nullptr;
            int count = 0;
            for (const In& in : series) if (const Book* b = numbered(in)) { one = b; ++count; }
            return count == 1 ? one : nullptr;
        }
        if (series.size() == 1) if (const Book* b = numbered(series[0])) return b;
    }
    // "Book 2" alone: the second in reading order. A title that's no book's isn't guessed by its number.
    const bool titled = !tk.empty() && tk.rfind("book", 0) != 0;
    if (!titled && n >= 1 && n <= (int)books.size()) return books[(size_t)n - 1];
    return nullptr;
}

std::vector<std::pair<std::string, std::string>> laterBookNotes(const Entry& e) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [k, text] : e.bookNotes)
        if (k.rfind(kLaterBook, 0) == 0 && !text.empty()) out.emplace_back(k.substr(std::string(kLaterBook).size()), text);
    return out;
}

int claimLaterBookNotes(Library& lib, const Id& world, const Id& book, const std::vector<Id>& booksInOrder) {
    std::vector<const Book*> books;
    for (const Id& b : booksInOrder) if (const Book* bk = lib.book(b)) books.push_back(bk);
    if (books.empty()) return 0;
    int moved = 0;
    std::vector<Entry> changed;
    for (const Entry* e : lib.entriesOf(world, world.empty() ? book : Id{})) {
        bool any = false;
        Entry x = *e;
        for (auto& [label, text] : laterBookNotes(*e))
            if (const Book* b = bookForLabel(books, label, &lib)) {
                x.setBookNote(b->id, mergeText(x.bookNote(b->id), text));
                x.setBookNote(std::string(kLaterBook) + label, "");
                ++moved;
                any = true;
            }
        if (any) changed.push_back(x);
    }
    for (auto& x : changed) lib.updateEntry(x);
    return moved;
}

namespace {

}  // namespace

bool apply(Library& lib, const Id& entryId, const Page& page, const std::vector<Id>& booksInOrder, Report& r, const std::string& baseFolder) {
    const Entry* cur = lib.entry(entryId);
    if (!cur) return false;
    Entry e = *cur;
    // Pictures by path (0.55): read, checked to be a picture, kept in Artwork; the blob is what the entry holds.
    auto readPicture = [&](const std::string& path) -> std::string {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path fp = fs::u8path(path);
        if (fp.is_relative() && !baseFolder.empty()) fp = fs::u8path(baseFolder) / fp;
        if (!fs::is_regular_file(fp, ec) || fs::file_size(fp, ec) > 80u * 1024 * 1024) return {};
        std::ifstream f(fp, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.empty() || !imageInfo(bytes).ok) return {};
        const auto stem = fp.stem().u8string();
        const Id aid = lib.addArtwork(bytes, std::string(stem.begin(), stem.end()), "Bible");
        const Artwork* a = aid.empty() ? nullptr : lib.artwork(aid);
        return a ? a->blob : std::string();
    };
    auto inGallery = [&](const std::string& blob) {
        return std::find_if(e.gallery.begin(), e.gallery.end(), [&](const Entry::Picture& g) { return g.blob == blob; });
    };
    if (!page.picture.empty()) {
        const std::string blob = readPicture(page.picture);
        if (blob.empty()) r.missingPictures.push_back(page.picture);
        else {
            if (e.imageBlob != blob) { e.imageBlob = blob; r.picture = true; }
            if (inGallery(blob) == e.gallery.end()) e.gallery.push_back({blob, ""});
        }
    }
    for (auto& [path, caption] : page.gallery) {
        const std::string blob = readPicture(path);
        if (blob.empty()) { r.missingPictures.push_back(path); continue; }
        auto it = inGallery(blob);
        if (it == e.gallery.end()) {
            e.gallery.push_back({blob, caption});
            ++r.gallery;
        } else if (!caption.empty() && it->caption != caption) {
            it->caption = caption;
            ++r.gallery;
        }
        if (e.imageBlob.empty()) { e.imageBlob = blob; r.picture = true; }   // the first picture is the main one
    }
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

    // Notes for each book (0.56: a title with a colon in it; a label that's a sentence's start goes with the note before;
    // a book that isn't in Writing Studio yet keeps its note under its title until it is).
    std::vector<const Book*> books;
    for (const Id& b : booksInOrder) if (const Book* bk = lib.book(b)) books.push_back(bk);
    std::string extraNotes;
    std::string lastKey;   // the note written last, for a paragraph whose start looked like a label
    for (auto [label, text] : page.bookNotes) {
        const Book* hit = nullptr;
        if (!label.empty()) {
            // "Alvora: The Fall of Valona: …" was read as the label "Alvora" and the rest.
            for (const Book* b : books) {
                const size_t colon = b->title.find(':');
                if (colon == std::string::npos || key(b->title.substr(0, colon)) != key(label)) continue;
                const std::string rest = trim(b->title.substr(colon + 1));
                std::string lt = text, lr = rest;
                for (auto& ch : lt) ch = (char)std::tolower((unsigned char)ch);
                for (auto& ch : lr) ch = (char)std::tolower((unsigned char)ch);
                if (rest.empty() || lt.rfind(lr, 0) != 0) continue;
                text = trim(text.substr(rest.size()));
                if (!text.empty() && text[0] == '(') {   // "(Book 4)"
                    const size_t close = text.find(')');
                    if (close != std::string::npos && close < 16) text = trim(text.substr(close + 1));
                }
                while (!text.empty() && (text[0] == ':' || text[0] == '.' || text[0] == '-' || text[0] == ' ')) text.erase(0, 1);
                if (text.rfind("\xE2\x80\x94", 0) == 0 || text.rfind("\xE2\x80\x93", 0) == 0) text = trim(text.substr(3));
                hit = b;
                break;
            }
        }
        if (!hit) hit = bookForLabel(books, label, &lib);
        if (hit) {
            if (e.bookNote(hit->id) != text) { e.setBookNote(hit->id, text); ++r.bookNotes; }
            lastKey = hit->id;
        } else if (!label.empty() && !lastKey.empty() && !looksLikeBookTitle(label)) {
            // "Later: he returns" under a book's note: more of that note, not another book.
            const std::string more = label + ": " + text;
            e.setBookNote(lastKey, mergeText(e.bookNote(lastKey), more));
        } else if (!label.empty()) {
            // A book that isn't here yet: kept under its title until it is.
            const std::string k = std::string(kLaterBook) + label;
            if (e.bookNote(k) != text) { e.setBookNote(k, text); ++r.bookNotes; }
            r.unmatchedBooks.push_back(label);
            lastKey = k;
        } else {
            extraNotes = mergeText(extraNotes, text);
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
    if (picture) parts.push_back("the picture");
    if (gallery) parts.push_back(count(gallery, "gallery picture", "gallery pictures"));
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

bool clearEntryText(Library& lib, const Id& entryId) {
    const Entry* e = lib.entry(entryId);
    if (!e) return false;
    Entry x = *e;
    x.aliases.clear();
    x.pronunciation.clear();
    x.summary.clear();
    x.details.clear();
    x.sections.clear();
    x.bookNotes.clear();
    x.notes.clear();
    x.revealed.clear();
    if (!lib.updateEntry(x)) return false;
    for (const EntryLink& l : lib.linksOf(entryId)) lib.removeLink(l.from, l.to, l.label);
    return true;
}

bool importPages(Library& lib, const Id& world, const Id& book, const std::vector<Batch>& pages, const std::vector<Id>& booksInOrder,
                 Import& out, bool replaceAll, const std::string& baseFolder) {
    const Id scopeBook = world.empty() ? book : Id{};
    std::vector<Page> parsed;
    for (const Batch& b : pages) parsed.push_back(parse(b.text));
    // An entry by a name: of the page's kind, else any kind.
    auto byName = [&](const std::string& name, const std::string& kind) -> const Entry* {
        if (trim(name).empty()) return nullptr;
        const Entry* e = lib.findEntryNamed(world, scopeBook, name, kind);
        return e ? e : lib.findEntryNamed(world, scopeBook, name);
    };
    // The page's entry: by its name, then a former name, then one of its other names (only if one entry has it).
    auto findFor = [&](size_t i, std::string* foundAs) -> const Entry* {
        const Batch& b = pages[i];
        if (const Entry* e = byName(b.name, b.kind)) return e;
        for (auto& f : parsed[i].formerly)
            if (const Entry* e = byName(f, b.kind)) { if (foundAs) *foundAs = e->name; return e; }
        const Entry* one = nullptr;
        bool twice = false;
        for (auto& a : parsed[i].aliases)
            if (const Entry* e = byName(a, b.kind)) {
                if (one && one != e) twice = true;
                one = e;
            }
        if (one && !twice) { if (foundAs) *foundAs = one->name; return one; }
        return nullptr;
    };
    std::set<Id> seen;
    auto noteUpdated = [&](const Entry& e) {
        if (seen.insert(e.id).second && std::find(out.made.begin(), out.made.end(), e.id) == out.made.end()) {
            out.updated.push_back(e.id);
            out.before.push_back(e);
            out.linksBefore.push_back(lib.linksOf(e.id));
        }
    };
    auto mergeAway = [&](const Id& keep, const Id& other) {
        const Entry* o = lib.entry(other);
        if (!o || keep == other) return false;
        out.mergedAway.push_back(*o);
        out.mergedAwayLinks.push_back(lib.linksOf(other));
        return lib.mergeEntries(keep, other);
    };

    // Pages that remove or merge their entry instead of filling it in.
    std::vector<bool> skip(pages.size(), false);
    for (size_t i = 0; i < pages.size(); ++i) {
        const Page& pg = parsed[i];
        if (!pg.remove && pg.mergeInto.empty()) continue;
        skip[i] = true;
        const Entry* e = findFor(i, nullptr);
        if (!e) { out.notFound.push_back(pages[i].name); continue; }
        if (pg.remove) {
            const Id id = e->id;
            if (lib.trashEntry(id)) out.removed.push_back(id);
            continue;
        }
        const Entry* into = byName(pg.mergeInto, pages[i].kind);
        if (into && into->id != e->id) {
            noteUpdated(*into);
            const Id keep = into->id, other = e->id;
            if (!mergeAway(keep, other)) return false;
        } else if (!into) {
            // Nothing called that: the entry takes that name.
            noteUpdated(*e);
            out.renamed.push_back({e->id, e->name});
            Entry x = *e;
            x.name = pg.mergeInto;
            if (!lib.updateEntry(x)) return false;
        }
    }

    // Every other page's entry first: found, or made, so pages naming each other link.
    std::vector<Id> target(pages.size());
    for (size_t i = 0; i < pages.size(); ++i) {
        if (skip[i]) continue;
        const Batch& b = pages[i];
        std::string foundAs;
        if (const Entry* e = findFor(i, &foundAs)) {
            target[i] = e->id;
            noteUpdated(*e);
            if (!foundAs.empty() && foundAs != b.name) out.renamed.push_back({e->id, foundAs});
            continue;
        }
        const Id id = lib.createEntry(world, scopeBook, b.kind, b.name);
        if (id.empty()) return false;
        target[i] = id;
        seen.insert(id);
        out.made.push_back(id);
    }
    // "Merged from": those entries are merged into the page's before it's applied.
    for (size_t i = 0; i < pages.size(); ++i) {
        if (skip[i]) continue;
        for (auto& name : parsed[i].mergeFrom) {
            const Entry* o = byName(name, pages[i].kind);
            if (!o) { out.notFound.push_back(name); continue; }
            if (o->id == target[i]) continue;
            if (!mergeAway(target[i], o->id)) return false;
        }
    }
    if (replaceAll)
        for (const Id& id : out.updated)
            if (!clearEntryText(lib, id)) return false;
    std::set<Id> changed;
    for (size_t i = 0; i < pages.size(); ++i) {
        if (skip[i]) continue;
        Report r;
        Page pg = parsed[i];
        if (pg.name.empty()) pg.name = pages[i].name;   // a page found by a former name takes its own
        if (!apply(lib, target[i], pg, booksInOrder, r, baseFolder)) return false;
        out.links += r.links;
        out.pictures += (r.picture ? 1 : 0) + r.gallery;
        for (auto& m : r.missingPictures) out.missingPictures.push_back(m);
        for (const Id& x : r.created) out.extra.push_back(x);
        if (!r.words().empty()) changed.insert(target[i]);
    }
    for (const Id& id : out.updated) if (!changed.count(id)) ++out.unchanged;
    // Characters in the family with no gender line (0.57): they read as "sibling", "child" or "partner" for want of it.
    {
        std::set<Id> checked;
        auto check = [&](const Id& id) {
            if (!checked.insert(id).second) return;
            const Entry* e = lib.entry(id);
            if (!e || e->deleted || e->kind != "character") return;
            bool family = false;
            for (const EntryLink& l : lib.linksOf(id)) {
                if (!isFamilyRelation(l.label)) continue;
                const Entry* o = lib.entry(l.from == id ? l.to : l.from);
                if (o && !o->deleted && o->kind == "character") { family = true; break; }
            }
            if (!family) return;
            std::istringstream in(e->details);
            for (std::string l; std::getline(in, l);) {
                std::string lab, val;
                if (!splitField(l, lab, val) || trim(val).empty()) continue;
                const std::string k = key(lab);
                if (k == "gender" || k == "sex" || k == "gender or sex" || k == "gender/sex") return;
            }
            out.noGender.push_back(id);
        };
        for (auto* v : {&out.made, &out.updated, &out.extra})
            for (const Id& id : *v) check(id);
    }
    return true;
}

std::string Import::words() const {
    std::vector<std::string> parts;
    auto count = [](size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
    if (!made.empty()) parts.push_back(count(made.size(), "new entry", "new entries"));
    if (updated.size() > (size_t)unchanged) parts.push_back(count(updated.size() - unchanged, "entry updated", "entries updated"));
    if (links) parts.push_back(count((size_t)links, "link", "links"));
    if (!extra.empty()) parts.push_back(count(extra.size(), "entry made for a name the pages mention", "entries made for names the pages mention"));
    if (!renamed.empty()) parts.push_back(count(renamed.size(), "entry renamed", "entries renamed"));
    if (!mergedAway.empty()) parts.push_back(count(mergedAway.size(), "entry merged into another", "entries merged into others"));
    if (!removed.empty()) parts.push_back(count(removed.size(), "entry removed", "entries removed"));
    if (pictures) parts.push_back(count((size_t)pictures, "picture", "pictures"));
    std::string o;
    for (size_t i = 0; i < parts.size(); ++i) o += (i ? (i + 1 == parts.size() ? " and " : ", ") : "") + parts[i];
    return o;
}

}  // namespace studio::paste

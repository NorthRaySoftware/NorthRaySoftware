// Continuity checks for the world bible: facts that disagree (two lines of an entry's details, or the
// details against what the manuscripts say about a character's eyes, hair or age), and entries whose
// names never appear in the text.
#include "studio/document.hpp"
#include "studio/review.hpp"
#include "studio/calendar.hpp"
#include "studio/words.hpp"

#include <nlohmann/json.hpp>
#include <cmath>
#include <optional>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <regex>
#include <map>

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

std::string GapIssue::key() const { return "gap|" + nodeId + "|" + lower(said); }

std::optional<GapWords> gapWords(const std::string& openingIn, double week, double month, double year) {
    const std::string t = lower(openingIn);
    static const std::map<std::string, double> numbers{{"a", 1}, {"an", 1}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5},
                                                       {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10}, {"eleven", 11},
                                                       {"twelve", 12}, {"fifteen", 15}, {"twenty", 20}, {"thirty", 30}, {"forty", 40},
                                                       {"fifty", 50}, {"a hundred", 100}, {"a couple of", 2}, {"a couple", 2}};
    auto unitDays = [&](std::string u) -> double {
        if (u.size() > 1 && u.back() == 's') u.pop_back();
        if (u == "day" || u == "night") return 1;
        if (u == "week") return week;
        if (u == "fortnight") return 2 * week;
        if (u == "month" || u == "moon") return month;
        if (u == "season") return year / 4;
        if (u == "year" || u == "winter" || u == "summer") return year;
        return 0;
    };
    // "the next morning", "the following day", "the day after", "next morning": one day on.
    static const std::regex next(R"(\b(the next|the following|next) (day|morning|evening|night|dawn)\b|\bthe day after\b)");
    static const std::regex span(
        R"(\b(?:after |for )?(well over|just over|a little over|more than|over|almost|nearly|not quite|just under|less than|barely|about|around|roughly|some )?\s*(a hundred|a couple of|a couple|a few|several|a|an|one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|fifteen|twenty|thirty|forty|fifty|\d+) (days?|nights?|weeks?|fortnights?|months?|moons?|seasons?|years?|winters?|summers?)( later| afterwards?| had passed| passed| went by| had gone by|,)?)");
    std::smatch m;
    if (std::regex_search(t, m, next)) return GapWords{m.str(0), 1, 1};
    for (auto it = std::sregex_iterator(t.begin(), t.end(), span); it != std::sregex_iterator(); ++it) {
        const std::smatch& x = *it;
        const bool after = x.str(0).rfind("after ", 0) == 0;
        if (!after && !x[4].matched) continue;          // "three days" alone isn't a gap; "three days later" or "after three days" is
        if (x[4].matched && x.str(4) == "," && !after) continue;
        const double u = unitDays(x.str(3));
        if (u <= 0) continue;
        std::string q = x[1].matched ? x.str(1) : std::string();
        while (!q.empty() && q.back() == ' ') q.pop_back();
        const std::string n = x.str(2);
        double lo, hi;
        if (n == "a few" || n == "several") {
            const double a = n == "a few" ? 2 : 3, b = n == "a few" ? 5 : 9;
            lo = a * u * 0.8;
            hi = b * u * 1.25;
        } else {
            double v = 1;
            if (auto f = numbers.find(n); f != numbers.end()) v = f->second;
            else v = std::atof(n.c_str());
            const double e = v * u;
            if (q == "well over" || q == "more than" || q == "over") { lo = e * 0.98; hi = -1; }
            else if (q == "just over" || q == "a little over") { lo = e * 0.98; hi = e * 1.5 + 1; }
            else if (q == "almost" || q == "nearly" || q == "not quite" || q == "just under") { lo = e * 0.6; hi = e * 1.02; }
            else if (q == "less than" || q == "barely") { lo = 0; hi = e * 1.02; }
            else if (q == "about" || q == "around" || q == "roughly" || q == "some") { lo = e * 0.75 - 1; hi = e * 1.35 + 1; }
            else if (u == 1) { lo = v; hi = v + 1; }            // "three days later": three days on (four, by the clock)
            else { lo = e * 0.8; hi = e * 1.25; }
        }
        std::string said = x.str(0);
        while (!said.empty() && (said.back() == ',' || said.back() == ' ')) said.pop_back();
        return GapWords{said, std::max(0.0, lo), hi};
    }
    return std::nullopt;
}

namespace {
// Days as words: "9 months", "1 year and 2 months", "3 weeks", "5 days".
std::string daysWords(double days, double month, double year) {
    auto plural = [](long long n, const char* one) { return std::to_string(n) + " " + one + (n == 1 ? "" : "s"); };
    const long long d = (long long)std::llround(days);
    if (days >= year - 0.5) {
        const long long y = (long long)std::floor((days + 0.5) / year);
        const long long mo = (long long)std::llround((days - y * year) / month);
        return plural(y, "year") + (mo > 0 ? " and " + plural(mo, "month") : "");
    }
    if (days >= month * 1.5) return plural((long long)std::llround(days / month), "month");
    if (days >= 14) return plural((long long)std::llround(days / 7), "week");
    return plural(d, "day");
}
// Very common words that are also names (Will, Hope, Grace): written in lower case they're nearly always the word.
const std::set<std::string>& commonWords() {
    static const std::set<std::string> s{"will", "may", "hope", "grace", "faith", "joy", "rose", "lily", "ivy", "iris", "mark", "bill", "jack",
                                         "pat", "sue", "rob", "art", "dawn", "summer", "autumn", "winter", "spring", "hunter", "mason",
                                         "page", "king", "queen", "lord", "lady", "sir", "chase", "drew", "frank", "gene", "guy", "mercy",
                                         "sky", "storm", "river", "stone", "wolf", "fox", "bear", "hawk", "raven", "crow", "sage", "hunter",
                                         "the", "a", "an", "of", "and", "in", "on", "at", "to", "for", "god", "gods", "sun", "moon"};
    return s;
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

    // Ages worked out from the calendar, per book (0.86): the text's "N years old" is checked against them first.
    std::map<Id, std::map<Id, std::pair<int, int>>> worked;   // entry -> book -> (age, age by the end); about: widened
    {
        const cal::Calendar c = worldId.empty() ? cal::calendarOfBook(lib, bookId) : cal::calendarOfWorld(lib, worldId);
        if (!c.empty()) {
            std::vector<Id> order;
            for (const Book* b : books) order.push_back(b->id);
            for (const Entry* e : entries) {
                if (e->kind != "character" && e->kind != "creature") continue;
                const cal::Birth bi = cal::birthOf(c, *e);
                if (!bi.known || bi.ageless || !bi.appears.empty()) continue;
                for (const cal::BookAge& a : cal::agesThroughBooks(lib, c, bi, order))
                    worked[e->id][a.book] = {a.age - (a.about ? 1 : 0), (a.turns ? a.turnsTo : a.age) + (a.about ? 1 : 0)};
                if (worked.count(e->id)) known[e->id];   // so the text is read for them
            }
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
                if (said.age > 0) {
                    auto wk = worked.find(who->id);
                    const auto inBook = wk == worked.end() ? decltype(wk->second)::const_iterator{} : wk->second.find(b->id);
                    if (wk != worked.end() && inBook != wk->second.end()) {
                        const auto [lo, hi] = inBook->second;
                        if (said.age < lo || said.age > hi)
                            report("Age", (lo == hi ? std::to_string(lo) : std::to_string(lo) + "\xE2\x80\x93" + std::to_string(hi)) +
                                              " in this book, worked out from the calendar",
                                   std::to_string(said.age) + " years old");
                    } else if (k->second.age > 0 && said.age != k->second.age)
                        report(k->second.ageLine, std::to_string(k->second.age), std::to_string(said.age) + " years old");
                }
            }
        }

    // ---- Entries the manuscripts never name.
    for (const Entry* e : entries) if (!seen.count(e->id)) out.unused.push_back(e->id);

    // ---- Time between chapters (0.86): what a chapter's opening says against the timeline's dates.
    for (const Book* b : books) {
        const cal::Calendar c = cal::calendarOfBook(lib, b->id);
        const nlohmann::json tl = nlohmann::json::parse(lib.setting("timeline:" + b->id), nullptr, false);
        if (!tl.is_object() || !tl.contains("nodes") || !tl["nodes"].is_object()) continue;
        const bool inDates = tl.value("calendar", false) && !c.empty();
        const std::string unit = tl.value("unit", std::string("day"));
        const double yearLen = inDates ? cal::meanYear(c) : 365.25;
        const double monthLen = inDates && !c.months.empty() ? yearLen / (double)c.months.size() : 30.44;
        const double weekLen = inDates && !c.weekdays.empty() ? (double)c.weekdays.size() : 7;
        const double scale = inDates || unit == "day" ? 1 : unit == "week" ? 7 : unit == "month" ? monthLen : unit == "year" ? yearLen : 1;
        const std::int64_t start = inDates ? (std::int64_t)std::floor(tl.value("start", 0.0)) : 0;
        auto dayOf = [&](const Id& id) -> std::optional<double> {
            const auto& n = tl["nodes"];
            if (!n.contains(id) || !n[id].is_object() || !n[id].contains("day") || !n[id]["day"].is_number()) return std::nullopt;
            return std::floor(n[id]["day"].get<double>()) * scale;
        };
        const auto all = lib.manuscriptOf(b->id);
        std::optional<double> prev;
        Id prevId;
        for (const ManuscriptNode* n : all) {
            if (n->kind != NodeKind::Chapter) continue;
            std::optional<double> day = dayOf(n->id);
            if (!day)
                for (const ManuscriptNode* m : all)
                    if (m->parentId == n->id && m->kind == NodeKind::Scene)
                        if ((day = dayOf(m->id))) break;
            if (!day) continue;
            if (prev) {
                // Its opening: the first two sentences, at most 300 characters.
                const std::string text = doc::plainText(lib.nodeContent(n->id));
                const auto ss = sentences(text);
                std::string opening;
                for (size_t i = 0; i < ss.size() && i < 2; ++i) opening += (i ? " " : "") + ss[i];
                if (opening.size() > 300) opening = opening.substr(0, 300);
                if (auto g = gapWords(opening, weekLen, monthLen, yearLen)) {
                    const double diff = *day - *prev;
                    if (diff < g->lo - 0.01 || (g->hi >= 0 && diff > g->hi + 0.01)) {
                        GapIssue gi;
                        gi.bookId = b->id;
                        gi.nodeId = n->id;
                        gi.prevId = prevId;
                        gi.said = g->said;
                        gi.actual = diff < 0 ? "before the chapter ahead of it" : diff == 0 ? "the same day" : daysWords(diff, monthLen, yearLen);
                        if (inDates)
                            gi.actual += " (from " + cal::formatDate(c, cal::fromDay(c, start + (std::int64_t)*prev)) + " to " +
                                         cal::formatDate(c, cal::fromDay(c, start + (std::int64_t)*day)) + ")";
                        for (auto& sx : ss) if (lower(sx).find(g->said) != std::string::npos) { gi.snippet = sx.size() > 240 ? sx.substr(0, 237) + "..." : sx; break; }
                        out.gaps.push_back(gi);
                    }
                }
            }
            prev = day;
            prevId = n->id;
        }
    }

    // ---- Ages worked out from the calendar (0.86): a typed Age that none of them agree with.
    {
        const cal::Calendar c = worldId.empty() ? cal::calendarOfBook(lib, bookId) : cal::calendarOfWorld(lib, worldId);
        if (!c.empty()) {
            std::vector<Id> order;
            for (const Book* b : books) order.push_back(b->id);
            for (const Entry* e : entries) {
                if (e->kind != "character" && e->kind != "creature") continue;
                const cal::Birth bi = cal::birthOf(c, *e);
                if (!bi.known) continue;
                const auto ages = cal::agesThroughBooks(lib, c, bi, order);
                const std::string other = cal::ageDisagreement(c, bi, ages);
                if (other.empty()) continue;
                FactIssue f;
                f.entry = e->id;
                f.label = "Age";
                f.bible = bi.typedAge;
                f.other = other;
                f.inBible = true;
                f.worked = true;
                f.note = "The page's Age is \xE2\x80\x9C" + bi.typedAge + "\xE2\x80\x9D, but worked out from " + bi.from + " in the calendar they're " + other + ".";
                out.facts.push_back(f);
            }
        }
    }

    // ---- Bible names written with the wrong capitals (0.86): "mimic" for the Mimic.
    {
        struct Name { const Entry* e; std::string name, key; };
        std::vector<Name> names;
        for (const Entry* e : entries) {
            std::vector<std::string> all{e->name};
            for (auto& a : e->aliases) all.push_back(a);
            for (std::string n : all) {
                n = trim(n);
                if (n.rfind("the ", 0) == 0 || n.rfind("The ", 0) == 0) n = n.substr(4);
                if (n.size() < 3) continue;
                if (!std::any_of(n.begin(), n.end(), [](unsigned char ch) { return std::isupper(ch); })) continue;
                const std::string k = lower(n);
                if (k == n || commonWords().count(k)) continue;
                names.push_back({e, n, k});
            }
        }
        std::sort(names.begin(), names.end(), [](const Name& a, const Name& b) { return a.key.size() > b.key.size(); });
        std::map<std::string, size_t> at;   // name -> index in out.capitals
        if (!names.empty())
            for (const Book* b : books)
                for (const ManuscriptNode* n : lib.manuscriptOf(b->id)) {
                    if (n->kind == NodeKind::Part) continue;
                    const std::string text = doc::plainText(lib.nodeContent(n->id));
                    const std::string low = lower(text);
                    std::vector<std::pair<size_t, size_t>> taken;   // longer names first; a shorter one inside isn't counted again
                    for (const Name& nm : names) {
                        for (size_t p = low.find(nm.key); p != std::string::npos; p = low.find(nm.key, p + 1)) {
                            if (!words::wholeWord(text, p, nm.key.size())) continue;
                            if (std::any_of(taken.begin(), taken.end(), [&](auto& r) { return p < r.second && p + nm.key.size() > r.first; })) continue;
                            taken.push_back({p, p + nm.key.size()});
                            const std::string written = text.substr(p, nm.key.size());
                            if (written == nm.name) continue;
                            // Wrong when a word the bible capitalises starts in lower case.
                            bool wrong = false;
                            for (size_t i = 0; i < written.size(); ++i) {
                                const bool wordStart = i == 0 || written[i - 1] == ' ' || written[i - 1] == '-';
                                if (wordStart && std::isupper((unsigned char)nm.name[i]) && std::islower((unsigned char)written[i])) wrong = true;
                            }
                            if (!wrong) continue;
                            const std::string id = nm.e->id + "|" + nm.name;
                            auto it = at.find(id);
                            if (it == at.end()) {
                                CapsIssue ci;
                                ci.entry = nm.e->id;
                                ci.name = nm.name;
                                ci.written = written;
                                ci.bookId = b->id;
                                ci.nodeId = n->id;
                                for (auto& sx : sentences(text))
                                    if (sx.find(written) != std::string::npos) { ci.snippet = sx.size() > 240 ? sx.substr(0, 237) + "..." : sx; break; }
                                it = at.emplace(id, out.capitals.size()).first;
                                out.capitals.push_back(ci);
                            }
                            ++out.capitals[it->second].count;
                        }
                    }
                }
        std::stable_sort(out.capitals.begin(), out.capitals.end(), [](const CapsIssue& a, const CapsIssue& b) { return a.count > b.count; });
    }
    return out;
}

}  // namespace studio::review

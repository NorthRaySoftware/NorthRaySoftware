// Writing Studio: ages worked out from the calendar (0.86). Portable and tested.
#include "studio/calendar.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace studio::cal {

namespace {
using nlohmann::json;

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
std::string lowered(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}
bool yes(const std::string& v) {
    const std::string l = lowered(trimmed(v));
    return l == "yes" || l == "y" || l == "true" || l == "1" || l == "ageless";
}
// A year alone ("2009 TA", "2009", "300 BTA"): read as the first day of that year.
std::optional<std::int64_t> yearOf(const Calendar& c, const std::string& v) {
    if (c.months.empty()) return std::nullopt;
    const std::string t = trimmed(v);
    if (t.empty() || !std::isdigit((unsigned char)t[0])) return std::nullopt;
    if (auto d = parseDate(c, "1 " + monthLabel(c, 0) + " " + t)) return d->year;
    return std::nullopt;
}
}  // namespace

Birth birthOf(const Calendar& c, const Entry& e) {
    Birth b;
    std::string bornText, birthdayText;
    std::istringstream in(e.details);
    for (std::string line; std::getline(in, line);) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string label = lowered(trimmed(line.substr(0, colon))), value = trimmed(line.substr(colon + 1));
        if (value.empty()) continue;
        if (label == "born" || label == "birth" || label == "date of birth" || label == "birth date" || label == "hatched") {
            if (bornText.empty()) bornText = value;
        } else if (label == "birthday") {
            if (birthdayText.empty()) birthdayText = value;
        } else if (label == "died" || label == "death" || label == "date of death" || label == "death date" || label == "killed") {
            if (!b.died)
                if (auto dt = parseDateTime(c, value)) b.died = dt->date;
        } else if (label == "ageless") {
            b.ageless = yes(value);
        } else if (label == "appears" || label == "apparent age" || label == "looks" || label == "appears to be") {
            b.appears = value;
        } else if (label == "age") {
            b.typedAge = value;
            if (lowered(value) == "ageless" || lowered(value) == "immortal") b.ageless = true;
        }
    }
    if (c.empty() || bornText.empty()) return b;
    if (auto dt = parseDateTime(c, bornText)) {
        b.known = true;
        b.date = dt->date;
        b.from = "Born: " + bornText;
        return b;
    }
    if (auto y = yearOf(c, bornText)) {
        // A year, with the day and month from a Birthday line when there is one.
        if (!birthdayText.empty())
            if (auto d = parseDate(c, birthdayText + " " + bornText)) {
                b.known = true;
                b.date = *d;
                b.from = "Born: " + bornText + ", Birthday: " + birthdayText;
                return b;
            }
        b.known = true;
        b.about = true;
        b.date = {*y, 0, 1};
        b.from = "Born: " + bornText;
    }
    return b;
}

std::optional<std::int64_t> agesAsOf(const Library& lib, const Calendar& c, const Id& book, bool* chosen) {
    if (chosen) *chosen = false;
    const StorySpan span = storySpan(lib, book);
    if (!span.on) return std::nullopt;
    if (const std::string own = lib.setting("ages-as-of:" + book); !own.empty())
        if (auto d = parseDate(c, own)) {
            if (chosen) *chosen = true;
            return toDay(c, *d);
        }
    const json tl = json::parse(lib.setting("timeline:" + book), nullptr, false);
    if (!tl.is_object() || !tl.contains("nodes") || !tl["nodes"].is_object()) return std::nullopt;
    for (const ManuscriptNode* n : lib.manuscriptOf(book)) {
        if (n->kind == NodeKind::Part || !tl["nodes"].contains(n->id)) continue;
        const json& v = tl["nodes"][n->id];
        if (v.is_object() && v.contains("day") && v["day"].is_number()) return span.start + (std::int64_t)std::floor(v["day"].get<double>());
    }
    return std::nullopt;
}

std::string agesAsOfText(const Library& lib, const Id& book) { return lib.setting("ages-as-of:" + book); }
bool setAgesAsOf(Library& lib, const Id& book, const std::string& text) { return lib.setSetting("ages-as-of:" + book, trimmed(text)); }

std::vector<BookAge> agesThroughBooks(const Library& lib, const Calendar& c, const Birth& b, const std::vector<Id>& books) {
    std::vector<BookAge> out;
    if (!b.known || c.empty()) return out;
    const std::int64_t bornDay = toDay(c, b.date);
    for (const Id& id : books) {
        const Book* bk = lib.book(id);
        if (!bk) continue;
        bool chosen = false;
        const auto asOf = agesAsOf(lib, c, id, &chosen);
        if (!asOf) continue;
        // The book's dated chapters and scenes: the last day, and each with its day for the birthday's chapter.
        const StorySpan span = storySpan(lib, id);
        const json tl = json::parse(lib.setting("timeline:" + id), nullptr, false);
        std::vector<std::pair<std::int64_t, const ManuscriptNode*>> dated;
        if (tl.is_object() && tl.contains("nodes") && tl["nodes"].is_object())
            for (const ManuscriptNode* n : lib.manuscriptOf(id)) {
                if (n->kind == NodeKind::Part || !tl["nodes"].contains(n->id)) continue;
                const json& v = tl["nodes"][n->id];
                if (v.is_object() && v.contains("day") && v["day"].is_number())
                    dated.push_back({span.start + (std::int64_t)std::floor(v["day"].get<double>()), n});
            }
        std::int64_t last = *asOf;
        for (auto& [d, n] : dated) last = std::max(last, d);
        if (*asOf < bornDay) continue;                                      // not born yet
        if (b.died && toDay(c, *b.died) < *asOf) continue;                  // dead before it
        BookAge a;
        a.book = id;
        a.title = bk->title;
        a.asOf = *asOf;
        a.last = last;
        a.chosenAsOf = chosen;
        a.about = b.about;
        const Date at = fromDay(c, *asOf);
        a.age = b.about ? (int)(at.year - b.date.year) : ageAt(c, b.date, at);
        if (!b.about) {
            // The next birthday after the "as of" day: inside the book's dates?
            Date next{at.year, b.date.month, b.date.day};
            auto clampDay = [&](Date d) {
                if (d.month >= (int)c.months.size()) d.month = (int)c.months.size() - 1;
                d.day = std::min(d.day, std::max(1, monthLength(c, d.year, d.month)));
                return d;
            };
            next = clampDay(next);
            if (toDay(c, next) <= *asOf) next = clampDay({at.year + 1, b.date.month, b.date.day});
            const std::int64_t nd = toDay(c, next);
            if (nd <= last && (!b.died || nd <= toDay(c, *b.died))) {
                a.turns = true;
                a.turnsTo = (int)(next.year - b.date.year);
                a.birthday = next;
                // The first chapter by date on or after it (a scene names its chapter).
                const ManuscriptNode* best = nullptr;
                std::int64_t bestDay = 0;
                for (auto& [d, n] : dated)
                    if (d >= nd && (!best || d < bestDay)) { best = n; bestDay = d; }
                if (best) {
                    const ManuscriptNode* ch = best;
                    if (ch->kind == NodeKind::Scene && !ch->parentId.empty())
                        if (const ManuscriptNode* p = lib.node(ch->parentId)) ch = p;
                    a.chapter = ch->id;
                    a.chapterTitle = ch->title;
                }
            }
        }
        out.push_back(a);
    }
    return out;
}

std::string ageText(const Calendar& c, const BookAge& a) {
    std::string t = (a.about ? "about " : "") + std::to_string(a.age);
    if (a.turns) {
        t += ", turns " + std::to_string(a.turnsTo) + " during this book (" + std::to_string(a.birthday.day) + " " + monthLabel(c, a.birthday.month);
        if (!a.chapterTitle.empty()) t += ", in \xE2\x80\x9C" + a.chapterTitle + "\xE2\x80\x9D";
        t += ")";
    }
    return t;
}

std::string ageDisagreement(const Calendar& c, const Birth& b, const std::vector<BookAge>& ages) {
    (void)c;
    if (b.ageless || !b.appears.empty() || b.typedAge.empty() || ages.empty()) return {};
    // The first number in the typed Age ("24", "about 24", "24 years").
    const std::string& t = b.typedAge;
    size_t i = 0;
    while (i < t.size() && !std::isdigit((unsigned char)t[i])) ++i;
    if (i == t.size()) return {};
    size_t j = i;
    while (j < t.size() && std::isdigit((unsigned char)t[j])) ++j;
    if (j - i > 9) return {};
    const long long typed = std::stoll(t.substr(i, j - i));
    const int slack = b.about ? 1 : 0;
    std::string said;
    for (const BookAge& a : ages) {
        const int lo = a.age - slack, hi = (a.turns ? a.turnsTo : a.age) + slack;
        if (typed >= lo && typed <= hi) return {};
        said += (said.empty() ? "" : "; ") + std::to_string(a.age) + (a.turns ? "\xE2\x80\x93" + std::to_string(a.turnsTo) : std::string()) +
                " in " + a.title;
    }
    return (b.about ? "about " : "") + said;
}

}  // namespace studio::cal

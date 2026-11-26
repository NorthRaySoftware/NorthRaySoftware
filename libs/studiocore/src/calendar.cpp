// A world's own calendar (0.74). See studio/calendar.hpp.

#include "studio/calendar.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>

namespace studio::cal {

using json = nlohmann::json;

namespace {

std::int64_t fdiv(std::int64_t a, std::int64_t b) {   // floor division, b > 0
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
// How many k in [a, b] have (k - off) a multiple of e.
std::int64_t countIn(std::int64_t a, std::int64_t b, int e, int off) {
    if (b < a || e <= 0) return 0;
    return fdiv(b - off, e) - fdiv(a - 1 - off, e);
}
bool applies(const LeapRule& r, std::int64_t year) {
    if (r.every <= 0) return false;
    std::int64_t m = (year - r.offset) % r.every;
    return m == 0;
}
std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
int baseYear(const Calendar& c) {
    int n = 0;
    for (auto& m : c.months) n += std::max(0, m.days);
    return n;
}
bool isNumber(const std::string& s) {
    if (s.empty()) return false;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i == s.size()) return false;
    for (; i < s.size(); ++i) if (!std::isdigit((unsigned char)s[i])) return false;
    return true;
}
const char* zeroId(MonthZero z) { return z == MonthZero::Count ? "count" : z == MonthZero::Extra ? "extra" : "none"; }
MonthZero zeroFrom(const std::string& s) {
    const std::string l = lower(s);
    if (l == "count" || l.find("from 0") != std::string::npos || l.find("counted") != std::string::npos) return MonthZero::Count;
    if (l == "extra" || l.find("extra") != std::string::npos || l.find("before") != std::string::npos) return MonthZero::Extra;
    return MonthZero::None;
}

}  // namespace

Calendar gregorian() {
    Calendar c;
    c.name = "Gregorian";
    const char* names[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September",
                           "October", "November", "December"};
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int i = 0; i < 12; ++i) c.months.push_back({names[i], days[i]});
    c.weekdays = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    c.firstWeekday = 0;   // 1 January of year 1 (proleptic Gregorian) was a Monday
    c.leap = {{4, 0, 1, 1}, {100, 0, 1, -1}, {400, 0, 1, 1}};
    c.eras = {{"Anno Domini", "AD", 1}};
    c.beforeName = "Before Christ";
    c.beforeAbbr = "BC";
    // A new moon fell on 6 January 2000.
    const double cycle = 29.530589;
    const double at = (double)toDay(c, {2000, 0, 6});
    c.moons = {{"Moon", cycle, cycle - std::fmod(at, cycle)}};
    c.seasons = {{"Winter", 0, 1}, {"Spring", 2, 20}, {"Summer", 5, 21}, {"Autumn", 8, 22}, {"Winter", 11, 21}};
    c.order = "dmy";
    c.dayParts = defaultDayParts(c);
    return c;
}

Calendar blank() {
    Calendar c;
    c.name = "New calendar";
    c.months = {{"First month", 30}};
    c.weekdays = {"First day", "Second day", "Third day", "Fourth day", "Fifth day", "Sixth day", "Seventh day"};
    c.dayParts = defaultDayParts(c);
    return c;
}

int monthNumber(const Calendar& c, int index) { return c.monthZero == MonthZero::None ? index + 1 : index; }

std::string monthLabel(const Calendar& c, int index) {
    if (index >= 0 && index < (int)c.months.size() && !trim(c.months[index].name).empty()) return c.months[index].name;
    return "Month " + std::to_string(monthNumber(c, index));
}

int monthLength(const Calendar& c, std::int64_t year, int month) {
    if (month < 0 || month >= (int)c.months.size()) return 0;
    int n = std::max(0, c.months[month].days);
    for (auto& r : c.leap) if (r.month == month && applies(r, year)) n += r.add;
    return std::max(0, n);
}

std::int64_t yearLength(const Calendar& c, std::int64_t year) {
    std::int64_t n = 0;
    for (int m = 0; m < (int)c.months.size(); ++m) n += monthLength(c, year, m);
    return n;
}

double meanYear(const Calendar& c) {
    double n = baseYear(c);
    for (auto& r : c.leap) if (r.every > 0 && r.month >= 0 && r.month < (int)c.months.size()) n += (double)r.add / r.every;
    return n;
}

std::int64_t daysBeforeYear(const Calendar& c, std::int64_t year) {
    const std::int64_t base = baseYear(c);
    std::int64_t n = base * (year - 1);
    for (auto& r : c.leap) {
        if (r.month < 0 || r.month >= (int)c.months.size()) continue;
        if (year > 1) n += (std::int64_t)r.add * countIn(1, year - 1, r.every, r.offset);
        else if (year < 1) n -= (std::int64_t)r.add * countIn(year, 0, r.every, r.offset);
    }
    return n;
}

Date clampDate(const Calendar& c, Date d) {
    if (c.months.empty()) return {d.year, 0, 1};
    d.month = std::clamp(d.month, 0, (int)c.months.size() - 1);
    d.day = std::clamp(d.day, 1, std::max(1, monthLength(c, d.year, d.month)));
    return d;
}

std::int64_t toDay(const Calendar& c, const Date& d0) {
    const Date d = clampDate(c, d0);
    std::int64_t n = daysBeforeYear(c, d.year);
    for (int m = 0; m < d.month; ++m) n += monthLength(c, d.year, m);
    return n + d.day - 1;
}

Date fromDay(const Calendar& c, std::int64_t day) {
    Date d;
    if (c.months.empty() || baseYear(c) <= 0) return d;
    const double mean = std::max(1.0, meanYear(c));
    std::int64_t y = 1 + (std::int64_t)std::floor((double)day / mean);
    for (int guard = 0; guard < 1000 && daysBeforeYear(c, y) > day; ++guard) --y;
    for (int guard = 0; guard < 1000 && daysBeforeYear(c, y + 1) <= day; ++guard) ++y;
    std::int64_t left = day - daysBeforeYear(c, y);
    d.year = y;
    for (int m = 0; m < (int)c.months.size(); ++m) {
        const int len = monthLength(c, y, m);
        if (left < len) { d.month = m; d.day = (int)left + 1; return d; }
        left -= len;
    }
    d.month = (int)c.months.size() - 1;
    d.day = std::max(1, monthLength(c, y, d.month));
    return d;
}

int weekday(const Calendar& c, std::int64_t day) {
    const int n = (int)c.weekdays.size();
    if (n == 0) return 0;
    std::int64_t w = (day + c.firstWeekday) % n;
    if (w < 0) w += n;
    return (int)w;
}

namespace {
// The start of the era after `e` (the next later start); false when it's the last.
bool nextEraStart(const Calendar& c, const Era& e, std::int64_t& next) {
    bool any = false;
    for (auto& x : c.eras)
        if (x.start > e.start && (!any || x.start < next)) { next = x.start; any = true; }
    return any;
}
}  // namespace

EraYear eraYear(const Calendar& c, std::int64_t year) {
    if (c.eras.empty()) return {year, "", "", false};
    const Era* e = nullptr;
    for (auto& x : c.eras) if (x.start <= year && (!e || x.start >= e->start)) e = &x;
    if (e) {
        std::int64_t next = 0;
        // Counting down (0.80): its last year, the one before the next era, is 1.
        if (e->countDown && nextEraStart(c, *e, next)) return {next - year, e->abbr, e->name, true};
        return {year - e->start + 1, e->abbr, e->name, false};
    }
    std::int64_t first = c.eras.front().start;
    for (auto& x : c.eras) first = std::min(first, x.start);
    return {first - year, c.beforeAbbr, c.beforeName, true};
}

std::int64_t plainYear(const Calendar& c, std::int64_t year, const std::string& era) {
    const std::string l = lower(trim(era));
    if (l.empty()) return year;
    for (auto& x : c.eras)
        if ((!x.abbr.empty() && lower(x.abbr) == l) || lower(x.name) == l) {
            std::int64_t next = 0;
            if (x.countDown && nextEraStart(c, x, next)) return next - year;
            return x.start + year - 1;
        }
    if (!c.eras.empty() && (lower(c.beforeAbbr) == l || lower(c.beforeName) == l)) {
        std::int64_t first = c.eras.front().start;
        for (auto& x : c.eras) first = std::min(first, x.start);
        return first - year;
    }
    return year;
}

namespace {
// The era's name or abbreviation after a year, as the calendar writes it (0.80: abbreviation, name or none).
std::string eraTag(const Calendar& c, const EraYear& e) {
    if (c.eraStyle == "none") return {};
    if (c.eraStyle == "name") return !e.name.empty() ? e.name : e.abbr;
    return !e.abbr.empty() ? e.abbr : e.name;
}
// Always with the era (abbreviation first), so it reads back: for calendar text.
std::string yearWithEra(const Calendar& c, std::int64_t year) {
    const EraYear e = eraYear(c, year);
    const std::string tag = !e.abbr.empty() ? e.abbr : e.name;
    return std::to_string(e.year) + (tag.empty() ? "" : " " + tag);
}
std::string padded(std::int64_t n, int width) {
    std::string d = std::to_string(n < 0 ? -n : n);
    while ((int)d.size() < width) d = "0" + d;
    return (n < 0 ? "-" : "") + d;
}
}  // namespace

std::string formatDateStyled(const Calendar& cIn, const Date& d, const std::string& form, const std::string& era) {
    Calendar c = cIn;
    if (form == "words") c.numeric = false;
    else if (form == "numbers") c.numeric = true;
    if (era == "abbr" || era == "name" || era == "none") c.eraStyle = era;
    return formatDate(c, d);
}

std::string formatYear(const Calendar& c, std::int64_t year) {
    const EraYear e = eraYear(c, year);
    std::string s = std::to_string(e.year);
    const std::string tag = eraTag(c, e);
    if (!tag.empty()) s += " " + tag;
    return s;
}

std::string formatDate(const Calendar& c, const Date& d0, bool withEra) {
    if (c.months.empty()) return {};
    const Date d = clampDate(c, d0);
    const EraYear e = eraYear(c, d.year);
    const std::string tag = withEra ? eraTag(c, e) : std::string();
    if (c.numeric) {
        // "01/00/1204 AE" (0.80): the day and month in two digits, the month as it's numbered, the year at least two.
        const std::string sep(1, c.separator ? c.separator : '/');
        const std::string dd = padded(d.day, 2), mm = padded(monthNumber(c, d.month), 2), yy = padded(e.year, 2);
        std::string s = c.order == "mdy" ? mm + sep + dd + sep + yy : c.order == "ymd" ? yy + sep + mm + sep + dd : dd + sep + mm + sep + yy;
        return tag.empty() ? s : s + " " + tag;
    }
    const std::string y = std::to_string(e.year) + (tag.empty() ? "" : " " + tag);
    const std::string m = monthLabel(c, d.month), dd = std::to_string(d.day);
    if (c.order == "mdy") return m + " " + dd + ", " + y;
    if (c.order == "ymd") return y + ", " + dd + " " + m;
    return dd + " " + m + " " + y;
}

std::string formatTime(const Calendar& c, int minutes) {
    const int mph = std::max(1, c.minutesPerHour);
    const int h = minutes / mph, m = minutes % mph;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d", h, m);
    return buf;
}

std::optional<int> parseTime(const Calendar& c, const std::string& s0) {
    const std::string s = lower(trim(s0));
    if (s.empty()) return std::nullopt;
    int h = 0, m = 0;
    char colon = 0;
    std::istringstream in(s);
    if (!(in >> h)) return std::nullopt;
    if (in >> colon) {
        if (colon != ':' && colon != '.') {
            in.putback(colon);
        } else if (!(in >> m)) return std::nullopt;
    }
    std::string rest;
    in >> rest;
    if (rest == "pm" && h < 12 && c.hoursPerDay == 24) h += 12;
    if (rest == "am" && h == 12 && c.hoursPerDay == 24) h = 0;
    if (h < 0 || h >= std::max(1, c.hoursPerDay) || m < 0 || m >= std::max(1, c.minutesPerHour)) return std::nullopt;
    return h * std::max(1, c.minutesPerHour) + m;
}

// ------------------------------------------------------------------ times of day (0.80)

std::vector<DayPart> defaultDayParts(const Calendar& c) {
    const int perDay = std::max(1, c.hoursPerDay) * std::max(1, c.minutesPerHour);
    auto at = [&](int hour24) { return (int)((long long)perDay * hour24 * 60 / (24 * 60)); };
    return {{"Dawn", at(5)}, {"Morning", at(7)}, {"Midday", at(12)}, {"Afternoon", at(13)}, {"Evening", at(18)}, {"Night", at(21)}};
}

const DayPart* dayPartAt(const Calendar& c, int minutes) {
    if (c.dayParts.empty() || minutes < 0) return nullptr;
    const DayPart* best = nullptr;
    const DayPart* latest = nullptr;
    for (auto& p : c.dayParts) {
        if (p.start <= minutes && (!best || p.start >= best->start)) best = &p;
        if (!latest || p.start >= latest->start) latest = &p;
    }
    return best ? best : latest;   // before the first: the last, running on round midnight
}

const DayPart* dayPartNamed(const Calendar& c, const std::string& name) {
    const std::string l = lower(trim(name));
    if (l.empty()) return nullptr;
    for (auto& p : c.dayParts) if (lower(trim(p.name)) == l) return &p;
    if (l.size() < 3) return nullptr;
    const DayPart* found = nullptr;
    int hits = 0;
    for (auto& p : c.dayParts) if (lower(trim(p.name)).rfind(l, 0) == 0) { found = &p; ++hits; }
    return hits == 1 ? found : nullptr;
}

std::optional<TimeOfDay> parseTimeOfDay(const Calendar& c, const std::string& s0) {
    std::string s = trim(s0);
    for (const char* w : {"at ", "in the ", "during the ", "by ", "the "}) {
        const std::string ws = w;
        if (lower(s).rfind(ws, 0) == 0) s = trim(s.substr(ws.size()));
    }
    if (s.empty()) return std::nullopt;
    TimeOfDay t;
    if (std::isdigit((unsigned char)s[0])) {
        // A clock time needs a colon or am/pm, so a lone number isn't taken for one.
        const std::string l = lower(s);
        if (l.find(':') == std::string::npos && l.find('.') == std::string::npos && l.find("am") == std::string::npos &&
            l.find("pm") == std::string::npos)
            return std::nullopt;
        const auto m = parseTime(c, s);
        if (!m) return std::nullopt;
        t.minutes = *m;
        return t;
    }
    if (const DayPart* p = dayPartNamed(c, s)) { t.part = p->name; return t; }
    return std::nullopt;
}

std::string timeOfDayText(const Calendar& c, const TimeOfDay& t, bool withPart) {
    if (!t.part.empty()) {
        std::string n = t.part;
        for (auto& ch : n) ch = (char)std::tolower((unsigned char)ch);
        return n;
    }
    if (t.minutes < 0) return {};
    std::string s = formatTime(c, t.minutes);
    if (withPart)
        if (const DayPart* p = dayPartAt(c, t.minutes)) s += " (" + lower(p->name) + ")";
    return s;
}

int timeSortKey(const Calendar& c, const TimeOfDay& t) {
    if (t.minutes >= 0) return t.minutes;
    if (const DayPart* p = dayPartNamed(c, t.part)) return p->start;
    return -1;
}

std::string dayPartsText(const Calendar& c) {
    if (c.dayParts.empty()) return "none";
    std::string s;
    for (auto& p : c.dayParts) s += (s.empty() ? "" : ", ") + p.name + " " + formatTime(c, p.start);
    return s;
}

std::vector<DayPart> parseDayParts(const Calendar& c, const std::string& text, std::vector<std::string>* warnings) {
    std::vector<DayPart> out;
    if (lower(trim(text)) == "none") return out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (item.empty()) continue;
        // "Witching hour 3:00": the time is the last word; "from" may come before it.
        const size_t sp = item.rfind(' ');
        std::string name = sp == std::string::npos ? std::string() : trim(item.substr(0, sp));
        const auto m = sp == std::string::npos ? std::nullopt : parseTime(c, item.substr(sp + 1));
        if (lower(name).size() > 5 && lower(name).compare(lower(name).size() - 5, 5, " from") == 0) name = trim(name.substr(0, name.size() - 5));
        if (name.empty() || !m) { if (warnings) warnings->push_back("A time of day needs a name and the time it starts (Dawn 5:00): " + item); continue; }
        out.push_back({name, *m});
    }
    std::stable_sort(out.begin(), out.end(), [](const DayPart& a, const DayPart& b) { return a.start < b.start; });
    return out;
}

std::optional<DateTime> parseDateTime(const Calendar& c, const std::string& s0) {
    const std::string s = trim(s0);
    if (const auto d = parseDate(c, s)) return DateTime{*d, {}};
    // The time after " at ", a comma, or as the last word(s).
    const std::string l = lower(s);
    std::vector<size_t> cuts;
    for (size_t i = l.size(); i-- > 0;) if (l[i] == ',' || l[i] == ' ') cuts.push_back(i);
    for (size_t cut : cuts) {
        std::string datePart = trim(s.substr(0, cut)), timePart = trim(s.substr(cut + 1));
        while (!datePart.empty() && datePart.back() == ',') datePart = trim(datePart.substr(0, datePart.size() - 1));
        const auto t = parseTimeOfDay(c, timePart);
        if (!t) continue;
        if (const auto d = parseDate(c, datePart)) return DateTime{*d, *t};
    }
    return std::nullopt;
}

std::optional<Date> parseDate(const Calendar& c, const std::string& s0) {
    if (c.months.empty()) return std::nullopt;
    // Split into words and numbers; keep a sign on a number written "-12".
    std::vector<std::string> tok;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } };
    std::string s = trim(s0);
    // An era at the end, by name (which may be several words) or abbreviation, after a space or a "|" (0.80:
    // "3 Frostmoon 1204 | AE", "01/00/1204 Age of Embers"). The longest that fits is taken.
    std::string era;
    {
        std::vector<std::string> names;
        for (auto& e : c.eras) { names.push_back(e.name); names.push_back(e.abbr); }
        names.push_back(c.beforeName);
        names.push_back(c.beforeAbbr);
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        const std::string ls = lower(s);
        for (const std::string& n : names) {
            const std::string ln = lower(trim(n));
            if (ln.empty() || ls.size() <= ln.size() || ls.compare(ls.size() - ln.size(), ln.size(), ln) != 0) continue;
            const char before = ls[ls.size() - ln.size() - 1];
            if (!std::isspace((unsigned char)before) && before != '|' && !std::isdigit((unsigned char)before)) continue;
            era = trim(n);
            s = trim(s.substr(0, s.size() - ln.size()));
            while (!s.empty() && (s.back() == '|' || s.back() == ',')) s = trim(s.substr(0, s.size() - 1));
            break;
        }
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char ch = (unsigned char)s[i];
        if (std::isspace(ch) || ch == ',' || ch == '/' || ch == '.' || ch == '|') { flush(); continue; }
        if (ch == '-') {
            // A minus starts a negative number only at the start or after a space; otherwise it separates.
            const bool sign = cur.empty() && (i == 0 || std::isspace((unsigned char)s[i - 1])) && i + 1 < s.size() &&
                              std::isdigit((unsigned char)s[i + 1]) && tok.empty();
            if (sign) { cur += '-'; continue; }
            flush();
            continue;
        }
        // "3rd" → "3" "rd": letters next to numbers split (bytes of other scripts stay with their word).
        if (!cur.empty() && cur != "-" && ch < 0x80 && (unsigned char)cur.back() < 0x80 &&
            (bool)std::isdigit((unsigned char)cur.back()) != (bool)std::isdigit(ch))
            flush();
        cur += (char)ch;
    }
    flush();
    // Drop ordinal endings and filler words.
    std::vector<std::string> t;
    for (auto& x : tok) {
        const std::string l = lower(x);
        if (l == "st" || l == "nd" || l == "rd" || l == "th" || l == "of" || l == "the" || l == "day" || l == "year") continue;
        t.push_back(x);
    }
    int month = -1;
    std::vector<std::int64_t> nums;
    std::vector<size_t> numAt;
    size_t monthAt = std::string::npos;
    for (size_t i = 0; i < t.size(); ++i) {
        const std::string l = lower(t[i]);
        if (isNumber(t[i])) { nums.push_back(std::strtoll(t[i].c_str(), nullptr, 10)); numAt.push_back(i); continue; }
        // "Month 3"
        if (l == "month" && i + 1 < t.size() && isNumber(t[i + 1])) {
            const int n = std::atoi(t[i + 1].c_str());
            const int idx = c.monthZero == MonthZero::None ? n - 1 : n;
            if (idx < 0 || idx >= (int)c.months.size()) return std::nullopt;
            month = idx; monthAt = i; ++i;
            continue;
        }
        // An era?
        bool isEra = false;
        for (auto& e : c.eras) if ((!e.abbr.empty() && lower(e.abbr) == l) || lower(e.name) == l) isEra = true;
        if (!c.beforeAbbr.empty() && lower(c.beforeAbbr) == l) isEra = true;
        if (isEra) { era = t[i]; continue; }
        // A month by name: exact, else a unique start of 3+ letters. Names may be several words.
        int found = -1, hits = 0;
        for (int m = 0; m < (int)c.months.size(); ++m) {
            const std::string name = lower(c.months[m].name);
            if (name == l) { found = m; hits = 1; break; }
        }
        if (found < 0) {
            // several words: try joining with following words
            for (int m = 0; m < (int)c.months.size() && found < 0; ++m) {
                const std::string name = lower(c.months[m].name);
                std::string joined = l;
                for (size_t j = i + 1; j < t.size() && joined.size() < name.size(); ++j) {
                    joined += " " + lower(t[j]);
                    if (joined == name) { found = m; hits = 1; i = j; break; }
                }
            }
        }
        if (found < 0 && l.size() >= 3) {
            for (int m = 0; m < (int)c.months.size(); ++m)
                if (lower(c.months[m].name).rfind(l, 0) == 0) { found = m; ++hits; }
        }
        if (found >= 0 && hits == 1) { month = found; monthAt = i; continue; }
        return std::nullopt;   // a word we don't know
    }
    Date d;
    if (month >= 0) {
        if (nums.size() == 1) {
            // "March 1990" is a month of a year; "3 March" is a day with no year, which isn't a date.
            if (numAt[0] < monthAt && c.order != "ymd") return std::nullopt;
            d.year = nums[0];
            d.day = 1;
        }
        else if (nums.size() == 2) {
            // the day is the number next to the month (before it, or after it for "Frostmoon 3, 1204")
            if (numAt[0] < monthAt && numAt[1] > monthAt) { d.day = (int)nums[0]; d.year = nums[1]; }
            else if (numAt[0] > monthAt) { d.day = (int)nums[0]; d.year = nums[1]; }
            else { d.year = nums[0]; d.day = (int)nums[1]; }   // "1204, 3 Frostmoon" (ymd) or "1204 3 Frostmoon"
        } else return std::nullopt;
        d.month = month;
    } else {
        if (nums.size() != 3) return std::nullopt;
        std::int64_t a = nums[0], b = nums[1], cc = nums[2];
        std::int64_t y, mo, dd;
        if (c.order == "ymd" || std::llabs(a) > 31) { y = a; mo = b; dd = cc; }
        else if (c.order == "mdy") { mo = a; dd = b; y = cc; }
        else { dd = a; mo = b; y = cc; }
        const std::int64_t idx = c.monthZero == MonthZero::None ? mo - 1 : mo;
        if (idx < 0 || idx >= (std::int64_t)c.months.size()) return std::nullopt;
        d.year = y; d.month = (int)idx; d.day = (int)dd;
    }
    if (era.empty() && c.eraStyle == "none" && !c.eras.empty()) {
        // Dates written with no era (0.80) are years of the latest era.
        const Era* last = &c.eras.front();
        for (auto& x : c.eras) if (x.start >= last->start) last = &x;
        era = !last->abbr.empty() ? last->abbr : last->name;
    }
    d.year = plainYear(c, d.year, era);
    if (d.day < 1 || d.day > monthLength(c, d.year, d.month)) return std::nullopt;
    return d;
}

int ageAt(const Calendar& c, const Date& born, const Date& at) {
    (void)c;
    std::int64_t a = at.year - born.year;
    if (at.month < born.month || (at.month == born.month && at.day < born.day)) --a;
    return (int)a;
}

double moonPhase(const Moon& m, std::int64_t day) {
    if (m.cycle <= 0) return 0;
    double p = std::fmod(((double)day + m.offset) / m.cycle, 1.0);
    if (p < 0) p += 1;
    return p;
}

namespace {
// The last time the moon's anchor came round on or before `day`; false when it has no anchor.
bool lastAnchor(const Calendar& c, const Moon& m, std::int64_t day, std::int64_t& last) {
    const MoonAnchor& a = m.anchor;
    if (a.repeat == MoonAnchor::None || c.months.empty()) return false;
    const Date d = fromDay(c, day);
    auto at = [&](std::int64_t year, int month) {
        return toDay(c, {year, month, std::clamp(a.day, 1, std::max(1, monthLength(c, year, month)))});
    };
    if (a.repeat == MoonAnchor::Year) {
        const int month = std::clamp(a.month, 0, (int)c.months.size() - 1);
        last = at(d.year, month);
        if (last > day) last = at(d.year - 1, month);
        return true;
    }
    // This month's anchor if it has come, else the last month before it that has one.
    std::int64_t y = d.year;
    int mo = d.month;
    for (int guard = 0; guard <= (int)c.months.size() * 2 + 1; ++guard) {
        if (std::find(a.except.begin(), a.except.end(), mo) == a.except.end()) {
            const std::int64_t t = at(y, mo);
            if (t <= day) { last = t; return true; }
        }
        if (--mo < 0) { mo = (int)c.months.size() - 1; --y; }
    }
    return false;   // every month excepted
}

struct PauseSpan { std::int64_t from, to; const MoonPause* p; };
// Each pause's days in the years y0..y1 (a pause whose end comes before its start runs on into the next year).
std::vector<PauseSpan> pauseSpans(const Calendar& c, const Moon& m, std::int64_t y0, std::int64_t y1) {
    std::vector<PauseSpan> out;
    if (c.months.empty()) return out;
    const int last = (int)c.months.size() - 1;
    for (std::int64_t y = y0; y <= y1; ++y)
        for (const MoonPause& p : m.pauses) {
            const int fm = std::clamp(p.fromMonth, 0, last), tm = std::clamp(p.toMonth, 0, last);
            const std::int64_t a = toDay(c, {y, fm, std::clamp(p.fromDay, 1, std::max(1, monthLength(c, y, fm)))});
            const bool wraps = tm < fm || (tm == fm && p.toDay < p.fromDay);
            const std::int64_t ty = wraps ? y + 1 : y;
            const std::int64_t b = toDay(c, {ty, tm, std::clamp(p.toDay, 1, std::max(1, monthLength(c, ty, tm)))});
            if (b >= a) out.push_back({a, b, &p});
        }
    std::sort(out.begin(), out.end(), [](const PauseSpan& x, const PauseSpan& y) { return x.from < y.from; });
    return out;
}
}  // namespace

double moonPhase(const Calendar& c, const Moon& m, std::int64_t day, std::int64_t* anchorDay) {
    if (c.months.empty() || m.cycle <= 0) return moonPhase(m, day);
    // Where it is reckoned from: its anchor's last coming, else day 0 (pauses before year 1 aren't counted).
    std::int64_t ref = 0;
    double pos = m.offset;
    if (lastAnchor(c, m, day, ref)) { pos = m.anchor.days; if (anchorDay) *anchorDay = ref; }
    else if (m.anchor.repeat != MoonAnchor::None || m.pauses.empty() || day < 0) return moonPhase(m, day);
    if (!m.pauses.empty() && day >= ref) {
        // A pause that sets a phase starts the reckoning again where it ends, so only the last such pause before the
        // day matters; look back from the day's year for it before walking the rest.
        const std::int64_t y1 = fromDay(c, day).year;
        std::int64_t y0 = fromDay(c, ref).year - 1;
        for (std::int64_t y = y1; y > y0 && y >= y1 - 2; --y)
            for (const PauseSpan& sp : pauseSpans(c, m, y, y))
                if (sp.p->setsPhase && sp.from <= day && sp.from >= ref) { y0 = std::max(y0, y - 1); break; }
        std::int64_t cur = ref;
        for (const PauseSpan& sp : pauseSpans(c, m, y0, y1)) {
            if (sp.to < cur) continue;
            if (sp.from > day) break;
            const std::int64_t st = std::max(sp.from, cur);
            pos += (double)(st - cur);
            cur = st;
            if (day <= sp.to) {   // within the pause
                double p = std::fmod((sp.p->setsPhase ? sp.p->days : pos) / m.cycle, 1.0);
                return p < 0 ? p + 1 : p;
            }
            if (sp.p->setsPhase) pos = sp.p->days;
            cur = sp.to;   // the last held day: the day after is one day on
        }
        pos += (double)(day - cur);
    } else {
        pos += (double)(day - ref);
    }
    double p = std::fmod(pos / m.cycle, 1.0);
    if (p < 0) p += 1;
    return p;
}

bool pausedOn(const Calendar& c, const Moon& m, std::int64_t day) {
    if (m.pauses.empty() || c.months.empty()) return false;
    const std::int64_t y = fromDay(c, day).year;
    for (const PauseSpan& sp : pauseSpans(c, m, y - 1, y)) if (sp.from <= day && day <= sp.to) return true;
    return false;
}

std::vector<EclipseOn> eclipsesOn(const Calendar& c, std::int64_t day) {
    std::vector<EclipseOn> out;
    if (c.months.empty()) return out;
    const Date d = fromDay(c, day);
    for (const Moon& m : c.moons)
        for (const Eclipse& e : m.eclipses) {
            bool on = false;
            switch (e.when) {
                case Eclipse::EveryYear: on = d.month == e.month && d.day == e.day; break;
                case Eclipse::Once: on = d.year == e.year && d.month == e.month && d.day == e.day; break;
                case Eclipse::AtPhaseOnDate:
                case Eclipse::AtPhaseInMonth: {
                    if (d.month != e.month || (e.when == Eclipse::AtPhaseOnDate && d.day != e.day)) break;
                    // The day it turns full (or new): crossing it, or brought to it by an anchor or a pause.
                    const double ph = moonPhase(c, m, day), prev = moonPhase(c, m, day - 1);
                    on = e.full ? (prev < 0.5 && ph >= 0.5) || (std::fabs(ph - 0.5) < 1e-9 && std::fabs(prev - 0.5) > 1e-9)
                                : (prev - ph > 0.5) || (ph < 1e-9 && prev > 1e-9);
                    break;
                }
            }
            if (on) out.push_back({&m, e.kind, e.time});
        }
    return out;
}

namespace {
std::string phaseWordsFor(double c, double d) {
    auto at = [&](double x) { return std::fabs(d - x) < 1e-6; };
    if (at(0)) return "new";
    if (at(c / 2)) return "full";
    if (at(c / 4)) return "first quarter";
    if (at(c * 3 / 4)) return "last quarter";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%g days in", d);
    return buf;
}
std::string phaseWords(const Moon& m) { return phaseWordsFor(m.cycle, m.anchor.days); }
// Reads a phase from the start of `t` (lower case): new, full, first or last quarter, or "N days in".
bool takePhase(std::string& t, double cycle, double& days) {
    auto take = [&](const std::string& w) { if (t.rfind(w, 0) == 0 && (t.size() == w.size() || t[w.size()] == ' ')) { t = trim(t.substr(w.size())); return true; } return false; };
    if (take("new moon") || take("new")) days = 0;
    else if (take("full moon") || take("full")) days = cycle / 2;
    else if (take("first quarter")) days = cycle / 4;
    else if (take("last quarter") || take("third quarter")) days = cycle * 3 / 4;
    else {
        char* end = nullptr;
        const double n = std::strtod(t.c_str(), &end);
        if (end == t.c_str()) return false;
        std::string rest = trim(std::string(end));
        bool ok = false;
        for (const char* w : {"days into its cycle", "days into the cycle", "days in", "day in"})
            if (rest.rfind(w, 0) == 0) { rest = trim(rest.substr(std::strlen(w))); ok = true; break; }
        if (!ok) return false;
        t = rest;
        days = n;
    }
    return true;
}
// "3 Thaw" (or "3rd Thaw") at the start of `t`: the day and the month; the rest is left in `t`.
bool takeDayMonth(const Calendar& c, std::string& t, int& day, int& month) {
    if (t.empty() || !std::isdigit((unsigned char)t[0])) return false;
    day = std::atoi(t.c_str());
    size_t k = 0;
    while (k < t.size() && std::isdigit((unsigned char)t[k])) ++k;
    t = trim(t.substr(k));
    for (const char* suf : {"st", "nd", "rd", "th"}) if (t.rfind(suf, 0) == 0 && (t.size() == 2 || t[2] == ' ')) { t = trim(t.substr(2)); break; }
    // The longest month name the rest starts with.
    int best = -1;
    size_t bestLen = 0;
    for (int i = 0; i < (int)c.months.size(); ++i)
        for (const std::string& name : {lower(c.months[i].name), lower(monthLabel(c, i))})
            if (!name.empty() && t.rfind(name, 0) == 0 && (t.size() == name.size() || t[name.size()] == ' ') && name.size() > bestLen) {
                best = i;
                bestLen = name.size();
            }
    if (best < 0) return false;
    month = best;
    t = trim(t.substr(bestLen));
    return day >= 1;
}
}  // namespace

std::string anchorText(const Calendar& c, const Moon& m) {
    const MoonAnchor& a = m.anchor;
    if (a.repeat == MoonAnchor::None) return {};
    std::string s = phaseWords(m) + " on ";
    if (a.repeat == MoonAnchor::Year) return s + std::to_string(a.day) + " " + monthLabel(c, a.month) + " every year";
    s += std::to_string(a.day) + " of every month";
    for (size_t i = 0; i < a.except.size(); ++i)
        s += (i == 0 ? " except " : i + 1 == a.except.size() ? " and " : ", ") + monthLabel(c, a.except[i]);
    return s;
}

bool parseAnchor(const Calendar& c, const std::string& text, Moon& m) {
    std::string t = lower(trim(text));
    MoonAnchor a;
    // The phase: new, full, first quarter, last quarter, or "N days in".
    auto take = [&](const std::string& w) { if (t.rfind(w, 0) == 0) { t = trim(t.substr(w.size())); return true; } return false; };
    if (!takePhase(t, m.cycle, a.days)) return false;
    if (!take("on")) return false;
    if (!t.empty() && !std::isdigit((unsigned char)t[0])) return false;
    a.day = std::atoi(t.c_str());
    size_t k = 0;
    while (k < t.size() && std::isdigit((unsigned char)t[k])) ++k;
    t = trim(t.substr(k));
    for (const char* suf : {"st", "nd", "rd", "th"}) if (t.rfind(suf, 0) == 0 && (t.size() == 2 || t[2] == ' ')) { t = trim(t.substr(2)); break; }
    auto monthOf = [&](const std::string& name) -> int {
        const std::string n = trim(name);
        for (int i = 0; i < (int)c.months.size(); ++i) if (lower(c.months[i].name) == n || lower(monthLabel(c, i)) == n) return i;
        return -1;
    };
    if (take("of every month") || take("every month") || take("of each month") || take("each month")) {
        a.repeat = MoonAnchor::Month;
        if (take("except") || take("but not") || take("skipping")) {
            std::string rest = t;
            for (std::string sep : {" and ", ", "}) {
                size_t at;
                while ((at = rest.find(sep)) != std::string::npos) rest.replace(at, sep.size(), "|");
            }
            std::stringstream ss(rest);
            std::string part;
            while (std::getline(ss, part, '|')) {
                const int mo = monthOf(part);
                if (mo < 0) return false;
                a.except.push_back(mo);
            }
        } else if (!t.empty()) return false;
    } else {
        const size_t every = t.rfind(" every year");
        const size_t each = t.rfind(" each year");
        const size_t cut = every != std::string::npos ? every : each;
        if (cut == std::string::npos || trim(t.substr(cut)).size() != std::string("every year").size()) return false;
        a.month = monthOf(t.substr(0, cut));
        if (a.month < 0) return false;
        a.repeat = MoonAnchor::Year;
    }
    if (a.day < 1) return false;
    m.anchor = a;
    return true;
}

std::string pauseText(const Calendar& c, const Moon& m, const MoonPause& p) {
    std::string s = "holds ";
    if (p.setsPhase) s += phaseWordsFor(m.cycle, p.days) + " ";
    return s + "from " + std::to_string(p.fromDay) + " " + monthLabel(c, p.fromMonth) + " to " + std::to_string(p.toDay) + " " +
           monthLabel(c, p.toMonth);
}

bool parsePause(const Calendar& c, const std::string& text, const Moon& m, MoonPause& out) {
    std::string t = lower(trim(text));
    auto take = [&](const std::string& w) { if (t.rfind(w, 0) == 0) { t = trim(t.substr(w.size())); return true; } return false; };
    if (!(take("holds") || take("pauses") || take("rests"))) return false;
    MoonPause p;
    if (!take("from")) {
        if (!takePhase(t, m.cycle, p.days)) return false;
        p.setsPhase = true;
        if (!take("from")) return false;
    }
    if (!takeDayMonth(c, t, p.fromDay, p.fromMonth)) return false;
    if (!(take("to") || take("until") || take("till"))) return false;
    if (!takeDayMonth(c, t, p.toDay, p.toMonth) || !t.empty()) return false;
    out = p;
    return true;
}

std::string eclipseText(const Calendar& c, const Eclipse& e) {
    std::string s = std::string(e.kind == Eclipse::Solar ? "solar" : "lunar") + " eclipse ";
    const std::string date = std::to_string(e.day) + " " + monthLabel(c, e.month);
    switch (e.when) {
        case Eclipse::EveryYear: s += "on " + date + " every year"; break;
        case Eclipse::Once: s += "on " + date + " " + yearWithEra(c, e.year); break;
        case Eclipse::AtPhaseOnDate: s += std::string("at every ") + (e.full ? "full" : "new") + " moon on " + date; break;
        case Eclipse::AtPhaseInMonth: s += std::string("at every ") + (e.full ? "full" : "new") + " moon in " + monthLabel(c, e.month); break;
    }
    if (!e.time.empty()) s += " at " + timeOfDayText(c, e.time);
    return s;
}

bool parseEclipse(const Calendar& c, const std::string& text0, Eclipse& out) {
    // A time of day at the end (0.80): "... every year at midnight".
    std::string text = trim(text0);
    TimeOfDay when;
    {
        const size_t at = lower(text).rfind(" at ");
        if (at != std::string::npos && lower(text).compare(at + 4, 5, "every") != 0 && lower(text).compare(at + 4, 4, "each") != 0)
            if (const auto tt = parseTimeOfDay(c, text.substr(at + 4))) { when = *tt; text = trim(text.substr(0, at)); }
    }
    std::string t = lower(trim(text));
    auto take = [&](const std::string& w) { if (t.rfind(w, 0) == 0) { t = trim(t.substr(w.size())); return true; } return false; };
    Eclipse e;
    if (take("solar")) e.kind = Eclipse::Solar;
    else if (take("lunar")) e.kind = Eclipse::Lunar;
    else return false;
    if (!take("eclipse")) return false;
    if (take("at every") || take("at each") || take("at the")) {
        if (take("full moon") || take("full")) e.full = true;
        else if (take("new moon") || take("new")) e.full = false;
        else return false;
        if (take("in")) {
            e.when = Eclipse::AtPhaseInMonth;
            int idx = -1;
            for (int i = 0; i < (int)c.months.size(); ++i)
                if (lower(c.months[i].name) == t || lower(monthLabel(c, i)) == t) idx = i;
            if (idx < 0) return false;
            e.month = idx;
        } else if (take("on")) {
            e.when = Eclipse::AtPhaseOnDate;
            if (!takeDayMonth(c, t, e.day, e.month) || !t.empty()) return false;
        } else return false;
    } else if (take("on")) {
        if (!takeDayMonth(c, t, e.day, e.month)) return false;
        if (t == "every year" || t == "each year") e.when = Eclipse::EveryYear;
        else {
            // A year, with its era if given.
            if (t.empty() || !(std::isdigit((unsigned char)t[0]) || t[0] == '-')) return false;
            const long long y = std::strtoll(t.c_str(), nullptr, 10);
            const size_t sp = t.find(' ');
            std::string era = sp == std::string::npos ? std::string() : trim(t.substr(sp + 1));
            // The era as written (any case).
            std::string eraAs;
            for (auto& x : c.eras) if (lower(x.abbr) == era || lower(x.name) == era) eraAs = x.abbr.empty() ? x.name : x.abbr;
            if (!era.empty() && (lower(c.beforeAbbr) == era || lower(c.beforeName) == era)) eraAs = c.beforeAbbr.empty() ? c.beforeName : c.beforeAbbr;
            if (!era.empty() && eraAs.empty()) return false;
            e.year = plainYear(c, y, eraAs);
            e.when = Eclipse::Once;
        }
    } else return false;
    e.time = when;
    out = e;
    return true;
}

std::string phaseName(double p) {
    if (p < 0.0339 || p >= 0.9661) return "New moon";
    if (p < 0.2161) return "Waxing crescent";
    if (p < 0.2839) return "First quarter";
    if (p < 0.4661) return "Waxing gibbous";
    if (p < 0.5339) return "Full moon";
    if (p < 0.7161) return "Waning gibbous";
    if (p < 0.7839) return "Last quarter";
    return "Waning crescent";
}

const Season* seasonOn(const Calendar& c, const Date& d) {
    if (c.seasons.empty()) return nullptr;
    auto key = [](int m, int dd) { return m * 1000 + dd; };
    const int here = key(d.month, d.day);
    const Season* best = nullptr;
    int bestKey = -1;
    for (auto& s : c.seasons) {
        const int k = key(s.month, s.day);
        if (k <= here && k > bestKey) { best = &s; bestKey = k; }
    }
    if (best) return best;
    // Before the first season starts this year: the last one of the year before.
    for (auto& s : c.seasons) {
        const int k = key(s.month, s.day);
        if (k > bestKey) { best = &s; bestKey = k; }
    }
    return best;
}

std::vector<const Holiday*> holidaysOn(const Calendar& c, const Date& d) {
    std::vector<const Holiday*> out;
    for (auto& h : c.holidays) if (h.month == d.month && h.day == d.day) out.push_back(&h);
    return out;
}

std::vector<std::string> problems(const Calendar& c) {
    std::vector<std::string> out;
    if (c.months.empty()) { out.push_back("The calendar has no months."); return out; }
    for (int m = 0; m < (int)c.months.size(); ++m)
        if (c.months[m].days < 0) out.push_back(monthLabel(c, m) + " has fewer than no days.");
    if (baseYear(c) <= 0) out.push_back("A year has no days: give a month some days.");
    for (int m = 0; m < (int)c.months.size(); ++m) {
        int least = std::max(0, c.months[m].days);
        for (auto& r : c.leap) if (r.month == m && r.add < 0) least += r.add;
        if (least < 0) out.push_back("A leap rule can take " + monthLabel(c, m) + " below no days.");
    }
    for (auto& r : c.leap) {
        if (r.every <= 0) out.push_back("A leap rule needs a number of years (every 4, every 100...).");
        if (r.month < 0 || r.month >= (int)c.months.size()) out.push_back("A leap rule names a month the calendar doesn't have.");
    }
    for (auto& m : c.moons) {
        if (m.anchor.repeat == MoonAnchor::Year && (m.anchor.month < 0 || m.anchor.month >= (int)c.months.size()))
            out.push_back("The moon " + m.name + " is anchored to a month the calendar doesn't have.");
        if (m.anchor.repeat == MoonAnchor::Month && (int)m.anchor.except.size() >= (int)c.months.size())
            out.push_back("The moon " + m.name + " is anchored every month but skips them all.");
        for (auto& p : m.pauses)
            if (p.fromMonth < 0 || p.fromMonth >= (int)c.months.size() || p.toMonth < 0 || p.toMonth >= (int)c.months.size())
                out.push_back("A pause of the moon " + m.name + " names a month the calendar doesn't have.");
        for (auto& e : m.eclipses)
            if (e.month < 0 || e.month >= (int)c.months.size())
                out.push_back("An eclipse of the moon " + m.name + " names a month the calendar doesn't have.");
    }
    for (auto& s : c.seasons)
        if (s.month < 0 || s.month >= (int)c.months.size() || s.day < 1 || s.day > c.months[s.month].days + 1)
            out.push_back("The season " + s.name + " starts on a day the calendar doesn't have.");
    for (auto& h : c.holidays)
        if (h.month < 0 || h.month >= (int)c.months.size() || h.day < 1 || h.day > c.months[h.month].days + 1)
            out.push_back("The holiday " + h.name + " falls on a day the calendar doesn't have.");
    if (c.hoursPerDay < 1 || c.minutesPerHour < 1) out.push_back("A day needs at least one hour of at least one minute.");
    for (auto& e : c.eras) {
        std::int64_t next = 0;
        if (e.countDown && !nextEraStart(c, e, next))
            out.push_back("The era " + e.name + " counts down, but no era comes after it to count down to: its years count up.");
    }
    const int perDay = std::max(1, c.hoursPerDay) * std::max(1, c.minutesPerHour);
    for (auto& p : c.dayParts)
        if (p.start < 0 || p.start >= perDay) out.push_back("The time of day " + p.name + " starts at a time the day doesn't have.");
    return out;
}

// ------------------------------------------------------------------ JSON

namespace {
void timeTo(json& o, const TimeOfDay& t) {
    if (t.minutes >= 0) o["time"] = t.minutes;
    if (!t.part.empty()) o["part"] = t.part;
}
TimeOfDay timeFrom(const json& o) {
    TimeOfDay t;
    if (o.contains("time") && o["time"].is_number_integer()) t.minutes = std::max(-1, o["time"].get<int>());
    if (o.contains("part") && o["part"].is_string()) t.part = o["part"].get<std::string>();
    return t;
}
}  // namespace

std::string toJson(const Calendar& c) {
    json j;
    j["name"] = c.name;
    j["monthZero"] = zeroId(c.monthZero);
    j["months"] = json::array();
    for (auto& m : c.months) j["months"].push_back({{"name", m.name}, {"days", m.days}});
    j["weekdays"] = c.weekdays;
    j["firstWeekday"] = c.firstWeekday;
    j["leap"] = json::array();
    for (auto& r : c.leap) j["leap"].push_back({{"every", r.every}, {"offset", r.offset}, {"month", r.month}, {"add", r.add}});
    j["eras"] = json::array();
    for (auto& e : c.eras) {
        json ej{{"name", e.name}, {"abbr", e.abbr}, {"start", e.start}};
        if (e.countDown) ej["countDown"] = true;
        j["eras"].push_back(ej);
    }
    j["before"] = {{"name", c.beforeName}, {"abbr", c.beforeAbbr}};
    j["moons"] = json::array();
    for (auto& m : c.moons) {
        json mj{{"name", m.name}, {"cycle", m.cycle}, {"offset", m.offset}};
        if (m.anchor.repeat != MoonAnchor::None)
            mj["anchor"] = {{"repeat", m.anchor.repeat == MoonAnchor::Year ? "year" : "month"}, {"month", m.anchor.month},
                            {"day", m.anchor.day}, {"days", m.anchor.days}, {"except", m.anchor.except}};
        if (!m.pauses.empty()) {
            mj["pauses"] = json::array();
            for (auto& p : m.pauses)
                mj["pauses"].push_back({{"fromMonth", p.fromMonth}, {"fromDay", p.fromDay}, {"toMonth", p.toMonth}, {"toDay", p.toDay},
                                        {"setsPhase", p.setsPhase}, {"days", p.days}});
        }
        if (!m.eclipses.empty()) {
            mj["eclipses"] = json::array();
            static const char* whens[] = {"year", "once", "phase-on", "phase-in"};
            for (auto& e : m.eclipses) {
                json ej{{"kind", e.kind == Eclipse::Solar ? "solar" : "lunar"}, {"when", whens[e.when]}, {"full", e.full},
                        {"month", e.month}, {"day", e.day}, {"year", e.year}};
                timeTo(ej, e.time);
                mj["eclipses"].push_back(ej);
            }
        }
        j["moons"].push_back(mj);
    }
    j["seasons"] = json::array();
    for (auto& s : c.seasons) j["seasons"].push_back({{"name", s.name}, {"month", s.month}, {"day", s.day}});
    j["holidays"] = json::array();
    for (auto& h : c.holidays) {
        json hj{{"name", h.name}, {"month", h.month}, {"day", h.day}, {"entry", h.entry}, {"notes", h.notes}};
        if (!h.symbol.empty()) hj["symbol"] = h.symbol;
        timeTo(hj, h.time);
        j["holidays"].push_back(hj);
    }
    j["hoursPerDay"] = c.hoursPerDay;
    j["minutesPerHour"] = c.minutesPerHour;
    j["order"] = c.order;
    j["numeric"] = c.numeric;
    j["separator"] = std::string(1, c.separator ? c.separator : '/');
    j["eraStyle"] = c.eraStyle;
    j["dayParts"] = json::array();
    for (auto& p : c.dayParts) j["dayParts"].push_back({{"name", p.name}, {"start", p.start}});
    return j.dump();
}

Calendar fromJson(const std::string& text) {
    Calendar c;
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return c;
    auto str = [](const json& o, const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); };
    auto num = [](const json& o, const char* k, double d) { return o.contains(k) && o[k].is_number() ? o[k].get<double>() : d; };
    c.name = str(j, "name");
    c.monthZero = zeroFrom(str(j, "monthZero"));
    if (j.contains("months") && j["months"].is_array())
        for (auto& m : j["months"]) if (m.is_object()) c.months.push_back({str(m, "name"), (int)num(m, "days", 30)});
    if (j.contains("weekdays") && j["weekdays"].is_array())
        for (auto& w : j["weekdays"]) if (w.is_string()) c.weekdays.push_back(w.get<std::string>());
    c.firstWeekday = (int)num(j, "firstWeekday", 0);
    if (j.contains("leap") && j["leap"].is_array())
        for (auto& r : j["leap"])
            if (r.is_object()) c.leap.push_back({(int)num(r, "every", 4), (int)num(r, "offset", 0), (int)num(r, "month", 0), (int)num(r, "add", 1)});
    if (j.contains("eras") && j["eras"].is_array())
        for (auto& e : j["eras"])
            if (e.is_object())
                c.eras.push_back({str(e, "name"), str(e, "abbr"), (std::int64_t)num(e, "start", 1),
                                  e.contains("countDown") && e["countDown"].is_boolean() && e["countDown"].get<bool>()});
    if (j.contains("before") && j["before"].is_object()) { c.beforeName = str(j["before"], "name"); c.beforeAbbr = str(j["before"], "abbr"); }
    if (j.contains("moons") && j["moons"].is_array())
        for (auto& m : j["moons"]) {
            if (!m.is_object()) continue;
            Moon mo{str(m, "name"), num(m, "cycle", 29.53), num(m, "offset", 0), {}, {}, {}};
            if (m.contains("anchor") && m["anchor"].is_object()) {
                const json& a = m["anchor"];
                const std::string r = str(a, "repeat");
                mo.anchor.repeat = r == "year" ? MoonAnchor::Year : r == "month" ? MoonAnchor::Month : MoonAnchor::None;
                mo.anchor.month = (int)num(a, "month", 0);
                mo.anchor.day = std::max(1, (int)num(a, "day", 1));
                mo.anchor.days = num(a, "days", 0);
                if (a.contains("except") && a["except"].is_array())
                    for (auto& x : a["except"]) if (x.is_number_integer()) mo.anchor.except.push_back(x.get<int>());
            }
            if (m.contains("pauses") && m["pauses"].is_array())
                for (auto& p : m["pauses"]) {
                    if (!p.is_object()) continue;
                    MoonPause mp;
                    mp.fromMonth = (int)num(p, "fromMonth", 0); mp.fromDay = std::max(1, (int)num(p, "fromDay", 1));
                    mp.toMonth = (int)num(p, "toMonth", 0); mp.toDay = std::max(1, (int)num(p, "toDay", 1));
                    mp.setsPhase = p.contains("setsPhase") && p["setsPhase"].is_boolean() && p["setsPhase"].get<bool>();
                    mp.days = num(p, "days", 0);
                    mo.pauses.push_back(mp);
                }
            if (m.contains("eclipses") && m["eclipses"].is_array())
                for (auto& x : m["eclipses"]) {
                    if (!x.is_object()) continue;
                    Eclipse e;
                    e.kind = str(x, "kind") == "solar" ? Eclipse::Solar : Eclipse::Lunar;
                    const std::string wh = str(x, "when");
                    e.when = wh == "once" ? Eclipse::Once : wh == "phase-on" ? Eclipse::AtPhaseOnDate : wh == "phase-in" ? Eclipse::AtPhaseInMonth : Eclipse::EveryYear;
                    e.full = !x.contains("full") || !x["full"].is_boolean() || x["full"].get<bool>();
                    e.month = (int)num(x, "month", 0); e.day = std::max(1, (int)num(x, "day", 1)); e.year = (std::int64_t)num(x, "year", 1);
                    e.time = timeFrom(x);
                    mo.eclipses.push_back(e);
                }
            c.moons.push_back(mo);
        }
    if (j.contains("seasons") && j["seasons"].is_array())
        for (auto& s : j["seasons"]) if (s.is_object()) c.seasons.push_back({str(s, "name"), (int)num(s, "month", 0), (int)num(s, "day", 1)});
    if (j.contains("holidays") && j["holidays"].is_array())
        for (auto& h : j["holidays"])
            if (h.is_object())
                c.holidays.push_back({str(h, "name"), (int)num(h, "month", 0), (int)num(h, "day", 1), str(h, "entry"), str(h, "notes"), timeFrom(h),
                                      str(h, "symbol")});
    c.hoursPerDay = std::max(1, (int)num(j, "hoursPerDay", 24));
    c.minutesPerHour = std::max(1, (int)num(j, "minutesPerHour", 60));
    c.order = str(j, "order");
    if (c.order != "mdy" && c.order != "ymd") c.order = "dmy";
    c.numeric = j.contains("numeric") && j["numeric"].is_boolean() && j["numeric"].get<bool>();
    const std::string sep = str(j, "separator");
    c.separator = sep == "-" || sep == "." ? sep[0] : '/';
    c.eraStyle = str(j, "eraStyle");
    if (c.eraStyle != "name" && c.eraStyle != "none") c.eraStyle = "abbr";
    if (j.contains("dayParts") && j["dayParts"].is_array()) {
        for (auto& p : j["dayParts"])
            if (p.is_object() && !str(p, "name").empty()) c.dayParts.push_back({str(p, "name"), std::max(0, (int)num(p, "start", 0))});
        std::stable_sort(c.dayParts.begin(), c.dayParts.end(), [](const DayPart& a, const DayPart& b) { return a.start < b.start; });
    } else if (!c.months.empty()) c.dayParts = defaultDayParts(c);   // calendars made before 0.80
    std::stable_sort(c.eras.begin(), c.eras.end(), [](const Era& a, const Era& b) { return a.start < b.start; });
    return c;
}

// ------------------------------------------------------------------ text
//
// Calendar: The Alvoran Reckoning
// Month zero: none            (none | counted from 0 | extra month 0 before month 1)
// Months:
// - Frostmoon: 30
// Week: Sunday, Moonday, ...
// Day 0 falls on: Moonday       (the weekday of the first day of year 1)
// Leap: every 4 years, 1 day to Frostmoon       (also: "every 100 years, -1 day to Frostmoon", "from year 2")
// Era: Age of Light (AL) from year 1
// Before the first era: Before Light (BL)
// Moon: Selene, 29.5 days, 3 days into its cycle on day 0
// Season: Winter from 1 Frostmoon
// Holiday: Founding Day on 3 Frostmoon
// Day: 24 hours of 60 minutes
// Dates: day month year       (or month day year, year month day)

std::string toText(const Calendar& c) {
    std::ostringstream o;
    auto dayMonth = [&](int m, int d) { return std::to_string(d) + " " + monthLabel(c, m); };
    o << "Calendar: " << c.name << "\n";
    o << "Month zero: " << (c.monthZero == MonthZero::Count ? "counted from 0" : c.monthZero == MonthZero::Extra ? "extra month 0 before month 1" : "none") << "\n";
    o << "Months:\n";
    for (auto& m : c.months) o << "- " << m.name << ": " << m.days << "\n";
    if (!c.weekdays.empty()) {
        o << "Week: ";
        for (size_t i = 0; i < c.weekdays.size(); ++i) o << (i ? ", " : "") << c.weekdays[i];
        o << "\n";
        o << "Day 0 falls on: " << c.weekdays[std::clamp(c.firstWeekday, 0, (int)c.weekdays.size() - 1)] << "\n";
    }
    for (auto& r : c.leap) {
        o << "Leap: every " << r.every << " years, " << r.add << (std::abs(r.add) == 1 ? " day" : " days") << " to " << monthLabel(c, r.month);
        if (r.offset) o << ", from year " << r.offset;
        o << "\n";
    }
    for (auto& e : c.eras)
        o << "Era: " << e.name << (e.abbr.empty() ? "" : " (" + e.abbr + ")") << " from year " << e.start << (e.countDown ? ", counting down" : "") << "\n";
    if (!c.beforeName.empty() || !c.beforeAbbr.empty())
        o << "Before the first era: " << c.beforeName << (c.beforeAbbr.empty() ? "" : " (" + c.beforeAbbr + ")") << "\n";
    for (auto& m : c.moons) {
        char buf[96];
        if (m.anchor.repeat != MoonAnchor::None) {
            std::snprintf(buf, sizeof buf, "%g days", m.cycle);
            o << "Moon: " << m.name << ", " << buf << ", " << anchorText(c, m);
        } else {
            std::snprintf(buf, sizeof buf, "%g days, %g days into its cycle on day 0", m.cycle, m.offset);
            o << "Moon: " << m.name << ", " << buf;
        }
        for (auto& p : m.pauses) o << ", " << pauseText(c, m, p);
        for (auto& e : m.eclipses) o << ", " << eclipseText(c, e);
        o << "\n";
    }
    for (auto& s : c.seasons) o << "Season: " << s.name << " from " << dayMonth(s.month, s.day) << "\n";
    for (auto& h : c.holidays)
        o << "Holiday: " << h.name << " on " << dayMonth(h.month, h.day) << (h.time.empty() ? "" : " at " + timeOfDayText(c, h.time))
          << (h.notes.empty() ? "" : " | " + h.notes) << (h.symbol.empty() ? "" : " | symbol: " + h.symbol) << "\n";
    o << "Day: " << c.hoursPerDay << " hours of " << c.minutesPerHour << " minutes\n";
    o << "Times of day: " << dayPartsText(c) << "\n";
    std::string dates;
    if (c.numeric) {
        const std::string sp(1, c.separator ? c.separator : '/');
        dates = c.order == "mdy" ? "mm" + sp + "dd" + sp + "yy" : c.order == "ymd" ? "yy" + sp + "mm" + sp + "dd" : "dd" + sp + "mm" + sp + "yy";
    } else dates = c.order == "mdy" ? "month day year" : c.order == "ymd" ? "year month day" : "day month year";
    if (c.eraStyle == "name") dates += " | era name";
    else if (c.eraStyle == "none") dates += " | no era";
    o << "Dates: " << dates << "\n";
    return o.str();
}

Calendar fromText(const std::string& text, std::vector<std::string>* warnings) {
    Calendar c;
    auto warn = [&](const std::string& s) { if (warnings) warnings->push_back(s); };
    std::istringstream in(text);
    std::string line;
    bool inMonths = false, dayPartsGiven = false;
    std::vector<std::pair<std::string, std::string>> later;   // lines that name months, read once the months are known
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (inMonths && (t[0] == '-' || t[0] == '*')) {
            std::string body = trim(t.substr(1));
            const size_t colon = body.rfind(':');
            Month m;
            if (colon != std::string::npos && isNumber(trim(body.substr(colon + 1)))) {
                m.name = trim(body.substr(0, colon));
                m.days = std::atoi(trim(body.substr(colon + 1)).c_str());
            } else {
                m.name = body;
                warn("No number of days for " + body + ": given 30.");
            }
            c.months.push_back(m);
            continue;
        }
        inMonths = false;
        const size_t colon = t.find(':');
        if (colon == std::string::npos) { warn("Not understood: " + t); continue; }
        const std::string key = lower(trim(t.substr(0, colon)));
        const std::string val = trim(t.substr(colon + 1));
        if (key == "calendar" || key == "name") c.name = val;
        else if (key == "month zero") c.monthZero = zeroFrom(val);
        else if (key == "months") inMonths = true;
        else if (key == "week") {
            std::stringstream ss(val);
            std::string d;
            while (std::getline(ss, d, ',')) if (!trim(d).empty()) c.weekdays.push_back(trim(d));
        } else if (key == "dates" || key == "order") {
            // "day month year", or as numbers (0.80): "dd/mm/yy", "01/00/01 (day month year)", "yy-mm-dd"; then
            // "| era", "| era name" or "| no era".
            std::string l = lower(val), style;
            const size_t bar = l.find('|');
            if (bar != std::string::npos) { style = trim(l.substr(bar + 1)); l = trim(l.substr(0, bar)); }
            c.eraStyle = style.find("no") == 0 || style == "none" ? "none" : style.find("name") != std::string::npos || style.find("full") != std::string::npos ? "name" : "abbr";
            c.numeric = false;
            for (char ch : l) if (ch == '/' || ch == '-' || ch == '.' || std::isdigit((unsigned char)ch)) { c.numeric = true; if (!std::isdigit((unsigned char)ch)) { c.separator = ch; break; } }
            if (l.find("number") != std::string::npos) c.numeric = true;
            // The order: the first of day/month/year words, else of the d/m/y letters.
            size_t di = l.find("day"), mi = l.find("month"), yi = l.find("year");
            if (di == std::string::npos && mi == std::string::npos && yi == std::string::npos) { di = l.find('d'); mi = l.find('m'); yi = l.find('y'); }
            if (yi != std::string::npos && (di == std::string::npos || yi < di) && (mi == std::string::npos || yi < mi)) c.order = "ymd";
            else if (mi != std::string::npos && (di == std::string::npos || mi < di)) c.order = "mdy";
            else c.order = "dmy";
        } else if (key == "times of day" || key == "times") {
            c.dayParts = parseDayParts(c, val, warnings);
            dayPartsGiven = true;
        } else if (key == "day") {
            int h = 24, m = 60;
            if (std::sscanf(val.c_str(), "%d hours of %d minutes", &h, &m) >= 1) { c.hoursPerDay = std::max(1, h); c.minutesPerHour = std::max(1, m); }
            else warn("Not understood: " + t);
        } else if (key == "era") {
            Era e;
            std::string v = val;
            for (const char* w : {", counting down", " counting down", ", counts down", " counts down"}) {
                const std::string ws = w;
                if (lower(v).size() > ws.size() && lower(v).compare(v.size() - ws.size(), ws.size(), ws) == 0) { e.countDown = true; v = trim(v.substr(0, v.size() - ws.size())); break; }
            }
            const size_t from = lower(v).rfind(" from year ");
            if (from != std::string::npos) { e.start = std::strtoll(v.c_str() + from + 11, nullptr, 10); v = trim(v.substr(0, from)); }
            const size_t open = v.rfind('(');
            if (open != std::string::npos && v.back() == ')') { e.abbr = trim(v.substr(open + 1, v.size() - open - 2)); v = trim(v.substr(0, open)); }
            e.name = v;
            c.eras.push_back(e);
        } else if (key == "before the first era" || key == "before") {
            std::string v = val;
            const size_t open = v.rfind('(');
            if (open != std::string::npos && !v.empty() && v.back() == ')') { c.beforeAbbr = trim(v.substr(open + 1, v.size() - open - 2)); v = trim(v.substr(0, open)); }
            c.beforeName = v;
        } else if (key == "moon") {
            Moon m;
            std::vector<std::string> parts;
            std::stringstream ss(val);
            std::string p;
            while (std::getline(ss, p, ',')) parts.push_back(trim(p));
            if (!parts.empty()) m.name = parts[0];
            if (parts.size() > 1) m.cycle = std::atof(parts[1].c_str());
            if (m.cycle <= 0) { warn("A moon needs its cycle in days: " + t); m.cycle = 29.53; }
            // The third part: days into its cycle on day 0, or an anchor to the calendar (0.78), read once the months
            // are known: "full on 15 of every month except Yearsend", "new on 1 Frostmoon every year".
            // Then, in any order (0.79): pauses ("holds new from 1 Rebirth to 14 Rebirth") and eclipses ("lunar eclipse on
            // 3 Frostmoon every year"). A part that starts none of these carries on the one before ("except A, B and C"
            // was split at its commas).
            std::string kind, item;
            auto flush = [&] {
                if (!item.empty()) later.push_back({"moon " + kind, std::to_string(c.moons.size()) + "|" + item});
                item.clear();
            };
            for (size_t k = 2; k < parts.size(); ++k) {
                const std::string pl = lower(parts[k]);
                if (pl.rfind("holds", 0) == 0 || pl.rfind("pauses", 0) == 0 || pl.rfind("rests", 0) == 0) { flush(); kind = "pause"; item = parts[k]; }
                else if (pl.find("eclipse") != std::string::npos) { flush(); kind = "eclipse"; item = parts[k]; }
                else if (pl.find(" on ") != std::string::npos && pl.find("day 0") == std::string::npos) { flush(); kind = "anchor"; item = parts[k]; }
                else if (!item.empty() && kind == "anchor") item += ", " + parts[k];
                else if (item.empty() && kind.empty()) m.offset = std::atof(parts[k].c_str());
                else { flush(); warn("Not understood in the moon " + m.name + ": " + parts[k]); }
            }
            flush();
            c.moons.push_back(m);
        } else if (key == "day 0 falls on" || key == "leap" || key == "season" || key == "holiday") {
            later.push_back({key, val});
        } else warn("Not understood: " + t);
    }
    auto monthNamed = [&](const std::string& s) -> int {
        const std::string l = lower(trim(s));
        for (int i = 0; i < (int)c.months.size(); ++i) if (lower(c.months[i].name) == l) return i;
        if (l.rfind("month ", 0) == 0) {
            const int n = std::atoi(l.c_str() + 6);
            const int i = c.monthZero == MonthZero::None ? n - 1 : n;
            if (i >= 0 && i < (int)c.months.size()) return i;
        }
        return -1;
    };
    auto dayMonth = [&](const std::string& s, int& m, int& d) {
        const std::string v = trim(s);
        const size_t sp = v.find(' ');
        if (sp == std::string::npos || !isNumber(v.substr(0, sp))) return false;
        d = std::atoi(v.c_str());
        m = monthNamed(v.substr(sp + 1));
        return m >= 0;
    };
    for (auto& [key, val] : later) {
        if (key == "moon anchor") {
            const size_t bar = val.find('|');
            const size_t i = (size_t)std::atoi(val.substr(0, bar).c_str());
            if (i < c.moons.size() && !parseAnchor(c, val.substr(bar + 1), c.moons[i]))
                warn("A moon's anchor wasn't understood (like \"full on 15 of every month\" or \"new on 1 Frostmoon every year\"): " + val.substr(bar + 1));
            continue;
        }
        if (key == "moon pause" || key == "moon eclipse") {
            const size_t bar = val.find('|');
            const size_t i = (size_t)std::atoi(val.substr(0, bar).c_str());
            if (i >= c.moons.size()) continue;
            if (key == "moon pause") {
                MoonPause p;
                if (parsePause(c, val.substr(bar + 1), c.moons[i], p)) c.moons[i].pauses.push_back(p);
                else warn("A moon's pause wasn't understood (like \"holds new from 1 Rebirth to 14 Rebirth\"): " + val.substr(bar + 1));
            } else {
                Eclipse e;
                if (parseEclipse(c, val.substr(bar + 1), e)) c.moons[i].eclipses.push_back(e);
                else warn("An eclipse wasn't understood (like \"lunar eclipse on 3 Frostmoon every year\"): " + val.substr(bar + 1));
            }
            continue;
        }
        if (key == "day 0 falls on") {
            const std::string l = lower(val);
            bool ok = false;
            for (int i = 0; i < (int)c.weekdays.size(); ++i) if (lower(c.weekdays[i]) == l) { c.firstWeekday = i; ok = true; }
            if (!ok) warn("Not a day of the week: " + val);
        } else if (key == "leap") {
            LeapRule r;
            const std::string l = lower(val);
            int every = 0, add = 0;
            const size_t to = l.rfind(" to ");
            if (std::sscanf(l.c_str(), "every %d years, %d", &every, &add) == 2 && to != std::string::npos) {
                r.every = every; r.add = add;
                std::string month = val.substr(to + 4);
                const size_t from = lower(month).find(", from year ");
                if (from != std::string::npos) { r.offset = std::atoi(month.c_str() + from + 12); month = month.substr(0, from); }
                r.month = monthNamed(month);
                if (r.month < 0) { warn("A leap rule names an unknown month: " + val); continue; }
                c.leap.push_back(r);
            } else warn("Not understood: Leap: " + val);
        } else if (key == "season") {
            const size_t from = lower(val).rfind(" from ");
            Season s;
            if (from == std::string::npos || !dayMonth(val.substr(from + 6), s.month, s.day)) { warn("Not understood: Season: " + val); continue; }
            s.name = trim(val.substr(0, from));
            c.seasons.push_back(s);
        } else if (key == "holiday") {
            std::string v = val, notes, symbol;
            // "| symbol: 🎆" at the end (0.88).
            if (const size_t sy = lower(v).rfind("| symbol:"); sy != std::string::npos) {
                symbol = trim(v.substr(sy + 9));
                v = trim(v.substr(0, sy));
            }
            const size_t bar = v.find('|');
            if (bar != std::string::npos) { notes = trim(v.substr(bar + 1)); v = trim(v.substr(0, bar)); }
            const size_t on = lower(v).rfind(" on ");
            Holiday h;
            std::string dm = on == std::string::npos ? std::string() : v.substr(on + 4);
            if (const size_t at = lower(dm).find(" at "); at != std::string::npos) dm = dm.substr(0, at);
            if (on == std::string::npos || !dayMonth(dm, h.month, h.day)) { warn("Not understood: Holiday: " + val); continue; }
            std::string when = v.substr(on + 4);
            {
                // "3 Frostmoon at dusk" (0.80)
                const size_t at = lower(when).find(" at ");
                if (at != std::string::npos) {
                    if (const auto tt = parseTimeOfDay(c, when.substr(at + 4))) h.time = *tt;
                    else warn("A holiday's time of day wasn't understood: " + val);
                }
            }
            h.name = trim(v.substr(0, on));
            h.notes = notes;
            h.symbol = symbol;
            c.holidays.push_back(h);
        }
    }
    std::stable_sort(c.eras.begin(), c.eras.end(), [](const Era& a, const Era& b) { return a.start < b.start; });
    if (!dayPartsGiven && !c.months.empty()) c.dayParts = defaultDayParts(c);
    if (c.months.empty()) warn("No months: list them under \"Months:\", one \"- Name: days\" a line.");
    return c;
}

// ------------------------------------------------------------------ dates on entries and timelines (0.75)

std::vector<DatedFact> datedFactsOf(const Calendar& c, const Entry& e) {
    std::vector<DatedFact> out;
    if (c.empty()) return out;
    std::istringstream in(e.details);
    std::string line;
    while (std::getline(in, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string label = trim(line.substr(0, colon)), value = trim(line.substr(colon + 1));
        if (label.empty() || value.empty()) continue;
        const auto dt = parseDateTime(c, value);
        if (!dt) continue;
        const auto d = std::optional<Date>(dt->date);
        DatedFact f;
        f.time = dt->time;
        const std::string l = lower(label);
        if (l == "born" || l == "birth" || l == "date of birth" || l == "birth date" || l == "hatched") f.kind = DatedFact::Birth;
        else if (l == "died" || l == "death" || l == "date of death" || l == "death date" || l == "killed") f.kind = DatedFact::Death;
        else if (e.kind == "event" && (l == "date" || l == "when" || l == "began" || l == "started" || l == "start")) f.kind = DatedFact::EventDate;
        f.entry = e.id;
        f.label = label;
        f.date = *d;
        f.day = toDay(c, *d);
        out.push_back(f);
    }
    return out;
}

std::vector<DatedFact> datedFacts(const Calendar& c, const std::vector<const Entry*>& entries) {
    std::vector<DatedFact> out;
    for (const Entry* e : entries) {
        if (!e || e->deleted) continue;
        auto f = datedFactsOf(c, *e);
        out.insert(out.end(), f.begin(), f.end());
    }
    std::stable_sort(out.begin(), out.end(), [](const DatedFact& a, const DatedFact& b) { return a.day < b.day; });
    return out;
}

StorySpan storySpan(const Library& lib, const Id& book) {
    StorySpan s;
    json j = json::parse(lib.setting("timeline:" + book), nullptr, false);
    if (!j.is_object()) return s;
    s.on = j.value("calendar", false);
    if (j.contains("start") && j["start"].is_number()) s.start = (std::int64_t)std::floor(j["start"].get<double>());
    if (!s.on) return s;
    auto widen = [&](double d) {
        const std::int64_t abs = s.start + (std::int64_t)std::floor(d);
        if (!s.any) { s.first = s.last = abs; s.any = true; }
        else { s.first = std::min(s.first, abs); s.last = std::max(s.last, abs); }
    };
    for (const char* k : {"nodes", "events"})
        if (j.contains(k) && j[k].is_object())
            for (auto& [id, v] : j[k].items())
                if (v.is_object() && v.contains("day") && v["day"].is_number()) widen(v["day"].get<double>());
    return s;
}

bool setTimelineCalendar(Library& lib, const Id& book, bool on, std::optional<std::int64_t> start) {
    json j = json::parse(lib.setting("timeline:" + book), nullptr, false);
    if (!j.is_object()) j = json::object();
    j["calendar"] = on;
    if (start) j["start"] = *start;
    return lib.setSetting("timeline:" + book, j.dump());
}

TimeOfDay eventTime(const Library& lib, const Id& book, const Id& entry) {
    json j = json::parse(lib.setting("timeline:" + book), nullptr, false);
    if (!j.is_object() || !j.contains("events") || !j["events"].is_object() || !j["events"].contains(entry) ||
        !j["events"][entry].is_object())
        return {};
    return timeFrom(j["events"][entry]);
}

bool setEventTime(Library& lib, const Id& book, const Id& entry, const TimeOfDay& t) {
    json j = json::parse(lib.setting("timeline:" + book), nullptr, false);
    if (!j.is_object() || !j.contains("events") || !j["events"].is_object() || !j["events"].contains(entry) ||
        !j["events"][entry].is_object())
        return false;   // only an event on the timeline has a time
    json& v = j["events"][entry];
    v.erase("time");
    v.erase("part");
    timeTo(v, t);
    return lib.setSetting("timeline:" + book, j.dump());
}

std::vector<AgeInBook> agesInBooks(const Library& lib, const Calendar& c, const Date& born, const std::vector<Id>& books,
                                   std::optional<Date> died) {
    std::vector<AgeInBook> out;
    const std::int64_t bornDay = toDay(c, born);
    const std::int64_t diedDay = died ? toDay(c, *died) : std::numeric_limits<std::int64_t>::max();
    for (const Id& b : books) {
        const StorySpan s = storySpan(lib, b);
        if (!s.on || !s.any) continue;
        if (s.last < bornDay || s.first > diedDay) continue;   // not alive during it
        const std::int64_t a = std::max(s.first, bornDay), z = std::min(s.last, diedDay);
        out.push_back({b, ageAt(c, born, fromDay(c, a)), ageAt(c, born, fromDay(c, z))});
    }
    return out;
}

// ------------------------------------------------------------------ where it's kept

std::string settingKeyForWorld(const Id& world) { return "calendar:world:" + world; }
std::string settingKeyForBook(const Id& book) { return "calendar:book:" + book; }

Calendar calendarOfWorld(const Library& lib, const Id& world) {
    if (world.empty()) return {};
    return fromJson(lib.setting(settingKeyForWorld(world)));
}
bool bookHasOwnCalendar(const Library& lib, const Id& book) { return !fromJson(lib.setting(settingKeyForBook(book))).empty(); }
Calendar calendarOfBook(const Library& lib, const Id& book) {
    Calendar own = fromJson(lib.setting(settingKeyForBook(book)));
    if (!own.empty()) return own;
    const Book* b = lib.book(book);
    return b ? calendarOfWorld(lib, b->worldId) : Calendar{};
}
bool saveWorldCalendar(Library& lib, const Id& world, const Calendar& c) {
    if (!lib.setSetting(settingKeyForWorld(world), c.empty() ? std::string() : toJson(c))) return false;
    // Plans imported before the calendar, or written for an older version of it, are read again (0.82).
    if (!c.empty())
        for (const Book& b : lib.books())
            if (!b.deleted && b.worldId == world && !bookHasOwnCalendar(lib, b.id)) rereadPlanDates(lib, b.id);
    return true;
}
bool saveBookCalendar(Library& lib, const Id& book, const Calendar& c) {
    if (!lib.setSetting(settingKeyForBook(book), c.empty() ? std::string() : toJson(c))) return false;
    rereadPlanDates(lib, book);
    return true;
}

int rereadPlanDates(Library& lib, const Id& book) {
    const Calendar c = calendarOfBook(lib, book);
    if (c.empty()) return 0;
    json tl = json::parse(lib.setting("timeline:" + book), nullptr, false);
    if (!tl.is_object() || !tl.contains("nodes") || !tl["nodes"].is_object()) return 0;
    int changed = 0;
    std::vector<std::pair<json*, std::int64_t>> read;   // each node read as a date, and its day
    for (auto& [id, v] : tl["nodes"].items()) {
        if (!v.is_object()) continue;
        std::string date = v.value("date", "");
        std::string rest;
        std::optional<Date> d;
        if (!date.empty()) d = parseDate(c, date);
        else if (!v.contains("day") && v.contains("when") && v["when"].is_string()) {
            // The longest start of the words (up to a ";" or "," or the end) that reads as a date, as on import.
            const std::string words = v["when"].get<std::string>();
            std::vector<size_t> cuts{words.size()};
            for (size_t k = 0; k < words.size(); ++k) if (words[k] == ';' || words[k] == ',') cuts.push_back(k);
            std::sort(cuts.rbegin(), cuts.rend());
            for (size_t cut : cuts)
                if ((d = parseDate(c, words.substr(0, cut)))) {
                    date = trim(words.substr(0, cut));
                    rest = trim(cut < words.size() ? words.substr(cut + 1) : std::string());
                    if (rest.empty()) v.erase("when");
                    else v["when"] = rest;
                    v["date"] = date;
                    break;
                }
        }
        if (!d) continue;
        read.push_back({&v, toDay(c, *d)});
    }
    // The timeline goes into calendar dates starting at the earliest of them (0.85: it took whichever came first in
    // the stored order, so the first chapter could land before day 0).
    if (!read.empty() && !tl.value("calendar", false)) {
        std::int64_t first = read.front().second;
        for (auto& r : read) first = std::min(first, r.second);
        tl["calendar"] = true;
        if (!tl.contains("start") || !tl["start"].is_number()) tl["start"] = first;
        tl["unit"] = "day";
    }
    for (auto& [vp, abs] : read) {
        json& v = *vp;
        const double day = (double)(abs - (std::int64_t)std::floor(tl["start"].get<double>()));
        if (!v.contains("day") || !v["day"].is_number() || v["day"].get<double>() != day) {
            v["day"] = day;
            ++changed;
        }
    }
    if (changed) lib.setSetting("timeline:" + book, tl.dump());
    return changed;
}

}  // namespace studio::cal

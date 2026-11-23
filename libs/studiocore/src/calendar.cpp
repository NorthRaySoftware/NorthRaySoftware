// A world's own calendar (0.74). See studio/calendar.hpp.

#include "studio/calendar.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    return c;
}

Calendar blank() {
    Calendar c;
    c.name = "New calendar";
    c.months = {{"First month", 30}};
    c.weekdays = {"First day", "Second day", "Third day", "Fourth day", "Fifth day", "Sixth day", "Seventh day"};
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

EraYear eraYear(const Calendar& c, std::int64_t year) {
    if (c.eras.empty()) return {year, "", "", false};
    const Era* e = nullptr;
    for (auto& x : c.eras) if (x.start <= year && (!e || x.start >= e->start)) e = &x;
    if (e) return {year - e->start + 1, e->abbr, e->name, false};
    std::int64_t first = c.eras.front().start;
    for (auto& x : c.eras) first = std::min(first, x.start);
    return {first - year, c.beforeAbbr, c.beforeName, true};
}

std::int64_t plainYear(const Calendar& c, std::int64_t year, const std::string& era) {
    const std::string l = lower(trim(era));
    if (l.empty()) return year;
    for (auto& x : c.eras)
        if (lower(x.abbr) == l || lower(x.name) == l) return x.start + year - 1;
    if (!c.eras.empty() && (lower(c.beforeAbbr) == l || lower(c.beforeName) == l)) {
        std::int64_t first = c.eras.front().start;
        for (auto& x : c.eras) first = std::min(first, x.start);
        return first - year;
    }
    return year;
}

std::string formatYear(const Calendar& c, std::int64_t year) {
    const EraYear e = eraYear(c, year);
    std::string s = std::to_string(e.year);
    const std::string tag = !e.abbr.empty() ? e.abbr : e.name;
    if (!tag.empty()) s += " " + tag;
    return s;
}

std::string formatDate(const Calendar& c, const Date& d0, bool withEra) {
    if (c.months.empty()) return {};
    const Date d = clampDate(c, d0);
    const std::string y = withEra ? formatYear(c, d.year) : std::to_string(eraYear(c, d.year).year);
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

std::optional<Date> parseDate(const Calendar& c, const std::string& s0) {
    if (c.months.empty()) return std::nullopt;
    // Split into words and numbers; keep a sign on a number written "-12".
    std::vector<std::string> tok;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } };
    const std::string s = trim(s0);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char ch = (unsigned char)s[i];
        if (std::isspace(ch) || ch == ',' || ch == '/' || ch == '.') { flush(); continue; }
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
    std::string era;
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
        for (auto& e : c.eras) if (lower(e.abbr) == l || lower(e.name) == l) isEra = true;
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

double moonPhase(const Calendar& c, const Moon& m, std::int64_t day, std::int64_t* anchorDay) {
    const MoonAnchor& a = m.anchor;
    if (a.repeat == MoonAnchor::None || c.months.empty() || m.cycle <= 0) return moonPhase(m, day);
    const Date d = fromDay(c, day);
    auto at = [&](std::int64_t year, int month) {
        return toDay(c, {year, month, std::clamp(a.day, 1, std::max(1, monthLength(c, year, month)))});
    };
    std::int64_t last = std::numeric_limits<std::int64_t>::min();
    if (a.repeat == MoonAnchor::Year) {
        const int month = std::clamp(a.month, 0, (int)c.months.size() - 1);
        last = at(d.year, month);
        if (last > day) last = at(d.year - 1, month);
    } else {
        // This month's anchor if it has come, else the last month before it that has one.
        std::int64_t y = d.year;
        int mo = d.month;
        for (int guard = 0; guard <= (int)c.months.size() * 2 + 1; ++guard) {
            if (std::find(a.except.begin(), a.except.end(), mo) == a.except.end()) {
                const std::int64_t t = at(y, mo);
                if (t <= day) { last = t; break; }
            }
            if (--mo < 0) { mo = (int)c.months.size() - 1; --y; }
        }
        if (last == std::numeric_limits<std::int64_t>::min()) return moonPhase(m, day);   // every month excepted
    }
    if (anchorDay) *anchorDay = last;
    double p = std::fmod(((double)(day - last) + a.days) / m.cycle, 1.0);
    if (p < 0) p += 1;
    return p;
}

namespace {
std::string phaseWords(const Moon& m) {
    const double c = m.cycle, d = m.anchor.days;
    auto near = [&](double x) { return std::fabs(d - x) < 1e-6; };
    if (near(0)) return "new";
    if (near(c / 2)) return "full";
    if (near(c / 4)) return "first quarter";
    if (near(c * 3 / 4)) return "last quarter";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%g days in", d);
    return buf;
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
    if (take("new moon") || take("new")) a.days = 0;
    else if (take("full moon") || take("full")) a.days = m.cycle / 2;
    else if (take("first quarter")) a.days = m.cycle / 4;
    else if (take("last quarter") || take("third quarter")) a.days = m.cycle * 3 / 4;
    else {
        char* end = nullptr;
        const double n = std::strtod(t.c_str(), &end);
        if (end == t.c_str()) return false;
        t = trim(std::string(end));
        if (!(take("days in") || take("day in") || take("days into its cycle") || take("days into the cycle"))) return false;
        a.days = n;
    }
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
    }
    for (auto& s : c.seasons)
        if (s.month < 0 || s.month >= (int)c.months.size() || s.day < 1 || s.day > c.months[s.month].days + 1)
            out.push_back("The season " + s.name + " starts on a day the calendar doesn't have.");
    for (auto& h : c.holidays)
        if (h.month < 0 || h.month >= (int)c.months.size() || h.day < 1 || h.day > c.months[h.month].days + 1)
            out.push_back("The holiday " + h.name + " falls on a day the calendar doesn't have.");
    if (c.hoursPerDay < 1 || c.minutesPerHour < 1) out.push_back("A day needs at least one hour of at least one minute.");
    return out;
}

// ------------------------------------------------------------------ JSON

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
    for (auto& e : c.eras) j["eras"].push_back({{"name", e.name}, {"abbr", e.abbr}, {"start", e.start}});
    j["before"] = {{"name", c.beforeName}, {"abbr", c.beforeAbbr}};
    j["moons"] = json::array();
    for (auto& m : c.moons) {
        json mj{{"name", m.name}, {"cycle", m.cycle}, {"offset", m.offset}};
        if (m.anchor.repeat != MoonAnchor::None)
            mj["anchor"] = {{"repeat", m.anchor.repeat == MoonAnchor::Year ? "year" : "month"}, {"month", m.anchor.month},
                            {"day", m.anchor.day}, {"days", m.anchor.days}, {"except", m.anchor.except}};
        j["moons"].push_back(mj);
    }
    j["seasons"] = json::array();
    for (auto& s : c.seasons) j["seasons"].push_back({{"name", s.name}, {"month", s.month}, {"day", s.day}});
    j["holidays"] = json::array();
    for (auto& h : c.holidays)
        j["holidays"].push_back({{"name", h.name}, {"month", h.month}, {"day", h.day}, {"entry", h.entry}, {"notes", h.notes}});
    j["hoursPerDay"] = c.hoursPerDay;
    j["minutesPerHour"] = c.minutesPerHour;
    j["order"] = c.order;
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
        for (auto& e : j["eras"]) if (e.is_object()) c.eras.push_back({str(e, "name"), str(e, "abbr"), (std::int64_t)num(e, "start", 1)});
    if (j.contains("before") && j["before"].is_object()) { c.beforeName = str(j["before"], "name"); c.beforeAbbr = str(j["before"], "abbr"); }
    if (j.contains("moons") && j["moons"].is_array())
        for (auto& m : j["moons"]) {
            if (!m.is_object()) continue;
            Moon mo{str(m, "name"), num(m, "cycle", 29.53), num(m, "offset", 0), {}};
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
            c.moons.push_back(mo);
        }
    if (j.contains("seasons") && j["seasons"].is_array())
        for (auto& s : j["seasons"]) if (s.is_object()) c.seasons.push_back({str(s, "name"), (int)num(s, "month", 0), (int)num(s, "day", 1)});
    if (j.contains("holidays") && j["holidays"].is_array())
        for (auto& h : j["holidays"])
            if (h.is_object()) c.holidays.push_back({str(h, "name"), (int)num(h, "month", 0), (int)num(h, "day", 1), str(h, "entry"), str(h, "notes")});
    c.hoursPerDay = std::max(1, (int)num(j, "hoursPerDay", 24));
    c.minutesPerHour = std::max(1, (int)num(j, "minutesPerHour", 60));
    c.order = str(j, "order");
    if (c.order != "mdy" && c.order != "ymd") c.order = "dmy";
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
    for (auto& e : c.eras) o << "Era: " << e.name << (e.abbr.empty() ? "" : " (" + e.abbr + ")") << " from year " << e.start << "\n";
    if (!c.beforeName.empty() || !c.beforeAbbr.empty())
        o << "Before the first era: " << c.beforeName << (c.beforeAbbr.empty() ? "" : " (" + c.beforeAbbr + ")") << "\n";
    for (auto& m : c.moons) {
        char buf[96];
        if (m.anchor.repeat != MoonAnchor::None) {
            std::snprintf(buf, sizeof buf, "%g days", m.cycle);
            o << "Moon: " << m.name << ", " << buf << ", " << anchorText(c, m) << "\n";
        } else {
            std::snprintf(buf, sizeof buf, "%g days, %g days into its cycle on day 0", m.cycle, m.offset);
            o << "Moon: " << m.name << ", " << buf << "\n";
        }
    }
    for (auto& s : c.seasons) o << "Season: " << s.name << " from " << dayMonth(s.month, s.day) << "\n";
    for (auto& h : c.holidays) o << "Holiday: " << h.name << " on " << dayMonth(h.month, h.day) << (h.notes.empty() ? "" : " | " + h.notes) << "\n";
    o << "Day: " << c.hoursPerDay << " hours of " << c.minutesPerHour << " minutes\n";
    o << "Dates: " << (c.order == "mdy" ? "month day year" : c.order == "ymd" ? "year month day" : "day month year") << "\n";
    return o.str();
}

Calendar fromText(const std::string& text, std::vector<std::string>* warnings) {
    Calendar c;
    auto warn = [&](const std::string& s) { if (warnings) warnings->push_back(s); };
    std::istringstream in(text);
    std::string line;
    bool inMonths = false;
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
            const std::string l = lower(val);
            c.order = l.rfind("month", 0) == 0 ? "mdy" : l.rfind("year", 0) == 0 ? "ymd" : "dmy";
        } else if (key == "day") {
            int h = 24, m = 60;
            if (std::sscanf(val.c_str(), "%d hours of %d minutes", &h, &m) >= 1) { c.hoursPerDay = std::max(1, h); c.minutesPerHour = std::max(1, m); }
            else warn("Not understood: " + t);
        } else if (key == "era") {
            Era e;
            std::string v = val;
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
            for (size_t k = 2; k < parts.size(); ++k) {
                const std::string pl = lower(parts[k]);
                if (pl.find(" on ") != std::string::npos && pl.find("day 0") == std::string::npos) {
                    // "except A, B and C" was split at the commas: join the rest back.
                    std::string anchor = parts[k];
                    for (size_t r = k + 1; r < parts.size(); ++r) anchor += ", " + parts[r];
                    later.push_back({"moon anchor", std::to_string(c.moons.size()) + "|" + anchor});
                    break;
                }
                m.offset = std::atof(parts[k].c_str());
            }
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
            std::string v = val, notes;
            const size_t bar = v.find('|');
            if (bar != std::string::npos) { notes = trim(v.substr(bar + 1)); v = trim(v.substr(0, bar)); }
            const size_t on = lower(v).rfind(" on ");
            Holiday h;
            if (on == std::string::npos || !dayMonth(v.substr(on + 4), h.month, h.day)) { warn("Not understood: Holiday: " + val); continue; }
            h.name = trim(v.substr(0, on));
            h.notes = notes;
            c.holidays.push_back(h);
        }
    }
    std::stable_sort(c.eras.begin(), c.eras.end(), [](const Era& a, const Era& b) { return a.start < b.start; });
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
        const auto d = parseDate(c, value);
        if (!d) continue;
        DatedFact f;
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
    return lib.setSetting(settingKeyForWorld(world), c.empty() ? std::string() : toJson(c));
}
bool saveBookCalendar(Library& lib, const Id& book, const Calendar& c) {
    return lib.setSetting(settingKeyForBook(book), c.empty() ? std::string() : toJson(c));
}

}  // namespace studio::cal

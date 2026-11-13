#include "cj/ical.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>

namespace cj::ical {

using radio::Programme;

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
}
bool endsWith(const std::string& s, const std::string& e) {
    return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

// --------------------------------------------------------------- lines

struct Line {
    std::string name;
    std::map<std::string, std::string> params;  // upper-case keys
    std::string value;
};

std::vector<std::string> unfold(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    while (i <= text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(i, e - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty() && (l[0] == ' ' || l[0] == '\t')) cur += l.substr(1);
        else {
            if (!cur.empty()) out.push_back(cur);
            cur = l;
        }
        i = e + 1;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::optional<Line> parseLine(const std::string& s) {
    Line l;
    bool q = false;
    size_t colon = std::string::npos;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') q = !q;
        else if (s[i] == ':' && !q) { colon = i; break; }
    }
    if (colon == std::string::npos) return std::nullopt;
    const std::string head = s.substr(0, colon);
    l.value = s.substr(colon + 1);
    size_t p = head.find(';');
    l.name = upper(head.substr(0, p));
    while (p != std::string::npos) {
        size_t n = p + 1;
        q = false;
        size_t end = n;
        for (; end < head.size(); ++end) {
            if (head[end] == '"') q = !q;
            else if (head[end] == ';' && !q) break;
        }
        const std::string kv = head.substr(n, end - n);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string v = kv.substr(eq + 1);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            l.params[upper(kv.substr(0, eq))] = v;
        }
        p = end < head.size() ? end : std::string::npos;
    }
    return l;
}

std::string unescapeText(const std::string& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            const char n = v[++i];
            o += (n == 'n' || n == 'N') ? '\n' : n;
        } else {
            o += v[i];
        }
    }
    return o;
}

// Google Calendar descriptions can contain simple HTML.
std::string stripHtml(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') {
            const size_t e = s.find('>', i);
            if (e == std::string::npos) { o += s.substr(i); break; }
            const std::string tag = lower(s.substr(i + 1, e - i - 1));
            if (tag.rfind("br", 0) == 0 || tag == "/p" || tag == "/div" || tag == "/li") o += '\n';
            i = e;
        } else if (s.compare(i, 5, "&amp;") == 0) { o += '&'; i += 4; }
        else if (s.compare(i, 4, "&lt;") == 0) { o += '<'; i += 3; }
        else if (s.compare(i, 4, "&gt;") == 0) { o += '>'; i += 3; }
        else if (s.compare(i, 6, "&nbsp;") == 0) { o += ' '; i += 5; }
        else if (s.compare(i, 6, "&quot;") == 0) { o += '"'; i += 5; }
        else if (s.compare(i, 5, "&#39;") == 0) { o += '\''; i += 4; }
        else o += s[i];
    }
    return o;
}

// --------------------------------------------------------------- times

enum class Zone { Utc, Uk, Unknown };

struct Stamp {
    CivilDateTime c;
    Zone zone = Zone::Uk;
    bool dateOnly = false;
    bool ok = false;
    std::string tzid;
};

Zone zoneFor(std::string tz) {
    if (tz.empty()) return Zone::Uk;  // floating: the station's own time
    while (!tz.empty() && tz.front() == '/') tz.erase(0, 1);
    static const char* uk[] = {"europe/london", "gb", "gb-eire", "europe/belfast", "europe/guernsey", "europe/jersey",
                               "europe/isle_of_man", "gmt standard time", "(utc+00:00) dublin, edinburgh, lisbon, london"};
    static const char* utc[] = {"utc", "etc/utc", "etc/gmt", "etc/zulu", "zulu", "z", "coordinated universal time",
                                "greenwich standard time", "etc/universal", "universal"};
    const std::string l = lower(tz);
    for (const char* u : uk) if (l == u || endsWith(l, std::string("/") + u)) return Zone::Uk;
    for (const char* u : utc) if (l == u || endsWith(l, std::string("/") + u)) return Zone::Utc;
    return Zone::Unknown;
}

bool digits(const std::string& s, size_t at, size_t n, int& out) {
    if (at + n > s.size()) return false;
    int v = 0;
    for (size_t i = at; i < at + n; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        v = v * 10 + (s[i] - '0');
    }
    out = v;
    return true;
}

Stamp parseStamp(const std::string& raw, const std::string& tzid) {
    Stamp st;
    const std::string v = trim(raw);
    st.tzid = tzid;
    if (!digits(v, 0, 4, st.c.year) || !digits(v, 4, 2, st.c.month) || !digits(v, 6, 2, st.c.day)) return st;
    if (st.c.month < 1 || st.c.month > 12 || st.c.day < 1 || st.c.day > 31) return st;
    if (v.size() == 8) {
        st.dateOnly = true;
        st.zone = Zone::Uk;
        st.ok = true;
        return st;
    }
    if (v.size() < 15 || v[8] != 'T') return st;
    if (!digits(v, 9, 2, st.c.hour) || !digits(v, 11, 2, st.c.minute) || !digits(v, 13, 2, st.c.second)) return st;
    if (st.c.hour > 23 || st.c.minute > 59 || st.c.second > 60) return st;
    if (v.size() == 16 && (v[15] == 'Z' || v[15] == 'z')) st.zone = Zone::Utc;
    else if (v.size() == 15) st.zone = zoneFor(tzid);
    else return st;
    st.ok = true;
    return st;
}

std::optional<UtcSeconds> toUtc(const CivilDateTime& c, Zone z) {
    if (z == Zone::Utc) return utcFromCivil(c);
    if (z == Zone::Uk) return ukToUtc(c);
    return std::nullopt;
}

// "PT2H", "PT1H30M", "P1D", "-PT5M"
std::optional<long long> parseDuration(const std::string& raw) {
    std::string v = upper(trim(raw));
    long long sign = 1;
    if (!v.empty() && (v[0] == '+' || v[0] == '-')) { if (v[0] == '-') sign = -1; v.erase(0, 1); }
    if (v.empty() || v[0] != 'P') return std::nullopt;
    long long total = 0, num = 0;
    bool time = false, any = false, haveNum = false;
    for (size_t i = 1; i < v.size(); ++i) {
        const char c = v[i];
        if (std::isdigit(static_cast<unsigned char>(c))) { num = num * 10 + (c - '0'); haveNum = true; continue; }
        if (c == 'T') { time = true; continue; }
        if (!haveNum) return std::nullopt;
        long long mult = 0;
        if (c == 'W' && !time) mult = 7 * 86400;
        else if (c == 'D' && !time) mult = 86400;
        else if (c == 'H' && time) mult = 3600;
        else if (c == 'M' && time) mult = 60;
        else if (c == 'S' && time) mult = 1;
        else return std::nullopt;
        total += num * mult;
        num = 0;
        haveNum = false;
        any = true;
    }
    if (!any || haveNum) return std::nullopt;
    return sign * total;
}

int daysInMonth(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

CivilDateTime addDays(CivilDateTime c, long long days) {
    const CivilDateTime d = civilFromUtc(utcFromCivil({c.year, c.month, c.day, 0, 0, 0}) + days * 86400);
    c.year = d.year;
    c.month = d.month;
    c.day = d.day;
    return c;
}

long long dayNumber(const CivilDateTime& c) { return daysFromCivil(c.year, c.month, c.day); }

// --------------------------------------------------------------- events

struct Event {
    std::string uid, summary, description, status, cls;
    Stamp start, end;
    bool hasEnd = false;
    std::optional<long long> duration;
    std::string rrule;
    std::vector<Stamp> exdates;
    Stamp recurrenceId;
    bool hasRecurrenceId = false;
};

struct Rule {
    std::string freq;
    int interval = 1;
    int count = -1;
    bool hasUntil = false;
    Stamp until;
    std::vector<std::pair<int, int>> byDay;  // (ordinal, weekday 0=Sun); ordinal 0 = every
    std::vector<int> byMonthDay;
    std::string unsupported;
};

int weekdayCode(const std::string& s) {
    static const char* names[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    for (int i = 0; i < 7; ++i) if (s == names[i]) return i;
    return -1;
}

Rule parseRule(const std::string& text) {
    Rule r;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find(';', i);
        if (e == std::string::npos) e = text.size();
        const std::string part = text.substr(i, e - i);
        i = e + 1;
        const size_t eq = part.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = upper(part.substr(0, eq)), v = upper(part.substr(eq + 1));
        if (k == "FREQ") r.freq = v;
        else if (k == "INTERVAL") r.interval = std::max(1, std::atoi(v.c_str()));
        else if (k == "COUNT") r.count = std::max(0, std::atoi(v.c_str()));
        else if (k == "UNTIL") { r.until = parseStamp(v, ""); r.hasUntil = r.until.ok; }
        else if (k == "BYDAY") {
            size_t j = 0;
            while (j < v.size()) {
                size_t f = v.find(',', j);
                if (f == std::string::npos) f = v.size();
                const std::string d = v.substr(j, f - j);
                j = f + 1;
                if (d.size() < 2) continue;
                const int wd = weekdayCode(d.substr(d.size() - 2));
                if (wd < 0) { r.unsupported = "BYDAY=" + d; continue; }
                const std::string ord = d.substr(0, d.size() - 2);
                r.byDay.push_back({ord.empty() ? 0 : std::atoi(ord.c_str()), wd});
            }
        } else if (k == "BYMONTHDAY") {
            size_t j = 0;
            while (j < v.size()) {
                size_t f = v.find(',', j);
                if (f == std::string::npos) f = v.size();
                r.byMonthDay.push_back(std::atoi(v.substr(j, f - j).c_str()));
                j = f + 1;
            }
        } else if (k == "WKST") {
            // Only matters for INTERVAL>1 weekly rules with several days; Monday assumed.
        } else {
            r.unsupported = k;
        }
    }
    if (r.freq != "DAILY" && r.freq != "WEEKLY" && r.freq != "MONTHLY" && r.freq != "YEARLY" && r.unsupported.empty())
        r.unsupported = "FREQ=" + r.freq;
    return r;
}

// Every candidate date of the rule, in order, starting from the event's own
// date. Calls `emit(date)` until it returns false.
template <typename F>
void expandDates(const Rule& r, const CivilDateTime& start, F emit) {
    const int kMaxSteps = 20000;
    if (r.freq == "DAILY") {
        for (int k = 0; k < kMaxSteps; ++k)
            if (!emit(addDays(start, static_cast<long long>(k) * r.interval))) return;
    } else if (r.freq == "WEEKLY") {
        std::vector<int> days;
        for (auto& [ord, wd] : r.byDay) days.push_back(wd);
        if (days.empty()) days.push_back(weekday(start.year, start.month, start.day));
        // Offsets from Monday (Mon=0 .. Sun=6).
        std::vector<int> offs;
        for (int wd : days) offs.push_back((wd + 6) % 7);
        std::sort(offs.begin(), offs.end());
        offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
        const int startOff = (weekday(start.year, start.month, start.day) + 6) % 7;
        const CivilDateTime monday = addDays(start, -startOff);
        for (int k = 0; k < kMaxSteps; ++k) {
            for (int o : offs) {
                const CivilDateTime d = addDays(monday, static_cast<long long>(k) * 7 * r.interval + o);
                if (dayNumber(d) < dayNumber(start)) continue;
                if (!emit(d)) return;
            }
        }
    } else if (r.freq == "MONTHLY") {
        for (int k = 0; k < kMaxSteps / 10; ++k) {
            const int mIndex = start.month - 1 + k * r.interval;
            CivilDateTime base = start;
            base.year = start.year + mIndex / 12;
            base.month = mIndex % 12 + 1;
            const int dim = daysInMonth(base.year, base.month);
            std::vector<int> dom;
            if (!r.byDay.empty()) {
                for (auto& [ord, wd] : r.byDay) {
                    std::vector<int> matches;
                    for (int d = 1; d <= dim; ++d) if (weekday(base.year, base.month, d) == wd) matches.push_back(d);
                    if (ord == 0) dom.insert(dom.end(), matches.begin(), matches.end());
                    else if (ord > 0 && ord <= static_cast<int>(matches.size())) dom.push_back(matches[ord - 1]);
                    else if (ord < 0 && -ord <= static_cast<int>(matches.size())) dom.push_back(matches[matches.size() + ord]);
                }
            } else if (!r.byMonthDay.empty()) {
                for (int d : r.byMonthDay) {
                    const int day = d > 0 ? d : dim + 1 + d;
                    if (day >= 1 && day <= dim) dom.push_back(day);
                }
            } else if (start.day <= dim) {
                dom.push_back(start.day);
            }
            std::sort(dom.begin(), dom.end());
            dom.erase(std::unique(dom.begin(), dom.end()), dom.end());
            for (int d : dom) {
                CivilDateTime c = base;
                c.day = d;
                if (dayNumber(c) < dayNumber(start)) continue;
                if (!emit(c)) return;
            }
        }
    } else if (r.freq == "YEARLY") {
        for (int k = 0; k < 500; ++k) {
            CivilDateTime c = start;
            c.year = start.year + k * r.interval;
            if (c.day > daysInMonth(c.year, c.month)) continue;  // 29 February
            if (!emit(c)) return;
        }
    }
}

std::string describeEvent(const Event& e) {
    return "'" + (e.summary.empty() ? e.uid : e.summary) + "'";
}

}  // namespace

std::string normaliseUrl(const std::string& raw) {
    std::string u = trim(raw);
    const std::string l = lower(u);
    if (l.rfind("webcal://", 0) == 0) u = "https://" + u.substr(9);
    else if (l.rfind("webcals://", 0) == 0) u = "https://" + u.substr(10);
    if (lower(u).rfind("https://", 0) != 0 || u.size() <= 8) return {};
    for (char c : u) if (static_cast<unsigned char>(c) <= ' ') return {};
    return u;
}

Result toProgrammes(const std::string& text, UtcSeconds from, UtcSeconds to, const std::vector<radio::Series>& series) {
    Result res;
    const std::vector<std::string> lines = unfold(text);
    std::vector<Event> events;
    std::vector<std::string> stack;
    Event cur;
    for (const std::string& raw : lines) {
        auto ol = parseLine(raw);
        if (!ol) continue;
        Line& l = *ol;
        if (l.name == "BEGIN") {
            const std::string what = upper(trim(l.value));
            if (what == "VCALENDAR") res.valid = true;
            stack.push_back(what);
            if (what == "VEVENT" && stack.size() >= 1) cur = Event{};
            continue;
        }
        if (l.name == "END") {
            const std::string what = upper(trim(l.value));
            if (!stack.empty() && stack.back() == what) stack.pop_back();
            if (what == "VEVENT") events.push_back(cur);
            continue;
        }
        if (stack.size() == 1 && stack[0] == "VCALENDAR" && l.name == "X-WR-CALNAME") res.calendarName = unescapeText(l.value);
        if (stack.empty() || stack.back() != "VEVENT") continue;  // ignore alarms, time zones

        auto tz = [&] { auto it = l.params.find("TZID"); return it == l.params.end() ? std::string{} : it->second; };
        if (l.name == "UID") cur.uid = trim(l.value);
        else if (l.name == "SUMMARY") cur.summary = trim(unescapeText(l.value));
        else if (l.name == "DESCRIPTION") cur.description = unescapeText(l.value);
        else if (l.name == "STATUS") cur.status = upper(trim(l.value));
        else if (l.name == "CLASS") cur.cls = upper(trim(l.value));
        else if (l.name == "DTSTART") cur.start = parseStamp(l.value, tz());
        else if (l.name == "DTEND") { cur.end = parseStamp(l.value, tz()); cur.hasEnd = cur.end.ok; }
        else if (l.name == "DURATION") cur.duration = parseDuration(l.value);
        else if (l.name == "RRULE") cur.rrule = l.value;
        else if (l.name == "RECURRENCE-ID") { cur.recurrenceId = parseStamp(l.value, tz()); cur.hasRecurrenceId = cur.recurrenceId.ok; }
        else if (l.name == "EXDATE") {
            size_t i = 0;
            while (i <= l.value.size()) {
                size_t e = l.value.find(',', i);
                if (e == std::string::npos) e = l.value.size();
                Stamp s = parseStamp(l.value.substr(i, e - i), tz());
                if (s.ok) cur.exdates.push_back(s);
                i = e + 1;
            }
        }
    }
    if (!res.valid) {
        res.warnings.push_back("The calendar link didn't return a calendar (.ics) file.");
        return res;
    }
    res.events = static_cast<int>(events.size());

    // Overrides of single occurrences: uid -> set of original start instants.
    std::map<std::string, std::set<UtcSeconds>> overridden;
    for (const Event& e : events) {
        if (!e.hasRecurrenceId) continue;
        if (auto u = toUtc(e.recurrenceId.c, e.recurrenceId.zone)) overridden[e.uid].insert(*u);
        else if (e.recurrenceId.dateOnly) overridden[e.uid].insert(utcFromCivil(e.recurrenceId.c));
    }

    auto makeProgramme = [&](const Event& e, UtcSeconds start, long long durSec) {
        Programme p;
        p.title = e.summary.empty() ? "Untitled programme" : e.summary;
        // CJ Radio is CJ Philip's own station: every programme is his. Guests are
        // just named in the description ("Guest: Name"), which is shown as written.
        p.description = trim(stripHtml(e.description));
        const std::string lt = lower(p.title);
        for (const radio::Series& s : series) {
            if (!s.title.empty() && lt.find(lower(s.title)) != std::string::npos) {
                p.seriesId = s.id;
                p.presenter = s.presenter;
                p.category = s.category;
                break;
            }
        }
        if (p.presenter.empty()) p.presenter = series.empty() || series[0].presenter.empty() ? "CJ Philip" : series[0].presenter;
        const CivilDateTime uk = utcToUk(start);
        p.kind = (!p.seriesId.empty() && weekday(uk.year, uk.month, uk.day) == 6) ? radio::Kind::RegularSaturday
                                                                                 : radio::Kind::Additional;
        p.status = e.status == "CANCELLED" ? radio::Status::Cancelled
                 : e.status == "TENTATIVE" ? radio::Status::Draft
                                           : radio::Status::Confirmed;
        if (p.status == radio::Status::Cancelled) p.note = "Cancelled";
        p.startUtc = start;
        p.durationMinutes = static_cast<int>(std::clamp<long long>((durSec + 59) / 60, 1, 24 * 60));
        char buf[24];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d", uk.year, uk.month, uk.day, uk.hour, uk.minute);
        p.authoredUkTime = buf;
        p.id = "cal:" + (e.uid.empty() ? p.title : e.uid) + "@" + formatUtcIso(start);
        return p;
    };

    int skippedAllDay = 0;
    for (const Event& e : events) {
        if (!e.start.ok) { res.warnings.push_back("Event " + describeEvent(e) + " has no readable start time; skipped."); continue; }
        if (e.cls == "PRIVATE" || e.cls == "CONFIDENTIAL") continue;
        if (e.start.dateOnly) { ++skippedAllDay; continue; }
        if (e.start.zone == Zone::Unknown) {
            res.warnings.push_back("Event " + describeEvent(e) + " uses the time zone '" + e.start.tzid +
                                   "', which CJ Radio can't convert; set the calendar to UK time. Skipped.");
            continue;
        }
        const auto start0 = toUtc(e.start.c, e.start.zone);
        if (!start0) { res.warnings.push_back("Event " + describeEvent(e) + " starts in the hour the clocks go forward; skipped."); continue; }
        long long dur = 3600;
        if (e.hasEnd && !e.end.dateOnly && e.end.zone != Zone::Unknown) {
            if (auto en = toUtc(e.end.c, e.end.zone)) dur = *en - *start0;
        } else if (e.duration) {
            dur = *e.duration;
        }
        if (dur <= 0) dur = 3600;

        auto add = [&](UtcSeconds s) {
            if (s + dur > from && s < to) res.programmes.push_back(makeProgramme(e, s, dur));
        };

        if (e.hasRecurrenceId || e.rrule.empty()) { add(*start0); continue; }

        const Rule rule = parseRule(e.rrule);
        if (!rule.unsupported.empty()) {
            res.warnings.push_back("Event " + describeEvent(e) + " repeats in a way CJ Radio doesn't understand (" +
                                   rule.unsupported + "); only its first date is listed.");
            add(*start0);
            continue;
        }
        std::set<UtcSeconds> ex;
        std::set<long long> exDays;
        for (const Stamp& s : e.exdates) {
            if (s.dateOnly) exDays.insert(dayNumber(s.c));
            else if (auto u = toUtc(s.c, s.zone == Zone::Unknown ? e.start.zone : s.zone)) ex.insert(*u);
        }
        const auto ov = overridden.find(e.uid);
        int produced = 0;
        expandDates(rule, e.start.c, [&](const CivilDateTime& d) {
            CivilDateTime c = d;
            c.hour = e.start.c.hour;
            c.minute = e.start.c.minute;
            c.second = e.start.c.second;
            if (rule.count >= 0 && produced >= rule.count) return false;
            if (rule.hasUntil) {
                if (rule.until.dateOnly) { if (dayNumber(c) > dayNumber(rule.until.c)) return false; }
                else if (auto u = toUtc(rule.until.c, rule.until.zone == Zone::Unknown ? Zone::Utc : rule.until.zone)) {
                    auto s = toUtc(c, e.start.zone);
                    if (s && *s > *u) return false;
                }
            }
            ++produced;
            const auto s = toUtc(c, e.start.zone);
            if (!s) return true;   // inside the spring-forward gap
            if (*s >= to) return false;
            if (ex.count(*s) || exDays.count(dayNumber(c))) return true;
            if (ov != overridden.end() && ov->second.count(*s)) return true;
            add(*s);
            return true;
        });
    }
    if (skippedAllDay)
        res.warnings.push_back(std::to_string(skippedAllDay) + " all-day event(s) have no start time, so they aren't listed.");
    std::sort(res.programmes.begin(), res.programmes.end(),
              [](const Programme& a, const Programme& b) { return a.startUtc < b.startUtc; });
    return res;
}

}  // namespace cj::ical

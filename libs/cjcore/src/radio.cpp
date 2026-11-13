#include "cj/radio.hpp"

#include <algorithm>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace cj::radio {

using nlohmann::json;

const char* kindLabel(Kind k) {
    switch (k) {
        case Kind::RegularSaturday: return "Saturday programme";
        case Kind::Additional: return "Additional programme";
        case Kind::Spontaneous: return "Unscheduled broadcast";
    }
    return "";
}

const char* statusLabel(Status s) {
    switch (s) {
        case Status::Draft: return "Draft";
        case Status::Confirmed: return "Confirmed";
        case Status::Cancelled: return "Cancelled";
        case Status::Postponed: return "Postponed";
    }
    return "";
}

const char* stateLabel(StationState s) {
    switch (s) {
        case StationState::Live: return "Live";
        case StationState::StartingSoon: return "Starting soon";
        case StationState::Upcoming: return "Off air";  // the station itself is off air
        case StationState::OffAir: return "Off air";
    }
    return "";
}

static bool byStart(const Programme& a, const Programme& b) { return a.startUtc < b.startUtc; }

StationStatus computeStatus(const Schedule& schedule, const LiveSignal& signal, UtcSeconds now) {
    StationStatus st;
    st.infrastructureConnected = signal.connected;

    std::vector<Programme> sorted = schedule.programmes;
    std::sort(sorted.begin(), sorted.end(), byStart);

    for (const Programme& p : sorted) {
        if (p.status == Status::Confirmed && p.startUtc > now && !st.next) st.next = p;
        if (p.status == Status::Confirmed && p.startUtc <= now && now < p.endUtc()) st.overdue = p;
        if ((p.status == Status::Cancelled || p.status == Status::Postponed) && p.endUtc() > now)
            st.recentChanges.push_back(p);
    }

    if (signal.connected && signal.onAir) {
        st.state = StationState::Live;
        st.overdue.reset();
        for (const Programme& p : sorted)
            if (!signal.programmeId.empty() && p.id == signal.programmeId) st.current = p;
        if (!st.current) {
            // Spontaneous broadcast, or one that started without a listing.
            Programme p;
            p.id = signal.programmeId;
            p.title = signal.title.empty() ? "Unscheduled broadcast" : signal.title;
            p.kind = Kind::Spontaneous;
            p.status = Status::Confirmed;
            p.startUtc = signal.startedUtc;
            st.current = p;
        }
        return st;
    }

    if (st.next && st.next->startUtc - now <= kStartingSoonMinutes * 60)
        st.state = StationState::StartingSoon;
    else if (st.next)
        st.state = StationState::Upcoming;
    else
        st.state = StationState::OffAir;
    return st;
}

std::vector<Programme> publicProgrammes(const Schedule& schedule, UtcSeconds from, UtcSeconds to) {
    std::vector<Programme> out;
    for (const Programme& p : schedule.programmes)
        if (p.isPublic() && p.endUtc() > from && p.startUtc < to) out.push_back(p);
    std::sort(out.begin(), out.end(), byStart);
    return out;
}

std::vector<Programme> draftSaturdaySuggestions(const Schedule& schedule, const Series& series,
                                                UtcSeconds from, UtcSeconds to) {
    std::vector<Programme> out;
    // Walk UK calendar days.
    CivilDateTime d = utcToUk(from);
    d.hour = d.minute = d.second = 0;
    for (int i = 0; i < 400; ++i) {
        const UtcSeconds dayStart = utcFromCivil(d);
        if (dayStart - 86400 > to) break;
        if (weekday(d.year, d.month, d.day) == 6) {
            CivilDateTime at = d;
            at.hour = series.defaultUkHour;
            at.minute = series.defaultUkMinute;
            auto start = ukToUtc(at);
            if (start && *start >= from && *start < to) {
                bool taken = false;
                for (const Programme& p : schedule.programmes) {
                    if (!p.isPublic()) continue;
                    const CivilDateTime pd = utcToUk(p.startUtc);
                    if (pd.year == d.year && pd.month == d.month && pd.day == d.day) taken = true;
                }
                if (!taken) {
                    Programme p;
                    p.id = series.id + "-draft-" + std::to_string(d.year * 10000 + d.month * 100 + d.day);
                    p.title = series.title;
                    p.presenter = series.presenter;
                    p.category = series.category;
                    p.kind = Kind::RegularSaturday;
                    p.status = Status::Draft;
                    p.startUtc = *start;
                    p.durationMinutes = series.defaultDurationMinutes;
                    p.seriesId = series.id;
                    out.push_back(p);
                }
            }
        }
        d = civilFromUtc(dayStart + 86400);
    }
    return out;
}

static Kind parseKind(const std::string& s) {
    if (s == "regular-saturday") return Kind::RegularSaturday;
    if (s == "spontaneous") return Kind::Spontaneous;
    return Kind::Additional;
}

static Status parseStatus(const std::string& s) {
    if (s == "confirmed") return Status::Confirmed;
    if (s == "cancelled") return Status::Cancelled;
    if (s == "postponed") return Status::Postponed;
    return Status::Draft;
}

// Demonstration files can't use fixed dates or they go stale. They may give
// {"relative": {"weekday": 6, "weeksAhead": 0, "ukTime": "19:00"}} instead,
// meaning "the next such weekday on or after today (UK), plus N weeks".
static std::optional<CivilDateTime> resolveRelative(const json& rel, UtcSeconds now) {
    const int targetWd = rel.value("weekday", 6);
    const int weeks = rel.value("weeksAhead", 0);
    const std::string time = rel.value("ukTime", "19:00");
    int hh = 0, mm = 0;
    if (std::sscanf(time.c_str(), "%d:%d", &hh, &mm) != 2) return std::nullopt;
    CivilDateTime today = utcToUk(now);
    const int wd = weekday(today.year, today.month, today.day);
    const int ahead = ((targetWd - wd) + 7) % 7 + weeks * 7;
    CivilDateTime d = civilFromUtc(utcFromCivil({today.year, today.month, today.day, 0, 0, 0}) + ahead * 86400);
    d.hour = hh;
    d.minute = mm;
    return d;
}

Schedule loadSchedule(const std::string& text, UtcSeconds now) {
    Schedule s;
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        s.warnings.push_back("Schedule file isn't valid JSON.");
        return s;
    }
    const bool demoFile = doc.value("demo", false);

    for (const json& j : doc.value("series", json::array())) {
        Series se;
        se.id = j.value("id", "");
        se.title = j.value("title", "");
        se.presenter = j.value("presenter", "");
        se.category = j.value("category", "");
        std::sscanf(j.value("defaultUkTime", "19:00").c_str(), "%d:%d", &se.defaultUkHour, &se.defaultUkMinute);
        se.defaultDurationMinutes = j.value("defaultDurationMinutes", 120);
        s.series.push_back(se);
    }

    for (const json& j : doc.value("programmes", json::array())) {
        Programme p;
        p.id = j.value("id", "");
        p.title = j.value("title", "");
        p.description = j.value("description", "");
        p.presenter = j.value("presenter", "");
        p.category = j.value("category", "");
        p.kind = parseKind(j.value("kind", "additional"));
        p.status = parseStatus(j.value("status", "draft"));
        p.durationMinutes = j.value("durationMinutes", 60);
        p.seriesId = j.value("series", "");
        p.movedTo = j.value("movedTo", "");
        p.note = j.value("note", "");
        p.demo = demoFile || j.value("demo", false);

        std::optional<CivilDateTime> uk;
        if (j.contains("startUk")) uk = parseLocalIso(j["startUk"].get<std::string>());
        else if (j.contains("relative")) uk = resolveRelative(j["relative"], now);

        if (!uk) {
            s.warnings.push_back("Programme '" + p.id + "' has no valid UK start time; skipped.");
            continue;
        }
        auto utc = ukToUtc(*uk);
        if (!utc) {
            s.warnings.push_back("Programme '" + p.id + "' starts during the hour the clocks go forward; skipped.");
            continue;
        }
        p.startUtc = *utc;
        char buf[24];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d", uk->year, uk->month, uk->day, uk->hour, uk->minute);
        p.authoredUkTime = buf;

        if (p.kind == Kind::RegularSaturday && weekday(uk->year, uk->month, uk->day) != 6)
            s.warnings.push_back("Programme '" + p.id + "' is marked as a Saturday programme but isn't on a Saturday.");
        if (p.durationMinutes <= 0 || p.durationMinutes > 24 * 60) {
            s.warnings.push_back("Programme '" + p.id + "' has an unrealistic duration; using 60 minutes.");
            p.durationMinutes = 60;
        }
        s.programmes.push_back(p);
    }
    std::sort(s.programmes.begin(), s.programmes.end(), byStart);
    return s;
}

}  // namespace cj::radio

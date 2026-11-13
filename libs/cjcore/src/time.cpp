#include "cj/time.hpp"

#include <cstdio>

namespace cj {

// Howard Hinnant's civil-date algorithms (public domain).
std::int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

static void civilFromDays(std::int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yy + (m <= 2));
}

int weekday(int y, int m, int d) {
    const std::int64_t z = daysFromCivil(y, m, d);
    return static_cast<int>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

CivilDateTime civilFromUtc(UtcSeconds t) {
    std::int64_t days = t / 86400;
    std::int64_t rem = t % 86400;
    if (rem < 0) { rem += 86400; --days; }
    CivilDateTime c;
    civilFromDays(days, c.year, c.month, c.day);
    c.hour = static_cast<int>(rem / 3600);
    c.minute = static_cast<int>((rem % 3600) / 60);
    c.second = static_cast<int>(rem % 60);
    return c;
}

UtcSeconds utcFromCivil(const CivilDateTime& c) {
    return daysFromCivil(c.year, c.month, c.day) * 86400 + c.hour * 3600 + c.minute * 60 + c.second;
}

static int lastSundayOf(int year, int month) {
    const int daysIn = (month == 3 || month == 10) ? 31 : 30;
    const int wd = weekday(year, month, daysIn);
    return daysIn - wd;
}

bool isBritishSummerTime(UtcSeconds t) {
    const CivilDateTime c = civilFromUtc(t);
    const UtcSeconds start = utcFromCivil({c.year, 3, lastSundayOf(c.year, 3), 1, 0, 0});
    const UtcSeconds end = utcFromCivil({c.year, 10, lastSundayOf(c.year, 10), 1, 0, 0});
    return t >= start && t < end;
}

int ukOffsetSeconds(UtcSeconds t) { return isBritishSummerTime(t) ? 3600 : 0; }

CivilDateTime utcToUk(UtcSeconds t) { return civilFromUtc(t + ukOffsetSeconds(t)); }

std::optional<UtcSeconds> ukToUtc(const CivilDateTime& uk) {
    const UtcSeconds asUtc = utcFromCivil(uk);
    // Try the BST interpretation first, then GMT.
    for (int offset : {3600, 0}) {
        const UtcSeconds candidate = asUtc - offset;
        if (ukOffsetSeconds(candidate) == offset) return candidate;
    }
    return std::nullopt;  // falls in the spring-forward gap
}

std::optional<CivilDateTime> parseLocalIso(const std::string& s) {
    CivilDateTime c;
    int n = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d%n", &c.year, &c.month, &c.day, &c.hour, &c.minute, &n) != 5)
        return std::nullopt;
    if (static_cast<size_t>(n) < s.size()) {
        int sec = 0, n2 = 0;
        if (std::sscanf(s.c_str() + n, ":%2d%n", &sec, &n2) != 1) return std::nullopt;
        c.second = sec;
        n += n2;
    }
    if (static_cast<size_t>(n) != s.size()) return std::nullopt;
    if (c.month < 1 || c.month > 12 || c.day < 1 || c.day > 31 || c.hour > 23 || c.minute > 59 || c.second > 59)
        return std::nullopt;
    // Reject dates like 31 November by round-tripping.
    const CivilDateTime back = civilFromUtc(utcFromCivil(c));
    if (back.day != c.day || back.month != c.month) return std::nullopt;
    return c;
}

std::optional<UtcSeconds> parseUtcIso(const std::string& s) {
    if (s.empty() || s.back() != 'Z') return std::nullopt;
    auto c = parseLocalIso(s.substr(0, s.size() - 1));
    if (!c) return std::nullopt;
    return utcFromCivil(*c);
}

std::string formatUtcIso(UtcSeconds t) {
    const CivilDateTime c = civilFromUtc(t);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", c.year, c.month, c.day, c.hour, c.minute, c.second);
    return buf;
}

}  // namespace cj

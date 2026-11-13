// UK civil time helpers.
//
// The host schedules programmes in UK time (Europe/London). We store the
// authored UK wall-clock time *and* resolve it to UTC, so listeners anywhere
// can be shown their own local time. The rules here cover the current UK
// definition of British Summer Time: clocks go forward at 01:00 UTC on the
// last Sunday of March and back at 01:00 UTC on the last Sunday of October.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cj {

// Seconds since 1970-01-01T00:00:00Z. Signed 64-bit, no leap seconds.
using UtcSeconds = std::int64_t;

struct CivilDateTime {
    int year = 1970;
    int month = 1;   // 1-12
    int day = 1;     // 1-31
    int hour = 0;
    int minute = 0;
    int second = 0;
};

// 0 = Sunday ... 6 = Saturday
int weekday(int year, int month, int day);

std::int64_t daysFromCivil(int year, int month, int day);
CivilDateTime civilFromUtc(UtcSeconds t);
UtcSeconds utcFromCivil(const CivilDateTime& c);   // treats c as UTC

// True if British Summer Time is in force at the given instant.
bool isBritishSummerTime(UtcSeconds t);

// UK offset from UTC in seconds at instant t (0 or 3600).
int ukOffsetSeconds(UtcSeconds t);

CivilDateTime utcToUk(UtcSeconds t);

// Resolve a UK wall-clock time to UTC.
// Times that don't exist (inside the spring-forward gap) return nullopt.
// Times that occur twice (autumn fall-back hour) resolve to the first (BST)
// occurrence, which is what someone typing "01:30" on that night means in
// practice. The schedule editor should warn about both cases.
std::optional<UtcSeconds> ukToUtc(const CivilDateTime& uk);

// "2026-11-14T19:00" or "2026-11-14T19:00:00". Returns nullopt on bad input.
std::optional<CivilDateTime> parseLocalIso(const std::string& s);

// "2026-11-14T19:00:00Z" style.
std::optional<UtcSeconds> parseUtcIso(const std::string& s);
std::string formatUtcIso(UtcSeconds t);

}  // namespace cj

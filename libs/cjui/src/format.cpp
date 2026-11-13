#include "cjui/format.hpp"

#include <windows.h>

#include <cwchar>

namespace cjui {

static SYSTEMTIME toLocal(cj::UtcSeconds t, SYSTEMTIME* utcOut = nullptr) {
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)(t + 11644473600LL) * 10000000ULL;
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME st, lt;
    FileTimeToSystemTime(&ft, &st);
    SystemTimeToTzSpecificLocalTime(nullptr, &st, &lt);
    if (utcOut) *utcOut = st;
    return lt;
}

static const wchar_t* kDays[] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};
static const wchar_t* kMonths[] = {L"January", L"February", L"March", L"April", L"May", L"June", L"July",
                                   L"August", L"September", L"October", L"November", L"December"};

cj::UtcSeconds nowUtc() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<cj::UtcSeconds>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

std::wstring formatWhen(cj::UtcSeconds t, bool withWeekday) {
    SYSTEMTIME lt = toLocal(t);
    wchar_t buf[64];
    std::wstring day = std::wstring(kDays[lt.wDayOfWeek]).substr(0, 3);
    std::wstring mon = std::wstring(kMonths[lt.wMonth - 1]).substr(0, 3);
    if (withWeekday) swprintf(buf, 64, L"%ls %d %ls, %02d:%02d", day.c_str(), lt.wDay, mon.c_str(), lt.wHour, lt.wMinute);
    else swprintf(buf, 64, L"%d %ls, %02d:%02d", lt.wDay, mon.c_str(), lt.wHour, lt.wMinute);
    return buf;
}

std::wstring formatDay(cj::UtcSeconds t) {
    SYSTEMTIME lt = toLocal(t);
    wchar_t buf[64];
    swprintf(buf, 64, L"%ls %d %ls", kDays[lt.wDayOfWeek], lt.wDay, kMonths[lt.wMonth - 1]);
    return buf;
}

std::wstring formatTime(cj::UtcSeconds t) {
    SYSTEMTIME lt = toLocal(t);
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d:%02d", lt.wHour, lt.wMinute);
    return buf;
}

int localDayKey(cj::UtcSeconds t) {
    SYSTEMTIME lt = toLocal(t);
    return lt.wYear * 10000 + lt.wMonth * 100 + lt.wDay;
}

int localWeekday(cj::UtcSeconds t) { return toLocal(t).wDayOfWeek; }

std::wstring formatRelative(cj::UtcSeconds t, cj::UtcSeconds now) {
    const SYSTEMTIME a = toLocal(t), b = toLocal(now);
    const long long d = cj::daysFromCivil(a.wYear, a.wMonth, a.wDay) - cj::daysFromCivil(b.wYear, b.wMonth, b.wDay);
    if (d == 0) return L"Today";
    if (d == 1) return L"Tomorrow";
    if (d == -1) return L"Yesterday";
    if (d > 1 && d < 7) return kDays[a.wDayOfWeek];
    if (d < 0 && d > -7) return std::to_wstring(-d) + L" days ago";
    const std::wstring w = formatWhen(t, false);
    return w.substr(0, w.find(L','));
}

std::wstring formatCountdown(cj::UtcSeconds t, cj::UtcSeconds now) {
    const long long mins = (t - now) / 60;
    if (mins < 1) return L"Any moment now";
    if (mins < 60) return L"In " + std::to_wstring(mins) + (mins == 1 ? L" minute" : L" minutes");
    if (mins < 24 * 60) return L"In " + std::to_wstring(mins / 60) + (mins / 60 == 1 ? L" hour" : L" hours");
    if (formatRelative(t, now) == L"Tomorrow") return L"Tomorrow";
    return L"In " + std::to_wstring((mins + 12 * 60) / (24 * 60)) + L" days";
}

std::wstring formatDuration(int minutes) {
    if (minutes <= 0) return {};
    const int h = minutes / 60, m = minutes % 60;
    if (h == 0) return std::to_wstring(m) + L" min";
    if (m == 0) return std::to_wstring(h) + L" hr";
    return std::to_wstring(h) + L" hr " + std::to_wstring(m) + L" min";
}

std::wstring formatElapsed(cj::UtcSeconds since, cj::UtcSeconds now) {
    long long s = now - since;
    if (s < 0) s = 0;
    wchar_t buf[32];
    swprintf(buf, 32, L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    return buf;
}

std::wstring ukHint(cj::UtcSeconds t) {
    SYSTEMTIME utc;
    SYSTEMTIME lt = toLocal(t, &utc);
    FILETIME a, b;
    SystemTimeToFileTime(&lt, &a);
    SystemTimeToFileTime(&utc, &b);
    const long long diff = (long long)(((ULONGLONG)a.dwHighDateTime << 32) | a.dwLowDateTime) -
                           (long long)(((ULONGLONG)b.dwHighDateTime << 32) | b.dwLowDateTime);
    if (diff / 10000000LL == cj::ukOffsetSeconds(t)) return {};
    const cj::CivilDateTime uk = cj::utcToUk(t);
    wchar_t buf[32];
    swprintf(buf, 32, L"  (%02d:%02d UK)", uk.hour, uk.minute);
    return buf;
}

}  // namespace cjui

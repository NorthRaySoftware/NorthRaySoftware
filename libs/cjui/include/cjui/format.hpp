// Dates and times as people read them, in the viewer's own time zone.
#pragma once

#include "cj/time.hpp"

#include <string>

namespace cjui {

std::wstring formatWhen(cj::UtcSeconds t, bool withWeekday = true);  // "Sat 14 Nov, 19:00"
std::wstring formatDay(cj::UtcSeconds t);                            // "Saturday 14 November"
std::wstring formatTime(cj::UtcSeconds t);                           // "19:00"
std::wstring formatRelative(cj::UtcSeconds t, cj::UtcSeconds now);   // "Today", "Tomorrow", "Saturday", "3 days ago"
std::wstring formatCountdown(cj::UtcSeconds t, cj::UtcSeconds now);  // "In 2 days", "In 40 minutes"
std::wstring formatDuration(int minutes);                            // "2 hr", "1 hr 30 min"
std::wstring formatElapsed(cj::UtcSeconds since, cj::UtcSeconds now);// "1:04:22"
// "  (19:00 UK)" when the viewer isn't on UK time, otherwise empty.
std::wstring ukHint(cj::UtcSeconds t);
int localDayKey(cj::UtcSeconds t);                                   // yyyymmdd in local time
int localWeekday(cj::UtcSeconds t);                                  // 0 = Sunday
cj::UtcSeconds nowUtc();

}  // namespace cjui

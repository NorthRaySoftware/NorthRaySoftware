// Reads a calendar subscription (iCalendar / .ics) into CJ Radio programmes.
//
// The host keeps the schedule in any calendar app that can publish an .ics
// link (Google Calendar, Outlook, Apple iCloud, Proton Calendar). CJ Radio
// and the website read that link. Rules:
//   * Each timed event is a programme. Repeating events (RRULE) are expanded,
//     with EXDATE exclusions and RECURRENCE-ID overrides (one week moved or
//     cancelled) applied.
//   * STATUS:CANCELLED -> Cancelled (shown as cancelled), TENTATIVE -> Draft
//     (never shown to listeners), anything else -> Confirmed.
//   * All-day events have no start time, so they're skipped rather than given
//     an invented one. Private/confidential events are skipped too.
//   * Times: UTC ("Z"), Europe/London (and its aliases), or floating (taken
//     as UK time, the station's time zone). Any other time zone is skipped
//     with a warning rather than guessed.
//   * Being in the calendar never makes the station "live".
#pragma once

#include "cj/radio.hpp"

#include <string>
#include <vector>

namespace cj::ical {

struct Result {
    std::vector<radio::Programme> programmes;  // occurrences overlapping [from, to), sorted
    std::vector<std::string> warnings;
    int events = 0;                            // VEVENTs read
    std::string calendarName;                  // X-WR-CALNAME, if given
    bool valid = false;                        // looked like a calendar at all
};

// `series` lets an event titled e.g. "The CJ Philip Show" pick up that
// series' presenter and category. A line "Presenter: Name" in the event's
// description sets the presenter explicitly.
Result toProgrammes(const std::string& icsText, UtcSeconds from, UtcSeconds to,
                    const std::vector<radio::Series>& series = {});

// "webcal://..." -> "https://...". Returns "" unless the result is https.
std::string normaliseUrl(const std::string& url);

}  // namespace cj::ical

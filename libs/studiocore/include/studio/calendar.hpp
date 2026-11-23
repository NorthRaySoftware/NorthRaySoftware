// A world's own calendar (0.74): months (numbered from 1, from 0, or with an extra month 0 before month 1), a week,
// eras, leap rules, moons, seasons, holidays and the hours of a day. Dates are counted as whole days from the first
// day of the first month of year 1 (day 0), so any two dates can be compared, subtracted and placed on a timeline.
//
// Kept in the library's settings as JSON: "calendar:world:<id>", or "calendar:book:<id>" for an independent book
// or a book with a calendar of its own. Portable; no Windows code.
#pragma once

#include "studio/library.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace studio::cal {

struct Month {
    std::string name;
    int days = 30;
};

// In years where (year - offset) is a multiple of `every`, `add` days (may be negative) go to month `month`.
// Gregorian: every 4 +1, every 100 -1, every 400 +1, all to February.
struct LeapRule {
    int every = 4;
    int offset = 0;
    int month = 0;
    int add = 1;
};

// An era counts its year 1 from the plain year `start`.
struct Era {
    std::string name;
    std::string abbr;
    std::int64_t start = 1;
};

// A moon may be anchored to the calendar (0.78) instead of running freely: on a date it's at a set point of its cycle,
// every year ("full on 3 Frostmoon every year") or every month ("full on 15 of every month", with months it skips,
// such as an intercalary month 0). The cycle runs on from the anchor and snaps back to it each time the anchor comes
// round, so a phase can be skipped or repeated where the year or month doesn't fit the cycle.
struct MoonAnchor {
    enum Repeat { None, Year, Month } repeat = None;
    int month = 0;              // Year: the anchor's month (index)
    int day = 1;                // the day of that month (a month too short for it uses its last day)
    double days = 0;            // how far into the cycle it is then: 0 new, cycle/4 first quarter, cycle/2 full...
    std::vector<int> except;    // Month: months with no anchor of their own (the cycle runs on through them)
};

struct Moon {
    std::string name;
    double cycle = 29.53;   // days from new moon to new moon
    double offset = 0;      // days into its cycle on day 0 (when it isn't anchored)
    MoonAnchor anchor;
};

struct Season {
    std::string name;
    int month = 0;   // month index
    int day = 1;     // it starts on this day
};

struct Holiday {
    std::string name;
    int month = 0;
    int day = 1;
    Id entry;        // a bible entry it's about (optional)
    std::string notes;
};

enum class MonthZero { None, Count, Extra };   // months 1..n; months counted 0..n-1; an extra month 0 before month 1

struct Calendar {
    std::string name;
    MonthZero monthZero = MonthZero::None;
    std::vector<Month> months;
    std::vector<std::string> weekdays;
    int firstWeekday = 0;                      // the weekday (index) of day 0
    std::vector<LeapRule> leap;
    std::vector<Era> eras;                     // in order of start
    std::string beforeName, beforeAbbr;        // years before the first era count back: "312 BL"
    std::vector<Moon> moons;
    std::vector<Season> seasons;
    std::vector<Holiday> holidays;
    int hoursPerDay = 24, minutesPerHour = 60;
    std::string order = "dmy";                 // how dates are written: dmy, mdy or ymd
    bool empty() const { return months.empty(); }
};

struct Date {
    std::int64_t year = 1;   // the plain year (year 1 is the year of day 0; 0 and negative years come before it)
    int month = 0;           // month index
    int day = 1;             // 1-based
    bool operator==(const Date& o) const { return year == o.year && month == o.month && day == o.day; }
};

Calendar gregorian();
Calendar blank();      // one month of 30 days, a seven-day week, to fill in

int monthNumber(const Calendar& c, int index);            // the month's number as written (0- or 1-based)
std::string monthLabel(const Calendar& c, int index);     // its name, or "Month N"
int monthLength(const Calendar& c, std::int64_t year, int month);
std::int64_t yearLength(const Calendar& c, std::int64_t year);
double meanYear(const Calendar& c);
std::int64_t daysBeforeYear(const Calendar& c, std::int64_t year);   // days from day 0 to the start of `year`
std::int64_t toDay(const Calendar& c, const Date& d);
Date fromDay(const Calendar& c, std::int64_t day);
Date clampDate(const Calendar& c, Date d);                 // a valid date (month and day in range)
int weekday(const Calendar& c, std::int64_t day);          // index into weekdays (0 when there's no week)

// A plain year as its era writes it.
struct EraYear { std::int64_t year; std::string abbr; std::string name; bool before = false; };
EraYear eraYear(const Calendar& c, std::int64_t year);
std::int64_t plainYear(const Calendar& c, std::int64_t year, const std::string& eraAbbrOrName);   // the reverse

std::string formatDate(const Calendar& c, const Date& d, bool withEra = true);   // "3 Frostmoon 1204 AL"
std::string formatYear(const Calendar& c, std::int64_t year);                    // "1204 AL"
std::string formatTime(const Calendar& c, int minutes);                          // "14:05" (minutes into the day)
std::optional<int> parseTime(const Calendar& c, const std::string& s);
// Reads "3 Frostmoon 1204", "Frostmoon 3, 1204 AL", "1204-2-3", "3/2/1204" (in the calendar's order), month names by
// any unique start of 3+ letters.
std::optional<Date> parseDate(const Calendar& c, const std::string& s);

int ageAt(const Calendar& c, const Date& born, const Date& at);   // whole years; negative before birth

double moonPhase(const Moon& m, std::int64_t day);   // 0 new, 0.5 full, towards 1 waning (running freely)
// The phase with the moon's anchor, if it has one (0.78); the day its cycle last snapped back to the anchor in `anchorDay`.
double moonPhase(const Calendar& c, const Moon& m, std::int64_t day, std::int64_t* anchorDay = nullptr);
std::string anchorText(const Calendar& c, const Moon& m);                  // "full on 15 of every month except Yearsend", "" free
bool parseAnchor(const Calendar& c, const std::string& text, Moon& m);    // the reverse; false if not understood
std::string phaseName(double phase);                 // "New moon", "Waxing crescent", ... "Full moon" ...
const Season* seasonOn(const Calendar& c, const Date& d);
std::vector<const Holiday*> holidaysOn(const Calendar& c, const Date& d);

// Problems that would make dates wrong (no months, a month with no days, a leap rule taking a month below 0...).
std::vector<std::string> problems(const Calendar& c);

std::string toJson(const Calendar& c);
Calendar fromJson(const std::string& json);
// A plain-text form for sharing and for writing by hand (the manual has a template).
std::string toText(const Calendar& c);
Calendar fromText(const std::string& text, std::vector<std::string>* warnings = nullptr);

// Dates written on bible entries (0.75): each detail line whose value reads as a date. Born / Birth / Date of birth
// are births, Died / Death / Date of death deaths; an event's Date / When / Began / Started is its date.
struct DatedFact {
    enum Kind { Birth, Death, EventDate, Other } kind = Other;
    Id entry;
    std::string label;   // the detail's label as written
    Date date;
    std::int64_t day = 0;
};
std::vector<DatedFact> datedFacts(const Calendar& c, const std::vector<const Entry*>& entries);
std::vector<DatedFact> datedFactsOf(const Calendar& c, const Entry& e);

// A book's timeline in calendar dates (0.75): "timeline:<book>" holds "calendar": true and "start", the calendar day
// of story day 0. Story days stay as they were, so switching back and forth loses nothing.
struct StorySpan { bool on = false; std::int64_t start = 0; bool any = false; std::int64_t first = 0, last = 0; };
StorySpan storySpan(const Library& lib, const Id& book);   // first/last: the calendar days of everything placed
bool setTimelineCalendar(Library& lib, const Id& book, bool on, std::optional<std::int64_t> start = std::nullopt);

// How old someone born on `born` is through a book: at its first and last dates (when it has calendar dates).
struct AgeInBook { Id book; int from = 0, to = 0; };
std::vector<AgeInBook> agesInBooks(const Library& lib, const Calendar& c, const Date& born, const std::vector<Id>& books,
                                   std::optional<Date> died = std::nullopt);

// Which calendar a book or world uses: the book's own, else its world's. Empty (no months) when there's none.
std::string settingKeyForWorld(const Id& world);
std::string settingKeyForBook(const Id& book);
Calendar calendarOfWorld(const Library& lib, const Id& world);
Calendar calendarOfBook(const Library& lib, const Id& book);
bool bookHasOwnCalendar(const Library& lib, const Id& book);
bool saveWorldCalendar(Library& lib, const Id& world, const Calendar& c);
bool saveBookCalendar(Library& lib, const Id& book, const Calendar& c);   // an empty calendar removes it

}  // namespace studio::cal

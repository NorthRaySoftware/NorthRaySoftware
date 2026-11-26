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

// An era counts its year 1 from the plain year `start`. One that counts down (0.80) numbers its years down to the next
// era instead, its last year being 1, like years before Christ.
struct Era {
    std::string name;
    std::string abbr;
    std::int64_t start = 1;
    bool countDown = false;
};

// A named time of day (0.80): it runs from `start` (minutes into the day) until the next one starts; the last runs on
// round midnight until the first.
struct DayPart {
    std::string name;
    int start = 0;
};

// A time of day: a clock time (minutes into the day) or one of the calendar's named times. Empty when neither.
struct TimeOfDay {
    int minutes = -1;
    std::string part;
    bool empty() const { return minutes < 0 && part.empty(); }
    bool operator==(const TimeOfDay& o) const { return minutes == o.minutes && part == o.part; }
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

// A pause (0.79): every year from one day to another (inclusive; it may run on into the next year) the moon holds still.
// With a phase it holds that phase and carries on from it afterwards ("holds new from 1 Rebirth to 14 Rebirth"); without
// one it holds whatever phase it had when the pause began and carries on from there ("holds from 1 Rebirth to 14 Rebirth").
struct MoonPause {
    int fromMonth = 0, fromDay = 1, toMonth = 0, toDay = 1;
    bool setsPhase = false;
    double days = 0;   // with setsPhase: days into its cycle it holds
};

// An eclipse (0.79): solar or lunar, on a date every year, once on a date of one year, or at each new (or full) moon of
// this moon that falls on a date or in a month.
struct Eclipse {
    enum Kind { Solar, Lunar } kind = Lunar;
    enum When { EveryYear, Once, AtPhaseOnDate, AtPhaseInMonth } when = EveryYear;
    bool full = true;            // At…: at the full moon (else the new moon)
    int month = 0, day = 1;      // the date (AtPhaseInMonth: the month only)
    std::int64_t year = 1;       // Once: the plain year
    TimeOfDay time;              // when in the day (0.80; optional)
};

struct Moon {
    std::string name;
    double cycle = 29.53;   // days from new moon to new moon
    double offset = 0;      // days into its cycle on day 0 (when it isn't anchored)
    MoonAnchor anchor;
    std::vector<MoonPause> pauses;
    std::vector<Eclipse> eclipses;
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
    TimeOfDay time;  // when in the day (0.80; optional)
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
    // 0.80: dates as numbers ("01/00/1204": day and month two digits, the month as numbered, the year at least two)
    // with `separator` between, and how the era follows a date: "abbr" (1204 AE), "name" (1204 Age of Embers), "none".
    bool numeric = false;
    char separator = '/';
    std::string eraStyle = "abbr";
    std::vector<DayPart> dayParts;             // named times of day, by start (0.80)
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
// A date in a chosen style (0.85), for typeset books and the shared bible: `form` "words" (4 Bloomtide 2033),
// "numbers" (04/04/2033, in the calendar's order and separator) or "" (as the calendar writes it); `era` "abbr", "name",
// "none" or "" (the calendar's choice).
std::string formatDateStyled(const Calendar& c, const Date& d, const std::string& form, const std::string& era);
std::string formatTime(const Calendar& c, int minutes);                          // "14:05" (minutes into the day)
std::optional<int> parseTime(const Calendar& c, const std::string& s);

// Times of day (0.80).
std::vector<DayPart> defaultDayParts(const Calendar& c);   // Dawn, Morning, Midday, Afternoon, Evening, Night
const DayPart* dayPartAt(const Calendar& c, int minutes);  // the named time a clock time falls in (nullptr if none)
const DayPart* dayPartNamed(const Calendar& c, const std::string& name);   // by name, or a unique start of 3+ letters
// "14:30", "2:30 pm", "dawn", "at dusk", "in the morning"; nullopt when it's none of these.
std::optional<TimeOfDay> parseTimeOfDay(const Calendar& c, const std::string& s);
// "dawn", or "14:30" (with " (afternoon)" when `withPart` and a named time holds it). Empty for an empty time.
std::string timeOfDayText(const Calendar& c, const TimeOfDay& t, bool withPart = false);
int timeSortKey(const Calendar& c, const TimeOfDay& t);    // minutes into the day (a named time's start); -1 empty
std::string dayPartsText(const Calendar& c);               // "Dawn 5:00, Morning 7:00, ..." (or "none")
std::vector<DayPart> parseDayParts(const Calendar& c, const std::string& text, std::vector<std::string>* warnings = nullptr);
// A date that may be followed by a time: "3 Frostmoon 1204 AE at dawn", "3 Frostmoon 1204, 14:30".
struct DateTime { Date date; TimeOfDay time; };
std::optional<DateTime> parseDateTime(const Calendar& c, const std::string& s);
// Reads "3 Frostmoon 1204", "Frostmoon 3, 1204 AL", "1204-2-3", "3/2/1204" (in the calendar's order), month names by
// any unique start of 3+ letters.
std::optional<Date> parseDate(const Calendar& c, const std::string& s);

int ageAt(const Calendar& c, const Date& born, const Date& at);   // whole years; negative before birth

double moonPhase(const Moon& m, std::int64_t day);   // 0 new, 0.5 full, towards 1 waning (running freely)
// The phase with the moon's anchor, if it has one (0.78); the day its cycle last snapped back to the anchor in `anchorDay`.
double moonPhase(const Calendar& c, const Moon& m, std::int64_t day, std::int64_t* anchorDay = nullptr);
std::string anchorText(const Calendar& c, const Moon& m);                  // "full on 15 of every month except Yearsend", "" free
bool parseAnchor(const Calendar& c, const std::string& text, Moon& m);    // the reverse; false if not understood
std::string pauseText(const Calendar& c, const Moon& m, const MoonPause& p);   // "holds new from 1 Rebirth to 14 Rebirth"
bool parsePause(const Calendar& c, const std::string& text, const Moon& m, MoonPause& out);
std::string eclipseText(const Calendar& c, const Eclipse& e);             // "lunar eclipse at every full moon in Thaw"
bool parseEclipse(const Calendar& c, const std::string& text, Eclipse& out);
bool pausedOn(const Calendar& c, const Moon& m, std::int64_t day);       // the moon is holding still that day
// The eclipses on a day: each with the moon it belongs to.
struct EclipseOn { const Moon* moon; Eclipse::Kind kind; TimeOfDay time; };
std::vector<EclipseOn> eclipsesOn(const Calendar& c, std::int64_t day);
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
    TimeOfDay time;      // "Born: 3 Frostmoon 1204 at dawn" (0.80)
    std::int64_t day = 0;
};
std::vector<DatedFact> datedFacts(const Calendar& c, const std::vector<const Entry*>& entries);
std::vector<DatedFact> datedFactsOf(const Calendar& c, const Entry& e);

// A book's timeline in calendar dates (0.75): "timeline:<book>" holds "calendar": true and "start", the calendar day
// of story day 0. Story days stay as they were, so switching back and forth loses nothing.
struct StorySpan { bool on = false; std::int64_t start = 0; bool any = false; std::int64_t first = 0, last = 0; };
StorySpan storySpan(const Library& lib, const Id& book);   // first/last: the calendar days of everything placed
bool setTimelineCalendar(Library& lib, const Id& book, bool on, std::optional<std::int64_t> start = std::nullopt);
// Reads a book's plan dates again in its calendar (0.82): each chapter or scene whose date came from a plan's When line
// ("date" in its timeline JSON) is put on that date, and one whose When words weren't read as a date (no calendar
// then, or one that couldn't read them) is put on the timeline when they now read. The timeline goes into calendar
// dates the first time. Run when a calendar is saved; returns how many were placed or moved.
int rereadPlanDates(Library& lib, const Id& book);
// A story event's time of day on a book's timeline (0.80), kept beside its day.
TimeOfDay eventTime(const Library& lib, const Id& book, const Id& entry);
bool setEventTime(Library& lib, const Id& book, const Id& entry, const TimeOfDay& t);

// How old someone born on `born` is through a book: at its first and last dates (when it has calendar dates).
struct AgeInBook { Id book; int from = 0, to = 0; };
std::vector<AgeInBook> agesInBooks(const Library& lib, const Calendar& c, const Date& born, const std::vector<Id>& books,
                                   std::optional<Date> died = std::nullopt);

// ------------------------------------------------------------------ ages worked out (0.86)
//
// A character's birth from their page: "Born:" with a whole date ("5 Bloomtide 2009 TA"), or a year alone ("2009 TA":
// the age is then "about"), or "Birthday: 5 Bloomtide" with a "Born:" year. "Died:" as for dated facts. "Ageless: yes"
// (or an Age of "ageless") marks someone who doesn't age; "Appears: 30" (or "Apparent age:") gives the age they look,
// shown in place of the worked-out one.
struct Birth {
    bool known = false;      // a date (or a year) to count from
    bool about = false;      // a year alone
    Date date;
    std::optional<Date> died;
    bool ageless = false;
    std::string appears;     // "30"
    std::string typedAge;    // the page's own Age line, as written
    std::string from;        // the line(s) it was read from ("Born: 5 Bloomtide 2009 TA")
};
Birth birthOf(const Calendar& c, const Entry& e);
// How old they are in one book: as of its "as of" day (the book's choice, else its first dated chapter in reading
// order), and whether a birthday falls inside the book's dates (up to its last dated chapter or scene), with the
// chapter it falls in or before.
struct BookAge {
    Id book;
    std::string title;
    std::int64_t asOf = 0, last = 0;
    bool chosenAsOf = false;   // the writer's own "as of" date
    int age = 0;
    bool about = false;
    bool turns = false;        // a birthday inside the book
    int turnsTo = 0;
    Date birthday;
    Id chapter;                // the chapter on or after it
    std::string chapterTitle;
};
// Books in `books` order with calendar dates in which they're alive at the "as of" day (born by then, not yet dead).
std::vector<BookAge> agesThroughBooks(const Library& lib, const Calendar& c, const Birth& b, const std::vector<Id>& books);
// "24, turns 25 during this book (5 Bloomtide, in “The Wall”)", "about 24", "25".
std::string ageText(const Calendar& c, const BookAge& a);
// A book's "as of" day for ages: the writer's date (setting "ages-as-of:<book>", in the calendar's words), else the
// first chapter or scene in reading order with a calendar date. nullopt when the book has no calendar dates.
std::optional<std::int64_t> agesAsOf(const Library& lib, const Calendar& c, const Id& book, bool* chosen = nullptr);
std::string agesAsOfText(const Library& lib, const Id& book);
bool setAgesAsOf(Library& lib, const Id& book, const std::string& text);   // "" goes back to the first dated chapter
// A typed Age that disagrees with every worked-out age (as of each book and at its end), with the ages it was checked
// against; empty when it agrees, or there's nothing to check.
std::string ageDisagreement(const Calendar& c, const Birth& b, const std::vector<BookAge>& ages);

// Which calendar a book or world uses: the book's own, else its world's. Empty (no months) when there's none.
std::string settingKeyForWorld(const Id& world);
std::string settingKeyForBook(const Id& book);
Calendar calendarOfWorld(const Library& lib, const Id& world);
Calendar calendarOfBook(const Library& lib, const Id& book);
bool bookHasOwnCalendar(const Library& lib, const Id& book);
bool saveWorldCalendar(Library& lib, const Id& world, const Calendar& c);
bool saveBookCalendar(Library& lib, const Id& book, const Calendar& c);   // an empty calendar removes it

}  // namespace studio::cal

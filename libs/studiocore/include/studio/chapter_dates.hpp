// Writing Studio: chapter dates as formatted text (phase 5 of the director's combined request, 0.105). One source for
// the date line Writing shows over a chapter and the typeset book, ebook and Word document print under its heading:
// the chapter's day on Planning's timeline (its own, else its first placed scene's) written in the book's date format,
// or the chapter's own.
#pragma once

#include "studio/calendar.hpp"
#include "studio/library.hpp"

#include <optional>
#include <string>

namespace studio::dates {

struct Format {
    // "numeric-dmy" 12/03/1450, "numeric-mdy" 03/12/1450, "numeric-ymd" 1450-03-12, "day-month" 12 Harvestmoon 1450,
    // "long" Tuesday, 12 Harvestmoon 1450, "month" Harvestmoon 1450, or "custom" (`pattern`).
    std::string style = "day-month";
    // Tokens: {D} 3, {DD} 03, {M} 4, {MM} 04, {Month} Harvestmoon, {Mon} Har, {Year} {YYYY} 1450, {Weekday} Tuesday,
    // {Era} the era's short form, {EraName} its full name. Anything else is written as it is.
    std::string pattern = "{D} {Month} {Year}";
    std::string era = "short";          // "none", "short" (1450 SE) or "full" (1450 The Second Era); not when the pattern names it
    std::string eraPlace = "after";     // "after" or "before" the date
    std::string eraSeparator = " ";     // between them: " ", " | ", ", ", " · " or the writer's own
    std::string time = "hidden";        // the time of day: "hidden", "appended" (after ", ") or "line" (a line of its own)
    std::string toJson() const;
    static Format fromJson(const std::string& json);   // the defaults where it says nothing
    bool operator==(const Format& o) const { return toJson() == o.toJson(); }
};
const std::vector<std::pair<std::string, std::string>>& styles();   // id -> label ("Numeric, day first")

// The date as the format writes it, and the time of day when it goes on its own line ("" otherwise).
struct Lines { std::string date, time; };
Lines format(const cal::Calendar& c, const cal::Date& d, const cal::TimeOfDay& t, const Format& f);

// The book's format and each chapter's own (setting "dates:<book>": {"format": {...}, "chapters": {id: {...}}}).
// A book saved before 0.105 starts from its Typesetting design's dateForm and dateEra.
Format bookFormat(const Library& lib, const Id& book);
void setBookFormat(Library& lib, const Id& book, const Format& f);
std::optional<Format> chapterFormat(const Library& lib, const Id& book, const Id& chapter);   // its own, if it has one
void setChapterFormat(Library& lib, const Id& book, const Id& chapter, const std::optional<Format>& f);
Format formatFor(const Library& lib, const Id& book, const Id& chapter);   // its own, else the book's

// A chapter's or scene's date line, when the book's timeline counts in calendar dates and it's placed (a chapter: its own
// day, else its first placed scene's). A scene's carries its chapter's format.
std::optional<Lines> lineFor(const Library& lib, const ManuscriptNode& n);

}  // namespace studio::dates

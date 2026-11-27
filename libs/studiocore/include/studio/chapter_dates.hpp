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

// How a part of the date line looks (5b, 0.106): its weight, slant and case ("as", "small" small capitals, "upper").
struct PartLook {
    bool bold = false, italic = true;
    std::string caps = "as";
    bool operator==(const PartLook& o) const { return bold == o.bold && italic == o.italic && caps == o.caps; }
};
// The line's look: each part's, its alignment ("left", "center", "right") and where it goes: "under" the title,
// "header" with the header picture (above the title), or "above" the text's first line. `sceneDates`: a scene's own
// date at the scene break before it, when it differs from its chapter's.
struct Look {
    PartLook date, era, time;
    std::string align = "center", position = "under";
    bool sceneDates = false;
};

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
    Look look;
    std::string toJson() const;
    static Format fromJson(const std::string& json);   // the defaults where it says nothing
    bool operator==(const Format& o) const { return toJson() == o.toJson(); }
};
const std::vector<std::pair<std::string, std::string>>& styles();   // id -> label ("Numeric, day first")

// The date as the format writes it, and the time of day when it goes on its own line ("" otherwise); each line also in
// parts (0 the date, 1 the era, 2 the time) for styling, and the format's look.
struct Seg { std::string text; int part = 0; };
struct Lines {
    std::string date, time;
    std::vector<Seg> dateSegs, timeSegs;
    Look look;
};
std::string applyCaps(const std::string& text, const std::string& caps);   // "upper": upper-cased (letters with cases)
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

// ------------------------------------------- a scene's own date at its break (0.108)

// A scene break in the text can carry its scene's date: attrs {"sid": its id, "day": days from the timeline's start (as
// the timeline counts), "time": minutes or "part": a part of the day}. The scenes of a chapter (or any text) with breaks:
// scene 1 opens it, scene n + 1 follows the n-th break (in the order they come, lists and quotes included).
struct Scene {
    int number = 1;
    std::string sid;                    // the break's id ("" for scene 1, or a break saved before 0.108)
    bool dated = false;                 // the break has a date of its own
    std::optional<Lines> line;          // its date line (scene 1: the chapter's)
    bool differs = false;               // dated, and not the chapter's date and time
    double day = 0;                     // when dated: as stored
    cal::TimeOfDay time;
};
// The scenes of `n`'s text (`content`, else its saved text). Empty when the text has no scene breaks.
std::vector<Scene> scenesOf(const Library& lib, const ManuscriptNode& n, const std::string& content = {});
// A break's date line from its attrs, written in its chapter's format (nullopt: undated, or no calendar dates).
std::optional<Lines> breakLine(const Library& lib, const ManuscriptNode& n, const std::string& attrsJson);
// The date lines to print after each break of a text, in order: set where the scene's date differs from its chapter's.
std::vector<std::optional<Lines>> breakLines(const Library& lib, const ManuscriptNode& n, const std::string& content);
std::string newBreakId();               // a fresh break id (12 lower-case letters and digits)
// "A scene's own date at its scene break": the book's format's look, and every chapter's own format's.
bool printsSceneDates(const Library& lib, const Id& book);
void setPrintsSceneDates(Library& lib, const Id& book, bool on);
// Whether the writer has been asked once to print scene dates (remembered with the book's dates).
bool askedSceneDates(const Library& lib, const Id& book);
void setAskedSceneDates(Library& lib, const Id& book);

// ------------------------------------------- typed date lines (5c, 0.107)

// A chapter whose text opens with a date line typed into it ("12 Harvestmoon 1450 SE", "Tuesday, 3 Frostmoon 1204, at
// dawn"): what it says, and whether it reads as a date in the book's calendar (else `why`, and it's left alone).
struct TypedDate {
    Id chapter;
    std::string title;
    int index = -1;                     // the paragraph, in the document's top-level content
    // 0.108: a date line straight after a scene break: the break's top-level index and its scene's number (else -1, 1).
    int breakIndex = -1;
    int scene = 1;
    std::string text;
    bool readable = false;
    cal::Date date;
    cal::TimeOfDay time;
    std::string becomes;                // the line as the book's format writes it
    std::string current;                // the chapter's date on the timeline now ("" none)
    std::string why;                    // not readable: why
};
// Every chapter's (or those in `only`) typed opening date line, and (0.108) each typed line straight after a scene break. Readable only when the book's timeline counts in its
// calendar's dates.
std::vector<TypedDate> findTypedDates(const Library& lib, const Id& book, const std::vector<Id>& only = {});
// Puts each readable one's date (and time) on the timeline as the chapter's own (or on its break as the scene's) and takes
// the line out of its text.
// What it changed, for undoing.
struct Converted {
    int count = 0;
    int scenes = 0;                     // of them, scenes' dates on their breaks (0.108)
    std::string timelineBefore;
    std::vector<std::pair<Id, std::string>> contentBefore;
};
Converted convertTypedDates(Library& lib, const Id& book, const std::vector<TypedDate>& found);
void undoConverted(Library& lib, const Id& book, const Converted& c);

}  // namespace studio::dates

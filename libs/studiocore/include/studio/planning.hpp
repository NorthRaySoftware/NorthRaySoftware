// Writing Studio: planning's own data (0.50): story structures (built in and the author's own), plot threads, and
// whole outlines as plain text to import and export. Portable and tested.
//
// Kept in the library's settings:
//   "structure:<book>"     {"template": id, "beats": {beat: [node, ...]}, "notes": {beat: text}}   (a beat's value was one
//                          node id before 0.50; still read)
//   "structures:<scope>"   [{id, name, about, beats: [{id, name, at, about}]}]   the author's own, per world ("world:<id>")
//                          or independent book ("book:<id>")
//   "threads:<book>"       {"threads": [{id, name, colour, about}], "nodes": {node: [thread, ...]}}
//   "timeline:<book>"      {"unit": ..., "nodes": {node: {"day": n, "when": text}}, "events": {entry: {"day": n}}}
//                          (the timeline's, 0.22; the bible's events placed on it, 0.52)
//   "nodeplaces:<book>"    {node: place entry}   where a chapter or scene happens (0.52)
#pragma once

#include "studio/library.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace studio::plan {

// ------------------------------------------------------------------ structures

struct Beat { std::string id, name; double at = 0; std::string about; };
struct Structure {
    std::string id, name, about;
    std::vector<Beat> beats;
    bool own = false;   // the author's, not built in
};
const std::vector<Structure>& builtInStructures();
std::vector<Structure> ownStructures(const Library& lib, const std::string& scope);
bool setOwnStructures(Library& lib, const std::string& scope, const std::vector<Structure>& list);
// Built-in first, then the author's; "" if none.
const Structure* findStructure(const std::vector<Structure>& all, const std::string& id);
std::vector<Structure> allStructures(const Library& lib, const std::string& scope);

// A structure as text, and back:
//   Structure: My shape
//   About: what it's for
//   0% Opening: what happens here
//   25% Into the woods: ...
// (a percentage, the beat's name, a colon, then what it's about; "Beat:" before a line is allowed.)
std::string structureText(const Structure& s);
// Several structures in one text are split at each "Structure:" line. Beats are sorted by where they fall; ids are
// made from the names. Lines that aren't understood are reported in `skipped`.
std::vector<Structure> parseStructures(const std::string& text, std::vector<std::string>* skipped = nullptr);

// A book's structure: which one, the chapters each beat is carried by, and notes per beat.
struct BookStructure {
    std::string templateId;
    std::map<std::string, std::vector<Id>> beats;
    std::map<std::string, std::string> notes;
};
BookStructure bookStructure(const Library& lib, const Id& book);
bool setBookStructure(Library& lib, const Id& book, const BookStructure& s);

// ------------------------------------------------------------------ plot threads

struct Thread { std::string id, name, colour, about; };
struct Threads {
    std::vector<Thread> threads;
    std::map<Id, std::vector<std::string>> nodes;   // node -> thread ids
    const Thread* find(const std::string& id) const;
    const Thread* named(const std::string& name) const;   // ignoring case
    bool has(const Id& node, const std::string& thread) const;
};
Threads threadsOf(const Library& lib, const Id& book);
bool setThreads(Library& lib, const Id& book, const Threads& t);
std::string newThreadId(const Threads& t);
// A pleasant colour for the n-th thread (a palette that reads on light and dark).
std::string threadColour(size_t n);

// Threads as text, and back:
//   Thread: The shards of light
//   Colour: #C0392B
//   About: Tom's search for the three shards.
//   Chapters: 1, 3-5, The Storm Wall
// Chapters are numbers (in reading order, chapters only), ranges, or titles (scenes by title too).
std::string threadsText(const Library& lib, const Id& book, const Threads& t);
struct ThreadImport { int added = 0, updated = 0, tagged = 0; std::vector<std::string> unknown; };
// Threads of the same name are updated (their chapters are added to); others are added.
bool importThreads(Library& lib, const Id& book, const std::string& text, ThreadImport& out, bool chapterLists = true);

// ------------------------------------------------------------------ places and story events (0.52)

// Where each chapter or scene happens: a place (or realm) of the bible.
std::map<Id, Id> nodePlaces(const Library& lib, const Id& book);
bool setNodePlace(Library& lib, const Id& book, const Id& node, const Id& place);   // empty place: none
// Who is present in each chapter or scene (0.59): characters (or creatures) of the bible, in the order chosen. A
// character only named in the text isn't present unless chosen here. Capture's cast list reads it.
std::map<Id, std::vector<Id>> nodeCast(const Library& lib, const Id& book);
bool setNodeCast(Library& lib, const Id& book, const Id& node, const std::vector<Id>& cast);   // empty: none
// A chapter's cast with its scenes' after it (no repeats, deleted entries left out).
std::vector<Id> castOf(const Library& lib, const Id& book, const Id& node);
// The bible's events placed on the book's timeline, by day (in the timeline's unit).
std::map<Id, double> timelineEvents(const Library& lib, const Id& book);
bool setTimelineEvent(Library& lib, const Id& book, const Id& entry, std::optional<double> day);   // nullopt: off it

// ------------------------------------------------------------------ outlines

// An outline as text (parts, chapters and scenes with their planning), to fill a book or copy one out:
//   Part: Part One
//   Chapter: The Forge
//   Viewpoint: Tom
//   Status: Draft
//   Target: 3000
//   Threads: The shards of light, Tom and Sarrah
//   When: Day 3            (or any words, "the night of the storm")
//   Place: The Forge       (a place or realm of the bible)
//   Tom finds the ledger... (the synopsis: any other lines)
//   Scene: Night watch
// Markdown headings work too: with three levels in use they're parts, chapters and scenes; with two, chapters and
// scenes; with one, chapters.
struct OutlineItem {
    NodeKind kind = NodeKind::Chapter;
    std::string title, synopsis, viewpoint, status, when, place;
    std::int64_t target = 0;
    std::vector<std::string> threads;
    std::vector<std::string> beats;   // "Beat: Midpoint": beats of the book's structure this chapter carries
    std::vector<std::string> cast;    // "Characters: Tom, Sarrah": who's present (0.59)
    std::string time;                 // "Time: 14:30": the time of day, in the world's calendar's hours (0.75)
};
// `structure`, if given, gets the "Book structure:" line (the structure the book follows, by name).
std::vector<OutlineItem> parseOutline(const std::string& text, std::string* structure = nullptr);
// A chapter's number in its title ("Chapter 12: Ashes", "12. Ashes", "Ch. 12"; 0 if none), and the title without it, in
// lower case ("ashes"; "" if it's only a number) (0.54).
int titleNumber(const std::string& title);
std::string titleCore(const std::string& title);
// What titles are matched by when a plan comes in (0.62): the core without numbers, marks or symbols ("1. 🗡 Steel and
// Snow" and "Steel and Snow" are "steel and snow"), or "#13" for a title that's only a number ("13. Chapter 13").
std::string matchKey(const std::string& title);
std::string outlineText(const Library& lib, const Id& book);
struct OutlineImport {
    int parts = 0, chapters = 0, scenes = 0, threads = 0, beats = 0;   // made
    int updatedParts = 0, updatedChapters = 0, updatedScenes = 0;      // already in the book, filled in (0.54)
    std::vector<Id> updated;
    std::vector<std::string> unknownViewpoints, unknownBeats, unknownPlaces, unknownCast;
    std::string structure;   // the structure set by "Book structure:", by name ("" none)
    bool unknownStructure = false;
    std::vector<Id> made;
    int trashed = 0;                              // empty parts, chapters and scenes the plan doesn't have (0.62)
    std::vector<std::string> keptWithWriting;    // chapters and scenes with writing the plan doesn't have, kept
    // When lines (0.82): dates read in the book's calendar, and those that look like dates but couldn't be read (as
    // "Chapter title: the words"), kept as words for when it happens and not put on the timeline.
    int datesRead = 0;
    std::vector<std::string> unreadDates;
};
// The plan replaces the book's plan (0.62). Parts, chapters and scenes already in the book are matched rather than added
// again: a part by its title, a chapter by its title or its key (matchKey: numbers, "Chapter 3:" and symbols aside),
// the copy with most writing first, or else by its number among the story's chapters (not when the title names
// another chapter), a scene by its title within its chapter. Each takes the plan's synopsis, target, viewpoint,
// place, who's there, time, threads and beats (blank when the plan has none); its status when the plan gives one; its
// title stays. The rest are added. Then everything is put in the plan's order, and what the plan doesn't have goes
// to the Trash when nothing in it is written (`trashed`) and stays when it is (`keptWithWriting`).
// New parts, chapters and scenes take their viewpoint from a character of the book's world (by name or other name),
// their threads (made when new) and their place on the timeline ("Day 3" sets the day; other words the description).
// `scope` is where the author's own structures are kept ("world:<id>" or "book:<id>"), for "Book structure:".
bool importOutline(Library& lib, const Id& book, const std::string& text, OutlineImport& out, const std::string& scope = {});

// ------------------------------------------------------------------ all of it

// One text with any of the three: structures ("Structure:" blocks), the book's outline ("Book structure:", "Part:",
// "Chapter:", "Scene:" or headings) and plot threads ("Thread:" blocks), in any order. Structures are read first
// (one of the same name as the author's own replaces it), then the outline, then the threads (so their chapter
// numbers count the outline's new chapters).
struct PlanningImport {
    OutlineImport outline;
    ThreadImport threads;
    int structuresAdded = 0, structuresReplaced = 0;
    bool bookTypeSet = false;      // "Book type: Novella" (0.76)
    int placements = 0;            // "Series: The Alvora Chronicles, Book 2.5, set during The Envoy" lines applied (0.76)
    std::vector<std::string> unknownSeries;   // series or books a Series: line names that this library doesn't have
    bool any() const;
};
bool importPlanning(Library& lib, const Id& book, const std::string& scope, const std::string& text, PlanningImport& out);
// How well a plan's chapters fit a book (0.82), before importing: the plan's chapters, and how many have the title of
// one of the book's chapters (exactly, or by matchKey; not by number alone), and how many chapters the book has.
struct PlanFit { int planned = 0, matched = 0, bookChapters = 0; };
PlanFit planFit(const Library& lib, const Id& book, const std::string& text);
// The book's whole plan as such a text: the outline (with its beats), its threads, and the structure it follows when
// it's the author's own.
std::string planningText(const Library& lib, const Id& book, const std::string& scope);
// Where the author's own structures for a book are kept.
std::string structureScope(const Library& lib, const Id& book);

}  // namespace studio::plan

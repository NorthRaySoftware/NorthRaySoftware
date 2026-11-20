// NorthRay Capture: overlay designs (Milestone 10, the overlay designer). A design is a page of
// elements (text, rectangles, ellipses, pictures) with fills (a colour or a gradient of any number of
// stops), outlines, rounded corners, shadows and opacity, layered bottom to top. It's a source like any
// other (kind "design"); the app draws it (src/video/design_render) and the designer edits it. This part
// is portable and tested: the document, editing, hit-testing, snapping guides and the templates.
#pragma once

#include "ncap/colour.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ncap::design {

using sourceutil::Rgba;

struct Stop {
    float at = 0;   // 0..1 along the gradient
    Rgba colour;
    std::string brand;   // linked to a brand colour (its key), or 
    bool operator==(const Stop&) const = default;
};

struct Fill {
    enum Type { None, Solid, Linear, Radial } type = Solid;
    Rgba colour{255, 255, 255, 255};   // Solid
    std::vector<Stop> stops;           // Linear and Radial: two or more, in order (Radial: centre to edge)
    float angle = 0;                   // Linear: degrees clockwise from left-to-right
    std::string brand;                 // Solid: linked to a brand colour (its key), or ""
    bool operator==(const Fill&) const = default;
    static Fill solid(Rgba c);
    static Fill linear(std::vector<Stop> s, float angle);
    static Fill radial(std::vector<Stop> s);
    static Fill none();
};
// The colour of a gradient at t (0..1), by its stops.
Rgba colourAt(const Fill& f, float t);

// Editing a gradient's stops on the designer's bar (0.15). Stops stay in order of where they are.
constexpr std::size_t kMaxStops = 16;
// Adds a stop at `at` in the gradient's own colour there (so nothing changes until it's moved or recoloured);
// its index, or -1 when it isn't a gradient or has kMaxStops already.
int addStop(Fill& f, float at);
// Moves a stop (clamped to 0..1); its index afterwards (it may pass others).
int moveStop(Fill& f, int index, float at);
// Removes a stop; never fewer than two.
bool removeStop(Fill& f, int index);
// The stop nearest t within `within`, or -1.
int stopNear(const Fill& f, float t, float within);

struct Shadow {
    bool on = false;
    float dx = 0, dy = 6, blur = 12;   // pixels
    Rgba colour{0, 0, 0, 140};
    std::string brand;   // linked to a brand colour, or ""
    bool operator==(const Shadow&) const = default;
};

// A glow (0.15): light round the element's shape (text, an outline, a shape or a picture's outline), with no
// offset, drawn under it. Strength above 1 spreads it further and brighter, below 1 makes it fainter.
struct Glow {
    bool on = false;
    float size = 18;        // how far it reaches, pixels
    float strength = 1.5f;  // 0.25..4
    Rgba colour{63, 207, 142, 230};
    std::string brand;      // linked to a brand colour, or ""
    bool operator==(const Glow&) const = default;
};
// The glow's coverage from a blurred shape's coverage (both 0..1): 1 - (1 - a)^strength, so it never goes over 1
// and strength 1 is the blur as it is.
float glowLevel(float coverage, float strength);

// A text outline (0.15), grown from the letters' own coverage (w x h, 0..255): every pixel within `radius` of the
// letters (distance to the nearest pixel at least half covered, exact, by a two-pass distance transform), with a
// one-pixel soft edge. The letters themselves stay fully covered. Wrong sizes give the coverage back unchanged.
std::vector<std::uint8_t> grow(const std::vector<std::uint8_t>& coverage, int w, int h, float radius);

// Timer, Qr and Icon draw like text or shapes: a timer is text that counts, a QR code is squares from a
// link, an icon is one of the built-in drawings (icons()).
enum class Kind { Text, Rect, Ellipse, Image, Timer, Qr, Icon, Chat };
const char* key(Kind k);
bool parse(const std::string& s, Kind& out);

enum class Anim { None, Fade, SlideLeft, SlideRight, SlideUp, SlideDown, Pop, Wipe };
const char* key(Anim a);
bool parse(const std::string& s, Anim& out);

struct Element {
    std::string id;
    std::string name;
    Kind kind = Kind::Rect;
    float x = 0, y = 0, w = 100, h = 100;   // pixels on the design's page
    float rotation = 0;                      // degrees, about the centre
    float opacity = 1;
    bool visible = true, locked = false;
    Fill fill;                               // shapes: the inside; text: the letters
    Rgba stroke{0, 0, 0, 0};                 // outline (alpha 0: none)
    std::string strokeBrand;                 // the outline linked to a brand colour, or ""
    float strokeWidth = 0;
    float radius = 0;                        // rectangles' corners; pictures' corners
    Shadow shadow;
    Glow glow;
    // Text (and a timer's look). A gradient on text runs across the words themselves, not the whole box (0.15).
    std::string text, font = "Segoe UI";
    float size = 64;
    bool bold = false, italic = false;
    std::string align = "left";              // left, centre, right
    // Picture.
    std::string file;                        // a path (or a library file)
    bool cover = true;                       // fill the box (cropping) or fit inside it
    // Timer.
    std::string timerMode = "countdown";     // countdown (for a length), until (a time of day), stopwatch, live (time since going live), clock,
                                             // golive (to the time set to go live), remaining (the planned length less the time live),
                                             // next (the next stream's day and time, as words) (0.11)
    int timerSeconds = 300;                  // countdown
    int timerUntil = 20 * 60;                // until: minutes after midnight
    std::string timerEnd = "Starting now!";  // shown at zero (countdown, until)
    bool timerDate = false;                  // clock: with the day and date
    bool timerPulse = true;                  // the last ten seconds pulse
    std::string timerScene;                  // at zero, switch to this scene (its id), if set
    // QR code: its text or link. Icon: its name.
    std::string qr, icon;
    // Chat (live Twitch chat; the text look is the element's font, size and fill).
    int chatCount = 8;                       // messages shown at most
    int chatFade = 0;                        // seconds a message stays (0: until pushed out)
    bool chatBubbles = true;                 // each message on a rounded card
    Rgba chatBubble{11, 26, 46, 210};        // the cards' colour
    std::string chatBubbleBrand = "panel";   // the cards linked to a brand colour, or ""
    bool chatNameColours = true;             // names in each viewer's own colour (else the main colour)
    // Where the messages sit: "left", "right", or "mine" (your own on the right, viewers' on the left, like a phone's
    // messages), each sized to its message with the text starting at its left. "": as the Align setting (before 0.10.27).
    std::string chatSide;
    // Elements grouped together (0.14) share a group id: they're picked, moved and hidden as one. "" = none.
    std::string group;
    // Entrance (when its scene is shown).
    Anim anim = Anim::None;
    int animMs = 500;
    bool operator==(const Element&) const = default;
};

struct Document {
    std::string name = "Design";
    int width = 1920, height = 1080;
    Fill background = Fill::none();
    bool onlyWhilePlaying = false;   // drawn only while music plays ({song} has something)
    bool onlyWhileSprint = false;    // drawn only while a writing sprint runs in Writing Studio (0.13)
    bool onlyCelebrating = false;    // drawn only for a few seconds when a celebration fires, coming in afresh (0.13)
    // The designer's grid (0 off; its spacing in pixels) and the guides placed from the rulers (page pixels) (0.14).
    int grid = 0;
    std::vector<float> guidesX, guidesY;
    std::vector<Element> elements;   // bottom first
    int nextId = 1;
    const Element* find(const std::string& id) const;
    Element* find(const std::string& id);
    int indexOf(const std::string& id) const;   // -1 if absent
};

// The brand kit: colours, fonts and a logo kept in one place (shared by the NorthRay apps in
// %LOCALAPPDATA%\NorthRay\brand.json). Designs link to it: a fill, gradient stop, outline or shadow can
// name a brand colour (by key), a text's font can be "brand:heading" or "brand:body", and a picture's
// file "brand:logo". Changing the kit changes every linked design. Linked colours keep their own
// see-through amount and take the brand colour's red, green and blue.
struct BrandColour {
    std::string key, name;
    Rgba colour;
    bool operator==(const BrandColour&) const = default;
};
struct Brand {
    std::vector<BrandColour> colours;          // always brandKeys(), in order
    std::string headingFont = "Segoe UI", bodyFont = "Segoe UI";
    std::vector<std::string> fontFiles;        // font files (.ttf, .otf) the designs can use, up to 16
    std::string logo;                          // a picture
    // Videos: a transition stinger (the scene switches `stingerCutMs` in) and an intro and outro to play
    // when their scenes go live. Files copied into the kit's folder; "" = not set.
    std::string stinger, intro, outro;
    int stingerCutMs = 500;                    // 0-10000
    bool operator==(const Brand&) const = default;
};
const std::vector<std::string>& brandKeys();   // primary, secondary, text, soft, panel, night
Brand defaultBrand();                           // the NorthRay aurora colours and Segoe UI
std::string toJson(const Brand& b);
Brand brandFromJson(const std::string& json);   // missing or bad parts take the defaults
const BrandColour* brandColour(const Brand& b, const std::string& key);
constexpr const char* kBrandHeading = "brand:heading";
constexpr const char* kBrandBody = "brand:body";
constexpr const char* kBrandLogo = "brand:logo";
// A copy of the design with every link replaced by the kit's value (unknown keys are left as stored).
Document applyBrand(const Document& d, const Brand& b);
// Links what matches the kit: colours with a brand colour's red, green and blue, and Segoe UI text
// (bold to the heading font, the rest to the body font). The templates are linked this way.
void linkToBrand(Document& d, const Brand& b);
bool usesBrand(const Document& d);
// Removes every link, keeping what's shown now (for a design that should stop following the kit).
Document unlinkBrand(const Document& d, const Brand& b);

std::string toJson(const Document& d);
Document fromJson(const std::string& json);   // bad parts dropped, values clamped (up to 200 elements)

// Editing. New elements get unique ids ("e1", "e2"...) and go on top.
std::string add(Document& d, Element e);
bool remove(Document& d, const std::string& id);
std::string duplicate(Document& d, const std::string& id);   // offset by 20 px
bool move(Document& d, const std::string& id, int delta);    // up (+1) or down (-1) a layer
bool toTop(Document& d, const std::string& id);
bool toBottom(Document& d, const std::string& id);

// The topmost visible, unlocked element under (x, y), honouring rotation; "" if none.
std::string hitTest(const Document& d, float x, float y);

// Snapping while moving or resizing: the box is nudged so its edges or centre meet the page's edges
// and centre or another element's, within `threshold` pixels; the guides that matched are returned.
struct Box {
    float x = 0, y = 0, w = 0, h = 0;
};
struct Snapped {
    Box box;
    std::vector<float> guidesX, guidesY;   // page coordinates of the lines to show
};
Snapped snap(const Document& d, const std::string& movingId, Box proposed, float threshold, bool resizing = false);
// Several moving at once (their joint box): none of them is a target. Snapping includes the page's guides; with a grid,
// an edge that met nothing else snaps to the grid.
Snapped snap(const Document& d, const std::vector<std::string>& moving, Box proposed, float threshold, bool resizing = false);

// Several elements (0.14).
Box boundsOf(const Document& d, const std::vector<std::string>& ids);   // their joint box (unrotated)
// Groups: the ids are given a new group id (returned) and gathered just below the topmost of them, in their order.
// Ungrouping clears it. `members` lists a group's elements bottom first; `pick` is what a click on `id` selects: its
// group's members, or itself.
std::string group(Document& d, const std::vector<std::string>& ids);
void ungroup(Document& d, const std::string& groupId);
std::vector<std::string> members(const Document& d, const std::string& groupId);
std::vector<std::string> pick(const Document& d, const std::string& id);
// Lining up: left, centre, right, top, middle, bottom: to their joint box (or the page when `toPage` or only one).
bool align(Document& d, const std::vector<std::string>& ids, const std::string& how, bool toPage = false);
// Spacing evenly across (or down): the first and last stay, the gaps between all become equal. Needs three or more.
bool distribute(Document& d, const std::vector<std::string>& ids, bool across);

// The entrance animation at time t (ms since shown): how far it's in (0..1), offsets and scale.
struct Pose {
    float opacity = 1, dx = 0, dy = 0, scale = 1, reveal = 1;   // reveal: Wipe's visible fraction left to right
};
Pose pose(const Element& e, float tMs, int pageW, int pageH);
bool animating(const Document& d, float tMs);   // any element still coming in
int entranceMs(const Document& d);               // how long the visible elements take to come in (0: none)

// What a timer shows now. `elapsedMs`: since its scene went live (countdown, stopwatch); `localSeconds`:
// seconds since local midnight; `dateText`: today ("Sun 15 Nov"); `liveMs`: since streaming began (-1 if not).
struct Clock {
    std::int64_t elapsedMs = 0;
    int localSeconds = 0;
    std::string dateText;
    std::int64_t liveMs = -1;
    std::int64_t goLiveInMs = -1;   // until the time set to go live (-1: none set)
    std::int64_t plannedMs = -1;    // how long the stream is meant to be (-1: not set)
    std::string nextText;           // the next stream, as words ("Sat 22 Nov, 19:00"; "" not set)
    std::int64_t sprintLeftMs = -1;  // a writing sprint in Writing Studio: time left (-1: none running) (0.13)
    std::int64_t celebrateMs = -1;   // since a celebration began (-1: none lately) (0.13)
};
constexpr std::int64_t kCelebrationMs = 7000;   // how long a celebration design shows
struct TimerShown {
    std::string text;
    bool ended = false;     // a countdown at zero
    float pulse = 1;        // scale for the last ten seconds (1 otherwise)
    std::int64_t remainingMs = -1;
};
TimerShown timerText(const Element& e, const Clock& c);
std::string durationText(std::int64_t ms);   // "4:59", "1:02:03" (rounded up for countdowns by the caller)
bool usesTime(const Document& d);   // timers, fading chat, or shown only during sprints or celebrations
// Whether a design shows anything now: one shown only during sprints or celebrations is hidden otherwise.
bool shownNow(const Document& d, const Clock& c);
bool usesChat(const Document& d);   // has a visible chat element            // anything that changes every second (timers)

// QR codes (via Project Nayuki's library): the modules, row by row, true = dark; empty if too long.
std::vector<std::vector<bool>> qrModules(const std::string& text);

// Built-in icons: drawn in a 24 x 24 box from simple paths ("M x y L x y C x1 y1 x2 y2 x y Z", and
// "O cx cy r" for a circle); filled with the element's fill.
struct Icon {
    const char* id;
    const char* name;
    const char* path;
};
const std::vector<Icon>& icons();
const Icon* iconById(const std::string& id);
// The path as figures of points (curves flattened), for drawing and for the tests.
std::vector<std::vector<std::pair<float, float>>> iconFigures(const std::string& path);

// Ready-made designs in the NorthRay aurora look. `id`s are stable.
struct Template {
    const char* id;
    const char* name;
    const char* group;   // "Holding screens", "On stream", "Panels"
    Document doc;
};
const std::vector<Template>& templates();
// Writing progress overlays made before the chapter's word count (0.10.28): adds it on the CHAPTER label's line,
// styled like the label. False (and unchanged) if it's another design, already has one, or has no such label.
bool addChapterWords(Document& d);
// Social handles and the like put into a template's text: "{name}" and "{twitch}" etc. On a line made of
// parts between "·", parts left empty are dropped with their dots (no "  ·  ·" when one isn't set).
// `showMissing` keeps an empty one as its "{tag}" instead (the designer, so it can be seen).
std::string fillPlaceholders(const std::string& text, const std::vector<std::pair<std::string, std::string>>& values, bool showMissing = false);

}  // namespace ncap::design

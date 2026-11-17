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

struct Shadow {
    bool on = false;
    float dx = 0, dy = 6, blur = 12;   // pixels
    Rgba colour{0, 0, 0, 140};
    std::string brand;   // linked to a brand colour, or ""
    bool operator==(const Shadow&) const = default;
};

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
    // Text (and a timer's look).
    std::string text, font = "Segoe UI";
    float size = 64;
    bool bold = false, italic = false;
    std::string align = "left";              // left, centre, right
    // Picture.
    std::string file;                        // a path (or a library file)
    bool cover = true;                       // fill the box (cropping) or fit inside it
    // Timer.
    std::string timerMode = "countdown";     // countdown (for a length), until (a time of day), stopwatch, live (time since going live), clock
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
};
struct TimerShown {
    std::string text;
    bool ended = false;     // a countdown at zero
    float pulse = 1;        // scale for the last ten seconds (1 otherwise)
    std::int64_t remainingMs = -1;
};
TimerShown timerText(const Element& e, const Clock& c);
std::string durationText(std::int64_t ms);   // "4:59", "1:02:03" (rounded up for countdowns by the caller)
bool usesTime(const Document& d);
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
// Social handles and the like put into a template's text: "{name}" and "{twitch}" etc. On a line made of
// parts between "·", parts left empty are dropped with their dots (no "  ·  ·" when one isn't set).
// `showMissing` keeps an empty one as its "{tag}" instead (the designer, so it can be seen).
std::string fillPlaceholders(const std::string& text, const std::vector<std::pair<std::string, std::string>>& values, bool showMissing = false);

}  // namespace ncap::design

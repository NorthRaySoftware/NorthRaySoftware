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
    bool operator==(const Stop&) const = default;
};

struct Fill {
    enum Type { None, Solid, Linear, Radial } type = Solid;
    Rgba colour{255, 255, 255, 255};   // Solid
    std::vector<Stop> stops;           // Linear and Radial: two or more, in order (Radial: centre to edge)
    float angle = 0;                   // Linear: degrees clockwise from left-to-right
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
    bool operator==(const Shadow&) const = default;
};

enum class Kind { Text, Rect, Ellipse, Image };
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
    float strokeWidth = 0;
    float radius = 0;                        // rectangles' corners; pictures' corners
    Shadow shadow;
    // Text.
    std::string text, font = "Segoe UI";
    float size = 64;
    bool bold = false, italic = false;
    std::string align = "left";              // left, centre, right
    // Picture.
    std::string file;                        // a path (or a library file)
    bool cover = true;                       // fill the box (cropping) or fit inside it
    // Entrance (when its scene is shown).
    Anim anim = Anim::None;
    int animMs = 500;
    bool operator==(const Element&) const = default;
};

struct Document {
    std::string name = "Design";
    int width = 1920, height = 1080;
    Fill background = Fill::none();
    std::vector<Element> elements;   // bottom first
    int nextId = 1;
    const Element* find(const std::string& id) const;
    Element* find(const std::string& id);
    int indexOf(const std::string& id) const;   // -1 if absent
};

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

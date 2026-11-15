#include "ncap/design.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace ncap::design {

using nlohmann::json;
using sourceutil::colourText;
using sourceutil::parseColour;

namespace {
constexpr float kPi = 3.14159265358979f;
const char* kKinds[] = {"text", "rect", "ellipse", "image"};
const char* kAnims[] = {"none", "fade", "slide-left", "slide-right", "slide-up", "slide-down", "pop", "wipe"};
float clampf(float v, float a, float b) { return std::isfinite(v) ? std::clamp(v, a, b) : a; }
std::uint8_t mix8(std::uint8_t a, std::uint8_t b, float t) { return (std::uint8_t)std::lround(a + (b - a) * t); }

json colourJson(const Rgba& c) { return colourText(c); }
Rgba colourFrom(const json& j, Rgba def) {
    Rgba c;
    return j.is_string() && parseColour(j.get<std::string>(), c) ? c : def;
}

json fillJson(const Fill& f) {
    json j = {{"type", f.type == Fill::None ? "none" : f.type == Fill::Solid ? "solid" : f.type == Fill::Linear ? "linear" : "radial"}};
    if (f.type == Fill::Solid) j["colour"] = colourJson(f.colour);
    if (f.type == Fill::Linear || f.type == Fill::Radial) {
        json s = json::array();
        for (auto& st : f.stops) s.push_back({{"at", st.at}, {"colour", colourJson(st.colour)}});
        j["stops"] = s;
        j["angle"] = f.angle;
    }
    return j;
}

Fill fillFrom(const json& j, const Fill& def) {
    if (!j.is_object()) return def;
    const std::string t = j.value("type", std::string());
    if (t == "none") return Fill::none();
    if (t == "solid") return Fill::solid(colourFrom(j.contains("colour") ? j["colour"] : json(), def.colour));
    if (t == "linear" || t == "radial") {
        std::vector<Stop> stops;
        if (j.contains("stops") && j["stops"].is_array())
            for (auto& s : j["stops"]) {
                if (!s.is_object() || stops.size() >= 16) continue;
                Stop st;
                st.at = s.contains("at") && s["at"].is_number() ? clampf(s["at"].get<float>(), 0, 1) : 0;
                st.colour = colourFrom(s.contains("colour") ? s["colour"] : json(), {255, 255, 255, 255});
                stops.push_back(st);
            }
        if (stops.size() < 2) return def;
        std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.at < b.at; });
        const float angle = j.contains("angle") && j["angle"].is_number() ? clampf(j["angle"].get<float>(), -360, 360) : 0;
        Fill f = Fill::linear(stops, angle);
        if (t == "radial") f.type = Fill::Radial;
        return f;
    }
    return def;
}

void corners(const Element& e, float out[8]) {
    const float cx = e.x + e.w / 2, cy = e.y + e.h / 2;
    const float a = e.rotation * kPi / 180, c = std::cos(a), s = std::sin(a);
    const float px[4] = {-e.w / 2, e.w / 2, e.w / 2, -e.w / 2}, py[4] = {-e.h / 2, -e.h / 2, e.h / 2, e.h / 2};
    for (int i = 0; i < 4; ++i) {
        out[i * 2] = cx + px[i] * c - py[i] * s;
        out[i * 2 + 1] = cy + px[i] * s + py[i] * c;
    }
}
}  // namespace

Fill Fill::solid(Rgba c) {
    Fill f;
    f.type = Solid;
    f.colour = c;
    return f;
}
Fill Fill::linear(std::vector<Stop> s, float angle) {
    Fill f;
    f.type = Linear;
    f.stops = std::move(s);
    f.angle = angle;
    return f;
}
Fill Fill::radial(std::vector<Stop> s) {
    Fill f = linear(std::move(s), 0);
    f.type = Radial;
    return f;
}
Fill Fill::none() {
    Fill f;
    f.type = None;
    return f;
}

Rgba colourAt(const Fill& f, float t) {
    if (f.type == Fill::Solid) return f.colour;
    if ((f.type != Fill::Linear && f.type != Fill::Radial) || f.stops.empty()) return {0, 0, 0, 0};
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= f.stops.front().at) return f.stops.front().colour;
    if (t >= f.stops.back().at) return f.stops.back().colour;
    for (std::size_t i = 1; i < f.stops.size(); ++i)
        if (t <= f.stops[i].at) {
            const Stop &a = f.stops[i - 1], &b = f.stops[i];
            const float u = b.at > a.at ? (t - a.at) / (b.at - a.at) : 1.0f;
            return {mix8(a.colour.r, b.colour.r, u), mix8(a.colour.g, b.colour.g, u), mix8(a.colour.b, b.colour.b, u), mix8(a.colour.a, b.colour.a, u)};
        }
    return f.stops.back().colour;
}

const char* key(Kind k) { return kKinds[(int)k]; }
bool parse(const std::string& s, Kind& out) {
    for (int i = 0; i < 4; ++i)
        if (s == kKinds[i]) {
            out = (Kind)i;
            return true;
        }
    return false;
}
const char* key(Anim a) { return kAnims[(int)a]; }
bool parse(const std::string& s, Anim& out) {
    for (int i = 0; i < 8; ++i)
        if (s == kAnims[i]) {
            out = (Anim)i;
            return true;
        }
    return false;
}

const Element* Document::find(const std::string& id) const {
    for (auto& e : elements)
        if (e.id == id) return &e;
    return nullptr;
}
Element* Document::find(const std::string& id) {
    for (auto& e : elements)
        if (e.id == id) return &e;
    return nullptr;
}
int Document::indexOf(const std::string& id) const {
    for (std::size_t i = 0; i < elements.size(); ++i)
        if (elements[i].id == id) return (int)i;
    return -1;
}

std::string toJson(const Document& d) {
    json els = json::array();
    for (auto& e : d.elements) {
        json j = {{"id", e.id},
                  {"name", e.name},
                  {"kind", key(e.kind)},
                  {"x", e.x},
                  {"y", e.y},
                  {"w", e.w},
                  {"h", e.h},
                  {"rotation", e.rotation},
                  {"opacity", e.opacity},
                  {"visible", e.visible},
                  {"locked", e.locked},
                  {"fill", fillJson(e.fill)},
                  {"stroke", colourJson(e.stroke)},
                  {"strokeWidth", e.strokeWidth},
                  {"radius", e.radius},
                  {"shadow", {{"on", e.shadow.on}, {"dx", e.shadow.dx}, {"dy", e.shadow.dy}, {"blur", e.shadow.blur}, {"colour", colourJson(e.shadow.colour)}}},
                  {"anim", key(e.anim)},
                  {"animMs", e.animMs}};
        if (e.kind == Kind::Text) {
            j["text"] = e.text;
            j["font"] = e.font;
            j["size"] = e.size;
            j["bold"] = e.bold;
            j["italic"] = e.italic;
            j["align"] = e.align;
        }
        if (e.kind == Kind::Image) {
            j["file"] = e.file;
            j["cover"] = e.cover;
        }
        els.push_back(j);
    }
    return json{{"format", "northray-capture-design"}, {"version", 1}, {"name", d.name}, {"width", d.width}, {"height", d.height},
                {"background", fillJson(d.background)}, {"elements", els}, {"nextId", d.nextId}}
        .dump();
}

Document fromJson(const std::string& text) {
    Document d;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return d;
    auto num = [](const json& o, const char* k, float def, float lo, float hi) {
        return o.contains(k) && o[k].is_number() ? clampf(o[k].get<float>(), lo, hi) : def;
    };
    auto flag = [](const json& o, const char* k, bool def) { return o.contains(k) && o[k].is_boolean() ? o[k].get<bool>() : def; };
    auto str = [](const json& o, const char* k, const std::string& def, std::size_t max) {
        return o.contains(k) && o[k].is_string() ? o[k].get<std::string>().substr(0, max) : def;
    };
    d.name = str(j, "name", d.name, 80);
    d.width = (int)num(j, "width", 1920, 16, 8192);
    d.height = (int)num(j, "height", 1080, 16, 8192);
    if (j.contains("background")) d.background = fillFrom(j["background"], Fill::none());
    int maxId = 0;
    if (j.contains("elements") && j["elements"].is_array())
        for (auto& o : j["elements"]) {
            if (!o.is_object() || d.elements.size() >= 200) continue;
            Element e;
            if (!parse(str(o, "kind", "", 16), e.kind)) continue;
            e.id = str(o, "id", "", 32);
            if (e.id.empty() || d.find(e.id)) continue;
            if (e.id.size() > 1 && e.id[0] == 'e') maxId = std::max(maxId, std::atoi(e.id.c_str() + 1));
            e.name = str(o, "name", "", 80);
            e.x = num(o, "x", 0, -20000, 20000);
            e.y = num(o, "y", 0, -20000, 20000);
            e.w = num(o, "w", 100, 1, 20000);
            e.h = num(o, "h", 100, 1, 20000);
            e.rotation = num(o, "rotation", 0, -360, 360);
            e.opacity = num(o, "opacity", 1, 0, 1);
            e.visible = flag(o, "visible", true);
            e.locked = flag(o, "locked", false);
            e.fill = fillFrom(o.contains("fill") ? o["fill"] : json(), e.kind == Kind::Image ? Fill::none() : Fill::solid({255, 255, 255, 255}));
            e.stroke = colourFrom(o.contains("stroke") ? o["stroke"] : json(), {0, 0, 0, 0});
            e.strokeWidth = num(o, "strokeWidth", 0, 0, 200);
            e.radius = num(o, "radius", 0, 0, 10000);
            if (o.contains("shadow") && o["shadow"].is_object()) {
                const json& s = o["shadow"];
                e.shadow.on = flag(s, "on", false);
                e.shadow.dx = num(s, "dx", 0, -200, 200);
                e.shadow.dy = num(s, "dy", 6, -200, 200);
                e.shadow.blur = num(s, "blur", 12, 0, 100);
                e.shadow.colour = colourFrom(s.contains("colour") ? s["colour"] : json(), {0, 0, 0, 140});
            }
            parse(str(o, "anim", "none", 16), e.anim);
            e.animMs = (int)num(o, "animMs", 500, 0, 5000);
            e.text = str(o, "text", "", 2000);
            e.font = str(o, "font", "Segoe UI", 64);
            e.size = num(o, "size", 64, 4, 1000);
            e.bold = flag(o, "bold", false);
            e.italic = flag(o, "italic", false);
            e.align = str(o, "align", "left", 8);
            if (e.align != "left" && e.align != "centre" && e.align != "right") e.align = "left";
            e.file = str(o, "file", "", 1024);
            e.cover = flag(o, "cover", true);
            d.elements.push_back(e);
        }
    d.nextId = std::max((int)num(j, "nextId", 1, 1, 1000000), maxId + 1);
    return d;
}

std::string add(Document& d, Element e) {
    if (d.elements.size() >= 200) return {};
    e.id = "e" + std::to_string(d.nextId++);
    while (d.find(e.id)) e.id = "e" + std::to_string(d.nextId++);
    if (e.name.empty()) e.name = e.kind == Kind::Text ? "Text" : e.kind == Kind::Rect ? "Rectangle" : e.kind == Kind::Ellipse ? "Ellipse" : "Picture";
    d.elements.push_back(e);
    return d.elements.back().id;
}

bool remove(Document& d, const std::string& id) {
    const int i = d.indexOf(id);
    if (i < 0) return false;
    d.elements.erase(d.elements.begin() + i);
    return true;
}

std::string duplicate(Document& d, const std::string& id) {
    const Element* e = d.find(id);
    if (!e) return {};
    Element c = *e;
    c.x += 20;
    c.y += 20;
    c.name += " copy";
    const int at = d.indexOf(id);
    const std::string nid = add(d, c);
    if (nid.empty()) return {};
    // Just above the original.
    Element moved = d.elements.back();
    d.elements.pop_back();
    d.elements.insert(d.elements.begin() + at + 1, moved);
    return nid;
}

bool move(Document& d, const std::string& id, int delta) {
    const int i = d.indexOf(id), j = i + delta;
    if (i < 0 || j < 0 || j >= (int)d.elements.size()) return false;
    std::swap(d.elements[(std::size_t)i], d.elements[(std::size_t)j]);
    return true;
}
bool toTop(Document& d, const std::string& id) {
    const int i = d.indexOf(id);
    if (i < 0 || i + 1 == (int)d.elements.size()) return false;
    Element e = d.elements[(std::size_t)i];
    d.elements.erase(d.elements.begin() + i);
    d.elements.push_back(e);
    return true;
}
bool toBottom(Document& d, const std::string& id) {
    const int i = d.indexOf(id);
    if (i <= 0) return false;
    Element e = d.elements[(std::size_t)i];
    d.elements.erase(d.elements.begin() + i);
    d.elements.insert(d.elements.begin(), e);
    return true;
}

std::string hitTest(const Document& d, float x, float y) {
    for (auto it = d.elements.rbegin(); it != d.elements.rend(); ++it) {
        const Element& e = *it;
        if (!e.visible || e.locked) continue;
        const float cx = e.x + e.w / 2, cy = e.y + e.h / 2;
        const float a = -e.rotation * kPi / 180;
        const float lx = (x - cx) * std::cos(a) - (y - cy) * std::sin(a), ly = (x - cx) * std::sin(a) + (y - cy) * std::cos(a);
        if (std::fabs(lx) > e.w / 2 || std::fabs(ly) > e.h / 2) continue;
        if (e.kind == Kind::Ellipse && (lx * lx) / (e.w * e.w / 4) + (ly * ly) / (e.h * e.h / 4) > 1) continue;
        return e.id;
    }
    return {};
}

Snapped snap(const Document& d, const std::string& movingId, Box p, float threshold, bool resizing) {
    std::vector<float> xs = {0, d.width / 2.0f, (float)d.width}, ys = {0, d.height / 2.0f, (float)d.height};
    for (auto& e : d.elements) {
        if (e.id == movingId || !e.visible || std::fabs(e.rotation) > 0.01f) continue;
        xs.insert(xs.end(), {e.x, e.x + e.w / 2, e.x + e.w});
        ys.insert(ys.end(), {e.y, e.y + e.h / 2, e.y + e.h});
    }
    Snapped out;
    auto best = [&](const std::vector<float>& lines, const std::vector<float>& mine, float& delta, float& line) {
        bool found = false;
        float bd = threshold + 1;
        for (float l : lines)
            for (float m : mine)
                if (std::fabs(l - m) <= threshold && std::fabs(l - m) < bd) {
                    bd = std::fabs(l - m);
                    delta = l - m;
                    line = l;
                    found = true;
                }
        return found;
    };
    float dx = 0, dy = 0, lx = 0, ly = 0;
    const std::vector<float> mx = resizing ? std::vector<float>{p.x + p.w} : std::vector<float>{p.x, p.x + p.w / 2, p.x + p.w};
    const std::vector<float> my = resizing ? std::vector<float>{p.y + p.h} : std::vector<float>{p.y, p.y + p.h / 2, p.y + p.h};
    if (best(xs, mx, dx, lx)) {
        if (resizing) p.w = std::max(1.0f, p.w + dx);
        else p.x += dx;
        out.guidesX.push_back(lx);
    }
    if (best(ys, my, dy, ly)) {
        if (resizing) p.h = std::max(1.0f, p.h + dy);
        else p.y += dy;
        out.guidesY.push_back(ly);
    }
    out.box = p;
    return out;
}

Pose pose(const Element& e, float t, int pageW, int pageH) {
    Pose p;
    if (e.anim == Anim::None || e.animMs <= 0 || t >= e.animMs) return p;
    const float u = std::clamp(t / (float)e.animMs, 0.0f, 1.0f);
    const float k = 1 - (1 - u) * (1 - u) * (1 - u);   // ease out
    switch (e.anim) {
        case Anim::None: break;
        case Anim::Fade: p.opacity = k; break;
        case Anim::SlideLeft: p.dx = -(e.x + e.w) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;          // in from the left edge
        case Anim::SlideRight: p.dx = (pageW - e.x) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;        // from the right
        case Anim::SlideUp: p.dy = (pageH - e.y) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;           // up from the bottom
        case Anim::SlideDown: p.dy = -(e.y + e.h) * (1 - k); p.opacity = std::min(1.0f, u * 3); break;          // down from the top
        case Anim::Pop: {
            // Grows past full size a little, then settles.
            const float s = 1.70158f, v = u - 1;
            p.scale = std::max(0.0f, 1 + (s + 1) * v * v * v + s * v * v);
            p.opacity = std::min(1.0f, u * 4);
            break;
        }
        case Anim::Wipe: p.reveal = k; break;
    }
    return p;
}

bool animating(const Document& d, float t) {
    for (auto& e : d.elements)
        if (e.visible && e.anim != Anim::None && t < e.animMs) return true;
    return false;
}

std::string fillPlaceholders(const std::string& text, const std::vector<std::pair<std::string, std::string>>& values, bool showMissing) {
    auto fill = [&](std::string part, bool& emptied) {
        bool hadTag = false, allEmpty = true;
        for (auto& [k, v] : values) {
            const std::string tag = "{" + k + "}";
            for (std::size_t at = 0; (at = part.find(tag, at)) != std::string::npos;) {
                hadTag = true;
                const std::string with = v.empty() && showMissing ? tag : v;
                if (!v.empty()) allEmpty = false;
                part.replace(at, tag.size(), with);
                at += with.size();
            }
        }
        std::string trimmed = part;
        trimmed.erase(0, trimmed.find_first_not_of(' '));
        trimmed.erase(trimmed.find_last_not_of(' ') + 1);
        emptied = hadTag && allEmpty && !showMissing && trimmed.empty();
        return part;
    };
    const std::string dot = "·";
    std::string out;
    std::size_t start = 0;
    while (true) {   // line by line
        const std::size_t nl = text.find('\n', start);
        const std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (line.find(dot) == std::string::npos) {
            bool e = false;
            out += fill(line, e);
        } else {
            std::vector<std::string> kept;
            std::size_t s = 0;
            while (true) {
                const std::size_t d = line.find(dot, s);
                std::string part = line.substr(s, d == std::string::npos ? std::string::npos : d - s);
                bool emptied = false;
                part = fill(part, emptied);
                part.erase(0, part.find_first_not_of(' '));
                part.erase(part.find_last_not_of(' ') + 1);
                if (!emptied && !part.empty()) kept.push_back(part);
                if (d == std::string::npos) break;
                s = d + dot.size();
            }
            for (std::size_t i = 0; i < kept.size(); ++i) out += (i ? "   " + dot + "   " : "") + kept[i];
        }
        if (nl == std::string::npos) break;
        out += '\n';
        start = nl + 1;
    }
    return out;
}

// ------------------------------------------------------------------ templates

namespace {
Rgba hex(const char* h) {
    Rgba c;
    parseColour(h, c);
    return c;
}
Element textEl(const char* name, const char* text, float x, float y, float w, float h, float size, const char* colour, bool bold,
               const char* align = "centre") {
    Element e;
    e.kind = Kind::Text;
    e.name = name;
    e.text = text;
    e.x = x, e.y = y, e.w = w, e.h = h;
    e.size = size;
    e.bold = bold;
    e.align = align;
    e.font = "Segoe UI";
    e.fill = Fill::solid(hex(colour));
    return e;
}
Element rectEl(const char* name, float x, float y, float w, float h, Fill f, float radius = 0) {
    Element e;
    e.kind = Kind::Rect;
    e.name = name;
    e.x = x, e.y = y, e.w = w, e.h = h;
    e.fill = f;
    e.radius = radius;
    return e;
}
Element ellipseEl(const char* name, float x, float y, float w, float h, Fill f) {
    Element e = rectEl(name, x, y, w, h, f);
    e.kind = Kind::Ellipse;
    return e;
}
// The NorthRay night with aurora light: a deep gradient and two soft glows.
void auroraPage(Document& d) {
    d.background = Fill::linear({{0, hex("#060B17")}, {0.55f, hex("#0B1A2E")}, {1, hex("#0E2A33")}}, 90);
    add(d, ellipseEl("Aurora (green)", -0.15f * d.width, -0.45f * d.height, 0.85f * d.width, 0.95f * d.height,
                     Fill::radial({{0, hex("#3FCF8E55")}, {0.55f, hex("#3FCF8E22")}, {1, hex("#3FCF8E00")}})));
    add(d, ellipseEl("Aurora (violet)", 0.4f * d.width, -0.35f * d.height, 0.75f * d.width, 0.8f * d.height,
                     Fill::radial({{0, hex("#7A5CFF4A")}, {0.6f, hex("#7A5CFF1C")}, {1, hex("#7A5CFF00")}})));
}
Document holding(const char* name, const char* title, const char* sub) {
    Document d;
    d.name = name;
    auroraPage(d);
    Element t = textEl("Title", title, 160, 380, 1600, 200, 132, "#F4F7FB", true);
    t.shadow = {true, 0, 8, 24, hex("#00000099")};
    t.anim = Anim::Pop;
    add(d, t);
    Element s = textEl("Subtitle", sub, 260, 580, 1400, 80, 44, "#9FB3C8", false);
    s.anim = Anim::Fade;
    s.animMs = 900;
    add(d, s);
    Element bar = rectEl("Accent line", 810, 690, 300, 6, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0), 3);
    bar.anim = Anim::Wipe;
    bar.animMs = 800;
    add(d, bar);
    Element soc = textEl("Socials", "{twitch}   ·   {youtube}   ·   {instagram}", 260, 900, 1400, 60, 34, "#CFE3F0", false);
    add(d, soc);
    return d;
}
}  // namespace

const std::vector<Template>& templates() {
    static const std::vector<Template> t = [] {
        std::vector<Template> v;
        v.push_back({"starting-soon", "Starting Soon", "Holding screens", holding("Starting Soon", "Starting soon", "Grab a drink, we'll be live in a moment")});
        v.push_back({"be-right-back", "Be Right Back", "Holding screens", holding("Be Right Back", "Be right back", "Don't go anywhere")});
        v.push_back({"ending", "Ending", "Holding screens", holding("Ending", "Thanks for watching", "See you next stream")});
        {
            Document d;
            d.name = "Lower third";
            d.width = 1000;
            d.height = 180;
            Element bg = rectEl("Bar", 20, 30, 900, 120, Fill::linear({{0, hex("#0B1A2EF0")}, {1, hex("#10213AE0")}}, 0), 18);
            bg.shadow = {true, 0, 8, 20, hex("#00000080")};
            bg.anim = Anim::SlideLeft;
            add(d, bg);
            Element acc = rectEl("Accent", 20, 30, 12, 120, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 90), 6);
            acc.anim = Anim::SlideLeft;
            add(d, acc);
            Element n = textEl("Name", "{name}", 60, 44, 820, 56, 46, "#F4F7FB", true, "left");
            n.anim = Anim::SlideLeft;
            add(d, n);
            Element r = textEl("Role", "Author · Artist · Broadcaster", 60, 100, 820, 40, 28, "#9FB3C8", false, "left");
            r.anim = Anim::SlideLeft;
            r.animMs = 700;
            add(d, r);
            v.push_back({"lower-third", "Lower third", "On stream", d});
        }
        {
            Document d;
            d.name = "Facecam frame";
            d.width = 680;
            d.height = 420;
            Element f = rectEl("Frame", 20, 20, 640, 360, Fill::none(), 28);
            f.stroke = hex("#3FCF8E");
            f.strokeWidth = 8;
            f.shadow = {true, 0, 0, 24, hex("#3FCF8E88")};
            add(d, f);
            Element tag = rectEl("Name tag", 40, 352, 260, 52, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0), 26);
            add(d, tag);
            add(d, textEl("Name", "{name}", 40, 358, 260, 40, 28, "#06101C", true));
            v.push_back({"facecam-frame", "Facecam frame", "On stream", d});
        }
        {
            Document d;
            d.name = "Socials bar";
            d.width = 1200;
            d.height = 90;
            add(d, rectEl("Bar", 0, 10, 1200, 70, Fill::solid(hex("#0B1A2ED0")), 35));
            add(d, textEl("Handles", "{twitch}   ·   {youtube}   ·   {instagram}", 30, 22, 1140, 46, 30, "#E8F1F7", false));
            v.push_back({"socials-bar", "Socials bar", "On stream", d});
        }
        {
            Document d;
            d.name = "Twitch panel";
            d.width = 320;
            d.height = 100;
            d.background = Fill::linear({{0, hex("#0B1A2E")}, {1, hex("#13304A")}}, 0);
            add(d, rectEl("Accent", 0, 92, 320, 8, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0)));
            add(d, textEl("Label", "ABOUT ME", 0, 26, 320, 48, 36, "#F4F7FB", true));
            v.push_back({"twitch-panel", "Twitch panel (320 × 100)", "Panels", d});
        }
        {
            Document d = holding("Offline banner", "Offline", "{schedule}");
            d.name = "Offline banner";
            v.push_back({"offline-banner", "Offline banner (1920 × 1080)", "Panels", d});
        }
        return v;
    }();
    return t;
}

}  // namespace ncap::design

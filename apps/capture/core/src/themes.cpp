// NorthRay Capture: themes (0.21), the ready-made designs in each theme, and the Writing card.
//
// A theme is a design style plus colours and fonts. Each style has its own pieces (a card, a label, a title, a rule, a
// portrait, an icon, a full page and a frame) and its own layouts for the holding screens, so a theme is a different
// set of designs, not the same ones recoloured. The pieces read every colour and font from the theme, so your own
// themes (a copy of a built-in one with other colours, fonts and corners) carry the style with their own look.
#include "ncap/design.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ncap::design {

using nlohmann::json;
using sourceutil::colourText;
using sourceutil::parseColour;

namespace {
constexpr float kPi = 3.14159265358979f;

Rgba hex(const char* h) {
    Rgba c;
    parseColour(h, c);
    return c;
}
Rgba al(Rgba c, int a) {
    c.a = (std::uint8_t)std::clamp(a, 0, 255);
    return c;
}
Rgba mixc(Rgba a, Rgba b, float t) {
    auto m = [&](std::uint8_t x, std::uint8_t y) { return (std::uint8_t)std::lround(x + (y - x) * t); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}
float lum(Rgba c) { return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f; }

Element textEl(const char* name, const std::string& text, float x, float y, float w, float h, float size, Rgba colour, bool bold,
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
    e.fill = Fill::solid(colour);
    return e;
}
Element rectEl(const std::string& name, float x, float y, float w, float h, Fill f, float radius = 0) {
    Element e;
    e.kind = Kind::Rect;
    e.name = name;
    e.x = x, e.y = y, e.w = w, e.h = h;
    e.fill = f;
    e.radius = radius;
    return e;
}
Element ellipseEl(const std::string& name, float x, float y, float w, float h, Fill f) {
    Element e = rectEl(name, x, y, w, h, f);
    e.kind = Kind::Ellipse;
    return e;
}
Element picEl(const std::string& name, const std::string& file, float x, float y, float w, float h, float radius) {
    Element p;
    p.kind = Kind::Image;
    p.name = name;
    p.file = file;
    p.cover = true;
    p.radius = radius;
    p.x = x, p.y = y, p.w = w, p.h = h;
    p.fill = Fill::none();
    return p;
}
// A straight line from (x1, y1) to (x2, y2), `t` thick: a thin rectangle turned about its middle.
Element lineEl(const std::string& name, float x1, float y1, float x2, float y2, float t, Fill f) {
    const float len = std::hypot(x2 - x1, y2 - y1);
    Element e = rectEl(name, (x1 + x2) / 2 - len / 2, (y1 + y2) / 2 - t / 2, len, t, f, t / 2);
    e.rotation = std::atan2(y2 - y1, x2 - x1) * 180.0f / kPi;
    return e;
}
Element diamondEl(const std::string& name, float cx, float cy, float size, Fill f) {
    Element e = rectEl(name, cx - size / 2, cy - size / 2, size, size, f);
    e.rotation = 45;
    return e;
}
// A seeded number 0..1 (the same each time a template is made).
float rnd(int i, int salt) {
    const float n = std::sin(i * 12.9898f + salt * 78.233f) * 43758.5453f;
    return n - std::floor(n);
}

// ------------------------------------------------------------------------------------------------ the built-in themes

Theme make(const char* id, const char* name, const char* style, std::vector<const char*> c, const char* hf, const char* bf,
           const char* lf) {
    Theme t;
    t.id = id, t.name = name, t.style = style;
    Rgba* slots[] = {&t.page, &t.page2, &t.panel, &t.panel2, &t.edge, &t.accent, &t.accent2, &t.accent3, &t.title, &t.body, &t.muted, &t.onAccent, &t.sky};
    for (std::size_t i = 0; i < std::size(slots) && i < c.size(); ++i) *slots[i] = hex(c[i]);
    t.headingFont = hf, t.bodyFont = bf, t.labelFont = lf;
    t.builtIn = true;
    return t;
}
}  // namespace

const std::vector<ThemeStyle>& themeStyles() {
    static const std::vector<ThemeStyle> s = {
        {"aurora", "NorthRay aurora", "Night panels with aurora light, glowing titles and round portraits"},
        {"bookish", "Bookish", "Parchment and ink, gold rules and frames, arched portraits"},
        {"fantasy", "Dark fantasy", "Black and crimson, silver blade corners and gems, embers and mist"},
        {"scifi", "Sci-fi", "Deep blue with neon HUD brackets, a grid horizon and readouts"},
        {"cosy", "Cosy", "Cream notes on warm wood, washi tape, fairy lights and polaroids"},
        {"minimal", "Minimal", "Flat panels, clean type and a single accent"}};
    return s;
}

const std::vector<Theme>& builtInThemes() {
    // page, page2, panel, panel2, edge, accent, accent2, accent3, title, body, muted, onAccent, sky
    static const std::vector<Theme> t = {
        make("northray", "NorthRay aurora", "aurora",
             {"#060B17", "#0E2A33", "#0B1A2E", "#10263E", "#3FCF8E", "#3FCF8E", "#7A5CFF", "#22C3E6", "#F4F7FB", "#C9D6E3", "#9FB3C8", "#06101C", "#070C18"},
             "Segoe UI", "Segoe UI", "Segoe UI"),
        make("bookish", "Bookish", "bookish",
             {"#F5E8C8", "#E4CD9A", "#F7EDD5", "#EAD7AC", "#A8853A", "#8C6A22", "#6B2E1E", "#D9B45A", "#2A1C0E", "#4A3622", "#7A6243", "#F5E3B0", "#1C130A"},
             "Georgia", "Georgia", "Georgia"),
        make("fantasy", "Dark fantasy", "fantasy",
             {"#08060A", "#1C0A0F", "#120B0F", "#1F0E14", "#B9BDC7", "#C42A3C", "#6E0F1C", "#FF8A3D", "#EEE8E3", "#D2C8C4", "#9C908D", "#F7EFEA", "#070507"},
             "Palatino Linotype", "Palatino Linotype", "Palatino Linotype"),
        make("scifi", "Sci-fi", "scifi",
             {"#020713", "#071C36", "#051226", "#0A2142", "#22E5FF", "#22E5FF", "#2D6BFF", "#FF3DCB", "#EAF9FF", "#B7D7EE", "#6E92B2", "#021018", "#020611"},
             "Bahnschrift", "Bahnschrift", "Consolas"),
        make("cosy", "Cosy", "cosy",
             {"#2E1C11", "#4B2F1D", "#FFF6E6", "#F2E0C2", "#8A5A35", "#D98A2B", "#A84E2C", "#7E9B5A", "#3A2315", "#5A3D28", "#8A6A50", "#FFF6E6", "#1E120A"},
             "Candara", "Candara", "Segoe Print"),
        make("minimal", "Minimal (charcoal)", "minimal",
             {"#141517", "#141517", "#1D1F22", "#1D1F22", "#2E3136", "#4F8BFF", "#4F8BFF", "#8FB4FF", "#F4F4F3", "#C8CACE", "#868B93", "#FFFFFF", "#121315"},
             "Segoe UI", "Segoe UI", "Segoe UI"),
        make("minimal-light", "Minimal (white)", "minimal",
             {"#F7F7F5", "#F7F7F5", "#FFFFFF", "#FFFFFF", "#E3E4E6", "#E5484D", "#E5484D", "#F08A8D", "#16171A", "#3B3E44", "#7C8089", "#FFFFFF", "#F2F2F0"},
             "Segoe UI", "Segoe UI", "Segoe UI"),
    };
    return t;
}

bool validThemeId(const std::string& id) {
    if (id.empty() || id.size() > 40) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

Theme themeById(const std::vector<Theme>& own, const std::string& id) {
    for (auto& t : own)
        if (t.id == id) return t;
    for (auto& t : builtInThemes())
        if (t.id == id) return t;
    return builtInThemes()[0];
}

Theme copyTheme(const Theme& from, const std::vector<Theme>& own) {
    Theme t = from;
    int n = 1;
    auto taken = [&](const std::string& id) {
        for (auto& o : own)
            if (o.id == id) return true;
        return false;
    };
    while (taken("own-" + std::to_string(n))) ++n;
    t.id = "own-" + std::to_string(n);
    t.name = (from.name + " (mine)").substr(0, 60);
    t.builtIn = false;
    return t;
}

std::string themesToJson(const std::vector<Theme>& own) {
    json a = json::array();
    for (auto& t : own) {
        if (t.builtIn) continue;
        a.push_back({{"id", t.id}, {"name", t.name}, {"style", t.style}, {"page", colourText(t.page)}, {"page2", colourText(t.page2)},
                     {"panel", colourText(t.panel)}, {"panel2", colourText(t.panel2)}, {"edge", colourText(t.edge)},
                     {"accent", colourText(t.accent)}, {"accent2", colourText(t.accent2)}, {"accent3", colourText(t.accent3)},
                     {"title", colourText(t.title)}, {"body", colourText(t.body)}, {"muted", colourText(t.muted)},
                     {"onAccent", colourText(t.onAccent)}, {"sky", colourText(t.sky)}, {"headingFont", t.headingFont},
                     {"bodyFont", t.bodyFont}, {"labelFont", t.labelFont}, {"corners", t.corners}});
    }
    return json{{"format", "northray-capture-themes"}, {"version", 1}, {"themes", a}}.dump(2);
}

std::vector<Theme> themesFromJson(const std::string& text) {
    std::vector<Theme> out;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("themes") || !j["themes"].is_array()) return out;
    std::set<std::string> ids;
    for (auto& o : j["themes"]) {
        if (!o.is_object() || out.size() >= 40) continue;
        if (!o.contains("id") || !o["id"].is_string() || !o.contains("style") || !o["style"].is_string()) continue;
        const std::string id = o["id"].get<std::string>(), style = o["style"].get<std::string>();
        bool known = false;
        for (auto& s : themeStyles()) known = known || style == s.key;
        bool builtin = false;
        for (auto& b : builtInThemes()) builtin = builtin || b.id == id;
        if (!validThemeId(id) || !known || builtin || ids.count(id)) continue;
        // Start from the first built-in theme of that style, so a colour left out still fits.
        Theme t;
        for (auto& b : builtInThemes())
            if (b.style == style) {
                t = b;
                break;
            }
        t.id = id, t.style = style, t.builtIn = false;
        t.name = o.contains("name") && o["name"].is_string() && !o["name"].get<std::string>().empty() ? o["name"].get<std::string>().substr(0, 60) : id;
        auto colour = [&](const char* k, Rgba& c) {
            Rgba v;
            if (o.contains(k) && o[k].is_string() && parseColour(o[k].get<std::string>(), v)) c = {v.r, v.g, v.b, 255};
        };
        colour("page", t.page), colour("page2", t.page2), colour("panel", t.panel), colour("panel2", t.panel2), colour("edge", t.edge);
        colour("accent", t.accent), colour("accent2", t.accent2), colour("accent3", t.accent3), colour("title", t.title);
        colour("body", t.body), colour("muted", t.muted), colour("onAccent", t.onAccent), colour("sky", t.sky);
        auto font = [&](const char* k, std::string& f) {
            if (o.contains(k) && o[k].is_string() && !o[k].get<std::string>().empty()) f = o[k].get<std::string>().substr(0, 64);
        };
        font("headingFont", t.headingFont), font("bodyFont", t.bodyFont), font("labelFont", t.labelFont);
        if (o.contains("corners") && o["corners"].is_number() && std::isfinite(o["corners"].get<double>()))
            t.corners = std::clamp(o["corners"].get<float>(), 0.0f, 2.0f);
        ids.insert(id);
        out.push_back(t);
    }
    return out;
}

Theme withWorldAccent(const Theme& t, Rgba w) {
    Theme o = t;
    w.a = 255;
    const bool darkPanels = lum(t.panel) < 0.5f;
    for (int i = 0; i < 10; ++i) {   // light enough to read on dark panels, dark enough on light ones
        const float l = lum(w);
        if (darkPanels ? l >= 0.42f : l <= 0.45f) break;
        w = darkPanels ? mixc(w, Rgba{255, 255, 255, 255}, 0.22f) : mixc(w, Rgba{0, 0, 0, 255}, 0.22f);
    }
    o.accent = w;
    return o;
}

void recolourAccent(Document& d, const Theme& t, Rgba w) {
    const Theme wt = withWorldAccent(t, w);
    const Rgba from = t.accent, to = wt.accent;
    auto fix = [&](Rgba& c) {
        if (c.r == from.r && c.g == from.g && c.b == from.b) c = {to.r, to.g, to.b, c.a};
    };
    auto fill = [&](Fill& f) {
        fix(f.colour);
        for (auto& s : f.stops) fix(s.colour);
    };
    fill(d.background);
    for (auto& e : d.elements) {
        fill(e.fill);
        fix(e.stroke);
        fix(e.shadow.colour);
        fix(e.glow.colour);
    }
}

namespace {
// ----------------------------------------------------------------------------------------------------- the pieces

// A theme, ready to make pieces from.
struct Look {
    Theme t;
    std::string s;
    explicit Look(const Theme& th) : t(th), s(th.style) {}
    bool is(const char* k) const { return s == k; }
    bool light() const { return lum(t.panel) > 0.6f; }   // light panels: ink-coloured words on them
    float r(float base) const { return base * t.corners; }
    // A label's words in the style's way: capitals, "// CAPITALS" or as written.
    std::string lab(const std::string& upper, const std::string& natural) const {
        if (is("bookish") || is("cosy")) return natural;
        if (is("scifi")) return "// " + upper;
        return upper;
    }
};
constexpr const char* kSerif = "Georgia";

Element text(const Look& L, const char* name, const std::string& words, float x, float y, float w, float h, float size, Rgba c,
             bool bold, const std::string& font, Anim a, const char* align) {
    Element e = textEl(name, words, x, y, w, h, size, c, bold, align);
    e.font = font;
    e.anim = a;
    (void)L;
    return e;
}

void card(Document& d, const Look& L, float x, float y, float w, float h, Anim a, const char* name = "Card") {
    const Theme& t = L.t;
    auto put = [&](Element e) {
        e.anim = a;
        add(d, e);
    };
    if (L.is("bookish")) {
        Element c = rectEl(name, x, y, w, h, Fill::linear({{0, t.panel}, {1, t.panel2}}, 90), L.r(12));
        c.stroke = t.edge, c.strokeWidth = 3;
        c.shadow = {true, 0, 8, 22, hex("#1E140A80"), ""};
        put(c);
        Element r = rectEl("Inner rule", x + 9, y + 9, w - 18, h - 18, Fill::none(), L.r(6));   // as on a title page
        r.stroke = al(t.edge, 0x99), r.strokeWidth = 1.5f;
        put(r);
    } else if (L.is("fantasy")) {
        Element c = rectEl(name, x, y, w, h, Fill::linear({{0, al(t.panel, 0xF4)}, {1, al(t.panel2, 0xF0)}}, 90), L.r(4));
        c.stroke = al(t.edge, 0xCC), c.strokeWidth = 2;
        c.shadow = {true, 0, 12, 30, hex("#000000B8"), ""};
        put(c);
        Element in = rectEl("Crimson rule", x + 7, y + 7, w - 14, h - 14, Fill::none(), L.r(2));
        in.stroke = al(t.accent, 0x99), in.strokeWidth = 1;
        put(in);
        Element g = ellipseEl("Ember light", x + w * 0.15f, y + h * 0.55f, w * 0.7f, h * 0.6f, Fill::radial({{0, al(t.accent, 0x26)}, {1, al(t.accent, 0)}}));
        put(g);
        for (auto [cx, cy] : std::vector<std::pair<float, float>>{{x, y}, {x + w, y}, {x, y + h}, {x + w, y + h}}) {
            put(diamondEl("Blade corner", cx, cy, 14, Fill::linear({{0, mixc(t.edge, Rgba{255, 255, 255, 255}, 0.4f)}, {1, t.edge}}, 90)));
            put(diamondEl("Blade corner (inner)", cx, cy, 6, Fill::solid(t.accent)));
        }
        if (w >= 200) {
            Element gem = diamondEl("Gem", x + w / 2, y, 16, Fill::linear({{0, t.accent3}, {1, t.accent}}, 90));
            gem.stroke = t.edge, gem.strokeWidth = 1.5f;
            gem.glow = {true, 14, 1.2f, al(t.accent, 0xAA), ""};
            put(gem);
        }
    } else if (L.is("scifi")) {
        Element c = rectEl(name, x, y, w, h, Fill::linear({{0, al(t.panel, 0xE8)}, {1, al(t.panel2, 0xD8)}}, 0), L.r(3));
        c.stroke = al(t.edge, 0x40), c.strokeWidth = 1;
        c.shadow = {true, 0, 0, 26, al(t.accent, 0x30), ""};
        put(c);
        Element scan = rectEl("Scan light", x, y + h * 0.18f, w, h * 0.3f, Fill::linear({{0, al(t.accent, 0)}, {0.5f, al(t.accent, 0x12)}, {1, al(t.accent, 0)}}, 90));
        put(scan);
        const float len = std::min(34.0f, std::min(w, h) * 0.3f), th = 3;
        for (int i = 0; i < 4; ++i) {   // HUD brackets at the corners
            const bool right = i % 2, bottom = i / 2;
            const float cx = right ? x + w : x, cy = bottom ? y + h : y;
            Element hz = rectEl("Bracket", right ? cx - len : cx, bottom ? cy - th : cy, len, th, Fill::solid(t.accent));
            Element vt = rectEl("Bracket", right ? cx - th : cx, bottom ? cy - len : cy, th, len, Fill::solid(t.accent));
            hz.glow = vt.glow = {true, 8, 1.0f, al(t.accent, 0x99), ""};
            put(hz);
            put(vt);
        }
        if (w >= 160) {
            put(rectEl("Tab", x + len + 10, y, std::min(90.0f, w * 0.2f), 4, Fill::solid(al(t.accent, 0xAA))));
            for (int k = 0; k < 4 && h >= 70; ++k) put(rectEl("Tick", x + w - 8, y + h * 0.3f + k * 9, 4, 4, Fill::solid(al(t.accent, 0x99 - k * 0x18))));
        }
    } else if (L.is("cosy")) {
        Element c = rectEl(name, x, y, w, h, Fill::linear({{0, t.panel}, {1, t.panel2}}, 90), L.r(22));
        c.stroke = t.edge, c.strokeWidth = 5;
        c.shadow = {true, 0, 8, 20, hex("#1E0F0580"), ""};
        put(c);
        Element st = rectEl("Stitching", x + 11, y + 11, w - 22, h - 22, Fill::none(), L.r(14));
        st.stroke = al(t.edge, 0x55), st.strokeWidth = 1.5f;
        put(st);
        if (w >= 240 && h >= 100) {
            Element t1 = rectEl("Washi tape", x - 18, y - 6, 92, 26, Fill::solid(al(t.accent, 0xB0)), 2);
            t1.rotation = -14;
            put(t1);
            Element t2 = rectEl("Washi tape", x + w - 74, y - 8, 92, 26, Fill::solid(al(t.accent3, 0xB0)), 2);
            t2.rotation = 11;
            put(t2);
        }
    } else if (L.is("minimal")) {
        Element c = rectEl(name, x, y, w, h, Fill::solid(al(t.panel, 0xF4)), L.r(10));
        c.stroke = t.edge, c.strokeWidth = 1;
        put(c);
        put(rectEl("Accent bar", x, y + std::min(h * 0.2f, 14.0f), 4, h - 2 * std::min(h * 0.2f, 14.0f), Fill::solid(t.accent), 2));
    } else {   // aurora
        Element c = rectEl(name, x, y, w, h, Fill::linear({{0, al(t.panel, 0xF2)}, {1, al(t.panel2, 0xEE)}}, 90), L.r(28));
        c.stroke = al(t.edge, 0x44), c.strokeWidth = 2;
        c.shadow = {true, 0, 10, 30, hex("#00000099"), ""};
        put(c);
        put(ellipseEl("Aurora light (violet)", x + w * 0.5f, y, w * 0.5f, h, Fill::radial({{0, al(t.accent2, 0x30)}, {1, al(t.accent2, 0)}})));
        put(ellipseEl("Aurora light (green)", x, y + h * 0.2f, w * 0.45f, h * 0.8f, Fill::radial({{0, al(t.accent, 0x26)}, {1, al(t.accent, 0)}})));
    }
}

Element label(const Look& L, const char* name, const std::string& upper, const std::string& natural, float x, float y, float w, float h, Anim a,
              const char* align = "left") {
    const Theme& t = L.t;
    const std::string words = L.lab(upper, natural);
    if (L.is("bookish")) {
        Element e = text(L, name, words, x, y, w, h, 24, t.accent, false, t.labelFont, a, align);
        e.italic = true;
        return e;
    }
    if (L.is("fantasy")) {
        Element e = text(L, name, words, x, y, w, h, 21, t.edge, true, t.labelFont, a, align);
        e.fill = Fill::linear({{0, mixc(t.edge, Rgba{255, 255, 255, 255}, 0.5f)}, {1, mixc(t.edge, t.panel, 0.25f)}}, 90);   // polished silver
        return e;
    }
    if (L.is("scifi")) {
        Element e = text(L, name, words, x, y, w, h, 20, t.accent, false, t.labelFont, a, align);
        e.glow = {true, 8, 0.8f, al(t.accent, 0x77), ""};
        return e;
    }
    if (L.is("cosy")) return text(L, name, words, x, y - 4, w, h + 8, 24, t.accent2, false, t.labelFont, a, align);   // handwritten
    if (L.is("minimal")) return text(L, name, words, x, y, w, h, 18, t.muted, true, t.labelFont, a, align);
    Element e = text(L, name, words, x, y, w, h, 22, t.accent, true, t.labelFont, a, align);
    e.fill = Fill::linear({{0, t.accent}, {1, t.accent3}}, 0);   // gradient letters
    return e;
}
Element title(const Look& L, const char* name, const std::string& words, float x, float y, float w, float h, float size, Anim a,
              const char* align = "left") {
    const Theme& t = L.t;
    Element e = text(L, name, words, x, y, w, h, size, t.title, true, t.headingFont, a, align);
    if (L.is("aurora")) e.glow = {true, 20, 1.0f, al(t.accent, 0x55), ""};
    else if (L.is("fantasy")) e.glow = {true, 18, 1.1f, al(t.accent, 0x66), ""};
    else if (L.is("scifi")) e.glow = {true, 14, 1.0f, al(t.accent, 0x70), ""};
    return e;
}
Element body(const Look& L, const char* name, const std::string& words, float x, float y, float w, float h, float size, Anim a,
             const char* align = "left") {
    return text(L, name, words, x, y, w, h, size, L.t.body, false, L.t.bodyFont, a, align);
}
Element muted(const Look& L, const char* name, const std::string& words, float x, float y, float w, float h, float size, Anim a,
              const char* align = "left") {
    Element e = text(L, name, words, x, y, w, h, size, L.t.muted, false, L.t.bodyFont, a, align);
    e.italic = L.is("bookish") || L.is("fantasy") || L.is("cosy");
    return e;
}
// A rule under a heading, `w` wide from x (centred pieces sit at its middle).
void rule(Document& d, const Look& L, float x, float y, float w, Anim a) {
    const Theme& t = L.t;
    auto put = [&](Element e) {
        e.anim = a;
        add(d, e);
    };
    const float mid = x + w / 2;
    if (L.is("bookish")) {
        put(rectEl("Gold rule", x, y, w, 2, Fill::solid(t.edge)));
        put(diamondEl("Ornament", mid, y + 1, 12, Fill::solid(t.edge)));
    } else if (L.is("fantasy")) {
        put(rectEl("Silver rule", x, y, w, 2, Fill::linear({{0, al(t.edge, 0)}, {0.5f, t.edge}, {1, al(t.edge, 0)}}, 0)));
        for (float dx : {-30.0f, 30.0f}) put(diamondEl("Silver ornament", mid + dx, y + 1, 8, Fill::solid(t.edge)));
        Element g = diamondEl("Gem", mid, y + 1, 16, Fill::linear({{0, t.accent3}, {1, t.accent}}, 90));
        g.glow = {true, 14, 1.2f, al(t.accent, 0xAA), ""};
        put(g);
    } else if (L.is("scifi")) {
        put(rectEl("HUD line", x, y, w, 2, Fill::solid(al(t.accent, 0x55))));
        Element seg = rectEl("HUD segment", x, y - 1, w * 0.22f, 4, Fill::solid(t.accent));
        seg.glow = {true, 10, 1.0f, al(t.accent, 0x99), ""};
        put(seg);
        for (int k = 0; k < 3; ++k) put(rectEl("HUD tick", x + w - 6 - k * 10, y - 5, 3, 12, Fill::solid(al(t.accent, 0xCC))));
    } else if (L.is("cosy")) {
        put(rectEl("Rule", x + w * 0.2f, y, w * 0.6f / 2 - 24, 2, Fill::solid(al(t.accent, 0xAA)), 1));
        put(rectEl("Rule", mid + 24, y, w * 0.6f / 2 - 24, 2, Fill::solid(al(t.accent, 0xAA)), 1));
        Element h;
        h.kind = Kind::Icon, h.icon = "heart", h.name = "Heart";
        h.x = mid - 13, h.y = y - 12, h.w = 26, h.h = 26;
        h.fill = Fill::solid(t.accent2);
        put(h);
    } else if (L.is("minimal")) {
        put(rectEl("Accent line", x, y, std::min(64.0f, w), 3, Fill::solid(t.accent), 1.5f));
    } else {
        Element l = rectEl("Accent line", x, y, w, 4, Fill::linear({{0, t.accent}, {0.5f, t.accent3}, {1, t.accent2}}, 0), 2);
        if (a == Anim::None) a = Anim::Wipe;
        put(l);
    }
}
// How tall a portrait is for its width: bookish arches are tall, the others square.
float portraitTall(const Look& L) { return L.is("bookish") ? 1.42f : 1.0f; }
// A portrait over a backing that shows until there's a picture.
void portrait(Document& d, const Look& L, const std::string& name, const std::string& file, float x, float y, float w, float h, Anim a, int ms) {
    const Theme& t = L.t;
    auto put = [&](Element e) {
        e.anim = a;
        e.animMs = ms;
        add(d, e);
    };
    if (L.is("cosy")) {   // a polaroid, a little turned, with a white border and room below
        Element back = rectEl(name + " polaroid", x - 8, y - 8, w + 16, h + 34, Fill::solid(hex("#FFFDF8")), 3);
        back.rotation = -3;
        back.shadow = {true, 2, 6, 12, hex("#2B160A66"), ""};
        put(back);
        Element hole = rectEl(name + " backing", x, y, w, h, Fill::linear({{0, t.accent}, {1, t.accent2}}, 135), 2);
        hole.rotation = -3;
        put(hole);
        Element p = picEl(name, file, x, y, w, h, 2);
        p.rotation = -3;
        put(p);
        return;
    }
    Element back;
    Element p = picEl(name, file, x, y, w, h, 0);
    if (L.is("bookish")) {
        back = rectEl(name + " backing", x, y, w, h, Fill::linear({{0, mixc(t.panel2, t.edge, 0.25f)}, {1, mixc(t.panel2, t.edge, 0.55f)}}, 90), w / 2);
        p.mask = "arch", p.stroke = t.edge, p.strokeWidth = 4;
        p.shadow = {true, 0, 4, 12, hex("#1E140A66"), ""};
    } else if (L.is("fantasy")) {
        back = ellipseEl(name + " backing", x + w * 0.06f, y + h * 0.06f, w * 0.88f, h * 0.88f, Fill::linear({{0, t.accent2}, {1, t.panel}}, 90));
        p.mask = "hexagon", p.stroke = t.edge, p.strokeWidth = 3;
        p.glow = {true, 16, 1.1f, al(t.accent, 0x88), ""};
    } else if (L.is("scifi")) {
        back = ellipseEl(name + " backing", x + w * 0.06f, y + h * 0.06f, w * 0.88f, h * 0.88f, Fill::linear({{0, t.accent2}, {1, t.panel}}, 135));
        p.mask = "hexagon", p.stroke = t.accent, p.strokeWidth = 3;
        p.glow = {true, 14, 1.0f, al(t.accent, 0x99), ""};
    } else if (L.is("minimal")) {
        back = ellipseEl(name + " backing", x, y, w, h, Fill::solid(t.edge));
        p.mask = "circle", p.stroke = t.accent, p.strokeWidth = 2;
    } else {
        back = ellipseEl(name + " backing", x, y, w, h, Fill::linear({{0, t.accent}, {1, t.accent2}}, 135));
        p.mask = "circle", p.stroke = t.accent, p.strokeWidth = 4;
        p.glow = {true, 18, 1.2f, al(t.accent, 0x88), ""};
    }
    put(back);
    put(p);
}
Element icon(const Look& L, const char* name, const char* id, float x, float y, float w, float h, Anim a) {
    const Theme& t = L.t;
    Element ic;
    ic.kind = Kind::Icon;
    ic.icon = id;
    ic.name = name;
    ic.x = x, ic.y = y, ic.w = w, ic.h = h;
    if (L.is("bookish")) ic.fill = Fill::linear({{0, t.accent3}, {1, mixc(t.accent3, t.accent, 0.6f)}}, 90);
    else if (L.is("fantasy")) ic.fill = Fill::linear({{0, t.accent3}, {1, t.accent}}, 90), ic.glow = {true, 14, 1.0f, al(t.accent, 0x88), ""};
    else if (L.is("scifi")) ic.fill = Fill::solid(t.accent), ic.glow = {true, 12, 1.0f, al(t.accent, 0x99), ""};
    else if (L.is("cosy")) ic.fill = Fill::linear({{0, t.accent}, {1, t.accent2}}, 90);
    else if (L.is("minimal")) ic.fill = Fill::solid(t.accent);
    else ic.fill = Fill::linear({{0, t.accent}, {1, t.accent2}}, 0), ic.glow = {true, 16, 1.0f, al(t.accent, 0x66), ""};
    ic.anim = a;
    return ic;
}
// A small badge with words on the main colour (ON AIR and the like).
void badge(Document& d, const Look& L, const char* name, const std::string& words, float x, float y, float w, float h, Anim a) {
    const Theme& t = L.t;
    Element pill = rectEl(name, x, y, w, h, Fill::solid(t.accent), L.is("scifi") || L.is("fantasy") ? L.r(3) : h / 2);
    Rgba ink = t.onAccent;
    if (L.is("aurora")) pill.fill = Fill::solid(hex("#E0566A")), ink = hex("#FFFFFF");   // on air is red
    if (L.is("bookish")) pill.fill = Fill::linear({{0, hex("#9B3A2C")}, {1, hex("#6E2219")}}, 90), pill.stroke = t.accent3, pill.strokeWidth = 2, ink = t.onAccent;
    if (L.is("fantasy")) pill.stroke = t.edge, pill.strokeWidth = 1.5f, pill.glow = {true, 12, 1.0f, al(t.accent, 0x88), ""};
    if (L.is("scifi")) pill.fill = Fill::none(), pill.stroke = t.accent, pill.strokeWidth = 2, pill.glow = {true, 10, 1.0f, al(t.accent, 0x88), ""}, ink = t.accent;
    pill.anim = Anim::Pop;
    add(d, pill);
    Element e = text(L, (std::string(name) + " text").c_str(), words, x, y + h * 0.08f, w, h * 0.84f, h * 0.5f, ink, true,
                     L.is("scifi") ? t.labelFont : t.headingFont, Anim::Pop, "centre");
    (void)a;
    add(d, e);
}

// ---------------------------------------------------------------------------------------------- full pages
void page(Document& d, const Look& L) {
    const Theme& t = L.t;
    const float W = (float)d.width, H = (float)d.height, m = std::min(W, H);
    if (L.is("bookish")) {
        d.background = Fill::linear({{0, t.page}, {1, t.page2}}, 90);
        add(d, ellipseEl("Paper light", W * 0.1f, H * 0.05f, W * 0.8f, H * 0.9f, Fill::radial({{0, hex("#FFF8E655")}, {1, hex("#FFF8E600")}})));
        const float g = m * 0.037f;
        Element outer = rectEl("Gold frame", g, g, W - 2 * g, H - 2 * g, Fill::none(), L.r(8));
        outer.stroke = t.edge, outer.strokeWidth = 4;
        add(d, outer);
        Element inner = rectEl("Inner rule", g + 14, g + 14, W - 2 * g - 28, H - 2 * g - 28, Fill::none(), L.r(4));
        inner.stroke = al(t.edge, 0x99), inner.strokeWidth = 1.5f;
        add(d, inner);
        for (auto [cx, cy] : std::vector<std::pair<float, float>>{{g + 7, g + 7}, {W - g - 7, g + 7}, {g + 7, H - g - 7}, {W - g - 7, H - g - 7}})
            add(d, diamondEl("Corner ornament", cx, cy, 18, Fill::solid(t.edge)));
    } else if (L.is("fantasy")) {
        d.background = Fill::linear({{0, t.page}, {0.6f, mixc(t.page, t.page2, 0.6f)}, {1, t.page2}}, 90);
        add(d, ellipseEl("Crimson glow", W * 0.05f, H * 0.55f, W * 0.9f, H * 0.9f, Fill::radial({{0, al(t.accent, 0x44)}, {0.6f, al(t.accent, 0x14)}, {1, al(t.accent, 0)}})));
        add(d, ellipseEl("Mist", -W * 0.1f, H * 0.72f, W * 0.7f, H * 0.22f, Fill::radial({{0, al(t.muted, 0x22)}, {1, al(t.muted, 0)}})));
        add(d, ellipseEl("Mist", W * 0.45f, H * 0.78f, W * 0.7f, H * 0.2f, Fill::radial({{0, al(t.muted, 0x1C)}, {1, al(t.muted, 0)}})));
        for (int i = 0; i < 22; ++i) {   // embers drifting up
            const float ex = rnd(i, 3) * W, ey = H * (0.35f + 0.6f * rnd(i, 7)), s = 3 + 5 * rnd(i, 11);
            Element e = ellipseEl("Ember", ex, ey, s, s, Fill::solid(mixc(t.accent3, t.accent, rnd(i, 13))));
            e.glow = {true, 10, 1.2f, al(t.accent3, 0xAA), ""};
            e.opacity = 0.45f + 0.55f * rnd(i, 17);
            add(d, e);
        }
        const float g = m * 0.04f;
        Element fr = rectEl("Silver frame", g, g, W - 2 * g, H - 2 * g, Fill::none(), L.r(2));
        fr.stroke = al(t.edge, 0x88), fr.strokeWidth = 2;
        add(d, fr);
        for (auto [cx, cy] : std::vector<std::pair<float, float>>{{g, g}, {W - g, g}, {g, H - g}, {W - g, H - g}}) {
            add(d, diamondEl("Blade corner", cx, cy, 26, Fill::linear({{0, mixc(t.edge, Rgba{255, 255, 255, 255}, 0.4f)}, {1, t.edge}}, 90)));
            Element gem = diamondEl("Corner gem", cx, cy, 10, Fill::solid(t.accent));
            gem.glow = {true, 12, 1.2f, al(t.accent, 0xAA), ""};
            add(d, gem);
        }
    } else if (L.is("scifi")) {
        d.background = Fill::linear({{0, t.page}, {0.62f, t.page2}, {1, t.page}}, 90);
        const float hz = H * 0.66f;
        add(d, ellipseEl("Horizon glow", -W * 0.1f, hz - H * 0.25f, W * 1.2f, H * 0.5f, Fill::radial({{0, al(t.accent2, 0x44)}, {1, al(t.accent2, 0)}})));
        for (int i = 1; i < 8; ++i) {   // the grid floor: lines closer together towards the horizon
            const float f = (float)i / 8, y = hz + (H - hz) * f * f;
            add(d, rectEl("Grid", 0, y, W, 2, Fill::solid(al(t.accent, 0x20 + i * 10))));
        }
        for (int k = -7; k <= 7; ++k) {
            const float bx = W / 2 + k * W * 0.16f;
            add(d, lineEl("Grid", W / 2 + k * W * 0.012f, hz, bx, H + 4, 2, Fill::linear({{0, al(t.accent, 0x10)}, {1, al(t.accent, 0x70)}}, 0)));
        }
        Element hl = rectEl("Horizon", 0, hz - 1, W, 3, Fill::linear({{0, al(t.accent, 0)}, {0.5f, t.accent}, {1, al(t.accent, 0)}}, 0));
        hl.glow = {true, 18, 1.2f, al(t.accent, 0x99), ""};
        add(d, hl);
        const float g = m * 0.045f, len = m * 0.08f;
        for (int i = 0; i < 4; ++i) {   // HUD corners
            const bool right = i % 2, bottom = i / 2;
            const float cx = right ? W - g : g, cy = bottom ? H - g : g;
            Element a = rectEl("HUD corner", right ? cx - len : cx, bottom ? cy - 4 : cy, len, 4, Fill::solid(t.accent));
            Element b = rectEl("HUD corner", right ? cx - 4 : cx, bottom ? cy - len : cy, 4, len, Fill::solid(t.accent));
            a.glow = b.glow = {true, 10, 1.0f, al(t.accent, 0x99), ""};
            add(d, a);
            add(d, b);
        }
        Element clock;   // a readout: the time now
        clock.kind = Kind::Timer;
        clock.name = "Clock readout";
        clock.timerMode = "clock";
        clock.x = W - g - 320, clock.y = g + 14, clock.w = 300, clock.h = 40;
        clock.size = 28, clock.font = t.labelFont, clock.align = "right";
        clock.fill = Fill::solid(t.accent);
        clock.glow = {true, 8, 0.8f, al(t.accent, 0x77), ""};
        add(d, clock);
    } else if (L.is("cosy")) {
        d.background = Fill::linear({{0, t.page2}, {1, t.page}}, 0);
        const int planks = 6;
        for (int i = 0; i < planks; ++i) {   // wood planks with dark gaps
            const float pw = W / planks;
            add(d, rectEl("Plank", i * pw, 0, pw, H, Fill::linear({{0, al(hex("#000000"), (int)(rnd(i, 5) * 0x26))}, {0.5f, al(hex("#FFFFFF"), (int)(rnd(i, 9) * 0x0C))}, {1, al(hex("#000000"), (int)(rnd(i, 21) * 0x22))}}, 90)));
            if (i) add(d, rectEl("Plank gap", i * pw - 2, 0, 4, H, Fill::solid(hex("#00000055"))));
        }
        add(d, ellipseEl("Lamp light", -W * 0.15f, -H * 0.3f, W * 0.8f, H * 1.0f, Fill::radial({{0, al(t.accent, 0x50)}, {0.6f, al(t.accent, 0x18)}, {1, al(t.accent, 0)}})));
        const int n = 14;
        for (int i = 0; i < n; ++i) {   // a string of fairy lights sagging across the top
            const float u0 = (i + 0.5f) / n, u1 = (i + 1.5f) / n;
            const float y0 = H * 0.05f + H * 0.07f * std::sin(kPi * u0), y1 = H * 0.05f + H * 0.07f * std::sin(kPi * u1);
            if (i + 1 < n) add(d, lineEl("Light string", u0 * W, y0, u1 * W, y1, 2, Fill::solid(hex("#1A0E0699"))));
            const Rgba c = i % 3 == 0 ? t.accent : i % 3 == 1 ? hex("#FFE9B0") : t.accent3;
            Element b = ellipseEl("Fairy light", u0 * W - 7, y0 - 2, 14, 18, Fill::solid(c));
            b.glow = {true, 18, 1.4f, al(c, 0xCC), ""};
            add(d, b);
        }
    } else if (L.is("minimal")) {
        d.background = Fill::linear({{0, t.page}, {1, t.page2}}, 90);
        const float g = m * 0.07f;
        add(d, rectEl("Accent square", g, g, 22, 22, Fill::solid(t.accent), L.r(3)));
    } else {   // aurora: the night with aurora light
        d.background = Fill::linear({{0, t.page}, {0.55f, t.panel}, {1, t.page2}}, 90);
        add(d, ellipseEl("Aurora (green)", -0.15f * W, -0.45f * H, 0.85f * W, 0.95f * H,
                         Fill::radial({{0, al(t.accent, 0x55)}, {0.55f, al(t.accent, 0x22)}, {1, al(t.accent, 0)}})));
        add(d, ellipseEl("Aurora (violet)", 0.4f * W, -0.35f * H, 0.75f * W, 0.8f * H,
                         Fill::radial({{0, al(t.accent2, 0x4A)}, {0.6f, al(t.accent2, 0x1C)}, {1, al(t.accent2, 0)}})));
    }
}

// ---------------------------------------------------------------------------------------------- the designs

struct Words {
    std::string title, sub;
};
// What each holding screen says in each style.
Words holdingWords(const Look& L, const std::string& id) {
    struct Row {
        const char *id, *style, *title, *sub;
    };
    static const Row rows[] = {
        {"starting-soon", "aurora", "Starting soon", "Grab a drink, we'll be live in a moment"},
        {"be-right-back", "aurora", "Be right back", "Don't go anywhere"},
        {"ending", "aurora", "Thanks for watching", "See you next stream"},
        {"starting-soon", "bookish", "Starting soon", "Settle in with a cup of tea"},
        {"be-right-back", "bookish", "Be right back", "Keep your place, I'll return shortly"},
        {"ending", "bookish", "Thank you for reading along", "Until the next chapter"},
        {"starting-soon", "fantasy", "Starting soon", "The gates open shortly"},
        {"be-right-back", "fantasy", "Be right back", "Hold the line, I shall return"},
        {"ending", "fantasy", "Thanks for watching", "Until our paths cross again"},
        {"starting-soon", "scifi", "Starting soon", "Systems coming online"},
        {"be-right-back", "scifi", "Be right back", "Signal paused, stand by"},
        {"ending", "scifi", "Thanks for watching", "Transmission ended. See you next stream"},
        {"starting-soon", "cosy", "Starting soon", "Grab a blanket and a warm drink"},
        {"be-right-back", "cosy", "Be right back", "Put the kettle on, I'll be back soon"},
        {"ending", "cosy", "Thanks for hanging out", "Sleep well, see you next time"},
        {"starting-soon", "minimal", "Starting soon", "We'll be live in a moment"},
        {"be-right-back", "minimal", "Be right back", "Back shortly"},
        {"ending", "minimal", "Thanks for watching", "See you next stream"},
    };
    if (id == "offline-banner") return {"Offline", "{schedule}"};
    for (auto& r : rows)
        if (id == r.id && L.s == r.style) return {r.title, r.sub};
    return {"Starting soon", ""};
}
const char* kSocials = "{twitch}   \xC2\xB7   {youtube}   \xC2\xB7   {instagram}";

Document holding(const Look& L, const std::string& id, const char* name) {
    Document d;
    d.name = name;
    const Words w = holdingWords(L, id);
    const Theme& t = L.t;
    page(d, L);
    if (L.is("bookish")) {
        Element above = label(L, "Above", "{name}", "{name}", 260, 290, 1400, 60, Anim::Fade, "centre");
        above.size = 40;
        add(d, above);
        add(d, title(L, "Title", w.title, 160, 360, 1600, 200, 124, Anim::Pop, "centre"));
        rule(d, L, 760, 590, 400, Anim::Fade);
        Element s = muted(L, "Subtitle", w.sub, 260, 620, 1400, 80, 44, Anim::Fade, "centre");
        s.animMs = 900;
        add(d, s);
        add(d, muted(L, "Socials", kSocials, 260, 880, 1400, 60, 32, Anim::None, "centre"));
    } else if (L.is("fantasy")) {
        Element above = label(L, "Above", "{name}", "{name}", 260, 270, 1400, 56, Anim::Fade, "centre");
        above.size = 34;
        add(d, above);
        rule(d, L, 660, 350, 600, Anim::Fade);
        Element tt = title(L, "Title", w.title, 160, 380, 1600, 190, 120, Anim::Pop, "centre");
        tt.shadow = {true, 0, 6, 20, hex("#000000CC"), ""};
        add(d, tt);
        rule(d, L, 660, 600, 600, Anim::Fade);
        Element s = muted(L, "Subtitle", w.sub, 260, 630, 1400, 80, 44, Anim::Fade, "centre");
        s.animMs = 900;
        add(d, s);
        add(d, muted(L, "Socials", kSocials, 260, 900, 1400, 60, 32, Anim::None, "centre"));
    } else if (L.is("scifi")) {
        add(d, label(L, "Above", "{name}", "{name}", 160, 250, 1200, 50, Anim::Fade));
        add(d, title(L, "Title", w.title, 150, 300, 1600, 180, 128, Anim::SlideLeft));
        rule(d, L, 160, 500, 760, Anim::Wipe);
        Element s = body(L, "Subtitle", w.sub, 160, 530, 1400, 70, 42, Anim::Fade);
        s.animMs = 900;
        add(d, s);
        Element so = text(L, "Socials", kSocials, 160, 975, 1400, 50, 28, t.accent, false, t.labelFont, Anim::None, "left");
        so.glow = {true, 8, 0.8f, al(t.accent, 0x66), ""};
        add(d, so);
    } else if (L.is("cosy")) {
        card(d, L, 260, 230, 1400, 560, Anim::Pop, "Note");
        add(d, label(L, "Above", "{name}", "{name}", 320, 290, 1280, 60, Anim::Fade, "centre"));
        add(d, title(L, "Title", w.title, 320, 370, 1280, 150, 100, Anim::Pop, "centre"));
        rule(d, L, 660, 560, 600, Anim::Fade);
        Element s = muted(L, "Subtitle", w.sub, 320, 600, 1280, 80, 42, Anim::Fade, "centre");
        s.animMs = 900;
        add(d, s);
        add(d, text(L, "Socials", kSocials, 260, 880, 1400, 60, 32, t.onAccent, false, t.bodyFont, Anim::None, "centre"));
    } else if (L.is("minimal")) {
        add(d, text(L, "Above", "{name}", 160, 360, 1400, 50, 30, t.muted, true, t.labelFont, Anim::Fade, "left"));
        add(d, title(L, "Title", w.title, 150, 410, 1650, 180, 140, Anim::SlideUp));
        rule(d, L, 160, 610, 400, Anim::Wipe);
        Element s = body(L, "Subtitle", w.sub, 160, 640, 1400, 70, 42, Anim::Fade);
        s.animMs = 900;
        add(d, s);
        add(d, muted(L, "Socials", kSocials, 160, 950, 1400, 50, 28, Anim::None, "left"));
    } else {   // aurora
        Element tt = text(L, "Title", w.title, 160, 380, 1600, 200, 132, t.title, true, t.headingFont, Anim::Pop, "centre");
        tt.shadow = {true, 0, 8, 24, hex("#00000099"), ""};
        add(d, tt);
        Element s = text(L, "Subtitle", w.sub, 260, 580, 1400, 80, 44, t.muted, false, t.bodyFont, Anim::Fade, "centre");
        s.animMs = 900;
        add(d, s);
        Element bar = rectEl("Accent line", 810, 690, 300, 6, Fill::linear({{0, t.accent}, {1, t.accent2}}, 0), 3);
        bar.anim = Anim::Wipe, bar.animMs = 800;
        add(d, bar);
        add(d, text(L, "Socials", kSocials, 260, 900, 1400, 60, 34, t.body, false, t.bodyFont, Anim::None, "centre"));
    }
    return d;
}

Document lowerThird(const Look& L) {
    Document d;
    d.name = "Lower third";
    d.width = 1000, d.height = 180;
    const Theme& t = L.t;
    const Anim a = L.is("minimal") ? Anim::SlideUp : Anim::SlideLeft;
    if (L.is("aurora")) {
        Element bg = rectEl("Bar", 20, 30, 900, 120, Fill::linear({{0, al(t.panel, 0xF0)}, {1, al(mixc(t.panel, t.panel2, 0.6f), 0xE0)}}, 0), L.r(18));
        bg.shadow = {true, 0, 8, 20, hex("#00000080"), ""};
        bg.anim = a;
        add(d, bg);
        Element acc = rectEl("Accent", 20, 30, 12, 120, Fill::linear({{0, t.accent}, {1, t.accent2}}, 90), 6);
        acc.anim = a;
        add(d, acc);
    } else {
        card(d, L, 20, 30, 900, 120, a, "Bar");
    }
    add(d, title(L, "Name", "{name}", 60, 42, 820, 58, 46, a));
    Element r = muted(L, "Role", "Author \xC2\xB7 Artist \xC2\xB7 Broadcaster", 60, 100, 820, 40, 28, a);
    r.animMs = 700;
    add(d, r);
    return d;
}

Document facecam(const Look& L) {
    Document d;
    d.name = "Facecam frame";
    d.width = 680, d.height = 420;
    const Theme& t = L.t;
    // "Frame" first with the same opening in every theme (20, 20, 640 x 360): scene templates fit the camera into it.
    Element f = rectEl("Frame", 20, 20, 640, 360, Fill::none(), L.r(28));
    if (L.is("bookish")) {
        f.radius = L.r(12), f.stroke = t.edge, f.strokeWidth = 10, f.shadow = {true, 0, 6, 18, hex("#1E140A88"), ""};
        add(d, f);
        Element in = rectEl("Inner rule", 32, 32, 616, 336, Fill::none(), L.r(6));
        in.stroke = al(t.page, 0xAA), in.strokeWidth = 2;
        add(d, in);
        card(d, L, 40, 350, 260, 56, Anim::None, "Name tag");
        add(d, title(L, "Name", "{name}", 40, 358, 260, 40, 26, Anim::None, "centre"));
    } else if (L.is("fantasy")) {
        f.radius = L.r(4), f.stroke = t.edge, f.strokeWidth = 8, f.shadow = {true, 0, 8, 24, hex("#000000CC"), ""};
        f.glow = {true, 18, 1.0f, al(t.accent, 0x77), ""};
        add(d, f);
        Element in = rectEl("Crimson rule", 30, 30, 620, 340, Fill::none(), L.r(2));
        in.stroke = al(t.accent, 0xCC), in.strokeWidth = 2;
        add(d, in);
        for (auto [cx, cy] : std::vector<std::pair<float, float>>{{20, 20}, {660, 20}, {20, 380}, {660, 380}}) {
            add(d, diamondEl("Blade corner", cx, cy, 30, Fill::linear({{0, mixc(t.edge, Rgba{255, 255, 255, 255}, 0.4f)}, {1, t.edge}}, 90)));
            Element g = diamondEl("Corner gem", cx, cy, 12, Fill::solid(t.accent));
            g.glow = {true, 12, 1.2f, al(t.accent, 0xAA), ""};
            add(d, g);
        }
        card(d, L, 210, 352, 260, 56, Anim::None, "Name tag");
        add(d, title(L, "Name", "{name}", 210, 360, 260, 40, 26, Anim::None, "centre"));
    } else if (L.is("scifi")) {
        f.radius = L.r(2), f.stroke = al(t.accent, 0x66), f.strokeWidth = 3;
        add(d, f);
        const float len = 70;
        for (int i = 0; i < 4; ++i) {
            const bool right = i % 2, bottom = i / 2;
            const float cx = right ? 668 : 12, cy = bottom ? 388 : 12;
            Element a = rectEl("Bracket", right ? cx - len : cx, bottom ? cy - 6 : cy, len, 6, Fill::solid(t.accent));
            Element b = rectEl("Bracket", right ? cx - 6 : cx, bottom ? cy - len : cy, 6, len, Fill::solid(t.accent));
            a.glow = b.glow = {true, 12, 1.1f, al(t.accent, 0x99), ""};
            add(d, a);
            add(d, b);
        }
        Element tag = rectEl("Name tag", 40, 360, 280, 44, Fill::solid(t.accent), L.r(2));
        tag.glow = {true, 12, 1.0f, al(t.accent, 0x88), ""};
        add(d, tag);
        add(d, text(L, "Name", "{name}", 54, 364, 260, 36, 24, t.onAccent, true, t.headingFont, Anim::None, "left"));
    } else if (L.is("cosy")) {
        f.radius = L.r(22), f.stroke = t.edge, f.strokeWidth = 14, f.shadow = {true, 0, 8, 22, hex("#1E0F0599"), ""};
        add(d, f);
        Element in = rectEl("Inner line", 34, 34, 612, 332, Fill::none(), L.r(12));
        in.stroke = al(t.panel, 0xCC), in.strokeWidth = 2;
        add(d, in);
        Element t1 = rectEl("Washi tape", 0, 4, 110, 32, Fill::solid(al(t.accent, 0xC0)), 2);
        t1.rotation = -18;
        add(d, t1);
        Element t2 = rectEl("Washi tape", 570, 4, 110, 32, Fill::solid(al(t.accent3, 0xC0)), 2);
        t2.rotation = 16;
        add(d, t2);
        card(d, L, 40, 346, 280, 60, Anim::None, "Name tag");
        add(d, text(L, "Name", "{name}", 40, 352, 280, 48, 28, t.accent2, false, t.labelFont, Anim::None, "centre"));
    } else if (L.is("minimal")) {
        f.radius = L.r(12), f.stroke = al(t.title, 0xDD), f.strokeWidth = 4;
        add(d, f);
        card(d, L, 40, 352, 260, 50, Anim::None, "Name tag");
        add(d, text(L, "Name", "{name}", 58, 360, 230, 34, 22, t.title, true, t.headingFont, Anim::None, "left"));
    } else {
        f.stroke = t.accent, f.strokeWidth = 8, f.shadow = {true, 0, 0, 24, al(t.accent, 0x88), ""};
        add(d, f);
        add(d, rectEl("Name tag", 40, 352, 260, 52, Fill::linear({{0, t.accent}, {1, t.accent2}}, 0), L.r(26)));
        add(d, text(L, "Name", "{name}", 40, 358, 260, 40, 28, t.onAccent, true, t.headingFont, Anim::None, "centre"));
    }
    return d;
}

Document socials(const Look& L) {
    Document d;
    d.name = "Socials bar";
    d.width = 1200, d.height = 90;
    if (L.is("aurora")) {
        add(d, rectEl("Bar", 0, 10, 1200, 70, Fill::solid(al(L.t.panel, 0xD0)), L.r(35)));
        add(d, text(L, "Handles", kSocials, 30, 22, 1140, 46, 30, L.t.title, false, L.t.bodyFont, Anim::None, "centre"));
        return d;
    }
    card(d, L, 4, 10, 1192, 70, Anim::None, "Bar");
    add(d, body(L, "Handles", kSocials, 30, 22, 1140, 46, 30, Anim::None, "centre"));
    return d;
}

Document twitchPanel(const Look& L) {
    Document d;
    d.name = "Twitch panel";
    d.width = 320, d.height = 100;
    const Theme& t = L.t;
    if (L.is("aurora")) {
        d.background = Fill::linear({{0, t.panel}, {1, mixc(t.panel2, t.accent3, 0.08f)}}, 0);
        add(d, rectEl("Accent", 0, 92, 320, 8, Fill::linear({{0, t.accent}, {1, t.accent2}}, 0)));
        add(d, text(L, "Label", "ABOUT ME", 0, 26, 320, 48, 36, t.title, true, t.headingFont, Anim::None, "centre"));
        return d;
    }
    if (L.is("bookish")) {
        d.background = Fill::linear({{0, t.page}, {1, t.page2}}, 90);
        Element edge = rectEl("Gold edge", 4, 4, 312, 92, Fill::none(), L.r(4));
        edge.stroke = t.edge, edge.strokeWidth = 3;
        add(d, edge);
        add(d, title(L, "Label", "About me", 0, 24, 320, 52, 38, Anim::None, "centre"));
        return d;
    }
    if (L.is("minimal")) d.background = Fill::solid(t.page);
    else d.background = Fill::linear({{0, t.page}, {1, t.page2}}, 90);
    card(d, L, 8, 8, 304, 84, Anim::None, "Panel");
    Element tl = title(L, "Label", L.is("cosy") ? "About me" : "ABOUT ME", 0, 24, 320, 52, 34, Anim::None, "centre");
    if (L.is("cosy")) tl.font = t.labelFont, tl.fill = Fill::solid(t.accent2), tl.bold = false;
    add(d, tl);
    return d;
}

Document showTitle(const Look& L) {
    Document d;
    d.name = "Show title";
    d.width = 1100, d.height = 220;
    card(d, L, 10, 10, 1080, 200, Anim::Fade);
    badge(d, L, "On air", "ON AIR", 40, 40, 170, 48, Anim::Pop);
    add(d, icon(L, "Microphone", "mic", 960, 50, 100, 100, Anim::Fade));
    add(d, title(L, "Show", "{show}", 40, 96, 900, 66, 54, Anim::SlideLeft));
    Element e = muted(L, "Episode", "{episode}", 40, 158, 900, 40, 28, Anim::SlideLeft);
    e.animMs = 700;
    add(d, e);
    return d;
}

Document chatBox(const Look& L) {
    Document d;
    d.name = "Chat box";
    d.width = 600, d.height = 760;
    const Theme& t = L.t;
    card(d, L, 0, 0, 600, 64, Anim::None, "Header");
    add(d, icon(L, "Chat icon", "chat", 22, 14, 36, 36, Anim::None));
    add(d, label(L, "Label", "LIVE CHAT", L.is("bookish") ? "From the reading room" : L.is("cosy") ? "Chatting by the fire" : "Live chat", 72, 15, 500, 34, Anim::None));
    Element e;
    e.kind = Kind::Chat;
    e.name = "Chat";
    e.x = 0, e.y = 80, e.w = 600, e.h = 680;
    e.size = 28;
    e.chatCount = 8;
    e.chatSide = "left";
    e.font = t.bodyFont;
    if (L.is("aurora")) {
        e.fill = Fill::solid(t.title);
    } else {
        e.fill = Fill::solid(L.light() ? t.title : t.title);
        e.chatBubble = al(t.panel, 0xF0);
        e.chatBubbleBrand = "";
        e.chatNameColours = !L.light();   // names in viewers' colours read on dark cards; ink on light ones
    }
    add(d, e);
    return d;
}

Document nowPlaying(const Look& L) {
    Document d;
    d.name = "Now playing";
    d.width = 760, d.height = 150;
    d.onlyWhilePlaying = true;
    const Anim a = Anim::SlideLeft;
    card(d, L, 10, 10, 740, 130, a);
    add(d, icon(L, "Music", "music", 40, 35, 80, 80, a));
    add(d, label(L, "Label", "NOW PLAYING", "Now playing", 145, 22, 580, 34, a));
    add(d, title(L, "Song", "{song}", 145, 52, 580, 48, 38, a));
    add(d, muted(L, "Artist", "{artist}", 145, 98, 580, 34, 26, a));
    return d;
}

Document logoWatermark() {
    Document d;
    d.name = "Logo watermark";
    d.width = 400, d.height = 400;
    Element e;
    e.kind = Kind::Image;
    e.name = "Logo";
    e.x = 0, e.y = 0, e.w = 400, e.h = 400;
    e.file = kBrandLogo;
    e.cover = false;
    e.fill = Fill::none();
    e.opacity = 0.6f;
    add(d, e);
    return d;
}
}  // namespace

const std::vector<ThemedTemplate>& themedTemplateList() {
    static const std::vector<ThemedTemplate> v = {
        {"starting-soon", "Starting Soon", "Holding screens"},
        {"be-right-back", "Be Right Back", "Holding screens"},
        {"ending", "Ending", "Holding screens"},
        {"lower-third", "Lower third", "On stream"},
        {"facecam-frame", "Facecam frame", "On stream"},
        {"socials-bar", "Socials bar", "On stream"},
        {"chat-box", "Chat box (your Twitch chat, live)", "On stream"},
        {"now-playing", "Now playing (your music's title and artist)", "On stream"},
        {"show-title", "Show title (your radio show and episode)", "On stream"},
        {"logo-watermark", "Logo watermark (your brand logo)", "On stream"},
        {"writing", "Writing (book, chapter, words, characters, sprints; each part on or off)", "Writing"},
        {"twitch-panel", "Twitch panel (320 \xC3\x97 100)", "Panels"},
        {"offline-banner", "Offline banner (1920 \xC3\x97 1080)", "Panels"},
    };
    return v;
}

Document themedTemplate(const std::string& id, const Theme& t) {
    const Look L(t);
    Document d;
    if (id == "starting-soon") d = holding(L, id, "Starting Soon");
    else if (id == "be-right-back") d = holding(L, id, "Be Right Back");
    else if (id == "ending") d = holding(L, id, "Ending");
    else if (id == "offline-banner") d = holding(L, id, "Offline banner");
    else if (id == "lower-third") d = lowerThird(L);
    else if (id == "facecam-frame") d = facecam(L);
    else if (id == "socials-bar") d = socials(L);
    else if (id == "twitch-panel") d = twitchPanel(L);
    else if (id == "show-title") d = showTitle(L);
    else if (id == "chat-box") d = chatBox(L);
    else if (id == "now-playing") d = nowPlaying(L);
    else if (id == "logo-watermark") d = logoWatermark();
    else if (id == "writing") d = writingCardDesign(t.id);
    else {
        d.name.clear();
        return d;
    }
    // The NorthRay theme follows the brand kit (its colours are the kit's defaults), as its templates always have.
    if (t.builtIn && t.id == "northray") linkToBrand(d, defaultBrand());
    d.templateId = id;
    d.theme = t.id;
    d.followTheme = true;
    if (d.writing.on) d.writing.look = t.id;
    return d;
}

void styleElement(Element& e, const Theme& t) {
    const Look L(t);
    auto clearLinks = [&] {
        e.fill.brand.clear();
        for (auto& s : e.fill.stops) s.brand.clear();
        e.strokeBrand.clear();
        e.shadow.brand.clear();
        e.glow.brand.clear();
    };
    clearLinks();
    switch (e.kind) {
    case Kind::Text:
    case Kind::Timer: {
        const bool big = e.size >= 40 || e.bold;
        e.font = big ? t.headingFont : t.bodyFont;
        e.fill = Fill::solid(big ? t.title : t.body);
        e.glow.on = false;
        if (big && (L.is("aurora") || L.is("fantasy") || L.is("scifi"))) e.glow = title(L, "", "", 0, 0, 1, 1, e.size, Anim::None).glow;
        break;
    }
    case Kind::Chat:
        e.font = t.bodyFont;
        e.fill = Fill::solid(t.title);
        e.chatBubble = al(t.panel, 0xF0);
        e.chatBubbleBrand.clear();
        break;
    case Kind::Rect: {
        Document tmp;
        card(tmp, L, e.x, e.y, e.w, e.h, Anim::None);
        const Element& c = tmp.elements.front();
        e.fill = c.fill, e.stroke = c.stroke, e.strokeWidth = c.strokeWidth, e.radius = std::min(c.radius, std::min(e.w, e.h) / 2);
        e.shadow = c.shadow;
        e.glow = c.glow;
        break;
    }
    case Kind::Ellipse:
        e.fill = Fill::linear({{0, t.accent}, {1, t.accent2}}, 135);
        e.stroke = {0, 0, 0, 0}, e.strokeWidth = 0;
        break;
    case Kind::Image: {
        if (e.file == kBrandLogo) break;   // a logo stays as it is
        Document tmp;
        portrait(tmp, L, "p", "x", e.x, e.y, e.w, e.h, Anim::None, 0);
        for (auto& p : tmp.elements)
            if (p.kind == Kind::Image) {
                e.mask = L.is("cosy") ? "" : p.mask;
                e.stroke = p.stroke, e.strokeWidth = p.strokeWidth, e.glow = p.glow, e.shadow = p.shadow;
                if (L.is("cosy")) e.stroke = hex("#FFFDF8"), e.strokeWidth = 8, e.shadow = {true, 2, 6, 12, hex("#2B160A66"), ""}, e.radius = 3;
            }
        break;
    }
    case Kind::Icon:
        e.fill = icon(L, "", "star", 0, 0, 1, 1, Anim::None).fill;
        e.glow = icon(L, "", "star", 0, 0, 1, 1, Anim::None).glow;
        break;
    case Kind::Qr:
        e.fill = Fill::solid(L.light() ? t.panel : hex("#FFFFFF"));
        e.stroke = L.light() ? t.title : hex("#000000");
        break;
    }
}

Document retheme(const Document& d, const Theme& from, const Theme& to) {
    auto restyleAll = [&](Document x) {
        for (auto& e : x.elements) styleElement(e, to);
        x.theme = to.id;
        if (x.writing.on) x.writing.look = to.id;
        return x;
    };
    if (d.templateId.empty()) return restyleAll(d);
    if (d.templateId == "writing" || d.writing.on) {   // drawn afresh each time in its theme; only what was added restyles
        Document x = restyleAll(d);
        x.writing.look = to.id;
        return x;
    }
    const Document was = themedTemplate(d.templateId, from), now = themedTemplate(d.templateId, to);
    if (now.name.empty()) return restyleAll(d);
    Document out = now;
    out.name = d.name;
    out.followTheme = d.followTheme;
    out.grid = d.grid, out.guidesX = d.guidesX, out.guidesY = d.guidesY;
    out.onlyWhilePlaying = d.onlyWhilePlaying, out.onlyWhileSprint = d.onlyWhileSprint, out.onlyCelebrating = d.onlyCelebrating;
    // The page's own size: the new design scaled to it.
    if ((d.width != now.width || d.height != now.height) && now.width > 0 && now.height > 0) {
        const float sx = (float)d.width / now.width, sy = (float)d.height / now.height, s = std::min(sx, sy);
        for (auto& e : out.elements) {
            e.x *= sx, e.y *= sy, e.w *= sx, e.h *= sy;
            e.size *= s, e.strokeWidth *= s, e.radius *= s;
        }
        out.width = d.width, out.height = d.height;
    }
    std::set<std::string> fromTemplate;
    for (auto& e : was.elements) fromTemplate.insert(e.id);
    for (const Element& e : d.elements) {
        if (!fromTemplate.count(e.id)) continue;
        const Element* before = was.find(e.id);
        if (!before || before->name != e.name) continue;
        // Words changed by hand go to the element of the same name in the new design; one hidden stays hidden.
        for (auto& n : out.elements) {
            if (n.name != e.name || n.kind != e.kind) continue;
            if ((e.kind == Kind::Text || e.kind == Kind::Timer) && e.text != before->text) n.text = e.text;
            if (e.kind == Kind::Timer) {
                n.timerMode = e.timerMode, n.timerSeconds = e.timerSeconds, n.timerUntil = e.timerUntil, n.timerEnd = e.timerEnd;
                n.timerScene = e.timerScene, n.timerDate = e.timerDate, n.timerPulse = e.timerPulse;
            }
            if (e.kind == Kind::Image && e.file != before->file) n.file = e.file;
            if (!e.visible) n.visible = false;
            break;
        }
    }
    for (const Element& e : d.elements) {   // added by hand: kept, in the new look
        if (fromTemplate.count(e.id)) continue;
        Element x = e;
        styleElement(x, to);
        x.id.clear();
        add(out, x);
    }
    return out;
}

bool recogniseTemplate(Document& d) {
    if (d.writing.on) {
        d.templateId = "writing";
        d.theme = d.writing.look;
        d.followTheme = true;
        return true;
    }
    std::string base = d.name, theme = "northray";
    if (base.size() > 10 && base.compare(base.size() - 10, 10, " (bookish)") == 0) {
        base.resize(base.size() - 10);
        theme = "bookish";
    }
    for (auto& tt : themedTemplateList()) {
        const Document ref = themedTemplate(tt.id, themeById({}, theme));
        if (ref.name != base) continue;
        // Most of the template's elements still there (by name and kind): it's that design, perhaps changed a little.
        int found = 0;
        for (auto& r : ref.elements)
            for (auto& e : d.elements)
                if (e.name == r.name && e.kind == r.kind) {
                    ++found;
                    break;
                }
        if (ref.elements.empty() || found * 10 < (int)ref.elements.size() * 7) return false;
        d.templateId = tt.id;
        d.theme = theme;
        d.followTheme = true;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------- the old template list

namespace {
// Writing progress: the chapter's own words, on the CHAPTER label's line at the right.
Element chapterWordsElement() {
    Element e = textEl("Chapter words", "{chapterline}", 50, 40, 1000, 30, 22, hex("#3FCF8E"), true, "right");
    e.anim = Anim::SlideUp;
    return e;
}
}  // namespace

bool addChapterWords(Document& d) {
    if (d.name != "Writing progress") return false;
    const Element* lbl = nullptr;
    for (const Element& e : d.elements) {
        if (e.kind == Kind::Text && (e.text.find("{chapterline}") != std::string::npos || e.text.find("{chapterwords}") != std::string::npos))
            return false;   // it has one (or they've put it somewhere of their own)
        if (e.kind == Kind::Text && e.name == "Label" && e.text == "CHAPTER") lbl = &e;
    }
    if (!lbl) return false;   // changed past recognising: left alone
    Element e = chapterWordsElement();
    e.x = lbl->x, e.y = lbl->y, e.w = lbl->w, e.h = lbl->h;
    e.size = lbl->size;
    e.font = lbl->font;   // brand fonts are in the name ("brand:heading")
    e.fill = lbl->fill;   // with its brand link
    e.bold = lbl->bold;
    e.italic = lbl->italic;
    e.anim = lbl->anim;
    e.animMs = lbl->animMs;
    return !add(d, e).empty();
}

// Every template in the NorthRay theme (the ids scene templates use) and every one in the Bookish theme ("-bookish"),
// as listed before themes; the designer now offers the stream theme's set (themedTemplateList).
const std::vector<Template>& templates() {
    static const std::vector<Template> t = [] {
        std::vector<Template> v;
        static std::vector<std::string> ids;
        ids.clear();
        ids.reserve(themedTemplateList().size() * 2);
        const char* bk = "Bookish (parchment, serif, ink and gold)";
        for (auto& tt : themedTemplateList()) {
            Document d = themedTemplate(tt.id, themeById({}, "northray"));
            v.push_back({tt.id, tt.name, tt.group, d});
        }
        for (auto& tt : themedTemplateList()) {
            if (std::string(tt.id) == "logo-watermark") continue;
            Document d = themedTemplate(tt.id, themeById({}, "bookish"));
            d.name += " (bookish)";
            ids.push_back(std::string(tt.id) + "-bookish");
            v.push_back({ids.back().c_str(), tt.name, bk, d});
        }
        std::stable_sort(v.begin(), v.end(), [](const Template& x, const Template& y) {
            auto rank = [](const char* g) {
                const std::string s(g);
                return s == "Holding screens" ? 0 : s == "On stream" ? 1 : s == "Writing" ? 2 : s.rfind("Bookish", 0) == 0 ? 3 : 4;
            };
            return rank(x.group) < rank(y.group);
        });
        return v;
    }();
    return t;
}

// ---------------------------------------------------------------------------------------------- the Writing card (0.20)

const std::vector<WritingPart>& writingParts() {
    static const std::vector<WritingPart> p = {{"book", "Book"},          {"chapter", "Chapter"},   {"progress", "Word progress"},
                                               {"viewpoint", "Viewpoint"}, {"cast", "Cast"},         {"sprint", "Sprint timer"},
                                               {"celebration", "Celebration"}, {"streamwords", "Stream words"}};
    return p;
}

namespace {
using Values = std::vector<std::pair<std::string, std::string>>;
std::string valueOf(const Values& v, const std::string& key) {
    for (auto it = v.rbegin(); it != v.rend(); ++it)
        if (it->first == key) return it->second;
    return {};
}
int castCount(const Values& v) {
    int n = 0;
    while (n < 60 && !valueOf(v, "cast" + std::to_string(n + 1)).empty()) ++n;
    return n;
}
bool partOn(const Document& d, const std::string& key, const std::vector<std::string>& hidden) {
    return std::find(d.writing.parts.begin(), d.writing.parts.end(), key) != d.writing.parts.end() &&
           std::find(hidden.begin(), hidden.end(), key) == hidden.end();
}
// Whether a part has something to show now.
bool partHas(const std::string& key, const Values& v, const Clock& c) {
    if (key == "book") return !valueOf(v, "book").empty();
    if (key == "chapter") return !valueOf(v, "chapter").empty();
    if (key == "progress") return !valueOf(v, "today").empty() || !valueOf(v, "chapterline").empty();
    if (key == "viewpoint") return !valueOf(v, "viewpoint").empty();
    if (key == "cast") return castCount(v) > 0;
    if (key == "sprint") return c.sprintLeftMs >= 0;
    if (key == "celebration") return c.celebrateMs >= 0 && c.celebrateMs < kCelebrationMs;
    if (key == "streamwords") return !valueOf(v, "streamline").empty();
    return false;
}
float partHeight(const std::string& key, const Look& L) {
    const bool tall = L.is("bookish"), polaroid = L.is("cosy");
    if (key == "book") return 230;
    if (key == "chapter") return 196;
    if (key == "progress") return 150;
    if (key == "viewpoint") return tall ? 190.0f : polaroid ? 196.0f : 170.0f;
    if (key == "cast") return tall ? 270.0f : polaroid ? 262.0f : 236.0f;
    if (key == "sprint") return 160;
    if (key == "celebration") return 110;
    return 96;   // streamwords
}
}  // namespace

bool writingMoving(const Document& d, const Values& values, const std::vector<std::string>& hidden) {
    if (!d.writing.on || !partOn(d, "cast", hidden)) return false;
    const float inner = d.width - 20 - 80;
    return castCount(values) * kCastSlot > inner;
}

Document buildWritingCard(const Document& d, const Values& values, const Clock& c, const std::vector<std::string>& hidden, const Theme* theme) {
    if (!d.writing.on) return d;
    const Theme th = theme ? *theme : themeById({}, d.writing.look);
    const Look L(th);
    const Theme& t = L.t;
    const bool bk = L.is("bookish");
    Document out;
    out.name = d.name;
    out.width = d.width, out.height = d.height;
    out.writing = d.writing;
    out.background = Fill::none();
    out.templateId = d.templateId, out.theme = d.theme, out.followTheme = d.followTheme;
    std::vector<std::string> shown;
    for (auto& p : writingParts())
        if (partOn(d, p.key, hidden) && partHas(p.key, values, c)) shown.push_back(p.key);
    if (shown.empty()) return out;   // nothing to show: the card hides (and what was added to it with it)
    const float W = (float)d.width - 20, x0 = 10 + 40, cw = W - 80;
    float total = 24;
    for (auto& k : shown) total += partHeight(k, L);
    total += 10;
    total = std::min(total, (float)d.height - 20);
    const Anim a = L.is("minimal") ? Anim::SlideUp : Anim::SlideLeft;
    card(out, L, 10, 10, W, total, a);
    float y = 10 + 24;
    bool first = true;
    for (const std::string& k : shown) {
        const float h = partHeight(k, L);
        if (y + h > (float)d.height - 10) break;   // no room left on the page
        if (!first) {   // a fine line between parts
            Rgba lc = bk ? al(t.edge, 0x66) : L.is("aurora") ? hex("#FFFFFF1A") : L.is("scifi") ? al(t.accent, 0x40) : L.is("fantasy") ? al(t.edge, 0x44) : al(t.edge, L.light() ? 0x88 : 0xFF);
            Element line = rectEl("Between parts", x0, y - 6, cw, bk ? 1.5f : 1.0f, Fill::solid(lc));
            if (L.is("fantasy")) line.fill = Fill::linear({{0, al(t.edge, 0)}, {0.5f, al(t.edge, 0x77)}, {1, al(t.edge, 0)}}, 0);
            line.anim = a;
            add(out, line);
        }
        first = false;
        if (k == "book") {
            Element cover = rectEl("Cover (until there is a picture)", x0, y + 14, 130, 192,
                                   bk ? Fill::linear({{0, hex("#6B2E1E")}, {1, hex("#3E1A12")}}, 135) : Fill::linear({{0, t.accent}, {1, t.accent2}}, 135), 8);
            cover.shadow = {true, 0, 6, 18, hex("#00000080"), ""};
            cover.anim = a;
            if (L.is("cosy")) cover.rotation = -3;
            add(out, cover);
            Element pic = picEl("Cover", "{cover}", x0, y + 14, 130, 192, 8);
            if (bk || L.is("fantasy")) pic.stroke = t.edge, pic.strokeWidth = 2;
            if (L.is("scifi")) pic.stroke = t.accent, pic.strokeWidth = 2, pic.glow = {true, 12, 1.0f, al(t.accent, 0x77), ""};
            if (L.is("cosy")) pic.rotation = -3;
            pic.anim = a;
            add(out, pic);
            const float tx = x0 + 160, tw = cw - 160;
            add(out, label(L, "Book label", "NOW WRITING", "Now writing", tx, y + 18, tw, 32, a));
            add(out, title(L, "Book", "{book}", tx, y + 54, tw, 104, 44, a));
            add(out, muted(L, "Series", "{series}", tx, y + 162, tw, 40, 26, a));
        } else if (k == "chapter") {
            const bool pic = !valueOf(values, "placepic").empty();
            const float tw = pic ? cw - 180 : cw;
            add(out, label(L, "Status", "{status}", "{status}", x0, y + 4, tw, 32, a));
            add(out, title(L, "Chapter", "{chapter}", x0, y + 38, tw, 64, 44, a));
            add(out, body(L, "Where and when", "{place}  \xC2\xB7  {storyday}", x0, y + 108, tw, 40, 28, a));
            add(out, muted(L, "Target", "{targetline}", x0, y + 148, tw, 36, 24, a));
            if (pic) {
                Element p = picEl("Place", "{placepic}", x0 + cw - 160, y + 6, 160, 170, L.r(16));
                if (bk) p.mask = "arch", p.stroke = t.edge, p.strokeWidth = 4;
                else if (L.is("fantasy") || L.is("scifi")) p.mask = "hexagon", p.stroke = L.is("scifi") ? t.accent : t.edge, p.strokeWidth = 3;
                else if (L.is("cosy")) p.stroke = hex("#FFFDF8"), p.strokeWidth = 8, p.rotation = 3, p.shadow = {true, 2, 6, 12, hex("#2B160A66"), ""};
                else if (L.is("minimal")) p.radius = L.r(10);
                if (L.is("aurora")) p.glow = {true, 16, 1.0f, al(t.accent2, 0x66), ""};
                if (L.is("fantasy")) p.glow = {true, 14, 1.0f, al(t.accent, 0x77), ""};
                p.anim = a;
                add(out, p);
            }
        } else if (k == "progress") {
            add(out, label(L, "Today label", "TODAY", "Today", x0, y + 4, cw, 32, a));
            add(out, label(L, "Chapter words", "{chapterline}", "{chapterline}", x0, y + 4, cw, 32, a, "right"));
            if (L.is("scifi")) out.elements.back().text = "{chapterline}";
            add(out, body(L, "Today", "{today}", x0, y + 40, cw, 46, 34, a));
            add(out, muted(L, "Streak", "{bookline}  \xC2\xB7  {streakline}", x0, y + 92, cw, 40, 24, a));
        } else if (k == "viewpoint") {
            if (bk) portrait(out, L, "Viewpoint picture", "{viewpointpic}", x0, y + 4, 120, 170, a, 500);
            else portrait(out, L, "Viewpoint picture", "{viewpointpic}", x0 + (L.is("cosy") ? 8 : 0), y + 8, 140, 140, a, 500);
            const float tx = x0 + 170, tw = cw - 170;
            add(out, label(L, "Viewpoint label", "THROUGH THE EYES OF", "Through the eyes of", tx, y + 44, tw, 32, a));
            add(out, title(L, "Viewpoint", "{viewpoint}", tx, y + 80, tw, 64, 42, a));
        } else if (k == "cast") {
            add(out, label(L, "Cast label", "IN THIS CHAPTER", "In this chapter", x0, y + 4, cw, 32, a));
            const int n = castCount(values);
            const float rowY = y + 44, rowH = h - 50;
            const float full = n * kCastSlot;
            std::vector<float> starts;
            if (full <= cw) starts.push_back(x0 + (cw - full) / 2);
            else {   // scrolling, looping: two copies a slot apart
                const float period = full + kCastSlot;
                const float off = std::fmod((float)std::max<std::int64_t>(0, c.elapsedMs) / 1000.0f * kCastSpeed, period);
                const float base = d.writing.castRightToLeft ? x0 - off : x0 + off - period;
                starts = {base, base + period};
                if (!d.writing.castRightToLeft) starts.push_back(base - period);
            }
            const std::size_t before = out.elements.size();
            for (float sx : starts)
                for (int i = 0; i < n; ++i) {
                    const float x = sx + i * kCastSlot;
                    if (x + kCastSlot < x0 - 4 || x > x0 + cw + 4) continue;   // off the row
                    const std::string num = std::to_string(i + 1);
                    const std::string pic = "{cast" + num + "pic}", who = "{cast" + num + "}";
                    float nameY = rowY + 122;
                    if (bk) {
                        portrait(out, L, "Cast " + num, pic, x + 27, rowY, 96, 136, Anim::None, 0);
                        nameY = rowY + 144;
                    } else if (L.is("cosy")) {
                        portrait(out, L, "Cast " + num, pic, x + 30, rowY + 8, 90, 90, Anim::None, 0);
                        nameY = rowY + 136;
                    } else {
                        portrait(out, L, "Cast " + num, pic, x + 20, rowY + 4, 110, 110, Anim::None, 0);
                    }
                    Element tn = L.is("aurora") ? textEl(("Cast name " + num).c_str(), who, x + 4, nameY, kCastSlot - 8, 56, 20, t.title, true, "centre")
                                                : body(L, ("Cast name " + num).c_str(), who, x + 4, nameY, kCastSlot - 8, 64, 20, Anim::None, "centre");
                    if (L.is("cosy")) tn.font = t.labelFont, tn.fill = Fill::solid(t.accent2);
                    add(out, tn);
                }
            for (std::size_t i = before; i < out.elements.size(); ++i) {
                Element& e = out.elements[i];
                e.clipX = x0, e.clipY = rowY - 10, e.clipW = cw, e.clipH = rowH + 10;
                e.anim = Anim::None;
            }
        } else if (k == "sprint") {
            add(out, label(L, "Sprint label", "WRITING SPRINT", bk ? "A writing sprint" : "Writing sprint", x0, y + 4, cw, 32, a));
            Element tm;
            tm.kind = Kind::Timer;
            tm.name = "Time left";
            tm.timerMode = "sprint";
            tm.timerEnd = bk ? "Pens down!" : L.is("fantasy") ? "The hour is up!" : L.is("scifi") ? "TIME" : "Time!";
            tm.x = x0, tm.y = y + 38, tm.w = cw, tm.h = 74;
            tm.size = 62;
            tm.bold = true;
            tm.align = "left";
            tm.font = t.headingFont;
            if (bk) tm.fill = Fill::solid(t.title);
            else if (L.is("aurora")) tm.fill = Fill::linear({{0, t.title}, {1, mixc(t.title, t.accent, 0.3f)}}, 90), tm.glow = {true, 22, 1.2f, al(t.accent, 0x66), ""};
            else {
                tm.fill = Fill::solid(t.title);
                tm.glow = title(L, "", "", 0, 0, 1, 1, 62, Anim::None).glow;
                if (L.is("scifi")) tm.font = t.labelFont, tm.fill = Fill::solid(t.accent);
            }
            tm.anim = Anim::Pop;
            add(out, tm);
            add(out, muted(L, "Sprint words", "{sprintline}", x0, y + 114, cw, 38, 26, a));
        } else if (k == "celebration") {
            Fill bandFill = bk ? Fill::linear({{0, hex("#E8C46A66")}, {1, hex("#E8C46A22")}}, 0)
                               : L.is("aurora") ? Fill::linear({{0, al(t.accent, 0xAA)}, {1, al(t.accent2, 0xAA)}}, 0)
                                                : Fill::linear({{0, al(t.accent, 0x66)}, {1, al(t.accent2, 0x22)}}, 0);
            Element band = rectEl("Celebration", x0 - 10, y + 2, cw + 20, h - 14, bandFill, L.r(18));
            band.anim = Anim::Pop;
            add(out, band);
            Element ic = icon(L, "Sparkle", bk || L.is("fantasy") ? "star" : L.is("cosy") ? "heart" : "sparkle", x0 + 8, y + 22, 56, 56, Anim::Pop);
            if (L.is("aurora")) ic.fill = Fill::solid(hex("#FFFFFF")), ic.glow = {true, 14, 1.0f, hex("#FFFFFF88"), ""};
            add(out, ic);
            Element tw = textEl("Celebration words", "{celebration}", x0 + 80, y + 14, cw - 90, 72, 32, L.is("aurora") ? hex("#FFFFFF") : t.title, true, "left");
            tw.font = t.headingFont;
            if (bk) tw.italic = true;
            tw.anim = Anim::Pop;
            add(out, tw);
        } else if (k == "streamwords") {
            add(out, label(L, "Stream label", "THIS STREAM", "This stream", x0, y + 4, cw, 32, a));
            add(out, body(L, "Stream words", "{streamline}", x0, y + 38, cw, 44, 32, a));
        }
        y += h;
    }
    if (th.builtIn && th.id == "northray") linkToBrand(out, defaultBrand());   // NorthRay follows the brand kit
    for (const Element& e : d.elements) out.elements.push_back(e);   // anything added in the designer, on top
    return out;
}

Document writingCardDesign(const std::string& theme, int width) {
    Document d;
    d.name = "Writing";
    d.width = std::clamp(width, 400, 3000);
    d.height = 1500;
    d.writing.on = true;
    for (auto& p : writingParts()) d.writing.parts.push_back(p.key);
    d.writing.look = validThemeId(theme) ? theme : "northray";
    return d;
}

bool fromOldWritingTemplate(Document& d) {
    if (d.writing.on) return false;
    static const std::vector<std::tuple<const char*, const char*, std::vector<const char*>>> old = {
        {"Book card", "{book}", {"book"}},
        {"Writing progress", "{today}", {"progress"}},
        {"Chapter card", "{chapter}", {"chapter"}},
        {"Character card", "{viewpointpic}", {"viewpoint"}},
        {"Cast list", "{cast1}", {"cast"}},
        {"Sprint timer", "{sprintline}", {"sprint"}},
        {"Celebration", "{celebration}", {"celebration"}}};
    std::string base = d.name;
    bool bk = false;
    if (base.size() > 10 && base.compare(base.size() - 10, 10, " (bookish)") == 0) { bk = true; base.resize(base.size() - 10); }
    for (auto& [name, mark, parts] : old) {
        if (base != name) continue;
        bool shows = false;
        for (auto& e : d.elements) shows = shows || e.text.find(mark) != std::string::npos;
        if (!shows) return false;
        Document w = writingCardDesign(bk ? "bookish" : "northray", d.width);
        w.name = d.name;
        w.width = d.width;
        w.height = d.height;
        w.writing.parts.assign(parts.begin(), parts.end());
        w.templateId = "writing";
        w.theme = w.writing.look;
        w.followTheme = true;
        d = w;
        return true;
    }
    return false;
}

}  // namespace ncap::design

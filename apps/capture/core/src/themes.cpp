// NorthRay Capture: the looks and themes of the ready-made designs (0.21), the templates, and the Writing card.
#include "ncap/design.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>
#include <vector>

namespace ncap::design {

using sourceutil::parseColour;
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
// Writing progress: the chapter's own words, on the CHAPTER label's line at the right.
Element chapterWordsElement() {
    Element e = textEl("Chapter words", "{chapterline}", 50, 40, 1000, 30, 22, "#3FCF8E", true, "right");
    e.anim = Anim::SlideUp;
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

// ---- The two looks for the writing, chat and now-playing templates (0.17): aurora (night panels, aurora light, glowing
// titles, gradient labels, round portraits) and bookish (parchment, serif type, ink and gold, arched portraits).
// Aurora's colours are the default brand kit's, so it follows the kit; bookish keeps its own.
constexpr const char* kSerif = "Georgia";
void lookCard(Document& d, bool bk, float x, float y, float w, float h, Anim a, const char* name = "Card") {
    Element c = rectEl(name, x, y, w, h,
                       bk ? Fill::linear({{0, hex("#F7EDD5")}, {1, hex("#EAD7AC")}}, 90) : Fill::linear({{0, hex("#0B1A2EF2")}, {1, hex("#10263EEE")}}, 90),
                       bk ? 12.0f : 28.0f);
    c.stroke = hex(bk ? "#A8853A" : "#3FCF8E44");
    c.strokeWidth = bk ? 3.0f : 2.0f;
    c.shadow = bk ? Shadow{true, 0, 8, 22, hex("#1E140A80"), ""} : Shadow{true, 0, 10, 30, hex("#00000099"), ""};
    c.anim = a;
    add(d, c);
    if (bk) {   // a fine second rule inside the edge, as on a book's title page
        Element r = rectEl("Inner rule", x + 9, y + 9, w - 18, h - 18, Fill::none(), 6);
        r.stroke = hex("#A8853A99");
        r.strokeWidth = 1.5f;
        r.anim = a;
        add(d, r);
    } else {   // aurora light, kept inside the card
        Element g1 = ellipseEl("Aurora light (violet)", x + w * 0.5f, y, w * 0.5f, h, Fill::radial({{0, hex("#7A5CFF30")}, {1, hex("#7A5CFF00")}}));
        g1.anim = a;
        add(d, g1);
        Element g2 = ellipseEl("Aurora light (green)", x, y + h * 0.2f, w * 0.45f, h * 0.8f, Fill::radial({{0, hex("#3FCF8E26")}, {1, hex("#3FCF8E00")}}));
        g2.anim = a;
        add(d, g2);
    }
}
Element lookLabel(bool bk, const char* name, const char* text, float x, float y, float w, float h, Anim a, const char* align = "left") {
    Element e = textEl(name, text, x, y, w, h, bk ? 24.0f : 22.0f, bk ? "#8C6A22" : "#3FCF8E", !bk, align);
    if (bk) {
        e.font = kSerif;
        e.italic = true;
    } else {
        e.fill = Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#22C3E6")}}, 0);   // gradient letters (0.15)
    }
    e.anim = a;
    return e;
}
Element lookTitle(bool bk, const char* name, const char* text, float x, float y, float w, float h, float size, Anim a, const char* align = "left") {
    Element e = textEl(name, text, x, y, w, h, size, bk ? "#2A1C0E" : "#F4F7FB", true, align);
    if (bk) e.font = kSerif;
    else e.glow = {true, 20, 1.0f, hex("#3FCF8E55"), ""};
    e.anim = a;
    return e;
}
Element lookBody(bool bk, const char* name, const char* text, float x, float y, float w, float h, float size, Anim a, const char* align = "left") {
    Element e = textEl(name, text, x, y, w, h, size, bk ? "#4A3622" : "#C9D6E3", false, align);
    if (bk) e.font = kSerif;
    e.anim = a;
    return e;
}
Element lookMuted(bool bk, const char* name, const char* text, float x, float y, float w, float h, float size, Anim a, const char* align = "left") {
    Element e = textEl(name, text, x, y, w, h, size, bk ? "#7A6243" : "#9FB3C8", false, align);
    if (bk) {
        e.font = kSerif;
        e.italic = true;
    }
    e.anim = a;
    return e;
}
// A rule under a heading: aurora's gradient line, or a gold line with a diamond at its middle.
void lookRule(Document& d, bool bk, float x, float y, float w, Anim a) {
    if (bk) {
        Element l = rectEl("Gold rule", x, y, w, 2, Fill::solid(hex("#A8853A")));
        l.anim = a;
        add(d, l);
        Element o = rectEl("Ornament", x + w / 2 - 6, y - 5, 12, 12, Fill::solid(hex("#A8853A")));
        o.rotation = 45;
        o.anim = a;
        add(d, o);
    } else {
        Element l = rectEl("Accent line", x, y, w, 4, Fill::linear({{0, hex("#3FCF8E")}, {0.5f, hex("#22C3E6")}, {1, hex("#7A5CFF")}}, 0), 2);
        l.anim = a == Anim::None ? Anim::Wipe : a;
        add(d, l);
    }
}
Element picEl(const char* name, const char* file, float x, float y, float w, float h, float radius) {
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
// A portrait: round with a glowing ring (aurora) or arched with a gold edge (bookish), over a backing that shows until
// there's a picture.
void lookPortrait(Document& d, bool bk, const char* name, const char* file, float x, float y, float w, float h, Anim a, int ms) {
    Element back = bk ? rectEl("Portrait backing", x, y, w, h, Fill::linear({{0, hex("#D9C394")}, {1, hex("#C2A66E")}}, 90), w / 2)
                      : ellipseEl("Portrait backing", x, y, w, h, Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 135));
    back.name = std::string(name) + " backing";
    back.anim = a;
    back.animMs = ms;
    add(d, back);
    Element p = picEl(name, file, x, y, w, h, 0);
    p.mask = bk ? "arch" : "circle";
    p.stroke = hex(bk ? "#A8853A" : "#3FCF8E");
    p.strokeWidth = 4;
    if (bk) p.shadow = {true, 0, 4, 12, hex("#1E140A66"), ""};
    else p.glow = {true, 18, 1.2f, hex("#3FCF8E88"), ""};
    p.anim = a;
    p.animMs = ms;
    add(d, p);
}
Element lookIcon(bool bk, const char* name, const char* icon, float x, float y, float w, float h, Anim a) {
    Element ic;
    ic.kind = Kind::Icon;
    ic.icon = icon;
    ic.name = name;
    ic.x = x, ic.y = y, ic.w = w, ic.h = h;
    ic.fill = bk ? Fill::linear({{0, hex("#D9B45A")}, {1, hex("#9A7424")}}, 90) : Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0);
    if (!bk) ic.glow = {true, 16, 1.0f, hex("#3FCF8E66"), ""};
    ic.anim = a;
    return ic;
}

Document chatBox(bool bk) {
    Document d;
    d.name = bk ? "Chat box (bookish)" : "Chat box";
    d.width = 600, d.height = 760;
    lookCard(d, bk, 0, 0, 600, 64, Anim::None, "Header");
    Element ic = lookIcon(bk, "Chat icon", "chat", 22, 14, 36, 36, Anim::None);
    add(d, ic);
    add(d, lookLabel(bk, "Label", bk ? "From the reading room" : "LIVE CHAT", 72, 15, 500, 34, Anim::None));
    Element e;
    e.kind = Kind::Chat;
    e.name = "Chat";
    e.x = 0, e.y = 80, e.w = 600, e.h = 680;
    e.size = 28;
    e.chatCount = 8;
    e.chatSide = "left";
    if (bk) {
        e.font = kSerif;
        e.fill = Fill::solid(hex("#2A1C0E"));
        e.chatBubble = hex("#F7EDD5F0");
        e.chatBubbleBrand = "";
    } else {
        e.fill = Fill::solid(hex("#F4F7FB"));
    }
    add(d, e);
    return d;
}
Document nowPlaying(bool bk) {
    Document d;
    d.name = bk ? "Now playing (bookish)" : "Now playing";
    d.width = 760, d.height = 150;
    d.onlyWhilePlaying = true;
    const Anim a = Anim::SlideLeft;
    lookCard(d, bk, 10, 10, 740, 130, a);
    add(d, lookIcon(bk, "Music", "music", 40, 35, 80, 80, a));
    add(d, lookLabel(bk, "Label", bk ? "Now playing" : "NOW PLAYING", 145, 22, 580, 34, a));
    add(d, lookTitle(bk, "Song", "{song}", 145, 52, 580, 48, 38, a));
    add(d, lookMuted(bk, "Artist", "{artist}", 145, 98, 580, 34, 26, a));
    return d;
}

// ---- The bookish look for the holding screens and the rest of the stream (0.18).
// A full page of paper: warm parchment, darker towards the edges, inside a gold double frame.
void bookishPage(Document& d) {
    const float W = (float)d.width, H = (float)d.height;
    d.background = Fill::linear({{0, hex("#F5E8C8")}, {1, hex("#E4CD9A")}}, 90);
    add(d, ellipseEl("Paper light", W * 0.1f, H * 0.05f, W * 0.8f, H * 0.9f, Fill::radial({{0, hex("#FFF8E655")}, {1, hex("#FFF8E600")}})));
    const float m = std::min(W, H) * 0.037f;
    Element outer = rectEl("Gold frame", m, m, W - 2 * m, H - 2 * m, Fill::none(), 8);
    outer.stroke = hex("#A8853A");
    outer.strokeWidth = 4;
    add(d, outer);
    Element inner = rectEl("Inner rule", m + 14, m + 14, W - 2 * m - 28, H - 2 * m - 28, Fill::none(), 4);
    inner.stroke = hex("#A8853A99");
    inner.strokeWidth = 1.5f;
    add(d, inner);
    for (auto [cx, cy] : std::vector<std::pair<float, float>>{{m + 7, m + 7}, {W - m - 7, m + 7}, {m + 7, H - m - 7}, {W - m - 7, H - m - 7}}) {
        Element o = rectEl("Corner ornament", cx - 9, cy - 9, 18, 18, Fill::solid(hex("#A8853A")));
        o.rotation = 45;
        add(d, o);
    }
}
Document holdingBookish(const char* name, const char* title, const char* sub) {
    Document d;
    d.name = std::string(name) + " (bookish)";
    bookishPage(d);
    Element above = lookLabel(true, "Above", "{name}", 260, 290, 1400, 60, Anim::Fade, "centre");
    above.size = 40;   // a full page: larger than a card's label
    add(d, above);
    Element t = lookTitle(true, "Title", title, 160, 360, 1600, 200, 124, Anim::Pop, "centre");
    add(d, t);
    lookRule(d, true, 760, 590, 400, Anim::Fade);
    Element s = lookMuted(true, "Subtitle", sub, 260, 620, 1400, 80, 44, Anim::Fade, "centre");
    s.animMs = 900;
    add(d, s);
    add(d, lookMuted(true, "Socials", "{twitch}   ·   {youtube}   ·   {instagram}", 260, 880, 1400, 60, 32, Anim::None, "centre"));
    return d;
}
Document lowerThirdBookish() {
    Document d;
    d.name = "Lower third (bookish)";
    d.width = 1000, d.height = 180;
    lookCard(d, true, 20, 30, 900, 120, Anim::SlideLeft, "Bar");
    add(d, lookTitle(true, "Name", "{name}", 60, 42, 820, 58, 46, Anim::SlideLeft));
    Element r = lookMuted(true, "Role", "Author · Artist · Broadcaster", 60, 100, 820, 40, 28, Anim::SlideLeft);
    r.animMs = 700;
    add(d, r);
    return d;
}
Document facecamBookish() {
    Document d;
    d.name = "Facecam frame (bookish)";
    d.width = 680, d.height = 420;
    // "Frame" first, with the same opening as the aurora frame: scene templates fit the camera into it.
    Element f = rectEl("Frame", 20, 20, 640, 360, Fill::none(), 12);
    f.stroke = hex("#A8853A");
    f.strokeWidth = 10;
    f.shadow = {true, 0, 6, 18, hex("#1E140A88"), ""};
    add(d, f);
    Element in = rectEl("Inner rule", 32, 32, 616, 336, Fill::none(), 6);
    in.stroke = hex("#F5E8C8AA");
    in.strokeWidth = 2;
    add(d, in);
    lookCard(d, true, 40, 350, 260, 56, Anim::None, "Name tag");
    add(d, lookTitle(true, "Name", "{name}", 40, 358, 260, 40, 26, Anim::None, "centre"));
    return d;
}
Document socialsBookish() {
    Document d;
    d.name = "Socials bar (bookish)";
    d.width = 1200, d.height = 90;
    lookCard(d, true, 4, 10, 1192, 70, Anim::None, "Bar");
    add(d, lookBody(true, "Handles", "{twitch}   ·   {youtube}   ·   {instagram}", 30, 22, 1140, 46, 30, Anim::None, "centre"));
    return d;
}
Document twitchPanelBookish() {
    Document d;
    d.name = "Twitch panel (bookish)";
    d.width = 320, d.height = 100;
    d.background = Fill::linear({{0, hex("#F5E8C8")}, {1, hex("#E4CD9A")}}, 90);
    Element edge = rectEl("Gold edge", 4, 4, 312, 92, Fill::none(), 4);
    edge.stroke = hex("#A8853A");
    edge.strokeWidth = 3;
    add(d, edge);
    add(d, lookTitle(true, "Label", "About me", 0, 24, 320, 52, 38, Anim::None, "centre"));
    return d;
}
Document showTitleBookish() {
    Document d;
    d.name = "Show title (bookish)";
    d.width = 1100, d.height = 220;
    lookCard(d, true, 10, 10, 1080, 200, Anim::Fade);
    Element pill = rectEl("On air", 40, 40, 170, 48, Fill::linear({{0, hex("#9B3A2C")}, {1, hex("#6E2219")}}, 90), 24);
    pill.stroke = hex("#D9B45A");
    pill.strokeWidth = 2;
    pill.anim = Anim::Pop;
    add(d, pill);
    Element oa = textEl("On air text", "ON AIR", 40, 44, 170, 40, 24, "#F5E3B0", true);
    oa.font = kSerif;
    add(d, oa);
    add(d, lookIcon(true, "Microphone", "mic", 960, 50, 100, 100, Anim::Fade));
    add(d, lookTitle(true, "Show", "{show}", 40, 96, 900, 66, 54, Anim::SlideLeft));
    Element e = lookMuted(true, "Episode", "{episode}", 40, 158, 900, 40, 28, Anim::SlideLeft);
    e.animMs = 700;
    add(d, e);
    return d;
}
}  // namespace

bool addChapterWords(Document& d) {
    if (d.name != "Writing progress") return false;
    const Element* label = nullptr;
    for (const Element& e : d.elements) {
        if (e.kind == Kind::Text && (e.text.find("{chapterline}") != std::string::npos || e.text.find("{chapterwords}") != std::string::npos))
            return false;   // it has one (or they've put it somewhere of their own)
        if (e.kind == Kind::Text && e.name == "Label" && e.text == "CHAPTER") label = &e;
    }
    if (!label) return false;   // changed past recognising: left alone
    Element e = chapterWordsElement();
    e.x = label->x, e.y = label->y, e.w = label->w, e.h = label->h;
    e.size = label->size;
    e.font = label->font;   // brand fonts are in the name ("brand:heading")
    e.fill = label->fill;   // with its brand link
    e.bold = label->bold;
    e.italic = label->italic;
    e.anim = label->anim;
    e.animMs = label->animMs;
    return !add(d, e).empty();
}

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
        {
            Document d;
            d.name = "Logo watermark";
            d.width = 400;
            d.height = 400;
            Element e;
            e.kind = Kind::Image;
            e.name = "Logo";
            e.x = 0, e.y = 0, e.w = 400, e.h = 400;
            e.file = kBrandLogo;
            e.cover = false;
            e.fill = Fill::none();
            e.opacity = 0.6f;
            add(d, e);
            v.push_back({"logo-watermark", "Logo watermark (your brand logo)", "On stream", d});
        }
        // The writing, chat and now-playing templates in two looks (0.17): aurora (the same ids as before, so scene
        // templates still find them) and bookish.
        struct Pair {
            const char *id, *name, *bookName;
            Document (*make)(bool);
            const char* group;
        };
        static const Pair pairs[] = {
            {"chat-box", "Chat box (your Twitch chat, live)", "Chat box (parchment cards)", chatBox, "On stream"},
            {"now-playing", "Now playing (your music's title and artist)", "Now playing", nowPlaying, "On stream"},
        };
        static std::vector<std::string> bookIds;   // kept alive: Template holds const char*
        bookIds.clear();
        for (auto& pr : pairs) bookIds.push_back(std::string(pr.id) + "-bookish");
        for (std::size_t i = 0; i < std::size(pairs); ++i) {
            v.push_back({pairs[i].id, pairs[i].name, pairs[i].group, pairs[i].make(false)});
            v.push_back({bookIds[i].c_str(), pairs[i].bookName, "Bookish (parchment, serif, ink and gold)", pairs[i].make(true)});
        }
        // And the rest of a stream in the bookish look (0.18).
        const char* bk = "Bookish (parchment, serif, ink and gold)";
        // The Writing card (0.20), in place of the seven separate writing templates.
        v.push_back({"writing", "Writing (book, chapter, words, characters, sprints; each part on or off)", "Writing", writingCardDesign(false)});
        v.push_back({"writing-bookish", "Writing", bk, writingCardDesign(true)});
        v.push_back({"starting-soon-bookish", "Starting Soon", bk, holdingBookish("Starting Soon", "Starting soon", "Settle in with a cup of tea")});
        v.push_back({"be-right-back-bookish", "Be Right Back", bk, holdingBookish("Be Right Back", "Be right back", "Keep your place, I'll return shortly")});
        v.push_back({"ending-bookish", "Ending", bk, holdingBookish("Ending", "Thank you for reading along", "Until the next chapter")});
        {
            Document d = holdingBookish("Offline banner", "Offline", "{schedule}");
            v.push_back({"offline-banner-bookish", "Offline banner (1920 × 1080)", bk, d});
        }
        v.push_back({"lower-third-bookish", "Lower third", bk, lowerThirdBookish()});
        v.push_back({"facecam-frame-bookish", "Facecam frame", bk, facecamBookish()});
        v.push_back({"socials-bar-bookish", "Socials bar", bk, socialsBookish()});
        v.push_back({"show-title-bookish", "Show title", bk, showTitleBookish()});
        v.push_back({"twitch-panel-bookish", "Twitch panel (320 × 100)", bk, twitchPanelBookish()});
        {   // A radio show's title and episode, with an on-air badge.
            Document d;
            d.name = "Show title";
            d.width = 1100;
            d.height = 220;
            Element card = rectEl("Card", 10, 10, 1080, 200, Fill::linear({{0, hex("#0B1A2EF0")}, {1, hex("#10213AE0")}}, 0), 30);
            card.shadow = {true, 0, 8, 20, hex("#00000080")};
            card.anim = Anim::Fade;
            add(d, card);
            Element pill = rectEl("On air", 40, 40, 170, 48, Fill::solid(hex("#E0566A")), 24);
            pill.anim = Anim::Pop;
            add(d, pill);
            add(d, textEl("On air text", "ON AIR", 40, 44, 170, 40, 24, "#FFFFFF", true));
            Element ic;
            ic.kind = Kind::Icon;
            ic.icon = "mic";
            ic.name = "Microphone";
            ic.x = 960, ic.y = 50, ic.w = 100, ic.h = 100;
            ic.fill = Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 0);
            ic.anim = Anim::Fade;
            add(d, ic);
            Element t = textEl("Show", "{show}", 40, 96, 900, 66, 54, "#F4F7FB", true, "left");
            t.anim = Anim::SlideLeft;
            add(d, t);
            Element e = textEl("Episode", "{episode}", 40, 158, 900, 40, 28, "#9FB3C8", false, "left");
            e.anim = Anim::SlideLeft;
            e.animMs = 700;
            add(d, e);
            v.push_back({"show-title", "Show title (your radio show and episode)", "On stream", d});
        }
        // Keep each group together (the watermark was added last).
        std::stable_sort(v.begin(), v.end(), [](const Template& x, const Template& y) {
            auto rank = [](const char* g) {
                const std::string s(g);
                return s == "Holding screens" ? 0 : s == "On stream" ? 1 : s == "Writing" ? 2 : s.rfind("Bookish", 0) == 0 ? 3 : 4;
            };
            return rank(x.group) < rank(y.group);
        });
        const Brand b = defaultBrand();
        for (auto& x : v) linkToBrand(x.doc, b);
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
float partHeight(const std::string& key, bool bk) {
    if (key == "book") return 230;
    if (key == "chapter") return 196;
    if (key == "progress") return 150;
    if (key == "viewpoint") return bk ? 190.0f : 170.0f;
    if (key == "cast") return bk ? 270.0f : 236.0f;
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

Document buildWritingCard(const Document& d, const Values& values, const Clock& c, const std::vector<std::string>& hidden) {
    if (!d.writing.on) return d;
    const bool bk = d.writing.look == "bookish";
    Document out;
    out.name = d.name;
    out.width = d.width, out.height = d.height;
    out.writing = d.writing;
    out.background = Fill::none();
    std::vector<std::string> shown;
    for (auto& p : writingParts())
        if (partOn(d, p.key, hidden) && partHas(p.key, values, c)) shown.push_back(p.key);
    if (shown.empty()) {   // nothing to show: the card hides (and what was added to it with it)
        return out;
    }
    const float W = (float)d.width - 20, x0 = 10 + 40, cw = W - 80;
    float total = 24;
    for (auto& k : shown) total += partHeight(k, bk);
    total += 10;
    total = std::min(total, (float)d.height - 20);
    const Anim a = Anim::SlideLeft;
    lookCard(out, bk, 10, 10, W, total, a);
    float y = 10 + 24;
    bool first = true;
    for (const std::string& k : shown) {
        const float h = partHeight(k, bk);
        if (y + h > (float)d.height - 10) break;   // no room left on the page
        if (!first) {   // a fine line between parts
            Element line = rectEl("Between parts", x0, y - 6, cw, bk ? 1.5f : 1.0f, Fill::solid(hex(bk ? "#A8853A66" : "#FFFFFF1A")));
            line.anim = a;
            add(out, line);
        }
        first = false;
        if (k == "book") {
            Element cover = rectEl("Cover (until there is a picture)", x0, y + 14, 130, 192,
                                   bk ? Fill::linear({{0, hex("#6B2E1E")}, {1, hex("#3E1A12")}}, 135) : Fill::linear({{0, hex("#3FCF8E")}, {1, hex("#7A5CFF")}}, 135), 8);
            cover.shadow = {true, 0, 6, 18, hex("#00000080"), ""};
            cover.anim = a;
            add(out, cover);
            Element pic = picEl("Cover", "{cover}", x0, y + 14, 130, 192, 8);
            if (bk) pic.stroke = hex("#A8853A"), pic.strokeWidth = 2;
            pic.anim = a;
            add(out, pic);
            const float tx = x0 + 160, tw = cw - 160;
            add(out, lookLabel(bk, "Book label", bk ? "Now writing" : "NOW WRITING", tx, y + 18, tw, 32, a));
            add(out, lookTitle(bk, "Book", "{book}", tx, y + 54, tw, 104, 44, a));
            add(out, lookMuted(bk, "Series", "{series}", tx, y + 162, tw, 40, 26, a));
        } else if (k == "chapter") {
            const bool pic = !valueOf(values, "placepic").empty();
            const float tw = pic ? cw - 180 : cw;
            add(out, lookLabel(bk, "Status", "{status}", x0, y + 4, tw, 32, a));
            add(out, lookTitle(bk, "Chapter", "{chapter}", x0, y + 38, tw, 64, 44, a));
            add(out, lookBody(bk, "Where and when", "{place}  \xC2\xB7  {storyday}", x0, y + 108, tw, 40, 28, a));
            add(out, lookMuted(bk, "Target", "{targetline}", x0, y + 148, tw, 36, 24, a));
            if (pic) {
                Element p = picEl("Place", "{placepic}", x0 + cw - 160, y + 6, 160, 170, 16);
                if (bk) p.mask = "arch", p.stroke = hex("#A8853A"), p.strokeWidth = 4;
                else p.glow = {true, 16, 1.0f, hex("#7A5CFF66"), ""};
                p.anim = a;
                add(out, p);
            }
        } else if (k == "progress") {
            add(out, lookLabel(bk, "Today label", bk ? "Today" : "TODAY", x0, y + 4, cw, 32, a));
            add(out, lookLabel(bk, "Chapter words", "{chapterline}", x0, y + 4, cw, 32, a, "right"));
            add(out, lookBody(bk, "Today", "{today}", x0, y + 40, cw, 46, 34, a));
            add(out, lookMuted(bk, "Streak", "{bookline}  \xC2\xB7  {streakline}", x0, y + 92, cw, 40, 24, a));
        } else if (k == "viewpoint") {
            if (bk) lookPortrait(out, bk, "Viewpoint picture", "{viewpointpic}", x0, y + 4, 120, 170, a, 500);
            else lookPortrait(out, bk, "Viewpoint picture", "{viewpointpic}", x0, y + 8, 140, 140, a, 500);
            const float tx = x0 + 170, tw = cw - 170;
            add(out, lookLabel(bk, "Viewpoint label", bk ? "Through the eyes of" : "THROUGH THE EYES OF", tx, y + 44, tw, 32, a));
            add(out, lookTitle(bk, "Viewpoint", "{viewpoint}", tx, y + 80, tw, 64, 42, a));
        } else if (k == "cast") {
            add(out, lookLabel(bk, "Cast label", bk ? "In this chapter" : "IN THIS CHAPTER", x0, y + 4, cw, 32, a));
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
                    if (bk) lookPortrait(out, bk, ("Cast " + num).c_str(), pic.c_str(), x + 27, rowY, 96, 136, Anim::None, 0);
                    else lookPortrait(out, bk, ("Cast " + num).c_str(), pic.c_str(), x + 20, rowY + 4, 110, 110, Anim::None, 0);
                    Element t = bk ? lookBody(bk, ("Cast name " + num).c_str(), who.c_str(), x + 4, rowY + 144, kCastSlot - 8, 64, 20, Anim::None, "centre")
                                   : textEl(("Cast name " + num).c_str(), who.c_str(), x + 4, rowY + 122, kCastSlot - 8, 56, 20, "#F4F7FB", true, "centre");
                    add(out, t);
                }
            for (std::size_t i = before; i < out.elements.size(); ++i) {
                Element& e = out.elements[i];
                e.clipX = x0, e.clipY = rowY - 10, e.clipW = cw, e.clipH = rowH + 10;
                e.anim = Anim::None;
            }
        } else if (k == "sprint") {
            add(out, lookLabel(bk, "Sprint label", bk ? "A writing sprint" : "WRITING SPRINT", x0, y + 4, cw, 32, a));
            Element t;
            t.kind = Kind::Timer;
            t.name = "Time left";
            t.timerMode = "sprint";
            t.timerEnd = bk ? "Pens down!" : "Time!";
            t.x = x0, t.y = y + 38, t.w = cw, t.h = 74;
            t.size = 62;
            t.bold = true;
            t.align = "left";
            if (bk) t.font = kSerif, t.fill = Fill::solid(hex("#2A1C0E"));
            else t.fill = Fill::linear({{0, hex("#F4F7FB")}, {1, hex("#BFF3DC")}}, 90), t.glow = {true, 22, 1.2f, hex("#3FCF8E66"), ""};
            t.anim = Anim::Pop;
            add(out, t);
            add(out, lookMuted(bk, "Sprint words", "{sprintline}", x0, y + 114, cw, 38, 26, a));
        } else if (k == "celebration") {
            Element band = rectEl("Celebration", x0 - 10, y + 2, cw + 20, h - 14,
                                  bk ? Fill::linear({{0, hex("#E8C46A66")}, {1, hex("#E8C46A22")}}, 0) : Fill::linear({{0, hex("#3FCF8EAA")}, {1, hex("#7A5CFFAA")}}, 0), 18);
            band.anim = Anim::Pop;
            add(out, band);
            Element ic = lookIcon(bk, "Sparkle", bk ? "star" : "sparkle", x0 + 8, y + 22, 56, 56, Anim::Pop);
            if (!bk) ic.fill = Fill::solid(hex("#FFFFFF")), ic.glow = {true, 14, 1.0f, hex("#FFFFFF88"), ""};
            add(out, ic);
            Element t = textEl("Celebration words", "{celebration}", x0 + 80, y + 14, cw - 90, 72, 32, bk ? "#2A1C0E" : "#FFFFFF", true, "left");
            if (bk) t.font = kSerif, t.italic = true;
            t.anim = Anim::Pop;
            add(out, t);
        } else if (k == "streamwords") {
            add(out, lookLabel(bk, "Stream label", bk ? "This stream" : "THIS STREAM", x0, y + 4, cw, 32, a));
            add(out, lookBody(bk, "Stream words", "{streamline}", x0, y + 38, cw, 44, 32, a));
        }
        y += h;
    }
    if (!bk) linkToBrand(out, defaultBrand());   // aurora follows the brand kit, as its templates do
    for (const Element& e : d.elements) out.elements.push_back(e);   // anything added in the designer, on top
    return out;
}

Document writingCardDesign(bool bookish, int width) {
    Document d;
    d.name = bookish ? "Writing (bookish)" : "Writing";
    d.width = std::clamp(width, 400, 3000);
    d.height = 1500;
    d.writing.on = true;
    for (auto& p : writingParts()) d.writing.parts.push_back(p.key);
    d.writing.look = bookish ? "bookish" : "aurora";
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
        Document w = writingCardDesign(bk, d.width);
        w.name = d.name;
        w.width = d.width;
        w.height = d.height;
        w.writing.parts.assign(parts.begin(), parts.end());
        d = w;
        return true;
    }
    return false;
}

}  // namespace ncap::design

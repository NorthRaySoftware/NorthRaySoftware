// Typesetting: manuscript -> paragraphs -> lines (Knuth–Plass) -> pages.
#include "typeset_internal.hpp"
#include "studio/fonts.hpp"

#include "internal.hpp"
#include "studio/document.hpp"
#include "studio/planning.hpp"

#include <algorithm>
#include <set>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace studio::typeset {

using nlohmann::json;
using namespace detail;

// ------------------------------------------------------------------ presets

const std::vector<Preset>& presets() {
    static const std::vector<Preset> v = [] {
        std::vector<Preset> p;
        Design classic;
        classic.chapterStyle = "number";
        p.push_back({"classic", "Classic", "Crimson Text, justified and hyphenated; chapters open with \u201cChapter One\u201d above the title.", classic});
        Design airy = classic;
        airy.font = "gentium-book-plus";
        airy.fontSize = 11;
        airy.leading = 15.5;
        airy.marginOutside = 0.7;
        airy.chaptersOnRecto = true;
        airy.chapterSinkLines = 9;
        airy.chapterStyle = "numeral";
        airy.dropCap = true;
        airy.leadIn = false;
        p.push_back({"airy", "Open", "Gentium Book with more space between lines; chapters on right-hand pages, a large numeral and a drop cap.", airy});
        Design ragged = classic;
        ragged.chapterStyle = "title";
        ragged.justify = false;
        ragged.hyphenate = false;
        ragged.indentEm = 1.5;
        p.push_back({"ragged", "Ragged right", "Crimson Text with an uneven right edge and no hyphenation: relaxed, informal.", ragged});
        Design large = classic;
        large.chapterStyle = "title";
        large.font = "gentium-book-plus";
        large.fontSize = 14;
        large.leading = 19;
        large.marginInside = 0.9;
        large.marginOutside = 0.65;
        p.push_back({"large-print", "Large print", "Gentium Book at 14 pt for easier reading.", large});
        Design comic;
        comic.comicScript = true;
        comic.trim = "8.5x11in";
        comic.font = "courier-prime";
        comic.fontSize = 12;
        comic.leading = 14.4;
        comic.marginTop = comic.marginBottom = comic.marginInside = comic.marginOutside = 1.0;
        comic.indentEm = 0;
        comic.justify = false;
        comic.hyphenate = false;
        comic.copyrightPage = false;
        comic.contents = false;
        comic.leadIn = false;
        comic.frontRoman = false;
        comic.chapterStyle = "title";
        comic.chapterSinkLines = 0;
        p.push_back({"comic-script", "Comic script", "Courier Prime on US Letter, the layout artists and letterers expect: each comic page on a new page, panels and balloons numbered.", comic});
        return p;
    }();
    return v;
}

const Preset& preset(const std::string& id) {
    for (auto& p : presets()) if (id == p.id) return p;
    return presets()[0];
}

namespace {

// ------------------------------------------------------------------- model

struct Span {
    std::string text;
    bool bold = false, italic = false, smallCaps = false, underline = false, strike = false, sup = false, sub = false;
    std::string font;              // a world's own typeface for these words (runes, handwriting: 0.41); "" = the book's
    bool sameStyle(const Span& o) const {
        return bold == o.bold && italic == o.italic && smallCaps == o.smallCaps && underline == o.underline && strike == o.strike &&
               sup == o.sup && sub == o.sub && font == o.font;
    }
};

enum class Align { Justify, Left, Center, Right };

struct Block {
    enum Type { Para, SceneBreak, Picture } type = Para;
    std::vector<Span> spans;
    Align align = Align::Justify;
    bool indent = true;            // first-line indent (unless after a heading or break)
    double leftEm = 0, rightEm = 0, firstEm = 0;   // extra indents (firstEm: hanging, negative)
    double scale = 1;              // type size relative to the body
    bool italic = false;
    int spaceBefore = 0, spaceAfter = 0;   // blank lines
    bool keepWithNext = false;
    bool heading = false;
    bool opening = false;          // the chapter's opening paragraph: lead-in and drop cap go here (0.94)
    bool breakBefore = false;      // starts a new page (a comic script's PAGE)
    // Drop cap: the letter(s) set large in the first lines (taken out of spans).
    std::string drop;
    int dropLines = 0;
    // Picture
    std::string blob, caption;
    Placement place;
};

void inlineSpans(const json& content, std::vector<Span>& out, const Span& base) {
    if (!content.is_array()) return;
    for (const json& n : content) {
        const std::string t = n.value("type", "");
        if (t == "hard_break") { Span s = base; s.text = "\n"; out.push_back(s); continue; }
        if (t != "text") continue;
        Span s = base;
        s.text = n.value("text", "");
        if (n.contains("marks"))
            for (const json& m : n["marks"]) {
                const std::string mt = m.value("type", "");
                if (mt == "em") s.italic = !s.italic;
                else if (mt == "strong") s.bold = true;
                else if (mt == "smallcaps") s.smallCaps = true;
                else if (mt == "underline") s.underline = true;
                else if (mt == "strike") s.strike = true;
                else if (mt == "sup") s.sup = true;
                else if (mt == "sub") s.sub = true;
                else if (mt == "font" && m.contains("attrs") && m["attrs"].is_object()) s.font = m["attrs"].value("family", "");
            }
        if (!s.text.empty()) out.push_back(std::move(s));
    }
}

void collectBlocks(const json& content, std::vector<Block>& out, const Block& ctx, int depth) {
    if (!content.is_array() || depth > 6) return;
    int listNo = 0;
    for (const json& n : content) {
        const std::string t = n.value("type", "");
        const json attrs = n.value("attrs", json::object());
        if (t == "paragraph") {
            Block b = ctx;
            const std::string st = attrs.value("style", "body");
            Span base;
            if (st == "first") b.indent = false;
            else if (st == "centered") { b.align = Align::Center; b.indent = false; }
            else if (st == "epigraph") { b.align = Align::Left; b.indent = false; b.leftEm += 2; b.rightEm += 2; b.scale *= 0.93; base.italic = true; }
            else if (st == "attribution") { b.align = Align::Right; b.indent = false; b.rightEm += 2; b.scale *= 0.93; b.spaceAfter = 1; }
            else if (st == "verse") { b.align = Align::Left; b.indent = false; b.leftEm += 2; }
            else if (st == "letter") { b.indent = false; b.leftEm += 1.5; b.rightEm += 1.5; }
            else if (st == "note") { b.indent = false; b.scale *= 0.88; }
            if (ctx.italic) base.italic = true;
            b.opening = depth == 0 && attrs.value("opening", false);
            inlineSpans(n.value("content", json::array()), b.spans, base);
            out.push_back(std::move(b));
        } else if (t == "heading") {
            Block b = ctx;
            const int level = std::clamp(attrs.value("level", 2), 1, 3);
            b.heading = true;
            b.keepWithNext = true;
            b.indent = false;
            b.align = level == 3 ? Align::Left : Align::Center;
            b.scale = level == 1 ? 1.25 : level == 2 ? 1.1 : 1.0;
            b.spaceBefore = 1;
            b.spaceAfter = level == 3 ? 0 : 1;
            Span base;
            base.italic = level == 3;
            base.bold = level != 3;
            inlineSpans(n.value("content", json::array()), b.spans, base);
            out.push_back(std::move(b));
        } else if (t == "blockquote") {
            Block q = ctx;
            q.leftEm += 1.5;
            q.rightEm += 1.5;
            q.scale *= 0.95;
            q.indent = false;
            const size_t from = out.size();
            collectBlocks(n.value("content", json::array()), out, q, depth + 1);
            if (out.size() > from) { out[from].spaceBefore = std::max(out[from].spaceBefore, 1); out.back().spaceAfter = std::max(out.back().spaceAfter, 1); }
        } else if (t == "bullet_list" || t == "ordered_list") {
            listNo = t == "ordered_list" ? attrs.value("start", 1) : 0;
            for (const json& li : n.value("content", json::array())) {
                Block item = ctx;
                item.leftEm += 1.6;
                item.indent = false;
                const size_t from = out.size();
                collectBlocks(li.value("content", json::array()), out, item, depth + 1);
                if (out.size() > from && out[from].type == Block::Para) {
                    // The bullet or number hangs in the indent.
                    Span mark;
                    mark.text = t == "ordered_list" ? std::to_string(listNo++) + ". " : "• ";
                    out[from].spans.insert(out[from].spans.begin(), mark);
                    out[from].firstEm = -1.6;
                }
            }
        } else if (t == "scene_break") {
            Block b;
            b.type = Block::SceneBreak;
            out.push_back(b);
        } else if (t == "image") {
            Block b;
            b.type = Block::Picture;
            b.blob = attrs.value("blob", "");
            b.caption = attrs.value("caption", "");
            Placement p;
            p.cropX = attrs.value("cropX", 0.0); p.cropY = attrs.value("cropY", 0.0);
            p.cropW = attrs.value("cropW", 1.0); p.cropH = attrs.value("cropH", 1.0);
            p.width = attrs.value("width", 1.0);
            p.align = attrs.value("align", "center");
            b.place = clampPlacement(p);
            if (!b.blob.empty()) out.push_back(b);
        }
    }
}

// ------------------------------------------------------- chapter numbers

std::string spelled(int n) {
    static const char* ones[] = {"Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten", "Eleven",
                                 "Twelve", "Thirteen", "Fourteen", "Fifteen", "Sixteen", "Seventeen", "Eighteen", "Nineteen"};
    static const char* tens[] = {"", "", "Twenty", "Thirty", "Forty", "Fifty", "Sixty", "Seventy", "Eighty", "Ninety"};
    if (n < 0 || n > 99) return std::to_string(n);
    if (n < 20) return ones[n];
    return std::string(tens[n / 10]) + (n % 10 ? std::string("-") + ones[n % 10] : std::string());
}

std::string roman(int n) {
    static const std::pair<int, const char*> parts[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
                                                         {50, "l"},   {40, "xl"},  {10, "x"},  {9, "ix"},   {5, "v"},   {4, "iv"}, {1, "i"}};
    std::string out;
    for (auto& [v, s] : parts) while (n >= v) { out += s; n -= v; }
    return out;
}

std::string lowerAscii(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ------------------------------------------------------- comic scripts

static std::string upperAscii(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
static std::string trimmedText(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// How many panels each comic page has, for "PAGE ONE (four panels)".
std::vector<int> comicPanelCounts(const json& content) {
    std::vector<int> out;
    if (!content.is_array()) return out;
    for (const json& n : content) {
        if (n.value("type", "") != "paragraph") continue;
        const std::string st = n.value("attrs", json::object()).value("style", "body");
        if (st == "page") out.push_back(0);
        else if (st == "panel") { if (out.empty()) out.push_back(0); ++out.back(); }
    }
    return out;
}

// A chapter (an issue) of a comic script as blocks: pages, panels and balloons are numbered here,
// so the writer never types a number. Page numbers carry on through the book (`pageNo`).
void comicBlocks(const json& content, std::vector<Block>& out, int& pageNo) {
    if (!content.is_array()) return;
    const std::vector<int> panels = comicPanelCounts(content);
    int panelNo = 0, balloonNo = 0, pageIndex = -1;
    bool firstPage = true;
    for (const json& n : content) {
        const std::string t = n.value("type", "");
        if (t != "paragraph") { collectBlocks(json::array({n}), out, Block{}, 0); continue; }
        const std::string st = n.value("attrs", json::object()).value("style", "body");
        std::vector<Span> spans;
        inlineSpans(n.value("content", json::array()), spans, Span{});
        std::string text;
        for (auto& s : spans) text += s.text;
        Block b;
        b.align = Align::Left;
        b.indent = false;
        b.spaceAfter = 1;
        if (st == "page") {
            ++pageNo;
            ++pageIndex;
            panelNo = balloonNo = 0;
            const int count = pageIndex < (int)panels.size() ? panels[pageIndex] : 0;
            std::string note = pageNo % 2 ? "right" : "left";
            if (count) note += ", " + lowerAscii(spelled(count)) + (count == 1 ? " panel" : " panels");
            Span s;
            s.bold = s.underline = true;
            s.text = "PAGE " + upperAscii(spelled(pageNo));
            b.spans.push_back(s);
            Span r;
            r.bold = true;
            r.text = " (" + note + ")";
            b.spans.push_back(r);
            if (!trimmedText(text).empty()) { Span x; x.text = "  " + trimmedText(text); b.spans.push_back(x); }
            b.breakBefore = !firstPage;
            b.keepWithNext = true;
            b.heading = true;
            firstPage = false;
            out.push_back(std::move(b));
            continue;
        }
        if (st == "panel") {
            ++panelNo;
            Block l = b;
            Span s;
            s.bold = true;
            s.text = "Panel " + std::to_string(panelNo);
            l.spans = {s};
            l.spaceAfter = 0;
            l.keepWithNext = true;
            l.heading = true;
            out.push_back(std::move(l));
            if (!trimmedText(text).empty()) { b.spans = spans; out.push_back(std::move(b)); }
            continue;
        }
        if (st == "dialogue" || st == "caption" || st == "sfx") {
            // "TOM: Hey mate." -> "1. TOM: Hey mate." (the name in bold capitals). SFX aren't numbered.
            b.leftEm = 4;
            b.firstEm = st == "sfx" ? 0 : -2;
            std::string who;
            size_t cut = std::string::npos;
            if (st == "dialogue") {
                const size_t colon = text.find(':');
                if (colon != std::string::npos && colon < 48) { who = trimmedText(text.substr(0, colon)); cut = colon + 1; }
            } else {
                who = st == "caption" ? "CAPTION" : "SFX";
                const std::string up = upperAscii(text.substr(0, std::min<size_t>(text.size(), 8)));
                if (up.rfind(who, 0) == 0) {
                    const size_t colon = text.find(':');
                    cut = colon != std::string::npos && colon < 12 ? colon + 1 : who.size();
                } else cut = 0;
            }
            std::vector<Span> rest;
            if (cut == std::string::npos) rest = spans;
            else {
                size_t skip = cut;
                for (auto s : spans) {
                    if (skip >= s.text.size()) { skip -= s.text.size(); continue; }
                    s.text = s.text.substr(skip);
                    skip = 0;
                    rest.push_back(s);
                }
                while (!rest.empty() && !rest.front().text.empty() && rest.front().text[0] == ' ') rest.front().text.erase(0, 1);
            }
            if (st != "sfx") { Span num; num.text = std::to_string(++balloonNo) + ". "; b.spans.push_back(num); }
            if (!who.empty()) { Span name; name.bold = true; name.text = upperAscii(who) + ": "; b.spans.push_back(name); }
            for (auto& s : rest) b.spans.push_back(s);
            out.push_back(std::move(b));
            continue;
        }
        // Anything else (notes to the artist, body text): plain, left-aligned.
        b.spans = spans;
        if (st == "note") for (auto& s : b.spans) s.italic = true;
        out.push_back(std::move(b));
    }
}

// A chapter title split into its label and the rest: "Chapter 3: The Storm" ->
// {3, "Three", "The Storm"}. Front and back matter (Prologue, Acknowledgements...)
// isn't numbered: special = true.
struct ChapterName {
    int number = 0;            // 0: not given
    std::string word;          // as written when it's a word ("Three")
    std::string rest;
    bool special = false;
};
ChapterName splitChapterTitle(const std::string& title) {
    ChapterName c;
    c.rest = title;
    const std::string low = lowerAscii(title);
    for (const char* sp : {"prologue", "epilogue", "acknowledg", "about the author", "afterword", "foreword", "preface", "introduction",
                           "notes", "appendix", "glossary", "also by", "dedication", "author's note", "author\u2019s note", "interlude"})
        if (low.rfind(sp, 0) == 0) { c.special = true; return c; }
    size_t i = 0;
    if (low.rfind("chapter ", 0) == 0) i = 8;
    else if (low.rfind("ch. ", 0) == 0) i = 4;
    else return c;
    size_t j = i;
    while (j < title.size() && title[j] != ' ' && title[j] != ':' && title[j] != '.' && title[j] != '-' && title[j] != ',') ++j;
    const std::string tok = title.substr(i, j - i);
    if (tok.empty()) return c;
    if (std::all_of(tok.begin(), tok.end(), [](char ch) { return std::isdigit((unsigned char)ch); })) c.number = std::atoi(tok.c_str());
    else {
        for (int n = 1; n < 100; ++n) if (lowerAscii(spelled(n)) == lowerAscii(tok)) { c.number = n; break; }
        if (!c.number) return c;   // "Chapter and verse": not a label
        c.word = tok;
    }
    // Skip the punctuation after the label.
    while (j < title.size() && (title[j] == ' ' || title[j] == ':' || title[j] == '.' || title[j] == '-' || title[j] == ',')) ++j;
    if (j + 3 <= title.size() && (title.compare(j, 3, "\u2014") == 0 || title.compare(j, 3, "\u2013") == 0)) {
        j += 3;
        while (j < title.size() && title[j] == ' ') ++j;
    }
    c.rest = title.substr(j);
    return c;
}

// --------------------------------------------------------------- shaping

struct Style {
    int face = 0;
    float size = 0;
    bool smallCaps = false, fakeSmallCaps = false;
    bool underline = false, strike = false;
    float rise = 0;                // baseline shift, pt (up negative)
};

std::string upper(const std::string& s) {
    std::u32string u = utf32(s);
    for (auto& c : u) c = (char32_t)std::towupper((wint_t)c);
    return utf8(u);
}

class Shaper {
public:
    Shaper(const Family& fam, std::string lang, std::string fontsFolder = {})
        : fam_(fam), lang_(std::move(lang)), fontsFolder_(std::move(fontsFolder)) {}
    Style style(const Span& s, float size) const {
        Style st;
        st.face = familyFor(s.font).pick(s.bold, s.italic);
        st.size = size;
        if (s.sup || s.sub) { st.size = size * 0.65f; st.rise = s.sup ? -size * 0.33f : size * 0.15f; }
        st.smallCaps = s.smallCaps;
        if (s.smallCaps && !Fonts::get().face(st.face).smallCaps) { st.fakeSmallCaps = true; st.size *= 0.8f; }
        st.underline = s.underline;
        st.strike = s.strike;
        return st;
    }
    const Shaped& get(const Style& st, const std::string& text) {
        std::string key = std::to_string(st.face) + "|" + std::to_string(st.size) + "|" + (st.smallCaps ? "1" : "0") + "|" + text;
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        const std::string t = st.fakeSmallCaps ? upper(text) : text;
        return cache_.emplace(std::move(key), shape(st.face, t, st.size, st.smallCaps && !st.fakeSmallCaps, lang_)).first->second;
    }
    float width(const Style& st, const std::string& text) { return get(st, text).width; }
    // Typefaces asked for on words but not found (the font was removed from the world).
    const std::set<std::string>& missing() const { return missing_; }
private:
    // The book's family, or a world's own one named on the words (opened once).
    const Family& familyFor(const std::string& id) const {
        if (id.empty()) return fam_;
        auto it = others_.find(id);
        if (it == others_.end()) {
            Family f;
            if (!openFamily(fontsFolder_, id, f)) { missing_.insert(id); f = fam_; }
            it = others_.emplace(id, f).first;
        }
        return it->second;
    }
    const Family& fam_;
    std::string lang_;
    std::string fontsFolder_;
    mutable std::unordered_map<std::string, Family> others_;
    mutable std::set<std::string> missing_;
    std::unordered_map<std::string, Shaped> cache_;
};

// ----------------------------------------------------- Knuth–Plass breaking

constexpr double kInf = 10000;

struct Item {
    enum Kind { Box, Glue, Penalty } kind = Box;
    double width = 0, stretch = 0, shrink = 0;
    double penalty = 0;
    bool flagged = false;
    // Box: a fragment of a word.
    int span = -1;                 // style (index into the paragraph's spans)
    std::string text;
    int word = -1;                 // fragments of one word share this
    // Penalty: the hyphen's style.
    int hyphenSpan = -1;
};

struct Breaks {
    std::vector<size_t> at;        // item index of each line's break
    std::vector<double> ratios;
};

// Total-fit line breaking (Knuth & Plass, 1981). widths[i] is line i's width
// (the last entry repeats). Returns the chosen breakpoints.
Breaks knuthPlass(const std::vector<Item>& items, const std::vector<double>& widths, double tolerance) {
    struct Node {
        size_t pos;
        int line, fitness;
        double width, stretch, shrink, demerits;
        int prev;                  // index into nodes
    };
    std::vector<Node> nodes;
    std::vector<int> active;
    nodes.push_back({0, 0, 1, 0, 0, 0, 0, -1});
    active.push_back(0);
    double sumW = 0, sumY = 0, sumZ = 0;
    auto lineWidth = [&](int line) { return widths[std::min<size_t>(line, widths.size() - 1)]; };
    for (size_t b = 0; b < items.size(); ++b) {
        const Item& it = items[b];
        const bool legal = (it.kind == Item::Penalty && it.penalty < kInf) ||
                           (it.kind == Item::Glue && b > 0 && items[b - 1].kind == Item::Box);
        if (legal) {
            struct Cand { double demerits = std::numeric_limits<double>::infinity(); int from = -1; double ratio = 0; };
            Cand best[4];
            std::vector<int> keep;
            for (int ai : active) {
                const Node& a = nodes[ai];
                double L = sumW - a.width + (it.kind == Item::Penalty ? it.width : 0);
                const double target = lineWidth(a.line);
                double r;
                if (L < target) { const double y = sumY - a.stretch; r = y > 0 ? (target - L) / y : kInf; }
                else if (L > target) { const double z = sumZ - a.shrink; r = z > 0 ? (target - L) / z : -kInf; }
                else r = 0;
                const bool forced = it.kind == Item::Penalty && it.penalty <= -kInf;
                if (r >= -1 && r <= tolerance) {
                    const double bad = 100 * std::pow(std::abs(r), 3);
                    double d = 1 + bad;
                    d *= d;
                    if (it.kind == Item::Penalty) {
                        if (it.penalty >= 0) d += it.penalty * it.penalty;
                        else if (it.penalty > -kInf) d -= it.penalty * it.penalty;
                    }
                    if (it.kind == Item::Penalty && it.flagged && a.pos < items.size() && items[a.pos].kind == Item::Penalty && items[a.pos].flagged)
                        d += 3000;   // two hyphenated lines in a row
                    const int fit = r < -0.5 ? 0 : r <= 0.5 ? 1 : r <= 1 ? 2 : 3;
                    if (std::abs(fit - a.fitness) > 1) d += 100;
                    d += a.demerits;
                    if (d < best[fit].demerits) best[fit] = {d, ai, r};
                }
                if (!(r < -1 || forced)) keep.push_back(ai);
            }
            active = keep;
            // Totals after the break: skip the glue and penalties that follow it.
            double w = sumW, y = sumY, z = sumZ;
            for (size_t k = b; k < items.size(); ++k) {
                const Item& n = items[k];
                if (n.kind == Item::Box) break;
                if (n.kind == Item::Glue) { w += n.width; y += n.stretch; z += n.shrink; }
                else if (n.penalty <= -kInf && k > b) break;
            }
            for (int f = 0; f < 4; ++f)
                if (best[f].from >= 0) {
                    nodes.push_back({b, nodes[best[f].from].line + 1, f, w, y, z, best[f].demerits, best[f].from});
                    active.push_back((int)nodes.size() - 1);
                }
            if (active.empty()) return {};   // nothing fits: the caller relaxes the tolerance
        }
        if (it.kind == Item::Box) sumW += it.width;
        else if (it.kind == Item::Glue) { sumW += it.width; sumY += it.stretch; sumZ += it.shrink; }
    }
    // The node that ends the paragraph with the fewest demerits.
    int bestNode = -1;
    for (int ai : active)
        if (nodes[ai].pos == items.size() - 1 && (bestNode < 0 || nodes[ai].demerits < nodes[bestNode].demerits)) bestNode = ai;
    Breaks out;
    if (bestNode < 0) return out;
    for (int n = bestNode; nodes[n].prev >= 0; n = nodes[n].prev) out.at.push_back(nodes[n].pos);
    std::reverse(out.at.begin(), out.at.end());
    return out;
}

// First fit: for ragged settings, and as the last resort when nothing else fits.
Breaks firstFit(const std::vector<Item>& items, const std::vector<double>& widths) {
    Breaks out;
    double w = 0;
    int line = 0;
    size_t lastBreak = (size_t)-1;
    double wAtLast = 0;
    auto lw = [&](int l) { return widths[std::min<size_t>(l, widths.size() - 1)]; };
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        const bool legal = (it.kind == Item::Penalty && it.penalty < kInf) || (it.kind == Item::Glue && i > 0 && items[i - 1].kind == Item::Box);
        if (it.kind == Item::Penalty && it.penalty <= -kInf) {
            out.at.push_back(i);
            ++line; w = 0; lastBreak = (size_t)-1;
            continue;
        }
        if (legal) {
            const double withPenalty = w + (it.kind == Item::Penalty ? it.width : 0);
            if (withPenalty <= lw(line) || lastBreak == (size_t)-1) { lastBreak = i; wAtLast = w; }
        }
        if (it.kind == Item::Box || it.kind == Item::Glue) {
            const bool atLineStart = w == 0 && it.kind == Item::Glue;
            if (!atLineStart) w += it.width;
        }
        if (w > lw(line) && lastBreak != (size_t)-1 && lastBreak < i) {
            out.at.push_back(lastBreak);
            ++line;
            // Restart after the break, skipping glue.
            size_t k = lastBreak + 1;
            w = 0;
            for (; k <= i; ++k) if (items[k].kind == Item::Box) break;
            for (size_t m = k; m <= i; ++m) if (items[m].kind != Item::Penalty) w += items[m].width;
            lastBreak = (size_t)-1;
            (void)wAtLast;
        }
    }
    if (out.at.empty() || out.at.back() != items.size() - 1) out.at.push_back(items.size() - 1);
    return out;
}

// ------------------------------------------------------------- set lines

// A line ready to go on a page: glyph runs with x relative to the text block's left edge.
struct SetLine {
    std::vector<GlyphRun> runs;
    std::vector<Rule> rules;       // y relative to the baseline
    std::vector<Disc> discs;       // a viewpoint's icon (0.103): x from the text's left, y relative to the baseline
    std::vector<PicturePlacement> pics;   // the same, for its picture (y is its top)
    int lines = 1;                 // grid lines it occupies (big headings take two)
    float extraBaseline = 0;       // baseline offset within its grid lines, pt
};

class Setter {
public:
    Setter(Shaper& sh, Book& book, const Hyphenator* hyph, const Design& d, double measure)
        : sh_(sh), book_(book), hyph_(hyph), d_(d), measure_(measure) {}

    int bookFont(int face) {
        if (auto it = fontIndex_.find(face); it != fontIndex_.end()) return it->second;
        FontFile f;
        f.path = Fonts::get().face(face).path;
        f.postscriptName = Fonts::get().face(face).postscriptName;
        book_.fonts.push_back(f);
        fontIndex_[face] = (int)book_.fonts.size() - 1;
        return fontIndex_[face];
    }

    // Shapes a piece of text for output, recording what each glyph stands for.
    void emit(SetLine& out, const Style& st, const std::string& text, float x) {
        const Shaped& s = sh_.get(st, text);
        if (s.glyphs.empty()) return;
        GlyphRun r;
        int runFace = s.faces.empty() ? st.face : s.faces[0];
        r.font = bookFont(runFace);
        r.size = st.size;
        r.y = st.rise;
        const std::string src = st.fakeSmallCaps ? upper(text) : text;
        for (size_t i = 0; i < s.glyphs.size(); ++i) {
            // A symbol from a fallback font: a run of its own, in that font.
            if (!s.faces.empty() && s.faces[i] != runFace) {
                if (!r.glyphs.empty()) out.runs.push_back(std::move(r));
                r = GlyphRun{};
                runFace = s.faces[i];
                r.font = bookFont(runFace);
                r.size = st.size;
                r.y = st.rise;
            }
            auto& map = book_.fonts[r.font].toUnicode;
            r.glyphs.push_back(s.glyphs[i]);
            r.xs.push_back(x + s.x[i]);
            r.dy.push_back(s.dy[i]);
            if (!map.count(s.glyphs[i])) {
                // The cluster's text: from this glyph's cluster to the next larger one.
                const std::uint32_t c = s.clusters[i];
                std::uint32_t end = (std::uint32_t)src.size();
                for (std::uint32_t c2 : s.clusters) if (c2 > c && c2 < end) end = c2;
                bool first = true;   // only the first glyph of a cluster carries its text
                for (size_t k = 0; k < i; ++k) if (s.clusters[k] == c) first = false;
                if (first && c < src.size()) map[s.glyphs[i]] = src.substr(c, end - c);
            }
        }
        if (st.underline) out.rules.push_back({x, st.size * 0.12f, s.width, std::max(0.4f, st.size * 0.05f)});
        if (st.strike) out.rules.push_back({x, -st.size * 0.28f, s.width, std::max(0.4f, st.size * 0.05f)});
        out.runs.push_back(std::move(r));
    }

    // Sets one paragraph. `noIndent` after headings, breaks and pictures.
    std::vector<SetLine> paragraph(const Block& b, bool noIndent, bool* loose = nullptr) {
        const float size = (float)(d_.fontSize * b.scale);
        std::vector<Style> styles;
        for (auto& s : b.spans) styles.push_back(sh_.style(s, size));
        const double em = size;
        const double left = b.leftEm * em, right = b.rightEm * em;
        // First-line indent; negative for a hanging bullet or number.
        const bool indentable = b.align == Align::Justify || b.align == Align::Left;
        const double first = (b.indent && !noIndent && indentable ? d_.indentEm * em : 0) + b.firstEm * em;
        const double full = measure_ - left - right;
        // The indent is a box at the start of the first line (so the line keeps
        // its full width); a hanging indent instead makes the first line wider.
        std::vector<double> widths = {first < 0 ? full - first : full, full};
        // A drop cap: the first lines make room for it.
        Style dropStyle;
        float dropW = 0;
        if (!b.drop.empty() && b.dropLines > 1) {
            Span ds = b.spans.empty() ? Span{} : b.spans[0];
            ds.smallCaps = ds.underline = ds.strike = ds.sup = ds.sub = false;
            const Style probe = sh_.style(ds, size);
            const double cap = Fonts::get().face(probe.face).capHeight;
            const float dropSize = (float)(((b.dropLines - 1) * d_.leading + cap * size) / cap);
            dropStyle = sh_.style(ds, dropSize);
            dropW = sh_.width(dropStyle, b.drop) + size * 0.3f;
            widths.assign((size_t)b.dropLines, full - dropW);
            widths.push_back(full);
        }

        // Items.
        std::vector<Item> items;
        if (first > 0) { Item ind; ind.kind = Item::Box; ind.width = first; items.push_back(ind); }
        const double startX = first < 0 ? first : 0;
        int word = 0;
        const bool canHyphenate = d_.hyphenate && hyph_ && hyph_->loaded() && b.align == Align::Justify && !b.heading;
        // Split into words: runs of non-space across spans.
        struct Piece { int span; std::string text; };
        std::vector<Piece> current;
        auto flushWord = [&]() {
            if (current.empty()) return;
            // Whole word text, and hyphenation points in code points.
            std::u32string all;
            std::vector<std::pair<int, int>> bounds;   // code point range per piece
            for (auto& p : current) { const auto u = utf32(p.text); bounds.push_back({(int)all.size(), (int)(all.size() + u.size())}); all += u; }
            std::vector<int> points;
            if (canHyphenate) {
                // Only the letters of the word (not leading or trailing punctuation).
                int a = 0, z = (int)all.size();
                while (a < z && !std::iswalpha((wint_t)all[a])) ++a;
                while (z > a && !std::iswalpha((wint_t)all[z - 1])) --z;
                // Names in capitals or small capitals aren't broken.
                bool caps = false;
                for (auto& p : current) if (b.spans[p.span].smallCaps) caps = true;
                int upperCount = 0;
                for (int k = a; k < z; ++k) if (std::iswupper((wint_t)all[k])) ++upperCount;
                if (upperCount > 1) caps = true;
                if (z - a >= 5 && !caps) for (int p : hyph_->points(all.substr(a, z - a))) points.push_back(a + p);
            }
            // Fragments: split each piece at hyphenation points.
            for (size_t pi = 0; pi < current.size(); ++pi) {
                const auto u = utf32(current[pi].text);
                int start = 0;
                for (int pt : points) {
                    if (pt <= bounds[pi].first || pt >= bounds[pi].second) continue;
                    const int cut = pt - bounds[pi].first;
                    Item box;
                    box.kind = Item::Box;
                    box.span = current[pi].span;
                    box.text = utf8(u.substr(start, cut - start));
                    box.word = word;
                    box.width = sh_.width(styles[box.span], box.text);
                    items.push_back(box);
                    Item pen;
                    pen.kind = Item::Penalty;
                    pen.penalty = 50;
                    pen.flagged = true;
                    pen.hyphenSpan = current[pi].span;
                    pen.width = sh_.width(styles[pen.hyphenSpan], "-");
                    items.push_back(pen);
                    start = cut;
                }
                Item box;
                box.kind = Item::Box;
                box.span = current[pi].span;
                box.text = utf8(u.substr(start));
                box.word = word;
                box.width = sh_.width(styles[box.span], box.text);
                if (!box.text.empty()) items.push_back(box);
                // A word ending in a dash may break after it.
                if (pi + 1 == current.size() && !box.text.empty()) {
                    const std::string& t = box.text;
                    if (t.size() >= 3 && (t.compare(t.size() - 3, 3, "—") == 0 || t.compare(t.size() - 3, 3, "–") == 0)) {
                        Item pen;
                        pen.kind = Item::Penalty;
                        pen.penalty = 30;
                        items.push_back(pen);
                    }
                }
            }
            current.clear();
            ++word;
        };
        for (size_t si = 0; si < b.spans.size(); ++si) {
            const std::string& t = b.spans[si].text;
            size_t i = 0;
            while (i < t.size()) {
                if (t[i] == '\n') {
                    flushWord();
                    Item g; g.kind = Item::Glue; g.stretch = kInf; items.push_back(g);
                    Item p; p.kind = Item::Penalty; p.penalty = -kInf; items.push_back(p);
                    ++i;
                    continue;
                }
                if (t[i] == ' ' || t[i] == '\t') {
                    flushWord();
                    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
                    if (!items.empty() && items.back().kind == Item::Box) {
                        Item g;
                        g.kind = Item::Glue;
                        g.width = sh_.width(styles[si], " ");
                        if (b.align == Align::Justify) { g.stretch = g.width * 0.6; g.shrink = g.width * 0.3; }
                        items.push_back(g);
                    }
                    continue;
                }
                size_t j = i;
                while (j < t.size() && t[j] != ' ' && t[j] != '\t' && t[j] != '\n') ++j;
                current.push_back({(int)si, t.substr(i, j - i)});
                i = j;
            }
        }
        flushWord();
        while (!items.empty() && items.back().kind == Item::Glue) items.pop_back();
        if (items.empty()) return {};
        { Item g; g.kind = Item::Glue; g.stretch = kInf; items.push_back(g); }
        { Item p; p.kind = Item::Penalty; p.penalty = -kInf; items.push_back(p); }

        Breaks br;
        if (b.align == Align::Justify) {
            br = knuthPlass(items, widths, 2.5);
            if (br.at.empty()) { br = knuthPlass(items, widths, 12); if (loose) *loose = true; }
        }
        if (br.at.empty()) br = firstFit(items, widths);

        // Lines.
        std::vector<SetLine> out;
        size_t start = 0;
        for (size_t li = 0; li < br.at.size(); ++li) {
            const size_t end = br.at[li];
            // Skip glue and penalties at the start of a line.
            while (start < end && items[start].kind != Item::Box) ++start;
            const bool last = li + 1 == br.at.size() || (items[end].kind == Item::Penalty && items[end].penalty <= -kInf);
            const double target = widths[std::min<size_t>(li, widths.size() - 1)];
            // Words on the line: consecutive boxes of one word and style become one piece.
            struct Out { int span; std::string text; bool space; float natural; };
            std::vector<Out> pieces;
            int spaces = 0;
            double natural = 0;
            for (size_t k = start; k < end; ++k) {
                const Item& it = items[k];
                if (it.kind == Item::Box) {
                    if (it.span < 0) { pieces.push_back({-1, std::string(), false, (float)it.width}); natural += it.width; continue; }
                    if (!pieces.empty() && pieces.back().span == it.span && !pieces.back().space && pieces.back().span >= 0 &&
                        k > start && items[k - 1].kind != Item::Glue)
                        pieces.back().text += it.text;
                    else pieces.push_back({it.span, it.text, false, 0});
                } else if (it.kind == Item::Glue && k + 1 < end) {
                    pieces.push_back({-1, std::string(), true, (float)it.width});
                    ++spaces;
                }
            }
            if (items[end].kind == Item::Penalty && items[end].flagged && !pieces.empty() && pieces.back().span >= 0)
                pieces.back().text += "-";
            // Natural widths, shaped as they'll print (with kerning across the whole piece).
            for (auto& p : pieces) {
                if (p.span >= 0) p.natural = sh_.width(styles[p.span], p.text);
                natural += p.span >= 0 ? p.natural : (p.space ? p.natural : 0);
            }
            double extra = 0;
            if (b.align == Align::Justify && !last && spaces > 0) extra = (target - natural) / spaces;
            if (b.align == Align::Justify && last && spaces > 0 && natural > target) extra = (target - natural) / spaces;
            double x = left + (li == 0 ? startX : 0) + ((int)li < b.dropLines ? dropW : 0);
            if (b.align == Align::Center) x += (target - natural) / 2;
            else if (b.align == Align::Right) x += target - natural;
            SetLine line;
            int lastSpan = -1;
            for (auto& p : pieces) {
                if (p.span < 0) {
                    // A real space between the words (0.94): invisible, but in the PDF's text, so a justified line copies,
                    // searches and reads aloud as words, not "hissapphire". The next word is still placed where justifying
                    // puts it. Never underlined or struck through: the gap wasn't before.
                    if (p.space && lastSpan >= 0) {
                        Style gap = styles[lastSpan];
                        gap.underline = gap.strike = false;
                        emit(line, gap, " ", (float)x);
                    }
                    x += p.natural + (p.space ? extra : 0);
                    continue;
                }
                emit(line, styles[p.span], p.text, (float)x);
                lastSpan = p.span;
                x += p.natural;
            }
            line.lines = 1;
            if (li == 0 && dropW > 0) {
                // The big letter, on the baseline of the last line it spans.
                const size_t at = line.runs.size();
                emit(line, dropStyle, b.drop, (float)left);
                for (size_t k = at; k < line.runs.size(); ++k) line.runs[k].y = (float)((b.dropLines - 1) * d_.leading);
            }
            out.push_back(std::move(line));
            start = end + 1;
        }
        return out;
    }

    // A single centred line in one style (titles, running heads, folios, ornaments).
    SetLine centred(const std::string& text, const Span& look, float size, double width, double letterSpace = 0) {
        SetLine line;
        const Style st = sh_.style(look, size);
        if (letterSpace <= 0) {
            const float w = sh_.width(st, text);
            emit(line, st, text, (float)((width - w) / 2));
            return line;
        }
        // Letter-spaced: set each character, then centre.
        const std::u32string u = utf32(text);
        double total = 0;
        std::vector<float> ws;
        for (char32_t c : u) { ws.push_back(sh_.width(st, utf8(std::u32string(1, c)))); total += ws.back() + letterSpace; }
        total -= letterSpace;
        double x = (width - total) / 2;
        for (size_t i = 0; i < u.size(); ++i) {
            emit(line, st, utf8(std::u32string(1, u[i])), (float)x);
            x += ws[i] + letterSpace;
        }
        return line;
    }

    float width(const std::string& text, const Span& look, float size) { return sh_.width(sh_.style(look, size), text); }

    // Whether every character of `text` is in the font or one of the symbol fonts (0.103: a viewpoint's symbol that
    // would otherwise print as an empty box).
    bool covers(const std::string& text, const Span& look, float size) {
        Fonts& fonts = Fonts::get();
        std::lock_guard<std::recursive_mutex> g(fonts.lock());
        const int face = sh_.style(look, size).face;
        const std::u32string u = utf32(text);
        for (char32_t cp : u) {
            const bool neutral = cp <= 0x20 || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF);
            if (neutral || fonts.hasChar(face, cp)) continue;
            bool any = false;
            for (int f : fonts.fallbacks()) if (fonts.hasChar(f, cp)) { any = true; break; }
            if (!any) return false;
        }
        return true;
    }

    // Where a line's glyphs reach, from its first glyph's x to its last glyph's end, pt (0, 0 when it has none).
    std::pair<float, float> extent(const SetLine& l) {
        float a = 1e9f, b = -1e9f;
        Fonts& fonts = Fonts::get();
        std::lock_guard<std::recursive_mutex> g(fonts.lock());
        for (const GlyphRun& r : l.runs) {
            if (r.glyphs.empty()) continue;
            const int fi = fonts.open(book_.fonts[r.font].path);
            a = std::min(a, r.xs.front());
            const float adv = fi >= 0 ? fonts.advance(fi, r.glyphs.back()) * r.size / (float)fonts.face(fi).upem : r.size * 0.5f;
            b = std::max(b, r.xs.back() + adv);
        }
        return a > b ? std::pair<float, float>{0, 0} : std::pair<float, float>{a, b};
    }

    // A contents line: the title on the left (parts in small capitals, chapters
    // indented), the page number on the right. Long titles are shortened.
    SetLine contentsLine(const std::string& title, const std::string& number, bool part, double width) {
        SetLine line;
        Span look;
        look.smallCaps = part;
        const float size = (float)d_.fontSize;
        const Style st = sh_.style(look, size);
        const Style nst = sh_.style(Span{}, size);
        const float nw = sh_.width(nst, number);
        const double indent = part ? 0 : size * 1.2;
        const double room = width - indent - nw - size * 1.5;
        std::u32string t = utf32(title);
        std::string text = title;
        while (t.size() > 4 && sh_.width(st, text) > room) {
            t.pop_back();
            while (!t.empty() && t.back() == U' ') t.pop_back();
            text = utf8(t) + "\u2026";
        }
        emit(line, st, text, (float)indent);
        emit(line, nst, number, (float)(width - nw));
        return line;
    }

private:
    Shaper& sh_;
    Book& book_;
    const Hyphenator* hyph_;
    const Design& d_;
    double measure_;
    std::unordered_map<int, int> fontIndex_;
};

// ------------------------------------------------------------------ pages

struct Flow {
    enum Kind { Line, Space, NewPage, Picture } kind = Line;
    SetLine line;
    int para = -1, index = 0, count = 1;   // the paragraph a line belongs to
    bool keepWithNext = false;
    bool recto = false;                      // NewPage: start on a right-hand page
    Page::Kind pageKind = Page::Text;        // NewPage: the kind of page it starts
    std::string chapter;                     // NewPage: chapter title for running heads
    int lines = 1;                           // Space / Picture: grid lines
    PicturePlacement pic;                    // Picture (x relative to the text block, y from its top)
    SetLine caption;
    std::vector<SetLine> over;               // Picture: lines set over it, centred (a chapter's title, 0.89)
};

}  // namespace

ChapterTitle chapterTitleParts(const std::string& title) {
    const ChapterName c = splitChapterTitle(title);
    return {c.number, c.word, c.rest, c.special};
}
std::string spelledNumber(int n) { return spelled(n); }

namespace {
// A title as compared for repeats: punctuation and spacing made plain, in lower case, without closing marks.
std::string titleKey(const std::string& t) {
    std::string k = plan::plainPunctuation(t);
    for (auto& c : k) c = (char)std::tolower((unsigned char)c);
    while (!k.empty() && (k.back() == '.' || k.back() == ':' || k.back() == ' ')) k.pop_back();
    return k;
}
std::string nodeText(const json& n) {
    if (!n.is_object()) return {};
    if (n.value("type", "") == "text") return n.value("text", "");
    std::string o;
    if (n.contains("content") && n["content"].is_array()) for (const json& c : n["content"]) o += nodeText(c);
    return o;
}
// What every printed and exported copy leaves out (0.94): draft notes; a heading at the top of a chapter's text that
// repeats its title (the heading prints it already); and a part made only to hold the front or back matter chapter
// of the same name ("Reader's Guide" holding "Reader's Guide"), which would print the title twice and list it twice.
void tidyForPrint(Source& s) {
    for (Source::Section& sec : s.sections) {
        sec.content = doc::withoutDraftNotes(sec.content);

        if (sec.kind == NodeKind::Scene || sec.content.find("\"heading\"") == std::string::npos) continue;
        json d = json::parse(sec.content, nullptr, false);
        if (!d.is_object() || !d.contains("content") || !d["content"].is_array() || d["content"].empty()) continue;
        json& first = d["content"][0];
        if (first.value("type", "") != "heading" || titleKey(nodeText(first)) != titleKey(sec.title) || titleKey(sec.title).empty()) continue;
        d["content"].erase(d["content"].begin());
        if (d["content"].empty()) d["content"] = json::array({json{{"type", "paragraph"}}});
        sec.content = d.dump();
    }
    std::vector<Source::Section> kept;
    for (size_t i = 0; i < s.sections.size(); ++i) {
        const Source::Section& sec = s.sections[i];
        if (sec.kind == NodeKind::Part && i + 1 < s.sections.size()) {
            const Source::Section& next = s.sections[i + 1];
            if (next.kind == NodeKind::Chapter && next.matter != 0 && titleKey(next.title) == titleKey(sec.title) &&
                doc::plainText(sec.content).find_first_not_of(" \n\r\t") == std::string::npos)
                continue;
        }
        kept.push_back(sec);
    }
    s.sections = std::move(kept);
}
}  // namespace


bool looksLikeDateLine(const std::string& raw) {
    std::string t = plan::plainPunctuation(raw);
    if (t.empty() || t.size() > 80) return false;
    std::string low;
    for (char c : t) low += (char)std::tolower((unsigned char)c);
    // Words, and what they say.
    std::vector<std::string> words;
    {
        std::string w;
        for (char c : low) {
            if (std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80 || c == '/' || c == '-') w += c;
            else if (!w.empty()) { words.push_back(w); w.clear(); }
        }
        if (!w.empty()) words.push_back(w);
    }
    if (words.empty() || words.size() > 10) return false;
    const char last = t.back();
    if (last == '?' || last == '!' || last == '"') return false;
    if (last == '.' && words.size() > 6) return false;
    static const std::set<std::string> sentence = {"i", "he", "she", "they", "we", "you", "it", "was", "were", "is", "are", "am", "had",
                                                   "has", "have", "said", "says", "did", "does", "will", "would", "could", "should",
                                                   "his", "her", "their", "my", "our", "your", "him", "them", "me", "us", "not", "never"};
    bool digit = false, dateWord = false;
    static const std::set<std::string> when = {"day", "days", "year", "years", "month", "week", "spring", "summer", "autumn", "fall",
                                               "winter", "dawn", "dusk", "midnight", "noon", "morning", "evening", "night", "era", "age",
                                               "january", "february", "march", "april", "may", "june", "july", "august", "september",
                                               "october", "november", "december", "monday", "tuesday", "wednesday", "thursday",
                                               "friday", "saturday", "sunday", "bc", "bce", "ad", "ce", "ac", "am", "pm"};
    for (const std::string& w : words) {
        if (sentence.count(w)) return false;
        for (char c : w) if (std::isdigit((unsigned char)c)) digit = true;
        if (when.count(w)) dateWord = true;
    }
    return digit || (dateWord && words.size() <= 5);
}

int openingParagraph(const std::string& docJson) {
    const json d = json::parse(docJson, nullptr, false);
    if (!d.is_object() || !d.contains("content") || !d["content"].is_array()) return -1;
    const json& c = d["content"];
    for (size_t i = 0; i < c.size(); ++i) {
        const json& n = c[i];
        const std::string t = n.value("type", "");
        if (t == "scene_break") return -1;
        if (t != "paragraph") continue;   // headings, pictures, quotations, lists
        const std::string style = n.contains("attrs") && n["attrs"].is_object() ? n["attrs"].value("style", "body") : "body";
        if (style != "body" && style != "first") continue;
        std::string text = nodeText(n);
        if (text.find_first_not_of(" \t\r\n ") == std::string::npos) continue;
        if (looksLikeDateLine(text)) continue;
        // A whole paragraph in italics or small capitals at the top is set apart, like a date or a heading.
        bool allMarked = true;
        for (const json& x : n.value("content", json::array())) {
            if (x.value("type", "") != "text") continue;
            bool marked = false;
            for (const json& m : x.value("marks", json::array())) {
                const std::string mt = m.value("type", "");
                if (mt == "em" || mt == "smallcaps" || mt == "strong") marked = true;
            }
            if (!marked && x.value("text", "").find_first_not_of(" ") != std::string::npos) { allMarked = false; break; }
        }
        if (allMarked && text.size() < 120) continue;
        return (int)i;
    }
    return -1;
}

std::string withOpening(const std::string& docJson) {
    if (docJson.find("\"opening\":true") != std::string::npos) return docJson;
    const int at = openingParagraph(docJson);
    if (at < 0) return docJson;
    json d = json::parse(docJson, nullptr, false);
    d["content"][(size_t)at]["attrs"]["opening"] = true;
    return d.dump();
}

Source snapshot(const Library& lib, const Id& bookId) {
    Source s;
    const studio::Book* b = lib.book(bookId);
    if (!b) return s;
    s.id = b->id;
    s.title = b->title;
    s.synopsis = b->synopsis;
    s.genre = b->genre;
    if (b->coverBlob.size() == 64) s.coverPath = lib.blobPath(b->coverBlob);
    s.subtitle = b->subtitle;
    s.author = !b->penName.empty() ? b->penName : b->author;
    s.trim = b->trim;
    s.language = b->language;
    s.isbn = b->isbn;
    // With editions, the copyright page lists each one's ISBN.
    for (const Edition* e : lib.editionsOf(bookId)) {
        if (e->isbn.empty()) continue;
        std::string l = e->name.empty() ? std::string(editionKindLabel(e->kind)) : e->name;
        if (e->name.empty()) for (auto& c : l) c = (char)std::tolower((unsigned char)c);
        s.isbns.push_back({l, e->isbn});
    }
    s.edition = b->edition;
    {
        const std::time_t t = std::time(nullptr);
        s.year = 1900 + std::gmtime(&t)->tm_year;
    }
    for (const ManuscriptNode* n : lib.manuscriptOf(bookId)) {
        Source::Section sec;
        sec.kind = n->kind;
        sec.matter = n->matter;
        sec.title = n->title.empty() ? std::string(label(n->kind)) : n->title;
        sec.content = lib.nodeContent(n->id);
        sec.words = n->wordCount;
        // Pictures it uses: where the file is, and its size.
        size_t at = 0;
        while ((at = sec.content.find("\"blob\":\"", at)) != std::string::npos) {
            at += 8;
            const std::string h = sec.content.substr(at, 64);
            if (h.size() == 64 && !s.picturePaths.count(h)) {
                s.picturePaths[h] = lib.blobPath(h);
                if (const Artwork* a = lib.artworkForBlob(h)) s.pictureSizes[h] = {a->width, a->height};
            }
        }
        s.sections.push_back(std::move(sec));
    }
    // The scene break's picture in the book's design (0.91).
    {
        const std::string sb = designFromJson(lib.setting("typeset:" + bookId)).sceneBreakBlob;
        if (!sb.empty() && !s.picturePaths.count(sb)) {
            s.picturePaths[sb] = lib.blobPath(sb);
            if (const Artwork* a = lib.artworkForBlob(sb)) s.pictureSizes[sb] = {a->width, a->height};
        }
    }
    // Each chapter's and part's header picture (0.89).
    {
        const headers::BookHeaders hb = headers::read(lib, bookId);
        size_t i = 0;
        for (const ManuscriptNode* n : lib.manuscriptOf(bookId)) {
            Source::Section& sec = s.sections[i++];
            const auto res = headers::resolve(lib, *n, hb);
            if (!res) continue;
            sec.hasHeader = true;
            sec.header = res->header;
            sec.headerFrom = res->from;
            if (const Entry* v = res->viewpoint.empty() ? nullptr : lib.entry(res->viewpoint)) sec.viewpoint = v->name;
            const std::string& h = res->header.blob;
            if (!h.empty() && !s.picturePaths.count(h)) {
                s.picturePaths[h] = lib.blobPath(h);
                if (const Artwork* a = lib.artworkForBlob(h)) s.pictureSizes[h] = {a->width, a->height};
            }
        }
    }
    // Each chapter's viewpoint (0.103): their icon, and their pictures' files.
    {
        size_t i = 0;
        for (const ManuscriptNode* n : lib.manuscriptOf(bookId)) {
            Source::Section& sec = s.sections[i++];
            if (n->kind != NodeKind::Chapter) continue;
            const Entry* e = lib.entry(icons::viewpointOf(lib, *n));
            if (!e || e->deleted) continue;
            sec.hasPov = true;
            sec.povId = e->id;
            sec.pov = icons::iconOf(*e);
            for (const std::string& h : {sec.pov.image, sec.pov.symbolImage})
                if (!h.empty() && !s.picturePaths.count(h)) {
                    s.picturePaths[h] = lib.blobPath(h);
                    if (const Artwork* a = lib.artworkForBlob(h)) s.pictureSizes[h] = {a->width, a->height};
                    else {   // a bible page's own picture: its size from the file, for the print checks
                        std::ifstream in(studio::detail::u8path(s.picturePaths[h]), std::ios::binary);
                        std::string head(256 * 1024, '\0');
                        in.read(head.data(), (std::streamsize)head.size());
                        head.resize((size_t)in.gcount());
                        const ImageInfo info = imageInfo(head);
                        if (info.width > 0) s.pictureSizes[h] = {info.width, info.height};
                    }
                }
        }
    }
    // Each chapter's date (0.85), when the timeline counts in the calendar's dates: its own, else its first placed scene's.
    if (const cal::StorySpan span = cal::storySpan(lib, bookId); span.on) {
        s.calendar = cal::calendarOfBook(lib, bookId);
        const json tl = json::parse(lib.setting("timeline:" + bookId), nullptr, false);
        if (!s.calendar.empty() && tl.is_object() && tl.contains("nodes") && tl["nodes"].is_object()) {
            const json& nodes = tl["nodes"];
            auto dayOf = [&](const Id& id) -> std::optional<std::int64_t> {
                if (!nodes.contains(id) || !nodes[id].is_object() || !nodes[id].contains("day") || !nodes[id]["day"].is_number()) return std::nullopt;
                return span.start + (std::int64_t)std::floor(nodes[id]["day"].get<double>());
            };
            size_t i = 0;
            const auto all = lib.manuscriptOf(bookId);
            for (const ManuscriptNode* n : all) {
                Source::Section& sec = s.sections[i++];
                if (n->kind != NodeKind::Chapter) continue;
                std::optional<std::int64_t> day = dayOf(n->id);
                if (!day)
                    for (const ManuscriptNode* m : all)
                        if (m->parentId == n->id && m->kind == NodeKind::Scene)
                            if ((day = dayOf(m->id))) break;
                if (day) { sec.dated = true; sec.calendarDay = *day; }
            }
        }
    }
    tidyForPrint(s);
    return s;
}

Design designFromJson(const std::string& text) {
    Design d = presets()[0].design;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return d;
    if (j.contains("preset")) d = preset(j.value("preset", "")).design;
    d.trim = j.value("trim", d.trim);
    d.font = j.value("font", d.font);
    d.fontSize = std::clamp(j.value("fontSize", d.fontSize), 7.0, 24.0);
    d.leading = std::clamp(j.value("leading", d.leading), d.fontSize, d.fontSize * 2.5);
    d.marginTop = std::clamp(j.value("marginTop", d.marginTop), 0.3, 2.0);
    d.marginBottom = std::clamp(j.value("marginBottom", d.marginBottom), 0.3, 2.0);
    d.marginInside = std::clamp(j.value("marginInside", d.marginInside), 0.3, 2.0);
    d.marginOutside = std::clamp(j.value("marginOutside", d.marginOutside), 0.3, 2.0);
    d.indentEm = std::clamp(j.value("indentEm", d.indentEm), 0.0, 4.0);
    d.justify = j.value("justify", d.justify);
    d.hyphenate = j.value("hyphenate", d.hyphenate);
    d.titlePage = j.value("titlePage", d.titlePage);
    d.copyrightPage = j.value("copyrightPage", d.copyrightPage);
    d.contents = j.value("contents", d.contents);
    d.leadIn = j.value("leadIn", d.leadIn);
    d.dropCap = j.value("dropCap", d.dropCap);
    d.frontRoman = j.value("frontRoman", d.frontRoman);
    d.chapterStyle = j.value("chapterStyle", d.chapterStyle);
    if (d.chapterStyle != "number" && d.chapterStyle != "numeral") d.chapterStyle = "title";
    d.chaptersOnRecto = j.value("chaptersOnRecto", d.chaptersOnRecto);
    d.runningHeads = j.value("runningHeads", d.runningHeads);
    d.chapterSinkLines = std::clamp(j.value("chapterSinkLines", d.chapterSinkLines), 0, 20);
    d.sceneBreak = j.value("sceneBreak", d.sceneBreak);
    {
        const std::string sb = j.value("sceneBreakBlob", std::string());
        if (sb.size() == 64 && std::all_of(sb.begin(), sb.end(), [](char c) { return std::isxdigit((unsigned char)c); })) d.sceneBreakBlob = sb;
    }
    d.comicScript = j.value("comicScript", d.comicScript);
    d.chapterDates = j.value("chapterDates", d.chapterDates);
    d.dateForm = j.value("dateForm", d.dateForm);
    if (d.dateForm != "numbers") d.dateForm = "words";
    d.dateEra = j.value("dateEra", d.dateEra);
    if (d.dateEra != "name" && d.dateEra != "none") d.dateEra = "abbr";
    d.povIcon = j.value("povIcon", d.povIcon);
    auto oneOf = [&](const char* key, std::string& v, std::initializer_list<const char*> ok) {
        const std::string got = j.value(key, v);
        for (const char* o : ok) if (got == o) { v = got; return; }
    };
    oneOf("povShow", d.povShow, {"portrait", "symbol", "initials"});
    oneOf("povPlace", d.povPlace, {"above", "before", "beside"});
    oneOf("povColour", d.povColour, {"full", "single", "text"});
    oneOf("povKey", d.povKey, {"none", "front", "back"});
    return d;
}

std::string designToJson(const Design& d) {
    return json{{"trim", d.trim}, {"font", d.font}, {"fontSize", d.fontSize}, {"leading", d.leading}, {"marginTop", d.marginTop},
                {"marginBottom", d.marginBottom}, {"marginInside", d.marginInside}, {"marginOutside", d.marginOutside},
                {"indentEm", d.indentEm}, {"justify", d.justify}, {"hyphenate", d.hyphenate}, {"titlePage", d.titlePage}, {"copyrightPage", d.copyrightPage}, {"contents", d.contents}, {"leadIn", d.leadIn}, {"dropCap", d.dropCap}, {"frontRoman", d.frontRoman}, {"chapterStyle", d.chapterStyle},
                {"chaptersOnRecto", d.chaptersOnRecto}, {"runningHeads", d.runningHeads}, {"chapterSinkLines", d.chapterSinkLines},
                {"sceneBreak", d.sceneBreak}, {"sceneBreakBlob", d.sceneBreakBlob}, {"comicScript", d.comicScript}, {"chapterDates", d.chapterDates}, {"dateForm", d.dateForm},
                {"dateEra", d.dateEra}, {"povIcon", d.povIcon}, {"povShow", d.povShow}, {"povPlace", d.povPlace}, {"povColour", d.povColour},
                {"povKey", d.povKey}}
        .dump();
}

Book typesetBook(const Source& src, const Design& design, const Resources& res) {
    Book book;
    if (src.title.empty() && src.sections.empty()) { book.error = "That book no longer exists."; return book; }
    book.design = design;
    book.title = src.title;
    book.author = src.author;
    book.picturePaths = src.picturePaths;
    const TrimSize& trim = trimSize(design.trim.empty() ? src.trim : design.trim);
    book.width = trim.widthIn * 72;
    book.height = trim.heightIn * 72;
    const Design& d = design;
    const double measure = book.width - (d.marginInside + d.marginOutside) * 72;
    const double textH = book.height - (d.marginTop + d.marginBottom) * 72;
    const int capacity = std::max(5, (int)std::floor(textH / d.leading));
    if (measure < 72 || textH < 144) { book.error = "The margins leave too little room for text on this trim size."; return book; }

    Family fam;
    if (!openFamily(res.fontsFolder, d.font, fam) && !openFamily(res.fontsFolder, presets()[0].design.font, fam)) {
        book.error = "The book fonts weren't found next to the program (" + res.fontsFolder + ").";
        return book;
    }
    // Symbols the book font doesn't have (✧, 🗡, ★: often a chapter's point-of-view mark) come from these.
    {
        const auto fl = studio::fonts::folders(res.fontsFolder);   // the symbols are the app's (the first folder)
        if (!fl.empty()) Fonts::get().useFallbacks(studio::detail::fromPath(studio::detail::u8path(fl[0]) / "symbols"));
    }
    const std::string lang = src.language.empty() ? "en-GB" : src.language;
    Hyphenator hyph;
    {
        std::string code = lang;
        for (auto& c : code) c = (char)std::tolower((unsigned char)c);
        if (code.rfind("en", 0) == 0 && code != "en-us") code = "en-gb";
        const std::string base = res.hyphenationFolder + "/hyph-" + code;
        if (d.hyphenate && !hyph.load(base + ".pat.txt", base + ".hyp.txt"))
            book.warnings.push_back("No hyphenation patterns for " + lang + ", so words aren't hyphenated.");
    }
    Shaper shaper(fam, lang, res.fontsFolder);
    Setter set(shaper, book, &hyph, d, measure);
    const float fs = (float)d.fontSize;

    // ---- The flow of lines.
    std::vector<Flow> flow;
    int paraNo = 0;
    int looseParas = 0;
    auto newPage = [&](Page::Kind k, bool recto, const std::string& chapter) {
        Flow f;
        f.kind = Flow::NewPage;
        f.pageKind = k;
        f.recto = recto;
        f.chapter = chapter;
        flow.push_back(f);
    };
    auto space = [&](int n) { if (n > 0) { Flow f; f.kind = Flow::Space; f.lines = n; flow.push_back(f); } };
    auto addLines = [&](std::vector<SetLine> lines, bool keepWithNext) {
        const int id = paraNo++;
        for (size_t i = 0; i < lines.size(); ++i) {
            Flow f;
            f.kind = Flow::Line;
            f.line = std::move(lines[i]);
            f.para = id;
            f.index = (int)i;
            f.count = (int)lines.size();
            f.keepWithNext = keepWithNext;
            flow.push_back(std::move(f));
        }
    };
    Span plain;
    Span italicLook; italicLook.italic = true;
    Span capsLook; capsLook.smallCaps = true;

    // A picture's size in pixels: from the artwork library, else read from its file.
    auto pixelSize = [&](const std::string& blob) -> std::pair<int, int> {
        int pw = 0, ph = 0;
        if (auto it = src.pictureSizes.find(blob); it != src.pictureSizes.end()) { pw = it->second.first; ph = it->second.second; }
        if ((pw <= 0 || ph <= 0) && src.picturePaths.count(blob)) {
            std::ifstream in(studio::detail::u8path(src.picturePaths.at(blob)), std::ios::binary);
            std::string head(256 * 1024, '\0');
            in.read(head.data(), (std::streamsize)head.size());
            head.resize((size_t)in.gcount());
            const ImageInfo info = imageInfo(head);
            pw = info.width; ph = info.height;
        }
        return {pw, ph};
    };
    // A scene break (0.91): the design's picture, a fifth of the text's width and at most three lines tall, or its mark.
    auto addSceneBreak = [&](const std::string& where) {
        space(1);
        if (!d.sceneBreakBlob.empty()) {
            const auto [pw, ph] = pixelSize(d.sceneBreakBlob);
            if (pw > 0 && ph > 0) {
                Flow f;
                f.kind = Flow::Picture;
                f.pic.blob = d.sceneBreakBlob;
                f.pic.role = "scenebreak";
                double w = measure * 0.2, h = w * ph / pw;
                const double cap = 3 * d.leading;
                if (h > cap) { w *= cap / h; h = cap; }
                f.pic.w = (float)w;
                f.pic.h = (float)h;
                f.pic.x = (float)((measure - w) / 2);
                f.lines = (int)std::ceil(h / d.leading);
                flow.push_back(std::move(f));
                space(1);
                return;
            }
            book.warnings.push_back("The scene break's picture couldn't be read, so “" + where + "” uses the mark instead.");
        }
        addLines({set.centred(d.sceneBreak, plain, fs, measure)}, true);
        space(1);
    };
    // A chapter's or part's header (0.89), put with the heading set from flow[headStart] on: a picture (the text's
    // width, a band cropped from its middle, or a small ornament) or a symbol set large, above, below or behind it.
    auto addHeader = [&](const headers::Header& h, size_t headStart, const std::string& where) {
        const size_t end = flow.size();
        if (h.blob.empty()) {
            if (!headers::printableSymbol(h.symbol)) return;
            SetLine l = set.centred(h.symbol, plain, fs * 2.4f, measure);
            l.lines = 3;
            if (h.title == "above") { space(1); addLines({std::move(l)}, false); return; }
            addLines({std::move(l)}, true);
            space(1);
            std::rotate(flow.begin() + (std::ptrdiff_t)headStart, flow.begin() + (std::ptrdiff_t)end, flow.end());   // before the title
            return;
        }
        const auto [pw, ph] = pixelSize(h.blob);
        if (pw <= 0 || ph <= 0) { book.warnings.push_back("The header picture of “" + where + "” couldn't be read and was left out."); return; }
        // The title's lines, for a title over the picture.
        std::vector<SetLine> over;
        int overLines = 0;
        if (h.title == "over") {
            for (size_t k = headStart; k < end; ++k) {
                if (flow[k].kind == Flow::Line) { over.push_back(flow[k].line); overLines += flow[k].line.lines; }
                else if (flow[k].kind == Flow::Space) { SetLine gap; gap.lines = flow[k].lines; over.push_back(gap); overLines += gap.lines; }
            }
            for (auto& l : over) for (auto& run : l.runs) run.light = h.tone == "dark";
            flow.erase(flow.begin() + (std::ptrdiff_t)headStart, flow.end());
        }
        Flow f;
        f.kind = Flow::Picture;
        f.pic.blob = h.blob;
        f.pic.role = "header";
        f.pic.tone = h.title == "over" ? (h.tone == "fade" ? 2 : 1) : 0;
        Placement& c = f.pic.crop;
        double w = measure, ht = 0;
        const double aspect = (double)ph / pw;   // height over width
        if (h.size == "band") {
            ht = measure / 3.2;
            // Cropped from the middle to the band's shape.
            if (aspect > ht / w) { c.cropH = (ht / w) / aspect; c.cropY = (1 - c.cropH) / 2; }
            else { c.cropW = aspect / (ht / w); c.cropX = (1 - c.cropW) / 2; }
        } else {
            if (h.size == "ornament") w = measure * 0.28;
            ht = w * aspect;
            const double cap = (h.size == "ornament" ? 6 : capacity * 0.45) * d.leading;
            if (ht > cap) { w *= cap / ht; ht = cap; }
        }
        // A title over it needs room: the picture is at least its lines and two more tall (cropped to fit).
        if (overLines > 0 && ht < (overLines + 2) * d.leading) {
            const double want = (overLines + 2) * d.leading;
            const double fullH = w * aspect;   // the whole picture's height at this width
            if (fullH >= want) { c.cropX = 0; c.cropW = 1; c.cropH = want / fullH; c.cropY = (1 - c.cropH) / 2; }
            else { c.cropY = 0; c.cropH = 1; c.cropW = std::max(0.05, fullH / want); c.cropX = (1 - c.cropW) / 2; }
            ht = want;
        }
        f.pic.w = (float)w;
        f.pic.h = (float)ht;
        f.pic.x = (float)((measure - w) / 2);
        f.lines = (int)std::ceil(ht / d.leading) + 1;
        f.over = std::move(over);
        if (h.title == "over") { flow.push_back(std::move(f)); return; }
        if (h.title == "above") { space(1); flow.push_back(std::move(f)); return; }
        flow.push_back(std::move(f));
        space(1);
        std::rotate(flow.begin() + (std::ptrdiff_t)headStart, flow.begin() + (std::ptrdiff_t)end, flow.end());   // before the title
    };

    // A viewpoint's icon (phase 4c, 0.103), drawn into `into` centred at (cx, cy): x from the text's left, y relative
    // to the line's baseline. Its picture (round), its symbol picture, its symbol or its initials, as the design says;
    // a disc of its colour ("full"), or in its colour or the ink with no disc. False when its symbol is in no font:
    // then it's left out (never a box) and named in book.missingSymbols.
    std::set<std::string> missingFor;
    auto rgbOf = [](const std::string& hex, std::uint8_t out[3]) {
        for (int i = 0; i < 3; ++i) out[i] = hex.size() == 7 ? (std::uint8_t)std::stoi(hex.substr(1 + i * 2, 2), nullptr, 16) : 0;
    };
    auto drawPov = [&](const Source::Section& sec, float diam, float cx, float cy, SetLine& into) -> bool {
        const icons::Icon& ic = sec.pov;
        const bool full = d.povColour == "full", inInk = d.povColour == "text";
        std::uint8_t colour[3], ink[3];
        rgbOf(ic.colour, colour);
        rgbOf(ic.ink, ink);
        auto hasPicture = [&](const std::string& b) { return !b.empty() && pixelSize(b).first > 0; };
        std::string show = "initials";
        if (d.povShow == "portrait" && hasPicture(ic.image)) show = "image";
        else if (d.povShow != "initials" && hasPicture(ic.symbolImage)) show = "symbolImage";
        else if (d.povShow != "initials" && !ic.symbol.empty()) show = "symbol";
        const float r = diam / 2;
        if (show == "symbol" && !set.covers(ic.symbol, plain, diam * 0.6f)) {
            if (missingFor.insert(sec.povId).second) book.missingSymbols.push_back(ic.name + " (" + ic.symbol + ")");
            return false;
        }
        auto ring = [&](float rr, const std::uint8_t* c, float width) {
            Disc dc{cx, cy, rr, width, {c[0], c[1], c[2]}};
            into.discs.push_back(dc);
        };
        const std::uint8_t black[3] = {0, 0, 0};
        const std::uint8_t* lineColour = inInk ? black : colour;
        if (show == "image" || show == "symbolImage") {
            float pr = r;
            if (full && show == "symbolImage") { ring(r, colour, 0); pr = r * 0.78f; }
            PicturePlacement pic;
            pic.blob = show == "image" ? ic.image : ic.symbolImage;
            pic.role = "viewpoint";
            pic.about = ic.name;
            pic.round = true;
            pic.x = cx - pr;
            pic.y = cy - pr;
            pic.w = pic.h = 2 * pr;
            // A square from the middle of the picture.
            const auto [pw, ph] = pixelSize(pic.blob);
            if (pw > ph) { pic.crop.cropW = (float)ph / pw; pic.crop.cropX = (1 - pic.crop.cropW) / 2; }
            else if (ph > pw) { pic.crop.cropH = (float)pw / ph; pic.crop.cropY = (1 - pic.crop.cropH) / 2; }
            into.pics.push_back(pic);
            if (show == "image") ring(r + diam * 0.03f, lineColour, std::max(0.6f, diam * 0.05f));
            return true;
        }
        const bool initials = show == "initials";
        const std::string text = initials ? ic.initials : ic.symbol;
        float gsize = initials ? diam * 0.42f : diam * 0.6f;
        if (full) ring(r, colour, 0);
        else if (initials) ring(r - diam * 0.03f, lineColour, std::max(0.6f, diam * 0.05f));
        else gsize = diam * 0.85f;   // a symbol alone, as large as the icon
        Span look;
        look.bold = initials;
        SetLine g = set.centred(text, look, gsize, diam);
        for (GlyphRun& run : g.runs) {
            for (float& x : run.xs) x += cx - r;
            run.y += cy + gsize * 0.35f;   // optically centred (about half the cap height below the middle)
            const std::uint8_t* c = full ? ink : inInk ? nullptr : colour;
            if (c) { run.coloured = true; run.rgb[0] = c[0]; run.rgb[1] = c[1]; run.rgb[2] = c[2]; }
            into.runs.push_back(std::move(run));
        }
        return true;
    };

    // The key of viewpoints (0.103): a page listing each viewpoint's icon and name, in the order they first tell a chapter.
    auto addKey = [&]() {
        std::vector<const Source::Section*> who;
        std::set<std::string> seen;
        for (const Source::Section& sec : src.sections)
            if (sec.kind == NodeKind::Chapter && sec.hasPov && sec.matter == 0 && seen.insert(sec.povId).second) who.push_back(&sec);
        if (who.empty()) return;
        newPage(Page::ChapterOpening, true, "Viewpoints");
        space(3);
        Block h;
        h.align = Align::Center;
        h.indent = false;
        h.scale = 1.3;
        h.spans.push_back({"Viewpoints"});
        auto hl = set.paragraph(h, true);
        for (auto& l : hl) l.lines = 2;
        addLines(std::move(hl), true);
        space(2);
        const float diam = fs * 1.7f, gap = fs * 0.8f;
        const float x0 = (float)(measure * 0.22);
        for (const Source::Section* sec : who) {
            Block nb;
            nb.align = Align::Left;
            nb.indent = false;
            nb.spans.push_back({sec->pov.name});
            auto nl = set.paragraph(nb, true);
            if (nl.empty()) continue;
            SetLine ln = std::move(nl[0]);
            for (GlyphRun& run : ln.runs) for (float& x : run.xs) x += x0 + diam + gap;
            ln.lines = 2;
            drawPov(*sec, diam, x0 + diam / 2, -fs * 0.35f, ln);
            addLines({std::move(ln)}, false);
        }
    };

    // Title page.
    if (d.titlePage) {
        newPage(Page::Title, true, {});
        space(capacity / 4);
        Block t;
        t.align = Align::Center;
        t.indent = false;
        t.scale = 2.2;
        t.spans.push_back({src.title});
        auto tl = set.paragraph(t, true);
        for (auto& l : tl) { l.lines = 3; }
        addLines(std::move(tl), true);
        if (!src.subtitle.empty()) {
            Block s = t;
            s.scale = 1.2;
            s.spans = {Span{src.subtitle}};
            s.spans[0].italic = true;
            auto sl = set.paragraph(s, true);
            for (auto& l : sl) l.lines = 2;
            addLines(std::move(sl), true);
        }
        if (!book.author.empty()) {
            space(capacity / 4);
            Block a = t;
            a.scale = 1.25;
            a.spans = {Span{book.author}};
            a.spans[0].smallCaps = true;
            addLines(set.paragraph(a, true), false);
        }
        // The back of the title page: only what the book's details say.
        std::vector<std::string> notice;
        if (d.copyrightPage) {
            if (!book.author.empty()) notice.push_back("Copyright \u00a9 " + std::to_string(src.year) + " " + book.author);
            if (!src.edition.empty()) notice.push_back(src.edition);
            // Every edition's ISBN, as copyright pages list them ("ISBN 978... (paperback)").
            if (!src.isbns.empty())
                for (auto& [label, isbn] : src.isbns) notice.push_back("ISBN " + isbn + (label.empty() ? "" : " (" + label + ")"));
            else if (!src.isbn.empty()) notice.push_back("ISBN " + src.isbn);
        }
        if (notice.empty()) newPage(Page::Blank, false, {});
        else {
            newPage(Page::Copyright, false, {});
            space(capacity - (int)notice.size());
            for (auto& n : notice) {
                Block c;
                c.align = Align::Left;
                c.indent = false;
                c.scale = 0.85;
                c.spans.push_back({n});
                addLines(set.paragraph(c, true), false);
            }
        }
    }
    // Contents: one line per part and chapter. Set now with placeholder page
    // numbers, then again once the pages are known (the length doesn't change).
    struct Entry { std::string title; bool part; size_t flowIndex; size_t openingFlow; };
    std::vector<Entry> entries;
    if (d.contents) {
        newPage(Page::Contents, true, {});
        space(3);
        Block h;
        h.align = Align::Center;
        h.indent = false;
        h.scale = 1.3;
        h.spans.push_back({"Contents"});
        auto hl = set.paragraph(h, true);
        for (auto& l : hl) l.lines = 2;
        addLines(std::move(hl), true);
        space(2);
        for (const Source::Section& sec : src.sections) {
            if (sec.kind == NodeKind::Scene) continue;
            entries.push_back({sec.title, sec.kind == NodeKind::Part, flow.size(), 0});
            addLines({set.contentsLine(sec.title, "000", sec.kind == NodeKind::Part, measure)}, false);
        }
    }
    if (d.povIcon && d.povKey == "front") addKey();   // after the contents (0.103)
    size_t entryNo = 0;
    bool bodyStarted = false;
    int chapterNo = 0;

    // The manuscript.
    std::string currentChapter;
    bool firstInChapter = true;
    bool afterBreak = true;
    int comicPage = 0;   // comic scripts: page numbers run on through the book
    if (src.sections.empty()) { book.error = "This book has no chapters yet."; return book; }
    for (const Source::Section& sec : src.sections) {
        const Source::Section* n = &sec;
        const std::string& title = sec.title;
        const json docj = json::parse(sec.kind == NodeKind::Chapter ? withOpening(sec.content) : sec.content, nullptr, false);
        std::vector<Block> blocks;
        if (docj.is_object()) {
            if (d.comicScript) comicBlocks(docj.value("content", json::array()), blocks, comicPage);
            else collectBlocks(docj.value("content", json::array()), blocks, Block{}, 0);
        }
        // Drop empty paragraphs (they'd be blank lines).
        blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [](const Block& b) {
            if (b.type != Block::Para) return false;
            for (auto& s : b.spans) for (char c : s.text) if (!std::isspace((unsigned char)c)) return false;
            return true;
        }), blocks.end());
        book.words += n->words;

        if (n->kind == NodeKind::Part) {
            currentChapter = title;
            if (entryNo < entries.size()) entries[entryNo++].openingFlow = flow.size();
            newPage(Page::PartTitle, true, title);
            bodyStarted = true;
            const bool head = n->hasHeader && !d.comicScript;
            space(head && !n->header.blob.empty() && n->header.size != "ornament" ? capacity / 8 : head ? capacity / 5 : capacity / 3);
            const size_t headStart = flow.size();
            Block t;
            t.align = Align::Center;
            t.indent = false;
            t.scale = 1.9;
            t.spans.push_back({title});
            auto tl = set.paragraph(t, true);
            for (auto& l : tl) l.lines = 3;
            addLines(std::move(tl), false);
            if (head) addHeader(n->header, headStart, title);
            // A part with its own text (an epigraph, an introduction) prints it on the part page.
            if (!blocks.empty()) space(2);
            for (auto& b : blocks) if (b.type == Block::Para) addLines(set.paragraph(b, true), false);
            newPage(Page::Blank, false, title);
            firstInChapter = true;
            continue;
        }
        if (n->kind == NodeKind::Chapter) {
            currentChapter = title;
            if (entryNo < entries.size()) entries[entryNo++].openingFlow = flow.size();
            // The story's first page is a right-hand page 1 when front matter has roman numbers.
            // Front matter stays in the front (roman page numbers, if chosen); the story starts after it.
            const bool front = n->matter == 1 && !bodyStarted;
            newPage(Page::ChapterOpening, d.chaptersOnRecto || (d.frontRoman && !bodyStarted && !front), title);
            if (!front) bodyStarted = true;
            // The viewpoint's icon (0.103) takes the place of the ornament their symbol gives by default, so it isn't printed twice.
            const bool pov = d.povIcon && n->hasPov && n->matter == 0 && !d.comicScript;
            const bool head = n->hasHeader && !d.comicScript && !(pov && n->headerFrom == "viewpoint" && n->header.blob.empty());
            const bool bigHead = head && !n->header.blob.empty() && n->header.size != "ornament";
            space(bigHead ? std::min(d.chapterSinkLines, 2) : head ? std::max(0, d.chapterSinkLines - 3) : d.chapterSinkLines);
            const size_t headStart = flow.size();
            // The viewpoint's icon above the heading; beside or before it goes on the heading's first line below.
            const float povDiam = (float)(d.povPlace == "above" ? fs * 2.6f : fs * 1.55f * 1.15f);
            if (pov && d.povPlace == "above") {
                SetLine il;
                il.lines = std::max(2, (int)std::ceil((povDiam + d.leading * 0.5) / d.leading));
                const float blockTop = -(float)(d.leading * (il.lines - 1) + d.leading * 0.78);
                if (drawPov(*n, povDiam, (float)(measure / 2), blockTop + (float)(il.lines * d.leading) / 2, il)) {
                    addLines({std::move(il)}, true);
                    space(1);
                }
            }
            // Before or beside the heading's first line: the line moves aside by half the icon and its gap.
            auto inlinePov = [&](std::vector<SetLine>& tl, float size) {
                if (!pov || d.povPlace == "above" || tl.empty()) return;
                SetLine& first = tl[0];
                const auto [a, b] = set.extent(first);
                if (b <= a) return;
                const float diam = size * 1.15f, gap = size * 0.45f, shift = (diam + gap) / 2;
                const bool before = d.povPlace == "before";
                for (GlyphRun& run : first.runs) for (float& x : run.xs) x += before ? shift : -shift;
                for (Rule& ru : first.rules) ru.x += before ? shift : -shift;
                const float cx = before ? a + shift - gap - diam / 2 : b - shift + gap + diam / 2;
                drawPov(*n, diam, cx, -size * 0.35f, first);
            };
            const ChapterName cn = splitChapterTitle(title);
            int num = 0;
            if (!cn.special && n->matter == 0) num = cn.number ? (chapterNo = cn.number) : ++chapterNo;
            Block t;
            t.align = Align::Center;
            t.indent = false;
            if (d.chapterStyle == "number" && num) {
                // "CHAPTER ONE", letter-spaced small capitals, then the title (if it has one).
                addLines({set.centred("Chapter " + (cn.word.empty() ? spelled(num) : cn.word), capsLook, fs * 1.05f, measure, fs * 0.12)}, true);
                if (!cn.rest.empty()) {
                    space(1);
                    t.scale = 1.45;
                    t.spans.push_back({cn.rest});
                    auto tl = set.paragraph(t, true);
                    for (auto& l : tl) l.lines = 2;
                    inlinePov(tl, fs * 1.45f);
                    addLines(std::move(tl), true);
                } else if (pov && d.povPlace != "above") {   // no title: beside the "CHAPTER ONE" line
                    SetLine& cl = flow.back().line;
                    std::vector<SetLine> one{cl};
                    inlinePov(one, fs * 1.3f);
                    cl = std::move(one[0]);
                }
            } else if (d.chapterStyle == "numeral" && num) {
                t.scale = 3.0;
                t.spans.push_back({std::to_string(num)});
                auto tl = set.paragraph(t, true);
                for (auto& l : tl) l.lines = 3;
                inlinePov(tl, fs * 2.0f);
                addLines(std::move(tl), true);
                if (!cn.rest.empty()) {
                    Block r2 = t;
                    r2.scale = 1.25;
                    r2.spans = {Span{cn.rest}};
                    r2.spans[0].italic = true;
                    auto rl = set.paragraph(r2, true);
                    for (auto& l : rl) l.lines = 2;
                    addLines(std::move(rl), true);
                }
            } else {
                t.scale = 1.55;
                t.spans.push_back({title});
                auto tl = set.paragraph(t, true);
                for (auto& l : tl) l.lines = 2;
                inlinePov(tl, fs * 1.55f);
                addLines(std::move(tl), true);
            }
            // Its date, in the style chosen, under the heading (0.85).
            if (d.chapterDates && n->dated && !src.calendar.empty()) {
                space(1);
                Block db;
                db.align = Align::Center;
                db.indent = false;
                Span sp{cal::formatDateStyled(src.calendar, cal::fromDay(src.calendar, n->calendarDay), d.dateForm, d.dateEra)};
                sp.italic = true;
                db.spans.push_back(sp);
                addLines(set.paragraph(db, true), true);
            }
            if (head) addHeader(n->header, headStart, title);
            space(2);
            firstInChapter = true;
            afterBreak = true;
        } else if (n->kind == NodeKind::Scene) {
            // A scene page from an import: a scene break between scenes, no title printed.
            if (!firstInChapter) addSceneBreak(title);
            afterBreak = true;
        }
        for (const Block& b : blocks) {
            if (b.type == Block::SceneBreak) {
                addSceneBreak(title);
                afterBreak = true;
                continue;
            }
            if (b.type == Block::Picture) {
                const auto [pw, ph] = pixelSize(b.blob);
                if (pw <= 0 || ph <= 0) { book.warnings.push_back("A picture in “" + title + "” couldn't be read and was left out."); continue; }
                Flow f;
                f.kind = Flow::Picture;
                f.pic.blob = b.blob;
                f.pic.crop = b.place;
                double w = measure * b.place.width;
                double h = w * (ph * b.place.cropH) / (pw * b.place.cropW);
                SetLine cap;
                int capLines = 0;
                if (!b.caption.empty()) {
                    Block c;
                    c.align = Align::Center;
                    c.indent = false;
                    c.scale = 0.9;
                    c.spans.push_back({b.caption});
                    c.spans[0].italic = true;
                    auto cl = set.paragraph(c, true);
                    capLines = (int)cl.size();
                    for (auto& l : cl) {   // captions are short: keep the first line
                        if (cap.runs.empty()) cap = std::move(l);
                    }
                    capLines = std::min(capLines, 1);
                }
                const double maxH = (capacity - 2 - capLines) * d.leading;
                if (h > maxH) { const double s = maxH / h; h *= s; w *= s; book.warnings.push_back("A picture in “" + title + "” was too tall for the page and was made smaller."); }
                f.pic.w = (float)w;
                f.pic.h = (float)h;
                f.pic.x = (float)(b.place.align == "left" ? 0 : b.place.align == "right" ? measure - w : (measure - w) / 2);
                f.lines = (int)std::ceil(h / d.leading) + 1 + capLines;
                f.caption = std::move(cap);
                flow.push_back(std::move(f));
                afterBreak = true;
                continue;
            }
            // Paragraph.
            if (b.breakBefore && !firstInChapter) newPage(Page::Text, false, title);
            if (b.spaceBefore && !firstInChapter) space(b.spaceBefore);
            bool loose = false;
            Block lead;
            const Block* use = &b;
            // The opening (0.94): on the paragraph snapshot() marked, the same one the ebook uses, in the story's chapters
            // (a prologue too, not front or back matter); never on a date line, an epigraph or a heading.
            const bool opening = b.opening && n->kind == NodeKind::Chapter && n->matter == 0;
            if (d.leadIn && opening) {
                // The chapter's first words (up to the first three) in small capitals.
                lead = b;
                std::vector<Span> spans;
                int words = 0;
                bool done = false;
                for (const Span& s : lead.spans) {
                    if (done || s.smallCaps) { spans.push_back(s); continue; }
                    size_t i = 0;
                    while (i < s.text.size() && words < 3) {
                        while (i < s.text.size() && s.text[i] == ' ') ++i;
                        size_t j = i;
                        while (j < s.text.size() && s.text[j] != ' ' && s.text[j] != '\n') ++j;
                        if (j > i) ++words;
                        i = j;
                        if (j < s.text.size() && s.text[j] == '\n') break;
                    }
                    Span head = s, rest = s;
                    head.text = s.text.substr(0, i);
                    head.smallCaps = true;
                    rest.text = s.text.substr(i);
                    if (!head.text.empty()) spans.push_back(head);
                    if (!rest.text.empty()) { spans.push_back(rest); done = true; }
                    if (words >= 3) done = true;
                }
                lead.spans = spans;
                use = &lead;
            }
            if (d.dropCap && opening) {
                // The first letter (with any opening quotation mark) comes out of the text and is set large.
                if (use != &lead) { lead = b; use = &lead; }
                for (auto& s : lead.spans) {
                    if (s.text.empty()) continue;
                    const std::u32string u = utf32(s.text);
                    size_t k = 0;
                    while (k < u.size() && !std::iswalnum((wint_t)u[k]) && k < 2) ++k;
                    if (k < u.size() && std::iswalnum((wint_t)u[k])) {
                        lead.drop = utf8(u.substr(0, k + 1));
                        if (s.smallCaps || d.leadIn) lead.drop = upper(lead.drop);
                        s.text = utf8(u.substr(k + 1));
                        lead.dropLines = 3;
                    }
                    break;
                }
            }
            auto lines = set.paragraph(*use, afterBreak || firstInChapter || b.heading || !use->drop.empty(), &loose);
            if (!use->drop.empty() && (int)lines.size() < use->dropLines) {
                // Too short for a drop cap: set it plainly.
                lead = b;
                lines = set.paragraph(lead, true, &loose);
            }
            if (loose) ++looseParas;
            addLines(std::move(lines), b.keepWithNext);
            space(b.spaceAfter);
            afterBreak = b.heading;
            firstInChapter = false;
        }
        if (n->kind == NodeKind::Chapter) firstInChapter = false;
    }
    if (d.povIcon && d.povKey == "back") addKey();   // at the very end (0.103)

    // ---- Pages.
    struct Placed { const Flow* f; int line; };
    std::vector<std::vector<Placed>> pages;
    std::vector<Page> meta;
    std::map<size_t, int> pageOfFlow;   // NewPage flow index -> the page it started
    int used = capacity;   // force a first page
    std::string chapterNow;
    auto paginate = [&]() {
    pages.clear();
    meta.clear();
    pageOfFlow.clear();
    used = capacity;
    chapterNow.clear();
    auto startPage = [&](Page::Kind k, const std::string& chapter) {
        pages.emplace_back();
        Page p;
        p.kind = k;
        p.chapter = chapter;
        meta.push_back(p);
        used = 0;
    };
    for (size_t i = 0; i < flow.size(); ++i) {
        const Flow& f = flow[i];
        if (f.kind == Flow::NewPage) {
            if (!f.chapter.empty()) chapterNow = f.chapter;
            // A right-hand page is odd-numbered: the next page's number is pages.size() + 1.
            if (f.recto && (pages.size() + 1) % 2 == 0) startPage(Page::Blank, chapterNow);
            startPage(f.pageKind, chapterNow);
            pageOfFlow[i] = (int)pages.size() - 1;
            continue;
        }
        if (f.kind == Flow::Space) {
            if (used == 0 && meta.back().kind == Page::Text) continue;   // no blank lines at the top of a page
            used = std::min(capacity, used + f.lines);
            continue;
        }
        int need = f.kind == Flow::Picture ? f.lines : f.line.lines;
        bool breakHere = used + need > capacity;
        // A picture just short of the room left on the page (under a chapter's heading, say) is made a little smaller
        // to stay, rather than leaving the heading alone and the picture on the next page (0.46): down to 70%.
        if (breakHere && f.kind == Flow::Picture && used > 0) {
            const int capLines = f.caption.runs.empty() ? 0 : 1;
            const int room = capacity - used - 1 - capLines, had = f.lines - 1 - capLines;
            if (room >= 4 && had > 0 && room >= had * 0.7) {
                Flow& g = flow[i];
                const float s = (float)(room * d.leading) / g.pic.h;
                if (s < 1) {
                    const float ow = g.pic.w;
                    g.pic.w *= s;
                    g.pic.h *= s;
                    g.pic.x += g.pic.crop.align == "left" ? 0 : g.pic.crop.align == "right" ? ow - g.pic.w : (ow - g.pic.w) / 2;
                    g.lines = room + 1 + capLines;
                    need = g.lines;
                    breakHere = used + need > capacity;
                }
            }
        }
        if (f.kind == Flow::Line && !breakHere && used + need == capacity) {
            // The last line of the page.
            if (f.count > 1 && f.index == 0) breakHere = true;                       // orphan: a paragraph's first line alone
            else if (f.count >= 4 && f.index == f.count - 2) breakHere = true;       // widow: its last line alone on the next page
            else if (f.keepWithNext && f.index == f.count - 1) breakHere = true;     // a heading at the foot of a page
        }
        if (f.kind == Flow::Line && f.keepWithNext && f.index == f.count - 1 && !breakHere) {
            // A heading (or scene break) needs two lines of text after it on the same page.
            int after = 0;
            for (size_t k = i + 1; k < flow.size() && after < 2; ++k) {
                if (flow[k].kind == Flow::Space) { after += flow[k].lines; continue; }
                if (flow[k].kind != Flow::Line) break;
                after += flow[k].line.lines;
            }
            if (used + need + std::min(after, 2) > capacity && meta.back().kind == Page::Text) breakHere = true;
        }
        if (breakHere) startPage(Page::Text, chapterNow);
        pages.back().push_back({&f, used});
        used += need;
    }
    };
    // Page numbers as printed: roman for the front matter (if chosen), then 1 from the story's first page.
    auto folioOf = [&](int index) {
        int bodyStart = -1;
        for (size_t k = 0; k < meta.size(); ++k)
            if (meta[k].kind == Page::PartTitle || meta[k].kind == Page::ChapterOpening) { bodyStart = (int)k; break; }
        if (!d.frontRoman || bodyStart < 0) return std::to_string(index + 1);
        return index < bodyStart ? roman(index + 1) : std::to_string(index - bodyStart + 1);
    };
    paginate();
    if (!entries.empty()) {
        // Now the page numbers are known: set the contents again and lay out the pages once more.
        for (const Entry& e : entries) {
            auto it = pageOfFlow.find(e.openingFlow);
            const std::string num = it == pageOfFlow.end() ? std::string() : folioOf(it->second);
            flow[e.flowIndex].line = set.contentsLine(e.title, num, e.part, measure);
        }
        paginate();
    }

    // ---- Page display lists.
    const float top = (float)(d.marginTop * 72);
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        Page page = meta[pi];
        const int number = (int)pi + 1;
        const bool recto = number % 2 == 1;
        const float left = (float)((recto ? d.marginInside : d.marginOutside) * 72);
        if (pages[pi].empty() && page.kind != Page::Title) page.kind = Page::Blank;
        auto place = [&](const SetLine& l, float baseline, float dx) {
            for (auto& r : l.runs) {
                GlyphRun g = r;
                g.y = baseline + r.y;
                for (auto& x : g.xs) x += left + dx;
                page.runs.push_back(std::move(g));
            }
            for (auto& r : l.rules) page.rules.push_back({r.x + left + dx, baseline + r.y, r.w, r.h});
            for (Disc dc : l.discs) { dc.cx += left + dx; dc.cy += baseline; page.discs.push_back(dc); }
            for (PicturePlacement pic : l.pics) { pic.x += left + dx; pic.y += baseline; page.pictures.push_back(pic); }
        };
        for (const Placed& p : pages[pi]) {
            const float lineTop = top + p.line * (float)d.leading;
            if (p.f->kind == Flow::Picture) {
                PicturePlacement pic = p.f->pic;
                pic.x += left;
                pic.y = lineTop + (float)d.leading * 0.35f;
                page.pictures.push_back(pic);
                // A title over the picture: its lines centred in it.
                if (!p.f->over.empty()) {
                    int total = 0;
                    for (const SetLine& l : p.f->over) total += l.lines;
                    float y = pic.y + (pic.h - total * (float)d.leading) / 2;
                    for (const SetLine& l : p.f->over) {
                        if (!l.runs.empty() || !l.rules.empty()) place(l, y + (float)d.leading * (l.lines - 1) + (float)(d.leading * 0.78), 0);
                        y += l.lines * (float)d.leading;
                    }
                }
                if (!p.f->caption.runs.empty())
                    place(p.f->caption, pic.y + pic.h + (float)d.leading * 1.1f, 0);
                continue;
            }
            const SetLine& l = p.f->line;
            // Baseline: on the grid line's baseline (the last of the lines it takes).
            const float baseline = lineTop + (float)d.leading * (l.lines - 1) + (float)(d.leading * 0.78);
            place(l, baseline, 0);
        }
        // Running heads and folios (none on title, part, blank or chapter-opening pages).
        if (page.kind == Page::Text || page.kind == Page::ChapterOpening || (page.kind == Page::Contents && d.frontRoman))
            page.folio = folioOf((int)pi);
        if (d.runningHeads && page.kind == Page::Text) {
            const std::string head = recto ? page.chapter : book.title;
            if (!head.empty()) {
                SetLine h = recto ? set.centred(head, italicLook, fs * 0.88f, measure) : set.centred(head, capsLook, fs * 0.88f, measure, fs * 0.08);
                place(h, top - (float)d.leading * 1.6f, 0);
            }
        }
        if (!page.folio.empty()) {
            SetLine fo = set.centred(page.folio, plain, fs * 0.88f, measure);
            place(fo, top + capacity * (float)d.leading + (float)d.leading * 1.6f, 0);
        }
        book.pages.push_back(std::move(page));
    }
    if (looseParas) book.warnings.push_back(std::to_string(looseParas) + (looseParas == 1 ? " paragraph is" : " paragraphs are") +
                                            " set looser than usual (long words or narrow columns).");
    for (const std::string& f : shaper.missing())
        book.warnings.push_back("Some words are marked for the typeface \"" + f + "\", which isn't in this world's fonts any more, so they're "
                                "set in the book's own.");
    book.ok = true;
    return book;
}

}  // namespace studio::typeset

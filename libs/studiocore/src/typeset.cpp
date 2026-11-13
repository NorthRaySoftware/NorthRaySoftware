// Typesetting: manuscript -> paragraphs -> lines (Knuth–Plass) -> pages.
#include "typeset_internal.hpp"

#include "internal.hpp"
#include "studio/document.hpp"

#include <algorithm>
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
    bool sameStyle(const Span& o) const {
        return bold == o.bold && italic == o.italic && smallCaps == o.smallCaps && underline == o.underline && strike == o.strike &&
               sup == o.sup && sub == o.sub;
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
    Shaper(const Family& fam, std::string lang) : fam_(fam), lang_(std::move(lang)) {}
    Style style(const Span& s, float size) const {
        Style st;
        st.face = fam_.pick(s.bold, s.italic);
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
private:
    const Family& fam_;
    std::string lang_;
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
        r.font = bookFont(st.face);
        r.size = st.size;
        r.y = st.rise;
        const std::string src = st.fakeSmallCaps ? upper(text) : text;
        auto& map = book_.fonts[r.font].toUnicode;
        for (size_t i = 0; i < s.glyphs.size(); ++i) {
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
            for (auto& p : pieces) {
                if (p.span < 0) { x += p.natural + (p.space ? extra : 0); continue; }
                emit(line, styles[p.span], p.text, (float)x);
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
};

}  // namespace

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
    return d;
}

std::string designToJson(const Design& d) {
    return json{{"trim", d.trim}, {"font", d.font}, {"fontSize", d.fontSize}, {"leading", d.leading}, {"marginTop", d.marginTop},
                {"marginBottom", d.marginBottom}, {"marginInside", d.marginInside}, {"marginOutside", d.marginOutside},
                {"indentEm", d.indentEm}, {"justify", d.justify}, {"hyphenate", d.hyphenate}, {"titlePage", d.titlePage}, {"copyrightPage", d.copyrightPage}, {"contents", d.contents}, {"leadIn", d.leadIn}, {"dropCap", d.dropCap}, {"frontRoman", d.frontRoman}, {"chapterStyle", d.chapterStyle},
                {"chaptersOnRecto", d.chaptersOnRecto}, {"runningHeads", d.runningHeads}, {"chapterSinkLines", d.chapterSinkLines},
                {"sceneBreak", d.sceneBreak}}
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
    Shaper shaper(fam, lang);
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
    size_t entryNo = 0;
    bool bodyStarted = false;
    int chapterNo = 0;

    // The manuscript.
    std::string currentChapter;
    bool firstInChapter = true;
    bool afterBreak = true;
    if (src.sections.empty()) { book.error = "This book has no chapters yet."; return book; }
    for (const Source::Section& sec : src.sections) {
        const Source::Section* n = &sec;
        const std::string& title = sec.title;
        const json docj = json::parse(sec.content, nullptr, false);
        std::vector<Block> blocks;
        if (docj.is_object()) collectBlocks(docj.value("content", json::array()), blocks, Block{}, 0);
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
            space(capacity / 3);
            Block t;
            t.align = Align::Center;
            t.indent = false;
            t.scale = 1.9;
            t.spans.push_back({title});
            auto tl = set.paragraph(t, true);
            for (auto& l : tl) l.lines = 3;
            addLines(std::move(tl), false);
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
            newPage(Page::ChapterOpening, d.chaptersOnRecto || (d.frontRoman && !bodyStarted), title);
            bodyStarted = true;
            space(d.chapterSinkLines);
            const ChapterName cn = splitChapterTitle(title);
            int num = 0;
            if (!cn.special) num = cn.number ? (chapterNo = cn.number) : ++chapterNo;
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
                    addLines(std::move(tl), true);
                }
            } else if (d.chapterStyle == "numeral" && num) {
                t.scale = 3.0;
                t.spans.push_back({std::to_string(num)});
                auto tl = set.paragraph(t, true);
                for (auto& l : tl) l.lines = 3;
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
                addLines(std::move(tl), true);
            }
            space(2);
            firstInChapter = true;
            afterBreak = true;
        } else if (n->kind == NodeKind::Scene) {
            // A scene page from an import: a scene break between scenes, no title printed.
            if (!firstInChapter) {
                space(1);
                addLines({set.centred(d.sceneBreak, plain, fs, measure)}, true);
                space(1);
            }
            afterBreak = true;
        }
        for (const Block& b : blocks) {
            if (b.type == Block::SceneBreak) {
                space(1);
                addLines({set.centred(d.sceneBreak, plain, fs, measure)}, true);
                space(1);
                afterBreak = true;
                continue;
            }
            if (b.type == Block::Picture) {
                int pw = 0, ph = 0;
                if (auto it = src.pictureSizes.find(b.blob); it != src.pictureSizes.end()) { pw = it->second.first; ph = it->second.second; }
                if ((pw <= 0 || ph <= 0) && src.picturePaths.count(b.blob)) {
                    // Not in the artwork library: read the size from the file.
                    std::ifstream in(studio::detail::u8path(src.picturePaths.at(b.blob)), std::ios::binary);
                    std::string head(256 * 1024, '\0');
                    in.read(head.data(), (std::streamsize)head.size());
                    head.resize((size_t)in.gcount());
                    const ImageInfo info = imageInfo(head);
                    pw = info.width; ph = info.height;
                }
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
            if (b.spaceBefore && !firstInChapter) space(b.spaceBefore);
            bool loose = false;
            Block lead;
            const Block* use = &b;
            if (d.leadIn && n->kind == NodeKind::Chapter && firstInChapter && !b.heading && b.align == Align::Justify) {
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
            if (d.dropCap && n->kind == NodeKind::Chapter && firstInChapter && !b.heading && b.align == Align::Justify) {
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
        const int need = f.kind == Flow::Picture ? f.lines : f.line.lines;
        bool breakHere = used + need > capacity;
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
        };
        for (const Placed& p : pages[pi]) {
            const float lineTop = top + p.line * (float)d.leading;
            if (p.f->kind == Flow::Picture) {
                PicturePlacement pic = p.f->pic;
                pic.x += left;
                pic.y = lineTop + (float)d.leading * 0.35f;
                page.pictures.push_back(pic);
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
    book.ok = true;
    return book;
}

}  // namespace studio::typeset

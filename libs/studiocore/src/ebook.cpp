// Ebook HTML from the manuscript's document model.
#include "studio/ebook.hpp"
#include "studio/fonts.hpp"
#include "internal.hpp"
#include <functional>
#include <filesystem>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace studio::ebook {

using nlohmann::json;

std::string escape(const std::string& t) {
    std::string out;
    out.reserve(t.size() + 8);
    for (char c : t) {
        // Control characters aren't allowed in XML (an EPUB is XHTML).
        if ((unsigned char)c < 0x20 && c != '\t' && c != '\n') continue;
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

namespace {

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4g", v);
    return b;
}

void inlineHtml(const json& content, std::string& out) {
    if (!content.is_array()) return;
    for (const json& n : content) {
        const std::string t = n.value("type", "");
        if (t == "hard_break") { out += "<br/>"; continue; }
        if (t != "text") continue;
        std::string open, close;
        if (n.contains("marks") && n["marks"].is_array())
            for (const json& m : n["marks"]) {
                const std::string mt = m.value("type", "");
                std::string o, c;
                if (mt == "em") { o = "<em>"; c = "</em>"; }
                else if (mt == "strong") { o = "<strong>"; c = "</strong>"; }
                else if (mt == "smallcaps") { o = "<span class=\"sc\">"; c = "</span>"; }
                else if (mt == "underline") { o = "<u>"; c = "</u>"; }
                else if (mt == "strike") { o = "<s>"; c = "</s>"; }
                else if (mt == "sup") { o = "<sup>"; c = "</sup>"; }
                else if (mt == "sub") { o = "<sub>"; c = "</sub>"; }
                else if (mt == "code") { o = "<code>"; c = "</code>"; }
                else if (mt == "font") {
                    // A world's own typeface (0.41); the id was checked when the document was saved.
                    std::string fam = m.contains("attrs") && m["attrs"].is_object() ? m["attrs"].value("family", "") : "";
                    if (!fam.empty() && fam.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos) {
                        o = "<span class=\"ff ff-" + fam + "\">";
                        c = "</span>";
                    }
                }
                else if (mt == "link") {
                    const std::string href = m.contains("attrs") ? m["attrs"].value("href", "") : "";
                    // Only web and mail links: nothing that runs or reaches into the reader's files.
                    if (href.rfind("https://", 0) == 0 || href.rfind("http://", 0) == 0 || href.rfind("mailto:", 0) == 0) {
                        o = "<a href=\"" + escape(href) + "\">";
                        c = "</a>";
                    }
                }
                open += o;
                close = c + close;
            }
        out += open + escape(n.value("text", "")) + close;
    }
}

// Where a picture is found. With `applied` set, the URL is of the picture already
// cropped (the EPUB), so the HTML carries no crop for a script to apply.
struct Pictures {
    std::function<std::string(const std::string& blob, const Placement& p)> url;
    bool applied = false;
    TonedPictureUrl toned;
};

void blocksHtml(const json& content, std::string& out, const Pictures& url, bool& afterBreak, int depth) {
    if (!content.is_array() || depth > 6) return;
    for (const json& n : content) {
        const std::string t = n.value("type", "");
        const json attrs = n.value("attrs", json::object());
        if (t == "paragraph") {
            std::string style = attrs.value("style", "body");
            static const char* known[] = {"body", "first", "centered", "epigraph", "attribution", "verse", "letter", "note"};
            if (std::find_if(std::begin(known), std::end(known), [&](const char* k) { return style == k; }) == std::end(known)) style = "body";
            if (style == "body" && afterBreak) style = "first";
            std::string inner;
            inlineHtml(n.value("content", json::array()), inner);
            if (inner.empty()) { out += "<p class=\"" + style + " empty\">&#160;</p>\n"; continue; }
            out += "<p class=\"" + style + "\">" + inner + "</p>\n";
            afterBreak = false;
        } else if (t == "heading") {
            const int level = std::clamp(attrs.value("level", 2), 1, 3) + 1;   // h1 is the chapter title
            std::string inner;
            inlineHtml(n.value("content", json::array()), inner);
            out += "<h" + std::to_string(level) + ">" + inner + "</h" + std::to_string(level) + ">\n";
            afterBreak = true;
        } else if (t == "blockquote") {
            out += "<blockquote>\n";
            bool ab = true;
            blocksHtml(n.value("content", json::array()), out, url, ab, depth + 1);
            out += "</blockquote>\n";
            afterBreak = true;
        } else if (t == "bullet_list" || t == "ordered_list") {
            const bool ordered = t == "ordered_list";
            const int start = attrs.value("start", 1);
            out += ordered ? (start != 1 ? "<ol start=\"" + std::to_string(start) + "\">\n" : std::string("<ol>\n")) : std::string("<ul>\n");
            for (const json& li : n.value("content", json::array())) {
                out += "<li>";
                bool ab = true;
                std::string inner;
                blocksHtml(li.value("content", json::array()), inner, url, ab, depth + 1);
                // A single paragraph inside an item doesn't need its own paragraph.
                if (inner.rfind("<p class=\"first\">", 0) == 0 && inner.find("<p", 3) == std::string::npos) {
                    inner = inner.substr(17);
                    const size_t endP = inner.rfind("</p>");
                    if (endP != std::string::npos) inner.erase(endP);
                }
                out += inner + "</li>\n";
            }
            out += ordered ? "</ol>\n" : "</ul>\n";
            afterBreak = true;
        } else if (t == "scene_break") {
            out += "<hr class=\"scene\"/>\n";
            afterBreak = true;
        } else if (t == "image") {
            const std::string blob = attrs.value("blob", "");
            if (blob.size() != 64) continue;
            Placement p;
            p.cropX = attrs.value("cropX", 0.0); p.cropY = attrs.value("cropY", 0.0);
            p.cropW = attrs.value("cropW", 1.0); p.cropH = attrs.value("cropH", 1.0);
            p.width = attrs.value("width", 1.0);
            p.align = attrs.value("align", "center");
            p = clampPlacement(p);
            const std::string alt = attrs.value("alt", ""), caption = attrs.value("caption", "");
            out += "<figure class=\"pic " + p.align + "\" style=\"width:" + num(p.width * 100) + "%\">";
            // The crop: the picture's visible part, as fractions (the stylesheet's script shows it).
            if (url.applied) out += "<div class=\"frame\">";
            else out += "<div class=\"frame\" data-crop=\"" + num(p.cropX) + " " + num(p.cropY) + " " + num(p.cropW) + " " + num(p.cropH) + "\">";
            out += "<img src=\"" + escape(url.url ? url.url(blob, p) : blob) + "\" alt=\"" + escape(alt) + "\"/></div>";
            if (!caption.empty()) out += "<figcaption>" + escape(caption) + "</figcaption>";
            out += "</figure>\n";
            afterBreak = true;
        }
    }
}

}  // namespace

namespace {

std::string docHtml(const std::string& docJson, const Pictures& url) {
    const json d = json::parse(docJson, nullptr, false);
    if (!d.is_object()) return {};
    std::string out;
    bool afterBreak = true;
    blocksHtml(d.value("content", json::array()), out, url, afterBreak, 0);
    // Empty paragraphs at the end are just the editor's last line.
    for (const char* tail : {"<p class=\"body empty\">&#160;</p>\n", "<p class=\"first empty\">&#160;</p>\n"}) {
        const std::string t = tail;
        while (out.size() >= t.size() && out.compare(out.size() - t.size(), t.size(), t) == 0) out.resize(out.size() - t.size());
    }
    return out;
}

// One UTF-8 character's length from its first byte.
size_t charLen(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }

// A CSS string ("..."): quotes and backslashes escaped, everything outside ASCII as \HEX.
std::string cssString(const std::string& t) {
    std::string out = "\"";
    for (size_t i = 0; i < t.size();) {
        const unsigned char c = (unsigned char)t[i];
        const size_t n = charLen(c);
        if (c < 0x20) { ++i; continue; }
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; ++i; continue; }
        if (n == 1) { out += (char)c; ++i; continue; }
        std::uint32_t cp = n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (size_t k = 1; k < n && i + k < t.size(); ++k) cp = (cp << 6) | ((unsigned char)t[i + k] & 0x3F);
        char b[16];
        std::snprintf(b, sizeof b, "\\%X ", (unsigned)cp);
        out += b;
        i += n;
    }
    return out + "\"";
}

Content buildWith(const typeset::Source& src, const Pictures& url, const Style& style) {
    Content c;
    c.title = src.title;
    c.subtitle = src.subtitle;
    c.author = src.author;
    c.language = src.language.empty() ? "en-GB" : src.language;
    int chapterNo = 0;
    for (const auto& sec : src.sections) {
        if (sec.kind == NodeKind::Scene && !c.chapters.empty()) {
            // A scene page from an import continues its chapter after a scene break.
            c.chapters.back().html += "<hr class=\"scene\"/>\n" + docHtml(sec.content, url);
            c.chapters.back().words += sec.words;
            continue;
        }
        Chapter ch;
        ch.title = sec.title;
        ch.part = sec.kind == NodeKind::Part;
        ch.words = sec.words;
        std::string body = docHtml(sec.content, url);
        std::string heading = "<h1>" + escape(sec.title) + "</h1>\n";
        auto withHeader = [&](const std::string& h) {
            if (!sec.hasHeader) return h;
            const headers::Header& hd = sec.header;
            if (hd.blob.empty()) return headerHtml(hd, {}, h);
            const int tone = hd.title == "over" ? (hd.tone == "fade" ? 2 : 1) : 0;
            if (tone && url.toned) return headerHtml(hd, url.toned(hd.blob, tone), h, true);
            return headerHtml(hd, url.url ? url.url(hd.blob, Placement{}) : hd.blob, h);
        };
        if (ch.part) heading = withHeader(heading);
        if (!ch.part) {
            // Numbered as the printed book is: chapters in the story, not prologues or front and back matter.
            const typeset::ChapterTitle t = typeset::chapterTitleParts(sec.title);
            int number = 0;
            if (!t.special && sec.matter == 0) number = t.number ? (chapterNo = t.number) : ++chapterNo;
            heading = chapterHeading(sec.title, number, t.word, t.rest, style);
            if (style.chapterDates && sec.dated && !src.calendar.empty())
                heading += "<p class=\"chapdate\">" + escape(cal::formatDateStyled(src.calendar, cal::fromDay(src.calendar, sec.calendarDay), style.dateForm, style.dateEra)) + "</p>\n";
            heading = withHeader(heading);
            if (sec.matter == 0) body = markOpening(body, style);   // a prologue opens like a chapter too
        }
        ch.html = std::string("<section class=\"") + (ch.part ? "part" : "chapter") + "\">\n" + heading + body + "</section>\n";
        c.chapters.push_back(std::move(ch));
    }
    return c;
}

Pictures plain(const PictureUrl& url) {
    Pictures p;
    if (url) p.url = [url](const std::string& blob, const Placement&) { return url(blob); };
    return p;
}

}  // namespace

std::string documentHtml(const std::string& docJson, const PictureUrl& url) { return docHtml(docJson, plain(url)); }

Content build(const typeset::Source& src, const PictureUrl& url, const Style& style) { return buildWith(src, plain(url), style); }

Content buildCropped(const typeset::Source& src, const CroppedPictureUrl& url, const Style& style) {
    Pictures p;
    p.url = url;
    p.applied = true;
    return buildWith(src, p, style);
}

Content buildCropped(const typeset::Source& src, const CroppedPictureUrl& url, const Style& style, const TonedPictureUrl& toned) {
    Pictures p;
    p.url = url;
    p.applied = true;
    p.toned = toned;
    return buildWith(src, p, style);
}

std::string headerHtml(const headers::Header& h, const std::string& src, const std::string& heading, bool baked) {
    if (h.blob.empty()) {
        if (!headers::printableSymbol(h.symbol)) return heading;
        const std::string orn = "<p class=\"ornament\" aria-hidden=\"true\">" + escape(h.symbol) + "</p>\n";
        return h.title == "above" ? heading + orn : orn + heading;
    }
    // A decorative picture has an empty description (readers skip it); one described says what it shows.
    const std::string img = "<img src=\"" + escape(src) + "\" alt=\"" + escape(h.alt) + "\"" + (h.alt.empty() ? " role=\"presentation\"" : "") + "/>";
    const std::string cls = "chead size-" + h.size + " title-" + h.title + (h.title == "over" ? " tone-" + h.tone : std::string()) + (baked ? " baked" : "");
    if (h.title == "over")
        return "<div class=\"" + cls + "\"><div class=\"frame\">" + img + "</div><div class=\"overtitle\">\n" + heading + "</div></div>\n";
    const std::string fig = "<figure class=\"" + cls + "\"><div class=\"frame\">" + img + "</div></figure>\n";
    return h.title == "above" ? heading + fig : fig + heading;
}

static std::string base();

Style styleFrom(const typeset::Design& d) {
    Style s;
    s.chapterStyle = d.chapterStyle;
    s.leadIn = d.leadIn;
    s.dropCap = d.dropCap;
    s.justify = d.justify;
    s.hyphenate = d.hyphenate;
    s.indentEm = d.indentEm;
    s.sceneBreak = d.sceneBreak.empty() ? std::string("* * *") : d.sceneBreak;
    s.chapterDates = d.chapterDates;
    s.dateForm = d.dateForm;
    s.dateEra = d.dateEra;
    return s;
}

std::string chapterHeading(const std::string& title, int number, const std::string& word, const std::string& rest, const Style& style) {
    if (number > 0 && style.chapterStyle == "number") {
        // "Chapter One" letter-spaced over the title; the whole heading is still the chapter's name for readers.
        std::string h = "<h1 class=\"numbered\"><span class=\"chapnum\">Chapter " + escape(word.empty() ? typeset::spelledNumber(number) : word) + "</span>";
        if (!rest.empty()) h += "<span class=\"chaptitle\">" + escape(rest) + "</span>";
        return h + "</h1>\n";
    }
    if (number > 0 && style.chapterStyle == "numeral") {
        std::string h = "<h1 class=\"numeral\"><span class=\"numeral\">" + std::to_string(number) + "</span>";
        if (!rest.empty()) h += "<span class=\"chaptitle\">" + escape(rest) + "</span>";
        return h + "</h1>\n";
    }
    return "<h1>" + escape(title) + "</h1>\n";
}

std::string markOpening(const std::string& html, const Style& style) {
    if (!style.leadIn && !style.dropCap) return html;
    const std::string open = "<p class=\"first\">";
    const size_t at = html.find(open);
    if (at == std::string::npos) return html;
    // Only when nothing but other first-paragraph kinds (an epigraph, a heading) comes before it.
    const size_t start = at + open.size();
    size_t stop = html.find('<', start);
    if (stop == std::string::npos) stop = html.size();
    const std::string text = html.substr(start, stop - start);
    if (text.empty()) return html;
    size_t i = 0;
    std::string drop;
    if (style.dropCap) {
        // The first letter, with any opening quotation marks before it.
        while (i < text.size()) {
            const size_t n = charLen((unsigned char)text[i]);
            const std::string ch = text.substr(i, n);
            const bool mark = ch == "\"" || ch == "'" || ch == "\u201C" || ch == "\u2018" || ch == "\u00AB" || ch == "&quot;";
            if (ch == "&") {   // an entity (&quot;): keep it whole
                const size_t semi = text.find(';', i);
                if (semi == std::string::npos) break;
                const std::string ent = text.substr(i, semi + 1 - i);
                drop += ent;
                i = semi + 1;
                if (ent != "&quot;") break;
                continue;
            }
            drop += ch;
            i += n;
            if (!mark) break;
        }
        if (drop.empty() || i >= text.size() || text[i] == ' ') { drop.clear(); i = 0; }   // a one-letter word: no drop cap
    }
    size_t lead = i;
    if (style.leadIn) {
        int words = drop.empty() ? 0 : 1;
        if (!drop.empty()) while (lead < text.size() && text[lead] != ' ') ++lead;   // the rest of the first word
        while (lead < text.size() && words < 3) {
            while (lead < text.size() && text[lead] == ' ') ++lead;
            const size_t w0 = lead;
            while (lead < text.size() && text[lead] != ' ') ++lead;
            if (lead > w0) ++words;
        }
    }
    std::string out = html.substr(0, at) + "<p class=\"first opening\">";
    if (!drop.empty()) out += "<span class=\"dropcap\">" + drop + "</span>";
    if (lead > i) out += "<span class=\"leadin\">" + text.substr(i, lead - i) + "</span>";
    out += html.substr(start + lead);
    return out;
}

std::string stylesheet(const Style& style) {
    std::string extra;
    if (!style.fontFamily.empty()) extra += "body { font-family: " + cssString(style.fontFamily) + ", serif; }\n";
    char b[160];
    std::snprintf(b, sizeof b, "p { text-indent: %.2gem; }\n", style.spaced ? 0.0 : style.indentEm);
    extra += b;
    if (style.spaced) extra += "p { margin: 0 0 0.75em; }\np.empty { margin: 0; }\n";
    if (!style.justify) extra += "p { text-align: left; }\n";
    if (!style.hyphenate) extra += "p { hyphens: manual; -webkit-hyphens: manual; }\n";
    std::string brk = style.sceneBreak.empty() ? std::string("* * *") : style.sceneBreak;
    for (size_t at = 0; (at = brk.find(' ', at)) != std::string::npos;) brk.replace(at, 1, "\u2003");   // wide spaces, as in print
    extra += "hr.scene::after { content: " + cssString(brk) + "; }\n";
    return base() + extra;
}

std::string base() {
    return R"CSS(/* NorthRay Writing Studio: reading stylesheet (ebook preview and EPUB). */
section { break-before: page; page-break-before: always; }
section.part { text-align: center; padding-top: 30%; }
h1 { font-size: 1.5em; font-weight: normal; text-align: center; margin: 2.5em 0 1.6em; line-height: 1.25; }
section.part h1 { font-size: 1.7em; }
h2 { font-size: 1.2em; text-align: center; margin: 1.4em 0 0.8em; }
h3 { font-size: 1.05em; text-align: center; margin: 1.2em 0 0.6em; }
h4 { font-size: 1em; font-style: italic; font-weight: normal; margin: 1em 0 0.4em; }
p { margin: 0; text-indent: 1.2em; text-align: justify; hyphens: auto; -webkit-hyphens: auto; orphans: 2; widows: 2; }
p.first, h1 + p, h2 + p, h3 + p, h4 + p, hr + p, figure + p, blockquote + p { text-indent: 0; }
p.centered { text-align: center; text-indent: 0; }
p.epigraph { font-style: italic; margin: 0 2em; text-indent: 0; text-align: left; font-size: 0.93em; }
p.attribution { text-align: right; margin: 0 2em 1em; text-indent: 0; font-size: 0.93em; }
p.verse { text-align: left; text-indent: 0; margin-left: 2em; }
p.letter { margin: 0 1.5em; text-indent: 0; }
p.note { font-size: 0.88em; text-indent: 0; }
p.empty { text-indent: 0; }
.sc { font-variant: small-caps; letter-spacing: 0.04em; }
blockquote { margin: 1em 1.5em; font-size: 0.95em; }
ul, ol { margin: 0.6em 0 0.6em 1.6em; padding: 0; }
hr.scene { border: 0; margin: 1.1em 0; text-align: center; height: 1.2em; }
hr.scene::after { content: "*\2003*\2003*"; }
figure.pic { margin: 1.2em auto; break-inside: avoid; page-break-inside: avoid; text-align: center; }
figure.pic.left { margin-left: 0; }
figure.pic.right { margin-right: 0; }
figure.pic .frame { overflow: hidden; line-height: 0; }
figure.pic img { max-width: 100%; }
figcaption { font-style: italic; font-size: 0.9em; margin-top: 0.4em; text-align: center; }
a { color: inherit; }
h1 span.chapnum { display: block; font-size: 0.6em; letter-spacing: 0.2em; text-transform: uppercase; margin-bottom: 1em; }
p.chapdate { text-align: center; text-indent: 0; font-style: italic; margin: -1em 0 2em; }
h1 span.numeral { display: block; font-size: 2.6em; line-height: 1; margin-bottom: 0.25em; }
h1 span.chaptitle { display: block; }
h1.numeral span.chaptitle { font-style: italic; font-size: 0.85em; }
span.leadin { font-variant: small-caps; letter-spacing: 0.05em; }
span.dropcap { float: left; font-size: 3.4em; line-height: 0.82; margin: 0.06em 0.08em 0 0; }
.chead { margin: 1.5em auto 0; text-align: center; break-inside: avoid; page-break-inside: avoid; }
.chead .frame { line-height: 0; overflow: hidden; }
.chead img { display: block; margin: 0 auto; max-width: 100%; }
.chead.size-full img, .chead.size-band img { width: 100%; }
.chead.size-band .frame { aspect-ratio: 3.2 / 1; }
.chead.size-band img { height: 100%; object-fit: cover; }
.chead.size-ornament img { width: 28%; }
.chead.title-above { margin-top: 0; margin-bottom: 1.6em; }
.chead.title-over { position: relative; margin-bottom: 1.6em; }
.chead.title-over .frame { min-height: 9em; }
.chead.title-over img { width: 100%; height: 100%; min-height: 9em; object-fit: cover; }
.chead.title-over .overtitle { position: absolute; left: 0; right: 0; top: 0; bottom: 0; display: flex; flex-direction: column; justify-content: center; }
.chead.title-over .overtitle h1 { margin: 0.3em 0; }
.chead.title-over .overtitle p.chapdate { margin: 0; }
.chead.tone-dark .overtitle { color: #fff; }
.chead.tone-dark:not(.baked) img { filter: brightness(0.42); }
.chead.tone-fade:not(.baked) img { opacity: 0.3; }
p.ornament { text-align: center; text-indent: 0; font-size: 2.4em; line-height: 1.2; margin: 1.2em 0 0; }
)CSS";
}


std::vector<std::string> fontsUsed(const typeset::Source& src) {
    std::vector<std::string> out;
    std::function<void(const json&)> walk = [&](const json& n) {
        if (n.is_object()) {
            if (n.contains("marks") && n["marks"].is_array())
                for (const json& m : n["marks"])
                    if (m.is_object() && m.value("type", "") == "font" && m.contains("attrs") && m["attrs"].is_object()) {
                        const std::string f = m["attrs"].value("family", "");
                        if (!f.empty() && std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
                    }
            if (n.contains("content")) walk(n["content"]);
        } else if (n.is_array()) {
            for (const json& c : n) walk(c);
        }
    };
    for (const auto& sec : src.sections) {
        if (sec.content.find("\"font\"") == std::string::npos) continue;
        walk(json::parse(sec.content, nullptr, false));
    }
    return out;
}

std::string worldFontCss(const std::string& fontsFolders, const std::string& id, const std::function<std::string(const std::string& path)>& url) {
    if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) return {};
    const std::string dir = studio::fonts::familyFolder(fontsFolders, id);
    if (dir.empty()) return {};
    namespace fs = std::filesystem;
    std::string css;
    std::error_code ec;
    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator(detail::u8path(dir), ec)) files.push_back(detail::fromPath(e.path()));
    std::sort(files.begin(), files.end());
    struct W { const char* suffix; const char* weight; const char* style; };
    for (const W& w : {W{"-Regular.ttf", "normal", "normal"}, W{"-Italic.ttf", "normal", "italic"}, W{"-Bold.ttf", "bold", "normal"},
                       W{"-BoldItalic.ttf", "bold", "italic"}}) {
        const std::string suf = w.suffix;
        for (const std::string& f : files) {
            const std::string name = detail::fromPath(detail::u8path(f).filename());
            if (name.size() <= suf.size() || name.compare(name.size() - suf.size(), suf.size(), suf) != 0) continue;
            if (suf == "-Italic.ttf" && name.size() > 15 && name.compare(name.size() - 15, 15, "-BoldItalic.ttf") == 0) continue;
            css += "@font-face { font-family: \"NR F " + id + "\"; font-weight: " + w.weight + "; font-style: " + w.style + "; src: url(\"" +
                   url(f) + "\"); }\n";
        }
    }
    if (css.empty()) return {};
    css += "span.ff-" + id + " { font-family: \"NR F " + id + "\", serif; }\n";
    return css;
}

}  // namespace studio::ebook

// Ebook HTML from the manuscript's document model.
#include "studio/ebook.hpp"

#include <algorithm>
#include <cmath>
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

Content buildWith(const typeset::Source& src, const Pictures& url) {
    Content c;
    c.title = src.title;
    c.subtitle = src.subtitle;
    c.author = src.author;
    c.language = src.language.empty() ? "en-GB" : src.language;
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
        const std::string body = docHtml(sec.content, url);
        ch.html = std::string("<section class=\"") + (ch.part ? "part" : "chapter") + "\">\n<h1>" + escape(sec.title) + "</h1>\n" + body + "</section>\n";
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

Content build(const typeset::Source& src, const PictureUrl& url) { return buildWith(src, plain(url)); }

Content buildCropped(const typeset::Source& src, const CroppedPictureUrl& url) {
    Pictures p;
    p.url = url;
    p.applied = true;
    return buildWith(src, p);
}

std::string stylesheet() {
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
)CSS";
}

}  // namespace studio::ebook

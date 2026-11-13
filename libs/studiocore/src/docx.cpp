// Word (.docx) and OpenDocument (.odt) import.
//
// Both are ZIP files of XML. Each is turned into simple, semantic HTML
// (headings, paragraphs with their style names as classes, italic/bold/small
// caps runs, lists, quotes, tables, links, pictures, footnotes) and then goes
// through the same converter as EPUB chapters, so styles and scene breaks are
// handled the same way everywhere.
#include "importutil.hpp"
#include "internal.hpp"
#include "markup.hpp"
#include "studio/document.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <functional>
#include <map>
#include <set>

namespace studio::import {

namespace mk = studio::markup;
using detail::Zip;
using detail::lowerAscii;

namespace {

std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

std::string classOf(const std::string& styleName) {
    std::string c;
    for (unsigned char ch : lowerAscii(styleName)) c += std::isalnum(ch) ? (char)ch : '-';
    return c;
}

std::string imageTypeOf(const std::string& b) {
    if (b.size() > 3 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8) return "image/jpeg";
    if (b.size() > 8 && b.compare(1, 3, "PNG") == 0) return "image/png";
    if (b.size() > 6 && b.compare(0, 3, "GIF") == 0) return "image/gif";
    if (b.size() > 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) return "image/webp";
    return {};
}

// Character formatting, as on/off/unset.
struct Fmt {
    int italic = -1, bold = -1, underline = -1, smallcaps = -1, strike = -1, sup = -1, sub = -1;
    void over(const Fmt& o) {
        for (auto m : {&Fmt::italic, &Fmt::bold, &Fmt::underline, &Fmt::smallcaps, &Fmt::strike, &Fmt::sup, &Fmt::sub})
            if (o.*m >= 0) this->*m = o.*m;
    }
};

std::string openTags(const Fmt& f) {
    std::string o;
    if (f.italic == 1) o += "<em>";
    if (f.bold == 1) o += "<strong>";
    if (f.underline == 1) o += "<u>";
    if (f.smallcaps == 1) o += "<span class=\"smallcaps\">";
    if (f.strike == 1) o += "<s>";
    if (f.sup == 1) o += "<sup>";
    else if (f.sub == 1) o += "<sub>";
    return o;
}
std::string closeTags(const Fmt& f) {
    std::string o;
    if (f.sup == 1) o += "</sup>";
    else if (f.sub == 1) o += "</sub>";
    if (f.strike == 1) o += "</s>";
    if (f.smallcaps == 1) o += "</span>";
    if (f.underline == 1) o += "</u>";
    if (f.bold == 1) o += "</strong>";
    if (f.italic == 1) o += "</em>";
    return o;
}

// The paragraph-level information a reader collects.
struct Para {
    std::string html;            // inline content
    std::string styleName;       // the style's display name ("Quote", "Heading 1")
    int heading = 0;             // 1..6
    bool center = false;
    bool listItem = false;
    bool ordered = false;
    bool quote = false;
};

// Turns paragraphs into HTML: groups list items and quotes, and adds notes.
std::string toHtml(const std::vector<Para>& paras, const std::vector<std::string>& notes) {
    std::string out = "<html><body>";
    int list = 0;  // 0 none, 1 ul, 2 ol
    bool quote = false;
    for (const Para& p : paras) {
        const int want = p.listItem ? (p.ordered ? 2 : 1) : 0;
        if (list && want != list) { out += list == 2 ? "</ol>" : "</ul>"; list = 0; }
        if (quote && !p.quote) { out += "</blockquote>"; quote = false; }
        if (p.quote && !quote) { out += "<blockquote>"; quote = true; }
        if (want && !list) { out += want == 2 ? "<ol>" : "<ul>"; list = want; }
        const std::string cls = p.styleName.empty() ? "" : " class=\"" + esc(classOf(p.styleName)) + "\"";
        const std::string st = p.center ? " style=\"text-align:center\"" : "";
        if (p.heading) {
            const std::string h = "h" + std::to_string(std::min(p.heading, 6));
            out += "<" + h + ">" + p.html + "</" + h + ">";
        } else if (want) {
            out += "<li><p" + cls + st + ">" + p.html + "</p></li>";
        } else {
            out += "<p" + cls + st + ">" + p.html + "</p>";
        }
    }
    if (list) out += list == 2 ? "</ol>" : "</ul>";
    if (quote) out += "</blockquote>";
    if (!notes.empty()) {
        out += "<h1>Notes</h1>";
        for (size_t i = 0; i < notes.size(); ++i) out += "<p class=\"note\">" + std::to_string(i + 1) + ". " + notes[i] + "</p>";
    }
    return out + "</body></html>";
}

bool on(const mk::Token& t) {
    const std::string v = lowerAscii(t.attr("val"));
    return v.empty() || (v != "0" && v != "false" && v != "off" && v != "none");
}

// Finishes a plan built from HTML.
// Takes the plan by reference: the context's image loader adds to this same plan.
Plan finish(Plan& plan, const std::string& html, const mk::HtmlContext& ctx) {
    if (std::getenv("STUDIO_DEBUG_IMPORT")) std::fprintf(stderr, "%s\n", html.c_str());
    std::string stem = plan.sourceName.substr(0, plan.sourceName.rfind('.'));
    if (plan.meta.title.empty()) plan.meta.title = stem;
    detail::sectionsFromHtml(plan, html, ctx, plan.meta.title);
    std::int64_t total = 0;
    for (auto& s : plan.sections) total += s.words;
    if (plan.sections.empty() || total == 0) { plan.error = "No readable text was found in this document."; return std::move(plan); }
    plan.ok = true;
    return std::move(plan);
}

}  // namespace

// ------------------------------------------------------------------ Word

Plan readDocx(const std::string& bytes, const std::string& name, const Limits& lim) {
    Plan plan;
    plan.format = Format::Docx;
    plan.sourceName = name;
    Zip zip;
    std::string err;
    if (!zip.open(bytes, lim, err)) { plan.error = "This Word file is damaged or isn't really a Word file."; return plan; }
    std::string documentXml;
    if (!zip.read("word/document.xml", documentXml, err)) {
        plan.error = zip.encryptedSeen() ? "This Word document is password-protected. Remove the password in Word and import it again."
                                         : "This doesn't look like a Word document (it has no word/document.xml).";
        return plan;
    }
    if (zip.has("EncryptionInfo") || zip.has("EncryptedPackage")) {
        plan.error = "This Word document is password-protected. Remove the password in Word and import it again.";
        return plan;
    }

    // Styles: id -> display name, heading level, formatting, based-on.
    struct Style { std::string name, basedOn, numId; int outline = -1; Fmt fmt; bool center = false; };
    std::map<std::string, Style> styles;
    {
        std::string xml;
        if (zip.read("word/styles.xml", xml, err)) {
            auto toks = mk::tokenize(mk::toUtf8(xml));
            Style* cur = nullptr;
            std::string curId;
            bool inRpr = false;
            for (auto& t : toks) {
                const std::string n = t.name;
                if (t.kind == mk::Token::Start && n == "w:style") { curId = t.attr("w:styleId"); cur = &styles[curId]; continue; }
                if (t.kind == mk::Token::End && n == "w:style") { cur = nullptr; continue; }
                if (!cur) continue;
                if (n == "w:rpr") { inRpr = t.kind == mk::Token::Start && !t.selfClosing; continue; }
                if (t.kind != mk::Token::Start) continue;
                if (n == "w:name") cur->name = t.attr("w:val");
                else if (n == "w:basedon") cur->basedOn = t.attr("w:val");
                else if (n == "w:outlinelvl") cur->outline = std::atoi(t.attr("w:val").c_str());
                else if (n == "w:jc") cur->center = lowerAscii(t.attr("w:val")) == "center";
                else if (n == "w:numid") cur->numId = t.attr("w:val");
                else if (inRpr) {
                    if (n == "w:i") cur->fmt.italic = on(t);
                    else if (n == "w:b") cur->fmt.bold = on(t);
                    else if (n == "w:smallcaps") cur->fmt.smallcaps = on(t);
                    else if (n == "w:u") cur->fmt.underline = on(t);
                    else if (n == "w:strike" || n == "w:dstrike") cur->fmt.strike = on(t);
                }
            }
        }
    }
    // Resolved formatting of a style (following basedOn, at most a few steps).
    std::function<Fmt(const std::string&, int)> styleFmt = [&](const std::string& id, int depth) -> Fmt {
        auto it = styles.find(id);
        if (it == styles.end() || depth > 8) return {};
        Fmt f = it->second.basedOn.empty() ? Fmt{} : styleFmt(it->second.basedOn, depth + 1);
        f.over(it->second.fmt);
        return f;
    };
    auto headingLevel = [&](const std::string& id) -> int {
        for (std::string cur = id; !cur.empty();) {
            auto it = styles.find(cur);
            if (it == styles.end()) break;
            const std::string nm = lowerAscii(it->second.name);
            if (nm.rfind("heading ", 0) == 0) return std::max(1, std::atoi(nm.c_str() + 8));
            if (nm == "title") return 1;
            if (it->second.outline >= 0 && it->second.outline < 9) return it->second.outline + 1;
            cur = it->second.basedOn;
            if (cur == id) break;
        }
        return 0;
    };

    // Relationships: links and pictures.
    std::map<std::string, std::string> rels;   // id -> target
    std::map<std::string, bool> relExternal;
    {
        std::string xml;
        if (zip.read("word/_rels/document.xml.rels", xml, err))
            for (auto& t : mk::tokenize(mk::toUtf8(xml)))
                if (t.kind == mk::Token::Start && t.local() == "relationship") {
                    rels[t.attr("Id").empty() ? t.attr("id") : t.attr("Id")] = t.attr("Target").empty() ? t.attr("target") : t.attr("Target");
                    relExternal[t.attr("id")] = lowerAscii(t.attr("targetmode")) == "external";
                }
    }
    // Numbering: which lists are numbered (decimal, roman, letters) rather than bulleted.
    std::set<std::string> orderedNums;
    {
        std::string xml;
        if (zip.read("word/numbering.xml", xml, err)) {
            std::map<std::string, bool> abstractOrdered;
            std::string curAbs;
            bool firstLvl = false;
            auto toks = mk::tokenize(mk::toUtf8(xml));
            std::map<std::string, std::string> numToAbs;
            std::string curNum;
            for (auto& t : toks) {
                if (t.kind != mk::Token::Start) continue;
                const std::string n = t.name;
                if (n == "w:abstractnum") { curAbs = t.attr("w:abstractNumId"); curNum.clear(); }
                else if (n == "w:lvl") firstLvl = t.attr("w:ilvl") == "0";
                else if (n == "w:numfmt" && firstLvl && !curAbs.empty()) {
                    const std::string v = lowerAscii(t.attr("w:val"));
                    abstractOrdered[curAbs] = v != "bullet" && v != "none";
                } else if (n == "w:num") { curNum = t.attr("w:numId"); curAbs.clear(); }
                else if (n == "w:abstractnumid" && !curNum.empty()) numToAbs[curNum] = t.attr("w:val");
            }
            for (auto& [num, abs] : numToAbs) if (abstractOrdered[abs]) orderedNums.insert(num);
        }
    }
    // Footnotes.
    std::map<std::string, std::string> footnoteText;
    {
        std::string xml;
        if (zip.read("word/footnotes.xml", xml, err)) {
            std::string id, text;
            bool inT = false;
            for (auto& t : mk::tokenize(mk::toUtf8(xml))) {
                if (t.kind == mk::Token::Start && t.name == "w:footnote") { id = t.attr("w:id"); text.clear(); }
                else if (t.kind == mk::Token::End && t.name == "w:footnote") { if (!id.empty()) footnoteText[id] = mk::collapseSpace(text); id.clear(); }
                else if (t.kind == mk::Token::Start && t.name == "w:t") inT = !t.selfClosing;
                else if (t.kind == mk::Token::End && t.name == "w:t") inT = false;
                else if (t.kind == mk::Token::Text && inT) text += t.text;
                else if (t.kind == mk::Token::End && t.name == "w:p") text += " ";
            }
        }
    }
    // Metadata.
    {
        std::string xml;
        if (zip.read("docProps/core.xml", xml, err)) {
            auto toks = mk::tokenize(mk::toUtf8(xml));
            for (size_t i = 0; i + 1 < toks.size(); ++i) {
                if (toks[i].kind != mk::Token::Start || toks[i + 1].kind != mk::Token::Text) continue;
                const std::string v = mk::collapseSpace(toks[i + 1].text);
                if (toks[i].name == "dc:title" && plan.meta.title.empty()) plan.meta.title = v;
                else if (toks[i].name == "dc:creator" && plan.meta.author.empty()) plan.meta.author = v;
                else if (toks[i].name == "dc:description" && plan.meta.description.empty()) plan.meta.description = v;
                else if (toks[i].name == "dc:language" && plan.meta.language.empty()) plan.meta.language = v;
            }
        }
    }

    // The document body.
    std::vector<Para> paras;
    std::vector<std::string> notes;
    Para para;
    bool inPara = false, inPPr = false, inRun = false, inRPr = false, inText = false;
    Fmt runDirect;
    std::string runStyle, paraStyle;
    std::string pendingText;              // text of the current run
    std::string linkHref;
    int skip = 0;                         // inside deleted text, field codes or a fallback copy
    bool sawChanges = false, sawTable = false;
    std::string numId;
    auto flushRun = [&] {
        if (pendingText.empty()) return;
        Fmt f = styleFmt(paraStyle, 0);
        f.over(styleFmt(runStyle, 0));
        f.over(runDirect);
        if (para.heading) { f.bold = -1; }             // headings are bold by design; don't mark it
        std::string t = esc(pendingText);
        if (!linkHref.empty()) t = "<a href=\"" + esc(linkHref) + "\">" + t + "</a>";
        para.html += openTags(f) + t + closeTags(f);
        pendingText.clear();
    };
    std::vector<std::string> pictureAlts;
    const auto toks = mk::tokenize(mk::toUtf8(documentXml));
    for (size_t i = 0; i < toks.size(); ++i) {
        const mk::Token& t = toks[i];
        const std::string& n = t.name;
        const bool start = t.kind == mk::Token::Start, end = t.kind == mk::Token::End;
        // Things to leave out entirely: deleted text (tracked changes), field instructions,
        // the duplicate "fallback" copy of drawings, and comments.
        if (start && !t.selfClosing && (n == "w:del" || n == "w:movefrom" || n == "mc:fallback" || n == "w:instrtext" ||
                                        n == "w:deltext" || n == "w:commentreference")) {
            if (n == "w:del" || n == "w:movefrom") sawChanges = true;
            ++skip;
            continue;
        }
        if (end && (n == "w:del" || n == "w:movefrom" || n == "mc:fallback" || n == "w:instrtext" || n == "w:deltext" ||
                    n == "w:commentreference")) {
            if (skip > 0) --skip;
            continue;
        }
        if (skip) continue;
        if (start && n == "w:ins") sawChanges = true;
        if (start && n == "w:tbl" && !sawTable) { sawTable = true; plan.warnings.push_back("Tables were turned into plain paragraphs (one per row)."); }

        if (start && n == "w:p" && !t.selfClosing) {
            inPara = true;
            para = Para();
            paraStyle.clear();
            numId.clear();
        } else if ((end && n == "w:p") || (start && n == "w:p" && t.selfClosing)) {
            flushRun();
            if (inPara || t.selfClosing) {
                auto st = styles.find(paraStyle);
                para.styleName = st != styles.end() ? st->second.name : "";
                para.heading = headingLevel(paraStyle);
                if (st != styles.end() && st->second.center) para.center = true;
                const std::string ln = lowerAscii(para.styleName);
                para.quote = !para.heading && (ln.find("quote") != std::string::npos || ln == "block text");
                // A list item: numbering on the paragraph, or in its style ("List Bullet", "List Number").
                std::string num = numId;
                for (std::string c = paraStyle; num.empty() && !c.empty();) {
                    auto it2 = styles.find(c);
                    if (it2 == styles.end()) break;
                    num = it2->second.numId;
                    if (c == it2->second.basedOn) break;
                    c = it2->second.basedOn;
                }
                if (!num.empty() && num != "0") {
                    para.listItem = true;
                    para.ordered = orderedNums.count(num) > 0 || ln.find("list number") != std::string::npos;
                } else if (ln.rfind("list bullet", 0) == 0 || ln.rfind("list number", 0) == 0) {
                    para.listItem = true;
                    para.ordered = ln.rfind("list number", 0) == 0;
                }
                paras.push_back(para);
            }
            inPara = false;
        } else if (n == "w:ppr") {
            inPPr = start && !t.selfClosing;
        } else if (start && inPPr && !inRPr && n == "w:pstyle") {
            paraStyle = t.attr("w:val");
        } else if (start && inPPr && !inRPr && n == "w:jc") {
            para.center = lowerAscii(t.attr("w:val")) == "center";
        } else if (start && inPPr && n == "w:numid") {
            numId = t.attr("w:val");
        } else if (start && inPPr && n == "w:outlinelvl") {
            const int l = std::atoi(t.attr("w:val").c_str());
            if (l >= 0 && l < 6 && !para.heading) para.heading = 0;  // style decides; a bare outline level on Normal text is ignored
        } else if (start && n == "w:r" && !t.selfClosing) {
            flushRun();
            inRun = true;
            runDirect = Fmt();
            runStyle.clear();
        } else if (end && n == "w:r") {
            flushRun();
            inRun = false;
        } else if (n == "w:rpr") {
            inRPr = start && !t.selfClosing;
        } else if (start && inRun && inRPr) {
            if (n == "w:rstyle") runStyle = t.attr("w:val");
            else if (n == "w:i") runDirect.italic = on(t);
            else if (n == "w:b") runDirect.bold = on(t);
            else if (n == "w:u") runDirect.underline = on(t);
            else if (n == "w:smallcaps") runDirect.smallcaps = on(t);
            else if (n == "w:strike" || n == "w:dstrike") runDirect.strike = on(t);
            else if (n == "w:vertalign") {
                const std::string v = lowerAscii(t.attr("w:val"));
                runDirect.sup = v == "superscript";
                runDirect.sub = v == "subscript";
            }
        } else if (n == "w:t") {
            inText = start && !t.selfClosing;
        } else if (t.kind == mk::Token::Text && inText && inPara) {
            pendingText += t.text;
        } else if (start && inPara && (n == "w:tab" || n == "w:ptab")) {
            pendingText += ' ';
        } else if (start && inPara && n == "w:br") {
            flushRun();
            if (lowerAscii(t.attr("w:type")) != "page") para.html += "<br/>";
        } else if (start && inPara && n == "w:nobreakhyphen") {
            pendingText += '-';
        } else if (start && inPara && n == "w:sym") {
            // A symbol-font character: keep it as its code point.
            const unsigned cp = (unsigned)std::strtoul(t.attr("w:char").c_str(), nullptr, 16);
            if (cp >= 0x20 && cp < 0xF000) pendingText += mk::decodeEntities("&#" + std::to_string(cp) + ";");
        } else if (start && n == "w:hyperlink") {
            flushRun();
            auto it = rels.find(t.attr("r:id"));
            linkHref.clear();
            if (it != rels.end() && (it->second.rfind("https://", 0) == 0 || it->second.rfind("http://", 0) == 0 || it->second.rfind("mailto:", 0) == 0))
                linkHref = it->second;
        } else if (end && n == "w:hyperlink") {
            flushRun();
            linkHref.clear();
        } else if (start && inPara && n == "w:footnotereference") {
            flushRun();
            auto it = footnoteText.find(t.attr("w:id"));
            if (it != footnoteText.end()) {
                notes.push_back(esc(it->second));
                para.html += "<sup>" + std::to_string(notes.size()) + "</sup>";
            }
        } else if (start && n == "wp:docpr") {
            pictureAlts.push_back(t.attr("descr"));
        } else if (start && inPara && n == "a:blip") {
            flushRun();
            auto it = rels.find(t.attr("r:embed"));
            if (it != rels.end()) {
                const std::string path = mk::resolve("word/document.xml", it->second);
                para.html += "<img src=\"/" + esc(path) + "\" alt=\"" + esc(pictureAlts.empty() ? "" : pictureAlts.back()) + "\"/>";
            }
        } else if (end && (n == "w:tc")) {
            flushRun();
            para.html += " \xC2\xB7 ";   // separate table cells within a row's paragraph
        }
    }
    if (sawChanges) plan.warnings.push_back("The document has tracked changes; they were imported as if accepted.");

    // The first paragraph in the Title style names the book.
    for (auto& p : paras) {
        if (lowerAscii(p.styleName) == "title") {
            if (plan.meta.title.empty()) plan.meta.title = mk::collapseSpace(mk::textOf(mk::tokenize(p.html), 0, 1u << 30));
            p.heading = 0;
            p.styleName = "front-title";
            p.center = true;
            break;
        }
        if (!mk::collapseSpace(p.html).empty()) break;
    }

    mk::HtmlContext ctx;
    ctx.filePath = "";  // image paths are absolute within the archive
    std::map<std::string, std::string> seen;
    ctx.loadImage = [&](const std::string& path) -> std::string {
        auto it = seen.find(path);
        if (it != seen.end()) return it->second;
        std::string data, e, hash;
        if (zip.read(path, data, e) && !imageTypeOf(data).empty()) {
            hash = sha256Hex(data);
            plan.images[hash] = std::move(data);
        } else if (!e.empty() || !data.empty()) {
            plan.warnings.push_back("A picture in an unsupported format (such as EMF or WMF) was left out.");
        }
        seen[path] = hash;
        return hash;
    };
    // "Heading 1" is often used for chapters; if there's just one, it's probably the book title.
    return finish(plan, toHtml(paras, notes), ctx);
}

// -------------------------------------------------------------- OpenDocument

Plan readOdt(const std::string& bytes, const std::string& name, const Limits& lim) {
    Plan plan;
    plan.format = Format::Odt;
    plan.sourceName = name;
    Zip zip;
    std::string err, content;
    if (!zip.open(bytes, lim, err)) { plan.error = "This OpenDocument file is damaged."; return plan; }
    {
        std::string manifest;
        if (zip.read("META-INF/manifest.xml", manifest, err) && manifest.find("encryption-data") != std::string::npos) {
            plan.error = "This document is password-protected. Remove the password (File › Properties in LibreOffice) and import it again.";
            return plan;
        }
    }
    if (!zip.read("content.xml", content, err)) { plan.error = "This doesn't look like an OpenDocument text file."; return plan; }

    // Styles from styles.xml (named) and content.xml (automatic), with inheritance.
    struct Style { std::string display, parent; Fmt fmt; bool center = false; };
    std::map<std::string, Style> styles;
    auto readStyles = [&](const std::string& xml) {
        Style* cur = nullptr;
        for (auto& t : mk::tokenize(mk::toUtf8(xml))) {
            if (t.kind == mk::Token::Start && t.name == "style:style") {
                const std::string nm = t.attr("style:name");
                cur = &styles[nm];
                cur->display = t.attr("style:display-name").empty() ? nm : t.attr("style:display-name");
                cur->parent = t.attr("style:parent-style-name");
            } else if (t.kind == mk::Token::End && t.name == "style:style") {
                cur = nullptr;
            } else if (cur && t.kind == mk::Token::Start && t.name == "style:text-properties") {
                const std::string fs = lowerAscii(t.attr("fo:font-style")), fw = lowerAscii(t.attr("fo:font-weight"));
                if (!fs.empty()) cur->fmt.italic = fs == "italic" || fs == "oblique";
                if (!fw.empty()) cur->fmt.bold = fw == "bold" || std::atoi(fw.c_str()) >= 600;
                if (!t.attr("fo:font-variant").empty()) cur->fmt.smallcaps = lowerAscii(t.attr("fo:font-variant")) == "small-caps";
                if (!t.attr("style:text-underline-style").empty()) cur->fmt.underline = lowerAscii(t.attr("style:text-underline-style")) != "none";
                if (!t.attr("style:text-line-through-style").empty()) cur->fmt.strike = lowerAscii(t.attr("style:text-line-through-style")) != "none";
                const std::string pos = lowerAscii(t.attr("style:text-position"));
                if (!pos.empty()) { cur->fmt.sup = pos.rfind("super", 0) == 0; cur->fmt.sub = pos.rfind("sub", 0) == 0; }
            } else if (cur && t.kind == mk::Token::Start && t.name == "style:paragraph-properties") {
                if (!t.attr("fo:text-align").empty()) cur->center = lowerAscii(t.attr("fo:text-align")) == "center";
            }
        }
    };
    {
        std::string sx;
        if (zip.read("styles.xml", sx, err)) readStyles(sx);
        readStyles(content);
    }
    std::function<Fmt(const std::string&, int)> fmtOf = [&](const std::string& nm, int depth) -> Fmt {
        auto it = styles.find(nm);
        if (it == styles.end() || depth > 8) return {};
        Fmt f = it->second.parent.empty() ? Fmt{} : fmtOf(it->second.parent, depth + 1);
        f.over(it->second.fmt);
        return f;
    };
    std::function<std::string(const std::string&, int)> namedOf = [&](const std::string& nm, int depth) -> std::string {
        auto it = styles.find(nm);
        if (it == styles.end() || depth > 8) return nm;
        // Automatic styles ("P3") stand in for a named one: use the named parent's name.
        if (!it->second.parent.empty() && nm.size() <= 4 && std::isupper((unsigned char)nm[0]) && std::isdigit((unsigned char)nm.back()))
            return namedOf(it->second.parent, depth + 1);
        return it->second.display;
    };
    auto centered = [&](const std::string& nm) {
        for (std::string c = nm; !c.empty();) {
            auto it = styles.find(c);
            if (it == styles.end()) break;
            if (it->second.center) return true;
            if (c == it->second.parent) break;
            c = it->second.parent;
        }
        return false;
    };
    // Metadata.
    {
        std::string meta;
        if (zip.read("meta.xml", meta, err)) {
            auto toks = mk::tokenize(mk::toUtf8(meta));
            for (size_t i = 0; i + 1 < toks.size(); ++i) {
                if (toks[i].kind != mk::Token::Start || toks[i + 1].kind != mk::Token::Text) continue;
                const std::string v = mk::collapseSpace(toks[i + 1].text);
                if (toks[i].name == "dc:title" && plan.meta.title.empty()) plan.meta.title = v;
                else if ((toks[i].name == "meta:initial-creator" || toks[i].name == "dc:creator") && plan.meta.author.empty()) plan.meta.author = v;
                else if (toks[i].name == "dc:description" && plan.meta.description.empty()) plan.meta.description = v;
                else if (toks[i].name == "dc:language" && plan.meta.language.empty()) plan.meta.language = v;
            }
        }
    }

    std::vector<Para> paras;
    std::vector<std::string> notes;
    std::vector<Fmt> spanStack;           // formatting of open spans
    std::vector<std::string> listStyles;  // open lists
    std::string paraStyle;
    Para para;
    bool inPara = false, inBody = false;
    int skip = 0;
    std::string linkHref;
    bool sawTable = false;
    auto emitText = [&](const std::string& text) {
        if (text.empty()) return;
        Fmt f = fmtOf(paraStyle, 0);
        for (auto& s : spanStack) f.over(s);
        if (para.heading) f.bold = -1;
        std::string t = esc(text);
        if (!linkHref.empty()) t = "<a href=\"" + esc(linkHref) + "\">" + t + "</a>";
        para.html += openTags(f) + t + closeTags(f);
    };
    const auto toks = mk::tokenize(mk::toUtf8(content));
    for (size_t i = 0; i < toks.size(); ++i) {
        const mk::Token& t = toks[i];
        const std::string& n = t.name;
        const bool start = t.kind == mk::Token::Start, end = t.kind == mk::Token::End;
        if (start && n == "office:text") { inBody = true; continue; }
        if (!inBody) continue;
        // Left out: tracked-change records, comments, and the note bodies (collected separately).
        if (start && !t.selfClosing && (n == "text:tracked-changes" || n == "office:annotation" || n == "text:note-body" ||
                                        n == "text:note-citation" || n == "text:sequence-decls")) {
            if (n == "text:note-body") {
                // Collect the note's text and leave a number in the paragraph.
                std::string noteText;
                int d = 1;
                size_t j = i + 1;
                for (; j < toks.size() && d > 0; ++j) {
                    if (toks[j].kind == mk::Token::Start && toks[j].name == "text:note-body" && !toks[j].selfClosing) ++d;
                    if (toks[j].kind == mk::Token::End && toks[j].name == "text:note-body") --d;
                    if (toks[j].kind == mk::Token::Text) noteText += toks[j].text;
                    if (toks[j].kind == mk::Token::End && toks[j].name == "text:p") noteText += " ";
                }
                notes.push_back(esc(mk::collapseSpace(noteText)));
                para.html += "<sup>" + std::to_string(notes.size()) + "</sup>";
                i = j - 1;
                continue;
            }
            ++skip;
            continue;
        }
        if (end && (n == "text:tracked-changes" || n == "office:annotation" || n == "text:note-citation" || n == "text:sequence-decls")) {
            if (skip > 0) --skip;
            continue;
        }
        if (skip) continue;
        if (start && n == "table:table" && !sawTable) { sawTable = true; plan.warnings.push_back("Tables were turned into plain paragraphs (one per row)."); }
        if (start && n == "text:list") listStyles.push_back(t.attr("text:style-name"));
        else if (end && n == "text:list") { if (!listStyles.empty()) listStyles.pop_back(); }
        else if (start && (n == "text:p" || n == "text:h") && !t.selfClosing) {
            inPara = true;
            para = Para();
            paraStyle = t.attr("text:style-name");
            spanStack.clear();
            if (n == "text:h") para.heading = std::clamp(std::atoi(t.attr("text:outline-level").c_str()), 1, 6);
        } else if ((end && (n == "text:p" || n == "text:h")) || (start && (n == "text:p" || n == "text:h") && t.selfClosing)) {
            if (t.selfClosing) { para = Para(); paraStyle = t.attr("text:style-name"); }
            para.styleName = namedOf(paraStyle, 0);
            para.center = para.center || centered(paraStyle);
            const std::string ln = lowerAscii(para.styleName);
            if (!para.heading && (ln == "title")) { para.styleName = "front-title"; if (plan.meta.title.empty()) plan.meta.title = mk::collapseSpace(mk::textOf(mk::tokenize(para.html), 0, 1u << 30)); }
            if (!para.heading && ln.rfind("heading ", 0) == 0) para.heading = std::max(1, std::atoi(ln.c_str() + 8));
            para.quote = !para.heading && (ln.find("quot") != std::string::npos);
            if (!listStyles.empty()) {
                para.listItem = true;
                // Numbered lists use a numbering style ("Numbering 123", "Numbering ABC", ...).
                const std::string ls = lowerAscii(namedOf(listStyles.back(), 0));
                para.ordered = ls.find("number") != std::string::npos || ls.find("num") == 0;
            }
            paras.push_back(para);
            inPara = false;
        } else if (start && n == "text:span" && !t.selfClosing) {
            spanStack.push_back(fmtOf(t.attr("text:style-name"), 0));
        } else if (end && n == "text:span") {
            if (!spanStack.empty()) spanStack.pop_back();
        } else if (t.kind == mk::Token::Text && inPara) {
            // ODF collapses whitespace itself; text:s and text:tab carry real spaces.
            std::string text = t.text;
            for (char& c : text) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            emitText(text);
        } else if (start && inPara && n == "text:s") {
            const int c = std::clamp(std::atoi(t.attr("text:c").c_str()), 1, 20);
            emitText(std::string(c, ' '));
        } else if (start && inPara && n == "text:tab") {
            emitText(" ");
        } else if (start && inPara && n == "text:line-break") {
            para.html += "<br/>";
        } else if (start && n == "text:a") {
            const std::string h = t.attr("xlink:href");
            linkHref = (h.rfind("https://", 0) == 0 || h.rfind("http://", 0) == 0 || h.rfind("mailto:", 0) == 0) ? h : "";
        } else if (end && n == "text:a") {
            linkHref.clear();
        } else if (start && n == "draw:image") {
            const std::string href = t.attr("xlink:href");
            if (!href.empty() && href.find("://") == std::string::npos)
                para.html += "<img src=\"/" + esc(mk::resolve("content.xml", href)) + "\" alt=\"\"/>";
        } else if (end && n == "table:table-cell") {
            para.html += " \xC2\xB7 ";
        }
    }

    mk::HtmlContext ctx;
    std::map<std::string, std::string> seen;
    ctx.loadImage = [&](const std::string& path) -> std::string {
        auto it = seen.find(path);
        if (it != seen.end()) return it->second;
        std::string data, e, hash;
        if (zip.read(path, data, e) && !imageTypeOf(data).empty()) {
            hash = sha256Hex(data);
            plan.images[hash] = std::move(data);
        } else {
            plan.warnings.push_back("A picture in an unsupported format (such as SVG or WMF) was left out.");
        }
        seen[path] = hash;
        return hash;
    };
    return finish(plan, toHtml(paras, notes), ctx);
}

}  // namespace studio::import

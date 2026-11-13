// EPUB and HTML import, file detection, and applying a plan to a book.
#include "studio/import.hpp"

#include "internal.hpp"
#include "markup.hpp"
#include "importutil.hpp"
#include "miniz.h"
#include "studio/document.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>

namespace studio::import {

using nlohmann::json;
namespace mk = studio::markup;

// ------------------------------------------------------------------ labels

const char* label(Format f) {
    switch (f) {
        case Format::Epub: return "EPUB";
        case Format::Pdf: return "PDF";
        case Format::Html: return "HTML";
        case Format::Markdown: return "Markdown";
        case Format::Text: return "Plain text";
        case Format::Docx: return "Word document";
        case Format::Odt: return "OpenDocument text";
        default: return "Unknown";
    }
}

const char* label(Role r) {
    switch (r) {
        case Role::Chapter: return "Chapter";
        case Role::Part: return "Part";
        case Role::Cover: return "Cover";
        case Role::TitlePage: return "Title page";
        case Role::Copyright: return "Copyright page";
        case Role::Contents: return "Contents";
        case Role::Dedication: return "Dedication";
        case Role::Epigraph: return "Epigraph";
        case Role::Foreword: return "Foreword";
        case Role::Preface: return "Preface";
        case Role::Prologue: return "Prologue";
        case Role::Epilogue: return "Epilogue";
        case Role::Afterword: return "Afterword";
        case Role::Acknowledgements: return "Acknowledgements";
        case Role::AboutAuthor: return "About the author";
        case Role::AlsoBy: return "Also by";
        case Role::Appendix: return "Appendix";
        case Role::Notes: return "Notes";
        default: return "Other";
    }
}

bool includedByDefault(Role r) {
    switch (r) {
        case Role::Cover: case Role::TitlePage: case Role::Copyright: case Role::Contents: case Role::AlsoBy: return false;
        default: return true;
    }
}

std::int64_t Plan::includedWords() const {
    std::int64_t n = 0;
    for (auto& s : sections)
        if (s.include && !(s.role == Role::Part && s.words <= kPartPageWords)) n += s.words;  // a bare part page brings no text
    return n;
}
int Plan::includedCount() const {
    int n = 0;
    for (auto& s : sections) if (s.include) ++n;
    return n;
}

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string baseName(const std::string& path) {
    const size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

// Letters and digits only, lower-case: for comparing titles.
std::string squash(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c) || c >= 0x80) o += (char)std::tolower(c);
    return o;
}

}  // namespace

// ------------------------------------------------------------------ roles

Role detail::roleFrom(const std::string& epubType, const std::string& title, const std::string& file, const std::string& bodyClass) {
    const std::string t = " " + lower(epubType) + " ";
    auto has = [&](const char* w) { return t.find(std::string(" ") + w + " ") != std::string::npos || t.find(std::string(":") + w + " ") != std::string::npos; };
    if (has("cover")) return Role::Cover;
    if (has("titlepage") || has("halftitlepage") || has("fulltitle")) return Role::TitlePage;
    if (has("copyright-page") || has("imprint") || has("colophon")) return Role::Copyright;
    if (has("toc")) return Role::Contents;
    if (has("dedication")) return Role::Dedication;
    if (has("epigraph")) return Role::Epigraph;
    if (has("foreword")) return Role::Foreword;
    if (has("preface") || has("introduction")) return Role::Preface;
    if (has("prologue")) return Role::Prologue;
    if (has("epilogue")) return Role::Epilogue;
    if (has("afterword") || has("conclusion")) return Role::Afterword;
    if (has("acknowledgments") || has("acknowledgements")) return Role::Acknowledgements;
    if (has("appendix")) return Role::Appendix;
    if (has("endnotes") || has("footnotes") || has("rearnotes")) return Role::Notes;
    if (has("part") || has("volume")) return Role::Part;
    const std::string s = lower(title) + " " + lower(baseName(file)) + " " + lower(bodyClass);
    auto in = [&](const char* w) { return s.find(w) != std::string::npos; };
    const std::string tl = lower(title);
    if (tl == "cover" || in("cover.") || in("cover_") || in("coverpage") || (bodyClass.find("cover") != std::string::npos)) return Role::Cover;
    if (tl == "title page" || in("titlepage") || in("title_page") || in("title-page") || tl == "title") return Role::TitlePage;
    if (in("copyright") || in("legal") || in("imprint")) return Role::Copyright;
    if (tl == "contents" || tl == "table of contents" || in("toc.") || in("nav.") || in("contents.")) return Role::Contents;
    if (in("dedication")) return Role::Dedication;
    if (in("epigraph")) return Role::Epigraph;
    if (in("foreword")) return Role::Foreword;
    if (in("preface") || tl == "introduction") return Role::Preface;
    if (in("prologue")) return Role::Prologue;
    if (in("epilogue")) return Role::Epilogue;
    if (in("afterword")) return Role::Afterword;
    if (in("acknowledg")) return Role::Acknowledgements;
    if (in("about the author") || in("about-the-author") || in("abouttheauthor") || in("about_the_author")) return Role::AboutAuthor;
    if (in("also by") || in("also-by") || in("alsoby") || in("other books")) return Role::AlsoBy;
    if (in("appendix")) return Role::Appendix;
    if (tl == "notes" || tl == "endnotes") return Role::Notes;
    return Role::Chapter;
}

// Removes heading blocks at the very start that only repeat the title
// (the Studio prints chapter titles from the chapter itself).
std::string detail::stripRepeatedTitle(const std::string& docJson, const std::string& title) {
    json d = json::parse(docJson, nullptr, false);
    if (!d.is_object() || !d.contains("content")) return docJson;
    const std::string st = squash(title);
    if (st.empty()) return docJson;
    int removed = 0;
    while (removed < 2 && !d["content"].empty() && d["content"][0].value("type", "") == "heading") {
        std::string ht;
        for (auto& c : d["content"][0].value("content", json::array())) ht += c.value("text", "");
        const std::string sh = squash(ht);
        if (sh.empty() || (st.find(sh) == std::string::npos && sh.find(st) == std::string::npos)) break;
        d["content"].erase(0);
        ++removed;
    }
    if (d["content"].empty()) return doc::emptyDoc();
    return d.dump();
}

namespace {
using detail::roleFrom;
using detail::Zip;
using detail::stripRepeatedTitle;

std::string appendDocs(const std::string& a, const std::string& b) {
    json da = json::parse(a, nullptr, false), db = json::parse(b, nullptr, false);
    if (!da.is_object()) return b;
    if (!db.is_object()) return a;
    for (auto& x : db["content"]) da["content"].push_back(x);
    // Drop a lone empty paragraph left from an empty first half.
    auto& c = da["content"];
    if (c.size() > 1 && c[0].value("type", "") == "paragraph" && !c[0].contains("content")) c.erase(0);
    return da.dump();
}


// -------------------------------------------------------------- EPUB parts

struct TocEntry {
    std::string label, file, fragment;
    int depth = 0;
    bool hasChildren = false;
};

void parseNavToc(const std::string& xhtml, const std::string& navPath, std::vector<TocEntry>& out) {
    auto toks = mk::tokenize(mk::toUtf8(xhtml));
    // Find <nav epub:type="toc"> (or the first nav).
    size_t start = toks.size(), firstNav = toks.size();
    for (size_t i = 0; i < toks.size(); ++i)
        if (toks[i].kind == mk::Token::Start && toks[i].local() == "nav") {
            if (firstNav == toks.size()) firstNav = i;
            if (lower(toks[i].attr("type")).find("toc") != std::string::npos) { start = i; break; }
        }
    if (start == toks.size()) start = firstNav;
    if (start == toks.size()) return;
    int olDepth = 0, navDepth = 0;
    for (size_t i = start; i < toks.size(); ++i) {
        const auto& t = toks[i];
        const std::string n = t.local();
        if (t.kind == mk::Token::Start && n == "nav") ++navDepth;
        if (t.kind == mk::Token::End && n == "nav" && --navDepth == 0) break;
        if (t.kind == mk::Token::Start && n == "ol") ++olDepth;
        if (t.kind == mk::Token::End && n == "ol") --olDepth;
        if (t.kind == mk::Token::Start && n == "li" && !out.empty() && olDepth > out.back().depth + 1) {}
        if (t.kind == mk::Token::Start && (n == "a" || n == "span")) {
            size_t e = i + 1;
            int d = 1;
            while (e < toks.size() && d > 0) {
                if (toks[e].kind == mk::Token::Start && toks[e].local() == n && !toks[e].selfClosing) ++d;
                if (toks[e].kind == mk::Token::End && toks[e].local() == n) --d;
                ++e;
            }
            TocEntry te;
            te.label = mk::textOf(toks, i + 1, e - 1);
            if (n == "a") te.file = mk::resolve(navPath, t.attr("href"), &te.fragment);
            te.depth = std::max(0, olDepth - 1);
            if (!out.empty() && te.depth > out.back().depth) out.back().hasChildren = true;
            if (!te.label.empty()) out.push_back(te);
            i = e - 1;
        }
    }
}

void parseNcx(const std::string& xml, const std::string& ncxPath, std::vector<TocEntry>& out) {
    auto toks = mk::tokenize(mk::toUtf8(xml));
    int depth = 0;
    std::vector<size_t> open;  // index in out for each open navPoint
    for (size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];
        const std::string n = t.local();
        if (t.kind == mk::Token::Start && n == "navpoint") {
            TocEntry te;
            te.depth = depth++;
            if (!open.empty()) out[open.back()].hasChildren = true;
            out.push_back(te);
            open.push_back(out.size() - 1);
        } else if (t.kind == mk::Token::End && n == "navpoint") {
            --depth;
            if (!open.empty()) open.pop_back();
        } else if (t.kind == mk::Token::Start && n == "text" && !open.empty() && out[open.back()].label.empty()) {
            size_t e = i + 1;
            while (e < toks.size() && !(toks[e].kind == mk::Token::End && toks[e].local() == "text")) ++e;
            out[open.back()].label = mk::textOf(toks, i + 1, e);
        } else if (t.kind == mk::Token::Start && n == "content" && !open.empty() && out[open.back()].file.empty()) {
            out[open.back()].file = mk::resolve(ncxPath, t.attr("src"), &out[open.back()].fragment);
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const TocEntry& e) { return e.file.empty(); }), out.end());
}

std::string imageTypeOf(const std::string& bytes) {
    if (bytes.size() > 3 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xD8) return "image/jpeg";
    if (bytes.size() > 8 && bytes.compare(1, 3, "PNG") == 0) return "image/png";
    if (bytes.size() > 6 && bytes.compare(0, 3, "GIF") == 0) return "image/gif";
    if (bytes.size() > 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) return "image/webp";
    return {};
}

}  // namespace

// ------------------------------------------------------------------- EPUB

Plan readEpub(const std::string& bytes, const std::string& name, const Limits& lim) {
    Plan plan;
    plan.format = Format::Epub;
    plan.sourceName = name;
    Zip zip;
    std::string err;
    if (!zip.open(bytes, lim, err)) { plan.error = err; return plan; }

    // DRM: refuse rather than produce garbage, and say why.
    if (zip.has("META-INF/rights.xml")) {
        plan.error = "This EPUB is protected by Adobe DRM (ADEPT), so its text can't be read. If it's your own book, import the "
                     "original, unprotected file you uploaded to the store.";
        return plan;
    }
    if (zip.has("META-INF/sinf.xml")) {
        plan.error = "This EPUB is protected by Apple's DRM (FairPlay), so its text can't be read. Import your own unprotected copy instead.";
        return plan;
    }
    if (zip.has("META-INF/license.lcpl")) {
        plan.error = "This EPUB is protected by Readium LCP, so its text can't be read. Import your own unprotected copy instead.";
        return plan;
    }
    if (zip.has("META-INF/encryption.xml")) {
        std::string enc;
        if (zip.read("META-INF/encryption.xml", enc, err)) {
            // Font obfuscation is allowed (it isn't DRM); anything else is.
            for (auto& t : mk::tokenize(enc))
                if (t.kind == mk::Token::Start && t.local() == "encryptionmethod") {
                    const std::string alg = t.attr("algorithm");
                    if (alg != "http://www.idpf.org/2008/embedding" && alg != "http://ns.adobe.com/pdf/enc#RC") {
                        plan.error = "This EPUB's text is encrypted (DRM), so it can't be imported. Import your own unprotected copy instead.";
                        return plan;
                    }
                }
        }
    }

    // container.xml -> the package document.
    std::string container, opfPath;
    if (!zip.read("META-INF/container.xml", container, err)) {
        plan.error = "This doesn't look like an EPUB: it has no META-INF/container.xml.";
        return plan;
    }
    for (auto& t : mk::tokenize(mk::toUtf8(container)))
        if (t.kind == mk::Token::Start && t.local() == "rootfile" && opfPath.empty()) opfPath = mk::percentDecode(t.attr("full-path"));
    std::string opf;
    if (opfPath.empty() || !zip.read(opfPath, opf, err)) { plan.error = "The EPUB's package file couldn't be read."; return plan; }

    struct Item { std::string id, href, type, props; };
    std::map<std::string, Item> manifest;
    std::vector<std::pair<std::string, bool>> spine;  // id, linear
    std::string ncxId, coverId;
    std::map<std::string, std::string> guide;          // file -> type
    std::map<std::string, std::string> refines;        // "#id" -> title-type
    struct Meta { std::string name, id, text, role; };
    std::vector<Meta> metas;
    {
        auto toks = mk::tokenize(mk::toUtf8(opf));
        bool inMeta = false;
        for (size_t i = 0; i < toks.size(); ++i) {
            const auto& t = toks[i];
            const std::string n = t.local();
            if (t.kind == mk::Token::Start && n == "metadata") inMeta = true;
            if (t.kind == mk::Token::End && n == "metadata") inMeta = false;
            if (t.kind != mk::Token::Start) continue;
            if (n == "item") {
                Item it{t.attr("id"), mk::resolve(opfPath, t.attr("href")), lower(t.attr("media-type")), lower(t.attr("properties"))};
                manifest[it.id] = it;
            } else if (n == "spine") {
                ncxId = t.attr("toc");
            } else if (n == "itemref") {
                spine.push_back({t.attr("idref"), lower(t.attr("linear")) != "no"});
            } else if (n == "reference") {
                guide[mk::resolve(opfPath, t.attr("href"))] = lower(t.attr("type"));
            } else if (inMeta && n == "meta") {
                if (lower(t.attr("name")) == "cover") coverId = t.attr("content");
                if (lower(t.attr("property")) == "title-type") {
                    size_t e = i + 1;
                    while (e < toks.size() && toks[e].kind != mk::Token::End) ++e;
                    refines[t.attr("refines")] = lower(mk::textOf(toks, i + 1, e));
                }
            } else if (inMeta && t.name.rfind("dc:", 0) == 0 && !t.selfClosing) {
                size_t e = i + 1;
                while (e < toks.size() && !(toks[e].kind == mk::Token::End && toks[e].name == t.name)) ++e;
                metas.push_back({n, t.attr("id"), mk::textOf(toks, i + 1, e), lower(t.attr("role"))});
            }
        }
    }
    // Metadata.
    for (auto& m : metas) {
        if (m.name == "title") {
            const std::string type = m.id.empty() ? "" : refines["#" + m.id];
            if (type == "subtitle") { if (plan.meta.subtitle.empty()) plan.meta.subtitle = m.text; }
            else if (plan.meta.title.empty() || type == "main") plan.meta.title = m.text;
        } else if (m.name == "creator" && plan.meta.author.empty() && (m.role.empty() || m.role == "aut")) plan.meta.author = m.text;
        else if (m.name == "language" && plan.meta.language.empty()) plan.meta.language = m.text;
        else if (m.name == "description" && plan.meta.description.empty()) {
            std::vector<std::string> w;
            plan.meta.description = doc::plainText(mk::convertHtml("<p>" + m.text + "</p>", {}, w)[0].json);
        } else if (m.name == "publisher" && plan.meta.publisher.empty()) plan.meta.publisher = m.text;
        else if (m.name == "identifier" && plan.meta.isbn.empty()) {
            std::string d;
            for (char c : m.text) if (std::isdigit((unsigned char)c) || c == 'X') d += c;
            if ((d.size() == 13 && (d.rfind("978", 0) == 0 || d.rfind("979", 0) == 0)) || (d.size() == 10 && lower(m.text).find("isbn") != std::string::npos))
                plan.meta.isbn = d;
        }
    }
    if (plan.meta.title.empty()) plan.meta.title = name;

    // Images are stored by content; each is read at most once.
    std::map<std::string, std::string> imageHashByPath;
    bool warnedSvg = false;
    auto loadImage = [&](const std::string& path) -> std::string {
        auto it = imageHashByPath.find(path);
        if (it != imageHashByPath.end()) return it->second;
        std::string data, e;
        std::string hash;
        if (zip.read(path, data, e)) {
            if (!imageTypeOf(data).empty()) {
                hash = sha256Hex(data);
                plan.images[hash] = std::move(data);
            } else if (lower(path).size() > 4 && lower(path).substr(lower(path).size() - 4) == ".svg" && !warnedSvg) {
                plan.warnings.push_back("SVG drawings aren't supported yet and were left out.");
                warnedSvg = true;
            }
        }
        imageHashByPath[path] = hash;
        return hash;
    };

    // Cover image.
    {
        std::string coverPath;
        for (auto& [id, it] : manifest) if (it.props.find("cover-image") != std::string::npos) coverPath = it.href;
        if (coverPath.empty() && !coverId.empty() && manifest.count(coverId)) coverPath = manifest[coverId].href;
        if (!coverPath.empty()) {
            std::string data, e;
            if (zip.read(coverPath, data, e) && !imageTypeOf(data).empty()) {
                plan.meta.coverType = imageTypeOf(data);
                plan.meta.coverBytes = std::move(data);
            }
        }
    }

    // Stylesheets: class -> italic/bold/small caps/centre, so styling done
    // through CSS isn't lost.
    std::map<std::string, mk::CssFlags> classes;
    for (auto& [id, it] : manifest)
        if (it.type == "text/css") {
            std::string css, e;
            if (zip.read(it.href, css, e))
                for (auto& [k, f] : mk::parseCss(mk::toUtf8(css))) classes[k] = f;
        }

    // Table of contents.
    std::vector<TocEntry> toc;
    std::string navPath;
    for (auto& [id, it] : manifest)
        if ((" " + it.props + " ").find(" nav ") != std::string::npos) navPath = it.href;
    if (!navPath.empty()) {
        std::string nav, e;
        if (zip.read(navPath, nav, e)) parseNavToc(nav, navPath, toc);
    }
    if (toc.empty() && !ncxId.empty() && manifest.count(ncxId)) {
        std::string ncx, e;
        if (zip.read(manifest[ncxId].href, ncx, e)) parseNcx(ncx, manifest[ncxId].href, toc);
    }
    if (toc.empty()) {
        for (auto& [id, it] : manifest)
            if (it.type == "application/x-dtbncx+xml") {
                std::string ncx, e;
                if (zip.read(it.href, ncx, e)) parseNcx(ncx, it.href, toc);
                break;
            }
    }
    // Only the top two levels become sections; deeper entries stay inside their chapter.
    std::map<std::string, std::vector<const TocEntry*>> tocByFile;
    for (auto& e : toc) if (e.depth <= 1) tocByFile[e.file].push_back(&e);
    // A two-level TOC only means "parts" if some top-level entry has children.
    bool hasParts = false;
    for (auto& e : toc) if (e.depth == 0 && e.hasChildren) hasParts = true;
    if (!hasParts) {
        tocByFile.clear();
        for (auto& e : toc) if (e.depth == 0) tocByFile[e.file].push_back(&e);
    }

    // Walk the spine.
    std::set<const TocEntry*> used;
    int untitled = 0;
    bool anyEncryptedText = false;
    for (auto& [idref, linear] : spine) {
        auto mit = manifest.find(idref);
        if (mit == manifest.end()) continue;
        const Item& item = mit->second;
        if (item.type != "application/xhtml+xml" && item.type != "text/html" && item.type != "application/x-dtbook+xml") continue;
        std::string html, e;
        if (!zip.read(item.href, html, e)) {
            if (zip.encryptedSeen()) anyEncryptedText = true;
            else plan.warnings.push_back("Couldn't read " + item.href + " (" + e + "); it was left out.");
            continue;
        }
        mk::HtmlContext ctx;
        ctx.filePath = item.href;
        ctx.classes = classes;
        ctx.loadImage = loadImage;
        const auto& entries = tocByFile[item.href];
        for (auto* te : entries) if (!te->fragment.empty()) ctx.splitIds.insert(te->fragment);
        auto parts = mk::convertHtml(html, ctx, plan.warnings);
        // One huge file with many chapter headings and no TOC to split it: split at the headings.
        if (toc.empty() && parts.size() == 1 && parts[0].words > 8000) {
            int h1 = 0, h2 = 0;
            for (auto& t : mk::tokenize(mk::toUtf8(html)))
                if (t.kind == mk::Token::Start) { h1 += t.local() == "h1"; h2 += t.local() == "h2"; }
            const int level = h1 >= 3 ? 1 : h2 >= 3 ? 2 : 0;
            if (level) {
                ctx.splitHeadingLevel = level;
                std::vector<std::string> w2;
                parts = mk::convertHtml(html, ctx, w2);
            }
        }
        const std::string guideType = guide.count(item.href) ? guide[item.href] : "";
        const bool isNav = item.href == navPath;
        for (size_t pi = 0; pi < parts.size(); ++pi) {
            const auto& part = parts[pi];
            // Which TOC entry starts here?
            const TocEntry* te = nullptr;
            for (auto* c : entries)
                if (!used.count(c) && (c->fragment == part.anchorId || (pi == 0 && part.anchorId.empty() && c->fragment.empty()))) { te = c; break; }
            if (!te && pi == 0 && !entries.empty() && !used.count(entries[0]) && part.anchorId.empty() && !part.empty) {
                // Content before the first fragment entry, when that entry is also the first thing: treat as its start.
                if (entries[0]->fragment.empty()) te = entries[0];
            }
            if (te) used.insert(te);
            const std::string epubType = !part.epubType.empty() ? part.epubType : guideType;
            if (!te && !toc.empty() && !plan.sections.empty() && !isNav) {
                // Not in the contents: a continuation of the previous section (e.g. a chapter split across files),
                // unless it's front or back matter of its own.
                const Role r = roleFrom(epubType, part.headings.empty() ? "" : part.headings[0], item.href, part.bodyClass);
                if (r == Role::Chapter && plan.sections.back().role != Role::Cover && plan.sections.back().role != Role::Contents &&
                    plan.sections.back().role != Role::Part) {
                    if (!part.empty) {
                        plan.sections.back().content = appendDocs(plan.sections.back().content, part.json);
                        plan.sections.back().source += ", " + item.href;
                    }
                    continue;
                }
            }
            if (part.empty && !te) continue;
            Section s;
            s.title = te ? te->label : (!part.headings.empty() ? part.headings[0] : "");
            s.role = isNav ? Role::Contents : roleFrom(epubType, s.title, item.href, part.bodyClass);
            if (te && hasParts) {
                s.level = te->depth;
                if (te->depth == 0 && te->hasChildren) s.role = Role::Part;
            }
            if (!linear && s.role == Role::Chapter) s.role = Role::Other;
            if (s.title.empty()) {
                if (s.role != Role::Chapter) s.title = label(s.role);
                else s.title = "Untitled section " + std::to_string(++untitled);
            }
            s.content = s.role == Role::Part ? part.json : stripRepeatedTitle(part.json, s.title);
            s.source = item.href + (part.anchorId.empty() ? "" : "#" + part.anchorId);
            plan.sections.push_back(std::move(s));
        }
    }
    if (anyEncryptedText) {
        plan.error = "Some of this EPUB's chapters are encrypted, so it can't be imported. Import an unprotected copy instead.";
        return plan;
    }
    // Finish: word counts, default inclusion; a part keeps its own page only if it has real text.
    for (auto& s : plan.sections) {
        s.words = doc::wordCount(s.content);
        s.include = includedByDefault(s.role);
        if (s.role == Role::Cover) s.include = false;
    }
    // A cover page that's only the cover image: drop the section entirely (the image becomes the book cover).
    plan.sections.erase(std::remove_if(plan.sections.begin(), plan.sections.end(),
                                       [](const Section& s) { return s.role == Role::Cover && s.words == 0; }),
                        plan.sections.end());
    if (plan.sections.empty()) { plan.error = "No readable text was found in this EPUB."; return plan; }
    std::int64_t total = 0;
    for (auto& s : plan.sections) total += s.words;
    if (total == 0) {
        plan.error = "This EPUB has no text to import (it may be made only of images, such as a comic or a scanned book).";
        return plan;
    }
    plan.ok = true;
    return plan;
}

// ------------------------------------------------------------------- HTML

// Splits a converted document at paragraphs that read like chapter headings
// ("Chapter 3", "CHAPTER ONE" + a title line, "Part Two", "Prologue"),
// keeping every block (quotes, lists, pictures) intact.
static void splitDocAtHeadings(Plan& plan, const std::string& docJson, const std::string& fallbackTitle, const std::string& source) {
    json d = json::parse(docJson, nullptr, false);
    json blocks = d.is_object() && d.contains("content") && d["content"].is_array() ? d["content"] : json::array();
    auto textOf = [](const json& b) {
        std::string t;
        if (b.contains("content") && b["content"].is_array())
            for (auto& c : b["content"]) if (c.is_object() && c.value("type", "") == "text") t += c.value("text", "");
        return mk::collapseSpace(t);
    };
    struct Cut { size_t at; size_t skip; std::string title; int kind; };
    std::vector<Cut> cuts;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const std::string type = blocks[i].value("type", "");
        if (type != "paragraph" && type != "heading") continue;
        const std::string t = textOf(blocks[i]);
        const int k = detail::headingKind(t);
        if (!k) continue;
        Cut c{i, 1, t, k};
        // "CHAPTER ONE" followed by a short title line.
        if (k == 1 && i + 1 < blocks.size()) {
            const std::string n = textOf(blocks[i + 1]);
            if (!n.empty() && n.size() <= 60 && n.back() != '.' && n.back() != ',' && std::isupper((unsigned char)n[0]) &&
                !detail::headingKind(n) && blocks[i + 1].value("type", "") != "scene_break")
                { c.title += ": " + n; c.skip = 2; }
        }
        cuts.push_back(c);
    }
    auto emit = [&](const std::string& title, Role role, int level, size_t from, size_t to) {
        json part = {{"type", "doc"}, {"content", json::array()}};
        for (size_t i = from; i < to; ++i) part["content"].push_back(blocks[i]);
        Section s;
        s.title = title;
        s.role = role;
        s.level = level;
        s.content = part["content"].empty() ? doc::emptyDoc() : part.dump();
        s.words = doc::wordCount(s.content);
        s.include = includedByDefault(role);
        s.source = source;
        plan.sections.push_back(std::move(s));
    };
    if (cuts.empty()) {
        emit(fallbackTitle, Role::Chapter, 0, 0, blocks.size());
        plan.warnings.push_back("No chapter headings were found, so everything came in as one chapter. You can split it in the editor.");
        return;
    }
    if (cuts[0].at > 0) {
        json front = {{"type", "doc"}, {"content", json::array()}};
        for (size_t i = 0; i < cuts[0].at; ++i) front["content"].push_back(blocks[i]);
        const auto words = doc::wordCount(front.dump());
        if (words > 0) emit(words > 150 ? "Opening" : "Front matter", words > 150 ? Role::Chapter : Role::TitlePage, 0, 0, cuts[0].at);
    }
    bool inPart = false;
    for (size_t k = 0; k < cuts.size(); ++k) {
        const size_t to = k + 1 < cuts.size() ? cuts[k + 1].at : blocks.size();
        const Role role = cuts[k].kind == 2 ? Role::Part : roleFrom("", cuts[k].title, "", "");
        if (role == Role::Part) inPart = true;
        emit(cuts[k].title, role, role != Role::Part && inPart ? 1 : 0, cuts[k].at + cuts[k].skip, to);
    }
}

void detail::sectionsFromHtml(Plan& plan, const std::string& html, const mk::HtmlContext& base, const std::string& fallbackTitle) {
    auto toks = mk::tokenize(html);
    int h1 = 0, h2 = 0;
    for (auto& t : toks)
        if (t.kind == mk::Token::Start) { h1 += t.local() == "h1"; h2 += t.local() == "h2"; }
    mk::HtmlContext ctx = base;
    ctx.splitHeadingLevel = h1 >= 2 ? 1 : h2 >= 2 ? 2 : 0;
    auto parts = mk::convertHtml(html, ctx, plan.warnings);
    if (ctx.splitHeadingLevel == 0) {
        // No heading structure: look for "Chapter 1"-style paragraphs instead.
        splitDocAtHeadings(plan, parts.empty() ? doc::emptyDoc() : parts[0].json, fallbackTitle, plan.sourceName);
        return;
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        auto& p = parts[i];
        if (p.empty) continue;
        Section s;
        if (i == 0 && (p.headings.empty() || ctx.splitHeadingLevel == 0)) {
            // Text before the first chapter heading.
            s.title = p.words > 150 ? "Opening" : "Front matter";
            s.role = p.words > 150 ? Role::Chapter : Role::TitlePage;
        } else {
            s.title = !p.headings.empty() ? p.headings[0] : "Section " + std::to_string(i + 1);
            s.role = roleFrom(p.epubType, s.title, "", p.bodyClass);
            if (detail::headingKind(s.title) == 2) s.role = Role::Part;
        }
        s.content = stripRepeatedTitle(p.json, s.title);
        s.words = doc::wordCount(s.content);
        s.include = includedByDefault(s.role);
        s.source = plan.sourceName;
        plan.sections.push_back(std::move(s));
    }
    // Chapters after a part sit inside it.
    bool inPart = false;
    for (auto& s : plan.sections) {
        if (s.role == Role::Part) { inPart = true; continue; }
        // Back matter belongs to the book, not the last part.
        const bool back = s.role == Role::Notes || s.role == Role::Acknowledgements || s.role == Role::AboutAuthor ||
                          s.role == Role::AlsoBy || s.role == Role::Appendix || s.role == Role::Afterword;
        if (back) inPart = false;
        if (inPart) s.level = 1;
    }
}

Plan readHtml(const std::string& htmlIn, const std::string& name) {
    Plan plan;
    plan.format = Format::Html;
    plan.sourceName = name;
    const std::string html = mk::toUtf8(htmlIn);
    mk::HtmlContext ctx;
    auto toks = mk::tokenize(html);
    for (size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];
        if (t.kind != mk::Token::Start) continue;
        if (t.local() == "style" && i + 1 < toks.size() && toks[i + 1].kind == mk::Token::Text)
            for (auto& [k, f] : mk::parseCss(toks[i + 1].text)) ctx.classes[k] = f;
        if (t.local() == "title" && plan.meta.title.empty()) plan.meta.title = mk::textOf(toks, i + 1, i + 2);
    }
    // Images in a loose HTML file can't be reached safely; they're left out.
    std::string stem = name.substr(0, name.rfind('.'));
    detail::sectionsFromHtml(plan, html, ctx, plan.meta.title.empty() ? stem : plan.meta.title);
    if (html.find("<img") != std::string::npos) plan.warnings.push_back("Images in a standalone HTML file aren't imported; add them in the Studio.");
    std::int64_t total = 0;
    for (auto& s : plan.sections) total += s.words;
    if (plan.sections.empty() || total == 0) { plan.error = "No readable text was found in this file."; return plan; }
    if (plan.meta.title.empty()) plan.meta.title = stem;
    plan.ok = true;
    return plan;
}

// ----------------------------------------------------------------- detect

Format detect(const std::string& bytes, const std::string& fileName) {
    const std::string ext = [&] {
        const size_t d = fileName.rfind('.');
        return d == std::string::npos ? std::string{} : lower(fileName.substr(d + 1));
    }();
    if (bytes.size() >= 4 && bytes.compare(0, 4, "PK\x03\x04") == 0) {
        // ZIP-based: EPUB, Word or OpenDocument. The first entry of an EPUB or ODT is its "mimetype".
        if (bytes.find("word/document.xml") != std::string::npos || ext == "docx") return Format::Docx;
        if (bytes.find("application/vnd.oasis.opendocument.text") != std::string::npos && bytes.find("application/vnd.oasis.opendocument.text") < 200) return Format::Odt;
        if (ext == "odt") return Format::Odt;
        return Format::Epub;  // checked properly when read
    }
    if (bytes.size() >= 5 && bytes.compare(0, 5, "%PDF-") == 0) return Format::Pdf;
    if (ext == "htm" || ext == "html" || ext == "xhtml") return Format::Html;
    if (ext == "md" || ext == "markdown") return Format::Markdown;
    if (ext == "txt" || ext == "text") return Format::Text;
    const std::string head = lower(bytes.substr(0, 512));
    if (head.find("<html") != std::string::npos || head.find("<!doctype html") != std::string::npos) return Format::Html;
    return Format::Unknown;
}

Plan readFile(const std::string& path, const Limits& lim) {
    Plan plan;
    plan.sourceName = baseName(path);
    std::error_code ec;
    const auto p = studio::detail::u8path(path);
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) { plan.error = "The file couldn't be opened."; return plan; }
    if (size > lim.maxFileBytes) { plan.error = "The file is too large to import (over 512 MB)."; return plan; }
    std::string bytes;
    {
        std::ifstream f(p, std::ios::binary);
        if (!f) { plan.error = "The file couldn't be opened."; return plan; }
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    // Kindle and Word files: say what to do instead.
    if (bytes.size() > 68 && (bytes.compare(60, 8, "BOOKMOBI") == 0 || bytes.compare(60, 8, "TEXtREAd") == 0)) {
        plan.error = "This is a Kindle (MOBI/AZW) file, which the Studio can't read. Import the EPUB or Word original instead.";
        return plan;
    }
    const std::string ext = lower(plan.sourceName.substr(plan.sourceName.rfind('.') == std::string::npos ? plan.sourceName.size() : plan.sourceName.rfind('.')));
    if (ext == ".azw" || ext == ".azw3" || ext == ".kfx" || ext == ".mobi") {
        plan.error = "Kindle files can't be imported. Import the EPUB original instead.";
        return plan;
    }
    if (ext == ".doc" || ext == ".rtf" || ext == ".pages" || ext == ".wpd") {
        plan.error = "This older word-processor format can't be imported. Save the document as a Word (.docx) or "
                     "OpenDocument (.odt) file and import that.";
        return plan;
    }
    switch (detect(bytes, plan.sourceName)) {
        case Format::Epub: return readEpub(bytes, plan.sourceName, lim);
        case Format::Pdf: return readPdf(path, plan.sourceName);
        case Format::Html: return readHtml(bytes, plan.sourceName);
        case Format::Docx: return readDocx(bytes, plan.sourceName, lim);
        case Format::Odt: return readOdt(bytes, plan.sourceName, lim);
        case Format::Markdown: return readText(mk::toUtf8(bytes), plan.sourceName, true);
        case Format::Text: return readText(mk::toUtf8(bytes), plan.sourceName, false);
        default:
            plan.error = "This kind of file can't be imported. The Studio reads EPUB, Word (.docx), OpenDocument (.odt), PDF, HTML, "
                         "Markdown and plain-text files.";
            return plan;
    }
}

// ------------------------------------------------------------------ apply

std::vector<Id> apply(Library& lib, const Id& bookId, const Plan& plan, const ApplyOptions& opt) {
    const Book* bk = lib.book(bookId);
    if (!bk || !plan.ok) return {};
    // Images first: the text refers to them by content hash.
    for (auto& [hash, bytes] : plan.images) {
        bool used = false;
        for (auto& s : plan.sections) if (s.include && s.content.find(hash) != std::string::npos) used = true;
        if (used && lib.storeBlob(bytes).empty()) return {};
    }
    // Sections -> tree. A part takes the level-1 sections after it.
    std::vector<NewNode> tree;
    int partIdx = -1;            // index in `tree` of the open part, or -1
    for (auto& s : plan.sections) {
        if (s.role == Role::Part) {
            partIdx = -1;
            if (!s.include) continue;
            tree.push_back({NodeKind::Part, s.title, {}, {}});
            partIdx = (int)tree.size() - 1;
            // A part page with real text keeps it as an opening chapter.
            if (s.words > kPartPageWords) tree[partIdx].children.push_back({NodeKind::Chapter, s.title, s.content, {}});
            continue;
        }
        if (s.level == 0) partIdx = -1;
        if (!s.include) continue;
        NewNode c{NodeKind::Chapter, s.title, s.content, {}};
        if (partIdx >= 0) tree[partIdx].children.push_back(c);
        else tree.push_back(c);
    }
    auto ids = lib.addNodes(bookId, opt.parentId, tree);
    if (ids.empty() && !tree.empty()) return {};
    // List the pictures in the artwork library, named after the book they came with.
    {
        int n = 0;
        const std::string from = plan.meta.title.empty() ? std::string("an import") : "\u201c" + plan.meta.title + "\u201d";
        for (auto& [hash, bytes] : plan.images) {
            bool used = false;
            for (auto& s : plan.sections) if (s.include && s.content.find(hash) != std::string::npos) used = true;
            if (used && !lib.artworkForBlob(hash)) lib.registerArtwork(hash, "Picture " + std::to_string(++n) + " from " + from, "Imported with " + from);
        }
    }
    // Book details: only fill what's empty, never overwrite the author's own.
    Book b = *lib.book(bookId);
    bool changed = false;
    auto fill = [&](std::string& field, const std::string& v) { if (field.empty() && !v.empty()) { field = v; changed = true; } };
    if (opt.setMetadata) {
        if ((b.title.empty() || b.title == "Untitled book" || b.title == "New book") && !plan.meta.title.empty()) { b.title = plan.meta.title; changed = true; }
        fill(b.subtitle, plan.meta.subtitle);
        fill(b.author, plan.meta.author);
        fill(b.synopsis, plan.meta.description);
        fill(b.isbn, plan.meta.isbn);
        if (!plan.meta.language.empty() && (b.language.empty() || b.language == "en-GB") && lower(plan.meta.language).rfind("en", 0) != 0) {
            b.language = plan.meta.language;
            changed = true;
        }
    }
    if (opt.setCover && b.coverBlob.empty() && !plan.meta.coverBytes.empty()) {
        const std::string h = lib.storeBlob(plan.meta.coverBytes);
        if (!h.empty()) { b.coverBlob = h; changed = true; }
    }
    if (changed) lib.updateBook(b);
    return ids;
}

// ------------------------------------------------------- plan hand-over

static const char* roleId(Role r) { return label(r); }
static Role roleFromId(const std::string& s) {
    for (int i = 0; i <= (int)Role::Other; ++i) if (s == label((Role)i)) return (Role)i;
    return Role::Chapter;
}

bool savePlan(const Plan& p, const std::string& folder, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(studio::detail::u8path(folder), ec);
    json j;
    j["ok"] = p.ok;
    j["error"] = p.error;
    j["format"] = (int)p.format;
    j["sourceName"] = p.sourceName;
    j["approximate"] = p.approximate;
    j["meta"] = {{"title", p.meta.title}, {"subtitle", p.meta.subtitle}, {"author", p.meta.author}, {"language", p.meta.language},
                 {"description", p.meta.description}, {"isbn", p.meta.isbn}, {"publisher", p.meta.publisher},
                 {"coverType", p.meta.coverType}};
    j["warnings"] = p.warnings;
    j["sections"] = json::array();
    for (auto& s : p.sections)
        j["sections"].push_back({{"title", s.title}, {"role", roleId(s.role)}, {"level", s.level}, {"include", s.include},
                                 {"content", s.content}, {"words", s.words}, {"source", s.source}});
    j["images"] = json::array();
    for (auto& [hash, bytes] : p.images) {
        if (!atomicWrite(folder + "/" + hash + ".img", bytes, error)) return false;
        j["images"].push_back(hash);
    }
    if (!p.meta.coverBytes.empty() && !atomicWrite(folder + "/cover.img", p.meta.coverBytes, error)) return false;
    // dump() with replacement: text from a damaged file may not be valid UTF-8.
    return atomicWrite(folder + "/plan.json", j.dump(-1, ' ', false, json::error_handler_t::replace), error);
}

static std::string readAll(const std::string& path) {
    std::ifstream f(studio::detail::u8path(path), std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string{};
}

Plan loadPlan(const std::string& folder) {
    Plan p;
    json j = json::parse(readAll(folder + "/plan.json"), nullptr, false);
    if (!j.is_object()) { p.error = "The file couldn't be read: the import helper didn't finish."; return p; }
    try {
        p.ok = j.value("ok", false);
        p.error = j.value("error", "");
        p.format = (Format)std::clamp(j.value("format", (int)Format::Unknown), 0, (int)Format::Unknown);
        p.sourceName = j.value("sourceName", "");
        p.approximate = j.value("approximate", false);
        const json& m = j["meta"];
        p.meta.title = m.value("title", ""); p.meta.subtitle = m.value("subtitle", ""); p.meta.author = m.value("author", "");
        p.meta.language = m.value("language", ""); p.meta.description = m.value("description", ""); p.meta.isbn = m.value("isbn", "");
        p.meta.publisher = m.value("publisher", ""); p.meta.coverType = m.value("coverType", "");
        p.meta.coverBytes = readAll(folder + "/cover.img");
        for (auto& w : j.value("warnings", json::array())) if (w.is_string()) p.warnings.push_back(w.get<std::string>());
        for (auto& s : j.value("sections", json::array())) {
            Section x;
            x.title = s.value("title", ""); x.role = roleFromId(s.value("role", "")); x.level = s.value("level", 0);
            x.include = s.value("include", true); x.content = s.value("content", ""); x.words = s.value("words", (std::int64_t)0);
            x.source = s.value("source", "");
            p.sections.push_back(std::move(x));
        }
        for (auto& h : j.value("images", json::array())) {
            if (!h.is_string()) continue;
            const std::string hash = h.get<std::string>();
            if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
            std::string bytes = readAll(folder + "/" + hash + ".img");
            if (sha256Hex(bytes) == hash) p.images[hash] = std::move(bytes);   // only what it says it is
        }
    } catch (const std::exception&) {
        p = Plan();
        p.error = "The file couldn't be read: the import helper's results were damaged.";
    }
    return p;
}

Plan readFileSafely(const std::string& path, const Limits& limits) {
    try {
        return readFile(path, limits);
    } catch (const std::bad_alloc&) {
        Plan p;
        p.error = "The file is too large or complex to import on this PC (it ran out of memory).";
        return p;
    } catch (const std::exception& e) {
        Plan p;
        p.error = std::string("Something in this file couldn't be read (") + e.what() + "). It may be damaged; if it opens "
                  "normally elsewhere, please send it to NorthRay Software so the importer can be fixed.";
        return p;
    }
}

}  // namespace studio::import

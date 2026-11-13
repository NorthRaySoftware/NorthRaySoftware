#include "markup.hpp"

#include "studio/document.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace studio::markup {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
}

const std::map<std::string, std::uint32_t>& namedEntities() {
    static const std::map<std::string, std::uint32_t> m = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"shy", 0xAD},
        {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
        {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"sbquo", 0x201A}, {"bdquo", 0x201E}, {"laquo", 0xAB}, {"raquo", 0xBB},
        {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"deg", 0xB0},
        {"middot", 0xB7}, {"bull", 0x2022}, {"dagger", 0x2020}, {"Dagger", 0x2021}, {"sect", 0xA7}, {"para", 0xB6},
        {"times", 0xD7}, {"divide", 0xF7}, {"pound", 0xA3}, {"euro", 0x20AC}, {"cent", 0xA2}, {"yen", 0xA5},
        {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"zwj", 0x200D}, {"zwnj", 0x200C}, {"prime", 0x2032},
        {"Prime", 0x2033}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE}, {"iexcl", 0xA1}, {"iquest", 0xBF},
        {"agrave", 0xE0}, {"aacute", 0xE1}, {"acirc", 0xE2}, {"atilde", 0xE3}, {"auml", 0xE4}, {"aring", 0xE5},
        {"aelig", 0xE6}, {"ccedil", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9}, {"ecirc", 0xEA}, {"euml", 0xEB},
        {"igrave", 0xEC}, {"iacute", 0xED}, {"icirc", 0xEE}, {"iuml", 0xEF}, {"eth", 0xF0}, {"ntilde", 0xF1},
        {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocirc", 0xF4}, {"otilde", 0xF5}, {"ouml", 0xF6}, {"oslash", 0xF8},
        {"ugrave", 0xF9}, {"uacute", 0xFA}, {"ucirc", 0xFB}, {"uuml", 0xFC}, {"yacute", 0xFD}, {"thorn", 0xFE},
        {"yuml", 0xFF}, {"szlig", 0xDF}, {"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acirc", 0xC2}, {"Atilde", 0xC3},
        {"Auml", 0xC4}, {"Aring", 0xC5}, {"AElig", 0xC6}, {"Ccedil", 0xC7}, {"Egrave", 0xC8}, {"Eacute", 0xC9},
        {"Ecirc", 0xCA}, {"Euml", 0xCB}, {"Igrave", 0xCC}, {"Iacute", 0xCD}, {"Icirc", 0xCE}, {"Iuml", 0xCF},
        {"ETH", 0xD0}, {"Ntilde", 0xD1}, {"Ograve", 0xD2}, {"Oacute", 0xD3}, {"Ocirc", 0xD4}, {"Otilde", 0xD5},
        {"Ouml", 0xD6}, {"Oslash", 0xD8}, {"Ugrave", 0xD9}, {"Uacute", 0xDA}, {"Ucirc", 0xDB}, {"Uuml", 0xDC},
        {"Yacute", 0xDD}, {"THORN", 0xDE}, {"oelig", 0x153}, {"OElig", 0x152}, {"scaron", 0x161}, {"Scaron", 0x160},
    };
    return m;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

}  // namespace

std::string decodeEntities(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { out += '&'; continue; }
        const std::string name = s.substr(i + 1, semi - i - 1);
        if (!name.empty() && name[0] == '#') {
            std::uint32_t cp = 0;
            if (name.size() > 1 && (name[1] == 'x' || name[1] == 'X')) cp = (std::uint32_t)std::strtoul(name.c_str() + 2, nullptr, 16);
            else cp = (std::uint32_t)std::strtoul(name.c_str() + 1, nullptr, 10);
            appendUtf8(out, cp);
            i = semi;
            continue;
        }
        auto it = namedEntities().find(name);
        if (it == namedEntities().end()) { out += '&'; continue; }
        appendUtf8(out, it->second);
        i = semi;
    }
    return out;
}

std::string toUtf8(const std::string& b) {
    auto utf16 = [&](bool le) {
        std::string out;
        for (size_t i = 2; i + 1 < b.size(); i += 2) {
            std::uint32_t u = le ? ((unsigned char)b[i] | ((unsigned char)b[i + 1] << 8)) : (((unsigned char)b[i] << 8) | (unsigned char)b[i + 1]);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < b.size()) {
                std::uint32_t lo = le ? ((unsigned char)b[i + 2] | ((unsigned char)b[i + 3] << 8)) : (((unsigned char)b[i + 2] << 8) | (unsigned char)b[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
            }
            appendUtf8(out, u);
        }
        return out;
    };
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) return utf16(true);
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFE && (unsigned char)b[1] == 0xFF) return utf16(false);
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) return b.substr(3);
    return b;
}

std::string Token::attr(const std::string& query) const {
    const std::string n = lower(query);   // names are stored lower-case
    for (auto& [k, v] : attrs) if (k == n) return v;
    for (auto& [k, v] : attrs) {
        const size_t c = k.find(':');
        if (c != std::string::npos && k.substr(c + 1) == n) return v;
    }
    return {};
}

std::string Token::local() const {
    const size_t c = name.find(':');
    return c == std::string::npos ? name : name.substr(c + 1);
}

std::vector<Token> tokenize(const std::string& src) {
    std::vector<Token> out;
    size_t i = 0;
    const size_t n = src.size();
    while (i < n) {
        if (src[i] != '<') {
            size_t e = src.find('<', i);
            if (e == std::string::npos) e = n;
            Token t;
            t.kind = Token::Text;
            t.text = decodeEntities(src.substr(i, e - i));
            out.push_back(std::move(t));
            i = e;
            continue;
        }
        if (src.compare(i, 4, "<!--") == 0) {
            const size_t e = src.find("-->", i + 4);
            i = e == std::string::npos ? n : e + 3;
            continue;
        }
        if (src.compare(i, 9, "<![CDATA[") == 0) {
            const size_t e = src.find("]]>", i + 9);
            Token t;
            t.kind = Token::Text;
            t.text = src.substr(i + 9, (e == std::string::npos ? n : e) - i - 9);
            out.push_back(std::move(t));
            i = e == std::string::npos ? n : e + 3;
            continue;
        }
        if (i + 1 < n && (src[i + 1] == '!' || src[i + 1] == '?')) {
            const size_t e = src.find('>', i);
            i = e == std::string::npos ? n : e + 1;
            continue;
        }
        const bool closing = i + 1 < n && src[i + 1] == '/';
        size_t j = i + (closing ? 2 : 1);
        size_t ns = j;
        while (j < n && !isSpace(src[j]) && src[j] != '>' && src[j] != '/') ++j;
        if (j == ns) { Token t; t.kind = Token::Text; t.text = "<"; out.push_back(t); ++i; continue; }
        Token t;
        t.kind = closing ? Token::End : Token::Start;
        t.name = lower(src.substr(ns, j - ns));
        // Attributes.
        while (j < n && src[j] != '>') {
            while (j < n && isSpace(src[j])) ++j;
            if (j < n && src[j] == '/') { t.selfClosing = true; ++j; continue; }
            if (j >= n || src[j] == '>') break;
            size_t as = j;
            while (j < n && !isSpace(src[j]) && src[j] != '=' && src[j] != '>' && src[j] != '/') ++j;
            std::string an = lower(src.substr(as, j - as));
            while (j < n && isSpace(src[j])) ++j;
            std::string av;
            if (j < n && src[j] == '=') {
                ++j;
                while (j < n && isSpace(src[j])) ++j;
                if (j < n && (src[j] == '"' || src[j] == '\'')) {
                    const char q = src[j++];
                    const size_t ve = src.find(q, j);
                    av = src.substr(j, (ve == std::string::npos ? n : ve) - j);
                    j = ve == std::string::npos ? n : ve + 1;
                } else {
                    const size_t vs = j;
                    while (j < n && !isSpace(src[j]) && src[j] != '>') ++j;
                    av = src.substr(vs, j - vs);
                }
            }
            if (!an.empty()) t.attrs.push_back({an, decodeEntities(av)});
            if (j == as) ++j;  // stray character: skip it
        }
        i = j < n ? j + 1 : n;
        // HTML's <script> and <style> hold raw text. Only those exact names: XML
        // such as Word's <w:style> is ordinary markup.
        if (t.kind == Token::Start && !t.selfClosing && (t.name == "script" || t.name == "style")) {
            // Keep style text (for CSS); skip scripts entirely.
            const std::string tag = t.name;
            const size_t start = i;
            size_t e = start;
            while (true) {
                e = src.find("</", e);
                if (e == std::string::npos) { e = n; break; }
                if (lower(src.substr(e + 2, tag.size())) == tag) break;
                e += 2;
            }
            out.push_back(t);
            if (t.name == "style") {
                Token tx;
                tx.kind = Token::Text;
                tx.text = src.substr(start, e - start);
                out.push_back(tx);
            }
            Token end;
            end.kind = Token::End;
            end.name = tag;
            out.push_back(end);
            const size_t gt = src.find('>', e);
            i = e >= n ? n : (gt == std::string::npos ? n : gt + 1);
            continue;
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string resolve(const std::string& baseFile, const std::string& hrefIn, std::string* fragment) {
    std::string href = hrefIn;
    const size_t hash = href.find('#');
    if (fragment) *fragment = hash == std::string::npos ? std::string{} : percentDecode(href.substr(hash + 1));
    if (hash != std::string::npos) href = href.substr(0, hash);
    const size_t q = href.find('?');
    if (q != std::string::npos) href = href.substr(0, q);
    href = percentDecode(href);
    if (href.empty()) return baseFile;  // a link within the same file
    if (href.find("://") != std::string::npos) return {};
    std::vector<std::string> parts;
    if (href[0] != '/') {
        const size_t slash = baseFile.rfind('/');
        if (slash != std::string::npos) {
            std::string dir = baseFile.substr(0, slash);
            size_t p = 0;
            while (p <= dir.size()) {
                size_t e = dir.find('/', p);
                if (e == std::string::npos) e = dir.size();
                if (e > p) parts.push_back(dir.substr(p, e - p));
                p = e + 1;
            }
        }
    }
    size_t p = 0;
    while (p <= href.size()) {
        size_t e = href.find('/', p);
        if (e == std::string::npos) e = href.size();
        const std::string seg = href.substr(p, e - p);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        p = e + 1;
    }
    std::string out;
    for (size_t k = 0; k < parts.size(); ++k) out += (k ? "/" : "") + parts[k];
    return out;
}

std::string collapseSpace(const std::string& s) {
    std::string out;
    bool sp = false;
    for (char c : s) {
        if (isSpace(c)) { sp = true; continue; }
        if (sp && !out.empty()) out += ' ';
        sp = false;
        out += c;
    }
    return out;
}

std::string textOf(const std::vector<Token>& t, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to && i < t.size(); ++i)
        if (t[i].kind == Token::Text) out += t[i].text;
        else if (t[i].kind == Token::Start && t[i].local() == "br") out += ' ';
    return collapseSpace(out);
}

// --------------------------------------------------------------------- CSS

CssFlags parseDeclarations(const std::string& decls) {
    CssFlags f;
    size_t p = 0;
    while (p < decls.size()) {
        size_t e = decls.find(';', p);
        if (e == std::string::npos) e = decls.size();
        const std::string d = decls.substr(p, e - p);
        p = e + 1;
        const size_t c = d.find(':');
        if (c == std::string::npos) continue;
        const std::string k = collapseSpace(lower(d.substr(0, c)));
        std::string v = collapseSpace(lower(d.substr(c + 1)));
        const size_t imp = v.find("!important");
        if (imp != std::string::npos) v = collapseSpace(v.substr(0, imp));
        if (k == "font-style") f.italic = (v == "italic" || v == "oblique") ? 1 : v == "normal" ? 0 : f.italic;
        else if (k == "font-weight") {
            const int num = std::atoi(v.c_str());
            f.bold = (v == "bold" || v == "bolder" || num >= 600) ? 1 : (v == "normal" || (num > 0 && num < 600)) ? 0 : f.bold;
        } else if (k == "font-variant" || k == "font-variant-caps") f.smallcaps = v.find("small-caps") != std::string::npos ? 1 : v == "normal" ? 0 : f.smallcaps;
        else if (k == "text-align") { f.center = v == "center" ? 1 : 0; f.right = v == "right" ? 1 : 0; }
        else if (k == "text-indent") f.noindent = (v == "0" || v.rfind("0px", 0) == 0 || v.rfind("0em", 0) == 0 || v == "0%") ? 1 : 0;
        else if (k == "display") f.hidden = v == "none" ? 1 : 0;
        else if (k == "font") {
            if (v.find("italic") != std::string::npos) f.italic = 1;
            if (v.find("bold") != std::string::npos) f.bold = 1;
            if (v.find("small-caps") != std::string::npos) f.smallcaps = 1;
        }
    }
    return f;
}

static void merge(CssFlags& into, const CssFlags& f) {
    for (auto m : {&CssFlags::italic, &CssFlags::bold, &CssFlags::smallcaps, &CssFlags::center, &CssFlags::noindent,
                   &CssFlags::hidden, &CssFlags::right})
        if (f.*m >= 0) into.*m = f.*m;
}

std::map<std::string, CssFlags> parseCss(const std::string& cssIn) {
    std::map<std::string, CssFlags> out;
    std::string css;
    // Strip comments and @-blocks we don't care about (media queries keep their inner rules).
    for (size_t i = 0; i < cssIn.size(); ++i) {
        if (cssIn.compare(i, 2, "/*") == 0) {
            const size_t e = cssIn.find("*/", i + 2);
            i = e == std::string::npos ? cssIn.size() : e + 1;
            continue;
        }
        css += cssIn[i];
    }
    size_t p = 0;
    while (p < css.size()) {
        const size_t open = css.find('{', p);
        if (open == std::string::npos) break;
        const size_t close = css.find('}', open);
        if (close == std::string::npos) break;
        std::string sel = collapseSpace(css.substr(p, open - p));
        // "@media x {" : step inside.
        if (!sel.empty() && sel[0] == '@') {
            if (sel.rfind("@media", 0) == 0) { p = open + 1; continue; }
            const size_t inner = css.find('{', open + 1);
            p = (inner != std::string::npos && inner < close) ? css.find('}', close + 1) + 1 : close + 1;
            if (p == 0) break;
            continue;
        }
        const CssFlags f = parseDeclarations(css.substr(open + 1, close - open - 1));
        size_t s = 0;
        while (s <= sel.size()) {
            size_t e = sel.find(',', s);
            if (e == std::string::npos) e = sel.size();
            std::string one = collapseSpace(sel.substr(s, e - s));
            s = e + 1;
            // The last compound selector decides: "div.x p.y" -> "p.y".
            const size_t sp = one.find_last_of(" >+~");
            if (sp != std::string::npos) one = one.substr(sp + 1);
            if (one.find(':') != std::string::npos) continue;  // pseudo-classes: skip
            size_t dot = one.find('.');
            while (dot != std::string::npos) {
                size_t end = one.find_first_of(".#[", dot + 1);
                const std::string cls = one.substr(dot + 1, (end == std::string::npos ? one.size() : end) - dot - 1);
                if (!cls.empty()) merge(out[cls], f);
                dot = end != std::string::npos && one[end] == '.' ? end : std::string::npos;
            }
        }
        p = close + 1;
    }
    return out;
}

// --------------------------------------------------------- HTML -> document

bool isSceneBreakText(const std::string& t) {
    std::string s = collapseSpace(t);
    if (s.empty() || s.size() > 40) return false;
    // Allowed: * # ~ - = + . o and common ornaments (any non-ASCII symbol except letters is fine).
    size_t i = 0;
    bool any = false;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (std::isalnum(c) && c != 'o') return false;
            if (!std::strchr("*#~-=+.o _|/\\", (int)c)) return false;
            if (c != ' ') any = true;
            ++i;
            continue;
        }
        // Multi-byte: accept common ornaments and dashes, not letters.
        std::uint32_t cp = 0;
        int len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
        for (int k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        const bool ornament = (cp >= 0x2010 && cp <= 0x2015) || cp == 0x2022 || cp == 0x2026 || cp == 0x2042 ||
                              cp == 0xA0 || cp == 0xA7 || cp == 0xB7 || (cp >= 0x2600 && cp <= 0x27BF) ||
                              (cp >= 0x25A0 && cp <= 0x25FF) || (cp >= 0x2300 && cp <= 0x23FF) || cp == 0x2E3A || cp == 0x2E3B;
        if (!ornament) return false;
        if (cp != 0xA0) any = true;
        i += len;
    }
    // "o" alone or "-" alone are too weak; require something stronger.
    return any && s != "o" && s != "-" && s != "." && s != "..";
}

namespace {

using Builder = studio::doc::Builder;

struct Run { std::string text; unsigned marks = 0; std::string href; bool br = false; };

struct ElemState {
    std::string name;
    unsigned set = 0, clear = 0;
    std::string href;
    std::string style;           // paragraph style this element asks for ("" = inherit)
    bool breakish = false;       // class says it's a scene break / ornament holder
    bool hidden = false;
    bool pre = false;
};

const std::set<std::string> kBlock = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "ul", "ol", "li",
                                      "section", "article", "header", "footer", "aside", "figure", "figcaption", "table",
                                      "tr", "td", "th", "thead", "tbody", "tfoot", "caption", "pre", "hr", "center", "dl",
                                      "dt", "dd", "address", "main", "body", "nav", "hgroup", "details", "summary"};
const std::set<std::string> kVoid = {"br", "hr", "img", "meta", "link", "input", "col", "area", "base", "wbr", "source", "embed", "param"};
const std::set<std::string> kSkip = {"head", "script", "style", "title", "noscript", "template", "object", "iframe",
                                     "video", "audio", "canvas", "math", "button", "select", "textarea", "form", "map"};

bool hasWord(const std::string& classes, const char* w) { return (" " + classes + " ").find(w) != std::string::npos; }

std::string styleFromClass(const std::string& cls, const CssFlags& f) {
    const std::string c = lower(cls);
    if (c.find("epigraph") != std::string::npos) return "epigraph";
    if (c.find("attribution") != std::string::npos || c.find("signature") != std::string::npos || c.find("byline") != std::string::npos ||
        c.find("source") != std::string::npos || c.find("cite") != std::string::npos) return "attribution";
    if (c.find("verse") != std::string::npos || c.find("poem") != std::string::npos || c.find("poetry") != std::string::npos ||
        c.find("stanza") != std::string::npos || c.find("lyric") != std::string::npos) return "verse";
    if (c.find("letter") != std::string::npos || c.find("salutation") != std::string::npos || c.find("closing") != std::string::npos) return "letter";
    if (c.find("footnote") != std::string::npos || c.find("endnote") != std::string::npos || c.find("note") != std::string::npos) return "note";
    if (f.center == 1 || c.find("center") != std::string::npos || c.find("centre") != std::string::npos) return "centered";
    if (f.noindent == 1 || c.find("noindent") != std::string::npos || c.find("no-indent") != std::string::npos ||
        c.find("first") != std::string::npos || c.find("flush") != std::string::npos || c.find("opener") != std::string::npos ||
        c.find("chapter-open") != std::string::npos) return "first";
    return {};
}

class Converter {
public:
    Converter(const HtmlContext& ctx, std::vector<std::string>& warnings) : ctx_(ctx), warnings_(warnings) {}

    std::vector<HtmlPart> run(const std::vector<Token>& toks) {
        parts_.push_back({});
        for (size_t i = 0; i < toks.size(); ++i) {
            const Token& t = toks[i];
            if (t.kind == Token::Text) { text(t.text); continue; }
            const std::string name = t.local();
            if (t.kind == Token::Start) {
                if (skipDepth_ > 0) {
                    if (!t.selfClosing && !kVoid.count(name)) ++skipDepth_;
                    continue;
                }
                start(t, name);
                if (t.selfClosing || kVoid.count(name)) end(name);
            } else {
                if (skipDepth_ > 0) { --skipDepth_; continue; }
                end(name);
            }
        }
        flush();
        finishPart();
        return std::move(parts_);
    }

private:
    const HtmlContext& ctx_;
    std::vector<std::string>& warnings_;
    std::vector<HtmlPart> parts_;
    Builder b_;
    std::vector<ElemState> stack_;
    std::vector<Run> runs_;
    int headingLevel_ = 0;       // open heading (1..6), 0 = none
    std::string paraStyle_;
    bool pendingSpace_ = false;
    int skipDepth_ = 0;
    int tableCell_ = 0;
    bool warnedTable_ = false, warnedMedia_ = false, warnedImage_ = false;

    unsigned marks() const {
        unsigned m = 0;
        for (auto& e : stack_) { m |= e.set; m &= ~e.clear; }
        return m;
    }
    std::string href() const {
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) if (!it->href.empty()) return it->href;
        return {};
    }
    bool inPre() const { for (auto& e : stack_) if (e.pre) return true; return false; }
    bool breakish() const { for (auto& e : stack_) if (e.breakish) return true; return false; }
    std::string currentStyle() const {
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) if (!it->style.empty()) return it->style;
        return "body";
    }
    bool inList() const {
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
            if (it->name == "ul" || it->name == "ol") return true;
            if (it->name == "blockquote") return false;
        }
        return false;
    }

    void finishPart() {
        HtmlPart& p = parts_.back();
        p.json = b_.toJson();
        p.words = b_.words();
        p.empty = b_.empty();
        b_ = Builder();
    }

    void split(const std::string& id) {
        flush();
        // Close containers on the builder so the finished part is valid; the
        // new part starts fresh (lists/quotes rarely span a split point).
        finishPart();
        HtmlPart np;
        np.anchorId = id;
        parts_.push_back(np);
    }

    void text(const std::string& raw) {
        if (skipDepth_ > 0 || raw.empty()) return;
        bool hiddenNow = false;
        for (auto& e : stack_) if (e.hidden) hiddenNow = true;
        if (hiddenNow) return;
        if (inPre()) {
            size_t p = 0;
            while (p <= raw.size()) {
                size_t e = raw.find('\n', p);
                if (e == std::string::npos) e = raw.size();
                const std::string line = raw.substr(p, e - p);
                if (!line.empty()) runs_.push_back({line, marks(), href()});
                if (e < raw.size()) runs_.push_back({"", 0, "", true});
                p = e + 1;
            }
            return;
        }
        const bool lead = !raw.empty() && isSpace(raw.front());
        const bool trail = !raw.empty() && isSpace(raw.back());
        const std::string core = collapseSpace(raw);
        if (lead && !runs_.empty()) pendingSpace_ = true;
        if (core.empty()) return;
        if (tableCell_ > 1 && !runs_.empty() && pendingCell_) { runs_.push_back({" · ", 0, ""}); pendingCell_ = false; pendingSpace_ = false; }
        // The space between words keeps only the styling both sides share
        // (no italic space after an italic word followed by plain text).
        if (pendingSpace_ && !runs_.empty() && !runs_.back().br) {
            const std::string h = runs_.back().href == href() ? href() : std::string{};
            runs_.push_back({" ", runs_.back().marks & marks(), h});
        }
        runs_.push_back({core, marks(), href()});
        pendingSpace_ = trail;
    }
    bool pendingCell_ = false;

    void flush() {
        pendingSpace_ = false;
        // Drop trailing breaks/spaces.
        while (!runs_.empty() && (runs_.back().br || runs_.back().text == " ")) runs_.pop_back();
        while (!runs_.empty() && (runs_.front().br || runs_.front().text == " ")) runs_.erase(runs_.begin());
        if (runs_.empty()) { headingLevel_ = headingLevel_ ? headingLevel_ : 0; return; }
        std::string plain;
        for (auto& r : runs_) plain += r.br ? " " : r.text;
        if (headingLevel_ == 0 && isSceneBreakText(plain)) {
            b_.sceneBreak();
            runs_.clear();
            return;
        }
        if (headingLevel_ > 0) {
            b_.heading(headingLevel_ <= 1 ? 1 : headingLevel_ == 2 ? 2 : 3);
            parts_.back().headings.push_back(collapseSpace(plain));
        } else {
            b_.paragraph(paraStyle_.empty() ? "body" : paraStyle_);
        }
        for (auto& r : runs_) {
            if (r.br) b_.hardBreak();
            else b_.text(r.text, r.marks, r.href);
        }
        b_.endBlock();
        runs_.clear();
    }

    void start(const Token& t, const std::string& name) {
        if (kSkip.count(name)) {
            if (name == "video" || name == "audio" || name == "math" || name == "iframe" || name == "object") {
                if (!warnedMedia_) warnings_.push_back("Audio, video, maths and embedded content can't be imported; they were left out.");
                warnedMedia_ = true;
            }
            skipDepth_ = 1;
            return;
        }
        ElemState e;
        e.name = name;
        const std::string cls = t.attr("class");
        CssFlags f;
        {
            std::string c = cls;
            size_t p = 0;
            while (p <= c.size()) {
                size_t sp = c.find(' ', p);
                if (sp == std::string::npos) sp = c.size();
                auto it = ctx_.classes.find(c.substr(p, sp - p));
                if (it != ctx_.classes.end()) merge(f, it->second);
                p = sp + 1;
            }
            const std::string inl = t.attr("style");
            if (!inl.empty()) merge(f, parseDeclarations(inl));
        }
        if (f.hidden == 1 || t.attr("hidden") == "hidden" || t.attr("aria-hidden") == "true") e.hidden = true;
        const std::string epubType = lower(t.attr("type"));  // epub:type (or plain type)
        if ((name == "body" || name == "section") && parts_.back().epubType.empty() && !epubType.empty() && name != "input")
            parts_.back().epubType = epubType;
        if ((name == "body" || name == "section") && parts_.back().bodyClass.empty()) parts_.back().bodyClass = lower(cls);

        // Split points.
        const std::string id = t.attr("id");
        if (!id.empty() && ctx_.splitIds.count(id) && !(parts_.size() == 1 && b_.empty() && runs_.empty())) split(id);
        else if (!id.empty() && ctx_.splitIds.count(id) && parts_.back().anchorId.empty()) parts_.back().anchorId = id;
        if (ctx_.splitHeadingLevel && name.size() == 2 && name[0] == 'h' && name[1] - '0' == ctx_.splitHeadingLevel &&
            !(b_.empty() && runs_.empty()))
            split(id);

        // Inline marks.
        if (name == "em" || name == "i" || name == "cite" || name == "dfn" || name == "var") e.set |= Builder::Em;
        if (name == "strong" || name == "b") e.set |= Builder::Strong;
        if (name == "u" || name == "ins") e.set |= Builder::Underline;
        if (name == "s" || name == "strike" || name == "del") e.set |= Builder::Strike;
        if (name == "sup") e.set |= Builder::Sup;
        if (name == "sub") e.set |= Builder::Sub;
        if (name == "code" || name == "kbd" || name == "samp" || name == "tt") e.set |= Builder::Code;
        if (f.italic == 1) e.set |= Builder::Em;
        if (f.italic == 0) e.clear |= Builder::Em;
        if (f.bold == 1) e.set |= Builder::Strong;
        if (f.bold == 0 && name != "b" && name != "strong") e.clear |= Builder::Strong;
        if (f.smallcaps == 1 || hasWord(lower(cls), " smallcaps ") || hasWord(lower(cls), " small-caps ") || hasWord(lower(cls), " sc "))
            e.set |= Builder::SmallCaps;
        if (name == "a") {
            const std::string h = t.attr("href");
            if (h.rfind("https://", 0) == 0 || h.rfind("http://", 0) == 0 || h.rfind("mailto:", 0) == 0) e.href = h;
        }
        const std::string lc = lower(cls);
        e.breakish = lc.find("break") != std::string::npos || lc.find("ornament") != std::string::npos ||
                     lc.find("separator") != std::string::npos || lc.find("dinkus") != std::string::npos ||
                     lc.find("asterism") != std::string::npos || epubType.find("separator") != std::string::npos;
        e.pre = name == "pre";

        // Blocks.
        if (kBlock.count(name) || name == "br" || name == "img" || name == "image") {
            if (name == "br") {
                if (!runs_.empty()) runs_.push_back({"", 0, "", true});
                pendingSpace_ = false;
            } else if (name == "hr") {
                flush();
                b_.sceneBreak();
            } else if (name == "img" || name == "image") {
                image(t);
            } else {
                if (name == "td" || name == "th") {
                    ++tableCell_;
                    pendingCell_ = true;
                } else {
                    flush();
                }
            }
            if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') headingLevel_ = name[1] - '0';
            if (name == "blockquote") b_.beginQuote();
            if (name == "ul" || name == "ol") b_.beginList(name == "ol");
            if (name == "table" && !warnedTable_) {
                warnings_.push_back("Tables were turned into plain paragraphs (one per row).");
                warnedTable_ = true;
            }
            if (name == "tr") tableCell_ = 0;
            std::string st = styleFromClass(cls, f);
            if (st.empty() && (epubType.find("epigraph") != std::string::npos)) st = "epigraph";
            if (st.empty() && name == "center") st = "centered";
            if (st.empty() && (name == "aside" && (epubType.find("note") != std::string::npos))) st = "note";
            if (st.empty() && name == "figcaption") st = "centered";
            e.style = st;
        }
        if (kVoid.count(name) || t.selfClosing) { stack_.push_back(e); return; }
        stack_.push_back(e);
        if (kBlock.count(name)) paraStyle_ = currentStyle();
    }

    void end(const std::string& name) {
        // Pop to the matching element (tolerating missing end tags).
        int idx = -1;
        for (int i = (int)stack_.size() - 1; i >= 0; --i) if (stack_[i].name == name) { idx = i; break; }
        if (idx < 0) return;
        const bool block = kBlock.count(name) > 0;
        if (block && name != "td" && name != "th") flush();
        if (block && name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') headingLevel_ = 0;
        if (name == "blockquote") b_.endQuote();
        if (name == "ul" || name == "ol") b_.endList();
        if (name == "tr") { flush(); tableCell_ = 0; }
        stack_.resize(idx);
        paraStyle_ = currentStyle();
    }

    void image(const Token& t) {
        std::string src = t.attr("src");
        if (src.empty()) src = t.attr("href");  // svg <image xlink:href>
        if (src.empty() || !ctx_.loadImage) return;
        if (breakish()) {  // an ornament standing in for a scene break
            flush();
            b_.sceneBreak();
            return;
        }
        const std::string path = resolve(ctx_.filePath, src);
        const std::string hash = path.empty() ? std::string{} : ctx_.loadImage(path);
        if (hash.empty()) {
            if (!warnedImage_) warnings_.push_back("Some images couldn't be read and were left out.");
            warnedImage_ = true;
            return;
        }
        flush();
        b_.image(hash, t.attr("alt"));
    }
};

}  // namespace

std::vector<HtmlPart> convertHtml(const std::string& html, const HtmlContext& ctx, std::vector<std::string>& warnings) {
    Converter c(ctx, warnings);
    return c.run(tokenize(toUtf8(html)));
}

}  // namespace studio::markup

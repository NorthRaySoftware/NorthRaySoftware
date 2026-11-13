// Plain text and Markdown import, and the line-based structuring shared with PDF import.
#include "importutil.hpp"
#include "markup.hpp"
#include "studio/document.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace studio::import {

namespace detail {

namespace {

using Builder = studio::doc::Builder;


std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool isRoman(const std::string& s) {
    static const std::regex r("^(?=[MDCLXVI])M{0,3}(CM|CD|D?C{0,3})(XC|XL|L?X{0,3})(IX|IV|V?I{0,3})$", std::regex::icase);
    return !s.empty() && s.size() <= 8 && std::regex_match(s, r);
}

const char* kNumberWords = "one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|thirteen|fourteen|fifteen|sixteen|"
                           "seventeen|eighteen|nineteen|twenty[- a-z]*|thirty[- a-z]*|forty[- a-z]*|fifty[- a-z]*|first|second|third|"
                           "fourth|fifth|sixth|seventh|eighth|ninth|tenth";

enum class HeadingKind { None, Chapter, Part, Special };

// Is this short line a chapter or part heading?
HeadingKind classifyHeading(const std::string& line) {
    const std::string t = trim(line);
    if (t.empty() || t.size() > 90) return HeadingKind::None;
    const std::string l = lowerAscii(t);
    static const std::regex chapter(std::string("^(chapter|chap\\.)\\s+([0-9]+|[ivxlcdm]+|") + kNumberWords + ")\\b.*$", std::regex::icase);
    static const std::regex part(std::string("^(part|book|volume)\\s+([0-9]+|[ivxlcdm]+|") + kNumberWords + ")\\b.*$", std::regex::icase);
    static const std::regex special("^(prologue|epilogue|interlude|preface|foreword|afterword|introduction|acknowledge?ments|"
                                    "dedication|about the author|author'?s note|a note from the author|appendix\\b.*)[.:]?$",
                                    std::regex::icase);
    if (std::regex_match(l, part)) return HeadingKind::Part;
    if (std::regex_match(l, chapter)) return HeadingKind::Chapter;
    if (std::regex_match(l, special)) return HeadingKind::Special;
    // A bare number or roman numeral on its own line.
    std::string d = t;
    if (!d.empty() && d.back() == '.') d.pop_back();
    if (!d.empty() && d.size() <= 3 && std::all_of(d.begin(), d.end(), [](char c) { return std::isdigit((unsigned char)c); })) return HeadingKind::Chapter;
    if (isRoman(d) && d != "I" && d != "i") return HeadingKind::Chapter;  // a lone "I" is usually the pronoun
    return HeadingKind::None;
}

// Markdown inline: **strong**, *em*, _em_, `code`, [text](url).
void markdownRuns(const std::string& s, Builder& b) {
    unsigned marks = 0;
    std::string buf;
    auto flushBuf = [&](const std::string& href = {}) {
        if (!buf.empty()) b.text(buf, marks, href);
        buf.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size()) { buf += s[++i]; continue; }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) {
            flushBuf();
            marks ^= Builder::Strong;
            ++i;
            continue;
        }
        if (c == '*' || c == '_') {
            // '_' only toggles at word boundaries, so snake_case stays as it is.
            const bool boundary = c == '*' || ((marks & Builder::Em) ? (i + 1 >= s.size() || !std::isalnum((unsigned char)s[i + 1]))
                                                                     : (i == 0 || !std::isalnum((unsigned char)s[i - 1])));
            if (boundary) { flushBuf(); marks ^= Builder::Em; continue; }
        }
        if (c == '`') { flushBuf(); marks ^= Builder::Code; continue; }
        if (c == '[') {
            const size_t close = s.find("](", i);
            const size_t end = close == std::string::npos ? std::string::npos : s.find(')', close);
            if (close != std::string::npos && end != std::string::npos) {
                flushBuf();
                buf = s.substr(i + 1, close - i - 1);
                flushBuf(s.substr(close + 2, end - close - 2));
                i = end;
                continue;
            }
        }
        buf += c;
    }
    flushBuf();
}

struct Para {
    std::vector<const Line*> lines;
};

}  // namespace

int headingKind(const std::string& line) {
    switch (classifyHeading(line)) {
        case HeadingKind::Chapter: return 1;
        case HeadingKind::Part: return 2;
        case HeadingKind::Special: return 3;
        default: return 0;
    }
}

void buildFromLines(const std::vector<Line>& lines, Plan& plan, bool markdown, const std::string& fallbackTitle) {
    // 1. Group lines into paragraphs.
    std::vector<Para> paras;
    for (const Line& l : lines) {
        const std::string t = trim(l.text);
        if (t.empty() && l.runs.empty()) { if (!paras.empty() && !paras.back().lines.empty()) paras.push_back({}); continue; }
        if (paras.empty() || paras.back().lines.empty() || l.startsParagraph || l.heading ||
            (!paras.back().lines.empty() && paras.back().lines.back()->heading))
            paras.push_back({});
        paras.back().lines.push_back(&l);
    }
    paras.erase(std::remove_if(paras.begin(), paras.end(), [](const Para& p) { return p.lines.empty(); }), paras.end());

    // 2. Find headings: known (PDF/Markdown), then patterns.
    struct Mark { size_t para; HeadingKind kind; std::string title; int level; };
    std::vector<Mark> heads;
    auto paraText = [](const Para& p) {
        std::string s;
        for (auto* l : p.lines) {
            std::string t = trim(l->text);
            if (!s.empty()) s += ' ';
            s += t;
        }
        return s;
    };
    bool anyMdH1 = false;
    if (markdown)
        for (auto& p : paras) if (p.lines.size() == 1 && p.lines[0]->text.rfind("# ", 0) == 0) anyMdH1 = true;
    for (size_t i = 0; i < paras.size(); ++i) {
        const Para& p = paras[i];
        const std::string t = paraText(p);
        if (markdown && p.lines.size() == 1 && t.size() > 1 && t[0] == '#') {
            size_t h = 0;
            while (h < t.size() && t[h] == '#') ++h;
            const std::string title = trim(t.substr(h));
            const HeadingKind k = classifyHeading(title);
            if (h == 1 || (!anyMdH1 && h == 2)) heads.push_back({i, k == HeadingKind::Part ? HeadingKind::Part : HeadingKind::Chapter, title, (int)h});
            continue;
        }
        if (p.lines.size() == 1 && p.lines[0]->heading) {
            const HeadingKind k = classifyHeading(t);
            heads.push_back({i, k == HeadingKind::Part ? HeadingKind::Part : HeadingKind::Chapter, t, p.lines[0]->headingLevel});
            continue;
        }
        if (p.lines.size() == 2) {
            // "CHAPTER ONE" with its title on the next line.
            const std::string l1 = trim(p.lines[0]->text), l2 = trim(p.lines[1]->text);
            const HeadingKind k = classifyHeading(l1);
            if (k != HeadingKind::None && !l2.empty() && l2.size() <= 60 && l2.back() != '.' && l2.back() != ',' &&
                std::isupper((unsigned char)l2[0])) {
                const bool numberOnly = lowerAscii(l1).find("chapter") == std::string::npos && k == HeadingKind::Chapter;
                heads.push_back({i, k, l1 + (numberOnly ? ". " : ": ") + l2, 1});
                continue;
            }
        }
        if (p.lines.size() == 1 && t.size() <= 90) {
            const HeadingKind k = classifyHeading(t);
            if (k != HeadingKind::None) heads.push_back({i, k, t, 1});
        }
    }
    // "CHAPTER ONE" followed by a short title line: join them ("Chapter One: The Storm").
    for (size_t k = 0; k < heads.size(); ++k) {
        const size_t next = heads[k].para + 1;
        // The title line may itself have been found as a heading (larger type in a PDF): it's
        // still the chapter's title, as long as it isn't another "Chapter ..." line.
        const bool nextIsTitleHead = k + 1 < heads.size() && heads[k + 1].para == next && paras[next].lines.size() == 1 &&
                                     paras[next].lines[0]->heading && classifyHeading(heads[k + 1].title) == HeadingKind::None &&
                                     classifyHeading(heads[k].title) != HeadingKind::None;
        if (heads[k].kind == HeadingKind::Chapter && next < paras.size() && (k + 1 >= heads.size() || heads[k + 1].para != next || nextIsTitleHead)) {
            const std::string nt = paraText(paras[next]);
            const bool shortTitle = paras[next].lines.size() == 1 && nt.size() <= 60 && !nt.empty() &&
                                    nt.back() != '.' && nt.back() != ',' && nt.back() != '"' && nt.find("”") == std::string::npos &&
                                    (paras[next].lines[0]->heading || std::count(nt.begin(), nt.end(), ' ') <= 7) &&
                                    std::isupper((unsigned char)nt[0]) && !studio::markup::isSceneBreakText(nt);
            const bool numberOnly = classifyHeading(heads[k].title) == HeadingKind::Chapter &&
                                    lowerAscii(heads[k].title).find("chapter") == std::string::npos;
            if (shortTitle && (paras[next].lines[0]->heading || numberOnly || heads[k].title.size() < 20)) {
                heads[k].title += (numberOnly ? ". " : ": ") + nt;
                heads[k].title = trim(heads[k].title);
                paras[next].lines.clear();  // consumed
                if (nextIsTitleHead) heads.erase(heads.begin() + (long)k + 1);
            }
        }
    }
    // Large type before the first "Chapter ..." / "Part ..." line in a PDF is the
    // title page (title, author), not chapters: leave it in the front matter.
    if (!markdown) {
        size_t firstLabel = heads.size();
        for (size_t k = 0; k < heads.size(); ++k) if (classifyHeading(heads[k].title) != HeadingKind::None) { firstLabel = k; break; }
        if (firstLabel < heads.size() && firstLabel > 0) {
            bool allSizeOnly = true;
            for (size_t k = 0; k < firstLabel; ++k)
                if (!(paras[heads[k].para].lines.size() == 1 && paras[heads[k].para].lines[0]->heading &&
                      classifyHeading(heads[k].title) == HeadingKind::None))
                    allSizeOnly = false;
            if (allSizeOnly) heads.erase(heads.begin(), heads.begin() + (long)firstLabel);
        }
    }

    // 3. Emit sections.
    auto emitPara = [&](const Para& p, Builder& b) {
        if (p.lines.empty()) return;
        const std::string t = paraText(p);
        if (studio::markup::isSceneBreakText(t)) { b.sceneBreak(); return; }
        if (markdown && t.rfind("> ", 0) == 0) {
            b.beginQuote();
            b.paragraph();
            markdownRuns(t.substr(2), b);
            b.endQuote();
            return;
        }
        std::string style = "body";
        b.paragraph(style);
        bool first = true;
        for (auto* l : p.lines) {
            if (!first) b.text(" ");
            first = false;
            if (!l->runs.empty()) {
                for (auto& [txt, m] : l->runs) b.text(txt, m);
            } else if (markdown) {
                markdownRuns(trim(l->text), b);
            } else {
                b.text(trim(l->text), l->italic ? (unsigned)Builder::Em : 0u);
            }
        }
        b.endBlock();
    };

    size_t start = 0;
    Builder front;
    if (heads.empty()) {
        Builder b;
        for (auto& p : paras) emitPara(p, b);
        Section s;
        s.title = fallbackTitle;
        s.content = b.toJson();
        s.words = doc::wordCount(s.content);
        s.source = "whole file";
        plan.sections.push_back(std::move(s));
        plan.warnings.push_back("No chapter headings were found, so everything came in as one chapter. You can split it in the editor.");
        return;
    }
    // Before the first heading: front matter (title lines, copyright) or an opening.
    for (start = 0; start < heads[0].para; ++start) emitPara(paras[start], front);
    if (!front.empty()) {
        Section s;
        const std::string j = front.toJson();
        const auto words = doc::wordCount(j);
        s.title = words > 150 ? "Opening" : "Front matter";
        s.role = words > 150 ? Role::Chapter : Role::TitlePage;
        s.content = j;
        s.words = words;
        s.include = words > 150;
        s.source = "before the first heading";
        plan.sections.push_back(std::move(s));
    }
    bool inPart = false;
    for (size_t k = 0; k < heads.size(); ++k) {
        const size_t from = heads[k].para + 1;
        const size_t to = k + 1 < heads.size() ? heads[k + 1].para : paras.size();
        Builder b;
        for (size_t i = from; i < to; ++i) emitPara(paras[i], b);
        Section s;
        s.title = heads[k].title;
        s.role = heads[k].kind == HeadingKind::Part ? Role::Part : roleFrom("", s.title, "", "");
        if (s.role == Role::Part) inPart = true;
        s.level = (s.role != Role::Part && inPart) ? 1 : 0;
        s.content = b.toJson();
        s.words = doc::wordCount(s.content);
        s.include = includedByDefault(s.role);
        s.source = paras[heads[k].para].lines.empty() ? "" : paras[heads[k].para].lines[0]->source;
        plan.sections.push_back(std::move(s));
    }
}

}  // namespace detail

Plan readText(const std::string& text, const std::string& name, bool markdown) {
    Plan plan;
    plan.format = markdown ? Format::Markdown : Format::Text;
    plan.sourceName = name;
    plan.approximate = !markdown;
    std::vector<Line> lines;
    size_t p = 0, nonBlank = 0, blank = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        Line l;
        l.text = text.substr(p, e - p);
        if (!l.text.empty() && l.text.back() == '\r') l.text.pop_back();
        l.source = "line " + std::to_string(lines.size() + 1);
        if (l.text.find_first_not_of(" \t") == std::string::npos) ++blank; else ++nonBlank;
        lines.push_back(std::move(l));
        p = e + 1;
    }
    // Files with one paragraph per line (no blank lines between): every line starts a paragraph.
    if (nonBlank > 20 && blank < nonBlank / 10)
        for (auto& l : lines) l.startsParagraph = true;
    std::string stem = name;
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    plan.meta.title = stem;
    detail::buildFromLines(lines, plan, markdown, stem);
    std::int64_t total = 0;
    for (auto& s : plan.sections) total += s.words;
    if (total == 0) { plan.error = "The file has no text to import."; return plan; }
    plan.ok = true;
    return plan;
}

}  // namespace studio::import

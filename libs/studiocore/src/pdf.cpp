// PDF import. PDFium (BSD/Apache, loaded at run time) supplies each
// character with its position, size and font; this file rebuilds lines,
// paragraphs and chapters from that, removing page numbers and running
// headers. A PDF is a picture of pages, not a manuscript, so the result is
// marked approximate and the author checks it in the preview.
#include "importutil.hpp"
#include "internal.hpp"
#include "markup.hpp"
#include "studio/document.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace studio::import {

namespace {

// --- the slice of the PDFium API we use, loaded by name -------------------

using DOC = void*;
using PAGE = void*;
using TEXTPAGE = void*;
using BOOKMARK = void*;
using DEST = void*;
using PAGEOBJ = void*;

struct Pdfium {
    void (*InitLibrary)() = nullptr;
    DOC (*LoadMemDocument64)(const void*, size_t, const char*) = nullptr;
    void (*CloseDocument)(DOC) = nullptr;
    unsigned long (*GetLastError)() = nullptr;
    unsigned long (*GetDocPermissions)(DOC) = nullptr;
    int (*GetPageCount)(DOC) = nullptr;
    PAGE (*LoadPage)(DOC, int) = nullptr;
    void (*ClosePage)(PAGE) = nullptr;
    float (*GetPageWidthF)(PAGE) = nullptr;
    float (*GetPageHeightF)(PAGE) = nullptr;
    unsigned long (*GetMetaText)(DOC, const char*, void*, unsigned long) = nullptr;
    TEXTPAGE (*TextLoadPage)(PAGE) = nullptr;
    void (*TextClosePage)(TEXTPAGE) = nullptr;
    int (*TextCountChars)(TEXTPAGE) = nullptr;
    unsigned (*TextGetUnicode)(TEXTPAGE, int) = nullptr;
    int (*TextIsGenerated)(TEXTPAGE, int) = nullptr;
    double (*TextGetFontSize)(TEXTPAGE, int) = nullptr;
    unsigned long (*TextGetFontInfo)(TEXTPAGE, int, void*, unsigned long, int*) = nullptr;
    int (*TextGetFontWeight)(TEXTPAGE, int) = nullptr;
    int (*TextGetCharBox)(TEXTPAGE, int, double*, double*, double*, double*) = nullptr;
    BOOKMARK (*BookmarkGetFirstChild)(DOC, BOOKMARK) = nullptr;
    BOOKMARK (*BookmarkGetNextSibling)(DOC, BOOKMARK) = nullptr;
    unsigned long (*BookmarkGetTitle)(BOOKMARK, void*, unsigned long) = nullptr;
    DEST (*BookmarkGetDest)(DOC, BOOKMARK) = nullptr;
    int (*DestGetDestPageIndex)(DOC, DEST) = nullptr;
    int (*PageCountObjects)(PAGE) = nullptr;
    PAGEOBJ (*PageGetObject)(PAGE, int) = nullptr;
    int (*PageObjGetType)(PAGEOBJ) = nullptr;
    bool ok = false;
    std::string why;
};

std::string g_folder;
std::mutex g_mutex;  // PDFium isn't thread-safe: one import at a time

Pdfium& pdfium() {
    static Pdfium p;
    static bool tried = false;
    if (tried) return p;
    tried = true;
#ifdef _WIN32
    std::wstring dir;
    if (!g_folder.empty()) {
        dir = studio::detail::u8path(g_folder).wstring();
    } else {
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
        dir.assign(buf, n);
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
    }
    HMODULE h = LoadLibraryExW((dir + L"\\pdfium.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { p.why = "pdfium.dll wasn't found next to the program."; return p; }
    auto sym = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(h, n)); };
#else
    std::string path = (g_folder.empty() ? std::string("") : g_folder + "/") + "libpdfium.so";
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h && !g_folder.empty()) h = dlopen("libpdfium.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { p.why = "libpdfium.so couldn't be loaded."; return p; }
    auto sym = [&](const char* n) { return dlsym(h, n); };
#endif
    bool all = true;
    auto get = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(sym(name));
        if (!fn) all = false;
    };
    get(p.InitLibrary, "FPDF_InitLibrary");
    get(p.LoadMemDocument64, "FPDF_LoadMemDocument64");
    get(p.CloseDocument, "FPDF_CloseDocument");
    get(p.GetLastError, "FPDF_GetLastError");
    get(p.GetDocPermissions, "FPDF_GetDocPermissions");
    get(p.GetPageCount, "FPDF_GetPageCount");
    get(p.LoadPage, "FPDF_LoadPage");
    get(p.ClosePage, "FPDF_ClosePage");
    get(p.GetPageWidthF, "FPDF_GetPageWidthF");
    get(p.GetPageHeightF, "FPDF_GetPageHeightF");
    get(p.GetMetaText, "FPDF_GetMetaText");
    get(p.TextLoadPage, "FPDFText_LoadPage");
    get(p.TextClosePage, "FPDFText_ClosePage");
    get(p.TextCountChars, "FPDFText_CountChars");
    get(p.TextGetUnicode, "FPDFText_GetUnicode");
    get(p.TextIsGenerated, "FPDFText_IsGenerated");
    get(p.TextGetFontSize, "FPDFText_GetFontSize");
    get(p.TextGetFontInfo, "FPDFText_GetFontInfo");
    get(p.TextGetFontWeight, "FPDFText_GetFontWeight");
    get(p.TextGetCharBox, "FPDFText_GetCharBox");
    get(p.BookmarkGetFirstChild, "FPDFBookmark_GetFirstChild");
    get(p.BookmarkGetNextSibling, "FPDFBookmark_GetNextSibling");
    get(p.BookmarkGetTitle, "FPDFBookmark_GetTitle");
    get(p.BookmarkGetDest, "FPDFBookmark_GetDest");
    get(p.DestGetDestPageIndex, "FPDFDest_GetDestPageIndex");
    get(p.PageCountObjects, "FPDFPage_CountObjects");
    get(p.PageGetObject, "FPDFPage_GetObject");
    get(p.PageObjGetType, "FPDFPageObj_GetType");
    if (!all) { p.why = "The PDF library is an unexpected version."; return p; }
    p.InitLibrary();
    p.ok = true;
    return p;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

std::string utf16le(const std::string& b) {
    std::string out;
    for (size_t i = 0; i + 1 < b.size(); i += 2) {
        std::uint32_t u = (unsigned char)b[i] | ((unsigned char)b[i + 1] << 8);
        if (u == 0) break;
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < b.size()) {
            const std::uint32_t lo = (unsigned char)b[i + 2] | ((unsigned char)b[i + 3] << 8);
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        appendUtf8(out, u);
    }
    return out;
}

std::string squash(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c) || c >= 0x80) o += (char)std::tolower(c);
    return o;
}

// --- page model -----------------------------------------------------------

struct Glyph { std::uint32_t cp; double l, r, b, t, size; bool italic, bold; };

struct PLine {
    std::string text;
    std::vector<std::pair<std::string, unsigned>> runs;
    double left = 1e9, right = -1e9, top = -1e9, bottom = 1e9, size = 0;
    int page = 0;
    bool drop = false;           // header, footer or page number
    bool heading = false;
    int headingLevel = 0;
    int chars = 0;
    std::string outlineTitle;    // set when a bookmark points here
};

struct PPage { double w = 0, h = 0; std::vector<PLine> lines; int images = 0; };

double roundHalf(double v) { return std::round(v * 2) / 2; }

// Builds lines from the glyphs PDFium returns in reading order; PDFium marks
// line ends with generated CR/LF.
void buildLines(const std::vector<Glyph>& gs, int pageNo, PPage& page) {
    PLine cur;
    cur.page = pageNo;
    std::map<double, int> sizes;
    unsigned curMarks = 0;
    std::string runText;
    auto endRun = [&] {
        if (!runText.empty()) cur.runs.push_back({runText, curMarks});
        runText.clear();
    };
    auto endLine = [&] {
        endRun();
        // Trim spaces at the ends of the line.
        while (!cur.runs.empty() && !cur.runs.back().first.empty() && cur.runs.back().first.back() == ' ') {
            cur.runs.back().first.pop_back();
            if (cur.runs.back().first.empty()) cur.runs.pop_back();
        }
        while (!cur.text.empty() && cur.text.back() == ' ') cur.text.pop_back();
        if (cur.chars > 0 && !cur.text.empty()) {
            int best = 0;
            for (auto& [s, n] : sizes) if (n > best) { best = n; cur.size = s; }
            page.lines.push_back(cur);
        }
        cur = PLine();
        cur.page = pageNo;
        sizes.clear();
        curMarks = 0;
    };
    for (size_t i = 0; i < gs.size(); ++i) {
        const Glyph& g = gs[i];
        if (g.cp == '\r' || g.cp == '\n') {
            if (g.cp == '\n' && i > 0 && gs[i - 1].cp == '\r') continue;
            endLine();
            continue;
        }
        if (g.cp == 0xAD || g.cp == 0xFFFE || g.cp == 0xFEFF || g.cp == 2) continue;  // soft hyphens and markers
        const bool space = g.cp == ' ' || g.cp == 0xA0 || g.cp == '\t';
        if (space) {
            if (!cur.text.empty() && cur.text.back() != ' ') { cur.text += ' '; runText += ' '; }
            continue;
        }
        const unsigned m = (g.italic ? (unsigned)doc::Builder::Em : 0u) | (g.bold ? (unsigned)doc::Builder::Strong : 0u);
        if (m != curMarks) {
            // Keep a trailing space with the earlier run.
            endRun();
            curMarks = m;
        }
        appendUtf8(cur.text, g.cp);
        appendUtf8(runText, g.cp);
        cur.left = std::min(cur.left, g.l);
        cur.right = std::max(cur.right, g.r);
        cur.top = std::max(cur.top, g.t);
        cur.bottom = std::min(cur.bottom, g.b);
        sizes[roundHalf(g.size)]++;
        ++cur.chars;
    }
    endLine();
}

bool isPageNumber(const std::string& t) {
    std::string s = t;
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '-' || c == '|' || c == '.'; }), s.end());
    if (s.empty() || s.size() > 6) return false;
    if (std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit((unsigned char)c); })) return true;
    return std::all_of(s.begin(), s.end(), [](char c) { return std::strchr("ivxlcdmIVXLCDM", c) != nullptr; });
}

std::string withoutDigits(const std::string& t) {
    std::string o;
    for (unsigned char c : t) if (!std::isdigit(c)) o += (char)std::tolower(c);
    return squash(o);
}

}  // namespace

void setPdfiumFolder(const std::string& folder) { g_folder = folder; }

bool pdfiumAvailable(std::string* why) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Pdfium& p = pdfium();
    if (why) *why = p.why;
    return p.ok;
}

Plan readPdf(const std::string& path, const std::string& name) {
    Plan plan;
    plan.format = Format::Pdf;
    plan.sourceName = name;
    plan.approximate = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    Pdfium& F = pdfium();
    if (!F.ok) { plan.error = "PDF import isn't available: " + F.why; return plan; }

    std::string bytes;
    {
        std::ifstream f(studio::detail::u8path(path), std::ios::binary);
        if (!f) { plan.error = "The file couldn't be opened."; return plan; }
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    DOC doc = F.LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
    if (!doc) {
        const unsigned long e = F.GetLastError();
        if (e == 4) plan.error = "This PDF is password-protected. Remove the password (in the program that made it) and import it again.";
        else if (e == 5) plan.error = "This PDF uses a kind of protection the Studio can't open.";
        else plan.error = "This doesn't look like a readable PDF; it may be damaged.";
        return plan;
    }
    struct Closer { Pdfium& F; DOC d; ~Closer() { F.CloseDocument(d); } } closer{F, doc};

    // Respect the PDF's own "no copying" setting: refuse, and say why.
    const unsigned long perms = F.GetDocPermissions(doc);
    if (perms != 0xFFFFFFFFul && !(perms & 0x10)) {
        plan.error = "This PDF's settings don't allow its text to be copied, so the Studio won't import it. If it's your own book, "
                     "export the PDF again without that restriction, or import the original manuscript.";
        return plan;
    }

    auto meta = [&](const char* tag) {
        const unsigned long n = F.GetMetaText(doc, tag, nullptr, 0);
        if (n <= 2) return std::string{};
        std::string buf(n, '\0');
        F.GetMetaText(doc, tag, buf.data(), n);
        std::string v = studio::markup::collapseSpace(utf16le(buf));
        // Placeholders some programs write instead of leaving the field empty.
        std::string low = v;
        for (auto& c : low) c = (char)std::tolower((unsigned char)c);
        for (const char* junk : {"unspecified", "untitled", "unknown", "none", "n/a", "-", "anonymous", "document", "title", "subject"})
            if (low == junk) return std::string{};
        // "Microsoft Word - My Novel.docx" -> "My Novel"
        if (v.rfind("Microsoft Word - ", 0) == 0) {
            v = v.substr(17);
            for (const char* ext : {".docx", ".doc", ".odt", ".rtf"}) {
                const size_t n = std::string(ext).size();
                if (v.size() > n && v.compare(v.size() - n, n, ext) == 0) v.resize(v.size() - n);
            }
        }
        return v;
    };
    plan.meta.title = meta("Title");
    plan.meta.author = meta("Author");
    plan.meta.description = meta("Subject");

    const int pageCount = F.GetPageCount(doc);
    if (pageCount <= 0) { plan.error = "This PDF has no pages."; return plan; }
    if (pageCount > 5000) { plan.error = "This PDF has too many pages to import."; return plan; }

    // 1. Glyphs -> lines, page by page.
    std::vector<PPage> pages(pageCount);
    std::map<double, long> sizeHist;
    long totalChars = 0;
    int imagePages = 0;
    std::map<std::string, std::pair<bool, bool>> fontStyle;  // font name -> italic, bold
    for (int pi = 0; pi < pageCount; ++pi) {
        PAGE page = F.LoadPage(doc, pi);
        if (!page) continue;
        PPage& pg = pages[pi];
        pg.w = F.GetPageWidthF(page);
        pg.h = F.GetPageHeightF(page);
        const int objs = F.PageCountObjects(page);
        for (int o = 0; o < objs && o < 5000; ++o)
            if (F.PageObjGetType(F.PageGetObject(page, o)) == 3) ++pg.images;
        if (pg.images) ++imagePages;
        TEXTPAGE tp = F.TextLoadPage(page);
        if (tp) {
            const int n = F.TextCountChars(tp);
            std::vector<Glyph> gs;
            gs.reserve(n);
            char fname[256];
            for (int i = 0; i < n; ++i) {
                Glyph g{};
                g.cp = F.TextGetUnicode(tp, i);
                if (g.cp == '\r' || g.cp == '\n' || g.cp == ' ') { gs.push_back(g); continue; }
                F.TextGetCharBox(tp, i, &g.l, &g.r, &g.b, &g.t);
                g.size = F.TextGetFontSize(tp, i);
                int flags = 0;
                const unsigned long len = F.TextGetFontInfo(tp, i, fname, sizeof fname, &flags);
                std::string fn = len > 0 && len <= sizeof fname ? std::string(fname) : std::string{};
                auto it = fontStyle.find(fn);
                if (it == fontStyle.end()) {
                    std::string lf = fn;
                    for (char& c : lf) c = (char)std::tolower((unsigned char)c);
                    const bool it_ = (flags & 0x40) || lf.find("italic") != std::string::npos || lf.find("oblique") != std::string::npos ||
                                     lf.find("-it") != std::string::npos;
                    const bool bd = (flags & 0x40000) || lf.find("bold") != std::string::npos || lf.find("black") != std::string::npos ||
                                    lf.find("heavy") != std::string::npos || lf.find("semibold") != std::string::npos;
                    it = fontStyle.emplace(fn, std::make_pair(it_, bd)).first;
                }
                g.italic = it->second.first;
                g.bold = it->second.second || F.TextGetFontWeight(tp, i) >= 600;
                gs.push_back(g);
                sizeHist[roundHalf(g.size)]++;
                ++totalChars;
            }
            buildLines(gs, pi, pg);
            F.TextClosePage(tp);
        }
        F.ClosePage(page);
    }
    if (totalChars < 20 * std::max(1, pageCount / 4)) {
        plan.error = imagePages > 0 ? "This PDF is made of scanned page images with no text in them. Importing it would need text "
                                      "recognition (OCR), which the Studio doesn't do. Import the original manuscript instead."
                                    : "This PDF has no text to import.";
        return plan;
    }

    // 2. Body text size and line spacing.
    double body = 10;
    { long best = 0; for (auto& [s, n] : sizeHist) if (n > best) { best = n; body = s; } }
    std::vector<double> gaps;
    for (auto& pg : pages)
        for (size_t i = 1; i < pg.lines.size(); ++i)
            if (std::abs(pg.lines[i].size - body) < 0.6 && std::abs(pg.lines[i - 1].size - body) < 0.6) {
                const double gap = pg.lines[i - 1].bottom - pg.lines[i].bottom;
                if (gap > 0 && gap < body * 4) gaps.push_back(gap);
            }
    std::sort(gaps.begin(), gaps.end());
    const double leading = gaps.empty() ? body * 1.3 : gaps[gaps.size() / 2];

    // 3. Running headers, footers and page numbers: lines near the top or
    // bottom edge that repeat across pages (ignoring digits), or are just a number.
    std::map<std::string, int> edgeCount;
    for (auto& pg : pages)
        for (auto& l : pg.lines) {
            const bool edge = l.top > pg.h * 0.88 || l.bottom < pg.h * 0.12;
            if (edge) edgeCount[withoutDigits(l.text)]++;
        }
    int dropped = 0;
    for (auto& pg : pages) {
        for (auto& l : pg.lines) {
            const bool nearEdge = l.top > pg.h * 0.88 || l.bottom < pg.h * 0.12;
            if (!nearEdge) continue;
            const std::string key = withoutDigits(l.text);
            if (isPageNumber(l.text) || (pageCount > 4 && !key.empty() && edgeCount[key] >= std::max(3, pageCount / 5))) {
                l.drop = true;
                ++dropped;
            }
        }
        pg.lines.erase(std::remove_if(pg.lines.begin(), pg.lines.end(), [](const PLine& l) { return l.drop; }), pg.lines.end());
    }

    // 4. Headings: noticeably larger type, short lines. Adjacent heading
    // lines merge ("CHAPTER ONE" / "The Storm"). A single large letter is a drop cap.
    std::vector<double> headSizes;
    for (auto& pg : pages) {
        for (size_t i = 0; i < pg.lines.size(); ++i) {
            PLine& l = pg.lines[i];
            if (l.size >= body * 1.18 && l.text.size() <= 90) {
                const std::string t = l.text;
                if (t.size() <= 2 && std::isalpha((unsigned char)t[0]) && i + 1 < pg.lines.size() && pg.lines[i + 1].size < body * 1.18) {
                    // Drop cap: glue onto the next line.
                    PLine& nx = pg.lines[i + 1];
                    nx.text = t + nx.text;
                    if (!nx.runs.empty()) nx.runs.front().first = t + nx.runs.front().first;
                    l.drop = true;
                    continue;
                }
                l.heading = true;
                headSizes.push_back(roundHalf(l.size));
            }
        }
        pg.lines.erase(std::remove_if(pg.lines.begin(), pg.lines.end(), [](const PLine& l) { return l.drop; }), pg.lines.end());
    }
    std::sort(headSizes.begin(), headSizes.end());
    headSizes.erase(std::unique(headSizes.begin(), headSizes.end()), headSizes.end());
    std::reverse(headSizes.begin(), headSizes.end());
    for (auto& pg : pages)
        for (auto& l : pg.lines)
            if (l.heading) {
                const auto it = std::find(headSizes.begin(), headSizes.end(), roundHalf(l.size));
                l.headingLevel = it == headSizes.end() ? 1 : (int)(it - headSizes.begin()) + 1;
            }

    // 5. Bookmarks (the PDF's outline), when present, are the most reliable chapter list.
    struct Mark { std::string title; int page; int depth; };
    std::vector<Mark> marks;
    std::function<void(BOOKMARK, int)> walk = [&](BOOKMARK parent, int depth) {
        if (depth > 1) return;
        for (BOOKMARK b = F.BookmarkGetFirstChild(doc, parent); b && marks.size() < 2000; b = F.BookmarkGetNextSibling(doc, b)) {
            const unsigned long n = F.BookmarkGetTitle(b, nullptr, 0);
            std::string buf(n, '\0');
            if (n) F.BookmarkGetTitle(b, buf.data(), n);
            DEST d = F.BookmarkGetDest(doc, b);
            const int page = d ? F.DestGetDestPageIndex(doc, d) : -1;
            const std::string title = studio::markup::collapseSpace(utf16le(buf));
            if (page >= 0 && !title.empty()) marks.push_back({title, page, depth});
            walk(b, depth + 1);
        }
    };
    walk(nullptr, 0);
    const bool useMarks = marks.size() >= 2;
    if (useMarks) {
        // Bookmarks decide chapter starts; type-size headings inside them stay as text.
        for (auto& pg : pages) for (auto& l : pg.lines) if (l.heading) { l.heading = false; l.headingLevel = 0; }
        for (auto& m : marks) {
            if (m.page >= pageCount) continue;
            auto& lines = pages[m.page].lines;
            const std::string want = squash(m.title);
            bool placed = false;
            for (size_t i = 0; i < lines.size() && i < 12 && !placed; ++i) {
                const std::string have = squash(lines[i].text);
                if (!have.empty() && (want.find(have) != std::string::npos || have.find(want) != std::string::npos)) {
                    lines[i].heading = true;
                    lines[i].headingLevel = m.depth + 1;
                    lines[i].outlineTitle = m.title;
                    // Following lines that are also part of the title.
                    for (size_t j = i + 1; j < lines.size() && j < i + 3; ++j)
                        if (want.find(squash(lines[j].text)) != std::string::npos && !squash(lines[j].text).empty()) lines[j].drop = true;
                    placed = true;
                }
            }
            if (!placed) {
                PLine h;
                h.text = m.title;
                h.heading = true;
                h.headingLevel = m.depth + 1;
                h.outlineTitle = m.title;
                h.page = m.page;
                h.chars = 1;
                h.size = body;
                lines.insert(lines.begin(), h);
            }
        }
        for (auto& pg : pages)
            pg.lines.erase(std::remove_if(pg.lines.begin(), pg.lines.end(), [](const PLine& l) { return l.drop; }), pg.lines.end());
    }

    // 6. Paragraphs: indentation, a gap above, or a short line ending a
    // sentence before. Hyphenated line ends are rejoined.
    std::map<long, int> leftHist;
    double textRight = 0;
    for (auto& pg : pages)
        for (auto& l : pg.lines)
            if (!l.heading && std::abs(l.size - body) < 0.6) { leftHist[std::lround(l.left)]++; textRight = std::max(textRight, l.right); }
    double margin = 0;
    { int best = 0; for (auto& [x, n] : leftHist) if (n > best) { best = n; margin = (double)x; } }
    std::vector<Line> out;
    int sceneGaps = 0;
    for (auto& pg : pages) {
        const PLine* prev = nullptr;
        for (auto& l : pg.lines) {
            Line ln;
            ln.source = "page " + std::to_string(l.page + 1);
            ln.heading = l.heading;
            ln.headingLevel = l.headingLevel;
            ln.text = l.outlineTitle.empty() ? l.text : l.outlineTitle;
            if (!l.heading && l.outlineTitle.empty()) ln.runs = l.runs;
            const bool indented = l.left > margin + body * 0.7 && l.left < margin + body * 6;
            const double gapAbove = prev ? prev->bottom - l.bottom : 0;
            const bool bigGap = prev && gapAbove > leading * 1.6;
            bool shortBefore = false;
            if (prev && !prev->text.empty()) {
                const char last = prev->text.back();
                const std::string& pt = prev->text;
                const bool endsSentence = std::strchr(".!?:\"')", last) != nullptr || (pt.size() >= 3 &&
                                          (pt.compare(pt.size() - 3, 3, "\xe2\x80\x9d") == 0 || pt.compare(pt.size() - 3, 3, "\xe2\x80\x99") == 0));
                shortBefore = endsSentence && prev->right < textRight - body * 2.5;
            }
            ln.startsParagraph = !prev || indented || bigGap || shortBefore || l.heading || (prev && prev->heading);
            // A large blank space between body paragraphs: a scene break.
            if (prev && !prev->heading && !l.heading && gapAbove > leading * 2.6 && gapAbove < pg.h * 0.5) {
                Line sb;
                sb.text = "* * *";
                sb.startsParagraph = true;
                sb.source = ln.source;
                out.push_back(sb);
                ++sceneGaps;
            }
            // Hyphenation: "some-" + "thing" -> "something" (keep real compounds before capitals).
            if (!out.empty() && !ln.startsParagraph && !out.back().heading) {
                Line& pl = out.back();
                std::string& ptxt = pl.runs.empty() ? pl.text : pl.runs.back().first;
                if (!ptxt.empty() && ptxt.back() == '-' && ptxt.size() >= 2 && std::isalpha((unsigned char)ptxt[ptxt.size() - 2]) &&
                    !ln.text.empty() && std::islower((unsigned char)ln.text[0])) {
                    ptxt.pop_back();
                    if (!pl.runs.empty()) { pl.text.pop_back(); }
                    // Join: the next line continues the word with no space.
                    if (pl.runs.empty()) pl.text += ln.text;
                    else {
                        for (auto& r : ln.runs) pl.runs.push_back(r);
                        pl.text += ln.text;
                    }
                    prev = &l;
                    continue;
                }
            }
            out.push_back(std::move(ln));
            prev = &l;
        }
        // Each page's last line continues on the next page unless the next starts a paragraph (decided there).
    }
    // Words broken across a page: the next page's first line is joined above
    // only when it doesn't look like a new paragraph (handled by startsParagraph).

    std::string stem = name;
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    if (plan.meta.title.empty()) plan.meta.title = stem;
    detail::buildFromLines(out, plan, false, plan.meta.title);
    for (auto& s : plan.sections) s.source = s.source.empty() ? "" : s.source;
    if (dropped) plan.warnings.push_back("Page numbers and running headers were removed (" + std::to_string(dropped) + " lines).");
    if (imagePages) plan.warnings.push_back("Pictures in the PDF weren't imported; add them in the Studio.");
    if (sceneGaps) plan.warnings.push_back("Large gaps between paragraphs became scene breaks (" + std::to_string(sceneGaps) + "). Check them in the preview.");
    plan.warnings.insert(plan.warnings.begin(), useMarks ? "Chapters were found from the PDF's bookmarks." :
                                                         "Chapters were found from heading sizes and wording, so check the list before importing.");
    std::int64_t total = 0;
    for (auto& s : plan.sections) total += s.words;
    if (total == 0) { plan.error = "No text could be read from this PDF."; return plan; }
    plan.ok = true;
    return plan;
}

}  // namespace studio::import

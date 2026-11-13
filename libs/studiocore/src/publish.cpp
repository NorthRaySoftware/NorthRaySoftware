// Publishing: EPUB 3 packaging, pre-flight checks, spine width.
#include "studio/publish.hpp"

#include "studio/ebook.hpp"
#include "studio/library.hpp"

#include "internal.hpp"
#include "miniz.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#include "stb/stb_image.h"   // the implementation is in typeset_output.cpp
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#include "stb/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace studio::publish {

using ebook::escape;

namespace {

std::string readAll(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream in(detail::u8path(path), std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ------------------------------------------------------------------ zip file

// A zip archive as the EPUB container wants it: "mimetype" first and stored
// uncompressed, everything else deflated.
class Zip {
public:
    void add(const std::string& name, const std::string& data, bool compress = true) {
        Entry e;
        e.name = name;
        e.crc = (std::uint32_t)mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.data()), data.size());
        e.size = (std::uint32_t)data.size();
        std::string body = data;
        e.method = 0;
        if (compress && !data.empty()) {
            size_t n = 0;
            void* d = tdefl_compress_mem_to_heap(data.data(), data.size(), &n,
                                                 (int)tdefl_create_comp_flags_from_zip_params(9, -15, MZ_DEFAULT_STRATEGY));
            if (d && n < data.size()) { body.assign(static_cast<const char*>(d), n); e.method = 8; }
            mz_free(d);
        }
        e.csize = (std::uint32_t)body.size();
        e.offset = (std::uint32_t)out_.size();
        u32(out_, 0x04034b50); u16(out_, 20); u16(out_, 0x0800); u16(out_, e.method); u16(out_, time_); u16(out_, date_);
        u32(out_, e.crc); u32(out_, e.csize); u32(out_, e.size); u16(out_, (std::uint16_t)name.size()); u16(out_, 0);
        out_ += name;
        out_ += body;
        entries_.push_back(e);
    }
    std::string finish() {
        const std::uint32_t cd = (std::uint32_t)out_.size();
        for (const Entry& e : entries_) {
            u32(out_, 0x02014b50); u16(out_, 20); u16(out_, 20); u16(out_, 0x0800); u16(out_, e.method); u16(out_, time_); u16(out_, date_);
            u32(out_, e.crc); u32(out_, e.csize); u32(out_, e.size); u16(out_, (std::uint16_t)e.name.size());
            u16(out_, 0); u16(out_, 0); u16(out_, 0); u16(out_, 0); u32(out_, 0); u32(out_, e.offset);
            out_ += e.name;
        }
        const std::uint32_t cdSize = (std::uint32_t)out_.size() - cd;
        u32(out_, 0x06054b50); u16(out_, 0); u16(out_, 0); u16(out_, (std::uint16_t)entries_.size()); u16(out_, (std::uint16_t)entries_.size());
        u32(out_, cdSize); u32(out_, cd); u16(out_, 0);
        return std::move(out_);
    }
    void setTime(const std::tm& t) {
        time_ = (std::uint16_t)((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2));
        date_ = (std::uint16_t)(((std::max(t.tm_year, 80) - 80) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday);
    }
private:
    struct Entry { std::string name; std::uint32_t crc = 0, size = 0, csize = 0, offset = 0; std::uint16_t method = 0; };
    static void u16(std::string& s, unsigned v) { s += (char)(v & 0xFF); s += (char)((v >> 8) & 0xFF); }
    static void u32(std::string& s, std::uint32_t v) { u16(s, v & 0xFFFF); u16(s, v >> 16); }
    std::string out_;
    std::vector<Entry> entries_;
    std::uint16_t time_ = 0, date_ = (1 << 5) | 1;
};

// ------------------------------------------------------------------ pictures

struct Rgba {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
};

Rgba decodeRgba(const std::string& bytes) {
    Rgba r;
    int n = 0;
    unsigned char* d = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), (int)bytes.size(), &r.w, &r.h, &n, 4);
    if (!d) return {};
    r.px.assign(d, d + (size_t)r.w * r.h * 4);
    stbi_image_free(d);
    return r;
}

Rgba cropTo(const Rgba& in, const Placement& p) {
    const int x0 = std::clamp((int)std::lround(p.cropX * in.w), 0, in.w - 1);
    const int y0 = std::clamp((int)std::lround(p.cropY * in.h), 0, in.h - 1);
    const int w = std::clamp((int)std::lround(p.cropW * in.w), 1, in.w - x0);
    const int h = std::clamp((int)std::lround(p.cropH * in.h), 1, in.h - y0);
    Rgba out;
    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h * 4);
    for (int y = 0; y < h; ++y)
        std::copy_n(&in.px[((size_t)(y0 + y) * in.w + x0) * 4], (size_t)w * 4, &out.px[(size_t)y * w * 4]);
    return out;
}

// Scaled down by averaging the pixels each new pixel covers.
Rgba shrink(const Rgba& in, int maxSide) {
    if (std::max(in.w, in.h) <= maxSide) return in;
    const double k = (double)maxSide / std::max(in.w, in.h);
    Rgba out;
    out.w = std::max(1, (int)std::lround(in.w * k));
    out.h = std::max(1, (int)std::lround(in.h * k));
    out.px.resize((size_t)out.w * out.h * 4);
    for (int y = 0; y < out.h; ++y) {
        const int sy0 = (int)((double)y * in.h / out.h), sy1 = std::max(sy0 + 1, (int)((double)(y + 1) * in.h / out.h));
        for (int x = 0; x < out.w; ++x) {
            const int sx0 = (int)((double)x * in.w / out.w), sx1 = std::max(sx0 + 1, (int)((double)(x + 1) * in.w / out.w));
            unsigned long sum[4] = {0, 0, 0, 0};
            unsigned long n = 0;
            for (int sy = sy0; sy < sy1 && sy < in.h; ++sy)
                for (int sx = sx0; sx < sx1 && sx < in.w; ++sx, ++n)
                    for (int c = 0; c < 4; ++c) sum[c] += in.px[((size_t)sy * in.w + sx) * 4 + c];
            for (int c = 0; c < 4; ++c) out.px[((size_t)y * out.w + x) * 4 + c] = (std::uint8_t)(n ? sum[c] / n : 0);
        }
    }
    return out;
}

bool opaque(const Rgba& r) {
    for (size_t i = 3; i < r.px.size(); i += 4)
        if (r.px[i] != 255) return false;
    return true;
}

void appendBytes(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), (size_t)size); }

// A picture ready for the EPUB: its bytes and file type.
struct Encoded {
    std::string bytes, ext, mime;
};
Encoded encode(const Rgba& r, bool photo) {
    Encoded e;
    if (photo && opaque(r)) {
        std::vector<std::uint8_t> rgb((size_t)r.w * r.h * 3);
        for (size_t i = 0, n = (size_t)r.w * r.h; i < n; ++i) std::copy_n(&r.px[i * 4], 3, &rgb[i * 3]);
        stbi_write_jpg_to_func(appendBytes, &e.bytes, r.w, r.h, 3, rgb.data(), 90);
        e.ext = "jpg";
        e.mime = "image/jpeg";
    } else {
        stbi_write_png_to_func(appendBytes, &e.bytes, r.w, r.h, 4, r.px.data(), r.w * 4);
        e.ext = "png";
        e.mime = "image/png";
    }
    return e;
}

const char* mimeOf(const std::string& format) {
    if (format == "jpeg") return "image/jpeg";
    if (format == "png") return "image/png";
    if (format == "gif") return "image/gif";
    return nullptr;   // WebP and BMP aren't read everywhere: converted
}
const char* extOf(const std::string& format) { return format == "jpeg" ? "jpg" : format == "png" ? "png" : "gif"; }

bool fullCrop(const Placement& p) { return p.cropX <= 1e-4 && p.cropY <= 1e-4 && p.cropW >= 0.9999 && p.cropH >= 0.9999; }

// ------------------------------------------------------------------- text

std::string xhtml(const std::string& lang, const std::string& title, const std::string& body, const std::string& bodyClass = {}) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n"
           "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"" + escape(lang) +
           "\" lang=\"" + escape(lang) + "\">\n<head>\n<meta charset=\"UTF-8\"/>\n<title>" + escape(title) +
           "</title>\n<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n</head>\n<body" +
           (bodyClass.empty() ? std::string() : " class=\"" + bodyClass + "\"") + ">\n" + body + "</body>\n</html>\n";
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
}

std::string isoNow() {
    char b[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return b;
}

// A stable identifier for a book without an ISBN (a name-based UUID from its id).
std::string bookUuid(const typeset::Source& src) {
    std::string h = sha256Hex("northray-writing-studio:" + (src.id.empty() ? src.title + "\n" + src.author : src.id));
    h[12] = '5';
    static const char* variant = "89ab";
    h[16] = variant[std::stoi(h.substr(16, 1), nullptr, 16) & 3];
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}

const char* kEpubCss = R"CSS(
/* EPUB pages */
body { margin: 0 4%; }
body.cover { margin: 0; padding: 0; text-align: center; }
div.cover { height: 100%; text-align: center; page-break-inside: avoid; }
div.cover img { max-width: 100%; max-height: 100%; height: auto; }
section.titlepage { text-align: center; padding-top: 25%; }
section.titlepage h1 { font-size: 2em; margin: 0 0 0.5em; }
section.titlepage p.subtitle { font-size: 1.2em; font-style: italic; margin: 0 0 3em; text-indent: 0; text-align: center; }
section.titlepage p.author { font-size: 1.2em; letter-spacing: 0.05em; text-indent: 0; text-align: center; }
section.copyright { padding-top: 40%; }
section.copyright p { text-indent: 0; text-align: left; font-size: 0.85em; margin: 0 0 0.6em; }
nav ol { list-style: none; margin: 0 0 0 1em; padding: 0; }
nav > ol { margin-left: 0; }
nav li { margin: 0.35em 0; }
nav h1 { margin-top: 1.5em; }
figure.pic .frame img { width: 100%; height: auto; }
div.scene { text-align: center; margin: 1.1em 0; text-indent: 0; }
div.scene + p { text-indent: 0; }
)CSS";

}  // namespace

// ----------------------------------------------------------------------- EPUB

EpubResult buildEpub(const typeset::Source& src, const EpubOptions& options) {
    EpubResult r;
    if (src.sections.empty()) { r.error = "The manuscript is empty: there's nothing to put in an ebook yet."; return r; }
    const std::string lang = src.language.empty() ? "en-GB" : src.language;
    const std::string title = src.title.empty() ? std::string("Untitled") : src.title;
    const int maxSide = std::max(320, options.maxPicturePixels);

    // Pictures: each blob and crop becomes one file in images/.
    struct Pic { std::string file, mime; bool ok = false; };
    std::map<std::string, Pic> pics;
    std::vector<std::pair<std::string, Encoded>> files;   // path -> data
    std::map<std::string, Rgba> decoded;
    bool allAlt = true, anyPicture = false;
    std::set<std::string> unreadable;
    const ebook::Content content = ebook::buildCropped(src, [&](const std::string& blob, const Placement& p) -> std::string {
        anyPicture = true;
        char key[160];
        std::snprintf(key, sizeof key, "%s:%.4f:%.4f:%.4f:%.4f", blob.c_str(), p.cropX, p.cropY, p.cropW, p.cropH);
        if (auto it = pics.find(key); it != pics.end()) return it->second.file;
        Pic pic;
        const auto path = src.picturePaths.find(blob);
        const std::string bytes = path == src.picturePaths.end() ? std::string() : readAll(path->second);
        const ImageInfo info = imageInfo(bytes);
        const std::string base = "images/p" + blob.substr(0, 12) + "-" + std::to_string(pics.size() + 1);
        if (info.ok && fullCrop(p) && mimeOf(info.format) && std::max(info.width, info.height) <= maxSide) {
            pic.file = base + "." + extOf(info.format);
            pic.mime = mimeOf(info.format);
            files.push_back({pic.file, Encoded{bytes, extOf(info.format), pic.mime}});
            pic.ok = true;
        } else {
            auto d = decoded.find(blob);
            if (d == decoded.end()) d = decoded.emplace(blob, decodeRgba(bytes)).first;
            if (d->second.w > 0) {
                const Encoded e = encode(shrink(fullCrop(p) ? d->second : cropTo(d->second, p), maxSide), info.format == "jpeg" || info.format == "webp");
                pic.file = base + "." + e.ext;
                pic.mime = e.mime;
                files.push_back({pic.file, e});
                pic.ok = true;
            } else {
                unreadable.insert(blob);
                pic.file = base + ".missing";
            }
        }
        pics[key] = pic;
        return pic.file;
    });
    for (const auto& sec : src.sections)
        if (sec.content.find("\"type\":\"image\"") != std::string::npos) {
            // Every picture needs a description for readers who can't see it.
            size_t at = 0;
            while ((at = sec.content.find("\"type\":\"image\"", at)) != std::string::npos) {
                const size_t end = sec.content.find('}', at);
                const std::string attrs = sec.content.substr(at, end == std::string::npos ? std::string::npos : end - at);
                if (attrs.find("\"alt\":\"") == std::string::npos || attrs.find("\"alt\":\"\"") != std::string::npos) allAlt = false;
                at += 14;
            }
        }
    if (!unreadable.empty())
        r.notes.push_back(std::to_string(unreadable.size()) + (unreadable.size() == 1 ? " picture couldn't be read and is left out." : " pictures couldn't be read and are left out."));

    // The cover.
    Encoded cover;
    if (!src.coverPath.empty()) {
        const std::string bytes = readAll(src.coverPath);
        const ImageInfo info = imageInfo(bytes);
        if (info.ok && mimeOf(info.format) && info.format != "gif") cover = Encoded{bytes, extOf(info.format), mimeOf(info.format)};
        else if (const Rgba px = decodeRgba(bytes); px.w > 0) cover = encode(shrink(px, 2560), true);
        if (cover.bytes.empty()) r.notes.push_back("The cover couldn't be read, so the ebook has none.");
    }

    std::string modified = options.modified.empty() ? isoNow() : options.modified;
    const std::string isbn = validIsbn(src.isbn) ? isbnDigits(src.isbn) : std::string();
    const std::string identifier = isbn.empty() ? "urn:uuid:" + bookUuid(src) : "urn:isbn:" + isbn;

    // Pages.
    struct Item { std::string id, href, mime, properties; bool spine = false; };
    std::vector<Item> items;
    std::vector<std::pair<std::string, std::string>> docs;   // href -> xhtml
    if (!cover.bytes.empty()) {
        items.push_back({"cover-image", "images/cover." + cover.ext, cover.mime, "cover-image", false});
        docs.push_back({"cover.xhtml", xhtml(lang, title, "<div class=\"cover\"><img src=\"images/cover." + cover.ext + "\" alt=\"" +
                                                                 escape("The cover of " + title) + "\"/></div>\n", "cover")});
        items.push_back({"cover", "cover.xhtml", "application/xhtml+xml", "", true});
    }
    {
        std::string b = "<section class=\"titlepage\" epub:type=\"titlepage\">\n<h1>" + escape(title) + "</h1>\n";
        if (!src.subtitle.empty()) b += "<p class=\"subtitle\">" + escape(src.subtitle) + "</p>\n";
        if (!src.author.empty()) b += "<p class=\"author\">" + escape(src.author) + "</p>\n";
        b += "</section>\n";
        docs.push_back({"title.xhtml", xhtml(lang, title, b)});
        items.push_back({"titlepage", "title.xhtml", "application/xhtml+xml", "", true});
    }
    {
        std::string b = "<section class=\"copyright\" epub:type=\"copyright-page\">\n";
        const int year = src.year > 0 ? src.year : 1900 + [] { const std::time_t t = std::time(nullptr); return std::gmtime(&t)->tm_year; }();
        b += "<p>Copyright &#169; " + std::to_string(year) + (src.author.empty() ? std::string() : " " + escape(src.author)) + "</p>\n";
        b += "<p>All rights reserved.</p>\n";
        if (!src.edition.empty()) b += "<p>" + escape(src.edition) + "</p>\n";
        if (!src.isbns.empty())
            for (auto& [label, i] : src.isbns) b += "<p>ISBN " + escape(i) + (label.empty() ? std::string() : " (" + escape(label) + ")") + "</p>\n";
        else if (!isbn.empty()) b += "<p>ISBN " + escape(src.isbn) + " (ebook)</p>\n";
        b += "</section>\n";
        docs.push_back({"copyright.xhtml", xhtml(lang, "Copyright", b)});
        items.push_back({"copyright", "copyright.xhtml", "application/xhtml+xml", "", true});
    }
    items.push_back({"nav", "nav.xhtml", "application/xhtml+xml", "nav", true});

    // Chapters, and the contents (parts hold the chapters after them).
    std::string navList, ncxPoints;
    int play = 1;
    bool inPart = false;
    std::string firstChapter;
    for (size_t i = 0; i < content.chapters.size(); ++i) {
        const ebook::Chapter& ch = content.chapters[i];
        char name[32];
        std::snprintf(name, sizeof name, "chapter-%03d.xhtml", (int)i + 1);
        std::string html = ch.html;
        replaceAll(html, "<section class=\"chapter\">", "<section class=\"chapter\" epub:type=\"chapter\" role=\"doc-chapter\">");
        replaceAll(html, "<section class=\"part\">", "<section class=\"part\" epub:type=\"part\" role=\"doc-part\">");
        replaceAll(html, "<hr class=\"scene\"/>", "<div class=\"scene\" role=\"separator\" aria-label=\"Scene break\">*&#160;&#160;*&#160;&#160;*</div>");
        docs.push_back({name, xhtml(lang, ch.title, html)});
        items.push_back({"c" + std::to_string(i + 1), name, "application/xhtml+xml", "", true});
        if (firstChapter.empty() && !ch.part) firstChapter = name;
        const std::string label = escape(ch.title);
        char point[64];
        std::snprintf(point, sizeof point, "<navPoint id=\"n%d\" playOrder=\"%d\">", play, play);
        ++play;
        if (ch.part) {
            if (inPart) { navList += "</ol></li>\n"; ncxPoints += "</navPoint>\n"; }
            navList += "<li><a href=\"" + std::string(name) + "\">" + label + "</a><ol>\n";
            ncxPoints += std::string(point) + "<navLabel><text>" + label + "</text></navLabel><content src=\"" + name + "\"/>\n";
            inPart = true;
        } else {
            navList += "<li><a href=\"" + std::string(name) + "\">" + label + "</a></li>\n";
            ncxPoints += std::string(point) + "<navLabel><text>" + label + "</text></navLabel><content src=\"" + name + "\"/></navPoint>\n";
        }
        ++r.chapters;
    }
    if (inPart) { navList += "</ol></li>\n"; ncxPoints += "</navPoint>\n"; }
    // A part with nothing after it still needs a list item inside its <ol>.
    replaceAll(navList, "<ol>\n</ol>", "");
    if (firstChapter.empty() && !content.chapters.empty()) firstChapter = "chapter-001.xhtml";

    {
        std::string b = "<nav epub:type=\"toc\" id=\"toc\" role=\"doc-toc\">\n<h1>Contents</h1>\n<ol>\n" + navList + "</ol>\n</nav>\n";
        b += "<nav epub:type=\"landmarks\" id=\"landmarks\" hidden=\"hidden\">\n<ol>\n";
        if (!cover.bytes.empty()) b += "<li><a epub:type=\"cover\" href=\"cover.xhtml\">Cover</a></li>\n";
        b += "<li><a epub:type=\"titlepage\" href=\"title.xhtml\">Title page</a></li>\n";
        b += "<li><a epub:type=\"toc\" href=\"nav.xhtml\">Contents</a></li>\n";
        if (!firstChapter.empty()) b += "<li><a epub:type=\"bodymatter\" href=\"" + firstChapter + "\">Start reading</a></li>\n";
        b += "</ol>\n</nav>\n";
        docs.push_back({"nav.xhtml", xhtml(lang, "Contents", b)});
    }
    const std::string ncx = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\" xml:lang=\"" +
                            escape(lang) + "\">\n<head>\n<meta name=\"dtb:uid\" content=\"" + escape(identifier) +
                            "\"/>\n<meta name=\"dtb:depth\" content=\"" + (navList.find("<ol>") != std::string::npos ? "2" : "1") +
                            "\"/>\n<meta name=\"dtb:totalPageCount\" content=\"0\"/>\n<meta name=\"dtb:maxPageNumber\" content=\"0\"/>\n</head>\n"
                            "<docTitle><text>" + escape(title) + "</text></docTitle>\n" +
                            (src.author.empty() ? std::string() : "<docAuthor><text>" + escape(src.author) + "</text></docAuthor>\n") +
                            "<navMap>\n" + ncxPoints + "</navMap>\n</ncx>\n";

    // The package.
    std::string meta;
    meta += "<dc:identifier id=\"bookid\">" + escape(identifier) + "</dc:identifier>\n";
    meta += "<dc:title id=\"title\">" + escape(title) + "</dc:title>\n";
    if (!src.subtitle.empty()) {
        meta += "<dc:title id=\"subtitle\">" + escape(src.subtitle) + "</dc:title>\n";
        meta += "<meta refines=\"#title\" property=\"title-type\">main</meta>\n<meta refines=\"#subtitle\" property=\"title-type\">subtitle</meta>\n";
    }
    if (!src.author.empty()) {
        meta += "<dc:creator id=\"author\">" + escape(src.author) + "</dc:creator>\n";
        meta += "<meta refines=\"#author\" property=\"role\" scheme=\"marc:relators\">aut</meta>\n";
    }
    meta += "<dc:language>" + escape(lang) + "</dc:language>\n";
    if (!src.synopsis.empty()) meta += "<dc:description>" + escape(src.synopsis) + "</dc:description>\n";
    if (!src.genre.empty()) meta += "<dc:subject>" + escape(src.genre) + "</dc:subject>\n";
    meta += "<meta property=\"dcterms:modified\">" + escape(modified) + "</meta>\n";
    if (!cover.bytes.empty()) meta += "<meta name=\"cover\" content=\"cover-image\"/>\n";
    // Accessibility: how the book can be read, and what helps.
    meta += "<meta property=\"schema:accessMode\">textual</meta>\n";
    if (anyPicture) meta += "<meta property=\"schema:accessMode\">visual</meta>\n";
    meta += std::string("<meta property=\"schema:accessModeSufficient\">") + (!anyPicture || allAlt ? "textual" : "textual,visual") + "</meta>\n";
    for (const char* f : {"structuralNavigation", "tableOfContents", "readingOrder"})
        meta += std::string("<meta property=\"schema:accessibilityFeature\">") + f + "</meta>\n";
    if (anyPicture && allAlt) meta += "<meta property=\"schema:accessibilityFeature\">alternativeText</meta>\n";
    meta += "<meta property=\"schema:accessibilityHazard\">none</meta>\n";
    meta += std::string("<meta property=\"schema:accessibilitySummary\">") +
            (anyPicture ? (allAlt ? "Reflowable text with chapter headings and a table of contents. Every picture has a text description."
                                  : "Reflowable text with chapter headings and a table of contents. Some pictures have no text description.")
                        : "Reflowable text with chapter headings and a table of contents. No pictures.") +
            "</meta>\n";

    std::string manifest = "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
                           "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n";
    std::string spine;
    for (const Item& it : items) {
        manifest += "<item id=\"" + it.id + "\" href=\"" + it.href + "\" media-type=\"" + it.mime + "\"" +
                    (it.properties.empty() ? std::string() : " properties=\"" + it.properties + "\"") + "/>\n";
        if (it.spine) spine += "<itemref idref=\"" + it.id + "\"" + (it.id == "cover" ? " linear=\"yes\"" : "") + "/>\n";
    }
    int n = 0;
    for (const auto& [path, e] : files) manifest += "<item id=\"img" + std::to_string(++n) + "\" href=\"" + path + "\" media-type=\"" + e.mime + "\"/>\n";
    r.pictures = n;

    const std::string opf = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\" "
                            "xml:lang=\"" + escape(lang) + "\" prefix=\"schema: http://schema.org/\">\n"
                            "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n" + meta + "</metadata>\n<manifest>\n" + manifest +
                            "</manifest>\n<spine toc=\"ncx\">\n" + spine + "</spine>\n</package>\n";

    // The archive.
    Zip zip;
    {
        std::tm t{};
        int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
        if (std::sscanf(modified.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6) {
            t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
            zip.setTime(t);
        }
    }
    zip.add("mimetype", "application/epub+zip", false);
    zip.add("META-INF/container.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
                                       "<rootfiles>\n<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n</rootfiles>\n</container>\n");
    zip.add("OEBPS/content.opf", opf);
    zip.add("OEBPS/toc.ncx", ncx);
    zip.add("OEBPS/style.css", ebook::stylesheet() + kEpubCss);
    for (const auto& [href, x] : docs) zip.add("OEBPS/" + href, x);
    if (!cover.bytes.empty()) zip.add("OEBPS/images/cover." + cover.ext, cover.bytes, cover.ext == "png");
    // JPEGs are already compressed; PNGs and GIFs gain a little.
    for (const auto& [path, e] : files) zip.add("OEBPS/" + path, e.bytes, e.ext != "jpg");
    r.bytes = zip.finish();
    r.ok = true;
    return r;
}

EpubResult writeEpub(const typeset::Source& src, const std::string& path, const EpubOptions& options) {
    EpubResult r = buildEpub(src, options);
    if (!r.ok) return r;
    std::string err;
    if (!atomicWrite(path, r.bytes, err)) { r.ok = false; r.error = err; }
    return r;
}

// ----------------------------------------------------------------- editions

std::string editionLabel(const Edition& e) {
    if (!e.name.empty()) return e.name;
    std::string l = editionKindLabel(e.kind);
    for (auto& c : l) c = (char)std::tolower((unsigned char)c);
    return l;
}

typeset::Source editionSource(const Library& lib, const Edition& e) {
    typeset::Source s = typeset::snapshot(lib, e.bookId);
    s.isbn = e.isbn;
    s.editionKind = e.kind;
    if (e.print() && !e.trim.empty()) s.trim = e.trim;   // snapshot() has already listed every edition's ISBN
    return s;
}

typeset::Design editionDesign(const Library& lib, const Edition& e) {
    typeset::Design d = e.design.empty() ? typeset::designFromJson(lib.setting("typeset:" + e.bookId)) : typeset::preset(e.design).design;
    if (!e.trim.empty()) d.trim = e.trim;
    else if (!e.design.empty()) d.trim.clear();   // a preset follows the book's trim
    return d;
}

// ------------------------------------------------------------------- checks

std::string isbnDigits(const std::string& isbn) {
    std::string d;
    for (char c : isbn)
        if (std::isdigit((unsigned char)c)) d += c;
        else if ((c == 'X' || c == 'x')) d += 'X';
    return d;
}

bool validIsbn(const std::string& isbn) {
    const std::string d = isbnDigits(isbn);
    for (char c : isbn)
        if (!std::isdigit((unsigned char)c) && c != '-' && c != ' ' && c != 'X' && c != 'x') return false;
    if (d.size() == 13) {
        if (d.find('X') != std::string::npos || (d.rfind("978", 0) != 0 && d.rfind("979", 0) != 0)) return false;
        int sum = 0;
        for (int i = 0; i < 12; ++i) sum += (d[i] - '0') * (i % 2 ? 3 : 1);
        return (10 - sum % 10) % 10 == d[12] - '0';
    }
    if (d.size() == 10) {
        if (d.find('X') < 9) return false;
        int sum = 0;
        for (int i = 0; i < 10; ++i) sum += (d[i] == 'X' ? 10 : d[i] - '0') * (10 - i);
        return sum % 11 == 0;
    }
    return false;
}

double spineInches(int pages, Paper paper) {
    // Typical print-on-demand paper: white about 0.0022in a page, cream about 0.0025in.
    return std::max(0, pages) * (paper == Paper::Cream ? 0.0025 : 0.002252);
}

std::vector<Check> preflight(const typeset::Source& src, const typeset::Book* printed) {
    std::vector<Check> out;
    auto add = [&](Check::Level l, const char* area, std::string title, std::string detail) {
        out.push_back(Check{l, area, std::move(title), std::move(detail)});
    };
    const bool ebook = src.editionKind == "ebook";
    const bool printEdition = !src.editionKind.empty() && !ebook;
    // Editions can't share an ISBN.
    {
        std::map<std::string, int> seen;
        for (auto& [label, i] : src.isbns) ++seen[isbnDigits(i)];
        for (auto& [d, n] : seen)
            if (n > 1 && !d.empty()) {
                add(Check::Problem, "Details", "Two editions have the same ISBN", "Each edition needs its own ISBN. Change one in the edition's details.");
                break;
            }
    }

    // Details.
    if (src.title.empty()) add(Check::Problem, "Details", "The book has no title", "Add one in the book's details.");
    if (src.author.empty()) add(Check::Problem, "Details", "No author name", "Add the author (or pen name) in the book's details.");
    if (src.isbn.empty())
        add(Check::Advice, "Details", src.editionKind.empty() ? "No ISBN" : "This edition has no ISBN",
            std::string("Stores and printers can give you one, or you can buy your own. Each edition needs its own ISBN.") +
                (printEdition ? "" : " Until then the EPUB uses its own identifier."));
    else if (!validIsbn(src.isbn))
        add(Check::Problem, "Details", "The ISBN doesn't look right", "Its check digit doesn't match. Check it against the one you were given.");
    else add(Check::Ok, "Details", "ISBN " + src.isbn, "");
    if (src.language.empty()) add(Check::Advice, "Details", "No language set", "The ebook will say English (UK).");
    if (src.synopsis.empty() && !printEdition) add(Check::Advice, "Ebook", "No description", "Stores show it on the book's page. Add a synopsis in the book's details.");

    // Manuscript.
    int empty = 0;
    std::int64_t words = 0;
    for (const auto& s : src.sections) {
        words += s.words;
        if (s.kind != NodeKind::Part && s.words == 0) ++empty;
    }
    if (src.sections.empty()) add(Check::Problem, "Manuscript", "The manuscript is empty", "Write or import the book first.");
    else if (empty) add(Check::Advice, "Manuscript", std::to_string(empty) + (empty == 1 ? " chapter has no text" : " chapters have no text"),
                        "Empty chapters still get a heading in print and in the ebook.");

    // Cover (the ebook's: a print cover is a separate file made to the printer's template).
    if (printEdition) {
    } else if (src.coverPath.empty())
        add(Check::Advice, "Ebook", "No cover", src.editionKind == "ebook" ? "Ebook stores need a cover. Add one in the book's details." : "Ebook stores need a cover. The print cover is made separately, using the spine width below.");
    else if (const ImageInfo c = imageInfo(readAll(src.coverPath)); c.ok) {
        if (c.height < 1600)
            add(Check::Advice, "Ebook", "The cover is small (" + std::to_string(c.width) + " x " + std::to_string(c.height) + " pixels)",
                "Ebook stores ask for at least 1600 pixels tall; 2560 is better.");
        else add(Check::Ok, "Ebook", "Cover " + std::to_string(c.width) + " x " + std::to_string(c.height) + " pixels", "");
    } else add(Check::Problem, "Ebook", "The cover can't be read", "Choose the cover picture again.");

    // Pictures: descriptions.
    int noAlt = 0, total = 0;
    for (const auto& s : src.sections) {
        size_t at = 0;
        while ((at = s.content.find("\"type\":\"image\"", at)) != std::string::npos) {
            ++total;
            const size_t end = s.content.find('}', at);
            const std::string attrs = s.content.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (attrs.find("\"alt\":\"") == std::string::npos || attrs.find("\"alt\":\"\"") != std::string::npos) ++noAlt;
            at += 14;
        }
    }
    if (noAlt)
        add(Check::Advice, "Pictures", std::to_string(noAlt) + (noAlt == 1 ? " picture has no description" : " pictures have no description"),
            "Screen readers read the description aloud. Add one in the picture's panel.");

    // Print.
    if (printed && printed->ok) {
        const int pages = (int)printed->pages.size();
        // Print resolution of every placed picture.
        int soft = 0, blurred = 0;
        for (const auto& p : printed->pages)
            for (const auto& pic : p.pictures) {
                const auto sz = src.pictureSizes.find(pic.blob);
                if (sz == src.pictureSizes.end() || pic.w <= 0) continue;
                const double ppi = sz->second.first * pic.crop.cropW / (pic.w / 72.0);
                if (ppi < 150) ++blurred;
                else if (ppi < 300) ++soft;
            }
        if (blurred)
            add(Check::Problem, "Pictures", std::to_string(blurred) + (blurred == 1 ? " picture will print blurred" : " pictures will print blurred"),
                "Under 150 pixels per inch at the size it prints. Use a larger file, or print it smaller.");
        if (soft)
            add(Check::Advice, "Pictures", std::to_string(soft) + (soft == 1 ? " picture may print soft" : " pictures may print soft"),
                "Under 300 pixels per inch at the size it prints.");
        if (total && !blurred && !soft) add(Check::Ok, "Pictures", "Every picture prints at 300 pixels per inch or more", "");
        if (pages < 24)
            add(Check::Problem, "Print", std::to_string(pages) + " pages", "Most print-on-demand services need at least 24 pages for a printed book.");
        else add(Check::Ok, "Print", std::to_string(pages) + " pages" + (pages % 2 ? " (the printer adds a blank page at the end)" : ""), "");
        if (pages > 0 && pages < 100)
            add(Check::Advice, "Print", "Too thin for text on the spine", "Printers usually allow spine text from about 100 pages.");
        for (const std::string& w : printed->warnings) add(Check::Advice, "Print", w, "");
        add(Check::Ok, "Print", "Fonts embedded", "Only the letters used are included, as printers require.");
    } else if (printed) {
        add(Check::Problem, "Print", "The book couldn't be typeset", printed->error);
    }
    (void)words;
    return out;
}

}  // namespace studio::publish

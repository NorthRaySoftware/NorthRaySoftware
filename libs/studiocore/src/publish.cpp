// Publishing: EPUB 3 packaging, pre-flight checks, spine width.
#include "studio/publish.hpp"
#include "studio/document.hpp"
#include "studio/fonts.hpp"

#include "studio/ebook.hpp"
#include "studio/library.hpp"

#include "internal.hpp"
#include "miniz.h"
#include <nlohmann/json.hpp>
#include "studio/artwork.hpp"
#include "studio/headers.hpp"
#include "studio/calendar.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
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

// A font's embedding permission (OpenType OS/2 fsType): false when its licence forbids putting it in a
// document (restricted licence) or allows bitmaps only.
bool fontMayBeEmbedded(const std::string& ttf) {
    auto u16 = [&](size_t at) { return at + 2 <= ttf.size() ? ((unsigned)(unsigned char)ttf[at] << 8) | (unsigned char)ttf[at + 1] : 0u; };
    auto u32 = [&](size_t at) { return (u16(at) << 16) | u16(at + 2); };
    if (ttf.size() < 12) return false;
    const unsigned tables = u16(4);
    for (unsigned i = 0; i < tables; ++i) {
        const size_t rec = 12 + (size_t)i * 16;
        if (rec + 16 > ttf.size()) return false;
        if (ttf.compare(rec, 4, "OS/2") == 0) {
            const unsigned fsType = u16(u32(rec + 8) + 8);
            return !(fsType & 0x0002) && !(fsType & 0x0200);
        }
    }
    return true;   // no OS/2 table: no restriction stated
}

const char* kEpubCss = R"CSS(
/* EPUB pages */
body { margin: 0 4%; }
body.cover { margin: 0; padding: 0; text-align: center; }
div.cover { height: 100%; height: 100vh; text-align: center; page-break-inside: avoid; }
div.cover img { max-width: 100%; max-height: 100%; height: auto; }
div.cover svg { display: block; width: 100%; height: 100%; }
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

    // The typeface: inside the ebook when its licence allows (the reader can still choose their own).
    ebook::Style style = options.style;
    std::vector<std::pair<std::string, std::string>> fontFiles;   // href -> bytes
    std::string fontCss, fontName, fontLicence, licenceFile = "OFL.txt";
    std::vector<std::pair<std::string, std::string>> worldLicences;   // href -> text
    bool ofl = true;
    if (!options.fontsFolder.empty() && !options.fontId.empty()) {
        namespace fs = std::filesystem;
        // The first of the font folders ("app|world") that has the family.
        const fs::path dir = detail::u8path(studio::fonts::familyFolder(options.fontsFolder, options.fontId));
        struct Want { const char* suffix; const char* weight; const char* styleName; };
        bool refused = false;
        for (const Want& w : {Want{"Regular", "normal", "normal"}, Want{"Italic", "normal", "italic"}, Want{"Bold", "bold", "normal"},
                              Want{"BoldItalic", "bold", "italic"}}) {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(dir, ec)) {
                const std::string n = detail::fromPath(e.path().filename());
                const std::string want = std::string("-") + w.suffix + ".ttf";
                if (n.size() <= want.size() || n.compare(n.size() - want.size(), want.size(), want) != 0) continue;
                const std::string bytes = readAll(detail::fromPath(e.path()));
                if (bytes.empty()) continue;
                if (!fontMayBeEmbedded(bytes)) { refused = true; break; }
                const std::string href = "fonts/" + n;
                fontFiles.push_back({href, bytes});
                fontCss += std::string("@font-face { font-family: \"NR Book\"; font-weight: ") + w.weight + "; font-style: " + w.styleName +
                           "; src: url(\"" + href + "\"); }\n";
            }
            if (refused) break;
        }
        if (refused) {
            fontFiles.clear();
            fontCss.clear();
            r.notes.push_back("The book's typeface doesn't allow being put inside an ebook, so readers see their own.");
        } else if (!fontFiles.empty()) {
            style.fontFamily = "NR Book";
            for (const typeset::FontFamilyInfo& f : typeset::fontFamilies(options.fontsFolder)) if (f.id == options.fontId) fontName = f.name;
            fontLicence = readAll(detail::fromPath(dir / "OFL.txt"));
            if (fontLicence.empty()) {
                fontLicence = readAll(detail::fromPath(dir / "LICENSE.txt"));
                licenceFile = "LICENSE.txt";
                ofl = false;
            }
        }
    }

    // World typefaces set on words (runes, handwriting: 0.41): each inside the ebook when its licence allows, under
    // fonts/<id>/, with its licence file; otherwise the words show in the reader's font.
    if (!options.fontsFolder.empty()) {
        for (const std::string& id : ebook::fontsUsed(src)) {
            std::vector<std::pair<std::string, std::string>> mine;
            bool refused = false;
            const std::string css = ebook::worldFontCss(options.fontsFolder, id, [&](const std::string& path) {
                const std::string bytes = readAll(path);
                if (bytes.empty() || !fontMayBeEmbedded(bytes)) { refused = true; return std::string(); }
                const std::string href = "fonts/" + id + "/" + detail::fromPath(detail::u8path(path).filename());
                mine.push_back({href, bytes});
                return href;
            });
            if (css.empty()) {
                r.notes.push_back("Words are marked for the typeface \"" + id + "\", which isn't in this world's fonts, so they show in the reader's font.");
                continue;
            }
            if (refused) {
                r.notes.push_back("The typeface \"" + id + "\" doesn't allow being put inside an ebook, so its words show in the reader's font.");
                continue;
            }
            fontCss += css;
            for (auto& f : mine) fontFiles.push_back(f);
            const std::string dir = studio::fonts::familyFolder(options.fontsFolder, id);
            for (const char* lic : {"OFL.txt", "LICENSE.txt"}) {
                const std::string text = readAll(detail::fromPath(detail::u8path(dir) / lic));
                if (!text.empty()) { worldLicences.push_back({"fonts/" + id + "/" + lic, text}); break; }
            }
        }
    }

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
    }, style, [&](const std::string& blob, int tone) -> std::string {
        // A header picture under its title (0.89): darkened or faded into a file of its own, on white.
        anyPicture = true;
        const std::string key = blob + "#tone" + std::to_string(tone);
        if (auto it = pics.find(key); it != pics.end()) return it->second.file;
        Pic pic;
        auto d = decoded.find(blob);
        if (d == decoded.end()) {
            const auto path = src.picturePaths.find(blob);
            d = decoded.emplace(blob, decodeRgba(path == src.picturePaths.end() ? std::string() : readAll(path->second))).first;
        }
        const std::string base = "images/h" + blob.substr(0, 12) + "-" + std::to_string(tone);
        if (d->second.w > 0) {
            Rgba t = shrink(d->second, maxSide);
            for (size_t i = 0; i + 3 < t.px.size(); i += 4) {
                const unsigned a = t.px[i + 3];
                for (int c = 0; c < 3; ++c) {
                    const unsigned v = (t.px[i + c] * a + 255 * (255 - a)) / 255;
                    t.px[i + c] = (std::uint8_t)(tone == 1 ? v * 0.42 : 255 - (255 - v) * 0.3);
                }
                t.px[i + 3] = 255;
            }
            const Encoded e = encode(t, true);
            pic.file = base + "." + e.ext;
            pic.mime = e.mime;
            files.push_back({pic.file, e});
            pic.ok = true;
        } else {
            unreadable.insert(blob);
            pic.file = base + ".missing";
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
    int coverW = 0, coverH = 0;
    if (!src.coverPath.empty()) {
        const std::string bytes = readAll(src.coverPath);
        const ImageInfo info = imageInfo(bytes);
        if (info.ok && mimeOf(info.format) && info.format != "gif") {
            cover = Encoded{bytes, extOf(info.format), mimeOf(info.format)};
            coverW = info.width;
            coverH = info.height;
        } else if (const Rgba px = decodeRgba(bytes); px.w > 0) {
            const Rgba small = shrink(px, 2560);
            cover = encode(small, true);
            coverW = small.w;
            coverH = small.h;
        }
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
        // The cover fills the screen, whatever its shape: an SVG keeps the picture's proportions (the way
        // the stores' own cover pages do), with the picture as a fallback description.
        const std::string href = "images/cover." + cover.ext, alt = escape("The cover of " + title);
        std::string body;
        if (coverW > 0 && coverH > 0) {
            const std::string vw = std::to_string(coverW), vh = std::to_string(coverH);
            body = "<div class=\"cover\"><svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.1\" "
                   "width=\"100%\" height=\"100%\" viewBox=\"0 0 " + vw + " " + vh + "\" preserveAspectRatio=\"xMidYMid meet\" role=\"img\" "
                   "aria-label=\"" + alt + "\"><title>" + alt + "</title><image width=\"" + vw + "\" height=\"" + vh + "\" xlink:href=\"" + href +
                   "\"/></svg></div>\n";
        } else body = "<div class=\"cover\"><img src=\"" + href + "\" alt=\"" + alt + "\"/></div>\n";
        docs.push_back({"cover.xhtml", xhtml(lang, title, body, "cover")});
        items.push_back({"cover", "cover.xhtml", "application/xhtml+xml", coverW > 0 ? "svg" : "", true});
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
        if (!fontName.empty())
            b += "<p>Set in " + escape(fontName) + (fontLicence.empty() ? std::string(".") : ofl ? std::string(", under the SIL Open Font License (inside this ebook).") : std::string(" (its licence is inside this ebook).")) + "</p>\n";
        b += "</section>\n";
        docs.push_back({"copyright.xhtml", xhtml(lang, "Copyright", b)});
        items.push_back({"copyright", "copyright.xhtml", "application/xhtml+xml", "", true});
    }
    items.push_back({"nav", "nav.xhtml", "application/xhtml+xml", "nav", true});

    // Chapters, and the contents (parts hold the chapters after them).
    std::string navList, ncxPoints;
    int play = 1;
    std::string sceneText = escape(style.sceneBreak.empty() ? std::string("* * *") : style.sceneBreak);
    replaceAll(sceneText, " ", "&#160;&#160;");
    const std::string sceneDiv = "<div class=\"scene\" role=\"separator\" aria-label=\"Scene break\">" + sceneText + "</div>";
    bool inPart = false;
    std::string firstChapter;
    for (size_t i = 0; i < content.chapters.size(); ++i) {
        const ebook::Chapter& ch = content.chapters[i];
        char name[32];
        std::snprintf(name, sizeof name, "chapter-%03d.xhtml", (int)i + 1);
        std::string html = ch.html;
        replaceAll(html, "<section class=\"chapter\">", "<section class=\"chapter\" epub:type=\"chapter\" role=\"doc-chapter\">");
        replaceAll(html, "<section class=\"part\">", "<section class=\"part\" epub:type=\"part\" role=\"doc-part\">");
        replaceAll(html, "<hr class=\"scene\"/>", sceneDiv);
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
    int fontNo = 0;
    for (const auto& f : fontFiles) manifest += "<item id=\"font" + std::to_string(++fontNo) + "\" href=\"" + f.first + "\" media-type=\"font/ttf\"/>\n";
    if (!fontLicence.empty()) manifest += "<item id=\"fontlicence\" href=\"fonts/" + licenceFile + "\" media-type=\"text/plain\"/>\n";
    for (size_t i = 0; i < worldLicences.size(); ++i)
        manifest += "<item id=\"fontlicence" + std::to_string(i + 2) + "\" href=\"" + worldLicences[i].first + "\" media-type=\"text/plain\"/>\n";
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
    zip.add("OEBPS/style.css", fontCss + ebook::stylesheet(style) + kEpubCss);
    for (const auto& [href, bytes] : fontFiles) zip.add("OEBPS/" + href, bytes, true);
    if (!fontLicence.empty()) zip.add("OEBPS/fonts/" + licenceFile, fontLicence);
    for (const auto& [href, text] : worldLicences) zip.add("OEBPS/" + href, text);
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

// ------------------------------------------------------------------ Word (0.104)

namespace {

std::string xe(const std::string& s) {   // XML text and attribute values
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;   // not allowed in XML 1.0
        else o += (char)c;
    }
    return o;
}

const char* kW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
const char* kR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

struct Docx {
    const typeset::Source& src;
    const DocxOptions& opt;
    DocxResult& res;
    std::string body;
    std::vector<std::string> rels;                        // relationship elements for document.xml.rels
    std::vector<std::pair<std::string, std::string>> media; // zip path -> bytes
    std::map<std::string, std::string> picRel;            // picture key -> rId
    std::map<std::string, std::pair<int, int>> picSize;   // rId -> pixels
    int nextRel = 10, nextPic = 1;
    double textWidthIn = 4.55;

    Docx(const typeset::Source& s, const DocxOptions& o, DocxResult& r) : src(s), opt(o), res(r) {}

    std::string rel(const std::string& type, const std::string& target, bool external = false) {
        const std::string id = "rId" + std::to_string(nextRel++);
        rels.push_back("<Relationship Id=\"" + id + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/" + type +
                       "\" Target=\"" + xe(target) + "\"" + (external ? " TargetMode=\"External\"" : "") + "/>");
        return id;
    }
    // A picture's relationship: stored once per key (a blob and its crop, or an icon).
    std::string addPicture(const std::string& key, const Encoded& e, int w, int h) {
        if (auto it = picRel.find(key); it != picRel.end()) return it->second;
        const std::string name = "media/image" + std::to_string(media.size() + 1) + "." + e.ext;
        media.push_back({"word/" + name, e.bytes});
        const std::string id = rel("image", name);
        picRel[key] = id;
        picSize[id] = {w, h};
        ++res.pictures;
        return id;
    }
    // A blob, cropped, as a picture ("" when it can't be read).
    std::string blobPicture(const std::string& blob, const Placement& p, int& w, int& h) {
        char key[160];
        std::snprintf(key, sizeof key, "%s:%.4f:%.4f:%.4f:%.4f", blob.c_str(), p.cropX, p.cropY, p.cropW, p.cropH);
        if (auto it = picRel.find(key); it != picRel.end()) { w = picSize[it->second].first; h = picSize[it->second].second; return it->second; }
        const auto path = src.picturePaths.find(blob);
        const std::string bytes = path == src.picturePaths.end() ? std::string() : readAll(path->second);
        Rgba d = decodeRgba(bytes);
        if (d.w <= 0) { res.notes.push_back("A picture couldn't be read and was left out."); return {}; }
        const bool full = p.cropX <= 0 && p.cropY <= 0 && p.cropW >= 1 && p.cropH >= 1;
        if (!full) d = cropTo(d, p);
        d = shrink(d, 3000);
        const ImageInfo info = imageInfo(bytes);
        Encoded e;
        if (full && (info.format == "jpeg" || info.format == "png") && std::max(info.width, info.height) <= 3000) {
            e.bytes = bytes;
            e.ext = info.format == "jpeg" ? "jpeg" : "png";
        } else {
            e = encode(d, info.format == "jpeg");
            if (e.ext == "jpg") e.ext = "jpeg";
        }
        w = d.w;
        h = d.h;
        return addPicture(key, e, d.w, d.h);
    }
    // An inline picture run, `wIn` inches wide (its height from its shape), described for screen readers by `alt`.
    std::string drawing(const std::string& rid, double wIn, double hIn, const std::string& alt) {
        const long long cx = (long long)std::llround(wIn * 914400), cy = (long long)std::llround(hIn * 914400);
        const std::string n = std::to_string(nextPic++);
        return "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"" + std::to_string(cx) + "\" cy=\"" +
               std::to_string(cy) + "\"/><wp:docPr id=\"" + n + "\" name=\"Picture " + n + "\" descr=\"" + xe(alt) +
               "\"/><wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr><a:graphic><a:graphicData "
               "uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic><pic:nvPicPr><pic:cNvPr id=\"" + n +
               "\" name=\"Picture " + n + "\" descr=\"" + xe(alt) + "\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\"" + rid +
               "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" + std::to_string(cx) +
               "\" cy=\"" + std::to_string(cy) + "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData>"
               "</a:graphic></wp:inline></w:drawing></w:r>";
    }
    static std::string run(const std::string& text, const std::string& rpr = {}) {
        if (text.empty()) return {};
        return "<w:r>" + (rpr.empty() ? std::string() : "<w:rPr>" + rpr + "</w:rPr>") + "<w:t xml:space=\"preserve\">" + xe(text) + "</w:t></w:r>";
    }
    void para(const std::string& style, const std::string& runs, const std::string& extra = {}) {
        body += "<w:p><w:pPr><w:pStyle w:val=\"" + style + "\"/>" + extra + "</w:pPr>" + runs + "</w:p>\n";
    }
    void pageBreak() { body += "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>\n"; }

    // The text of a paragraph: runs with their emphasis, line breaks and links.
    std::string inlines(const nlohmann::json& content) {
        std::string out;
        if (!content.is_array()) return out;
        for (const auto& n : content) {
            const std::string t = n.value("type", "");
            if (t == "hard_break") { out += "<w:r><w:br/></w:r>"; continue; }
            if (t != "text") continue;
            std::string rpr, href;
            if (n.contains("marks") && n["marks"].is_array())
                for (const auto& m : n["marks"]) {
                    const std::string mt = m.value("type", "");
                    if (mt == "strong") rpr += "<w:b/>";
                    else if (mt == "em") rpr += "<w:i/>";
                    else if (mt == "smallcaps") rpr += "<w:smallCaps/>";
                    else if (mt == "strike") rpr += "<w:strike/>";
                    else if (mt == "underline") rpr += "<w:u w:val=\"single\"/>";
                    else if (mt == "sup") rpr += "<w:vertAlign w:val=\"superscript\"/>";
                    else if (mt == "sub") rpr += "<w:vertAlign w:val=\"subscript\"/>";
                    else if (mt == "code") rpr += "<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\"/>";
                    else if (mt == "link") {
                        const std::string h = m.contains("attrs") && m["attrs"].is_object() ? m["attrs"].value("href", "") : "";
                        if (h.rfind("https://", 0) == 0 || h.rfind("http://", 0) == 0 || h.rfind("mailto:", 0) == 0) href = h;
                    }
                }
            // Word wants its run properties in schema order: fonts, bold, italic, caps, strike, underline, vertical alignment.
            std::string ordered;
            for (const char* tag : {"<w:rFonts", "<w:b/>", "<w:i/>", "<w:smallCaps/>", "<w:strike/>", "<w:u ", "<w:vertAlign"}) {
                const size_t at = rpr.find(tag);
                if (at == std::string::npos) continue;
                ordered += rpr.substr(at, rpr.find("/>", at) + 2 - at);
            }
            const std::string r = run(n.value("text", ""), href.empty() ? ordered : "<w:rStyle w:val=\"Hyperlink\"/>" + ordered);
            if (!href.empty()) out += "<w:hyperlink r:id=\"" + rel("hyperlink", href, true) + "\">" + r + "</w:hyperlink>";
            else out += r;
        }
        return out;
    }

    void sceneBreak() {
        const typeset::Design& d = opt.design;
        if (!d.sceneBreakBlob.empty()) {
            int w = 0, h = 0;
            const std::string rid = blobPicture(d.sceneBreakBlob, Placement{}, w, h);
            if (!rid.empty() && w > 0) {
                const double wi = textWidthIn * 0.2;
                para("SceneBreak", drawing(rid, wi, wi * h / w, "Scene break"));
                return;
            }
        }
        para("SceneBreak", run(d.sceneBreak));
    }

    // The manuscript's blocks. `first`: the next body paragraph opens (no indent).
    void blocks(const nlohmann::json& content, bool& first, int depth, const std::string& quoteStyle = {}) {
        if (!content.is_array() || depth > 6) return;
        for (const auto& n : content) {
            const std::string t = n.value("type", "");
            const nlohmann::json attrs = n.value("attrs", nlohmann::json::object());
            if (t == "paragraph") {
                std::string st = attrs.value("style", "body");
                static const std::map<std::string, std::string> styles = {{"body", "BodyText"}, {"first", "FirstParagraph"},
                    {"centered", "Centered"}, {"epigraph", "Epigraph"}, {"attribution", "Attribution"}, {"verse", "Verse"},
                    {"letter", "Letter"}, {"note", "Note"}};
                auto it = styles.find(st);
                std::string style = it == styles.end() ? "BodyText" : it->second;
                if (style == "BodyText" && first) style = "FirstParagraph";
                if (!quoteStyle.empty()) style = quoteStyle;
                const std::string r = inlines(n.value("content", nlohmann::json::array()));
                para(style, r);
                if (!r.empty()) first = false;
            } else if (t == "heading") {
                const int level = std::clamp(attrs.value("level", 2), 1, 3) + 1;
                para("Heading" + std::to_string(level), inlines(n.value("content", nlohmann::json::array())));
                first = true;
            } else if (t == "blockquote") {
                bool f = true;
                blocks(n.value("content", nlohmann::json::array()), f, depth + 1, "Quote");
                first = true;
            } else if (t == "bullet_list" || t == "ordered_list") {
                int no = attrs.value("start", 1);
                for (const auto& li : n.value("content", nlohmann::json::array())) {
                    bool lead = true;
                    for (const auto& p : li.value("content", nlohmann::json::array())) {
                        if (p.value("type", "") != "paragraph") { bool f = true; blocks(nlohmann::json::array({p}), f, depth + 1); continue; }
                        const std::string mark = !lead ? "\t" : t == "ordered_list" ? std::to_string(no) + ".\t" : "•\t";
                        para("ListParagraph", run(mark) + inlines(p.value("content", nlohmann::json::array())));
                        lead = false;
                    }
                    ++no;
                }
                first = true;
            } else if (t == "scene_break") {
                sceneBreak();
                first = true;
            } else if (t == "image") {
                const std::string blob = attrs.value("blob", "");
                if (blob.size() != 64) continue;
                Placement p;
                p.cropX = attrs.value("cropX", 0.0); p.cropY = attrs.value("cropY", 0.0);
                p.cropW = attrs.value("cropW", 1.0); p.cropH = attrs.value("cropH", 1.0);
                p.width = attrs.value("width", 1.0);
                p.align = attrs.value("align", "center");
                p = clampPlacement(p);
                int w = 0, h = 0;
                const std::string rid = blobPicture(blob, p, w, h);
                if (rid.empty() || w <= 0) continue;
                const double wi = textWidthIn * p.width;
                const std::string jc = p.align == "left" ? "left" : p.align == "right" ? "right" : "center";
                para("Picture", drawing(rid, wi, wi * h / w, attrs.value("alt", "")), "<w:jc w:val=\"" + jc + "\"/>");
                if (const std::string cap = attrs.value("caption", ""); !cap.empty()) para("Caption", run(cap));
                first = true;
            }
        }
    }

    // The viewpoint's icon as a picture run (rendered as the typeset book draws it), or "" (a note when its symbol is in no font).
    std::map<std::string, std::string> iconRel;
    std::set<std::string> noted;
    std::string icon(const typeset::Source::Section& sec, float diamPt, const std::string& alt) {
        const std::string key = sec.povId + "@" + std::to_string((int)std::lround(diamPt * 10));
        std::string rid;
        if (auto it = iconRel.find(key); it != iconRel.end()) rid = it->second;
        else {
            bool missing = false;
            const typeset::Bitmap bm = typeset::renderViewpointIcon(src, sec, opt.design, opt.resources, diamPt, 300, &missing);
            if (bm.width <= 0) {
                if (missing && noted.insert(sec.povId).second)
                    res.notes.push_back("The viewpoint symbol of " + sec.pov.name + " (" + sec.pov.symbol +
                                        ") isn't in any of the book's fonts, so it's left out rather than showing as an empty box.");
                iconRel[key] = "";
                return {};
            }
            Rgba r;
            r.w = bm.width;
            r.h = bm.height;
            r.px = bm.rgba;
            Encoded e = encode(r, false);
            rid = addPicture("icon:" + key, e, r.w, r.h);
            iconRel[key] = rid;
        }
        if (rid.empty()) return {};
        const auto [w, h] = picSize[rid];
        const double wi = w / 300.0;
        return drawing(rid, wi, wi * h / std::max(1, w), alt);
    }
};

std::string docxStyles(const typeset::Design& d, const std::string& font, const std::string& lang) {
    const int sz = (int)std::lround(d.fontSize * 2);   // half-points
    const int line = (int)std::lround(d.leading * 20);  // twips
    const int indent = (int)std::lround(d.indentEm * d.fontSize * 20);
    const std::string jc = d.justify ? "both" : "left";
    auto style = [&](const std::string& id, const std::string& name, const std::string& ppr, const std::string& rpr, const std::string& based = "Normal",
                     const std::string& extra = {}) {
        return "<w:style w:type=\"paragraph\" w:customStyle=\"1\" w:styleId=\"" + id + "\"><w:name w:val=\"" + name + "\"/><w:basedOn w:val=\"" + based +
               "\"/>" + extra + "<w:qFormat/><w:pPr>" + ppr + "</w:pPr><w:rPr>" + rpr + "</w:rPr></w:style>";
    };
    auto heading = [&](const std::string& id, const std::string& name, int outline, const std::string& ppr, const std::string& rpr) {
        return "<w:style w:type=\"paragraph\" w:styleId=\"" + id + "\"><w:name w:val=\"" + name + "\"/><w:basedOn w:val=\"Normal\"/>"
               "<w:next w:val=\"FirstParagraph\"/><w:qFormat/><w:pPr><w:keepNext/>" + ppr + "<w:outlineLvl w:val=\"" + std::to_string(outline) +
               "\"/></w:pPr><w:rPr>" + rpr + "</w:rPr></w:style>";
    };
    const std::string centred = "<w:spacing w:before=\"0\" w:after=\"0\"/><w:ind w:firstLine=\"0\"/><w:jc w:val=\"center\"/>";
    std::string s = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:styles xmlns:w=\"") + kW + "\">";
    s += "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"" + xe(font) + "\" w:hAnsi=\"" + xe(font) + "\" w:eastAsia=\"" + xe(font) +
         "\" w:cs=\"" + xe(font) + "\"/><w:sz w:val=\"" + std::to_string(sz) + "\"/><w:szCs w:val=\"" + std::to_string(sz) + "\"/><w:lang w:val=\"" +
         xe(lang) + "\"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:widowControl/><w:spacing w:before=\"0\" w:after=\"0\" w:line=\"" +
         std::to_string(line) + "\" w:lineRule=\"exact\"/></w:pPr></w:pPrDefault></w:docDefaults>";
    s += "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/><w:pPr><w:jc w:val=\"" + jc +
         "\"/></w:pPr></w:style>";
    s += style("BodyText", "Body Text", "<w:ind w:firstLine=\"" + std::to_string(indent) + "\"/>", "");
    s += style("FirstParagraph", "First Paragraph", "<w:ind w:firstLine=\"0\"/>", "", "BodyText", "<w:next w:val=\"BodyText\"/>");
    s += style("Centered", "Centred", centred, "");
    s += style("Epigraph", "Epigraph", "<w:ind w:left=\"720\" w:right=\"720\" w:firstLine=\"0\"/><w:jc w:val=\"left\"/>", "<w:i/>");
    s += style("Attribution", "Attribution", "<w:ind w:left=\"720\" w:right=\"720\" w:firstLine=\"0\"/><w:jc w:val=\"right\"/>", "");
    s += style("Verse", "Verse", "<w:ind w:left=\"720\" w:firstLine=\"0\"/><w:jc w:val=\"left\"/>", "");
    s += style("Letter", "Letter", "<w:ind w:left=\"360\" w:right=\"360\" w:firstLine=\"0\"/>", "<w:i/>");
    s += style("Note", "Note", "<w:ind w:firstLine=\"0\"/>", "<w:sz w:val=\"" + std::to_string(sz * 85 / 100) + "\"/>");
    s += style("Quote", "Quote", "<w:ind w:left=\"720\" w:right=\"720\" w:firstLine=\"0\"/>", "");
    s += style("ListParagraph", "List Paragraph", "<w:tabs><w:tab w:val=\"left\" w:pos=\"720\"/></w:tabs><w:ind w:left=\"720\" w:hanging=\"360\"/>", "");
    s += style("SceneBreak", "Scene Break", "<w:spacing w:before=\"" + std::to_string(line / 2) + "\" w:after=\"" + std::to_string(line / 2) +
                                            "\"/><w:ind w:firstLine=\"0\"/><w:jc w:val=\"center\"/>", "");
    s += style("Picture", "Picture", "<w:spacing w:before=\"" + std::to_string(line / 2) + "\" w:after=\"" + std::to_string(line / 2) +
                                     "\" w:line=\"240\" w:lineRule=\"auto\"/><w:ind w:firstLine=\"0\"/><w:jc w:val=\"center\"/>", "");
    s += style("Caption", "Caption", centred, "<w:i/><w:sz w:val=\"" + std::to_string(sz * 85 / 100) + "\"/>");
    s += style("Title", "Title", centred + "<w:spacing w:before=\"2880\" w:after=\"240\" w:line=\"240\" w:lineRule=\"auto\"/>",
               "<w:sz w:val=\"" + std::to_string(sz * 22 / 10) + "\"/>");
    s += style("Subtitle", "Subtitle", centred + "<w:spacing w:after=\"960\"/>", "<w:i/><w:sz w:val=\"" + std::to_string(sz * 12 / 10) + "\"/>");
    s += style("Author", "Author", centred, "<w:smallCaps/><w:sz w:val=\"" + std::to_string(sz * 125 / 100) + "\"/>");
    s += style("Copyright", "Copyright", "<w:ind w:firstLine=\"0\"/><w:jc w:val=\"left\"/><w:spacing w:after=\"120\"/>",
               "<w:sz w:val=\"" + std::to_string(sz * 85 / 100) + "\"/>");
    s += style("ChapterNumber", "Chapter Number", centred + "<w:spacing w:after=\"240\"/><w:keepNext/>", "<w:smallCaps/><w:spacing w:val=\"20\"/>");
    s += style("ChapterNumeral", "Chapter Numeral", centred + "<w:spacing w:line=\"240\" w:lineRule=\"auto\"/><w:keepNext/>",
               "<w:sz w:val=\"" + std::to_string(sz * 3) + "\"/>");
    s += style("ChapterDate", "Chapter Date", centred + "<w:spacing w:before=\"120\"/><w:keepNext/>", "<w:i/>");
    s += style("PovIcon", "Viewpoint Icon", centred + "<w:spacing w:after=\"240\" w:line=\"240\" w:lineRule=\"auto\"/><w:keepNext/>", "");
    s += style("Ornament", "Ornament", centred + "<w:spacing w:after=\"240\" w:line=\"240\" w:lineRule=\"auto\"/><w:keepNext/>",
               "<w:sz w:val=\"" + std::to_string(sz * 24 / 10) + "\"/>");
    s += style("KeyLine", "Viewpoint Key", "<w:ind w:left=\"1440\" w:firstLine=\"0\"/><w:jc w:val=\"left\"/><w:spacing w:after=\"160\" "
                                         "w:line=\"240\" w:lineRule=\"auto\"/>", "");
    s += heading("Heading1", "heading 1", 0, centred + "<w:spacing w:before=\"" + std::to_string(line * 4) + "\" w:after=\"" + std::to_string(line * 2) +
                                             "\" w:line=\"240\" w:lineRule=\"auto\"/>", "<w:sz w:val=\"" + std::to_string(sz * 155 / 100) + "\"/>");
    s += heading("Heading2", "heading 2", 1, "<w:ind w:firstLine=\"0\"/><w:jc w:val=\"center\"/><w:spacing w:before=\"" + std::to_string(line) +
                                             "\" w:after=\"" + std::to_string(line / 2) + "\"/>", "<w:b/>");
    s += heading("Heading3", "heading 3", 2, "<w:ind w:firstLine=\"0\"/><w:spacing w:before=\"" + std::to_string(line / 2) + "\"/>", "<w:b/>");
    s += heading("Heading4", "heading 4", 3, "<w:ind w:firstLine=\"0\"/><w:spacing w:before=\"" + std::to_string(line / 2) + "\"/>", "<w:i/>");
    s += heading("PartTitle", "Part Title", 0, centred + "<w:spacing w:before=\"" + std::to_string(line * 8) + "\" w:line=\"240\" w:lineRule=\"auto\"/>",
                 "<w:smallCaps/><w:sz w:val=\"" + std::to_string(sz * 19 / 10) + "\"/>");
    s += "<w:style w:type=\"paragraph\" w:styleId=\"TOC1\"><w:name w:val=\"toc 1\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:tabs><w:tab "
         "w:val=\"right\" w:leader=\"dot\" w:pos=\"9000\"/></w:tabs><w:ind w:firstLine=\"0\"/><w:jc w:val=\"left\"/></w:pPr></w:style>";
    s += "<w:style w:type=\"paragraph\" w:styleId=\"Footer\"><w:name w:val=\"footer\"/><w:basedOn w:val=\"Normal\"/><w:pPr><w:jc w:val=\"center\"/>"
         "</w:pPr><w:rPr><w:sz w:val=\"" + std::to_string(sz * 85 / 100) + "\"/></w:rPr></w:style>";
    s += "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/><w:rPr><w:u w:val=\"single\"/></w:rPr></w:style>";
    s += "</w:styles>";
    return s;
}

}  // namespace

DocxResult buildDocx(const typeset::Source& src, const DocxOptions& options) {
    DocxResult r;
    if (src.sections.empty()) { r.error = "The manuscript is empty: there's nothing to put in a Word document yet."; return r; }
    const typeset::Design& d = options.design;
    Docx x(src, options, r);
    const TrimSize& trim = trimSize(d.trim.empty() ? src.trim : d.trim);
    x.textWidthIn = std::max(1.0, trim.widthIn - d.marginInside - d.marginOutside);
    std::string font = "Georgia";
    for (const auto& f : typeset::fontFamilies(options.resources.fontsFolder))
        if (f.id == d.font) font = f.name;
    const std::string lang = src.language.empty() ? "en-GB" : src.language;
    const std::string title = src.title.empty() ? std::string("Untitled") : src.title;

    // Title page, copyright page and contents, as the design has them.
    if (d.titlePage) {
        x.para("Title", Docx::run(title));
        if (!src.subtitle.empty()) x.para("Subtitle", Docx::run(src.subtitle));
        if (!src.author.empty()) x.para("Author", Docx::run(src.author));
        x.pageBreak();
        if (d.copyrightPage) {
            std::vector<std::string> notice;
            if (!src.author.empty()) notice.push_back("Copyright © " + std::to_string(src.year) + " " + src.author);
            if (!src.edition.empty()) notice.push_back(src.edition);
            if (!src.isbns.empty()) for (auto& [label, isbn] : src.isbns) notice.push_back("ISBN " + isbn + (label.empty() ? "" : " (" + label + ")"));
            else if (!src.isbn.empty()) notice.push_back("ISBN " + src.isbn);
            if (!notice.empty()) {
                for (int i = 0; i < 14; ++i) x.para("Copyright", "");
                for (const auto& l : notice) x.para("Copyright", Docx::run(l));
                x.pageBreak();
            }
        }
    }
    if (d.contents) {
        // Word's own contents, from the chapter headings: filled in when the document opens (settings: updateFields).
        x.para("Heading1", Docx::run("Contents"), "<w:outlineLvl w:val=\"9\"/>");
        x.body += "<w:p><w:pPr><w:pStyle w:val=\"TOC1\"/></w:pPr><w:r><w:fldChar w:fldCharType=\"begin\" w:dirty=\"true\"/></w:r><w:r><w:instrText "
                  "xml:space=\"preserve\"> TOC \\o \"1-1\" \\h \\z \\u </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>";
        for (const auto& sec : src.sections)
            if (sec.kind != NodeKind::Scene) x.body += Docx::run(sec.title) + "<w:r><w:br/></w:r>";
        x.body += "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>\n";
        x.pageBreak();
    }
    // The key of viewpoints, as the typeset book places it.
    auto key = [&] {
        std::set<std::string> seen;
        std::vector<const typeset::Source::Section*> who;
        for (const auto& sec : src.sections)
            if (sec.kind == NodeKind::Chapter && sec.hasPov && sec.matter == 0 && seen.insert(sec.povId).second) who.push_back(&sec);
        if (who.empty()) return;
        x.para("Heading1", Docx::run("Viewpoints"), "<w:pageBreakBefore/>");
        for (const auto* sec : who) x.para("KeyLine", x.icon(*sec, (float)d.fontSize * 1.7f, sec->pov.name) + Docx::run(" " + sec->pov.name));
    };
    if (d.povIcon && d.povKey == "front") key();

    int chapterNo = 0;
    bool firstSection = true;
    for (const auto& sec : src.sections) {
        const nlohmann::json doc = nlohmann::json::parse(sec.kind == NodeKind::Chapter ? typeset::withOpening(sec.content) : sec.content, nullptr, false);
        const nlohmann::json content = doc.is_object() ? doc.value("content", nlohmann::json::array()) : nlohmann::json::array();
        bool first = true;
        const std::string newPage = firstSection && !d.titlePage && !d.contents ? std::string() : std::string("<w:pageBreakBefore/>");
        firstSection = false;
        if (sec.kind == NodeKind::Scene) {
            x.sceneBreak();
            x.blocks(content, first, 0);
            continue;
        }
        if (sec.kind == NodeKind::Part) {
            x.para("PartTitle", Docx::run(sec.title), newPage);
            x.blocks(content, first, 0);
            continue;
        }
        ++r.chapters;
        const typeset::ChapterTitle t = typeset::chapterTitleParts(sec.title);
        int number = 0;
        if (!t.special && sec.matter == 0) number = t.number ? (chapterNo = t.number) : ++chapterNo;
        const bool pov = d.povIcon && sec.hasPov && sec.matter == 0;
        const std::string told = "Chapter told by " + sec.pov.name;
        // Its header: a picture or an ornament above the heading (the viewpoint's default ornament gives way to the icon).
        std::string headBefore = newPage;
        const bool head = sec.hasHeader && !(pov && sec.headerFrom == "viewpoint" && sec.header.blob.empty());
        if (head && !sec.header.blob.empty()) {
            int w = 0, h = 0;
            Placement p;
            const std::string rid = x.blobPicture(sec.header.blob, p, w, h);
            if (!rid.empty() && w > 0) {
                const double wi = x.textWidthIn * (sec.header.size == "ornament" ? 0.28 : 1.0);
                const double hi = std::min(wi * h / w, sec.header.size == "band" ? x.textWidthIn / 3.2 : 4.0);
                x.para("Picture", x.drawing(rid, hi * w / h < wi ? hi * w / h : wi, hi, sec.header.alt), headBefore);
                headBefore.clear();
            }
        } else if (head && headers::printableSymbol(sec.header.symbol)) {
            x.para("Ornament", Docx::run(sec.header.symbol), headBefore);
            headBefore.clear();
        }
        if (pov && d.povPlace == "above") {
            const std::string ic = x.icon(sec, (float)d.fontSize * 2.6f, told);
            if (!ic.empty()) { x.para("PovIcon", ic, headBefore); headBefore.clear(); }
        }
        auto titled = [&](const std::string& text, float size) {   // the heading's words, with the icon before or beside them
            std::string runs = Docx::run(text);
            if (pov && d.povPlace != "above") {
                const std::string ic = x.icon(sec, size * 1.15f, told);
                if (!ic.empty()) runs = d.povPlace == "before" ? ic + Docx::run(" ") + runs : runs + Docx::run(" ") + ic;
            }
            return runs;
        };
        const float hs = (float)d.fontSize * 1.55f;
        if (d.chapterStyle == "number" && number) {
            const std::string label = "Chapter " + (t.word.empty() ? typeset::spelledNumber(number) : t.word);
            if (t.rest.empty()) x.para("Heading1", titled(label, hs), headBefore);
            else {
                x.para("ChapterNumber", Docx::run(label), headBefore);
                x.para("Heading1", titled(t.rest, hs), "<w:spacing w:before=\"0\"/>");
            }
        } else if (d.chapterStyle == "numeral" && number) {
            x.para("ChapterNumeral", Docx::run(std::to_string(number)), headBefore);
            x.para("Heading1", titled(t.rest.empty() ? sec.title : t.rest, hs), "<w:spacing w:before=\"0\"/>");
        } else
            x.para("Heading1", titled(sec.title, hs), headBefore);
        if (d.chapterDates && !sec.dateLine.empty()) {   // the same line as the printed book (0.105)
            x.para("ChapterDate", Docx::run(sec.dateLine));
            if (!sec.timeLine.empty()) x.para("ChapterDate", Docx::run(sec.timeLine), "<w:spacing w:before=\"0\"/>");
        }
        x.blocks(content, first, 0);
    }
    if (d.povIcon && d.povKey == "back") key();

    // Page size and margins (mirrored, like the book), and page numbers at the foot.
    const std::string footerId = x.rel("footer", "footer1.xml");
    auto tw = [](double inches) { return std::to_string((long long)std::llround(inches * 1440)); };
    x.body += "<w:sectPr><w:footerReference w:type=\"default\" r:id=\"" + footerId + "\"/><w:pgSz w:w=\"" + tw(trim.widthIn) + "\" w:h=\"" +
              tw(trim.heightIn) + "\"/><w:pgMar w:top=\"" + tw(d.marginTop) + "\" w:right=\"" + tw(d.marginOutside) + "\" w:bottom=\"" +
              tw(d.marginBottom) + "\" w:left=\"" + tw(d.marginInside) + "\" w:header=\"" + tw(d.marginTop / 2) + "\" w:footer=\"" +
              tw(d.marginBottom / 2) + "\" w:gutter=\"0\"/>" + (d.titlePage ? "<w:titlePg/>" : "") + "</w:sectPr>";

    const std::string ns = std::string("xmlns:w=\"") + kW + "\" xmlns:r=\"" + kR +
                           "\" xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
                           "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                           "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"";
    const std::string document = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document " + ns + "><w:body>\n" + x.body +
                                 "</w:body></w:document>";
    const std::string footer = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:ftr xmlns:w=\"") + kW +
                               "\"><w:p><w:pPr><w:pStyle w:val=\"Footer\"/></w:pPr><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText "
                               "xml:space=\"preserve\"> PAGE </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>1</w:t></w:r>"
                               "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p></w:ftr>";
    const std::string settings = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:settings xmlns:w=\"") + kW + "\">" +
                                 (d.contents ? "<w:updateFields w:val=\"true\"/>" : "") +
                                 "<w:mirrorMargins/><w:defaultTabStop w:val=\"720\"/><w:compat><w:compatSetting w:name=\"compatibilityMode\" "
                                 "w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"15\"/></w:compat></w:settings>";
    std::string docRels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships "
                          "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                          "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                          "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings\" Target=\"settings.xml\"/>";
    for (const std::string& rl : x.rels) docRels += rl;
    docRels += "</Relationships>";
    std::string modified = options.modified;
    if (modified.empty()) {
        char b[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
        modified = b;
    }
    const std::string core = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<cp:coreProperties "
                             "xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                             "xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><dc:title>" + xe(title) +
                             "</dc:title><dc:creator>" + xe(src.author) + "</dc:creator><dc:language>" + xe(lang) +
                             "</dc:language><dcterms:modified xsi:type=\"dcterms:W3CDTF\">" + modified + "</dcterms:modified></cp:coreProperties>";
    const std::string types = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                              "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                              "<Default Extension=\"xml\" ContentType=\"application/xml\"/><Default Extension=\"png\" ContentType=\"image/png\"/>"
                              "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
                              "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
                              "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
                              "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>"
                              "<Override PartName=\"/word/footer1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml\"/>"
                              "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/></Types>";
    const std::string rootRels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships "
                                 "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
                                 "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
                                 "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" "
                                 "Target=\"docProps/core.xml\"/></Relationships>";
    Zip zip;
    zip.add("[Content_Types].xml", types);
    zip.add("_rels/.rels", rootRels);
    zip.add("docProps/core.xml", core);
    zip.add("word/document.xml", document);
    zip.add("word/styles.xml", docxStyles(d, font, lang));
    zip.add("word/settings.xml", settings);
    zip.add("word/footer1.xml", footer);
    zip.add("word/_rels/document.xml.rels", docRels);
    for (const auto& [path, bytes] : x.media) zip.add(path, bytes, path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0);
    r.bytes = zip.finish();
    r.ok = true;
    return r;
}

DocxResult writeDocx(const typeset::Source& src, const std::string& path, const DocxOptions& options) {
    DocxResult r = buildDocx(src, options);
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
    s.colourPrint = e.print() && e.colour;
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
    // Bracketed notes left in the text (0.94): "[More to go here]", "[check the date]". Draft notes are gone by now
    // (the snapshot leaves them out), so whatever's still in brackets will print.
    {
        int count = 0;
        std::vector<std::string> where;
        std::string example;
        for (const auto& s : src.sections) {
            if (s.kind == NodeKind::Part && s.content.find('[') == std::string::npos) continue;
            const std::string text = doc::plainText(s.content);
            int here = 0;
            for (size_t a = text.find('['); a != std::string::npos; a = text.find('[', a + 1)) {
                const size_t b = text.find(']', a + 1);
                if (b == std::string::npos || b - a < 2 || b - a > 300) continue;
                const std::string inside = text.substr(a + 1, b - a - 1);
                if (inside.find('\n') != std::string::npos || inside.find('[') != std::string::npos) continue;
                ++here;
                if (example.empty()) example = "[" + inside.substr(0, 60) + (inside.size() > 60 ? "…" : "") + "]";
            }
            if (here) {
                count += here;
                if (where.size() < 3) where.push_back(s.title);
            }
        }
        if (count) {
            std::string list;
            for (size_t i = 0; i < where.size(); ++i) list += (i ? (i + 1 == where.size() ? " and " : ", ") : "") + where[i];
            add(Check::Advice, "Manuscript",
                std::to_string(count) + (count == 1 ? " note in square brackets will print" : " notes in square brackets will print"),
                "For example " + example + " in " + list + (where.size() < 3 ? "" : " and elsewhere") +
                    ". If it's a note to yourself, select it in Writing and make it a draft note (right-click › Draft note, or the "
                    "Draft note style for a whole paragraph): draft notes are never printed or exported.");
        }
    }

    // Cover (the ebook's: a print cover is a separate file made to the printer's template).
    if (printEdition) {
    } else if (src.coverPath.empty())
        add(Check::Advice, "Ebook", "No cover", src.editionKind == "ebook" ? "Ebook stores need a cover. Design one under Cover and look, or add a picture in the book's details." : "Ebook stores need a cover. The print cover is made separately, using the spine width below.");
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
    // Chapter header pictures without a description (0.91): the ebook treats them as decoration, which readers skip.
    if (!printEdition) {
        std::vector<std::string> undescribed;
        for (const auto& s : src.sections)
            if (s.hasHeader && !s.header.blob.empty() && s.header.alt.empty()) undescribed.push_back(s.title);
        if (!undescribed.empty()) {
            std::string names;
            for (size_t i = 0; i < undescribed.size() && i < 4; ++i) names += (i ? ", " : "") + std::string("\u201C") + undescribed[i] + "\u201D";
            if (undescribed.size() > 4) names += " and " + std::to_string(undescribed.size() - 4) + " more";
            add(Check::Advice, "Ebook",
                std::to_string(undescribed.size()) + (undescribed.size() == 1 ? " chapter's header picture has no description" : " chapters' header pictures have no description"),
                "In the ebook, readers who can't see them hear nothing: they're treated as decoration. If one shows something the chapter needs, "
                "describe it (Describe it\u2026 under its header picture). " + names + ".");
        }
    }

    // Print.
    if (printed && printed->ok) {
        const int pages = (int)printed->pages.size();
        // Print resolution of every placed picture; header and scene-break pictures by name (0.91).
        int soft = 0, blurred = 0;
        std::set<std::string> named;
        bool sceneBreakDone = false;
        for (const auto& p : printed->pages)
            for (const auto& pic : p.pictures) {
                const auto sz = src.pictureSizes.find(pic.blob);
                if (sz == src.pictureSizes.end() || pic.w <= 0) continue;
                const double ppi = sz->second.first * pic.crop.cropW / (pic.w / 72.0);
                if (pic.role == "header" || pic.role == "scenebreak" || pic.role == "viewpoint") {
                    if (ppi >= 300) continue;
                    const std::string what = pic.role == "header"      ? "The header picture of \u201C" + p.chapter + "\u201D"
                                           : pic.role == "viewpoint" ? "The viewpoint picture of " + pic.about   // (0.103)
                                                                     : std::string("The scene break's picture");
                    if (pic.role == "scenebreak" ? sceneBreakDone : !named.insert(pic.role == "viewpoint" ? "pov:" + pic.about : p.chapter).second) continue;
                    if (pic.role == "scenebreak") sceneBreakDone = true;
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "%d", (int)std::lround(ppi));
                    if (ppi < 150)
                        add(Check::Problem, "Pictures", what + " will print blurred",
                            std::string(buf) + " pixels per inch at the size it prints; 300 is best. Use a larger file, or a smaller size (a band or an ornament).");
                    else
                        add(Check::Advice, "Pictures", what + " may print soft", std::string(buf) + " pixels per inch at the size it prints; 300 is best.");
                    continue;
                }
                if (ppi < 150) ++blurred;
                else if (ppi < 300) ++soft;
            }
        // Header and scene-break pictures in colour, in an edition printed in grey.
        if (printEdition && !src.colourPrint) {
            std::vector<std::string> colourful;
            std::set<std::string> seen;
            for (const auto& p : printed->pages)
                for (const auto& pic : p.pictures) {
                    if ((pic.role != "header" && pic.role != "scenebreak") || !seen.insert(pic.blob).second) continue;
                    const auto path = src.picturePaths.find(pic.blob);
                    if (path == src.picturePaths.end()) continue;
                    const Rgba px = decodeRgba(readAll(path->second));
                    if (px.w <= 0) continue;
                    double spread = 0;
                    size_t n = 0;
                    const size_t step = std::max<size_t>(1, (size_t)px.w * px.h / 20000);
                    for (size_t i = 0; i < (size_t)px.w * px.h; i += step, ++n) {
                        const std::uint8_t* q = &px.px[i * 4];
                        spread += std::max({q[0], q[1], q[2]}) - std::min({q[0], q[1], q[2]});
                    }
                    if (n && spread / n > 24) colourful.push_back(pic.role == "header" ? "the header of \u201C" + p.chapter + "\u201D" : std::string("the scene break"));
                }
            if (!colourful.empty()) {
                std::string names;
                for (size_t i = 0; i < colourful.size() && i < 4; ++i) names += (i ? ", " : "") + colourful[i];
                names[0] = (char)std::toupper((unsigned char)names[0]);
                add(Check::Advice, "Print", "Colour header pictures will print in black and white",
                    "This edition prints pictures in grey, so look at the proof to see they still read well, or choose colour pictures for "
                    "the edition. " + names + (colourful.size() > 4 ? " and more." : "."));
            }
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
        // Viewpoint symbols no font has (0.103): left out of the headings, so they never print as empty boxes.
        for (const std::string& m : printed->missingSymbols)
            add(Check::Problem, "Print", "The viewpoint symbol of " + m + " isn't in any of the book's fonts",
                "It's left out of the chapter headings rather than printing as an empty box. Choose a symbol from the Studio's "
                "ornaments, a picture as the symbol, or another font in Typesetting.");
        add(Check::Ok, "Print", "Fonts embedded", "Only the letters used are included, as printers require.");
    } else if (printed) {
        add(Check::Problem, "Print", "The book couldn't be typeset", printed->error);
    }
    (void)words;
    return out;
}

}  // namespace studio::publish

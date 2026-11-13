// Drawing typeset pages: as pixels (the preview) and as a PDF. Both read the
// same display list (studio::typeset::Book).
#include "typeset_internal.hpp"

#include "internal.hpp"

#include "miniz.h"

#include <hb-subset.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_MAX_DIMENSIONS 20000
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace studio::typeset {

using namespace detail;

namespace {

constexpr std::uint8_t kInk[3] = {0x1E, 0x1D, 0x1A};

std::string readAll(const std::string& path) {
    std::ifstream in(studio::detail::u8path(path), std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A decoded picture, RGB on white.
struct Pixels {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgb;
};
Pixels decode(const std::string& bytes) {
    Pixels p;
    int n = 0;
    unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), (int)bytes.size(), &p.w, &p.h, &n, 4);
    if (!data) return {};
    p.rgb.resize((size_t)p.w * p.h * 3);
    for (size_t i = 0, n4 = (size_t)p.w * p.h; i < n4; ++i) {
        const unsigned a = data[i * 4 + 3];
        for (int c = 0; c < 3; ++c) p.rgb[i * 3 + c] = (std::uint8_t)((data[i * 4 + c] * a + 255 * (255 - a)) / 255);
    }
    stbi_image_free(data);
    return p;
}

}  // namespace

// ----------------------------------------------------------------- preview

Bitmap renderPage(const Book& book, int index, double dpi) {
    Bitmap bm;
    if (index < 0 || index >= (int)book.pages.size()) return bm;
    const double s = dpi / 72.0;
    bm.width = (int)std::lround(book.width * s);
    bm.height = (int)std::lround(book.height * s);
    bm.rgba.assign((size_t)bm.width * bm.height * 4, 255);
    const Page& page = book.pages[index];
    auto blend = [&](int x, int y, unsigned cov) {
        if (x < 0 || y < 0 || x >= bm.width || y >= bm.height || !cov) return;
        std::uint8_t* px = &bm.rgba[((size_t)y * bm.width + x) * 4];
        for (int c = 0; c < 3; ++c) px[c] = (std::uint8_t)((kInk[c] * cov + px[c] * (255 - cov)) / 255);
    };

    // Pictures first, so text over them (captions are below anyway) stays readable.
    for (const PicturePlacement& pic : page.pictures) {
            auto path = book.picturePaths.find(pic.blob);
            const Pixels img = path == book.picturePaths.end() ? Pixels{} : decode(readAll(path->second));
            const int x0 = (int)std::lround(pic.x * s), y0 = (int)std::lround(pic.y * s);
            const int w = std::max(1, (int)std::lround(pic.w * s)), h = std::max(1, (int)std::lround(pic.h * s));
            if (img.w <= 0) {
                for (int y = y0; y < y0 + h; ++y)
                    for (int x = x0; x < x0 + w; ++x)
                        if (x >= 0 && y >= 0 && x < bm.width && y < bm.height) {
                            std::uint8_t* px = &bm.rgba[((size_t)y * bm.width + x) * 4];
                            px[0] = px[1] = px[2] = 0xE6;
                        }
                continue;
            }
            const double sx0 = pic.crop.cropX * img.w, sy0 = pic.crop.cropY * img.h;
            const double sw = pic.crop.cropW * img.w, sh = pic.crop.cropH * img.h;
            for (int y = 0; y < h; ++y) {
                const int ty = y0 + y;
                if (ty < 0 || ty >= bm.height) continue;
                // Area average over the source pixels this pixel covers (sharp when shrinking).
                const double fy0 = sy0 + sh * y / h, fy1 = sy0 + sh * (y + 1) / h;
                for (int x = 0; x < w; ++x) {
                    const int tx = x0 + x;
                    if (tx < 0 || tx >= bm.width) continue;
                    const double fx0 = sx0 + sw * x / w, fx1 = sx0 + sw * (x + 1) / w;
                    const int ax = std::clamp((int)fx0, 0, img.w - 1), bx = std::clamp((int)std::ceil(fx1) - 1, ax, img.w - 1);
                    const int ay = std::clamp((int)fy0, 0, img.h - 1), by = std::clamp((int)std::ceil(fy1) - 1, ay, img.h - 1);
                    unsigned sum[3] = {0, 0, 0}, n = 0;
                    for (int yy = ay; yy <= by; yy += std::max(1, (by - ay) / 4))
                        for (int xx = ax; xx <= bx; xx += std::max(1, (bx - ax) / 4)) {
                            const std::uint8_t* sp = &img.rgb[((size_t)yy * img.w + xx) * 3];
                            sum[0] += sp[0]; sum[1] += sp[1]; sum[2] += sp[2]; ++n;
                        }
                    std::uint8_t* px = &bm.rgba[((size_t)ty * bm.width + tx) * 4];
                    for (int c = 0; c < 3; ++c) px[c] = (std::uint8_t)(sum[c] / n);
                }
            }
        }

    std::lock_guard<std::recursive_mutex> guard(Fonts::get().lock());
    Fonts& fonts = Fonts::get();
    for (const GlyphRun& r : page.runs) {
        const int fi = fonts.open(book.fonts[r.font].path);
        if (fi < 0) continue;
        FT_Face face = fonts.face(fi).ft;
        FT_Set_Char_Size(face, 0, (FT_F26Dot6)std::lround(r.size * 64), (FT_UInt)std::lround(dpi), (FT_UInt)std::lround(dpi));
        for (size_t i = 0; i < r.glyphs.size(); ++i) {
            const double gx = r.xs[i] * s, gy = (r.y + r.dy[i]) * s;
            const double fx = std::floor(gx);
            FT_Vector delta{(FT_Pos)std::lround((gx - fx) * 64), 0};
            FT_Set_Transform(face, nullptr, &delta);
            if (FT_Load_Glyph(face, r.glyphs[i], FT_LOAD_NO_HINTING | FT_LOAD_RENDER) != 0) continue;
            const FT_Bitmap& b = face->glyph->bitmap;
            const int ox = (int)fx + face->glyph->bitmap_left, oy = (int)std::lround(gy) - face->glyph->bitmap_top;
            for (unsigned yy = 0; yy < b.rows; ++yy)
                for (unsigned xx = 0; xx < b.width; ++xx) blend(ox + (int)xx, oy + (int)yy, b.buffer[yy * b.pitch + xx]);
        }
        FT_Set_Transform(face, nullptr, nullptr);
    }
    for (const Rule& r : page.rules) {
        const double x0 = r.x * s, x1 = (r.x + r.w) * s, y0 = r.y * s, y1 = (r.y + r.h) * s;
        for (int y = (int)std::floor(y0); y < (int)std::ceil(y1); ++y)
            for (int x = (int)std::floor(x0); x < (int)std::ceil(x1); ++x) {
                const double cov = std::clamp(std::min<double>(y + 1, y1) - std::max<double>(y, y0), 0.0, 1.0);
                blend(x, y, (unsigned)std::lround(cov * 255));
            }
    }
    return bm;
}

// --------------------------------------------------------------------- PDF

namespace {

std::string deflate(const std::string& in) {
    mz_ulong n = mz_compressBound((mz_ulong)in.size());
    std::string out(n, '\0');
    if (mz_compress2(reinterpret_cast<unsigned char*>(out.data()), &n, reinterpret_cast<const unsigned char*>(in.data()),
                     (mz_ulong)in.size(), 6) != MZ_OK)
        return {};
    out.resize(n);
    return out;
}

std::string pdfString(const std::string& utf8in) {
    // A text string as UTF-16BE with a byte-order mark, in hex.
    static const char* hex = "0123456789ABCDEF";
    std::string out = "<FEFF";
    for (char32_t c : utf32(utf8in)) {
        auto put = [&](unsigned v) { for (int sh = 12; sh >= 0; sh -= 4) out += hex[(v >> sh) & 15]; };
        if (c >= 0x10000) { c -= 0x10000; put(0xD800 + (c >> 10)); put(0xDC00 + (c & 0x3FF)); }
        else put((unsigned)c);
    }
    return out + ">";
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0" || s.empty()) s = "0";
    return s;
}

class PdfWriter {
public:
    int reserve() { offsets_.push_back(0); return (int)offsets_.size(); }
    void object(int id, const std::string& body) {
        offsets_[id - 1] = out_.size();
        out_ += std::to_string(id) + " 0 obj\n" + body + "\nendobj\n";
    }
    void stream(int id, const std::string& dict, const std::string& data, bool compress) {
        std::string d = compress ? deflate(data) : data;
        std::string full = dict;
        full.pop_back();   // the closing ">>"
        full.pop_back();
        full += (compress ? " /Filter /FlateDecode" : "") + std::string(" /Length ") + std::to_string(d.size()) + " >>\nstream\n";
        offsets_[id - 1] = out_.size();
        out_ += std::to_string(id) + " 0 obj\n" + full;
        out_ += d;
        out_ += "\nendstream\nendobj\n";
    }
    std::string finish(int root, int info, const std::string& id = {}) {
        const size_t xref = out_.size();
        std::string x = "xref\n0 " + std::to_string(offsets_.size() + 1) + "\n0000000000 65535 f \n";
        char line[32];
        for (size_t o : offsets_) { std::snprintf(line, sizeof line, "%010llu 00000 n \n", (unsigned long long)o); x += line; }
        x += "trailer\n<< /Size " + std::to_string(offsets_.size() + 1) + " /Root " + std::to_string(root) + " 0 R /Info " +
             std::to_string(info) + " 0 R" + (id.empty() ? std::string() : " /ID [<" + id + "> <" + id + ">]") + " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
        return out_ + x;
    }
    std::string out_ = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
private:
    std::vector<size_t> offsets_;
};

// JPEG colour components (1 grey, 3 RGB, 4 CMYK), or 0 if not a baseline/progressive JPEG.
int jpegComponents(const std::string& b) {
    size_t i = 2;
    while (i + 9 < b.size()) {
        if ((std::uint8_t)b[i] != 0xFF) { ++i; continue; }
        const std::uint8_t m = (std::uint8_t)b[i + 1];
        if (m == 0xFF) { ++i; continue; }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        const unsigned len = ((std::uint8_t)b[i + 2] << 8) | (std::uint8_t)b[i + 3];
        if ((m == 0xC0 || m == 0xC1 || m == 0xC2) && i + 9 < b.size()) return (std::uint8_t)b[i + 9];
        i += 2 + len;
    }
    return 0;
}



// A picture converted for print: grey, or CMYK (a plain conversion; the printer's
// own colour management takes it from there).
std::string printPixels(const Pixels& px, bool cmyk) {
    const size_t n = (size_t)px.w * px.h;
    std::string out(n * (cmyk ? 4 : 1), '\0');
    for (size_t i = 0; i < n; ++i) {
        const double r = px.rgb[i * 3] / 255.0, g = px.rgb[i * 3 + 1] / 255.0, b = px.rgb[i * 3 + 2] / 255.0;
        if (!cmyk) {
            out[i] = (char)(std::uint8_t)std::lround((0.299 * r + 0.587 * g + 0.114 * b) * 255);
            continue;
        }
        const double k = 1 - std::max({r, g, b});
        const double d = k < 1 ? 1 - k : 1;
        const double c = (1 - r - k) / d, m = (1 - g - k) / d, y = (1 - b - k) / d;
        out[i * 4] = (char)(std::uint8_t)std::lround(std::clamp(c, 0.0, 1.0) * 255);
        out[i * 4 + 1] = (char)(std::uint8_t)std::lround(std::clamp(m, 0.0, 1.0) * 255);
        out[i * 4 + 2] = (char)(std::uint8_t)std::lround(std::clamp(y, 0.0, 1.0) * 255);
        out[i * 4 + 3] = (char)(std::uint8_t)std::lround(k * 255);
    }
    return out;
}

std::string hexId(const std::string& seed) {
    std::uint64_t a = 1469598103934665603ull, b = 0x9E3779B97F4A7C15ull;
    for (unsigned char c : seed) { a = (a ^ c) * 1099511628211ull; b = (b ^ c) * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull; }
    char out[33];
    std::snprintf(out, sizeof out, "%016llX%016llX", (unsigned long long)a, (unsigned long long)b);
    return out;
}

}  // namespace

bool writePdf(const Book& book, const std::string& path, std::string& error, const PdfOptions& options) {
    const std::string bytes = pdfBytes(book, error, options);
    if (bytes.empty()) return false;
    return atomicWrite(path, bytes, error);
}

std::string pdfBytes(const Book& book, std::string& error, const PdfOptions& options) {
    if (book.pages.empty()) { error = "There's nothing to export yet."; return {}; }
    const bool x1a = options.printReady;
    const double bleed = x1a && options.bleed ? 9.0 : 0.0;   // 0.125in
    PdfWriter pdf;
    if (x1a) pdf.out_ = "%PDF-1.3\n%\xE2\xE3\xCF\xD3\n";
    const int catalog = pdf.reserve(), pagesId = pdf.reserve(), info = pdf.reserve();
    std::lock_guard<std::recursive_mutex> guard(Fonts::get().lock());
    Fonts& fonts = Fonts::get();

    // Glyphs used per font, with their widths.
    std::vector<std::set<std::uint32_t>> used(book.fonts.size());
    for (auto& p : book.pages) for (auto& r : p.runs) used[r.font].insert(r.glyphs.begin(), r.glyphs.end());
    std::vector<int> fontObj(book.fonts.size());
    std::vector<int> faceOf(book.fonts.size());
    for (size_t f = 0; f < book.fonts.size(); ++f) {
        const int fi = fonts.open(book.fonts[f].path);
        if (fi < 0) { error = "A font couldn't be read: " + book.fonts[f].path; return {}; }
        faceOf[f] = fi;
        const Face& face = fonts.face(fi);
        const double k = 1000.0 / face.upem;
        const int type0 = pdf.reserve(), cid = pdf.reserve(), desc = pdf.reserve(), file = pdf.reserve(), cmap = pdf.reserve();
        fontObj[f] = type0;
        std::string name = face.postscriptName;
        for (auto& c : name) if (c == ' ' || c == '/' || c == '(' || c == ')') c = '-';
        // Embed only the glyphs used (glyph ids kept, so the text needs no renumbering).
        std::string fontData = face.data;
        {
            hb_subset_input_t* in = hb_subset_input_create_or_fail();
            if (in) {
                hb_set_t* gs = hb_subset_input_glyph_set(in);
                hb_set_add(gs, 0);
                for (std::uint32_t g : used[f]) hb_set_add(gs, g);
                hb_subset_input_set_flags(in, HB_SUBSET_FLAGS_RETAIN_GIDS);
                hb_set_t* drop = hb_subset_input_set(in, HB_SUBSET_SETS_DROP_TABLE_TAG);
                for (hb_tag_t t : {HB_TAG('G', 'S', 'U', 'B'), HB_TAG('G', 'P', 'O', 'S'), HB_TAG('G', 'D', 'E', 'F'), HB_TAG('k', 'e', 'r', 'n'),
                                   HB_TAG('D', 'S', 'I', 'G')})
                    hb_set_add(drop, t);
                if (hb_face_t* sub = hb_subset_or_fail(face.hbFace, in)) {
                    hb_blob_t* blob = hb_face_reference_blob(sub);
                    unsigned len = 0;
                    const char* d = hb_blob_get_data(blob, &len);
                    if (d && len > 12) {
                        fontData.assign(d, len);
                        // Subset fonts are named with a six-letter tag (PDF convention).
                        std::uint32_t h = 2166136261u;
                        for (std::uint32_t g : used[f]) h = (h ^ g) * 16777619u;
                        std::string tag;
                        for (int k = 0; k < 6; ++k) { tag += (char)('A' + h % 26); h /= 26; }
                        name = tag + "+" + name;
                    }
                    hb_blob_destroy(blob);
                    hb_face_destroy(sub);
                }
                hb_subset_input_destroy(in);
            }
        }
        pdf.stream(file, "<< /Length1 " + std::to_string(fontData.size()) + " >>", fontData, true);
        const FT_Face ft = face.ft;
        pdf.object(desc, "<< /Type /FontDescriptor /FontName /" + name + " /Flags 6 /FontBBox [" + num(ft->bbox.xMin * k) + " " +
                             num(ft->bbox.yMin * k) + " " + num(ft->bbox.xMax * k) + " " + num(ft->bbox.yMax * k) + "] /ItalicAngle 0 /Ascent " +
                             num(face.ascender * 1000) + " /Descent " + num(-face.descender * 1000) + " /CapHeight " +
                             num(face.capHeight * 1000) + " /StemV 80 /FontFile2 " + std::to_string(file) + " 0 R >>");
        std::string w = "[";
        for (std::uint32_t g : used[f]) w += " " + std::to_string(g) + " [" + num(fonts.advance(fi, g) * k) + "]";
        w += " ]";
        pdf.object(cid, "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /" + name +
                            " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " +
                            std::to_string(desc) + " 0 R /CIDToGIDMap /Identity /W " + w + " >>");
        // ToUnicode: what each glyph stands for, so text can be searched and copied.
        std::string cm = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                         "/CMapName /NorthRay-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
        std::vector<std::pair<std::uint32_t, std::string>> maps;
        for (std::uint32_t g : used[f])
            if (auto it = book.fonts[f].toUnicode.find(g); it != book.fonts[f].toUnicode.end()) maps.push_back(*it);
        for (size_t i = 0; i < maps.size(); i += 100) {
            const size_t n = std::min<size_t>(100, maps.size() - i);
            cm += std::to_string(n) + " beginbfchar\n";
            char gh[8];
            for (size_t j = i; j < i + n; ++j) {
                std::snprintf(gh, sizeof gh, "%04X", maps[j].first & 0xFFFF);
                std::string u = pdfString(maps[j].second);
                u = "<" + u.substr(5);   // without the byte-order mark
                cm += std::string("<") + gh + "> " + u + "\n";
            }
            cm += "endbfchar\n";
        }
        cm += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
        pdf.stream(cmap, "<< >>", cm, true);
        pdf.object(type0, "<< /Type /Font /Subtype /Type0 /BaseFont /" + name + " /Encoding /Identity-H /DescendantFonts [" +
                              std::to_string(cid) + " 0 R] /ToUnicode " + std::to_string(cmap) + " 0 R >>");
    }

    // Pictures: each stored once.
    std::map<std::string, int> imageObj;
    for (auto& p : book.pages)
        for (auto& pic : p.pictures) {
            if (imageObj.count(pic.blob)) continue;
            auto where = book.picturePaths.find(pic.blob);
            const std::string bytes = where == book.picturePaths.end() ? std::string() : readAll(where->second);
            const ImageInfo inf = imageInfo(bytes);
            const int id = pdf.reserve();
            const int comps = inf.format == "jpeg" ? jpegComponents(bytes) : 0;
            if (comps == 1 || (comps == 3 && !x1a)) {
                pdf.stream(id, "<< /Type /XObject /Subtype /Image /Width " + std::to_string(inf.width) + " /Height " + std::to_string(inf.height) +
                                   " /ColorSpace /" + (comps == 1 ? "DeviceGray" : "DeviceRGB") + " /BitsPerComponent 8 /Filter /DCTDecode >>",
                           bytes, false);
            } else {
                const Pixels px = decode(bytes);
                if (px.w <= 0) { pdf.object(id, "<< >>"); imageObj[pic.blob] = 0; continue; }
                if (x1a) {
                    pdf.stream(id, "<< /Type /XObject /Subtype /Image /Width " + std::to_string(px.w) + " /Height " + std::to_string(px.h) +
                                       " /ColorSpace /" + (options.colourPictures ? "DeviceCMYK" : "DeviceGray") + " /BitsPerComponent 8 >>",
                               printPixels(px, options.colourPictures), true);
                    imageObj[pic.blob] = id;
                    continue;
                }
                pdf.stream(id, "<< /Type /XObject /Subtype /Image /Width " + std::to_string(px.w) + " /Height " + std::to_string(px.h) +
                                   " /ColorSpace /DeviceRGB /BitsPerComponent 8 >>",
                           std::string(px.rgb.begin(), px.rgb.end()), true);
            }
            imageObj[pic.blob] = id;
        }

    // Pages.
    std::string kids;
    const double H = book.height;
    std::vector<int> pageIds;
    for (size_t pi = 0; pi < book.pages.size(); ++pi) {
        const Page& p = book.pages[pi];
        const int pid = pdf.reserve(), content = pdf.reserve();
        pageIds.push_back(pid);
        std::string c;
        // With bleed, the extra is at the top, bottom and outside edge: on the right of
        // a right-hand page (page 1, 3, ...), on the left of a left-hand page.
        const double ox = bleed > 0 && pi % 2 == 1 ? bleed : 0, oy = bleed;
        if (bleed > 0) c += "q 1 0 0 1 " + num(ox) + " " + num(oy) + " cm\n";
        for (auto& pic : p.pictures) {
            auto it = imageObj.find(pic.blob);
            if (it == imageObj.end() || !it->second) continue;
            const double fullW = pic.w / pic.crop.cropW, fullH = pic.h / pic.crop.cropH;
            const double ox = pic.x - pic.crop.cropX * fullW;
            const double oyTop = pic.y - pic.crop.cropY * fullH;
            c += "q " + num(pic.x) + " " + num(H - pic.y - pic.h) + " " + num(pic.w) + " " + num(pic.h) + " re W n " + num(fullW) + " 0 0 " +
                 num(fullH) + " " + num(ox) + " " + num(H - oyTop - fullH) + " cm /Im" + std::to_string(it->second) + " Do Q\n";
        }
        // On screen the ink is a warm near-black; in print, pure black on the black plate only.
        if (x1a) c += "0 g\nBT\n";
        else c += num(kInk[0] / 255.0) + " " + num(kInk[1] / 255.0) + " " + num(kInk[2] / 255.0) + " rg\nBT\n";
        int curFont = -1;
        double curSize = -1;
        char gh[8];
        for (auto& r : p.runs) {
            if (r.glyphs.empty()) continue;
            if (r.font != curFont || r.size != curSize) {
                c += "/F" + std::to_string(r.font) + " " + num(r.size) + " Tf\n";
                curFont = r.font;
                curSize = r.size;
            }
            const int fi = faceOf[r.font];
            const double unit = r.size / fonts.face(fi).upem;
            size_t i = 0;
            while (i < r.glyphs.size()) {
                // One TJ per stretch of glyphs on the same baseline; kerning goes in as adjustments.
                const double dyNow = r.dy[i];
                c += "1 0 0 1 " + num(r.xs[i]) + " " + num(H - (r.y + dyNow)) + " Tm [";
                double pen = r.xs[i];
                for (; i < r.glyphs.size() && r.dy[i] == dyNow; ++i) {
                    const double adj = (r.xs[i] - pen) * 1000.0 / r.size;
                    if (std::abs(adj) > 0.05) c += num(-adj) + " ";
                    std::snprintf(gh, sizeof gh, "%04X", r.glyphs[i] & 0xFFFF);
                    c += std::string("<") + gh + ">";
                    pen = r.xs[i] + fonts.advance(fi, r.glyphs[i]) * unit;
                }
                c += "] TJ\n";
            }
        }
        c += "ET\n";
        for (auto& rule : p.rules) c += num(rule.x) + " " + num(H - rule.y - rule.h) + " " + num(rule.w) + " " + num(rule.h) + " re f\n";
        if (bleed > 0) c += "Q\n";
        pdf.stream(content, "<< >>", c, true);
        std::string fontsRes;
        for (size_t f = 0; f < book.fonts.size(); ++f) fontsRes += " /F" + std::to_string(f) + " " + std::to_string(fontObj[f]) + " 0 R";
        std::string xobj;
        for (auto& pic : p.pictures)
            if (auto it = imageObj.find(pic.blob); it != imageObj.end() && it->second)
                xobj += " /Im" + std::to_string(it->second) + " " + std::to_string(it->second) + " 0 R";
        const std::string media = "[0 0 " + num(book.width + bleed) + " " + num(book.height + 2 * bleed) + "]";
        const std::string trimBox = "[" + num(ox) + " " + num(oy) + " " + num(ox + book.width) + " " + num(oy + book.height) + "]";
        pdf.object(pid, "<< /Type /Page /Parent " + std::to_string(pagesId) + " 0 R /MediaBox " + media + " /TrimBox " + trimBox +
                            (bleed > 0 ? " /BleedBox " + media : std::string()) +
                            " /Resources << /Font <<" + fontsRes + " >>" + (xobj.empty() ? "" : " /XObject <<" + xobj + " >>") +
                            " >> /Contents " + std::to_string(content) + " 0 R >>");
        kids += " " + std::to_string(pid) + " 0 R";
    }
    pdf.object(pagesId, "<< /Type /Pages /Kids [" + kids + " ] /Count " + std::to_string(book.pages.size()) + " >>");
    char date[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(date, sizeof date, "D:%Y%m%d%H%M%SZ", std::gmtime(&t));
    std::string intents;
    if (x1a) {
        // The printing condition, by its registered name (no profile needs embedding for PDF/X-1a:2001).
        const int oi = pdf.reserve();
        pdf.object(oi, "<< /Type /OutputIntent /S /GTS_PDFX /OutputConditionIdentifier (CGATS TR 001) /OutputCondition (SWOP \\(Publication\\) Grade 5 Paper) "
                       "/RegistryName (http://www.color.org) /Info (U.S. Web Coated \\(SWOP\\) v2) >>");
        intents = " /OutputIntents [" + std::to_string(oi) + " 0 R]";
    }
    pdf.object(catalog, "<< /Type /Catalog /Pages " + std::to_string(pagesId) + " 0 R" + intents + " >>");
    const std::string title = book.title.empty() ? std::string("Untitled") : book.title;
    pdf.object(info, "<< /Title " + pdfString(title) + (book.author.empty() ? "" : " /Author " + pdfString(book.author)) +
                         " /Creator (NorthRay Writing Studio) /Producer (NorthRay Writing Studio) /CreationDate (" + date + ") /ModDate (" + date + ")" +
                         (x1a ? " /Trapped /False /GTS_PDFXVersion (PDF/X-1:2001) /GTS_PDFXConformance (PDF/X-1a:2001)" : "") + " >>");
    return pdf.finish(catalog, info, hexId(title + book.author + date + std::to_string(book.pages.size())));
}

}  // namespace studio::typeset

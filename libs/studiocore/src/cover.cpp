// Ebook covers: text set with the typesetter's fonts (HarfBuzz shaping, FreeType rasterising) over a
// colour or a picture, written as a PNG.
#include "studio/cover.hpp"

#include "internal.hpp"
#include "typeset_internal.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>

#include "stb/stb_image.h"         // implemented in typeset_output.cpp
#include "miniz.h"

namespace studio::cover {

using nlohmann::json;
using namespace typeset::detail;

const std::vector<Layout>& layouts() {
    static const std::vector<Layout> l = {
        {"classic", "Classic", "A coloured cover with a double frame, the title in capitals."},
        {"picture", "Picture", "Your picture fills the cover; the words sit over shaded top and bottom."},
        {"band", "Picture and band", "Your picture above a coloured band holding the title and author."},
        {"minimal", "Minimal", "Plain paper, a large title and a fine rule."},
    };
    return l;
}

const std::vector<Colours>& palettes() {
    static const std::vector<Colours> p = {
        {"wine", "Wine and gold", "#6B2D3C", "#F4EBDD", "#D9B26B"},
        {"night", "Night sky", "#0E1A2E", "#EAF0F8", "#59D6A8"},
        {"forest", "Forest", "#1F3B2C", "#EFE8D6", "#C9A55A"},
        {"ocean", "Deep sea", "#123B4A", "#F1F5F2", "#8FD3D1"},
        {"ember", "Ember", "#2A1612", "#F6E7D6", "#E0773B"},
        {"paper", "Paper", "#F3EEE3", "#1E1B18", "#9C2F2F"},
        {"slate", "Slate", "#2E333B", "#F2F2EE", "#B8C2CC"},
    };
    return p;
}

namespace {

struct Colour { double r = 0, g = 0, b = 0; };

Colour parse(const std::string& hex, Colour fallback) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() != 6 || h.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return fallback;
    const unsigned v = (unsigned)std::stoul(h, nullptr, 16);
    return {(double)(v >> 16 & 255), (double)(v >> 8 & 255), (double)(v & 255)};
}

double luminance(const Colour& c) { return (0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) / 255.0; }

struct Canvas {
    int w = 0, h = 0;
    std::vector<double> px;   // RGB, 0..255
    void fill(int x0, int y0, int x1, int y1, Colour c, double a = 1) {
        x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(w, x1); y1 = std::min(h, y1);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) blend(x, y, c, a);
    }
    void blend(int x, int y, Colour c, double a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        double* p = &px[((size_t)y * w + x) * 3];
        p[0] += (c.r - p[0]) * a; p[1] += (c.g - p[1]) * a; p[2] += (c.b - p[2]) * a;
    }
    // A rectangle's outline `t` pixels thick.
    void frame(int x0, int y0, int x1, int y1, int t, Colour c, double a = 1) {
        fill(x0, y0, x1, y0 + t, c, a);
        fill(x0, y1 - t, x1, y1, c, a);
        fill(x0, y0 + t, x0 + t, y1 - t, c, a);
        fill(x1 - t, y0 + t, x1, y1 - t, c, a);
    }
    // Shading from `a0` at y0 to `a1` at y1.
    void shade(int y0, int y1, Colour c, double a0, double a1) {
        for (int y = std::max(0, y0); y < std::min(h, y1); ++y) {
            const double t = (double)(y - y0) / std::max(1, y1 - y0);
            const double a = a0 + (a1 - a0) * (t * t * (3 - 2 * t));
            for (int x = 0; x < w; ++x) blend(x, y, c, a);
        }
    }
};

// A picture scaled to fill the box (cropped on its longer side), drawn into it.
bool drawPicture(Canvas& cv, const std::string& path, int bx, int by, int bw, int bh, double focusY) {
    std::ifstream in(studio::detail::u8path(path), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string bytes = ss.str();
    int iw = 0, ih = 0, n = 0;
    unsigned char* d = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), (int)bytes.size(), &iw, &ih, &n, 3);
    if (!d) return false;
    const double k = std::max((double)bw / iw, (double)bh / ih);
    const double sw = bw / k, sh = bh / k;   // the part of the picture that shows, in its pixels
    const double sx0 = (iw - sw) / 2, sy0 = (ih - sh) * std::clamp(focusY, 0.0, 1.0);
    for (int y = 0; y < bh; ++y) {
        const double fy0 = sy0 + sh * y / bh, fy1 = sy0 + sh * (y + 1) / bh;
        const int ay = std::clamp((int)fy0, 0, ih - 1), zy = std::clamp((int)std::ceil(fy1) - 1, ay, ih - 1);
        for (int x = 0; x < bw; ++x) {
            const double fx0 = sx0 + sw * x / bw, fx1 = sx0 + sw * (x + 1) / bw;
            const int ax = std::clamp((int)fx0, 0, iw - 1), zx = std::clamp((int)std::ceil(fx1) - 1, ax, iw - 1);
            double sum[3] = {0, 0, 0};
            int cnt = 0;
            const int stepY = std::max(1, (zy - ay) / 3), stepX = std::max(1, (zx - ax) / 3);
            for (int yy = ay; yy <= zy; yy += stepY)
                for (int xx = ax; xx <= zx; xx += stepX) {
                    const unsigned char* p = &d[((size_t)yy * iw + xx) * 3];
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; ++cnt;
                }
            const int tx = bx + x, ty = by + y;
            if (tx < 0 || ty < 0 || tx >= cv.w || ty >= cv.h) continue;
            double* q = &cv.px[((size_t)ty * cv.w + tx) * 3];
            for (int c = 0; c < 3; ++c) q[c] = sum[c] / cnt;
        }
    }
    stbi_image_free(d);
    return true;
}

std::string upperCase(const std::string& s) {
    // ASCII and the common Latin-1 letters (enough for titles; the font has the capitals).
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') out += (char)(c - 32);
        else if (c == 0xC3 && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0xA0 && (unsigned char)s[i + 1] <= 0xBE && (unsigned char)s[i + 1] != 0xB7) {
            out += (char)c;
            out += (char)((unsigned char)s[++i] - 0x20);
        } else out += (char)c;
    }
    return out;
}

struct Writer {
    Canvas& cv;
    Family fam;
    std::string lang = "en";

    // A line's width at `size` with `tracking` (extra space after each glyph, in ems).
    float width(int face, const std::string& t, float size, double tracking) {
        const Shaped s = shape(face, t, size, false, lang);
        return s.width + (float)(tracking * size) * (float)std::max<size_t>(0, s.glyphs.empty() ? 0 : s.glyphs.size() - 1);
    }

    // Draws a line centred on `cx` (or starting at `cx` when !centre), its baseline at `y`.
    void line(int face, const std::string& t, float size, double tracking, double cx, double y, Colour c, bool centre = true) {
        if (face < 0 || t.empty()) return;
        std::lock_guard<std::recursive_mutex> guard(Fonts::get().lock());
        const Shaped s = shape(face, t, size, false, lang);
        const float track = (float)(tracking * size);
        const float total = s.width + track * (float)(s.glyphs.empty() ? 0 : s.glyphs.size() - 1);
        const double x0 = centre ? cx - total / 2 : cx;
        for (size_t i = 0; i < s.glyphs.size(); ++i) {
            const int fi = s.faces.empty() ? face : s.faces[i];
            FT_Face ft = Fonts::get().face(fi).ft;
            FT_Set_Char_Size(ft, 0, (FT_F26Dot6)std::lround(size * 64), 72, 72);
            const double gx = x0 + s.x[i] + track * (float)i, gy = y + (s.dy.empty() ? 0 : s.dy[i]);
            const double fx = std::floor(gx);
            FT_Vector delta{(FT_Pos)std::lround((gx - fx) * 64), 0};
            FT_Set_Transform(ft, nullptr, &delta);
            if (FT_Load_Glyph(ft, s.glyphs[i], FT_LOAD_NO_HINTING | FT_LOAD_RENDER) != 0) continue;
            const FT_Bitmap& b = ft->glyph->bitmap;
            const int ox = (int)fx + ft->glyph->bitmap_left, oy = (int)std::lround(gy) - ft->glyph->bitmap_top;
            for (unsigned yy = 0; yy < b.rows; ++yy)
                for (unsigned xx = 0; xx < b.width; ++xx) cv.blend(ox + (int)xx, oy + (int)yy, c, b.buffer[yy * b.pitch + xx] / 255.0);
            FT_Set_Transform(ft, nullptr, nullptr);
        }
    }

    // Words wrapped into at most `maxLines` lines no wider than `maxW`, at the largest size from `maxSize`
    // down to `minSize` that fits. Lines are balanced (no long line over a lone word).
    std::vector<std::string> fit(int face, const std::string& text, float maxW, float maxSize, float minSize, int maxLines, double tracking,
                                 float& sizeOut) {
        std::vector<std::string> words;
        std::istringstream in(text);
        for (std::string w; in >> w;) words.push_back(w);
        if (words.empty()) { sizeOut = maxSize; return {}; }
        for (float size = maxSize; size >= minSize; size *= 0.94f) {
            // Every way of breaking into k lines, smallest widest line first (titles are short).
            for (int k = 1; k <= maxLines && k <= (int)words.size(); ++k) {
                std::vector<std::string> best;
                float bestW = 1e9f;
                std::vector<int> cut(k - 1);
                // Breaks chosen greedily by target width, then compared with neighbours.
                const float total = width(face, text, size, tracking);
                std::vector<std::string> lines;
                std::string cur;
                const float target = total / k;
                for (size_t i = 0; i < words.size(); ++i) {
                    const std::string next = cur.empty() ? words[i] : cur + " " + words[i];
                    const bool lastLine = (int)lines.size() == k - 1;
                    if (!cur.empty() && !lastLine && width(face, next, size, tracking) > target * 1.12f) {
                        lines.push_back(cur);
                        cur = words[i];
                    } else cur = next;
                }
                if (!cur.empty()) lines.push_back(cur);
                float widest = 0;
                for (auto& l : lines) widest = std::max(widest, width(face, l, size, tracking));
                if (widest <= maxW && (int)lines.size() <= maxLines) { best = lines; bestW = widest; }
                if (!best.empty()) { sizeOut = size; (void)bestW; return best; }
            }
        }
        // Still too long: the smallest size, as many lines as it takes.
        sizeOut = minSize;
        std::vector<std::string> lines;
        std::string cur;
        for (auto& w : words) {
            const std::string next = cur.empty() ? w : cur + " " + w;
            if (!cur.empty() && width(face, next, minSize, tracking) > maxW) { lines.push_back(cur); cur = w; }
            else cur = next;
        }
        if (!cur.empty()) lines.push_back(cur);
        return lines;
    }
};

}  // namespace

Result render(const Spec& sp, const std::string& fontsFolder, int W, int H) {
    Result r;
    if (W < 200 || H < 300) { r.error = "The cover is too small to draw."; return r; }
    Canvas cv;
    cv.w = W;
    cv.h = H;
    const Colour bg = parse(sp.background, {107, 45, 60}), ink = parse(sp.ink, {244, 235, 221}), accent = parse(sp.accent, {217, 178, 107});
    cv.px.assign((size_t)W * H * 3, 0);
    for (size_t i = 0; i < (size_t)W * H; ++i) { cv.px[i * 3] = bg.r; cv.px[i * 3 + 1] = bg.g; cv.px[i * 3 + 2] = bg.b; }

    Writer wr{cv, {}, "en"};
    if (!openFamily(fontsFolder, sp.fontId.empty() ? "crimson-text" : sp.fontId, wr.fam) && !openFamily(fontsFolder, "crimson-text", wr.fam)) {
        r.error = "The cover's typeface couldn't be opened.";
        return r;
    }
    Fonts::get().useFallbacks(fontsFolder + "/symbols");
    const int reg = wr.fam.regular, ital = wr.fam.pick(false, true), bold = wr.fam.pick(true, false);
    const double u = W / 1600.0;   // the layouts are drawn for 1600 pixels wide
    const std::string title = sp.titleCaps ? upperCase(sp.title) : sp.title;
    const std::string author = upperCase(sp.author);
    const bool picture = !sp.picturePath.empty();
    const std::string layout = sp.layout;

    // The title block: the lines, centred, from `top`; returns the baseline of the last line.
    auto titleBlock = [&](double top, double maxW, float maxSize, int maxLines, Colour c, double tracking) {
        float size = maxSize;
        const auto lines = wr.fit(sp.titleCaps ? reg : bold, title.empty() ? std::string("Untitled") : title, (float)maxW, maxSize, maxSize * 0.35f,
                                  maxLines, tracking, size);
        const double lead = size * (sp.titleCaps ? 1.08 : 1.12);
        double y = top + size * 0.78;
        for (size_t i = 0; i < lines.size(); ++i, y += lead) wr.line(sp.titleCaps ? reg : bold, lines[i], size, tracking, W / 2.0, y, c);
        return y - lead;
    };
    auto smallLine = [&](const std::string& t, int face, float size, double tracking, double y, Colour c) {
        if (t.empty()) return;
        float s = size;
        const auto lines = wr.fit(face, t, (float)(W - 260 * u), size, size * 0.6f, 2, tracking, s);
        for (size_t i = 0; i < lines.size(); ++i) wr.line(face, lines[i], s, tracking, W / 2.0, y + i * s * 1.2, c);
    };

    if (layout == "picture") {
        if (picture && !drawPicture(cv, sp.picturePath, 0, 0, W, H, sp.focusY)) { r.error = "The picture couldn't be read."; return r; }
        // Shade the top and bottom with the background colour so the words read on any picture.
        cv.shade(0, (int)(H * 0.46), bg, 0.82, 0.0);
        cv.shade((int)(H * 0.70), H, bg, 0.0, 0.88);
        smallLine(upperCase(sp.series), reg, (float)(40 * u), 0.22, H * 0.075, accent);
        const double end = titleBlock(H * 0.10, W - 200 * u, (float)(220 * u), 3, ink, 0.04);
        if (!sp.subtitle.empty()) smallLine(sp.subtitle, ital, (float)(70 * u), 0.0, end + 150 * u, ink);
        smallLine(author, reg, (float)(78 * u), 0.16, H * 0.915, ink);
    } else if (layout == "band") {
        const int bandTop = (int)(H * 0.62);
        if (picture) {
            if (!drawPicture(cv, sp.picturePath, 0, 0, W, bandTop, sp.focusY)) { r.error = "The picture couldn't be read."; return r; }
        } else {
            const Colour deeper{bg.r * 0.7, bg.g * 0.7, bg.b * 0.7};
            cv.shade(0, bandTop, deeper, 1.0, 0.0);
        }
        cv.fill(0, bandTop, W, bandTop + (int)(14 * u), accent);
        smallLine(upperCase(sp.series), reg, (float)(38 * u), 0.22, bandTop + 110 * u, accent);
        const double end = titleBlock(bandTop + (sp.series.empty() ? 90 : 150) * u, W - 220 * u, (float)(170 * u), 2, ink, 0.04);
        if (!sp.subtitle.empty()) smallLine(sp.subtitle, ital, (float)(62 * u), 0.0, end + 135 * u, ink);
        smallLine(author, reg, (float)(70 * u), 0.16, H * 0.94, ink);
    } else if (layout == "minimal") {
        // The ink is the title's colour; the rule is the accent.
        smallLine(upperCase(sp.series), reg, (float)(40 * u), 0.22, H * 0.16, accent);
        const double end = titleBlock(H * 0.30, W - 260 * u, (float)(200 * u), 4, ink, 0.0);
        cv.fill((int)(W / 2 - 140 * u), (int)(end + 90 * u), (int)(W / 2 + 140 * u), (int)(end + 96 * u), accent);
        if (!sp.subtitle.empty()) smallLine(sp.subtitle, ital, (float)(66 * u), 0.0, end + 210 * u, ink);
        smallLine(author, reg, (float)(74 * u), 0.18, H * 0.86, ink);
    } else {
        // Classic: a double frame, the title in the upper half, a short rule, the author at the foot.
        cv.frame((int)(70 * u), (int)(70 * u), W - (int)(70 * u), H - (int)(70 * u), (int)(7 * u), accent);
        cv.frame((int)(96 * u), (int)(96 * u), W - (int)(96 * u), H - (int)(96 * u), std::max(1, (int)(2 * u)), accent, 0.8);
        if (picture) {
            // A picture, if given, sits in a window under the title.
            const int px0 = (int)(220 * u), pw = W - 2 * px0, py0 = (int)(H * 0.47), ph = (int)(H * 0.30);
            if (!drawPicture(cv, sp.picturePath, px0, py0, pw, ph, sp.focusY)) { r.error = "The picture couldn't be read."; return r; }
            cv.frame(px0 - (int)(6 * u), py0 - (int)(6 * u), px0 + pw + (int)(6 * u), py0 + ph + (int)(6 * u), (int)(4 * u), accent);
        }
        smallLine(upperCase(sp.series), reg, (float)(40 * u), 0.22, H * 0.12, accent);
        const double end = titleBlock(H * (picture ? 0.17 : 0.24), W - 380 * u, (float)(200 * u), 3, ink, 0.06);
        cv.fill((int)(W / 2 - 110 * u), (int)(end + 80 * u), (int)(W / 2 + 110 * u), (int)(end + 86 * u), accent);
        if (!sp.subtitle.empty()) smallLine(sp.subtitle, ital, (float)(68 * u), 0.0, end + 190 * u, ink);
        smallLine(author, reg, (float)(76 * u), 0.2, H * 0.875, ink);
    }

    std::vector<std::uint8_t> rgb((size_t)W * H * 3);
    for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = (std::uint8_t)std::clamp((int)std::lround(cv.px[i]), 0, 255);
    size_t len = 0;
    if (void* png = tdefl_write_image_to_png_file_in_memory(rgb.data(), W, H, 3, &len)) {
        r.png.assign(static_cast<const char*>(png), len);
        mz_free(png);
    }
    r.ok = !r.png.empty();
    if (!r.ok) r.error = "The cover couldn't be written.";
    r.width = W;
    r.height = H;
    (void)luminance;
    return r;
}

std::string toJson(const Spec& s) {
    return json{{"layout", s.layout}, {"title", s.title}, {"subtitle", s.subtitle}, {"author", s.author}, {"series", s.series},
                {"background", s.background}, {"ink", s.ink}, {"accent", s.accent}, {"picture", s.picturePath}, {"focusY", s.focusY},
                {"font", s.fontId}, {"titleCaps", s.titleCaps}}
        .dump();
}

Spec fromJson(const std::string& text) {
    Spec s;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return s;
    s.layout = j.value("layout", s.layout);
    s.title = j.value("title", s.title);
    s.subtitle = j.value("subtitle", s.subtitle);
    s.author = j.value("author", s.author);
    s.series = j.value("series", s.series);
    s.background = j.value("background", s.background);
    s.ink = j.value("ink", s.ink);
    s.accent = j.value("accent", s.accent);
    s.picturePath = j.value("picture", s.picturePath);
    s.focusY = j.value("focusY", s.focusY);
    s.fontId = j.value("font", s.fontId);
    s.titleCaps = j.value("titleCaps", s.titleCaps);
    return s;
}

}  // namespace studio::cover

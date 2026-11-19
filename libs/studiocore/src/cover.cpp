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
#include <functional>
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
        {"arch", "Arch", "Your picture in a tall arched window, the title beneath."},
        {"medallion", "Medallion", "Your picture in a ringed circle between the title and the author."},
        {"sidebar", "Sidebar", "Your picture fills the cover; a coloured column down the left holds the words."},
        {"stacked", "Stacked", "No picture: the title's words stacked large, filling the cover between thick rules."},
        {"starfield", "Starfield", "A night sky of stars over a glow; your picture, if any, along the horizon."},
        {"ornate", "Ornate", "A wide decorated border with corner pieces, the title framed inside."},
        {"modern", "Modern", "Your picture fills the cover; the title at the foot, set to the left."},
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


struct Canvas {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;   // RGB
    void fill(int x0, int y0, int x1, int y1, Colour c, double a = 1) {
        x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(w, x1); y1 = std::min(h, y1);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) blend(x, y, c, a);
    }
    void blend(int x, int y, Colour c, double a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        std::uint8_t* p = &px[((size_t)y * w + x) * 3];
        p[0] = (std::uint8_t)std::lround(p[0] + (c.r - p[0]) * a);
        p[1] = (std::uint8_t)std::lround(p[1] + (c.g - p[1]) * a);
        p[2] = (std::uint8_t)std::lround(p[2] + (c.b - p[2]) * a);
    }
    // A rectangle's outline `t` pixels thick.
    void frame(int x0, int y0, int x1, int y1, int t, Colour c, double a = 1) {
        fill(x0, y0, x1, y0 + t, c, a);
        fill(x0, y1 - t, x1, y1, c, a);
        fill(x0, y0 + t, x0 + t, y1 - t, c, a);
        fill(x1 - t, y0 + t, x1, y1 - t, c, a);
    }
    // A filled circle, its edge smoothed.
    void disc(double cx, double cy, double r, Colour c, double a = 1) {
        for (int y = (int)std::floor(cy - r - 1); y <= (int)std::ceil(cy + r + 1); ++y)
            for (int x = (int)std::floor(cx - r - 1); x <= (int)std::ceil(cx + r + 1); ++x) {
                const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                const double cover = std::clamp(r + 0.5 - d, 0.0, 1.0);
                if (cover > 0) blend(x, y, c, a * cover);
            }
    }
    // A ring of thickness `t` at radius `r` (smoothed), drawn only above `maxY` and between `minX` and `maxX`
    // (an arch's sides are drawn as straight lines down from there).
    void ring(double cx, double cy, double r, double t, Colour c, double maxY = 1e9, double minX = -1e9, double /*maxX*/ = 1e9, double a = 1) {
        const double r0 = r - t / 2, r1 = r + t / 2;
        const int yEnd = (int)std::min<double>(h, std::min(maxY, cy + r1 + 1));
        for (int y = std::max(0, (int)(cy - r1 - 1)); y < yEnd; ++y)
            for (int x = std::max(0, (int)(cx - r1 - 1)); x <= std::min(w - 1, (int)(cx + r1 + 1)); ++x) {
                if (y > cy && maxY < 1e8) continue;
                const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                const double cover = std::clamp(std::min(d - r0 + 0.5, r1 + 0.5 - d), 0.0, 1.0);
                if (cover > 0) blend(x, y, c, a * cover);
            }
        if (maxY < 1e8 && minX > -1e8) {   // an arch: straight sides from the middle down
            fill((int)(cx - r1), (int)cy, (int)(cx - r0), (int)maxY, c, a);
            fill((int)(cx + r0), (int)cy, (int)(cx + r1), (int)maxY, c, a);
            fill((int)(cx - r1), (int)maxY - (int)t, (int)(cx + r1), (int)maxY, c, a);
        }
    }
    // A square turned on its corner, `size` from point to point.
    void diamond(double cx, double cy, double size, Colour c, double a = 1) {
        const double half = size / 2;
        for (int y = (int)(cy - half - 1); y <= (int)(cy + half + 1); ++y)
            for (int x = (int)(cx - half - 1); x <= (int)(cx + half + 1); ++x) {
                const double d = std::fabs(x + 0.5 - cx) + std::fabs(y + 0.5 - cy);
                const double cover = std::clamp(half + 0.5 - d, 0.0, 1.0);
                if (cover > 0) blend(x, y, c, a * cover);
            }
    }
    // A soft horizontal glow centred on `y`, `spread` pixels each way.
    void glow(double y, double spread, Colour c, double a) {
        for (int yy = std::max(0, (int)(y - spread)); yy < std::min(h, (int)(y + spread)); ++yy) {
            const double t = 1 - std::fabs(yy - y) / spread;
            for (int x = 0; x < w; ++x) blend(x, yy, c, a * t * t);
        }
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
bool drawPicture(Canvas& cv, const std::string& path, int bx, int by, int bw, int bh, double focusY,
                 const std::function<bool(int, int)>& inside = {}) {
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
            if (inside && !inside(tx, ty)) continue;
            std::uint8_t* q = &cv.px[((size_t)ty * cv.w + tx) * 3];
            for (int c = 0; c < 3; ++c) q[c] = (std::uint8_t)std::lround(sum[c] / cnt);
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

    // Body text in lines no wider than `maxW` (paragraphs kept; an empty line between them).
    std::vector<std::string> wrap(int face, const std::string& text, float size, float maxW) {
        std::vector<std::string> out;
        std::istringstream paras(text);
        for (std::string para; std::getline(paras, para);) {
            if (para.find_first_not_of(" \t\r") == std::string::npos) { if (!out.empty() && !out.back().empty()) out.push_back(""); continue; }
            std::istringstream in(para);
            std::string cur;
            for (std::string w; in >> w;) {
                const std::string next = cur.empty() ? w : cur + " " + w;
                if (!cur.empty() && width(face, next, size, 0) > maxW) { out.push_back(cur); cur = w; }
                else cur = next;
            }
            if (!cur.empty()) out.push_back(cur);
        }
        return out;
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

namespace {

Canvas blank(int W, int H, const Colour& bg) {
    Canvas cv;
    cv.w = W;
    cv.h = H;
    cv.px.resize((size_t)W * H * 3);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        cv.px[i * 3] = (std::uint8_t)bg.r; cv.px[i * 3 + 1] = (std::uint8_t)bg.g; cv.px[i * 3 + 2] = (std::uint8_t)bg.b;
    }
    return cv;
}

// The front cover, filling the canvas.
bool drawFront(Canvas& cv, const Spec& sp, const std::string& fontsFolder, Result& r) {
    const int W = cv.w, H = cv.h;
    const Colour bg = parse(sp.background, {107, 45, 60}), ink = parse(sp.ink, {244, 235, 221}), accent = parse(sp.accent, {217, 178, 107});
    Writer wr{cv, {}, "en"};
    if (!openFamily(fontsFolder, sp.fontId.empty() ? "crimson-text" : sp.fontId, wr.fam) && !openFamily(fontsFolder, "crimson-text", wr.fam)) {
        r.error = "The cover's typeface couldn't be opened.";
        return false;
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
        if (picture && !drawPicture(cv, sp.picturePath, 0, 0, W, H, sp.focusY)) { r.error = "The picture couldn't be read."; return false; }
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
            if (!drawPicture(cv, sp.picturePath, 0, 0, W, bandTop, sp.focusY)) { r.error = "The picture couldn't be read."; return false; }
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
    } else if (layout == "arch") {
        // A tall arched window holding the picture (or a deeper shade of the background), the title beneath.
        const double ax0 = 230 * u, ax1 = W - 230 * u, ay0 = H * 0.12, ay1 = H * 0.60;
        const double rad = (ax1 - ax0) / 2, cx = W / 2.0, cy = ay0 + rad;
        auto inArch = [&](int x, int y) {
            if (x < ax0 || x >= ax1 || y < ay0 || y >= ay1) return false;
            if (y >= cy) return true;
            const double dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= rad * rad;
        };
        if (picture) {
            if (!drawPicture(cv, sp.picturePath, (int)ax0, (int)ay0, (int)(ax1 - ax0), (int)(ay1 - ay0), sp.focusY, inArch)) {
                r.error = "The picture couldn't be read.";
                return false;
            }
        } else {
            const Colour deeper{bg.r * 0.72, bg.g * 0.72, bg.b * 0.72};
            for (int y = (int)ay0; y < (int)ay1; ++y)
                for (int x = (int)ax0; x < (int)ax1; ++x) if (inArch(x, y)) cv.blend(x, y, deeper, 1);
        }
        cv.ring(cx, cy, rad + 14 * u, 5 * u, accent, ay1, ax0 - 14 * u, ax1 + 14 * u);
        smallLine(upperCase(sp.series), reg, (float)(38 * u), 0.22, H * 0.075, accent);
        const double end = titleBlock(H * 0.645, W - 240 * u, (float)(170 * u), 2, ink, 0.05);
        if (!sp.subtitle.empty()) smallLine(sp.subtitle, ital, (float)(60 * u), 0.0, end + 120 * u, ink);
        smallLine(author, reg, (float)(70 * u), 0.18, H * 0.93, accent);
    } else if (layout == "medallion") {
        // The title above, a round picture in the middle with a ring, the author below; fine rules between.
        const double cx = W / 2.0, cy = H * 0.56, rad = 430 * u;
        smallLine(upperCase(sp.series), reg, (float)(38 * u), 0.22, H * 0.07, accent);
        const double end = titleBlock(H * 0.10, W - 240 * u, (float)(170 * u), 2, ink, 0.05);
        (void)end;
        auto inCircle = [&](int x, int y) { const double dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= rad * rad; };
        if (picture) {
            if (!drawPicture(cv, sp.picturePath, (int)(cx - rad), (int)(cy - rad), (int)(2 * rad), (int)(2 * rad), sp.focusY, inCircle)) {
                r.error = "The picture couldn't be read.";
                return false;
            }
        } else {
            const Colour deeper{bg.r * 0.72, bg.g * 0.72, bg.b * 0.72};
            for (int y = (int)(cy - rad); y < (int)(cy + rad); ++y)
                for (int x = (int)(cx - rad); x < (int)(cx + rad); ++x) if (inCircle(x, y)) cv.blend(x, y, deeper, 1);
        }
        cv.ring(cx, cy, rad + 16 * u, 6 * u, accent);
        cv.ring(cx, cy, rad + 34 * u, 2 * u, accent, 1e9, -1e9, 1e9, 0.7);
        cv.fill((int)(120 * u), (int)(cy - 1.5 * u), (int)(cx - rad - 60 * u), (int)(cy + 1.5 * u), accent, 0.8);
        cv.fill((int)(cx + rad + 60 * u), (int)(cy - 1.5 * u), W - (int)(120 * u), (int)(cy + 1.5 * u), accent, 0.8);
        if (!sp.subtitle.empty()) smallLine(sp.subtitle, ital, (float)(60 * u), 0.0, cy + rad + 140 * u, ink);
        smallLine(author, reg, (float)(70 * u), 0.18, H * 0.93, ink);
    } else if (layout == "sidebar") {
        // The picture fills the cover; a column down the left in the background colour holds the words, left-aligned.
        if (picture && !drawPicture(cv, sp.picturePath, 0, 0, W, H, sp.focusY)) { r.error = "The picture couldn't be read."; return false; }
        const int colW = (int)(W * 0.46);
        cv.fill(0, 0, colW, H, bg, picture ? 0.92 : 1.0);
        cv.fill(colW, 0, colW + (int)(10 * u), H, accent);
        const double lx = 90 * u, maxW = colW - 150 * u;
        if (!sp.series.empty()) {
            float s = (float)(34 * u);
            const auto lines = wr.fit(reg, upperCase(sp.series), (float)maxW, s, s * 0.6f, 3, 0.2, s);
            for (size_t i = 0; i < lines.size(); ++i) wr.line(reg, lines[i], s, 0.2, lx, H * 0.09 + i * s * 1.3, accent, false);
        }
        float size = 0;
        const int face = sp.titleCaps ? reg : bold;
        const auto lines = wr.fit(face, title.empty() ? std::string("Untitled") : title, (float)maxW, (float)(150 * u), (float)(50 * u), 6, 0.03, size);
        double y = H * 0.20 + size * 0.78;
        for (auto& l : lines) { wr.line(face, l, size, 0.03, lx, y, ink, false); y += size * 1.1; }
        cv.fill((int)lx, (int)(y + 10 * u), (int)(lx + 160 * u), (int)(y + 16 * u), accent);
        if (!sp.subtitle.empty()) {
            float s = (float)(54 * u);
            const auto sl = wr.fit(ital, sp.subtitle, (float)maxW, s, s * 0.6f, 4, 0, s);
            for (size_t i = 0; i < sl.size(); ++i) wr.line(ital, sl[i], s, 0, lx, y + 120 * u + i * s * 1.25, ink, false);
        }
        {
            float s = (float)(58 * u);
            const auto al = wr.fit(reg, author, (float)maxW, s, s * 0.6f, 2, 0.14, s);
            for (size_t i = 0; i < al.size(); ++i) wr.line(reg, al[i], s, 0.14, lx, H * 0.92 - (al.size() - 1 - i) * s * 1.25, ink, false);
        }
    } else if (layout == "stacked") {
        // Type as the picture: the title's words stacked, each as wide as the cover allows, between thick rules.
        smallLine(author, reg, (float)(64 * u), 0.2, H * 0.085, accent);
        cv.fill((int)(110 * u), (int)(H * 0.12), W - (int)(110 * u), (int)(H * 0.12 + 12 * u), ink);
        std::vector<std::string> words;
        {
            std::istringstream in(title.empty() ? std::string("Untitled") : title);
            for (std::string w; in >> w;) words.push_back(w);
        }
        // Short words join the next ("OF THE") so no line is a lone small word.
        std::vector<std::string> lines;
        for (size_t i = 0; i < words.size(); ++i) {
            if (!lines.empty() && (words[i].size() <= 3 || lines.back().size() <= 3) && lines.size() >= 1 && words[i].size() + lines.back().size() < 9)
                lines.back() += " " + words[i];
            else lines.push_back(words[i]);
        }
        if (lines.size() > 6) {
            float s = 0;
            lines = wr.fit(sp.titleCaps ? reg : bold, title, (float)(W - 240 * u), (float)(200 * u), (float)(60 * u), 6, 0.0, s);
        }
        const int face = sp.titleCaps ? reg : bold;
        const double top = H * 0.16, bottom = H * (sp.subtitle.empty() ? 0.86 : 0.80);
        std::vector<float> sizes;
        double total = 0;
        for (auto& l : lines) {
            const float w100 = wr.width(face, l, 100, 0.02);
            float s = (float)std::min(420.0 * u, (W - 240 * u) / std::max(1.0f, w100) * 100);
            sizes.push_back(s);
            total += s * 0.92;
        }
        const double room = bottom - top;
        const double k = total > room ? room / total : 1.0;
        double y = top + (room - total * k) / 2;
        for (size_t i = 0; i < lines.size(); ++i) {
            const float s = (float)(sizes[i] * k);
            y += s * 0.80;
            wr.line(face, lines[i], s, 0.02, W / 2.0, y, i % 2 ? accent : ink);
            y += s * 0.12;
        }
        cv.fill((int)(110 * u), (int)(bottom + 20 * u), W - (int)(110 * u), (int)(bottom + 32 * u), ink);
        if (!sp.subtitle.empty()) smallLine(sp.subtitle, ital, (float)(60 * u), 0.0, bottom + 130 * u, ink);
        smallLine(upperCase(sp.series), reg, (float)(36 * u), 0.22, H * 0.95, accent);
    } else if (layout == "starfield") {
        // A night sky: the background deepening upwards, scattered stars, a glow of the accent along the horizon.
        const Colour deep{bg.r * 0.45, bg.g * 0.45, bg.b * 0.45};
        cv.shade(0, (int)(H * 0.7), deep, 1.0, 0.0);
        if (picture) {
            const int py0 = (int)(H * 0.62);
            if (!drawPicture(cv, sp.picturePath, 0, py0, W, H - py0, sp.focusY)) { r.error = "The picture couldn't be read."; return false; }
            cv.shade(py0, (int)(py0 + 260 * u), bg, 1.0, 0.0);
            cv.shade((int)(H * 0.84), H, bg, 0.0, 0.85);
        }
        std::uint32_t seed = 2166136261u;
        for (char c : sp.title) seed = (seed ^ (unsigned char)c) * 16777619u;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
        for (int i = 0; i < 420; ++i) {
            const double x = rnd() * W, y = rnd() * H * 0.62, b = 0.25 + 0.75 * rnd() * rnd();
            const double rr = (b > 0.8 ? 3.2 : b > 0.5 ? 2.2 : 1.4) * u;
            cv.disc(x, y, rr, ink, b);
            if (b > 0.85) cv.disc(x, y, rr * 4, ink, 0.07);
        }
        cv.glow(H * 0.62, 240 * u, accent, 0.35);
        smallLine(upperCase(sp.series), reg, (float)(38 * u), 0.24, H * 0.09, accent);
        const double end = titleBlock(H * 0.17, W - 220 * u, (float)(200 * u), 3, ink, 0.06);
        if (!sp.subtitle.empty()) smallLine(sp.subtitle, ital, (float)(62 * u), 0.0, end + 130 * u, ink);
        smallLine(author, reg, (float)(72 * u), 0.2, H * 0.93, ink);
    } else if (layout == "ornate") {
        // A wide border with corner pieces and a diamond at each side's middle, the title in a panel.
        const int m = (int)(60 * u), t = (int)(26 * u);
        cv.fill(m, m, W - m, m + t, accent);
        cv.fill(m, H - m - t, W - m, H - m, accent);
        cv.fill(m, m, m + t, H - m, accent);
        cv.fill(W - m - t, m, W - m, H - m, accent);
        cv.frame(m + t + (int)(16 * u), m + t + (int)(16 * u), W - m - t - (int)(16 * u), H - m - t - (int)(16 * u), std::max(1, (int)(3 * u)), accent, 0.85);
        const double cs = 110 * u;
        for (int cx : {m + t / 2, W - m - t / 2})
            for (int cy : {m + t / 2, H - m - t / 2}) {
                cv.diamond(cx, cy, cs, accent, 1.0);
                cv.diamond(cx, cy, cs * 0.55, bg, 1.0);
                cv.diamond(cx, cy, cs * 0.3, accent, 1.0);
            }
        for (auto [cx, cy] : std::initializer_list<std::pair<double, double>>{{W / 2.0, m + t / 2.0}, {W / 2.0, H - m - t / 2.0}, {m + t / 2.0, H / 2.0}, {W - m - t / 2.0, H / 2.0}}) {
            cv.diamond(cx, cy, 50 * u, accent, 1.0);
            cv.diamond(cx, cy, 24 * u, bg, 1.0);
        }
        if (picture) {
            const int px0 = (int)(250 * u), pw = W - 2 * px0, py0 = (int)(H * 0.48), ph = (int)(H * 0.30);
            if (!drawPicture(cv, sp.picturePath, px0, py0, pw, ph, sp.focusY)) { r.error = "The picture couldn't be read."; return false; }
            cv.frame(px0 - (int)(8 * u), py0 - (int)(8 * u), px0 + pw + (int)(8 * u), py0 + ph + (int)(8 * u), (int)(5 * u), accent);
        }
        smallLine(upperCase(sp.series), reg, (float)(36 * u), 0.22, H * 0.15, accent);
        const double end = titleBlock(H * (picture ? 0.19 : 0.27), W - 420 * u, (float)(180 * u), 3, ink, 0.06);
        if (!sp.subtitle.empty() && !picture) smallLine(sp.subtitle, ital, (float)(62 * u), 0.0, end + 150 * u, ink);
        if (!sp.subtitle.empty() && picture) smallLine(sp.subtitle, ital, (float)(54 * u), 0.0, H * 0.44, ink);
        smallLine(author, reg, (float)(70 * u), 0.2, H * 0.86, ink);
    } else if (layout == "modern") {
        // The picture fills the cover; the words sit at the foot, left-aligned, over a deep shade.
        if (picture && !drawPicture(cv, sp.picturePath, 0, 0, W, H, sp.focusY)) { r.error = "The picture couldn't be read."; return false; }
        if (!picture) cv.shade(0, H, Colour{bg.r * 0.6, bg.g * 0.6, bg.b * 0.6}, 0.0, 1.0);
        cv.shade((int)(H * 0.45), H, bg, 0.0, 0.92);
        cv.shade(0, (int)(H * 0.14), bg, 0.6, 0.0);
        const double lx = 110 * u, maxW = W - 220 * u;
        if (!sp.series.empty()) wr.line(reg, upperCase(sp.series), (float)(36 * u), 0.22, lx, H * 0.07, accent, false);
        float size = 0;
        const int face = sp.titleCaps ? reg : bold;
        const auto lines = wr.fit(face, title.empty() ? std::string("Untitled") : title, (float)maxW, (float)(190 * u), (float)(60 * u), 3, 0.02, size);
        const double lead = size * 1.06;
        const double authorY = H * 0.935;
        double subH = sp.subtitle.empty() ? 0 : 110 * u;
        double y = authorY - 140 * u - subH - (lines.size() - 1) * lead;
        cv.fill((int)lx, (int)(y - size * 0.95), (int)(lx + 200 * u), (int)(y - size * 0.95 + 12 * u), accent);
        for (auto& l : lines) { wr.line(face, l, size, 0.02, lx, y, ink, false); y += lead; }
        if (!sp.subtitle.empty()) wr.line(ital, sp.subtitle, (float)(58 * u), 0, lx, y - lead + 100 * u, ink, false);
        wr.line(reg, author, (float)(62 * u), 0.18, lx, authorY, accent, false);
    } else {
        // Classic: a double frame, the title in the upper half, a short rule, the author at the foot.
        cv.frame((int)(70 * u), (int)(70 * u), W - (int)(70 * u), H - (int)(70 * u), (int)(7 * u), accent);
        cv.frame((int)(96 * u), (int)(96 * u), W - (int)(96 * u), H - (int)(96 * u), std::max(1, (int)(2 * u)), accent, 0.8);
        if (picture) {
            // A picture, if given, sits in a window under the title.
            const int px0 = (int)(220 * u), pw = W - 2 * px0, py0 = (int)(H * 0.47), ph = (int)(H * 0.30);
            if (!drawPicture(cv, sp.picturePath, px0, py0, pw, ph, sp.focusY)) { r.error = "The picture couldn't be read."; return false; }
            cv.frame(px0 - (int)(6 * u), py0 - (int)(6 * u), px0 + pw + (int)(6 * u), py0 + ph + (int)(6 * u), (int)(4 * u), accent);
        }
        smallLine(upperCase(sp.series), reg, (float)(40 * u), 0.22, H * 0.12, accent);
        const double end = titleBlock(H * (picture ? 0.17 : 0.24), W - 380 * u, (float)(200 * u), 3, ink, 0.06);
        cv.fill((int)(W / 2 - 110 * u), (int)(end + 80 * u), (int)(W / 2 + 110 * u), (int)(end + 86 * u), accent);
        if (!sp.subtitle.empty()) smallLine(sp.subtitle, ital, (float)(68 * u), 0.0, end + 190 * u, ink);
        smallLine(author, reg, (float)(76 * u), 0.2, H * 0.875, ink);
    }
    return true;
}

}  // namespace

Result render(const Spec& sp, const std::string& fontsFolder, int W, int H) {
    Result r;
    if (W < 200 || H < 300) { r.error = "The cover is too small to draw."; return r; }
    Canvas cv = blank(W, H, parse(sp.background, {107, 45, 60}));
    if (!drawFront(cv, sp, fontsFolder, r)) return r;
    size_t len = 0;
    if (void* png = tdefl_write_image_to_png_file_in_memory(cv.px.data(), W, H, 3, &len)) {
        r.png.assign(static_cast<const char*>(png), len);
        mz_free(png);
    }
    r.ok = !r.png.empty();
    if (!r.ok) r.error = "The cover couldn't be written.";
    r.width = W;
    r.height = H;
    return r;
}

std::string toJson(const Spec& s) {
    return json{{"layout", s.layout}, {"title", s.title}, {"subtitle", s.subtitle}, {"author", s.author}, {"series", s.series},
                {"background", s.background}, {"ink", s.ink}, {"accent", s.accent}, {"picture", s.picturePath}, {"focusY", s.focusY},
                {"font", s.fontId}, {"titleCaps", s.titleCaps}, {"back", s.backText}}
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
    s.backText = j.value("back", s.backText);
    return s;
}

// ------------------------------------------------------------------ print covers

namespace {

std::string pdfNum(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

// One page holding the picture, the size of the whole cover, with the trim marked (TrimBox).
std::string coverPdf(const Canvas& cv, double wPt, double hPt, double bleedPt, const std::string& title) {
    mz_ulong n = mz_compressBound((mz_ulong)cv.px.size());
    std::string img(n, '\0');
    if (mz_compress2(reinterpret_cast<unsigned char*>(img.data()), &n, cv.px.data(), (mz_ulong)cv.px.size(), 6) != MZ_OK) return {};
    img.resize(n);
    std::string safeTitle;
    for (char c : title) { if (c == '(' || c == ')' || c == '\\') safeTitle += '\\'; if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) safeTitle += c; }
    const std::string content = "q " + pdfNum(wPt) + " 0 0 " + pdfNum(hPt) + " 0 0 cm /Im0 Do Q\n";
    std::vector<std::string> objs = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + pdfNum(wPt) + " " + pdfNum(hPt) + "] /BleedBox [0 0 " + pdfNum(wPt) + " " + pdfNum(hPt) +
            "] /TrimBox [" + pdfNum(bleedPt) + " " + pdfNum(bleedPt) + " " + pdfNum(wPt - bleedPt) + " " + pdfNum(hPt - bleedPt) +
            "] /Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream",
        "<< /Type /XObject /Subtype /Image /Width " + std::to_string(cv.w) + " /Height " + std::to_string(cv.h) +
            " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length " + std::to_string(img.size()) + " >>\nstream\n" + img +
            "\nendstream",
        "<< /Title (" + safeTitle + " - cover) /Producer (NorthRay Writing Studio) >>",
    };
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> at;
    for (size_t i = 0; i < objs.size(); ++i) {
        at.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t a : at) {
        char b[24];
        std::snprintf(b, sizeof b, "%010zu 00000 n \n", a);
        out += b;
    }
    out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R /Info 6 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

}  // namespace

WrapResult renderWrap(const Spec& sp, const Wrap& wp, const std::string& fontsFolder, bool pdf) {
    WrapResult r;
    if (wp.trimW < 2 || wp.trimH < 2 || wp.spine < 0 || wp.bleed < 0 || wp.dpi < 20 || wp.dpi > 600) { r.error = "The cover's measurements aren't right."; return r; }
    const double d = wp.dpi;
    const int bleed = (int)std::lround(wp.bleed * d), trimW = (int)std::lround(wp.trimW * d), trimH = (int)std::lround(wp.trimH * d);
    const int spine = std::max(1, (int)std::lround(wp.spine * d));
    const int W = 2 * (bleed + trimW) + spine, H = trimH + 2 * bleed;
    const Colour bg = parse(sp.background, {107, 45, 60}), ink = parse(sp.ink, {244, 235, 221}), accent = parse(sp.accent, {217, 178, 107});
    Canvas cv = blank(W, H, bg);

    // The front: drawn on its own, then placed at the right (it runs into the bleed at the top, bottom and outside).
    {
        Canvas front = blank(trimW + bleed, H, bg);
        Result fr;
        if (!drawFront(front, sp, fontsFolder, fr)) { r.error = fr.error; return r; }
        const int x0 = bleed + trimW + spine;
        for (int y = 0; y < H; ++y) std::copy_n(&front.px[(size_t)y * front.w * 3], (size_t)front.w * 3, &cv.px[((size_t)y * W + x0) * 3]);
    }
    Writer wr{cv, {}, "en"};
    openFamily(fontsFolder, sp.fontId.empty() ? "crimson-text" : sp.fontId, wr.fam);
    const int reg = wr.fam.regular, ital = wr.fam.pick(false, true);

    // The back: the blurb at the top, a rule, and the space printers put the barcode in.
    {
        const double inset = 0.6 * d;
        const double x0 = bleed + inset, top = bleed + 0.85 * d, maxW = trimW - 2 * inset;
        const float size = (float)(0.16 * d);
        double y = top;
        if (!sp.series.empty()) {
            wr.line(reg, upperCase(sp.series), (float)(0.11 * d), 0.2, bleed + trimW / 2.0, bleed + 0.55 * d, accent);
        }
        for (const std::string& l : wr.wrap(ital >= 0 && wp.backItalic ? ital : reg, wp.backText, size, (float)maxW)) {
            if (!l.empty()) wr.line(ital >= 0 && wp.backItalic ? ital : reg, l, size, 0, x0, y, ink, false);
            y += size * (l.empty() ? 0.8 : 1.45);
            if (y > H - bleed - 2.0 * d) break;   // never into the barcode's space
        }
        if (!wp.backText.empty()) cv.fill((int)(bleed + trimW / 2.0 - 0.4 * d), (int)(y + 0.15 * d), (int)(bleed + trimW / 2.0 + 0.4 * d), (int)(y + 0.15 * d + std::max(2.0, 0.012 * d)), accent);
        if (wp.barcodeArea) {
            // 2 x 1.2 in, white, 0.4 in in from the trim: left clear for the ISBN barcode the printer adds.
            const int bw = (int)(2.0 * d), bh = (int)(1.2 * d);
            const int bx = bleed + trimW - (int)(0.4 * d) - bw, by = bleed + trimH - (int)(0.4 * d) - bh;
            cv.fill(bx, by, bx + bw, by + bh, {255, 255, 255});
        }
    }

    // The spine: the title and author along it, reading from top to bottom, when there's room.
    r.spineText = false;
    if (wp.spineText && wp.spine >= 0.25 && reg >= 0) {
        Canvas strip = blank(trimH, spine, bg);
        Writer sw{strip, wr.fam, "en"};
        const float size = (float)std::min(spine * 0.42, 0.28 * d);
        const std::string t = sp.titleCaps ? upperCase(sp.title) : sp.title, a = upperCase(sp.author);
        const double base = spine / 2.0 + size * 0.34;
        const double gap = 0.35 * d;
        const float tw = sw.width(reg, t, size, 0.08), aw = a.empty() ? 0 : sw.width(reg, a, size * 0.8f, 0.12);
        if (tw + aw + gap < trimH - 0.8 * d) {
            const double startX = (trimH - (tw + (aw > 0 ? aw + gap : 0))) / 2;
            sw.line(reg, t, size, 0.08, startX, base, ink, false);
            if (aw > 0) sw.line(reg, a, size * 0.8f, 0.12, startX + tw + gap, base, accent, false);
            // Turned a quarter clockwise onto the cover.
            const int sx0 = bleed + trimW;
            for (int y = 0; y < strip.h; ++y)
                for (int x = 0; x < strip.w; ++x) {
                    const int tx = sx0 + (strip.h - 1 - y), ty = bleed + x;
                    if (tx < 0 || ty < 0 || tx >= W || ty >= H) continue;
                    std::copy_n(&strip.px[((size_t)y * strip.w + x) * 3], 3, &cv.px[((size_t)ty * W + tx) * 3]);
                }
            r.spineText = true;
        }
    }

    r.width = W;
    r.height = H;
    if (pdf) {
        r.pdf = coverPdf(cv, W * 72.0 / d, H * 72.0 / d, bleed * 72.0 / d, sp.title);
        if (r.pdf.empty()) { r.error = "The cover PDF couldn't be written."; return r; }
    } else {
        size_t len = 0;
        if (void* png = tdefl_write_image_to_png_file_in_memory(cv.px.data(), W, H, 3, &len)) {
            r.png.assign(static_cast<const char*>(png), len);
            mz_free(png);
        }
    }
    r.ok = true;
    return r;
}

}  // namespace studio::cover

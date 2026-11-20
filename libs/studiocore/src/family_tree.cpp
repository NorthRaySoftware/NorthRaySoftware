// A family tree as a picture: see studio/family_tree.hpp.
#include "studio/family_tree.hpp"

#include "internal.hpp"
#include "typeset_internal.hpp"

#include "miniz.h"
#include "stb/stb_image.h"   // implemented in typeset_output.cpp

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <cmath>
#include <map>
#include <set>

namespace studio::familytree {

using typeset::detail::Family;
using typeset::detail::Fonts;
using typeset::detail::Shaped;
using typeset::detail::shape;

namespace {

struct RGB { double r = 0, g = 0, b = 0; };
RGB hex(unsigned v) { return {(double)(v >> 16 & 255), (double)(v >> 8 & 255), (double)(v & 255)}; }
RGB mix(RGB a, RGB b, double t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

// An RGB picture with anti-aliased shapes: enough for cards, lines and text.
struct Canvas {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
    Canvas(int W, int H, RGB bg) : w(W), h(H), px((size_t)W * H * 3) {
        for (size_t i = 0; i < (size_t)W * H; ++i) { px[i * 3] = (std::uint8_t)bg.r; px[i * 3 + 1] = (std::uint8_t)bg.g; px[i * 3 + 2] = (std::uint8_t)bg.b; }
    }
    void blend(int x, int y, RGB c, double a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        a = std::min(1.0, a);
        std::uint8_t* p = &px[((size_t)y * w + x) * 3];
        p[0] = (std::uint8_t)std::lround(p[0] + (c.r - p[0]) * a);
        p[1] = (std::uint8_t)std::lround(p[1] + (c.g - p[1]) * a);
        p[2] = (std::uint8_t)std::lround(p[2] + (c.b - p[2]) * a);
    }
    // A rounded rectangle, filled (t <= 0) or outlined with thickness t.
    void round(double x0, double y0, double x1, double y1, double rad, RGB c, double t = 0, double a = 1) {
        for (int y = (int)std::floor(y0) - 1; y <= (int)std::ceil(y1) + 1; ++y)
            for (int x = (int)std::floor(x0) - 1; x <= (int)std::ceil(x1) + 1; ++x) {
                const double cx = std::clamp(x + 0.5, x0 + rad, x1 - rad), cy = std::clamp(y + 0.5, y0 + rad, y1 - rad);
                const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy) - rad;   // < 0 inside
                double cover;
                if (t <= 0) cover = std::clamp(0.5 - d, 0.0, 1.0);
                else cover = std::clamp(std::min(0.5 - d, d + t + 0.5), 0.0, 1.0);
                blend(x, y, c, cover * a);
            }
    }
    void rect(double x0, double y0, double x1, double y1, RGB c, double a = 1) { round(x0, y0, x1, y1, 0, c, 0, a); }
    void disc(double cx, double cy, double r, RGB c, double a = 1) { round(cx - r, cy - r, cx + r, cy + r, r, c, 0, a); }
    void ring(double cx, double cy, double r, RGB c, double t) { round(cx - r - t / 2, cy - r - t / 2, cx + r + t / 2, cy + r + t / 2, r + t / 2, c, t); }
    // A line of thickness t with round ends.
    void line(double ax, double ay, double bx, double by, RGB c, double t) {
        const double minX = std::min(ax, bx) - t, maxX = std::max(ax, bx) + t, minY = std::min(ay, by) - t, maxY = std::max(ay, by) + t;
        const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        for (int y = (int)std::floor(minY); y <= (int)std::ceil(maxY); ++y)
            for (int x = (int)std::floor(minX); x <= (int)std::ceil(maxX); ++x) {
                const double px0 = x + 0.5, py0 = y + 0.5;
                const double u = len2 > 0 ? std::clamp(((px0 - ax) * dx + (py0 - ay) * dy) / len2, 0.0, 1.0) : 0;
                const double d = std::hypot(px0 - (ax + u * dx), py0 - (ay + u * dy));
                blend(x, y, c, std::clamp(t / 2 + 0.5 - d, 0.0, 1.0));
            }
    }
    void dashed(double ax, double ay, double bx, double by, RGB c, double t, double dash) {
        const double len = std::hypot(bx - ax, by - ay);
        if (len <= 0) return;
        for (double s = 0; s < len; s += dash * 2) {
            const double e = std::min(len, s + dash);
            line(ax + (bx - ax) * s / len, ay + (by - ay) * s / len, ax + (bx - ax) * e / len, ay + (by - ay) * e / len, c, t);
        }
    }
    // A picture filling a box (centred, cut to fit), clipped to a circle when `circle`.
    void picture(const std::string& path, double bx, double by, double bw, double bh, bool circle) {
        int iw = 0, ih = 0, n = 0;
        std::string bytes;
        {
            std::ifstream f(studio::detail::u8path(path), std::ios::binary);
            bytes.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        if (bytes.empty()) return;
        unsigned char* data = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), (int)bytes.size(), &iw, &ih, &n, 3);
        if (!data) return;
        const double sc = std::max(bw / iw, bh / ih);
        const double ox = (iw * sc - bw) / 2, oy = std::min((ih * sc - bh) * 0.25, (ih * sc - bh) / 2);   // faces sit high
        for (int y = 0; y < (int)bh; ++y)
            for (int x = 0; x < (int)bw; ++x) {
                double a = 1;
                if (circle) {
                    const double d = std::hypot(x + 0.5 - bw / 2, y + 0.5 - bh / 2);
                    a = std::clamp(bw / 2 + 0.5 - d, 0.0, 1.0);
                    if (a <= 0) continue;
                }
                const int sx = std::clamp((int)((x + ox) / sc), 0, iw - 1), sy = std::clamp((int)((y + oy) / sc), 0, ih - 1);
                const unsigned char* s = data + ((size_t)sy * iw + sx) * 3;
                blend((int)(bx + x), (int)(by + y), RGB{(double)s[0], (double)s[1], (double)s[2]}, a);
            }
        stbi_image_free(data);
    }
};

// Text in one face at `size` pixels: its width, and drawing it with its baseline at y.
struct Ink {
    Canvas& cv;
    float width(int face, const std::string& t, float size) {
        if (face < 0 || t.empty()) return 0;
        std::lock_guard<std::recursive_mutex> g(Fonts::get().lock());
        return shape(face, t, size, false, "en").width;
    }
    void draw(int face, const std::string& t, float size, double x, double y, RGB c) {
        if (face < 0 || t.empty()) return;
        std::lock_guard<std::recursive_mutex> g(Fonts::get().lock());
        const Shaped s = shape(face, t, size, false, "en");
        for (size_t i = 0; i < s.glyphs.size(); ++i) {
            const int fi = s.faces.empty() ? face : s.faces[i];
            FT_Face ft = Fonts::get().face(fi).ft;
            FT_Set_Char_Size(ft, 0, (FT_F26Dot6)std::lround(size * 64), 72, 72);
            const double gx = x + s.x[i], gy = y + (s.dy.empty() ? 0 : s.dy[i]);
            const double fx = std::floor(gx);
            FT_Vector delta{(FT_Pos)std::lround((gx - fx) * 64), 0};
            FT_Set_Transform(ft, nullptr, &delta);
            if (FT_Load_Glyph(ft, s.glyphs[i], FT_LOAD_NO_HINTING | FT_LOAD_RENDER) == 0) {
                const FT_Bitmap& b = ft->glyph->bitmap;
                const int ox = (int)fx + ft->glyph->bitmap_left, oy = (int)std::lround(gy) - ft->glyph->bitmap_top;
                for (unsigned yy = 0; yy < b.rows; ++yy)
                    for (unsigned xx = 0; xx < b.width; ++xx) cv.blend(ox + (int)xx, oy + (int)yy, c, b.buffer[yy * (unsigned)b.pitch + xx] / 255.0);
            }
            FT_Set_Transform(ft, nullptr, nullptr);
        }
    }
    // Words in up to `lines` lines of `maxW`; the last cut short with an ellipsis.
    std::vector<std::string> wrap(int face, const std::string& text, float size, float maxW, int lines) {
        std::vector<std::string> words, out;
        std::string cur;
        for (char ch : text + " ") {
            if (ch == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); } else cur += ch;
        }
        cur.clear();
        for (size_t i = 0; i < words.size(); ++i) {
            const std::string next = cur.empty() ? words[i] : cur + " " + words[i];
            if (!cur.empty() && width(face, next, size) > maxW) {
                out.push_back(cur);
                cur = words[i];
                if ((int)out.size() == lines - 1) {
                    for (size_t j = i + 1; j < words.size(); ++j) cur += " " + words[j];
                    break;
                }
            } else cur = next;
        }
        if (!cur.empty()) out.push_back(cur);
        if (!out.empty()) {
            std::string& last = out.back();
            if (width(face, last, size) > maxW) {
                while (!last.empty() && width(face, last + "\xE2\x80\xA6", size) > maxW) {
                    last.pop_back();
                    while (!last.empty() && ((unsigned char)last.back() & 0xC0) == 0x80) last.pop_back();
                }
                last += "\xE2\x80\xA6";
            }
        }
        return out;
    }
};

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// A "Label: value" line's value from an entry's details, for any of `labels` (lower case).
std::string detail(const Entry& e, std::initializer_list<const char*> labels) {
    size_t at = 0;
    while (at < e.details.size()) {
        size_t nl = e.details.find('\n', at);
        if (nl == std::string::npos) nl = e.details.size();
        const std::string line = e.details.substr(at, nl - at);
        at = nl + 1;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string lab = line.substr(0, colon), val = line.substr(colon + 1);
        while (!lab.empty() && lab.back() == ' ') lab.pop_back();
        while (!val.empty() && (val.front() == ' ')) val.erase(0, 1);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\r')) val.pop_back();
        for (const char* l : labels) if (lower(lab) == l && !val.empty()) return val;
    }
    return {};
}

std::string initials(const std::string& name) {
    std::string out;
    bool start = true;
    for (unsigned char c : name) {
        if (c == ' ') { start = true; continue; }
        if (start && c < 0x80 && std::isalpha(c) && out.size() < 2) out += (char)std::toupper(c);
        start = false;
    }
    return out.empty() ? std::string("?") : out;
}

RGB genderColour(const std::string& g) {
    const std::string l = lower(g);
    if (l == "female") return hex(0xB4507A);
    if (l == "male") return hex(0x3C72C4);
    if (l == "non-binary" || l == "genderfluid" || l == "agender") return hex(0x8A6A1C);
    if (l == "intersex") return hex(0x6E4FB8);
    return hex(0x7A7F8C);
}

void genderSign(Canvas& cv, double cx, double cy, double s, const std::string& g, RGB c) {
    const std::string l = lower(g);
    const double r = s / 2, t = std::max(1.4, s / 8);
    if (l == "female") {
        cy -= s * 0.28;
        cv.ring(cx, cy, r, c, t);
        cv.line(cx, cy + r, cx, cy + r + s * 0.6, c, t);
        cv.line(cx - s * 0.28, cy + r + s * 0.32, cx + s * 0.28, cy + r + s * 0.32, c, t);
    } else if (l == "male") {
        cx -= s * 0.18; cy += s * 0.18;
        cv.ring(cx, cy, r, c, t);
        const double a = 0.7071, sx = cx + r * a, sy = cy - r * a, ex = sx + s * 0.42, ey = sy - s * 0.42;
        cv.line(sx, sy, ex, ey, c, t);
        cv.line(ex - s * 0.34, ey, ex, ey, c, t);
        cv.line(ex, ey, ex, ey + s * 0.34, c, t);
    } else if (l == "non-binary" || l == "genderfluid" || l == "agender" || l == "intersex") {
        cy += s * 0.12;
        cv.ring(cx, cy, r, c, t);
        cv.line(cx, cy - r, cx, cy - r - s * 0.6, c, t);
        cv.line(cx - s * 0.24, cy - r - s * 0.3, cx + s * 0.24, cy - r - s * 0.3, c, t);
    }
}

}  // namespace

std::string title(const Library& lib, const Id& root, bool bloodline) {
    const Entry* r = lib.entry(root);
    if (!r) return {};
    if (!bloodline) return r->name + "'s family tree";
    for (const auto& g : lib.families(r->worldId, r->worldId.empty() ? r->bookId : Id{}))
        if (g.root == root && !g.name.empty()) return g.name;   // told apart from a family with the same surname
    const auto lay = lib.familyLayout(root, 150, true);
    std::map<std::string, int> surnames;
    for (auto& p : lay.people)
        if (const Entry* x = lib.entry(p.id); x && !p.inLaw)
            if (const size_t sp = x->name.rfind(' '); sp != std::string::npos && sp + 1 < x->name.size()) ++surnames[x->name.substr(sp + 1)];
    std::string surname;
    int best = 1;
    for (auto& [sn, n] : surnames) if (n > best) { best = n; surname = sn; }
    return surname.empty() ? r->name + "'s family" : "The " + surname + " family";
}

Picture render(const Library& lib, const Id& root, const std::string& fontsFolder, const Options& o) {
    Picture out;
    const Entry* rootE = lib.entry(root);
    if (!rootE) { out.error = "That person isn't in the bible."; return out; }
    const auto lay = lib.familyLayout(root, 150, o.bloodline);
    if (lay.people.empty()) { out.error = "There's no family to draw."; return out; }
    out.people = (int)lay.people.size();

    // Faces: Gentium Book Plus (bold for names), from the app's fonts.
    Family fam;
    if (!typeset::detail::openFamily(fontsFolder, "gentium-book-plus", fam) && !typeset::detail::openFamily(fontsFolder, "crimson-text", fam)) {
        out.error = "The fonts weren't found next to the program.";
        return out;
    }
    const int regular = fam.regular, bold = fam.bold >= 0 ? fam.bold : fam.regular, italic = fam.italic >= 0 ? fam.italic : fam.regular;

    const double S = o.scale;
    const double cardW = 196 * S, cardH = 80 * S, gapX = 26 * S, gapY = 70 * S, margin = 36 * S;
    const double headH = o.heading ? 70 * S : 0, keyH = o.key ? 40 * S : 0;
    const int gens = lay.maxGeneration - lay.minGeneration + 1;
    const double treeW = lay.width * (cardW + gapX) - gapX, treeH = gens * cardH + (gens - 1) * gapY;
    const int W = (int)std::ceil(std::max(treeW, (o.key ? 700 : 420) * S) + margin * 2);
    const int H = (int)std::ceil(treeH + margin * 2 + headH + keyH);
    if ((double)W * H > 60e6) { out.error = "The tree is too big to draw as one picture."; return out; }
    const RGB paper = hex(0xFFFFFF), ink = hex(0x1B2030), muted = hex(0x5B6170), faint = hex(0x8A8F9C), rule = hex(0xD5D9E2);
    const RGB lineC = hex(0x8F96A5);
    Canvas cv(W, H, paper);
    Ink tx{cv};

    // The heading.
    if (o.heading) {
        tx.draw(bold, title(lib, root, o.bloodline), (float)(22 * S), margin, margin + 22 * S, ink);
        const std::string sub = std::to_string(lay.people.size()) + (lay.people.size() == 1 ? " person" : " people") + " across " +
                                std::to_string(gens) + (gens == 1 ? " generation" : " generations");
        tx.draw(regular, sub, (float)(12.5 * S), margin, margin + 44 * S, muted);
    }
    const double x0 = margin + (std::max(treeW, (o.key ? 700 : 420) * S) - treeW) / 2, y0 = margin + headH;
    std::map<Id, std::array<double, 4>> at;   // x, y, w, h
    for (auto& p : lay.people) at[p.id] = {x0 + p.x * (cardW + gapX), y0 + (p.generation - lay.minGeneration) * (cardH + gapY), cardW, cardH};
    auto right = [&](const Id& id) { return at[id][0] + at[id][2]; };

    // Partners: side by side, a line between (two for a marriage).
    std::set<std::pair<Id, Id>> couples;
    for (auto& [a, b] : lay.couples) {
        couples.insert({std::min(a, b), std::max(a, b)});
        auto ra = at[a], rb = at[b];
        if (std::fabs(ra[1] - rb[1]) > 1) continue;
        if (ra[0] > rb[0]) std::swap(ra, rb);
        const double cy = ra[1] + ra[3] / 2;
        bool married = false;
        for (const EntryLink& l : lib.linksOf(a)) if (l.label == "Married to" && (l.from == b || l.to == b)) married = true;
        if (married) {
            cv.line(ra[0] + ra[2], cy - 2.2 * S, rb[0], cy - 2.2 * S, lineC, 1.5 * S);
            cv.line(ra[0] + ra[2], cy + 2.2 * S, rb[0], cy + 2.2 * S, lineC, 1.5 * S);
        } else {
            cv.line(ra[0] + ra[2], cy, rb[0], cy, lineC, 1.5 * S);
        }
    }
    // Children by their parents; two parents in a row who aren't partners are joined by a dashed line.
    // A child hangs from their main parents (birth, else adoptive, foster, step); other parents get a dotted line of
    // their own. Adopted, step and foster lines are dotted and labelled; twins come down from one point (0.46).
    std::map<std::vector<Id>, std::vector<Id>> byParents;
    std::vector<std::pair<Id, Id>> otherParents;
    auto howOf = [&](const Id& par, const Id& ch) {
        const auto it = lay.parentHow.find({par, ch});
        return it == lay.parentHow.end() ? std::string() : it->second;
    };
    auto rankHow = [](const std::string& h) { return h.empty() ? 0 : h == "adoptive" ? 1 : h == "foster" ? 2 : 3; };
    bool kinds = false;
    {
        std::map<Id, std::vector<Id>> parentsOf;
        for (auto& [par, ch] : lay.parentChild) parentsOf[ch].push_back(par);
        for (auto& [ch, ps] : parentsOf) {
            int best = 9;
            for (const Id& p : ps) best = std::min(best, rankHow(howOf(p, ch)));
            std::vector<Id> mainPs;
            for (const Id& p : ps) {
                if (rankHow(howOf(p, ch)) == best) mainPs.push_back(p); else otherParents.push_back({p, ch});
                if (!howOf(p, ch).empty()) kinds = true;
            }
            std::sort(mainPs.begin(), mainPs.end());
            byParents[mainPs].push_back(ch);
        }
    }
    std::set<std::pair<Id, Id>> twinPairs;
    for (auto& sb : lay.siblings) {
        if (sb.how == "twin") { twinPairs.insert({sb.a, sb.b}); twinPairs.insert({sb.b, sb.a}); }
        if (!sb.sharedParents && sb.how != "" && sb.how != "twin" && sb.how != "half") kinds = true;
    }
    auto dots = [&](double ax, double ay, double bx, double by) { cv.dashed(ax, ay, bx, by, lineC, 1.5 * S, 2.2 * S); };
    auto tagOf = [](const std::string& h) { return h == "adoptive" ? std::string("adopted") : h == "step" ? std::string("step")
                                                  : h == "foster" ? std::string("foster") : std::string(); };
    const float tagSz = (float)(10.5 * S);
    // Several sets of parents over one row of children: each set's line at its own height, so they don't run together.
    std::map<std::vector<Id>, double> busShift;
    {
        std::map<long, std::vector<std::pair<double, std::vector<Id>>>> rows;
        for (auto& [ps, kids] : byParents) {
            double top = 1e18, px = 0;
            for (const Id& k : kids) top = std::min(top, at[k][1]);
            for (const Id& p : ps) px += at[p][0];
            rows[std::lround(top)].push_back({ps.empty() ? 0 : px / ps.size(), ps});
        }
        for (auto& [row, gs] : rows) {
            std::sort(gs.begin(), gs.end());
            const double step = std::min(9 * S, gapY * 0.55 / std::max<size_t>(1, gs.size()));
            for (size_t i = 0; i < gs.size(); ++i) busShift[gs[i].second] = ((double)i - (gs.size() - 1) / 2.0) * step;
        }
    }
    for (auto& [ps, kids] : byParents) {
        double ox = 0, oy = 0;
        bool pair = ps.size() == 2 && std::fabs(at[ps[0]][1] - at[ps[1]][1]) < 1;
        if (pair) {
            auto a = at[ps[0]], b = at[ps[1]];
            if (a[0] > b[0]) std::swap(a, b);
            oy = a[1] + a[3] / 2;
            ox = (a[0] + a[2] + b[0]) / 2;
            if (!couples.count({std::min(ps[0], ps[1]), std::max(ps[0], ps[1])})) cv.dashed(a[0] + a[2], oy, b[0], oy, lineC, 1.5 * S, 5 * S);
        } else {
            double sx = 0, by = 0;
            for (const Id& p : ps) { sx += at[p][0] + at[p][2] / 2; by = std::max(by, at[p][1] + at[p][3]); }
            ox = sx / ps.size();
            oy = by;
        }
        double minX = ox, maxX = ox, topY = 1e18;
        for (const Id& k : kids) {
            double kx = at[k][0] + at[k][2] / 2;
            for (const Id& o2 : kids) if (o2 != k && twinPairs.count({k, o2})) kx = (kx + at[o2][0] + at[o2][2] / 2) / 2;
            minX = std::min(minX, kx);
            maxX = std::max(maxX, kx);
            topY = std::min(topY, at[k][1]);
        }
        const double bus = topY - gapY / 2 + busShift[ps];
        cv.line(ox, oy, ox, bus, lineC, 1.5 * S);
        cv.line(minX, bus, maxX, bus, lineC, 1.5 * S);
        for (const Id& k : kids) {
            const double kx = at[k][0] + at[k][2] / 2, ky = at[k][1];
            double fromX = kx;
            for (const Id& o2 : kids) if (o2 != k && twinPairs.count({k, o2})) fromX = (kx + at[o2][0] + at[o2][2] / 2) / 2;
            const std::string how = ps.empty() ? std::string() : howOf(ps[0], k);
            const double midY = bus + (ky - bus) * 0.45;
            if (how.empty()) {
                if (fromX != kx) { cv.line(fromX, bus, kx, midY, lineC, 1.5 * S); cv.line(kx, midY, kx, ky, lineC, 1.5 * S); }
                else cv.line(kx, bus, kx, ky, lineC, 1.5 * S);
            } else {
                if (fromX != kx) { dots(fromX, bus, kx, midY); dots(kx, midY, kx, ky); }
                else dots(kx, bus, kx, ky);
                tx.draw(italic, tagOf(how), tagSz, kx + 5 * S, bus + 13 * S, muted);
            }
        }
    }
    for (auto& [par, ch] : otherParents) {
        if (!at.count(par) || !at.count(ch)) continue;
        const auto pr = at[par], kr = at[ch];
        const double px = pr[0] + pr[2] / 2 + 14 * S, kx = kr[0] + kr[2] / 2 + 14 * S, midY = kr[1] - gapY * 0.28;
        dots(px, pr[1] + pr[3], px, midY);
        dots(px, midY, kx, midY);
        dots(kx, midY, kx, kr[1]);
        tx.draw(italic, tagOf(howOf(par, ch)), tagSz, kx + 5 * S, midY + 13 * S, muted);
    }
    for (auto& sb : lay.siblings) {
        if (sb.sharedParents || !at.count(sb.a) || !at.count(sb.b)) continue;
        auto ra = at[sb.a], rb = at[sb.b];
        if (ra[0] > rb[0]) std::swap(ra, rb);
        const double ax = ra[0] + ra[2] / 2, bx = rb[0] + rb[2] / 2, topY = ra[1] - gapY * 0.3;
        const bool plain = sb.how.empty() || sb.how == "twin" || sb.how == "half";
        if (plain) { cv.line(ax, ra[1], ax, topY, lineC, 1.5 * S); cv.line(ax, topY, bx, topY, lineC, 1.5 * S); cv.line(bx, topY, bx, rb[1], lineC, 1.5 * S); }
        else { dots(ax, ra[1], ax, topY); dots(ax, topY, bx, topY); dots(bx, topY, bx, rb[1]); }
        const std::string tag = sb.how == "twin" ? "twins" : sb.how == "half" ? "half-siblings" : sb.how == "step" ? "step-siblings"
                              : sb.how == "adoptive" ? "adoptive siblings" : sb.how == "foster" ? "foster siblings" : "siblings";
        tx.draw(italic, tag, tagSz, (ax + bx) / 2 - tx.width(italic, tag, tagSz) / 2, topY - 5 * S, muted);
    }
    // The people.
    for (auto& p : lay.people) {
        const Entry* x = lib.entry(p.id);
        if (!x) continue;
        const auto r = at[p.id];
        const std::string g = detail(*x, {"gender", "sex", "gender/sex", "gender or sex"});
        const RGB gc = genderColour(g);
        // A soft shadow, the card, its band.
        cv.round(r[0] + 1 * S, r[1] + 2 * S, r[0] + r[2] + 1 * S, r[1] + r[3] + 3 * S, 8 * S, hex(0x000000), 0, 0.06);
        cv.round(r[0], r[1], r[0] + r[2], r[1] + r[3], 8 * S, p.inLaw ? hex(0xF6F7FA) : paper);
        cv.round(r[0], r[1], r[0] + r[2], r[1] + r[3], 8 * S, p.id == root ? hex(0x0F8F6E) : mix(rule, gc, 0.45), p.id == root ? 2.4 * S : 1.2 * S);
        cv.rect(r[0] + 1.5 * S, r[1] + 10 * S, r[0] + 5 * S, r[1] + r[3] - 10 * S, gc, p.inLaw ? 0.45 : 1);
        // Picture or initials.
        const double pd = 52 * S, px = r[0] + 14 * S, py = r[1] + (r[3] - pd) / 2;
        bool pic = false;
        if (!x->imageBlob.empty()) {
            const std::string path = lib.blobPath(x->imageBlob);
            if (!path.empty()) { cv.picture(path, px, py, pd, pd, true); pic = true; }
        }
        if (!pic) {
            cv.disc(px + pd / 2, py + pd / 2, pd / 2, mix(paper, gc, 0.16));
            const std::string in = initials(x->name);
            const float isz = (float)(17 * S);
            tx.draw(bold, in, isz, px + pd / 2 - tx.width(bold, in, isz) / 2, py + pd / 2 + isz * 0.36, mix(gc, ink, 0.35));
        }
        cv.ring(px + pd / 2, py + pd / 2, pd / 2, mix(paper, gc, 0.5), 1.2 * S);
        // Name on up to two lines, then a sign and a detail (or "married in").
        const double nx = px + pd + 12 * S, nw = r[0] + r[2] - 10 * S - nx;
        const float nsz = (float)(14 * S), ssz = (float)(11.5 * S);
        const auto lines = tx.wrap(bold, x->name, nsz, (float)nw, 2);
        double ny = r[1] + (lines.size() > 1 ? 24 : 30) * S;
        for (auto& l : lines) { tx.draw(bold, l, nsz, nx, ny, ink); ny += 17 * S; }
        std::string second;
        if (p.inLaw) second = p.joined == "married" ? "married in" : p.joined == "partner" ? "partner" : "other parent";
        else {
            second = detail(*x, {"age"});
            if (!second.empty()) second = "Age " + second;
            if (second.empty()) { const std::string b = detail(*x, {"born", "birth", "birthday", "date of birth"}); if (!b.empty()) second = "Born " + b; }
            if (second.empty()) second = detail(*x, {"title", "occupation", "role"});
        }
        double sx = nx;
        const double ly = r[1] + r[3] - 14 * S;
        if (!g.empty() && !p.inLaw) { genderSign(cv, sx + 6 * S, ly - 4 * S, 9 * S, g, gc); sx += 18 * S; }
        if (!second.empty()) {
            const auto s2 = tx.wrap(p.inLaw ? italic : regular, second, ssz, (float)(r[0] + r[2] - 10 * S - sx), 1);
            if (!s2.empty()) tx.draw(p.inLaw ? italic : regular, s2[0], ssz, sx, ly, muted);
        }
    }
    // The key.
    if (o.key) {
        const double ky = H - margin - 8 * S;
        double kx = margin;
        const float ksz = (float)(11 * S);
        auto item = [&](int kind, const std::string& label) {
            if (kind == 0) cv.line(kx, ky - 4 * S, kx + 26 * S, ky - 4 * S, lineC, 1.5 * S);
            if (kind == 1) { cv.line(kx, ky - 6.2 * S, kx + 26 * S, ky - 6.2 * S, lineC, 1.5 * S); cv.line(kx, ky - 1.8 * S, kx + 26 * S, ky - 1.8 * S, lineC, 1.5 * S); }
            if (kind == 2) cv.dashed(kx, ky - 4 * S, kx + 26 * S, ky - 4 * S, lineC, 1.5 * S, 5 * S);
            if (kind == 4) cv.dashed(kx, ky - 4 * S, kx + 26 * S, ky - 4 * S, lineC, 1.5 * S, 2.2 * S);
            if (kind == 3) cv.rect(kx + 8 * S, ky - 12 * S, kx + 18 * S, ky + 2 * S, hex(0xF6F7FA)), cv.round(kx + 8 * S, ky - 12 * S, kx + 18 * S, ky + 2 * S, 2 * S, rule, 1 * S);
            kx += 32 * S;
            tx.draw(regular, label, ksz, kx, ky, faint);
            kx += tx.width(regular, label, ksz) + 22 * S;
        };
        item(0, "partners");
        item(1, "married");
        item(2, "parents, not partners");
        item(3, "from another family");
        if (kinds) item(4, "adopted, step or foster");
    }
    // Turned a quarter for a book's page when much wider than tall.
    int OW = W, OH = H;
    std::vector<std::uint8_t> pxOut;
    if (o.turnWide && W > H * 1.3) {
        out.turned = true;
        OW = H;
        OH = W;
        pxOut.resize(cv.px.size());
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                // clockwise: (x, y) -> (H - 1 - y, x)
                const int nx = H - 1 - y, ny = x;
                std::memcpy(&pxOut[((size_t)ny * OW + nx) * 3], &cv.px[((size_t)y * W + x) * 3], 3);
            }
    } else {
        pxOut.swap(cv.px);
    }
    size_t len = 0;
    if (void* png = tdefl_write_image_to_png_file_in_memory(pxOut.data(), OW, OH, 3, &len)) {
        out.png.assign(static_cast<const char*>(png), len);
        mz_free(png);
    }
    out.width = OW;
    out.height = OH;
    out.ok = !out.png.empty();
    if (!out.ok) out.error = "The picture couldn't be made.";
    return out;
}

}  // namespace studio::familytree

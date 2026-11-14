#include "ncap/placement.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ncap {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kMinSize = 2;

Vec2 rotate(Vec2 v, float degrees) {
    const float a = degrees * kPi / 180, c = std::cos(a), s = std::sin(a);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

// Which edges a handle moves.
struct Edges { bool left, top, right, bottom; };
Edges edgesOf(Handle h) {
    switch (h) {
        case Handle::TopLeft: return {true, true, false, false};
        case Handle::Top: return {false, true, false, false};
        case Handle::TopRight: return {false, true, true, false};
        case Handle::Right: return {false, false, true, false};
        case Handle::BottomRight: return {false, false, true, true};
        case Handle::Bottom: return {false, false, false, true};
        case Handle::BottomLeft: return {true, false, false, true};
        case Handle::Left: return {true, false, false, false};
        default: return {false, false, false, false};
    }
}

// A handle's position within the frame, 0..1 each way.
Vec2 handleUnit(Handle h) {
    switch (h) {
        case Handle::TopLeft: return {0, 0};
        case Handle::Top: return {0.5f, 0};
        case Handle::TopRight: return {1, 0};
        case Handle::Right: return {1, 0.5f};
        case Handle::BottomRight: return {1, 1};
        case Handle::Bottom: return {0.5f, 1};
        case Handle::BottomLeft: return {0, 1};
        case Handle::Left: return {0, 0.5f};
        default: return {0.5f, 0.5f};
    }
}

bool isCorner(Handle h) { return h == Handle::TopLeft || h == Handle::TopRight || h == Handle::BottomRight || h == Handle::BottomLeft; }

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

float normaliseDegrees(float d) {
    d = std::fmod(d, 360.0f);
    if (d < 0) d += 360;
    if (d >= 359.999f) d = 0;
    return d;
}

// The size of a w×h frame's bounding box once rotated.
Vec2 rotatedExtent(float w, float h, float degrees) {
    const float a = degrees * kPi / 180, c = std::fabs(std::cos(a)), s = std::fabs(std::sin(a));
    return {w * c + h * s, w * s + h * c};
}

}  // namespace

Vec2 croppedSize(const Transform& t, float sw, float sh) {
    if (sw <= 0 || sh <= 0) return {0, 0};
    return {std::max(1.0f, sw - t.cropLeft - t.cropRight), std::max(1.0f, sh - t.cropTop - t.cropBottom)};
}

Box frameOf(const Transform& t, float sw, float sh) {
    const Vec2 c = croppedSize(t, sw, sh);
    return {t.x, t.y, t.width > 0 ? t.width : c.x, t.height > 0 ? t.height : c.y};
}

void cornersOf(const Transform& t, float sw, float sh, Vec2 out[4]) {
    const Box f = frameOf(t, sw, sh);
    const Vec2 c{f.x + f.w / 2, f.y + f.h / 2};
    const Vec2 local[4] = {{-f.w / 2, -f.h / 2}, {f.w / 2, -f.h / 2}, {f.w / 2, f.h / 2}, {-f.w / 2, f.h / 2}};
    for (int i = 0; i < 4; ++i) {
        const Vec2 r = rotate(local[i], t.rotation);
        out[i] = {c.x + r.x, c.y + r.y};
    }
}

Box boundsOf(const Transform& t, float sw, float sh) {
    Vec2 p[4];
    cornersOf(t, sw, sh, p);
    float x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;
    for (auto& q : p) {
        x0 = std::min(x0, q.x);
        x1 = std::max(x1, q.x);
        y0 = std::min(y0, q.y);
        y1 = std::max(y1, q.y);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

bool hits(const Transform& t, float sw, float sh, Vec2 p) {
    const Box f = frameOf(t, sw, sh);
    if (f.w <= 0 || f.h <= 0) return false;
    const Vec2 c{f.x + f.w / 2, f.y + f.h / 2};
    const Vec2 l = rotate({p.x - c.x, p.y - c.y}, -t.rotation);
    return std::fabs(l.x) <= f.w / 2 && std::fabs(l.y) <= f.h / 2;
}

Id itemAt(const std::vector<SceneItem>& items, const std::vector<ItemSize>& sizes, Vec2 p) {
    for (size_t i = items.size(); i-- > 0;) {
        if (!items[i].visible || i >= sizes.size()) continue;
        if (hits(items[i].transform, sizes[i].w, sizes[i].h, p)) return items[i].id;
    }
    return {};
}

Vec2 handlePoint(const Transform& t, float sw, float sh, Handle h) {
    const Box f = frameOf(t, sw, sh);
    const Vec2 u = handleUnit(h);
    const Vec2 c{f.x + f.w / 2, f.y + f.h / 2};
    const Vec2 r = rotate({(u.x - 0.5f) * f.w, (u.y - 0.5f) * f.h}, t.rotation);
    return {c.x + r.x, c.y + r.y};
}

Handle handleAt(const Transform& t, float sw, float sh, Vec2 p, float radius) {
    const Box f = frameOf(t, sw, sh);
    if (f.w <= 0 || f.h <= 0) return Handle::None;
    // Corners first: on a small item they win over the edge handles beside them.
    const Handle order[] = {Handle::TopLeft, Handle::TopRight, Handle::BottomRight, Handle::BottomLeft,
                            Handle::Top,     Handle::Right,    Handle::Bottom,      Handle::Left};
    Handle best = Handle::None;
    float bestD = radius * radius;
    for (Handle h : order) {
        const Vec2 q = handlePoint(t, sw, sh, h);
        const float d = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
        if (d <= bestD && (best == Handle::None || d < bestD)) {
            best = h;
            bestD = d;
        }
    }
    return best;
}

int handleCursor(Handle h, float rotation) {
    float base;
    switch (h) {
        case Handle::Left: case Handle::Right: base = 0; break;
        case Handle::Top: case Handle::Bottom: base = 90; break;
        case Handle::TopLeft: case Handle::BottomRight: base = 45; break;
        case Handle::TopRight: case Handle::BottomLeft: base = 135; break;
        default: return 0;
    }
    float a = std::fmod(base + rotation, 180.0f);
    if (a < 0) a += 180;
    const int step = (int)std::lround(a / 45.0f) % 4;   // 0, 45, 90, 135
    static const int cursor[4] = {0, 2, 1, 3};
    return cursor[step];
}

Transform resizeBy(const Transform& start, float sw, float sh, Handle h, Vec2 delta, bool free) {
    const Box f = frameOf(start, sw, sh);
    Transform t = start;
    if (h == Handle::None || f.w <= 0 || f.h <= 0) return t;
    const Edges e = edgesOf(h);
    const Vec2 d = rotate(delta, -start.rotation);   // the drag in the frame's own directions
    float l = 0, top = 0, r = f.w, b = f.h;
    if (e.left) l = std::min(l + d.x, r - kMinSize);
    if (e.right) r = std::max(r + d.x, l + kMinSize);
    if (e.top) top = std::min(top + d.y, b - kMinSize);
    if (e.bottom) b = std::max(b + d.y, top + kMinSize);
    if (isCorner(h) && !free) {
        // Keep the shape: the larger change wins.
        const float sx = (r - l) / f.w, sy = (b - top) / f.h;
        const float s = std::max(std::fabs(sx - 1) >= std::fabs(sy - 1) ? sx : sy, kMinSize / std::min(f.w, f.h));
        const float nw = f.w * s, nh = f.h * s;
        if (e.left) l = r - nw; else r = l + nw;
        if (e.top) top = b - nh; else b = top + nh;
    }
    // The fixed point (the opposite corner or edge) stays where it was on the canvas.
    const Vec2 u = handleUnit(h);
    const Vec2 anchor{(1 - u.x) * f.w, (1 - u.y) * f.h};   // in the old frame, from its top-left
    const Vec2 oldCentre{f.x + f.w / 2, f.y + f.h / 2};
    const Vec2 ar = rotate({anchor.x - f.w / 2, anchor.y - f.h / 2}, start.rotation);
    const Vec2 anchorCanvas{oldCentre.x + ar.x, oldCentre.y + ar.y};
    // Edges that don't move keep the anchor's local coordinate; work out the new centre from it.
    const Vec2 newCentreLocal{(l + r) / 2, (top + b) / 2};
    const Vec2 cr = rotate({newCentreLocal.x - anchor.x, newCentreLocal.y - anchor.y}, start.rotation);
    const Vec2 centre{anchorCanvas.x + cr.x, anchorCanvas.y + cr.y};
    t.width = r - l;
    t.height = b - top;
    t.x = centre.x - t.width / 2;
    t.y = centre.y - t.height / 2;
    return t;
}

Transform cropBy(const Transform& start, float sw, float sh, Handle h, Vec2 delta) {
    const Box f = frameOf(start, sw, sh);
    const Vec2 c = croppedSize(start, sw, sh);
    if (h == Handle::None || f.w <= 0 || f.h <= 0 || c.x <= 0) return start;
    const float sx = c.x / f.w, sy = c.y / f.h;   // source pixels per frame pixel
    const Edges e = edgesOf(h);
    // Which side of the source each frame edge is (mirroring swaps them).
    const float srcLeftOfFrameLeft = start.flipH ? start.cropRight : start.cropLeft;
    const float srcRightOfFrameRight = start.flipH ? start.cropLeft : start.cropRight;
    const float srcTopOfFrameTop = start.flipV ? start.cropBottom : start.cropTop;
    const float srcBottomOfFrameBottom = start.flipV ? start.cropTop : start.cropBottom;
    Vec2 d = rotate(delta, -start.rotation);
    // Inward movement (positive crops more), limited both ways.
    auto limit = [](float inward, float uncropMax, float cropMax) { return std::clamp(inward, -uncropMax, cropMax); };
    const float roomX = (c.x - 1) / sx, roomY = (c.y - 1) / sy;
    float inL = 0, inR = 0, inT = 0, inB = 0;
    if (e.left) inL = limit(d.x, srcLeftOfFrameLeft / sx, roomX);
    if (e.right) inR = limit(-d.x, srcRightOfFrameRight / sx, roomX);
    if (e.top) inT = limit(d.y, srcTopOfFrameTop / sy, roomY);
    if (e.bottom) inB = limit(-d.y, srcBottomOfFrameBottom / sy, roomY);
    d = {e.left ? inL : -inR, e.top ? inT : -inB};
    Transform t = resizeBy(start, sw, sh, h, rotate(d, start.rotation), true);
    // The frame's edges moved inward by these amounts; the source loses the same, scaled.
    const float fl = inL * sx, fr = inR * sx, ft = inT * sy, fb = inB * sy;
    t.cropLeft = std::max(0.0f, start.cropLeft + (start.flipH ? fr : fl));
    t.cropRight = std::max(0.0f, start.cropRight + (start.flipH ? fl : fr));
    t.cropTop = std::max(0.0f, start.cropTop + (start.flipV ? fb : ft));
    t.cropBottom = std::max(0.0f, start.cropBottom + (start.flipV ? ft : fb));
    return t;
}

Vec2 snapOffset(const Box& m, float cw, float ch, float threshold) {
    auto best = [&](const float mine[3], const float targets[3]) {
        float off = 0, dist = threshold;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const float d = targets[j] - mine[i];
                if (std::fabs(d) < dist) {
                    dist = std::fabs(d);
                    off = d;
                }
            }
        return off;
    };
    const float mx[3] = {m.x, m.x + m.w / 2, m.right()}, tx[3] = {0, cw / 2, cw};
    const float my[3] = {m.y, m.y + m.h / 2, m.bottom()}, ty[3] = {0, ch / 2, ch};
    return {best(mx, tx), best(my, ty)};
}

Transform fitToCanvas(const Transform& t, float sw, float sh, float cw, float ch) {
    const Box f = frameOf(t, sw, sh);
    if (f.w <= 0 || f.h <= 0 || cw <= 0 || ch <= 0) return t;
    const Vec2 ext = rotatedExtent(f.w, f.h, t.rotation);
    const float s = std::min(cw / ext.x, ch / ext.y);
    Transform r = t;
    r.width = f.w * s;
    r.height = f.h * s;
    r.x = cw / 2 - r.width / 2;
    r.y = ch / 2 - r.height / 2;
    return r;
}

Transform stretchToCanvas(const Transform& t, float, float, float cw, float ch) {
    Transform r = t;
    const float rot = normaliseDegrees(t.rotation);
    const bool sideways = std::fabs(rot - 90) < 0.5f || std::fabs(rot - 270) < 0.5f;
    r.width = sideways ? ch : cw;
    r.height = sideways ? cw : ch;
    r.x = cw / 2 - r.width / 2;
    r.y = ch / 2 - r.height / 2;
    return r;
}

Transform centreOnCanvas(const Transform& t, float sw, float sh, float cw, float ch) {
    const Box f = frameOf(t, sw, sh);
    Transform r = t;
    r.x = cw / 2 - f.w / 2;
    r.y = ch / 2 - f.h / 2;
    return r;
}

Transform resetTransform(const Transform& t) {
    Transform r;
    r.opacity = t.opacity;
    return r;
}

Transform rotateBy(const Transform& t, float sw, float sh, float degrees) {
    // Fix the size first, so the centre stays put even if the source's size changes later.
    const Box f = frameOf(t, sw, sh);
    Transform r = t;
    if (f.w > 0 && f.h > 0) {
        r.width = f.w;
        r.height = f.h;
    }
    r.rotation = normaliseDegrees(t.rotation + degrees);
    return r;
}

int matchWindow(const WindowIdentity& wanted, const std::vector<WindowIdentity>& candidates) {
    const std::string exe = lower(wanted.exe), cls = wanted.windowClass;
    int best = -1, bestScore = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const WindowIdentity& c = candidates[i];
        if (!exe.empty() && lower(c.exe) != exe) continue;
        int score = 1;
        const bool sameClass = !cls.empty() && c.windowClass == cls;
        if (sameClass) score += 100;
        if (c.title == wanted.title && !wanted.title.empty()) score += 1000;
        else {
            size_t n = 0;
            while (n < c.title.size() && n < wanted.title.size() && c.title[n] == wanted.title[n]) ++n;
            score += (int)std::min<size_t>(n, 90);
        }
        // Without a program to go by, a window class or an exact title is needed.
        if (exe.empty() && !sameClass && c.title != wanted.title) continue;
        if (score > bestScore) {
            bestScore = score;
            best = (int)i;
        }
    }
    return best;
}

}  // namespace ncap

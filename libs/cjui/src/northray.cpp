#include "cjui/northray.hpp"

#include <algorithm>

namespace cjui::northray {

Palette palette(bool dark) {
    Palette p = makePalette(dark);
    if (!dark) {
        p.paper = hexColor("#F3F5F9"); p.surface = hexColor("#FFFFFF"); p.sunken = hexColor("#E8ECF3");
        p.ink = hexColor("#0E1526"); p.inkMuted = hexColor("#4E5A70"); p.inkFaint = hexColor("#8590A4");
        p.rule = hexColor("#D9DEE7"); p.ribbon = hexColor("#0F8F6E"); p.focus = hexColor("#2D5BD8");
    } else {
        p.paper = hexColor("#0D1220"); p.surface = hexColor("#141B2C"); p.sunken = hexColor("#0A0F1A");
        p.ink = hexColor("#E9EEF7"); p.inkMuted = hexColor("#A4AEC2"); p.inkFaint = hexColor("#6B768C");
        p.rule = hexColor("#232C40"); p.ribbon = hexColor("#5BE3B4"); p.focus = hexColor("#8AB4FF");
    }
    return p;
}

void roundCorners(Gfx& g, const Rect& r, float radius, Color around) {
    const float t = radius;
    g.strokeRounded({r.x - t, r.y - t, r.w + 2 * t, r.h + 2 * t}, radius + t / 2, around, t);  // it insets by half the width
}

void nightSky(Gfx& g, const Rect& r, float radius, float intensity, unsigned seed, Color around) {
    g.fillGradient(r, radius, kNightTop, kNightBottom, true);
    g.pushClip(r);
    const float cx = r.x + r.w * 0.5f;
    g.glow(r.x + r.w * 0.18f, r.y + r.h * 0.10f, r.w * 0.55f, r.h * 0.55f, kAuroraTeal.withAlpha(0.32f * intensity));
    g.glow(r.x + r.w * 0.62f, r.y + r.h * 0.00f, r.w * 0.45f, r.h * 0.45f, kAuroraViolet.withAlpha(0.26f * intensity));
    g.glow(cx, r.y - r.h * 0.05f, r.w * 0.25f, r.h * 0.65f, kAuroraGreen.withAlpha(0.22f * intensity));
    unsigned x = seed * 2654435761u + 1;
    auto rnd = [&] { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return (x % 10000) / 10000.0f; };
    const int n = (int)(r.w * r.h / 2600);
    for (int i = 0; i < n; ++i) {
        const float sx = r.x + rnd() * r.w, sy = r.y + rnd() * r.h, s = 0.5f + rnd() * 0.9f;
        g.circle(sx, sy, s, kNightInk.withAlpha(0.18f + 0.5f * rnd()), true);
    }
    g.popClip();
    if (radius > 0 && around.a > 0) roundCorners(g, r, radius, around);
}

void auroraBand(Gfx& g, const Rect& r, bool dark) {
    const Color stops[] = {hexColor("#7CF5B0"), hexColor("#2FC6A0"), hexColor("#3A8FD8"), hexColor("#9B7BE0")};
    const int n = 48;
    for (int i = 0; i < n; ++i) {
        const float t = i / (float)(n - 1) * 3;
        const int k = std::min(2, (int)t);
        const Color c = stops[k].mix(stops[k + 1], t - k);
        g.fill({r.x + r.w * i / n, r.y, r.w / n + 1, r.h}, c.withAlpha(dark ? 0.85f : 0.95f));
    }
}

}  // namespace cjui::northray

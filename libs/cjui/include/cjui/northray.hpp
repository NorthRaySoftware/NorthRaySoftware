// The NorthRay look, shared by every NorthRay app: the palette and the night
// sky with aurora light used for sidebars, banners and the browser's tab strip.
// Mirrors shared/metadata/brand/tokens.json; change them together.
#pragma once

#include "cjui/gfx.hpp"
#include "cjui/window.hpp"

namespace cjui::northray {

// The night: fixed colours, the same in light and dark themes.
inline constexpr Color kNightTop = {0.039f, 0.063f, 0.125f, 1}, kNightBottom = {0.071f, 0.106f, 0.200f, 1};
inline constexpr Color kNightInk = {0.91f, 0.93f, 0.97f, 1}, kNightMuted = {0.62f, 0.67f, 0.77f, 1};
inline constexpr Color kAuroraGreen = {0.36f, 0.89f, 0.71f, 1}, kAuroraTeal = {0.18f, 0.78f, 0.63f, 1};
inline constexpr Color kAuroraBlue = {0.23f, 0.56f, 0.85f, 1}, kAuroraViolet = {0.61f, 0.48f, 0.88f, 1};

// Cool, quiet neutrals for pages; `ribbon` is the accent (aurora green).
Palette palette(bool dark);

// Rounds a rectangle's corners by painting over them with what's around it,
// which works on every Direct2D (some don't honour rounded clip layers).
void roundCorners(Gfx& g, const Rect& r, float radius, Color around);

// A night sky in `r`: gradient, curtains of aurora, a scatter of stars (the
// same every time for a given seed). `intensity` scales the aurora.
void nightSky(Gfx& g, const Rect& r, float radius, float intensity, unsigned seed, Color around = {});

// A thin band of northern light: the brand's one ornament (3 DIPs tall at the
// top of a night surface).
void auroraBand(Gfx& g, const Rect& r, bool dark);

}  // namespace cjui::northray

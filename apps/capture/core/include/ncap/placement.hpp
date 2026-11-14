// NorthRay Capture: where scene items sit on the canvas, and how the preview moves and resizes
// them (part of system 4). Pure geometry, unit-tested; the compositor and the preview both use it,
// so what's drawn and what's clicked always agree.
//
// Coordinates are canvas pixels. An item's frame is its transform's (x, y, width, height) before
// rotation; rotation turns that frame about its centre. Flips mirror the picture inside the frame.
#pragma once

#include "ncap/scenes.hpp"

#include <string>
#include <vector>

namespace ncap {

struct Vec2 { float x = 0, y = 0; };
struct Box {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
};

// The size of a source's picture after cropping (never below 1x1). `w`/`h` are the source's own
// size; 0 when it isn't known yet (nothing captured), which gives 0.
Vec2 croppedSize(const Transform& t, float sourceW, float sourceH);
// The item's frame on the canvas, before rotation: the transform's size, or the cropped source's
// own size where the transform's is 0.
Box frameOf(const Transform& t, float sourceW, float sourceH);
// The four corners after rotation (top-left, top-right, bottom-right, bottom-left of the frame).
void cornersOf(const Transform& t, float sourceW, float sourceH, Vec2 out[4]);
// The axis-aligned box around the rotated frame.
Box boundsOf(const Transform& t, float sourceW, float sourceH);
// Is a canvas point inside the rotated frame?
bool hits(const Transform& t, float sourceW, float sourceH, Vec2 p);

// The topmost visible item under a point (the last in the list is on top). Locked items are found
// too, so they can be selected; moving them is refused by the caller. `sizes` gives each item's
// source size, in the same order as `items`. Empty when none.
struct ItemSize { float w = 0, h = 0; };
Id itemAt(const std::vector<SceneItem>& items, const std::vector<ItemSize>& sizes, Vec2 p);

// Resize handles: the four corners and the four edge middles.
enum class Handle { None, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left };
Vec2 handlePoint(const Transform& t, float sourceW, float sourceH, Handle h);
// The handle within `radius` canvas pixels of `p`, or None.
Handle handleAt(const Transform& t, float sourceW, float sourceH, Vec2 p, float radius);
// The cursor shape to show for a handle at this rotation: 0 = ↔, 1 = ↕, 2 = ⤡ (\), 3 = ⤢ (/).
int handleCursor(Handle h, float rotation);

// Dragging a handle by `delta` (canvas pixels, from where the drag started) from `start`: the
// opposite corner or edge stays put. Corners keep the shape unless `free`; edges never do. Sizes
// never go below 2 px. Returns the new transform (with a definite width and height).
Transform resizeBy(const Transform& start, float sourceW, float sourceH, Handle h, Vec2 delta, bool free);

// Alt+dragging a handle crops instead: the moved edges cut into the picture (or uncover it again),
// keeping its scale and keeping what's left in place. Flips are allowed for (dragging the left edge
// of a mirrored item crops the source's right side). At least 1 source pixel always stays.
Transform cropBy(const Transform& start, float sourceW, float sourceH, Handle h, Vec2 delta);

// Snapping while moving: if an edge or the centre of `moving` (a bounds box) comes within
// `threshold` of the canvas's edges or centre lines, the offset that lines it up; else 0.
Vec2 snapOffset(const Box& moving, float canvasW, float canvasH, float threshold);

// Quick placements (the right-click menu).
Transform fitToCanvas(const Transform& t, float sourceW, float sourceH, float canvasW, float canvasH);   // keeps the shape
Transform stretchToCanvas(const Transform& t, float sourceW, float sourceH, float canvasW, float canvasH);
Transform centreOnCanvas(const Transform& t, float sourceW, float sourceH, float canvasW, float canvasH);
Transform resetTransform(const Transform& t);   // the source's own size at the top-left, unrotated, uncropped
Transform rotateBy(const Transform& t, float sourceW, float sourceH, float degrees);   // about the centre

// Finding a window again after the app restarts (window capture's settings remember these three).
struct WindowIdentity {
    std::string title, windowClass, exe;   // exe: "Minecraft.Windows.exe" (no folder)
};
// The best match for `wanted` among `candidates`, or -1. The program must match; then the window
// class, then the title (exactly, then the longest shared beginning, since titles often add the
// document or world name after a fixed part).
int matchWindow(const WindowIdentity& wanted, const std::vector<WindowIdentity>& candidates);

}  // namespace ncap

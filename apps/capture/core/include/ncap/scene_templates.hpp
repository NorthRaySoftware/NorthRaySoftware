// NorthRay Capture: scene templates (New scene › From a template). Each is a scene made of parts: your
// screen, your camera with a facecam frame that follows it, the aurora sky, and overlay designs from
// design::templates(), placed in proportion to the canvas. Portable and unit-tested; the window makes the
// sources (reusing a camera or screen already captured) and the items.
#pragma once

#include <string>
#include <vector>

namespace ncap::scene_templates {

enum class PartKind { Display, Camera, AuroraSky, Design };

struct Part {
    PartKind kind;
    const char* design = "";   // a design::templates() id, for Design
    float x = 0, y = 0, w = 1;  // left, top and width as fractions of the canvas
    int follows = -1;          // the part it follows (a facecam frame its camera), or -1
};

struct SceneTemplate {
    const char* id;
    const char* name;          // the scene's name
    const char* description;   // shown beside it in the menu
    std::vector<Part> parts;   // bottom to top
};
const std::vector<SceneTemplate>& all();

struct Rect {
    float x, y, w, h;
};
// Where a part goes on a `cw`×`ch` canvas. Designs keep their page's shape (`dw`×`dh`); the screen fills the
// canvas; the camera is 16:9.
Rect place(const Part& p, float cw, float ch, float dw = 0, float dh = 0);
// A facecam frame design (`dw`×`dh`, its opening at `hole`) placed so the opening sits over `camera`.
Rect frameAround(const Rect& camera, float dw, float dh, const Rect& hole);

}  // namespace ncap::scene_templates

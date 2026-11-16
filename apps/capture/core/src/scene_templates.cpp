#include "ncap/scene_templates.hpp"

namespace ncap::scene_templates {

const std::vector<SceneTemplate>& all() {
    using K = PartKind;
    static const std::vector<SceneTemplate> v = {
        {"gaming",
         "Gaming",
         "your screen, camera in the corner, chat and music",
         {
             {K::Display, "", 0, 0, 1},
             {K::Camera, "", 0.745f, 0.715f, 0.235f},
             {K::Design, "facecam-frame", 0, 0, 0, 1},
             {K::Design, "chat-box", 0.012f, 0.30f, 0.20f},
             {K::Design, "now-playing", 0.012f, 0.02f, 0.27f},
         }},
        {"chatting",
         "Just chatting",
         "a big camera, chat beside it, your name, the aurora behind",
         {
             {K::AuroraSky, "", 0, 0, 1},
             {K::Camera, "", 0.04f, 0.08f, 0.6f},
             {K::Design, "facecam-frame", 0, 0, 0, 1},
             {K::Design, "chat-box", 0.68f, 0.08f, 0.29f},
             {K::Design, "lower-third", 0.04f, 0.78f, 0.45f},
         }},
        {"art",
         "Art",
         "your drawing app full screen, camera in the corner, socials",
         {
             {K::Display, "", 0, 0, 1},
             {K::Camera, "", 0.775f, 0.03f, 0.205f},
             {K::Design, "facecam-frame", 0, 0, 0, 1},
             {K::Design, "now-playing", 0.012f, 0.02f, 0.25f},
             {K::Design, "socials-bar", 0.2f, 0.905f, 0.6f},
         }},
        {"reading",
         "Reading and writing",
         "your book's card, camera, chat, the aurora behind",
         {
             {K::AuroraSky, "", 0, 0, 1},
             {K::Camera, "", 0.05f, 0.08f, 0.5f},
             {K::Design, "facecam-frame", 0, 0, 0, 1},
             {K::Design, "book-card", 0.05f, 0.7f, 0.42f},
             {K::Design, "chat-box", 0.66f, 0.08f, 0.29f},
         }},
        {"radio",
         "Radio show",
         "the show's title, now playing, camera, an aurora that moves with the music",
         {
             {K::AuroraPulse, "", 0, 0, 1},
             {K::Camera, "", 0.58f, 0.1f, 0.38f},
             {K::Design, "facecam-frame", 0, 0, 0, 1},
             {K::Design, "show-title", 0.04f, 0.12f, 0.5f},
             {K::Design, "now-playing", 0.04f, 0.4f, 0.4f},
             {K::Design, "socials-bar", 0.2f, 0.905f, 0.6f},
         }},
    };
    return v;
}

Rect place(const Part& p, float cw, float ch, float dw, float dh) {
    const float x = p.x * cw, y = p.y * ch, w = p.w * cw;
    switch (p.kind) {
        case PartKind::Display:
        case PartKind::AuroraSky:
        case PartKind::AuroraPulse: return {x, y, w, w * ch / cw};
        case PartKind::Camera: return {x, y, w, w * 9 / 16};
        case PartKind::Design: return {x, y, w, dw > 0 ? w * dh / dw : w * 9 / 16};
    }
    return {x, y, w, w};
}

Rect frameAround(const Rect& camera, float dw, float dh, const Rect& hole) {
    if (hole.w <= 0 || hole.h <= 0) return camera;
    const float sx = camera.w / hole.w, sy = camera.h / hole.h;
    return {camera.x - hole.x * sx, camera.y - hole.y * sy, dw * sx, dh * sy};
}

}  // namespace ncap::scene_templates

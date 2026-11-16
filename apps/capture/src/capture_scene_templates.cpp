// NorthRay Capture's window: new scenes from templates (core/scene_templates). The parts become sources and
// items: the screen is the main monitor (one already captured is reused), the camera is the one already in
// use or the first Windows lists (none: those parts are left out and you're told), the facecam frame
// follows the camera, the aurora sky and designs take the brand kit's colours.
#include "capture_window.hpp"

#include "log.hpp"
#include "video/devices.hpp"

#include "ncap/aurora_sky.hpp"
#include "ncap/design.hpp"
#include "ncap/scene_templates.hpp"

#include "cjui/menu.hpp"

#include <nlohmann/json.hpp>

namespace ncap {

using cjui::MenuItem;
using cjui::narrow;
using cjui::widen;
using nlohmann::json;
namespace st = scene_templates;

void CaptureWindow::newSceneMenu() {
    std::vector<MenuItem> items = {MenuItem::item(L"An empty scene…", 1), MenuItem::sep(), MenuItem::item(L"FROM A TEMPLATE", 0, {}, false)};
    const auto& ts = st::all();
    for (std::size_t i = 0; i < ts.size(); ++i) items.push_back(MenuItem::item(widen(ts[i].name), 100 + (UINT)i, widen(ts[i].description)));
    POINT pt;
    GetCursorPos(&pt);
    const UINT cmd = cjui::showMenu(hwnd(), items, pt, pal_);
    if (cmd == 1) newScene();
    else if (cmd >= 100 && cmd < 100 + ts.size()) sceneFromTemplate(ts[cmd - 100]);
}

void CaptureWindow::sceneFromTemplate(const st::SceneTemplate& t) {
    const float cw = (float)scenes_.canvasWidth, ch = (float)scenes_.canvasHeight;
    const Id scene = scenes_.addScene(t.name);
    std::vector<Id> items(t.parts.size());
    std::vector<st::Rect> rects(t.parts.size(), st::Rect{0, 0, 0, 0});
    std::vector<std::wstring> notes;
    auto designDoc = [](const char* id) -> const design::Document* {
        for (auto& d : design::templates())
            if (std::string(id) == d.id) return &d.doc;
        return nullptr;
    };
    for (std::size_t i = 0; i < t.parts.size(); ++i) {
        const st::Part& p = t.parts[i];
        Id source;
        const design::Document* doc = p.kind == st::PartKind::Design ? designDoc(p.design) : nullptr;
        st::Rect r = st::place(p, cw, ch, doc ? (float)doc->width : 0, doc ? (float)doc->height : 0);
        if (p.follows >= 0) {
            if (items[(size_t)p.follows].empty()) continue;   // what it goes with isn't there
            const design::Element* hole = doc && !doc->elements.empty() ? &doc->elements[0] : nullptr;
            if (doc && hole) r = st::frameAround(rects[(size_t)p.follows], (float)doc->width, (float)doc->height, {hole->x, hole->y, hole->w, hole->h});
        }
        switch (p.kind) {
            case st::PartKind::Display: {
                std::string monitor;
                for (auto& m : video::listMonitors())
                    if (m.primary || monitor.empty()) monitor = narrow(m.device);
                if (monitor.empty()) {
                    notes.push_back(L"no screen was found");
                    break;
                }
                for (auto& s : scenes_.sources())   // the screen already captured: the same source
                    if (s.kind == SourceKind::Display && s.settings.find(json(monitor).dump()) != std::string::npos) source = s.id;
                if (source.empty())
                    source = scenes_.addSource(SourceKind::Display, "Main screen", json{{"monitor", monitor}, {"method", "auto"}, {"cursor", true}}.dump());
                break;
            }
            case st::PartKind::Camera: {
                for (auto& s : scenes_.sources())   // a camera can only be opened once: use the one in use
                    if (s.kind == SourceKind::Camera) {
                        source = s.id;
                        break;
                    }
                if (source.empty()) {
                    const auto cams = video::listCameras();
                    if (cams.empty()) {
                        notes.push_back(L"no camera was found, so the camera and its frame were left out");
                        break;
                    }
                    source = scenes_.addSource(SourceKind::Camera, narrow(cams[0].name), json{{"device", narrow(cams[0].link)}, {"name", narrow(cams[0].name)}}.dump());
                }
                break;
            }
            case st::PartKind::AuroraSky: {
                aurora_sky::Settings a;
                brandSky(a);
                a.width = (int)cw;
                a.height = (int)ch;
                source = scenes_.addSource(SourceKind::AuroraSky, "Aurora sky", aurora_sky::toJson(a));
                break;
            }
            case st::PartKind::Design:
                if (doc) source = scenes_.addSource(SourceKind::Design, doc->name, design::toJson(*doc));
                break;
        }
        if (source.empty()) continue;
        items[i] = scenes_.addItem(scene, source, Transform{r.x, r.y, r.w, r.h});
        rects[i] = r;
        if (p.follows >= 0 && !items[i].empty()) {
            std::string err;
            if (!scenes_.followItem(scene, items[i], items[(size_t)p.follows], err)) log::warn("Scene template: a frame couldn't follow its camera: " + err);
        }
    }
    scenes_.setCurrent(scene);
    syncSources();
    syncAudio();
    scenesChanged();
    log::info(std::string("Scene added from the template \"") + t.id + "\": " + scenes_.scene(scene)->name);
    std::wstring msg = L"Added the " + widen(t.name) + L" scene. Right-click its designs to edit them; Your details fills in names and links.";
    for (auto& n : notes) msg += L" (" + n + L".)";
    flash(msg, 9);
}

}  // namespace ncap

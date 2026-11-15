// NorthRay Capture's window: sources, the live preview, and moving and resizing items on it.
#include "capture_window.hpp"

#include "log.hpp"

#include "cjui/dialog.hpp"
#include "cjui/menu.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>

namespace ncap {

using cjui::MenuItem;
using cjui::narrow;
using cjui::Rect;
using cjui::widen;
using nlohmann::json;

namespace {

enum : UINT {
    // The "+" menu
    CmdDisplay0 = 1000, CmdWindow0 = 2000, CmdCamera0 = 3000, CmdExisting0 = 4000, CmdScene0 = 5000,
    // An item's menu
    CmdRename = 100, CmdShow, CmdLock, CmdRemove,
    CmdFit, CmdStretch, CmdCentre, CmdReset, CmdRotCw, CmdRotCcw, CmdRot180, CmdFlipH, CmdFlipV, CmdResetCrop,
    CmdUp, CmdDown, CmdTop, CmdBottom,
    CmdMethodAuto, CmdMethodDup, CmdMethodWgc, CmdMethodGdi, CmdCursor,
};

bool keyDown(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

// Kinds with a picture source of their own (scenes inside scenes are composed from their items instead).
bool isCapture(SourceKind k) {
    return k == SourceKind::Display || k == SourceKind::Window || k == SourceKind::Camera || k == SourceKind::Colour || k == SourceKind::Image ||
           k == SourceKind::Text || k == SourceKind::Slideshow || k == SourceKind::Region || k == SourceKind::Media || k == SourceKind::Design;
}

std::string shortTitle(std::string t) {
    // "Document - Program" titles: the program's own part is usually after the last " - ".
    if (t.size() > 40) {
        const size_t dash = t.rfind(" - ");
        if (dash != std::string::npos && t.size() - dash - 3 >= 3) t = t.substr(dash + 3);
    }
    if (t.size() > 40) {
        size_t cut = 39;
        while (cut > 0 && ((unsigned char)t[cut] & 0xC0) == 0x80) --cut;   // not inside a UTF-8 character
        t = t.substr(0, cut) + "\xE2\x80\xA6";
    }
    return t;
}

}  // namespace

// ---------------------------------------------------------------- engine and sources

void CaptureWindow::startEngine() {
    std::wstring error;
    if (!engine_.start(hwnd(), settings_.outputFps, error)) {
        engineError_ = error;
        log::error("The video engine couldn't start: " + narrow(error));
        return;
    }
    log::info("Video engine started on " + narrow(engine_.gpu().adapterName()));
    syncSources();
    applyTransitionFiles();
}

void CaptureWindow::syncSources() {
    if (!engine_.running()) return;
    // Captures for every capture source in the collection (shared by the scenes that show them).
    for (auto it = live_.begin(); it != live_.end();) {
        if (!scenes_.source(it->first)) it = live_.erase(it);
        else ++it;
    }
    for (const Source& s : scenes_.sources()) {
        if (!isCapture(s.kind)) continue;
        auto it = live_.find(s.id);
        if (it == live_.end()) {
            Live l;
            l.capture = video::createVideoSource(s.kind, s.settings);
            l.settings = s.settings;
            live_[s.id] = l;
        } else if (it->second.settings != s.settings) {
            it->second.settings = s.settings;
            if (it->second.capture) it->second.capture->setSettings(s.settings);
        }
    }
    syncAudio();   // media files' sound joins the mixer once their source exists
}

ItemSize CaptureWindow::sizeOf(const Id& source) const {
    if (const Source* src = scenes_.source(source); src && src->kind == SourceKind::Scene)   // a scene inside: the canvas's size
        return {(float)scenes_.canvasWidth, (float)scenes_.canvasHeight};
    auto it = live_.find(source);
    if (it == live_.end() || !it->second.capture) return {};
    return {(float)it->second.capture->width(), (float)it->second.capture->height()};
}

std::vector<ItemSize> CaptureWindow::sizesOf(const Scene& s) const {
    std::vector<ItemSize> out;
    out.reserve(s.items.size());
    for (auto& i : s.items) out.push_back(sizeOf(i.source));
    return out;
}

const SceneItem* CaptureWindow::selectedItem() const { return selected_.empty() ? nullptr : scenes_.item(scenes_.current(), selected_); }

const Source* CaptureWindow::selectedSource() const {
    const SceneItem* i = selectedItem();
    return i ? scenes_.source(i->source) : nullptr;
}

std::wstring CaptureWindow::sourceStatus(const Id& source, bool* problem) const {
    if (problem) *problem = false;
    const Source* s = scenes_.source(source);
    if (!s) return {};
    if (s->kind == SourceKind::Scene) {
        const Scene* shown = scenes_.scene(SceneCollection::nestedScene(*s));
        return shown ? L"Shows the scene \u201C" + widen(shown->name) + L"\u201D, as it is" : L"The scene it showed is gone";
    }
    if (!isCapture(s->kind)) return L"Arrives in a later version";
    if (!engineError_.empty()) {
        if (problem) *problem = true;
        return L"Video capture isn't running";
    }
    auto it = live_.find(source);
    if (it == live_.end() || !it->second.capture) return {};
    if (problem) *problem = it->second.capture->problem();
    return it->second.capture->status();
}

video::Items CaptureWindow::buildItems(const Id& sceneId, int depth) const {
    video::Items out;
    const Scene* sc = scenes_.scene(sceneId);
    if (!sc) return out;
    for (auto& i : sc->items) {
        video::DrawItem d;
        d.id = i.id;
        d.transform = i.transform;
        d.border = i.border;
        d.visible = i.visible;
        const Source* src = scenes_.source(i.source);
        if (src && src->kind == SourceKind::Scene) {
            const Id n = SceneCollection::nestedScene(*src);
            if (!n.empty() && depth < 4) {
                d.nested = n;
                d.nestedItems = std::make_shared<const video::Items>(buildItems(n, depth + 1));
            }
        } else {
            auto it = live_.find(i.source);
            if (it != live_.end()) d.source = it->second.capture;
        }
        out.push_back(std::move(d));
    }
    return out;
}

void CaptureWindow::pushSnapshot() {
    if (!engine_.running()) return;
    video::Snapshot s;
    s.canvasW = scenes_.canvasWidth;
    s.canvasH = scenes_.canvasHeight;
    s.programScene = programScene();
    if (s.programScene != enteredScene_) {
        // A scene going live: its designs play their entrances.
        enteredScene_ = s.programScene;
        if (const Scene* sc = scenes_.scene(s.programScene))
            for (auto& it : sc->items)
                if (const Source* src = scenes_.source(it.source); src && src->kind == SourceKind::Design)
                    if (auto l = live_.find(src->id); l != live_.end() && l->second.capture) l->second.capture->command("enter");
    }
    s.items = buildItems(s.programScene, 0);
    s.transition = settings_.transition;
    s.stinger = stinger_;
    s.shaderCode = settings_.transition.kind == transition::Kind::Shader ? shaderCode_ : std::string();
    s.studio = settings_.studioMode;
    if (s.studio) s.previewItems = buildItems(scenes_.current(), 0);
    s.selected = selected_;
    s.hovered = pdrag_ == PreviewDrag::None ? hoverItem_ : Id{};
    if (const SceneItem* i = selectedItem()) s.selectedLocked = i->locked;
    s.guidesX = guidesX_;
    s.guidesY = guidesY_;
    s.uiScale = gfx_.dpiScale();
    const cjui::Color bg = pal_.dark ? cjui::hexColor("#05080F") : cjui::hexColor("#0B1120");
    s.background[0] = bg.r;
    s.background[1] = bg.g;
    s.background[2] = bg.b;
    engine_.setSnapshot(std::move(s));
}

void CaptureWindow::placePreview() {
    const float sc = gfx_.dpiScale();
    RECT r{(LONG)std::lround(previewInner_.x * sc), (LONG)std::lround(previewInner_.y * sc), (LONG)std::lround(previewInner_.right() * sc),
           (LONG)std::lround(previewInner_.bottom() * sc)};
    const int fps = settings_.lowResource ? std::min(settings_.previewFps, 10) : settings_.previewFps;
    engine_.setPreview(r, previewLive_, fps);
}

video::Fit CaptureWindow::editFit() const {
    return video::layoutViews((float)scenes_.canvasWidth, (float)scenes_.canvasHeight, previewInner_.w, previewInner_.h, settings_.studioMode).edit;
}

Vec2 CaptureWindow::toCanvas(float x, float y) const {
    const video::Fit f = editFit();
    return {(x - previewInner_.x - f.x) / f.scale, (y - previewInner_.y - f.y) / f.scale};
}

// ---------------------------------------------------------------- adding and removing

void CaptureWindow::addSourceMenu(POINT screen) {
    const auto monitors = video::listMonitors();
    const auto windows = video::listWindows();
    const auto cameras = video::listCameras();
    std::vector<MenuItem> mon, win, cam, existing;
    for (size_t i = 0; i < monitors.size() && i < 900; ++i) mon.push_back(MenuItem::item(monitors[i].label(), CmdDisplay0 + (UINT)i));
    for (size_t i = 0; i < windows.size() && i < 900; ++i) {
        std::wstring exe = widen(windows[i].id.exe);
        if (exe.size() > 4 && _wcsicmp(exe.c_str() + exe.size() - 4, L".exe") == 0) exe.resize(exe.size() - 4);
        win.push_back(MenuItem::item(widen(shortTitle(windows[i].id.title)), CmdWindow0 + (UINT)i, exe));
    }
    for (size_t i = 0; i < cameras.size() && i < 900; ++i) cam.push_back(MenuItem::item(cameras[i].name, CmdCamera0 + (UINT)i));
    if (mon.empty()) mon.push_back(MenuItem::item(L"No displays found", 1, {}, false));
    if (win.empty()) win.push_back(MenuItem::item(L"No windows to capture", 1, {}, false));
    if (cam.empty()) cam.push_back(MenuItem::item(L"No cameras or capture cards found", 1, {}, false));
    // Sources other scenes already show (shared: captured once).
    const Scene* here = scenes_.scene(scenes_.current());
    std::vector<Id> others;
    for (auto& s : scenes_.sources()) {
        if (isAudioOnly(s.kind)) continue;   // heard in every scene already
        if (s.kind == SourceKind::Scene) continue;   // under "Scene" below
        bool shown = false;
        if (here)
            for (auto& i : here->items) shown = shown || i.source == s.id;
        if (!shown && others.size() < 900) {
            others.push_back(s.id);
            existing.push_back(MenuItem::item(widen(s.name), CmdExisting0 + (UINT)others.size() - 1));
        }
    }
    std::vector<MenuItem> items = {
        MenuItem::sub(L"Display (whole monitor)", mon),
        MenuItem::sub(L"Window", win),
        MenuItem::sub(L"Camera or capture card", cam),
    };
    if (!existing.empty()) items.push_back(MenuItem::sub(L"A source from another scene", existing));
    // Another scene, shown inside this one (never one that would end up inside itself).
    std::vector<Id> nestable;
    std::vector<MenuItem> sceneItems;
    for (auto& sc : scenes_.scenes()) {
        if (sc.id == scenes_.current() || nestable.size() >= 900) continue;
        const bool loops = scenes_.wouldLoop(scenes_.current(), sc.id);
        nestable.push_back(sc.id);
        sceneItems.push_back(MenuItem::item(widen(sc.name), CmdScene0 + (UINT)nestable.size() - 1, loops ? L"it shows this one" : L"", !loops));
    }
    if (sceneItems.empty()) sceneItems.push_back(MenuItem::item(L"Make another scene first", 1, {}, false));
    items.push_back(MenuItem::sub(L"Scene (another scene inside this one)", sceneItems));
    items.push_back(MenuItem::item(L"Sync test (flash and beep)", 10, L"checks recordings"));
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::item(L"Image or animated GIF\u2026", 5));
    items.push_back(MenuItem::item(L"Video or sound file\u2026", 11, L"or several, in turn"));
    items.push_back(MenuItem::item(L"Overlay design\u2026", 12, L"Starting Soon, lower third, frame\u2026"));
    items.push_back(MenuItem::item(L"Text\u2026", 6));
    items.push_back(MenuItem::item(L"Slideshow (a folder of images)\u2026", 7));
    items.push_back(MenuItem::item(L"Colour\u2026", 8));
    items.push_back(MenuItem::item(L"Part of the screen\u2026", 9, L"drag a rectangle"));
    items.push_back(MenuItem::sep());
    const AudioChoices sound = audioChoices();
    for (auto& m : audioAddItems(sound)) items.push_back(m);
    const UINT cmd = cjui::showMenu(hwnd(), items, screen, pal_);
    if (audioAddCommand(cmd, sound)) return;
    switch (cmd) {
        case 5: addImage(); return;
        case 6: addText(); return;
        case 7: addSlideshow(); return;
        case 8: addColour(); return;
        case 9: addRegion(); return;
        case 11: addMedia(); return;
        case 12: addDesign(); return;
        default: break;
    }
    if (cmd == 10) {
        // The pattern in this scene, and the tone among the sound sources (once).
        addCapture(SourceKind::Colour, "Sync test pattern", nlohmann::json{{"sync", true}}.dump());
        bool haveTone = false;
        for (auto& s : scenes_.sources())
            if (s.settings.find("#synctone") != std::string::npos) haveTone = true;
        if (!haveTone) {
            scenes_.addSource(SourceKind::InputAudio, "Sync test tone",
                              nlohmann::json{{"device", "#synctone"}, {"volume", 1.0}, {"muted", false}, {"buses", (unsigned)(routing::Stream | routing::Track1)}}.dump());
            syncAudio();
            scenesChanged();
        }
        flash(L"Record about 20 seconds, then check in an editor that each white flash and its beep start together.", 10);
        return;
    }
    if (cmd >= CmdDisplay0 && cmd < CmdDisplay0 + monitors.size()) {
        const auto& m = monitors[cmd - CmdDisplay0];
        addCapture(SourceKind::Display, "Display " + std::to_string(m.number),
                   json{{"monitor", narrow(m.device)}, {"method", "auto"}, {"cursor", true}}.dump());
    } else if (cmd >= CmdWindow0 && cmd < CmdWindow0 + windows.size()) {
        const auto& w = windows[cmd - CmdWindow0];
        addCapture(SourceKind::Window, shortTitle(w.id.title),
                   json{{"title", w.id.title}, {"class", w.id.windowClass}, {"exe", w.id.exe}, {"method", "auto"}, {"cursor", true}}.dump());
    } else if (cmd >= CmdCamera0 && cmd < CmdCamera0 + cameras.size()) {
        const auto& c = cameras[cmd - CmdCamera0];
        addCapture(SourceKind::Camera, narrow(c.name), json{{"device", narrow(c.link)}, {"name", narrow(c.name)}}.dump());
    } else if (cmd >= CmdExisting0 && cmd < CmdExisting0 + others.size()) {
        addExisting(others[cmd - CmdExisting0]);
    } else if (cmd >= CmdScene0 && cmd < CmdScene0 + nestable.size()) {
        const Id src = scenes_.sceneSource(nestable[cmd - CmdScene0]);
        log::info("Scene shown inside \"" + scenes_.scene(scenes_.current())->name + "\": " + scenes_.scene(nestable[cmd - CmdScene0])->name);
        addExisting(src);
    }
}

void CaptureWindow::addCapture(SourceKind kind, const std::string& name, const std::string& settings) {
    const Id src = scenes_.addSource(kind, name, settings);
    const Id item = scenes_.addItem(scenes_.current(), src);
    if (item.empty()) return;
    selected_ = item;
    focus_ = Focus::Sources;
    pendingFit_.insert(item);
    log::info(std::string("Source added: ") + kindKey(kind) + " \"" + scenes_.source(src)->name + "\"");
    syncSources();
    scenesChanged();
}

void CaptureWindow::addExisting(const Id& source) {
    const Id item = scenes_.addItem(scenes_.current(), source);
    if (item.empty()) return;
    selected_ = item;
    pendingFit_.insert(item);
    scenesChanged();
}

void CaptureWindow::removeItem(const Id& itemId) {
    const SceneItem* it = scenes_.item(scenes_.current(), itemId);
    if (!it) return;
    const Source* src = scenes_.source(it->source);
    const std::wstring name = src ? widen(src->name) : L"this source";
    if (cjui::messageBox(hwnd(), L"Remove “" + name + L"” from this scene?", L"Remove source", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    const Id sourceId = it->source;
    scenes_.removeItem(scenes_.current(), itemId);
    // A source no scene shows any more is deleted, and its capture stops.
    const auto unused = scenes_.unusedSources();
    if (std::find(unused.begin(), unused.end(), sourceId) != unused.end()) scenes_.removeSource(sourceId);
    if (selected_ == itemId) selected_.clear();
    pendingFit_.erase(itemId);
    syncSources();
    scenesChanged();
}

void CaptureWindow::renameSource(const Id& source) {
    const Source* s = scenes_.source(source);
    if (!s) return;
    askText(L"Rename the source", widen(s->name), [this, source](const std::wstring& name) {
        if (!scenes_.renameSource(source, narrow(name))) {
            flash(name.find_first_not_of(L' ') == std::wstring::npos ? L"A source needs a name." : L"Another source already has that name.", 5);
            return;
        }
        scenesChanged();
    });
}

void CaptureWindow::setTransform(const Id& item, const Transform& t, bool save) {
    scenes_.setTransform(scenes_.current(), item, t);
    pendingFit_.erase(item);   // placed by hand
    if (save) scenesChanged();
    else invalidate();
}

void CaptureWindow::checkPendingFits() {
    for (auto it = pendingFit_.begin(); it != pendingFit_.end();) {
        const SceneItem* i = scenes_.item(scenes_.current(), *it);
        if (!i) {
            ++it;   // in another scene for now
            continue;
        }
        const ItemSize sz = sizeOf(i->source);
        if (sz.w <= 0) {
            ++it;
            continue;
        }
        const float cw = (float)scenes_.canvasWidth, ch = (float)scenes_.canvasHeight;
        // Bigger than the canvas: fitted inside it. Smaller: centred at its own size.
        const Transform t = (sz.w > cw || sz.h > ch) ? fitToCanvas(i->transform, sz.w, sz.h, cw, ch) : centreOnCanvas(i->transform, sz.w, sz.h, cw, ch);
        scenes_.setTransform(scenes_.current(), *it, t);
        it = pendingFit_.erase(it);
        scenesChanged();
    }
}

// ---------------------------------------------------------------- an item's menu

void CaptureWindow::transformCommand(UINT cmd) {
    const SceneItem* i = selectedItem();
    if (!i) return;
    const ItemSize sz = sizeOf(i->source);
    const float cw = (float)scenes_.canvasWidth, ch = (float)scenes_.canvasHeight;
    Transform t = i->transform;
    switch (cmd) {
        case CmdFit: t = fitToCanvas(t, sz.w, sz.h, cw, ch); break;
        case CmdStretch: t = stretchToCanvas(t, sz.w, sz.h, cw, ch); break;
        case CmdCentre: t = centreOnCanvas(t, sz.w, sz.h, cw, ch); break;
        case CmdReset: t = resetTransform(t); break;
        case CmdRotCw: t = rotateBy(t, sz.w, sz.h, 90); break;
        case CmdRotCcw: t = rotateBy(t, sz.w, sz.h, -90); break;
        case CmdRot180: t = rotateBy(t, sz.w, sz.h, 180); break;
        case CmdFlipH: t.flipH = !t.flipH; break;
        case CmdFlipV: t.flipV = !t.flipV; break;
        case CmdResetCrop: {
            // Uncropped at the same scale, so nothing jumps.
            const Box f = frameOf(t, sz.w, sz.h);
            const Vec2 c = croppedSize(t, sz.w, sz.h);
            if (c.x > 0 && t.width > 0) {
                const float sx = f.w / c.x, sy = f.h / c.y;
                t.x -= (t.flipH ? t.cropRight : t.cropLeft) * sx;
                t.y -= (t.flipV ? t.cropBottom : t.cropTop) * sy;
                t.width = sz.w * sx;
                t.height = sz.h * sy;
            }
            t.cropLeft = t.cropTop = t.cropRight = t.cropBottom = 0;
            break;
        }
        default: return;
    }
    if (i->locked) {
        flash(L"This source is locked. Unlock it to move or change it.", 4);
        return;
    }
    setTransform(i->id, t);
}

void CaptureWindow::itemMenu(const Id& itemId, POINT screen) {
    const SceneItem* i = scenes_.item(scenes_.current(), itemId);
    if (!i) return;
    selected_ = itemId;
    invalidate();
    const Source* src = scenes_.source(i->source);
    const json settings = src ? json::parse(src->settings, nullptr, false) : json();
    const std::string method = settings.is_object() && settings.contains("method") && settings["method"].is_string() ? settings["method"].get<std::string>() : "auto";
    const bool cursor = !(settings.is_object() && settings.contains("cursor") && settings["cursor"].is_boolean()) || settings["cursor"].get<bool>();
    const bool movable = !i->locked;
    std::vector<MenuItem> transform = {
        MenuItem::item(L"Fit to the canvas", CmdFit, L"Ctrl+F", movable),
        MenuItem::item(L"Stretch to the canvas", CmdStretch, L"Ctrl+S", movable),
        MenuItem::item(L"Centre", CmdCentre, L"Ctrl+D", movable),
        MenuItem::item(L"Reset", CmdReset, L"Ctrl+R", movable),
        MenuItem::sep(),
        MenuItem::item(L"Rotate 90° clockwise", CmdRotCw, {}, movable),
        MenuItem::item(L"Rotate 90° anticlockwise", CmdRotCcw, {}, movable),
        MenuItem::item(L"Rotate 180°", CmdRot180, {}, movable),
        MenuItem::sep(),
        MenuItem::item(L"Flip horizontally", CmdFlipH, {}, movable, i->transform.flipH),
        MenuItem::item(L"Flip vertically", CmdFlipV, {}, movable, i->transform.flipV),
        MenuItem::sep(),
        MenuItem::item(L"Remove the crop", CmdResetCrop, L"Alt+drag crops",
                       movable && (i->transform.cropLeft > 0 || i->transform.cropTop > 0 || i->transform.cropRight > 0 || i->transform.cropBottom > 0)),
    };
    const auto& items = scenes_.scene(scenes_.current())->items;
    const bool top = !items.empty() && items.back().id == itemId, bottom = !items.empty() && items.front().id == itemId;
    std::vector<MenuItem> order = {
        MenuItem::item(L"Move up", CmdUp, L"Ctrl+Up", !top),
        MenuItem::item(L"Move down", CmdDown, L"Ctrl+Down", !bottom),
        MenuItem::item(L"To the top", CmdTop, {}, !top),
        MenuItem::item(L"To the bottom", CmdBottom, {}, !bottom),
    };
    std::vector<MenuItem> menu = {
        MenuItem::item(L"Rename…", CmdRename, L"F2"),
        MenuItem::item(i->visible ? L"Hide" : L"Show", CmdShow),
        MenuItem::item(i->locked ? L"Unlock" : L"Lock", CmdLock),
        MenuItem::sep(),
    };
    if (src) {
        const std::size_t before = menu.size();
        madeMenuItems(*src, menu);
        if (menu.size() > before) menu.push_back(MenuItem::sep());
    }
    menu.push_back(MenuItem::sub(L"Transform", transform));
    menu.push_back(MenuItem::sub(L"Order", order));
    itemExtraItems(*i, menu);
    if (src && (src->kind == SourceKind::Display || src->kind == SourceKind::Window)) {
        std::vector<MenuItem> how = {MenuItem::item(L"Automatic (recommended)", CmdMethodAuto, {}, true, method == "auto")};
        if (src->kind == SourceKind::Display) how.push_back(MenuItem::item(L"Desktop Duplication", CmdMethodDup, {}, true, method == "duplication"));
        how.push_back(MenuItem::item(L"Windows Graphics Capture", CmdMethodWgc, {}, true, method == "wgc"));
        how.push_back(MenuItem::item(L"Compatibility (GDI, slower)", CmdMethodGdi, {}, true, method == "gdi"));
        menu.push_back(MenuItem::sep());
        menu.push_back(MenuItem::sub(L"Capture method", how));
        menu.push_back(MenuItem::item(L"Show the mouse pointer", CmdCursor, {}, true, cursor));
    }
    menu.push_back(MenuItem::sep());
    menu.push_back(MenuItem::item(L"Remove from this scene", CmdRemove, L"Delete"));
    const UINT cmd = cjui::showMenu(hwnd(), menu, screen, pal_);
    const Id scene = scenes_.current();
    auto changeSetting = [&](const char* key, const json& value) {
        if (!src) return;
        json s = json::parse(src->settings, nullptr, false);
        if (!s.is_object()) s = json::object();
        s[key] = value;
        scenes_.setSourceSettings(src->id, s.dump());
        syncSources();
        scenesChanged();
    };
    switch (cmd) {
        case CmdRename: if (src) renameSource(src->id); break;
        case CmdShow: scenes_.setItemVisible(scene, itemId, !i->visible); scenesChanged(); break;
        case CmdLock: scenes_.setItemLocked(scene, itemId, !i->locked); scenesChanged(); break;
        case CmdRemove: removeItem(itemId); break;
        case CmdUp: scenes_.moveItem(scene, itemId, 1); scenesChanged(); break;
        case CmdDown: scenes_.moveItem(scene, itemId, -1); scenesChanged(); break;
        case CmdTop: scenes_.moveItem(scene, itemId, 100000); scenesChanged(); break;
        case CmdBottom: scenes_.moveItem(scene, itemId, -100000); scenesChanged(); break;
        case CmdMethodAuto: changeSetting("method", "auto"); break;
        case CmdMethodDup: changeSetting("method", "duplication"); break;
        case CmdMethodWgc: changeSetting("method", "wgc"); break;
        case CmdMethodGdi: changeSetting("method", "gdi"); break;
        case CmdCursor: changeSetting("cursor", !cursor); break;
        default:
            if (src && madeCommand(cmd, src->id)) break;
            if (itemExtraCommand(cmd, itemId)) break;
            transformCommand(cmd);
            break;
    }
}

// ---------------------------------------------------------------- the preview's mouse

bool CaptureWindow::previewMouseDown(float x, float y) {
    if (!previewLive_ || !previewInner_.contains(x, y)) return false;
    focus_ = Focus::Preview;
    const Scene* sc = scenes_.scene(scenes_.current());
    if (!sc) return true;
    const Vec2 p = toCanvas(x, y);
    const float perPixel = 1.0f / std::max(0.0001f, editFit().scale);
    // A handle of the selected item first.
    if (const SceneItem* sel = selectedItem(); sel && !sel->locked && sel->visible) {
        const ItemSize sz = sizeOf(sel->source);
        const Handle h = handleAt(sel->transform, sz.w, sz.h, p, 8 * perPixel);
        if (h != Handle::None) {
            pdrag_ = keyDown(VK_MENU) ? PreviewDrag::Crop : PreviewDrag::Resize;
            dragHandle_ = h;
            dragStartT_ = sel->transform;
            dragStartCanvas_ = p;
            dragSize_ = sz;
            return true;
        }
    }
    const Id hit = itemAt(sc->items, sizesOf(*sc), p);
    selected_ = hit;
    if (const SceneItem* i = selectedItem(); i && !i->locked) {
        pdrag_ = PreviewDrag::Move;
        dragStartT_ = i->transform;
        dragStartCanvas_ = p;
        dragSize_ = sizeOf(i->source);
    }
    invalidate();
    return true;
}

bool CaptureWindow::previewMouseMove(float x, float y, bool leftDown) {
    if (pdrag_ != PreviewDrag::None && !leftDown) {
        // The button came up outside the window.
        pdrag_ = PreviewDrag::None;
        guidesX_.clear();
        guidesY_.clear();
        scenesChanged();
        return true;
    }
    const SceneItem* sel = selectedItem();
    if (pdrag_ != PreviewDrag::None && sel) {
        const Vec2 p = toCanvas(x, y);
        const Vec2 d{p.x - dragStartCanvas_.x, p.y - dragStartCanvas_.y};
        Transform t = dragStartT_;
        guidesX_.clear();
        guidesY_.clear();
        if (pdrag_ == PreviewDrag::Move) {
            t.x += d.x;
            t.y += d.y;
            if (!keyDown(VK_CONTROL)) {   // Ctrl: no snapping
                const float cw = (float)scenes_.canvasWidth, ch = (float)scenes_.canvasHeight;
                const float perPixel = 1.0f / std::max(0.0001f, editFit().scale);
                const Vec2 off = snapOffset(boundsOf(t, dragSize_.w, dragSize_.h), cw, ch, 10 * perPixel);
                t.x += off.x;
                t.y += off.y;
                const Box b = boundsOf(t, dragSize_.w, dragSize_.h);
                for (float g : {0.0f, cw / 2, cw})
                    if (off.x != 0 && (std::fabs(b.x - g) < 0.01f || std::fabs(b.x + b.w / 2 - g) < 0.01f || std::fabs(b.right() - g) < 0.01f)) guidesX_.push_back(g);
                for (float g : {0.0f, ch / 2, ch})
                    if (off.y != 0 && (std::fabs(b.y - g) < 0.01f || std::fabs(b.y + b.h / 2 - g) < 0.01f || std::fabs(b.bottom() - g) < 0.01f)) guidesY_.push_back(g);
            }
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        } else if (pdrag_ == PreviewDrag::Resize) {
            t = resizeBy(dragStartT_, dragSize_.w, dragSize_.h, dragHandle_, d, keyDown(VK_SHIFT));
        } else {
            t = cropBy(dragStartT_, dragSize_.w, dragSize_.h, dragHandle_, d);
        }
        setTransform(sel->id, t, false);
        pushSnapshot();
        return true;
    }
    // Hovering: show what's under the pointer, and the right cursor.
    if (!previewLive_ || !previewInner_.contains(x, y)) {
        if (!hoverItem_.empty()) {
            hoverItem_.clear();
            invalidate();
        }
        return false;
    }
    const Scene* sc = scenes_.scene(scenes_.current());
    const Vec2 p = toCanvas(x, y);
    const Id hit = sc ? itemAt(sc->items, sizesOf(*sc), p) : Id{};
    if (hit != hoverItem_) {
        hoverItem_ = hit;
        invalidate();
    }
    const float perPixel = 1.0f / std::max(0.0001f, editFit().scale);
    if (sel && !sel->locked && sel->visible) {
        const ItemSize sz = sizeOf(sel->source);
        const Handle h = handleAt(sel->transform, sz.w, sz.h, p, 8 * perPixel);
        if (h != Handle::None) {
            static const LPCWSTR shapes[4] = {IDC_SIZEWE, IDC_SIZENS, IDC_SIZENWSE, IDC_SIZENESW};
            SetCursor(LoadCursorW(nullptr, keyDown(VK_MENU) ? IDC_CROSS : shapes[handleCursor(h, sel->transform.rotation)]));
            return true;
        }
    }
    if (!hit.empty()) {
        const SceneItem* i = scenes_.item(scenes_.current(), hit);
        SetCursor(LoadCursorW(nullptr, i && !i->locked ? IDC_SIZEALL : IDC_ARROW));
    }
    return true;
}

// ---------------------------------------------------------------- keys for the selected item

bool CaptureWindow::sourceKey(WPARAM vk, bool ctrl, bool shift) {
    if (focus_ == Focus::Scenes) return false;
    const SceneItem* i = selectedItem();
    if (!i) return false;
    const Id id = i->id;
    if (vk == VK_ESCAPE) {
        selected_.clear();
        invalidate();
        return true;
    }
    if (vk == VK_DELETE) {
        removeItem(id);
        return true;
    }
    if (vk == VK_F2) {
        renameSource(i->source);
        return true;
    }
    if (ctrl && (vk == VK_UP || vk == VK_DOWN)) {
        scenes_.moveItem(scenes_.current(), id, vk == VK_UP ? 1 : -1);
        scenesChanged();
        return true;
    }
    if (ctrl) {
        const UINT cmd = vk == 'F' ? (UINT)CmdFit : vk == 'S' ? (UINT)CmdStretch : vk == 'D' ? (UINT)CmdCentre : vk == 'R' ? (UINT)CmdReset : 0u;
        if (cmd) {
            transformCommand(cmd);
            return true;
        }
        return false;
    }
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) {
        if (i->locked) return true;
        const float step = shift ? 10.0f : 1.0f;
        Transform t = i->transform;
        if (vk == VK_LEFT) t.x -= step;
        if (vk == VK_RIGHT) t.x += step;
        if (vk == VK_UP) t.y -= step;
        if (vk == VK_DOWN) t.y += step;
        setTransform(id, t);
        return true;
    }
    return false;
}

}  // namespace ncap

#include "ncap/scenes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <cctype>

namespace ncap {

namespace {

struct KindName { SourceKind kind; const char* key; };
const KindName kKinds[] = {
    {SourceKind::Display, "display"}, {SourceKind::Window, "window"},       {SourceKind::Region, "region"},
    {SourceKind::Camera, "camera"},   {SourceKind::Image, "image"},         {SourceKind::Media, "media"},
    {SourceKind::Text, "text"},       {SourceKind::Colour, "colour"},       {SourceKind::Browser, "browser"},
    {SourceKind::Slideshow, "slideshow"}, {SourceKind::Scene, "scene"},     {SourceKind::InputAudio, "input-audio"}, {SourceKind::DesktopAudio, "desktop-audio"},
    {SourceKind::ApplicationAudio, "app-audio"},
    {SourceKind::Design, "design"},
    {SourceKind::Soundboard, "soundboard"},
    {SourceKind::AuroraSky, "aurora-sky"},
    {SourceKind::AuroraPulse, "aurora-pulse"},
};

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}

}  // namespace

const char* kindKey(SourceKind k) {
    for (auto& n : kKinds) if (n.kind == k) return n.key;
    return "colour";
}

std::optional<SourceKind> kindFromKey(const std::string& key) {
    for (auto& n : kKinds) if (key == n.key) return n.kind;
    return std::nullopt;
}

bool hasSound(SourceKind k) { return isAudioOnly(k) || k == SourceKind::Media; }

bool isAudioOnly(SourceKind k) {
    return k == SourceKind::InputAudio || k == SourceKind::DesktopAudio || k == SourceKind::ApplicationAudio || k == SourceKind::Soundboard;
}

SceneCollection::SceneCollection() {
    current_ = addScene("Scene");
    changes_ = 0;
}

Id SceneCollection::newId(char prefix) { return std::string(1, prefix) + std::to_string(nextId_++); }

const Scene* SceneCollection::scene(const Id& id) const {
    for (auto& s : scenes_) if (s.id == id) return &s;
    return nullptr;
}
Scene* SceneCollection::sceneMut(const Id& id) {
    for (auto& s : scenes_) if (s.id == id) return &s;
    return nullptr;
}
const Source* SceneCollection::source(const Id& id) const {
    for (auto& s : sources_) if (s.id == id) return &s;
    return nullptr;
}

bool SceneCollection::setCurrent(const Id& id) {
    if (!scene(id) || id == current_) return scene(id) != nullptr;
    current_ = id;
    ++changes_;
    return true;
}

std::string SceneCollection::uniqueSceneName(const std::string& wantedIn) const {
    std::string wanted = trim(wantedIn);
    if (wanted.empty()) wanted = "Scene";
    auto taken = [&](const std::string& n) {
        return std::any_of(scenes_.begin(), scenes_.end(), [&](const Scene& s) { return lower(s.name) == lower(n); });
    };
    if (!taken(wanted)) return wanted;
    // "Scene 2", "Scene 3"... (a name that already ends in a number counts on from it)
    std::string base = wanted;
    int n = 2;
    const size_t sp = base.find_last_of(' ');
    if (sp != std::string::npos && sp + 1 < base.size() && std::all_of(base.begin() + (long)sp + 1, base.end(), ::isdigit) && base.size() - sp < 6) {
        n = std::stoi(base.substr(sp + 1)) + 1;
        base = base.substr(0, sp);
    }
    for (;; ++n) {
        const std::string c = base + " " + std::to_string(n);
        if (!taken(c)) return c;
    }
}

Id SceneCollection::addScene(const std::string& name) {
    Scene s;
    s.id = newId('c');
    s.name = uniqueSceneName(name);
    scenes_.push_back(s);
    if (current_.empty()) current_ = s.id;
    ++changes_;
    return s.id;
}

bool SceneCollection::renameScene(const Id& id, const std::string& nameIn) {
    Scene* s = sceneMut(id);
    const std::string name = trim(nameIn);
    if (!s || name.empty() || name.size() > 120) return false;
    for (auto& o : scenes_)
        if (o.id != id && lower(o.name) == lower(name)) return false;
    if (s->name == name) return true;
    s->name = name;
    ++changes_;
    return true;
}

Id SceneCollection::duplicateScene(const Id& id) {
    const Scene* s = scene(id);
    if (!s) return {};
    Scene copy = *s;
    copy.id = newId('c');
    copy.name = uniqueSceneName(s->name + " (copy)");
    std::map<Id, Id> ids;
    for (auto& it : copy.items) {   // the same sources: nothing is captured twice
        const Id old = it.id;
        it.id = newId('i');
        ids[old] = it.id;
    }
    for (auto& it : copy.items)
        if (!it.follow.item.empty()) it.follow.item = ids.count(it.follow.item) ? ids[it.follow.item] : Id();
    // Next to the original.
    const auto at = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& x) { return x.id == id; });
    scenes_.insert(at + 1, copy);
    ++changes_;
    return copy.id;
}

bool SceneCollection::removeScene(const Id& id) {
    if (scenes_.size() <= 1) return false;
    const auto it = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& x) { return x.id == id; });
    if (it == scenes_.end()) return false;
    const size_t at = (size_t)(it - scenes_.begin());
    scenes_.erase(it);
    if (current_ == id) current_ = scenes_[std::min(at, scenes_.size() - 1)].id;
    // Scenes that nested it lose that item, and the source showing it goes.
    std::vector<Id> gone;
    for (auto& src : sources_)
        if (nestedScene(src) == id) gone.push_back(src.id);
    for (auto& g : gone) removeSource(g);
    ++changes_;
    return true;
}

bool SceneCollection::moveScene(const Id& id, int delta) {
    const auto it = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& x) { return x.id == id; });
    if (it == scenes_.end()) return false;
    const long at = it - scenes_.begin(), to = at + delta;
    if (to < 0 || to >= (long)scenes_.size()) return false;
    std::swap(scenes_[(size_t)at], scenes_[(size_t)to]);
    ++changes_;
    return true;
}

Id SceneCollection::addSource(SourceKind kind, const std::string& nameIn, const std::string& settings) {
    Source s;
    s.id = newId('s');
    s.kind = kind;
    std::string name = trim(nameIn);
    if (name.empty()) name = kindKey(kind);
    // Unique among sources too, so the mixer's channels can be told apart.
    std::string candidate = name;
    for (int n = 2; std::any_of(sources_.begin(), sources_.end(), [&](const Source& o) { return lower(o.name) == lower(candidate); }); ++n)
        candidate = name + " " + std::to_string(n);
    s.name = candidate;
    s.settings = settings.empty() ? "{}" : settings;
    sources_.push_back(s);
    ++changes_;
    return s.id;
}

Id SceneCollection::addItem(const Id& sceneId, const Id& sourceId, const Transform& t) {
    Scene* s = sceneMut(sceneId);
    const Source* src = source(sourceId);
    if (!s || !src) return {};
    if (const Id n = nestedScene(*src); !n.empty() && wouldLoop(sceneId, n)) return {};   // never a scene inside itself
    SceneItem it;
    it.id = newId('i');
    it.source = sourceId;
    it.transform = t;
    s->items.push_back(it);
    ++changes_;
    return it.id;
}

bool SceneCollection::removeItem(const Id& sceneId, const Id& item) {
    Scene* s = sceneMut(sceneId);
    if (!s) return false;
    const auto n = s->items.size();
    s->items.erase(std::remove_if(s->items.begin(), s->items.end(), [&](const SceneItem& i) { return i.id == item; }), s->items.end());
    if (s->items.size() == n) return false;
    for (auto& i : s->items)
        if (i.follow.item == item) i.follow = {};   // what followed it stays where it is
    ++changes_;
    return true;
}

Id SceneCollection::nestedScene(const Source& s) {
    if (s.kind != SourceKind::Scene) return {};
    const nlohmann::json j = nlohmann::json::parse(s.settings, nullptr, false);
    if (!j.is_object() || !j.contains("scene") || !j["scene"].is_string()) return {};
    return j["scene"].get<std::string>();
}

Id SceneCollection::sceneSource(const Id& sceneId) {
    const Scene* sc = scene(sceneId);
    if (!sc) return {};
    for (auto& src : sources_)
        if (nestedScene(src) == sceneId) return src.id;
    return addSource(SourceKind::Scene, sc->name, nlohmann::json{{"scene", sceneId}}.dump());
}

bool SceneCollection::wouldLoop(const Id& into, const Id& nested) const {
    // Depth-first through what `nested` shows: reaching `into` means a loop.
    std::vector<Id> todo{nested};
    std::vector<Id> seen;
    while (!todo.empty()) {
        const Id at = todo.back();
        todo.pop_back();
        if (at == into) return true;
        if (std::find(seen.begin(), seen.end(), at) != seen.end()) continue;
        seen.push_back(at);
        if (const Scene* sc = scene(at))
            for (auto& i : sc->items)
                if (const Source* src = source(i.source)) {
                    const Id n = nestedScene(*src);
                    if (!n.empty()) todo.push_back(n);
                }
    }
    return false;
}

Id SceneCollection::duplicateItem(const Id& sceneId, const Id& itemId) {
    Scene* sc = sceneMut(sceneId);
    if (!sc) return {};
    auto it = std::find_if(sc->items.begin(), sc->items.end(), [&](const SceneItem& i) { return i.id == itemId; });
    if (it == sc->items.end()) return {};
    SceneItem copy = *it;
    copy.id = newId('i');
    copy.transform.x += 24;   // a little down and to the right, so it's seen to be a copy
    copy.transform.y += 24;
    copy.locked = false;
    sc->items.insert(it + 1, copy);
    ++changes_;
    return copy.id;
}

bool SceneCollection::isGroupItem(const Id& sceneId, const Id& itemId) const {
    const SceneItem* it = item(sceneId, itemId);
    const Source* src = it ? source(it->source) : nullptr;
    const Scene* inner = src ? scene(nestedScene(*src)) : nullptr;
    return inner && inner->folder == "Groups";
}

Id SceneCollection::groupItems(const Id& sceneId, const std::vector<Id>& ids, std::string& error) {
    Scene* sc = sceneMut(sceneId);
    if (!sc || ids.size() < 2) {
        error = "Choose at least two sources to group.";
        return {};
    }
    std::vector<SceneItem> moving;
    std::size_t at = sc->items.size();
    for (std::size_t k = 0; k < sc->items.size(); ++k)
        if (std::find(ids.begin(), ids.end(), sc->items[k].id) != ids.end()) {
            moving.push_back(sc->items[k]);
            at = std::min(at, k);
        }
    if (moving.size() != ids.size()) {
        error = "Those sources aren't all in this scene.";
        return {};
    }
    int n = 1;
    for (auto& s2 : scenes_)
        if (s2.folder == "Groups") ++n;
    const Id inner = addScene("Group " + std::to_string(n));
    sceneMut(inner)->folder = "Groups";
    sc = sceneMut(sceneId);   // addScene may have moved the scenes
    for (auto& m : moving) {   // their order kept, bottom first
        m.locked = false;
        sceneMut(inner)->items.push_back(m);
    }
    sc->items.erase(std::remove_if(sc->items.begin(), sc->items.end(),
                                   [&](const SceneItem& i) { return std::find(ids.begin(), ids.end(), i.id) != ids.end(); }),
                    sc->items.end());
    SceneItem g;
    g.id = newId('i');
    g.source = sceneSource(inner);
    sc = sceneMut(sceneId);
    sc->items.insert(sc->items.begin() + (long)std::min(at, sc->items.size()), g);
    ++changes_;
    return g.id;
}

bool SceneCollection::addToGroup(const Id& sceneId, const Id& groupItem, const Id& itemId, std::string& error) {
    if (!isGroupItem(sceneId, groupItem)) {
        error = "That isn't a group.";
        return false;
    }
    Scene* sc = sceneMut(sceneId);
    auto it = std::find_if(sc->items.begin(), sc->items.end(), [&](const SceneItem& i) { return i.id == itemId; });
    if (it == sc->items.end() || itemId == groupItem) {
        error = "That source isn't in this scene.";
        return false;
    }
    const SceneItem* g = item(sceneId, groupItem);
    const Transform gt = g->transform;
    if (gt.rotation != 0 || gt.flipH || gt.flipV || gt.cropLeft > 0 || gt.cropTop > 0 || gt.cropRight > 0 || gt.cropBottom > 0) {
        error = "The group is rotated, mirrored or cropped, so a source can't be put into it where it shows. Reset the group first.";
        return false;
    }
    // Where it shows now, in the group's own canvas.
    const float sx = gt.width > 0 ? gt.width / (float)canvasWidth : 1, sy = gt.height > 0 ? gt.height / (float)canvasHeight : 1;
    SceneItem moved = *it;
    moved.transform.x = (moved.transform.x - gt.x) / sx;
    moved.transform.y = (moved.transform.y - gt.y) / sy;
    if (moved.transform.width > 0) moved.transform.width /= sx;
    if (moved.transform.height > 0) moved.transform.height /= sy;
    const Id inner = nestedScene(*source(g->source));
    sc->items.erase(it);
    sceneMut(inner)->items.push_back(moved);
    ++changes_;
    return true;
}

bool SceneCollection::ungroup(const Id& sceneId, const Id& groupItem, std::string& error) {
    if (!isGroupItem(sceneId, groupItem)) {
        error = "That isn't a group.";
        return false;
    }
    const SceneItem g = *item(sceneId, groupItem);
    const Transform gt = g.transform;
    if (gt.rotation != 0 || gt.flipH || gt.flipV || gt.cropLeft > 0 || gt.cropTop > 0 || gt.cropRight > 0 || gt.cropBottom > 0) {
        error = "The group is rotated, mirrored or cropped, so its sources can't be put back exactly where they show. Reset the group first.";
        return false;
    }
    if (g.border.on) {
        error = "The group has a border, which its sources would lose. Turn the border off first.";
        return false;
    }
    const Id inner = nestedScene(*source(g.source));
    const float sx = gt.width > 0 ? gt.width / (float)canvasWidth : 1, sy = gt.height > 0 ? gt.height / (float)canvasHeight : 1;
    std::vector<SceneItem> back = scene(inner)->items;
    for (auto& m : back) {
        m.transform.x = gt.x + m.transform.x * sx;
        m.transform.y = gt.y + m.transform.y * sy;
        // An item at its own size gets an explicit size when the group was scaled.
        if (m.transform.width > 0) m.transform.width *= sx;
        if (m.transform.height > 0) m.transform.height *= sy;
        m.transform.opacity *= gt.opacity;
        if (!g.visible) m.visible = false;
    }
    Scene* sc = sceneMut(sceneId);
    auto pos = std::find_if(sc->items.begin(), sc->items.end(), [&](const SceneItem& i) { return i.id == groupItem; });
    const long at = pos - sc->items.begin();
    sc->items.erase(pos);
    sc->items.insert(sc->items.begin() + at, back.begin(), back.end());
    // The group's scene goes if nothing else shows it.
    bool shownElsewhere = false;
    for (auto& s2 : scenes_)
        for (auto& i : s2.items)
            if (i.source == g.source) shownElsewhere = true;
    if (!shownElsewhere) removeScene(inner);
    ++changes_;
    return true;
}

std::vector<Id> SceneCollection::unusedSources() const {
    std::vector<Id> out;
    for (auto& src : sources_) {
        if (isAudioOnly(src.kind)) continue;   // sound sources belong to every scene (the mixer), not to one
        bool used = false;
        for (auto& s : scenes_)
            for (auto& i : s.items) used = used || i.source == src.id;
        if (!used) out.push_back(src.id);
    }
    return out;
}

bool SceneCollection::setSceneMute(const Id& scene, const Id& sourceId, bool muted) {
    Scene* sc = sceneMut(scene);
    const Source* src = source(sourceId);
    if (!sc || !src || !hasSound(src->kind)) return false;
    auto it = std::find(sc->mutes.begin(), sc->mutes.end(), sourceId);
    if (muted == (it != sc->mutes.end())) return true;
    if (muted) sc->mutes.push_back(sourceId);
    else sc->mutes.erase(it);
    ++changes_;
    return true;
}

void SceneCollection::removeSource(const Id& id) {
    for (auto& s : scenes_) {
        s.items.erase(std::remove_if(s.items.begin(), s.items.end(), [&](const SceneItem& i) { return i.source == id; }), s.items.end());
        s.mutes.erase(std::remove(s.mutes.begin(), s.mutes.end(), id), s.mutes.end());
    }
    sources_.erase(std::remove_if(sources_.begin(), sources_.end(), [&](const Source& s) { return s.id == id; }), sources_.end());
    ++changes_;
}

bool SceneCollection::renameSource(const Id& id, const std::string& wanted) {
    const std::string name = trim(wanted);
    if (name.empty() || name.size() > 120) return false;
    Source* mine = nullptr;
    for (auto& s : sources_) {
        if (s.id == id) mine = &s;
        else if (lower(s.name) == lower(name)) return false;
    }
    if (!mine) return false;
    if (mine->name != name) {
        mine->name = name;
        ++changes_;
    }
    return true;
}

bool SceneCollection::setSourceSettings(const Id& id, const std::string& settings) {
    for (auto& s : sources_)
        if (s.id == id) {
            if (s.settings != settings) {
                s.settings = settings;
                ++changes_;
            }
            return true;
        }
    return false;
}

const SceneItem* SceneCollection::item(const Id& sceneId, const Id& itemId) const {
    const Scene* s = scene(sceneId);
    if (!s) return nullptr;
    for (auto& i : s->items)
        if (i.id == itemId) return &i;
    return nullptr;
}

namespace {
bool sameTransform(const Transform& a, const Transform& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height && a.rotation == b.rotation && a.cropLeft == b.cropLeft &&
           a.cropTop == b.cropTop && a.cropRight == b.cropRight && a.cropBottom == b.cropBottom && a.opacity == b.opacity && a.flipH == b.flipH &&
           a.flipV == b.flipV;
}
}  // namespace

bool SceneCollection::setTransform(const Id& sceneId, const Id& itemId, const Transform& t) {
    SceneItem* it = const_cast<SceneItem*>(item(sceneId, itemId));
    if (!it) return false;
    if (!sameTransform(it->transform, t)) {
        it->transform = t;
        ++changes_;
        Scene* sc = sceneMut(sceneId);
        // Moved itself while following: it now sits there relative to what it follows.
        if (!it->follow.item.empty()) {
            const SceneItem* target = item(sceneId, it->follow.item);
            if (target && target->transform.width > 0 && target->transform.height > 0 && t.width > 0 && t.height > 0) remember(*it, *target);
            else it->follow = {};
        }
        if (sc) placeFollowers(*sc, itemId, 0);
    }
    return true;
}

namespace {
constexpr float kPi = 3.14159265358979f;
}

void SceneCollection::remember(SceneItem& f, const SceneItem& t) {
    const Transform& a = t.transform;
    const Transform& b = f.transform;
    // The follower's centre, turned back by the target's rotation about the target's centre.
    const float acx = a.x + a.width / 2, acy = a.y + a.height / 2;
    const float r = -a.rotation * kPi / 180;
    const float dx = b.x + b.width / 2 - acx, dy = b.y + b.height / 2 - acy;
    const float ux = dx * std::cos(r) - dy * std::sin(r), uy = dx * std::sin(r) + dy * std::cos(r);
    f.follow.w = b.width / a.width;
    f.follow.h = b.height / a.height;
    f.follow.x = (ux + a.width / 2) / a.width - f.follow.w / 2;
    f.follow.y = (uy + a.height / 2) / a.height - f.follow.h / 2;
    f.follow.rotation = b.rotation - a.rotation;
}

void SceneCollection::placeFollowers(Scene& sc, const Id& target, int depth) {
    if (depth > 8) return;
    const auto t = std::find_if(sc.items.begin(), sc.items.end(), [&](const SceneItem& i) { return i.id == target; });
    if (t == sc.items.end()) return;
    const Transform a = t->transform;
    if (a.width <= 0 || a.height <= 0) return;
    std::vector<Id> moved;
    for (auto& f : sc.items) {
        if (f.follow.item != target || f.id == target) continue;
        Transform b = f.transform;
        b.width = f.follow.w * a.width;
        b.height = f.follow.h * a.height;
        // Its centre in the target's own (unturned) frame, then turned with it.
        const float ux = (f.follow.x + f.follow.w / 2) * a.width - a.width / 2, uy = (f.follow.y + f.follow.h / 2) * a.height - a.height / 2;
        const float r = a.rotation * kPi / 180;
        const float cx = a.x + a.width / 2 + ux * std::cos(r) - uy * std::sin(r), cy = a.y + a.height / 2 + ux * std::sin(r) + uy * std::cos(r);
        b.x = cx - b.width / 2;
        b.y = cy - b.height / 2;
        b.rotation = std::fmod(a.rotation + f.follow.rotation + 360.0f, 360.0f);
        if (!sameTransform(b, f.transform)) {
            f.transform = b;
            ++changes_;
            moved.push_back(f.id);
        }
    }
    for (auto& m : moved) placeFollowers(sc, m, depth + 1);   // anything following those
}

bool SceneCollection::followItem(const Id& sceneId, const Id& itemId, const Id& targetId, std::string& error) {
    Scene* sc = sceneMut(sceneId);
    SceneItem* f = const_cast<SceneItem*>(item(sceneId, itemId));
    const SceneItem* t = item(sceneId, targetId);
    if (!sc || !f || !t) {
        error = "Both have to be in this scene.";
        return false;
    }
    if (itemId == targetId) {
        error = "An item can't follow itself.";
        return false;
    }
    // Not round in a circle: the target mustn't (through others) follow this item.
    Id walk = t->follow.item;
    for (int guard = 0; !walk.empty() && guard < 64; ++guard) {
        if (walk == itemId) {
            error = "That one already follows this one.";
            return false;
        }
        const SceneItem* w = item(sceneId, walk);
        walk = w ? w->follow.item : Id();
    }
    if (t->transform.width <= 0 || t->transform.height <= 0 || f->transform.width <= 0 || f->transform.height <= 0) {
        error = "Both need a size set (move or resize them once).";
        return false;
    }
    f->follow.item = targetId;
    remember(*f, *t);
    ++changes_;
    return true;
}

bool SceneCollection::unfollow(const Id& sceneId, const Id& itemId) {
    SceneItem* f = const_cast<SceneItem*>(item(sceneId, itemId));
    if (!f || f->follow.item.empty()) return false;
    f->follow = {};
    ++changes_;
    return true;
}

bool SceneCollection::setItemVisible(const Id& sceneId, const Id& itemId, bool visible) {
    SceneItem* it = const_cast<SceneItem*>(item(sceneId, itemId));
    if (!it) return false;
    if (it->visible != visible) {
        it->visible = visible;
        ++changes_;
    }
    return true;
}

bool SceneCollection::setItemLocked(const Id& sceneId, const Id& itemId, bool locked) {
    SceneItem* it = const_cast<SceneItem*>(item(sceneId, itemId));
    if (!it) return false;
    if (it->locked != locked) {
        it->locked = locked;
        ++changes_;
    }
    return true;
}

bool SceneCollection::setItemBorder(const Id& sceneId, const Id& itemId, const border::Style& b) {
    SceneItem* it = const_cast<SceneItem*>(item(sceneId, itemId));
    if (!it) return false;
    if (!(it->border == b)) {
        it->border = b;
        ++changes_;
    }
    return true;
}

bool SceneCollection::setItemKey(const Id& sceneId, const Id& itemId, const keying::Key& k) {
    SceneItem* it = const_cast<SceneItem*>(item(sceneId, itemId));
    if (!it) return false;
    if (!(it->key == k)) {
        it->key = k;
        ++changes_;
    }
    return true;
}

bool SceneCollection::moveItem(const Id& sceneId, const Id& itemId, int delta) {
    Scene* s = sceneMut(sceneId);
    if (!s) return false;
    auto it = std::find_if(s->items.begin(), s->items.end(), [&](const SceneItem& i) { return i.id == itemId; });
    if (it == s->items.end()) return false;
    const long from = it - s->items.begin();
    const long to = std::clamp(from + (long)delta, 0L, (long)s->items.size() - 1);
    if (to == from) return false;
    SceneItem moving = *it;
    s->items.erase(it);
    s->items.insert(s->items.begin() + to, moving);
    ++changes_;
    return true;
}

bool SceneCollection::setSceneFolder(const Id& id, const std::string& wanted) {
    const std::string folder = trim(wanted).substr(0, 120);
    auto it = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& x) { return x.id == id; });
    if (it == scenes_.end()) return false;
    if (it->folder == folder) return true;
    Scene moved = *it;
    moved.folder = folder;
    scenes_.erase(it);
    // Next to the folder's other scenes (after its last one); a new folder or none: at the end.
    auto last = scenes_.end();
    if (!folder.empty())
        for (auto k = scenes_.begin(); k != scenes_.end(); ++k)
            if (k->folder == folder) last = k + 1;
    scenes_.insert(last, moved);
    ++changes_;
    return true;
}

std::string SceneCollection::exportScene(const Id& id) const {
    using nlohmann::json;
    if (!scene(id)) return {};
    // The scene and everything inside it, however deep.
    std::vector<Id> scenesNeeded{id}, sourcesNeeded;
    for (std::size_t k = 0; k < scenesNeeded.size(); ++k)
        for (auto& i : scene(scenesNeeded[k])->items) {
            if (std::find(sourcesNeeded.begin(), sourcesNeeded.end(), i.source) == sourcesNeeded.end()) sourcesNeeded.push_back(i.source);
            if (const Source* src = source(i.source)) {
                const Id n = nestedScene(*src);
                if (!n.empty() && scene(n) && std::find(scenesNeeded.begin(), scenesNeeded.end(), n) == scenesNeeded.end()) scenesNeeded.push_back(n);
            }
        }
    json all = json::parse(toJson());
    json out = {{"format", "northray-capture-scene"}, {"version", 1}, {"scene", id}, {"canvas", all["canvas"]}};
    json srcs = json::array(), scs = json::array();
    for (auto& s : all["sources"])
        if (std::find(sourcesNeeded.begin(), sourcesNeeded.end(), s["id"].get<std::string>()) != sourcesNeeded.end()) srcs.push_back(s);
    for (auto& s : all["scenes"])
        if (std::find(scenesNeeded.begin(), scenesNeeded.end(), s["id"].get<std::string>()) != scenesNeeded.end()) {
            s.erase("folder");
            scs.push_back(s);
        }
    out["sources"] = srcs;
    out["scenes"] = scs;
    return out.dump(1);
}

Id SceneCollection::importScene(const std::string& text, std::string& error, bool* scaled) {
    using nlohmann::json;
    if (scaled) *scaled = false;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || j.value("format", std::string()) != "northray-capture-scene") {
        error = "That isn't a scene exported from NorthRay Capture.";
        return {};
    }
    // Read it as a collection of its own (the same checks as a saved file), then copy it in.
    SceneCollection in;
    json asCollection = {{"version", 1}, {"canvas", j.contains("canvas") ? j["canvas"] : json::object()},
                         {"sources", j.contains("sources") ? j["sources"] : json::array()},
                         {"scenes", j.contains("scenes") ? j["scenes"] : json::array()}};
    const std::string top = j.value("scene", std::string());
    if (!in.fromJson(asCollection.dump()) || !in.scene(top)) {
        error = "The scene file is damaged or incomplete.";
        return {};
    }
    const float sx = (float)canvasWidth / (float)in.canvasWidth, sy = (float)canvasHeight / (float)in.canvasHeight;
    const bool scale = std::fabs(sx - 1) > 0.001f || std::fabs(sy - 1) > 0.001f;
    if (scaled) *scaled = scale;
    std::map<Id, Id> sceneIds, sourceIds, itemIds;
    for (auto& sc : in.scenes_) sceneIds[sc.id] = addScene(sc.name);   // unique names
    for (auto& src : in.sources_) {
        std::string settings = src.settings;
        if (src.kind == SourceKind::Scene) {
            const Id n = nestedScene(src);
            settings = json{{"scene", sceneIds.count(n) ? sceneIds[n] : Id()}}.dump();
        }
        sourceIds[src.id] = addSource(src.kind, src.name, settings);
    }
    for (auto& sc : in.scenes_) {
        Scene* mine = sceneMut(sceneIds[sc.id]);
        for (auto& item : sc.items) {
            SceneItem copy = item;
            copy.id = newId('i');
            itemIds[item.id] = copy.id;
            copy.source = sourceIds[item.source];
            if (scale) {
                copy.transform.x *= sx;
                copy.transform.y *= sy;
                copy.transform.width *= sx;
                copy.transform.height *= sy;
            }
            mine->items.push_back(copy);
        }
    }
    for (auto& sc : in.scenes_)   // follow links point at the new items
        for (auto& it : sceneMut(sceneIds[sc.id])->items)
            if (!it.follow.item.empty()) it.follow.item = itemIds.count(it.follow.item) ? itemIds[it.follow.item] : Id();
    // Scene sources showing a scene that wasn't in the file show nothing: remove them.
    for (auto& [old, made] : sourceIds)
        if (const Source* s = source(made); s && s->kind == SourceKind::Scene && nestedScene(*s).empty()) removeSource(made);
    ++changes_;
    return sceneIds[top];
}

std::string SceneCollection::toJson() const {
    using nlohmann::json;
    json j;
    j["version"] = 1;
    j["nextId"] = nextId_;
    j["canvas"] = {{"width", canvasWidth}, {"height", canvasHeight}};
    j["current"] = current_;
    auto& srcs = j["sources"] = json::array();
    for (auto& s : sources_) {
        json settings = json::parse(s.settings, nullptr, false);
        srcs.push_back({{"id", s.id}, {"kind", kindKey(s.kind)}, {"name", s.name}, {"settings", settings.is_discarded() ? json::object() : settings}});
    }
    auto& scs = j["scenes"] = json::array();
    for (auto& s : scenes_) {
        json items = json::array();
        for (auto& i : s.items) {
            const Transform& t = i.transform;
            items.push_back({{"id", i.id}, {"source", i.source}, {"visible", i.visible}, {"locked", i.locked},
                             {"transform", {{"x", t.x}, {"y", t.y}, {"width", t.width}, {"height", t.height}, {"rotation", t.rotation},
                                            {"crop", {t.cropLeft, t.cropTop, t.cropRight, t.cropBottom}}, {"opacity", t.opacity}, {"flipH", t.flipH}, {"flipV", t.flipV}}}});
            if (i.border.on) items.back()["border"] = json::parse(border::toJson(i.border));
            if (!(i.key == keying::Key{})) items.back()["key"] = json::parse(keying::toJson(i.key));
            if (!i.follow.item.empty())
                items.back()["follow"] = {{"item", i.follow.item}, {"x", i.follow.x}, {"y", i.follow.y}, {"w", i.follow.w}, {"h", i.follow.h},
                                          {"rotation", i.follow.rotation}};
        }
        json sc = {{"id", s.id}, {"name", s.name}, {"items", items}};
        if (!s.folder.empty()) sc["folder"] = s.folder;
        if (!s.mutes.empty()) sc["mutes"] = s.mutes;
        scs.push_back(sc);
    }
    return j.dump(1);
}

bool SceneCollection::fromJson(const std::string& text) {
    using nlohmann::json;
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("scenes") || !j["scenes"].is_array()) return false;
    SceneCollection c;
    c.scenes_.clear();
    c.current_.clear();
    auto num = [](const json& o, const char* k, float d) { return o.contains(k) && o[k].is_number() ? o[k].get<float>() : d; };
    if (j.contains("canvas") && j["canvas"].is_object()) {
        c.canvasWidth = std::clamp((int)num(j["canvas"], "width", 1920), 16, 8192);
        c.canvasHeight = std::clamp((int)num(j["canvas"], "height", 1080), 16, 8192);
    }
    if (j.contains("sources") && j["sources"].is_array())
        for (auto& s : j["sources"]) {
            if (!s.is_object() || !s.contains("id") || !s["id"].is_string()) continue;
            const auto kind = kindFromKey(s.value("kind", ""));
            if (!kind) continue;   // a kind from a newer version: left out
            Source src;
            src.id = s["id"].get<std::string>();
            src.kind = *kind;
            src.name = s.value("name", std::string(kindKey(*kind)));
            src.settings = s.contains("settings") ? s["settings"].dump() : "{}";
            c.sources_.push_back(src);
        }
    for (auto& s : j["scenes"]) {
        if (!s.is_object() || !s.contains("id") || !s["id"].is_string()) continue;
        Scene sc;
        sc.id = s["id"].get<std::string>();
        sc.name = s.value("name", std::string("Scene"));
        if (s.contains("folder") && s["folder"].is_string()) sc.folder = trim(s["folder"].get<std::string>()).substr(0, 120);
        if (s.contains("mutes") && s["mutes"].is_array())
            for (auto& m : s["mutes"])
                if (m.is_string() && sc.mutes.size() < 64) sc.mutes.push_back(m.get<std::string>());
        if (s.contains("items") && s["items"].is_array())
            for (auto& i : s["items"]) {
                if (!i.is_object() || !i.contains("source") || !i["source"].is_string() || !c.source(i["source"].get<std::string>())) continue;
                SceneItem it;
                it.id = i.value("id", std::string());
                it.source = i["source"].get<std::string>();
                it.visible = i.value("visible", true);
                it.locked = i.value("locked", false);
                if (i.contains("transform") && i["transform"].is_object()) {
                    const json& t = i["transform"];
                    it.transform.x = num(t, "x", 0);
                    it.transform.y = num(t, "y", 0);
                    it.transform.width = std::max(0.0f, num(t, "width", 0));
                    it.transform.height = std::max(0.0f, num(t, "height", 0));
                    it.transform.rotation = num(t, "rotation", 0);
                    it.transform.opacity = std::clamp(num(t, "opacity", 1), 0.0f, 1.0f);
                    it.transform.flipH = t.contains("flipH") && t["flipH"].is_boolean() && t["flipH"].get<bool>();
                    it.transform.flipV = t.contains("flipV") && t["flipV"].is_boolean() && t["flipV"].get<bool>();
                    if (t.contains("crop") && t["crop"].is_array() && t["crop"].size() == 4) {
                        float* crop[] = {&it.transform.cropLeft, &it.transform.cropTop, &it.transform.cropRight, &it.transform.cropBottom};
                        for (size_t k = 0; k < 4; ++k)
                            if (t["crop"][k].is_number()) *crop[k] = std::max(0.0f, t["crop"][k].get<float>());
                    }
                }
                if (i.contains("border") && i["border"].is_object()) it.border = border::fromJson(i["border"].dump());
                if (i.contains("key") && i["key"].is_object()) it.key = keying::fromJson(i["key"].dump());
                if (i.contains("follow") && i["follow"].is_object()) {
                    const json& f = i["follow"];
                    auto num = [&](const char* k, float lo, float hi, float d) {
                        return f.contains(k) && f[k].is_number() && std::isfinite(f[k].get<double>()) ? std::clamp(f[k].get<float>(), lo, hi) : d;
                    };
                    if (f.contains("item") && f["item"].is_string()) {
                        it.follow.item = f["item"].get<std::string>();
                        it.follow.x = num("x", -100, 100, 0);
                        it.follow.y = num("y", -100, 100, 0);
                        it.follow.w = num("w", 0.001f, 100, 1);
                        it.follow.h = num("h", 0.001f, 100, 1);
                        it.follow.rotation = num("rotation", -360, 360, 0);
                    }
                }
                sc.items.push_back(it);
            }
        for (auto& it : sc.items) {   // following something not in this scene (or itself): not following
            const bool there = std::any_of(sc.items.begin(), sc.items.end(), [&](const SceneItem& o) { return o.id == it.follow.item && o.id != it.id; });
            if (!there) it.follow = {};
        }
        c.scenes_.push_back(sc);
    }
    if (c.scenes_.empty()) return false;
    c.nextId_ = j.contains("nextId") && j["nextId"].is_number_unsigned() ? j["nextId"].get<std::uint64_t>() : 1;
    // Never hand out an id already in use (a hand-edited file).
    auto bump = [&](const Id& id) {
        if (id.size() > 1 && std::all_of(id.begin() + 1, id.end(), ::isdigit)) c.nextId_ = std::max<std::uint64_t>(c.nextId_, std::stoull(id.substr(1)) + 1);
    };
    for (auto& s : c.sources_) bump(s.id);
    for (auto& s : c.scenes_) {
        bump(s.id);
        for (auto& i : s.items) bump(i.id);
    }
    const std::string cur = j.value("current", std::string());
    c.current_ = c.scene(cur) ? cur : c.scenes_.front().id;
    c.changes_ = 0;
    *this = std::move(c);
    return true;
}

}  // namespace ncap

#include "ncap/scenes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace ncap {

namespace {

struct KindName { SourceKind kind; const char* key; };
const KindName kKinds[] = {
    {SourceKind::Display, "display"}, {SourceKind::Window, "window"},       {SourceKind::Region, "region"},
    {SourceKind::Camera, "camera"},   {SourceKind::Image, "image"},         {SourceKind::Media, "media"},
    {SourceKind::Text, "text"},       {SourceKind::Colour, "colour"},       {SourceKind::Browser, "browser"},
    {SourceKind::Scene, "scene"},     {SourceKind::InputAudio, "input-audio"}, {SourceKind::DesktopAudio, "desktop-audio"},
    {SourceKind::ApplicationAudio, "app-audio"},
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

bool isAudioOnly(SourceKind k) { return k == SourceKind::InputAudio || k == SourceKind::DesktopAudio || k == SourceKind::ApplicationAudio; }

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
    for (auto& it : copy.items) it.id = newId('i');   // the same sources: nothing is captured twice
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

void SceneCollection::removeSource(const Id& id) {
    for (auto& s : scenes_)
        s.items.erase(std::remove_if(s.items.begin(), s.items.end(), [&](const SceneItem& i) { return i.source == id; }), s.items.end());
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
    }
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
        }
        scs.push_back({{"id", s.id}, {"name", s.name}, {"items", items}});
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
                sc.items.push_back(it);
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
